// Unit tests for core/loader: mapper, syscall-stub rewriter, code scanner.
//
// Real PE: bin/guest/guest_dll.dll (built by tests/guest). Synthetic PEs are
// assembled in memory by SynthPE below to reach corner cases no linker
// produces. Scanner tests run hand-assembled machine code (bytes verified with
// llvm-mc) from VirtualAlloc'd buffers.
#include "check.h"
#include "win10xbox/loader.h"

#include <windows.h>
#include <intrin.h>
#include <emmintrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace win10xbox::loader;

namespace {

using u8 = std::uint8_t;
using Bytes = std::vector<u8>;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
std::wstring GuestPath(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf, n);
    p.resize(p.find_last_of(L'\\'));
    return p + L"\\guest\\" + name;
}

void* FindFreeBase(std::size_t size) {
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    return p;
}

DWORD ProtAt(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(p, &mbi, sizeof(mbi));
    return mbi.State == MEM_COMMIT ? mbi.Protect : 0;
}

std::uint64_t Rd64(const void* base, std::uint32_t rva) {
    std::uint64_t v;
    std::memcpy(&v, static_cast<const u8*>(base) + rva, 8);
    return v;
}
std::uint32_t Rd32(const void* base, std::uint32_t rva) {
    std::uint32_t v;
    std::memcpy(&v, static_cast<const u8*>(base) + rva, 4);
    return v;
}

void Put(Bytes& b, std::size_t at, const void* p, std::size_t n) {
    if (b.size() < at + n) b.resize(at + n);
    std::memcpy(b.data() + at, p, n);
}
void Put32(Bytes& b, std::size_t at, std::uint32_t v) { Put(b, at, &v, 4); }
void Put64(Bytes& b, std::size_t at, std::uint64_t v) { Put(b, at, &v, 8); }

std::uint32_t Align(std::uint32_t v, std::uint32_t a) { return (v + a - 1) & ~(a - 1); }

// ---------------------------------------------------------------------------
// Synthetic PE builder
// ---------------------------------------------------------------------------
struct SynthSection {
    std::string   name;
    std::uint32_t va = 0;
    std::uint32_t vsize = 0;
    Bytes         raw;
    std::uint32_t chars = IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA;
};

struct SynthPE {
    std::uint16_t machine = IMAGE_FILE_MACHINE_AMD64;
    std::uint16_t magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    std::uint16_t file_chars = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    std::uint64_t image_base = 0x10000000;
    std::uint32_t section_alignment = 0x1000, file_alignment = 0x200;
    std::uint32_t size_of_headers = 0;   // 0: computed
    std::uint32_t size_of_image = 0;     // 0: computed
    std::uint32_t entry = 0;
    std::uint32_t dir_rva[16] = {}, dir_size[16] = {};
    std::vector<SynthSection> secs;

    Bytes Build() const {
        const std::uint32_t lfanew = 0x80;
        const std::uint32_t opt_size = sizeof(IMAGE_OPTIONAL_HEADER64);
        std::uint32_t hdr_end = lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + opt_size +
                                static_cast<std::uint32_t>(secs.size() * sizeof(IMAGE_SECTION_HEADER));
        std::uint32_t soh = size_of_headers ? size_of_headers : Align(hdr_end, file_alignment);
        std::uint32_t soi = size_of_image;
        if (!soi) {
            std::uint32_t top = Align(soh, section_alignment);
            for (auto& s : secs) top = (std::max)(top, Align(s.va + (s.vsize ? s.vsize : static_cast<std::uint32_t>(s.raw.size())), section_alignment));
            soi = top;
        }
        Bytes f(soh, 0xEE);
        std::memset(f.data(), 0, hdr_end);   // keep padding recognisable, zero the real headers
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = static_cast<LONG>(lfanew);
        Put(f, 0, &dos, sizeof(dos));
        Put32(f, lfanew, IMAGE_NT_SIGNATURE);
        IMAGE_FILE_HEADER fh{};
        fh.Machine = machine;
        fh.NumberOfSections = static_cast<WORD>(secs.size());
        fh.SizeOfOptionalHeader = static_cast<WORD>(opt_size);
        fh.Characteristics = file_chars;
        Put(f, lfanew + 4, &fh, sizeof(fh));
        IMAGE_OPTIONAL_HEADER64 oh{};
        oh.Magic = magic;
        oh.AddressOfEntryPoint = entry;
        oh.ImageBase = image_base;
        oh.SectionAlignment = section_alignment;
        oh.FileAlignment = file_alignment;
        oh.MajorSubsystemVersion = 6;
        oh.SizeOfImage = soi;
        oh.SizeOfHeaders = soh;
        oh.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
        oh.NumberOfRvaAndSizes = 16;
        for (int i = 0; i < 16; ++i) {
            oh.DataDirectory[i].VirtualAddress = dir_rva[i];
            oh.DataDirectory[i].Size = dir_size[i];
        }
        Put(f, lfanew + 4 + sizeof(fh), &oh, sizeof(oh));
        std::uint32_t cursor = soh;
        std::size_t sh_off = lfanew + 4 + sizeof(fh) + opt_size;
        for (auto& s : secs) {
            IMAGE_SECTION_HEADER sh{};
            std::memcpy(sh.Name, s.name.data(), (std::min)(s.name.size(), std::size_t(8)));
            sh.VirtualAddress = s.va;
            sh.Misc.VirtualSize = s.vsize;
            sh.Characteristics = s.chars;
            if (!s.raw.empty()) {
                cursor = Align(cursor, file_alignment);
                sh.PointerToRawData = cursor;
                sh.SizeOfRawData = Align(static_cast<std::uint32_t>(s.raw.size()), file_alignment);
                f.resize(cursor + sh.SizeOfRawData, 0);
                std::memcpy(f.data() + cursor, s.raw.data(), s.raw.size());
                cursor += sh.SizeOfRawData;
            }
            Put(f, sh_off, &sh, sizeof(sh));
            sh_off += sizeof(sh);
        }
        return f;
    }
};

struct ExpFunc { std::uint32_t rva = 0; std::string fwd; };
// names: (name, function index) in the order they should appear in the name table.
Bytes BuildExports(std::uint32_t blob_rva, std::uint32_t ordinal_base, const std::vector<ExpFunc>& funcs,
                   const std::vector<std::pair<std::string, std::uint32_t>>& names, std::uint32_t* size_out) {
    const std::uint32_t n = static_cast<std::uint32_t>(funcs.size()), m = static_cast<std::uint32_t>(names.size());
    const std::uint32_t o_funcs = 40, o_names = o_funcs + 4 * n, o_ords = o_names + 4 * m, o_str = o_ords + 2 * m;
    Bytes b(o_str, 0);
    std::uint32_t cur = o_str;
    auto add_str = [&](const std::string& s) {
        Put(b, cur, s.c_str(), s.size() + 1);
        std::uint32_t at = cur;
        cur += static_cast<std::uint32_t>(s.size() + 1);
        return blob_rva + at;
    };
    std::uint32_t dll_name = add_str("synth.dll");
    for (std::uint32_t i = 0; i < n; ++i)
        Put32(b, o_funcs + 4 * i, funcs[i].fwd.empty() ? funcs[i].rva : add_str(funcs[i].fwd));
    for (std::uint32_t i = 0; i < m; ++i) {
        Put32(b, o_names + 4 * i, add_str(names[i].first));
        std::uint16_t ord = static_cast<std::uint16_t>(names[i].second);
        Put(b, o_ords + 2 * i, &ord, 2);
    }
    IMAGE_EXPORT_DIRECTORY ed{};
    ed.Name = dll_name;
    ed.Base = ordinal_base;
    ed.NumberOfFunctions = n;
    ed.NumberOfNames = m;
    ed.AddressOfFunctions = blob_rva + o_funcs;
    ed.AddressOfNames = blob_rva + o_names;
    ed.AddressOfNameOrdinals = blob_rva + o_ords;
    Put(b, 0, &ed, sizeof(ed));
    *size_out = static_cast<std::uint32_t>(b.size());
    return b;
}

// One IMAGE_BASE_RELOCATION block; `extra` trailing bytes make the size odd/unaligned.
Bytes RelocBlock(std::uint32_t page_rva, const std::vector<std::pair<std::uint32_t, unsigned>>& entries,
                 std::size_t extra = 0) {
    Bytes b;
    Put32(b, 0, page_rva);
    Put32(b, 4, static_cast<std::uint32_t>(8 + entries.size() * 2 + extra));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        std::uint16_t e = static_cast<std::uint16_t>((entries[i].second << 12) | (entries[i].first & 0xFFF));
        Put(b, 8 + 2 * i, &e, 2);
    }
    b.resize(8 + entries.size() * 2 + extra, 0);
    return b;
}

constexpr std::uint32_t kRX = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
constexpr std::uint32_t kRW = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_CNT_INITIALIZED_DATA;
constexpr std::uint32_t kRO = IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA;

}  // namespace

// ===========================================================================
// Mapper: real PE
// ===========================================================================
TEST(loader_map_guest_dll) {
    MappedImage img;
    NTSTATUS st = MapImageFromFile(GuestPath(L"guest_dll.dll").c_str(), kDefaultMapFlags, nullptr, &img);
    CHECK_MSG(st >= 0, "status 0x%08lx", static_cast<unsigned long>(st));
    if (st < 0) return;
    CHECK(img.base != nullptr);
    CHECK(img.size >= 0x1000 && img.size % 0x1000 == 0);
    CHECK(img.is_dll);
    CHECK(img.entry != nullptr && img.entry > img.base);
    CHECK(img.image_base_preferred != 0);
    CHECK(img.relocated == (reinterpret_cast<std::uintptr_t>(img.base) != img.image_base_preferred));
    CHECK_EQ(st, img.relocated ? kStatusImageNotAtBase : kStatusSuccess);
    CHECK(!img.path.empty());
    CHECK(img.function_table != nullptr && img.function_count > 0);

    auto add = reinterpret_cast<int (*)(int, int)>(FindExport(img.base, "add"));
    CHECK(add != nullptr);
    if (add) CHECK_EQ(add(1, 2), 3);
    CHECK(FindExport(img.base, "read_tsc") != nullptr);
    CHECK(FindExport(img.base, "cpuid_ecx1") != nullptr);
    CHECK(FindExport(img.base, "no_such_export") == nullptr);
    CHECK(FindExport(img.base, "") == nullptr);
    CHECK(FindExport(img.base, nullptr) == nullptr);
    CHECK(FindExport(nullptr, "add") == nullptr);
    // By ordinal: every named export is reachable that way, and ordinal 0 / huge are not.
    bool found_ord = false;
    for (unsigned o = 1; o < 16 && !found_ord; ++o) found_ord = FindExportByOrdinal(img.base, static_cast<std::uint16_t>(o)) == FindExport(img.base, "add");
    CHECK(found_ord);
    CHECK(FindExportByOrdinal(img.base, 0) == nullptr);
    CHECK(FindExportByOrdinal(img.base, 0xFFFF) == nullptr);

    // Protections: headers read-only, code RX, data RW.
    CHECK_EQ(ProtAt(img.base), static_cast<DWORD>(PAGE_READONLY));
    CHECK_EQ(ProtAt(FindExport(img.base, "add")), static_cast<DWORD>(PAGE_EXECUTE_READ));

    // The TLS callback array holds absolute addresses: they must have been relocated into the image.
    auto* dos = static_cast<const IMAGE_DOS_HEADER*>(img.base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(static_cast<const u8*>(img.base) + dos->e_lfanew);
    CHECK_EQ(nt->OptionalHeader.ImageBase, static_cast<ULONGLONG>(reinterpret_cast<std::uintptr_t>(img.base)));
    IMAGE_DATA_DIRECTORY tlsd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    CHECK(tlsd.VirtualAddress != 0);
    if (tlsd.VirtualAddress) {
        auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(static_cast<const u8*>(img.base) + tlsd.VirtualAddress);
        auto lo = reinterpret_cast<std::uintptr_t>(img.base), hi = lo + img.size;
        CHECK(tls->StartAddressOfRawData >= lo && tls->StartAddressOfRawData < hi);
        CHECK(tls->AddressOfIndex >= lo && tls->AddressOfIndex < hi);
        auto cbs = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
        CHECK(reinterpret_cast<std::uintptr_t>(cbs) >= lo && reinterpret_cast<std::uintptr_t>(cbs) < hi);
        if (cbs && *cbs) {
            CHECK(reinterpret_cast<std::uintptr_t>(*cbs) >= lo && reinterpret_cast<std::uintptr_t>(*cbs) < hi);
            auto hits = reinterpret_cast<int (*)()>(FindExport(img.base, "tls_callback_hits"));
            auto reason = reinterpret_cast<unsigned (*)()>(FindExport(img.base, "tls_callback_reason"));
            CHECK(hits && reason);
            if (hits && reason) {
                CHECK_EQ(hits(), 0);
                (*cbs)(img.base, DLL_PROCESS_ATTACH, nullptr);   // relocated data pointer written by the callback
                CHECK_EQ(hits(), 1);
                CHECK_EQ(reason(), 1u);
            }
        }
    }
    UnmapImage(img);
    CHECK(img.base == nullptr);
}

TEST(loader_map_relocated_when_preferred_is_occupied) {
    MappedImage a, b;
    std::wstring path = GuestPath(L"guest_dll.dll");
    CHECK(MapImageFromFile(path.c_str(), kDefaultMapFlags, nullptr, &a) >= 0);
    // Ask for the address `a` already occupies.
    NTSTATUS st = MapImageFromFile(path.c_str(), kDefaultMapFlags, a.base, &b);
    CHECK_EQ(st, kStatusImageNotAtBase);
    CHECK(b.relocated);
    CHECK(b.base != nullptr && b.base != a.base);
    CHECK_EQ(b.image_base_preferred, a.image_base_preferred);
    auto add = reinterpret_cast<int (*)(int, int)>(FindExport(b.base, "add"));
    CHECK(add != nullptr);
    if (add) CHECK_EQ(add(20, 22), 42);

    // The same request with NoRelocate must fail cleanly.
    MappedImage c;
    CHECK_EQ(MapImageFromFile(path.c_str(), kDefaultMapFlags | MapFlags::NoRelocate, a.base, &c),
             kStatusConflictingAddresses);
    CHECK(c.base == nullptr);

    // An explicit base is honoured when still free; without NoRelocate it is a
    // request, not a guarantee, so on a busy address space (real Windows ASLR)
    // the mapper may relocate elsewhere. Require a valid, usable image either way.
    void* want = FindFreeBase(a.size);
    MappedImage d;
    NTSTATUS st2 = MapImageFromFile(path.c_str(), kDefaultMapFlags, want, &d);
    CHECK(st2 >= 0);
    CHECK(d.base != nullptr);
    auto add2 = reinterpret_cast<int (*)(int, int)>(FindExport(d.base, "add"));
    CHECK(add2 != nullptr);
    if (add2) CHECK_EQ(add2(20, 22), 42);
    UnmapImage(d);
    UnmapImage(b);
    UnmapImage(a);
}

TEST(loader_reserve_only_then_finalize) {
    MappedImage img;
    NTSTATUS st = MapImageFromFile(GuestPath(L"guest_dll.dll").c_str(), MapFlags::ReserveOnly, nullptr, &img);
    CHECK(st >= 0);
    void* add = FindExport(img.base, "add");
    CHECK(add != nullptr);
    CHECK_EQ(ProtAt(add), static_cast<DWORD>(PAGE_READWRITE));
    CHECK_EQ(ProtAt(img.base), static_cast<DWORD>(PAGE_READWRITE));
    CHECK(img.function_table == nullptr);   // RegisterUnwind not requested
    CHECK_EQ(FinalizeProtections(img), kStatusSuccess);
    CHECK_EQ(ProtAt(add), static_cast<DWORD>(PAGE_EXECUTE_READ));
    CHECK_EQ(ProtAt(img.base), static_cast<DWORD>(PAGE_READONLY));
    CHECK_EQ(FinalizeProtections(img), kStatusSuccess);   // idempotent
    UnmapImage(img);
}

// ===========================================================================
// Mapper: synthetic images and corner cases
// ===========================================================================
TEST(loader_reject_bad_images) {
    MappedImage img;
    Bytes junk(256, 0x41);
    CHECK_EQ(MapImageFromMemory(junk.data(), junk.size(), MapFlags::None, nullptr, &img), kStatusInvalidImageFormat);
    CHECK_EQ(MapImageFromMemory(junk.data(), 10, MapFlags::None, nullptr, &img), kStatusInvalidImageFormat);
    CHECK_EQ(MapImageFromMemory(nullptr, 0, MapFlags::None, nullptr, &img), kStatusInvalidParameter);

    SynthPE pe;
    pe.secs.push_back({".text", 0x1000, 0x1000, Bytes(16, 0x90), kRX});
    {   // PE32
        SynthPE p = pe;
        p.magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        Bytes f = p.Build();
        CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img), kStatusInvalidImageWin32);
    }
    {   // other machines
        for (int m : {IMAGE_FILE_MACHINE_ARM64, IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_ARMNT}) {
            SynthPE p = pe;
            p.machine = static_cast<std::uint16_t>(m);
            Bytes f = p.Build();
            CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img), kStatusInvalidImageFormat);
        }
    }
    Bytes good = pe.Build();
    CHECK(MapImageFromMemory(good.data(), good.size(), MapFlags::None, nullptr, &img) >= 0);
    UnmapImage(img);
    {   // truncated in the middle of the section raw data
        Bytes f = good;
        f.resize(f.size() - 0x100);
        CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img), kStatusInvalidImageFormat);
        // truncated before the section table ends
        CHECK_EQ(MapImageFromMemory(good.data(), 0x100, MapFlags::None, nullptr, &img), kStatusInvalidImageFormat);
    }
    {   // bad PE signature
        Bytes f = good;
        f[0x80] = 'X';
        CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img), kStatusInvalidImageFormat);
    }
    {   // section outside SizeOfImage
        SynthPE p = pe;
        p.size_of_image = 0x1000;
        Bytes f = p.Build();
        CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img), kStatusInvalidImageFormat);
    }
    {   // non power-of-two alignment
        SynthPE p = pe;
        p.section_alignment = 0x1800;
        Bytes f = p.Build();
        CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img), kStatusInvalidImageFormat);
    }
    {   // unsupported relocation type
        SynthPE p = pe;
        p.secs.push_back({".reloc", 0x2000, 0x100, RelocBlock(0x1000, {{0x10, IMAGE_REL_BASED_HIGH}}), kRO});
        p.dir_rva[IMAGE_DIRECTORY_ENTRY_BASERELOC] = 0x2000;
        p.dir_size[IMAGE_DIRECTORY_ENTRY_BASERELOC] = 12;
        Bytes f = p.Build();
        void* other = FindFreeBase(0x4000);
        CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, other, &img), kStatusInvalidImageFormat);
        CHECK(img.base == nullptr);
        // ...but it is irrelevant when no relocation is needed.
        p.image_base = reinterpret_cast<std::uintptr_t>(other);
        f = p.Build();
        CHECK(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img) >= 0);
        UnmapImage(img);
    }
}

TEST(loader_zero_sections_and_headers_readonly) {
    SynthPE pe;
    Bytes f = pe.Build();
    MappedImage img;
    NTSTATUS st = MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img);
    CHECK(st >= 0);
    CHECK_EQ(img.size, std::size_t(0x1000));
    CHECK_EQ(ProtAt(img.base), static_cast<DWORD>(PAGE_READONLY));
    CHECK(img.entry == nullptr);
    CHECK(FindExport(img.base, "x") == nullptr);   // no export directory at all
    UnmapImage(img);
}

TEST(loader_sizeofheaders_beyond_first_section_rva) {
    SynthPE pe;
    pe.size_of_headers = 0x1200;                       // spills 0x200 bytes into the section at 0x1000
    SynthSection text{".text", 0x1000, 0x1000, Bytes(0x1000, 0x90), kRX};
    text.raw[0] = 0xAA;
    text.raw[0x1FF] = 0xBB;
    pe.secs.push_back(text);
    Bytes f = pe.Build();
    MappedImage img;
    CHECK(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img) >= 0);
    const u8* b = static_cast<const u8*>(img.base);
    CHECK_EQ(b[0x1000], 0xAA);        // the section wins over the header padding (0xEE)
    CHECK_EQ(b[0x11FF], 0xBB);
    CHECK_EQ(b[0x1200], 0x90);
    // The overlapped page gets the union of header (R) and section (RX).
    CHECK_EQ(ProtAt(b + 0x1000), static_cast<DWORD>(PAGE_EXECUTE_READ));
    CHECK_EQ(ProtAt(b), static_cast<DWORD>(PAGE_READONLY));
    UnmapImage(img);
}

TEST(loader_unsorted_and_overlapping_sections) {
    SynthPE pe;
    // Listed high-to-low, and the middle one overlaps the first by 0x800 bytes.
    SynthSection hi{".rdata", 0x3000, 0x1000, Bytes(0x1000, 0x33), kRO};
    SynthSection mid{".data", 0x2000, 0x1000, Bytes(0x1000, 0x22), kRW};
    SynthSection lo{".text", 0x1800, 0x1000, Bytes(0x1000, 0x11), kRX};   // overlaps .data? 0x1800..0x2800 yes
    pe.secs = {hi, mid, lo};
    Bytes f = pe.Build();
    MappedImage img;
    NTSTATUS st = MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img);
    CHECK(st >= 0);
    if (st < 0) return;
    const u8* b = static_cast<const u8*>(img.base);
    CHECK_EQ(b[0x1800], 0x11);
    CHECK_EQ(b[0x1FFF], 0x11);
    CHECK_EQ(b[0x2000], 0x22);   // ascending-RVA copy order: the higher section overwrites the overlap
    CHECK_EQ(b[0x2FFF], 0x22);
    CHECK_EQ(b[0x3000], 0x33);
    // page 0x1000 holds .text only; page 0x2000 is .text|.data (union); page 0x3000 .rdata only
    CHECK_EQ(ProtAt(b + 0x1000), static_cast<DWORD>(PAGE_EXECUTE_READ));
    CHECK_EQ(ProtAt(b + 0x2000), static_cast<DWORD>(PAGE_EXECUTE_READWRITE));
    CHECK_EQ(ProtAt(b + 0x3000), static_cast<DWORD>(PAGE_READONLY));
    UnmapImage(img);
}

TEST(loader_zero_fill_and_virtual_size) {
    SynthPE pe;
    SynthSection d{".data", 0x1000, 0x3000, Bytes(0x10, 0x5A), kRW};   // VirtualSize >> raw size
    SynthSection s{".sml", 0x4000, 0x8, Bytes(0x200, 0x77), kRO};      // VirtualSize < raw size: only 8 bytes copied
    pe.secs = {d, s};
    Bytes f = pe.Build();
    MappedImage img;
    CHECK(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img) >= 0);
    const u8* b = static_cast<const u8*>(img.base);
    CHECK_EQ(b[0x100F], 0x5A);
    for (std::uint32_t i = 0x1010; i < 0x4000; i += 0x7F) CHECK_EQ(b[i], 0);
    CHECK_EQ(b[0x4007], 0x77);
    CHECK_EQ(b[0x4008], 0);
    UnmapImage(img);
}

TEST(loader_relocations) {
    SynthPE pe;
    pe.image_base = 0x40000000;
    SynthSection text{".text", 0x1000, 0x3000, Bytes(0x3000, 0), kRW};
    Put64(text.raw, 0x10, pe.image_base + 0x1234);       // DIR64
    Put64(text.raw, 0xFFC, pe.image_base + 0x5678);      // DIR64 spanning the 0x1000/0x2000 page boundary
    Put32(text.raw, 0x1020, static_cast<std::uint32_t>(pe.image_base + 0x77));   // HIGHLOW
    Put64(text.raw, 0x1100, pe.image_base + 0x99);       // in the block that follows the odd-sized one
    Put64(text.raw, 0x1200, 0x4141414141414141ull);      // rva 0x2200: not relocated
    // block1: 4 entries incl. ABSOLUTE padding; block2: odd size (11): 1 entry + 1 stray byte;
    // block3 starts at an odd offset and lives on the next page.
    Bytes rel = RelocBlock(0x1000, {{0x10, IMAGE_REL_BASED_DIR64}, {0xFFC, IMAGE_REL_BASED_DIR64},
                                    {0, IMAGE_REL_BASED_ABSOLUTE}, {0, IMAGE_REL_BASED_ABSOLUTE}});
    Bytes b2 = RelocBlock(0x2000, {{0x20, IMAGE_REL_BASED_HIGHLOW}}, 1);
    Bytes b3 = RelocBlock(0x2000, {{0x100, IMAGE_REL_BASED_DIR64}});
    CHECK_EQ(b2.size() % 2, std::size_t(1));
    rel.insert(rel.end(), b2.begin(), b2.end());
    rel.insert(rel.end(), b3.begin(), b3.end());
    // zero padding after the last block must be tolerated as a terminator
    const std::uint32_t dir_size = static_cast<std::uint32_t>(rel.size());
    rel.resize(rel.size() + 8, 0);
    pe.secs = {text, {".reloc", 0x4000, 0x100, rel, kRO}};
    pe.dir_rva[IMAGE_DIRECTORY_ENTRY_BASERELOC] = 0x4000;
    pe.dir_size[IMAGE_DIRECTORY_ENTRY_BASERELOC] = dir_size + 8;
    Bytes f = pe.Build();

    void* want = FindFreeBase(0x10000);
    MappedImage img;
    NTSTATUS st = MapImageFromMemory(f.data(), f.size(), MapFlags::None, want, &img);
    CHECK_EQ(st, kStatusImageNotAtBase);
    CHECK(img.base == want);
    CHECK(img.relocated);
    const std::uint64_t nb = reinterpret_cast<std::uintptr_t>(img.base);
    CHECK_EQ(Rd64(img.base, 0x1010), nb + 0x1234);
    CHECK_EQ(Rd64(img.base, 0x1FFC), nb + 0x5678);
    CHECK_EQ(Rd32(img.base, 0x2020), static_cast<std::uint32_t>(nb + 0x77));
    CHECK_EQ(Rd64(img.base, 0x2100), nb + 0x99);          // block 3 starts at an odd offset
    CHECK_EQ(Rd64(img.base, 0x2200), 0x4141414141414141ull);   // not listed: untouched
    UnmapImage(img);

    // At its own base nothing is touched.
    MappedImage at_base;
    pe.image_base = reinterpret_cast<std::uintptr_t>(FindFreeBase(0x10000));
    Put64(pe.secs[0].raw, 0x10, pe.image_base + 0x1234);
    f = pe.Build();
    CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &at_base), kStatusSuccess);
    CHECK(!at_base.relocated);
    CHECK_EQ(Rd64(at_base.base, 0x1010), pe.image_base + 0x1234);
    UnmapImage(at_base);
}

TEST(loader_relocs_stripped_and_no_relocate) {
    SynthPE pe;
    pe.file_chars |= IMAGE_FILE_RELOCS_STRIPPED;
    pe.image_base = reinterpret_cast<std::uintptr_t>(FindFreeBase(0x10000));
    pe.secs.push_back({".text", 0x1000, 0x1000, Bytes(0x10, 0x90), kRX});
    Bytes f = pe.Build();
    MappedImage a, b;
    CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &a), kStatusSuccess);
    CHECK(a.base == reinterpret_cast<void*>(static_cast<std::uintptr_t>(pe.image_base)));
    // Base now occupied: a stripped image must not be moved.
    CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &b), kStatusConflictingAddresses);
    CHECK(b.base == nullptr);
    UnmapImage(a);

    // NoRelocate on a normal image: same behaviour.
    SynthPE pe2;
    pe2.image_base = reinterpret_cast<std::uintptr_t>(FindFreeBase(0x10000));
    pe2.secs.push_back({".text", 0x1000, 0x1000, Bytes(0x10, 0x90), kRX});
    Bytes f2 = pe2.Build();
    CHECK_EQ(MapImageFromMemory(f2.data(), f2.size(), MapFlags::NoRelocate, nullptr, &a), kStatusSuccess);
    CHECK_EQ(MapImageFromMemory(f2.data(), f2.size(), MapFlags::NoRelocate, nullptr, &b), kStatusConflictingAddresses);
    UnmapImage(a);
}

TEST(loader_image_base_zero) {
    SynthPE pe;
    pe.image_base = 0;
    SynthSection d{".data", 0x1000, 0x1000, Bytes(0x1000, 0), kRW};
    Put64(d.raw, 0x40, 0x1234);   // image_base(0) + 0x1234
    pe.secs = {d, {".reloc", 0x2000, 0x100, RelocBlock(0x1000, {{0x40, IMAGE_REL_BASED_DIR64}}), kRO}};
    pe.dir_rva[IMAGE_DIRECTORY_ENTRY_BASERELOC] = 0x2000;
    pe.dir_size[IMAGE_DIRECTORY_ENTRY_BASERELOC] = 12;
    Bytes f = pe.Build();
    MappedImage img;
    CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img), kStatusImageNotAtBase);
    CHECK(img.relocated);
    CHECK_EQ(img.image_base_preferred, std::uint64_t(0));
    CHECK_EQ(Rd64(img.base, 0x1040), 0x1234 + reinterpret_cast<std::uintptr_t>(img.base));
    UnmapImage(img);
    MappedImage img2;
    CHECK_EQ(MapImageFromMemory(f.data(), f.size(), MapFlags::NoRelocate, nullptr, &img2), kStatusConflictingAddresses);
}

TEST(loader_exports_corner_cases) {
    SynthPE pe;
    pe.secs.push_back({".text", 0x1000, 0x1000, Bytes(0x100, 0x90), kRX});
    // function table: ordinal base 5.  idx0 foo, idx1 hole, idx2 forwarder "NTDLL.RtlFoo",
    // idx3 ordinal forwarder, idx4 out-of-range RVA, idx5 bar.
    std::vector<ExpFunc> funcs = {{0x1010, {}}, {0, {}}, {0, "NTDLL.RtlFoo"}, {0, "NTDLL.#123"}, {0x7FFFFFF0, {}}, {0x1020, {}}};
        std::vector<std::pair<std::string, std::uint32_t>> names = {
        {"FwdName", 2}, {"zeta", 0}, {"bar", 5}, {"foo", 0}, {"OrdFwd", 3}, {"OutOfRange", 4}, {"Hole", 1}};   // not sorted
    std::uint32_t esz = 0;
    Bytes exp = BuildExports(0x2000, 5, funcs, names, &esz);
    pe.secs.push_back({".edata", 0x2000, static_cast<std::uint32_t>(exp.size()), exp, kRO});
    pe.dir_rva[IMAGE_DIRECTORY_ENTRY_EXPORT] = 0x2000;
    pe.dir_size[IMAGE_DIRECTORY_ENTRY_EXPORT] = esz;
    Bytes f = pe.Build();
    MappedImage img;
    CHECK(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img) >= 0);
    const u8* b = static_cast<const u8*>(img.base);
    std::string fwd;
    CHECK(FindExport(img.base, "foo", &fwd) == b + 0x1010);
    CHECK(fwd.empty());
    CHECK(FindExport(img.base, "zeta") == b + 0x1010);          // alias of foo, found by linear fallback
    CHECK(FindExport(img.base, "bar") == b + 0x1020);
    CHECK(FindExport(img.base, "FwdName", &fwd) == nullptr);
    CHECK_EQ(fwd, std::string("NTDLL.RtlFoo"));
    CHECK(FindExport(img.base, "OrdFwd", &fwd) == nullptr);
    CHECK_EQ(fwd, std::string("NTDLL.#123"));
    CHECK(FindExport(img.base, "OutOfRange", &fwd) == nullptr);
    CHECK(fwd.empty());
    CHECK(FindExport(img.base, "Hole") == nullptr);
    CHECK(FindExport(img.base, "fo") == nullptr);               // prefix of an existing name
    CHECK(FindExport(img.base, "fooo") == nullptr);
    // By ordinal (base 5): 5..10
    CHECK(FindExportByOrdinal(img.base, 5) == b + 0x1010);
    CHECK(FindExportByOrdinal(img.base, 6) == nullptr);         // hole
    CHECK(FindExportByOrdinal(img.base, 7, &fwd) == nullptr);
    CHECK_EQ(fwd, std::string("NTDLL.RtlFoo"));
    CHECK(FindExportByOrdinal(img.base, 8, &fwd) == nullptr);
    CHECK_EQ(fwd, std::string("NTDLL.#123"));
    CHECK(FindExportByOrdinal(img.base, 9) == nullptr);         // RVA past SizeOfImage
    CHECK(FindExportByOrdinal(img.base, 10) == b + 0x1020);
    CHECK(FindExportByOrdinal(img.base, 11) == nullptr);        // beyond the table
    CHECK(FindExportByOrdinal(img.base, 1) == nullptr);         // below the ordinal base
    CHECK(FindExportByOrdinal(img.base, 0) == nullptr);
    UnmapImage(img);
}

// ===========================================================================
// Syscall stub rewriting
// ===========================================================================
namespace {
Bytes WindowsThunk(std::uint32_t id) {
    Bytes t = {0x4C, 0x8B, 0xD1, 0xB8, 0, 0, 0, 0, 0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01,
               0x75, 0x03, 0x0F, 0x05, 0xC3, 0xCD, 0x2E, 0xC3};
    Put32(t, 4, id);
    return t;
}
Bytes WineThunk(std::uint32_t id) {
    Bytes t = {0x4C, 0x8B, 0xD1, 0xB8, 0, 0, 0, 0, 0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01,
               0x75, 0x03, 0x0F, 0x05, 0xC3, 0xEB, 0x01, 0xC3, 0xFF, 0x14, 0x25, 0x00, 0x10, 0xFE, 0x7F, 0xC3};
    Put32(t, 4, id);
    return t;
}
}  // namespace

TEST(loader_rewrite_syscall_stubs) {
    // .text: NtAlpha(0x10) windows form @0x1000, NtBeta(0x2A) wine form @0x1020 (also exported as ZwBeta),
    // NtGamma @0x1040 windows form, NtBroken @0x1060 (wrong tail), NtdllHelper @0x1080 (not a syscall name).
    Bytes text(0x200, 0xCC);
    auto put_thunk = [&](std::size_t at, const Bytes& t) { std::memcpy(text.data() + at, t.data(), t.size()); };
    put_thunk(0x00, WindowsThunk(0x10));
    put_thunk(0x20, WineThunk(0x2A));
    put_thunk(0x60 - 0x20, WindowsThunk(0x33));    // 0x40
    Bytes broken = WindowsThunk(0x44);
    broken[18] = 0x90;                              // syscall opcode corrupted
    put_thunk(0x60, broken);
    put_thunk(0x80, WindowsThunk(0x55));
    SynthPE pe;
    pe.secs.push_back({".text", 0x1000, 0x200, text, kRX});
    std::vector<ExpFunc> funcs = {{0x1000, {}}, {0x1020, {}}, {0x1040, {}}, {0x1060, {}}, {0x1080, {}}, {0, "ntdll.NtForwarded"}};
    std::vector<std::pair<std::string, std::uint32_t>> names = {
        {"NtAlpha", 0}, {"NtBeta", 1}, {"NtBroken", 3}, {"NtForwarded", 5}, {"NtGamma", 2}, {"NtdllHelper", 4}, {"ZwBeta", 1}, {"ZwGamma", 2}};
    std::uint32_t esz = 0;
    Bytes exp = BuildExports(0x2000, 1, funcs, names, &esz);
    pe.secs.push_back({".edata", 0x2000, static_cast<std::uint32_t>(exp.size()), exp, kRO});
    pe.dir_rva[IMAGE_DIRECTORY_ENTRY_EXPORT] = 0x2000;
    pe.dir_size[IMAGE_DIRECTORY_ENTRY_EXPORT] = esz;
    Bytes f = pe.Build();

    MappedImage img;
    CHECK(MapImageFromMemory(f.data(), f.size(), MapFlags::None, nullptr, &img) >= 0);
    u8* b = static_cast<u8*>(img.base);
    u8 before_beta[32];
    std::memcpy(before_beta, b + 0x1020, 32);

    void* target = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x00007FF123456789ull));
    std::vector<StubPatch> patched;
    std::vector<std::string> unmatched;
    std::size_t n = RewriteSyscallStubs(img, target, &patched, &unmatched);
    CHECK_EQ(n, std::size_t(3));
    CHECK_EQ(patched.size(), std::size_t(3));
    int seen = 0;
    for (auto& p : patched) {
        if (p.name == "NtAlpha") { CHECK_EQ(p.id, 0x10u); CHECK(p.address == b + 0x1000); ++seen; }
        if (p.name == "NtBeta")  { CHECK_EQ(p.id, 0x2Au); CHECK(p.address == b + 0x1020); ++seen; }
        if (p.name == "NtGamma") { CHECK_EQ(p.id, 0x33u); CHECK(p.address == b + 0x1040); ++seen; }
        CHECK(p.name.compare(0, 2, "Zw") != 0);   // aliases are reported once, under the Nt* name
    }
    CHECK_EQ(seen, 3);
    // unmatched: the broken thunk and the forwarder; neither the helper nor the Zw aliases
    CHECK_EQ(unmatched.size(), std::size_t(2));
    bool u_broken = false, u_fwd = false;
    for (auto& s : unmatched) { u_broken |= s == "NtBroken"; u_fwd |= s == "NtForwarded"; }
    CHECK(u_broken && u_fwd);

    static const u8 jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    for (std::uint32_t off : {0x1000u, 0x1020u, 0x1040u}) {
        CHECK(std::memcmp(b + off, "\x4C\x8B\xD1\xB8", 4) == 0);   // mov r10,rcx ; mov eax,id preserved
        CHECK(std::memcmp(b + off + 8, jmp, 6) == 0);
        std::uint64_t t;
        std::memcpy(&t, b + off + 14, 8);
        CHECK_EQ(t, 0x00007FF123456789ull);
    }
    CHECK_EQ(Rd32(b, 0x1004), 0x10u);
    CHECK_EQ(Rd32(b, 0x1024), 0x2Au);
    CHECK(std::memcmp(b + 0x1060, broken.data(), broken.size()) == 0);   // untouched
    // Wine thunk bytes after the 22 rewritten ones are left alone.
    CHECK(std::memcmp(b + 0x1020 + 22, before_beta + 22, 10) == 0);
    CHECK_EQ(ProtAt(b + 0x1000), static_cast<DWORD>(PAGE_EXECUTE_READ));   // protection restored

    // Re-running re-targets (rewritten thunks are recognised).
    void* target2 = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x00007FF000000042ull));
    patched.clear();
    unmatched.clear();
    CHECK_EQ(RewriteSyscallStubs(img, target2, &patched, &unmatched), std::size_t(3));
    std::uint64_t t2;
    std::memcpy(&t2, b + 0x1000 + 14, 8);
    CHECK_EQ(t2, 0x00007FF000000042ull);
    CHECK_EQ(RewriteSyscallStubs(img, nullptr, nullptr, nullptr), std::size_t(0));
    UnmapImage(img);
}

// ===========================================================================
// Scanner
// ===========================================================================
namespace {

struct TestCtx {
    bool decline = false;
    int calls = 0;
    int by_kind[8] = {};
};

bool TestHandler(SiteKind k, ScanRegs* r, void* c) {
    auto* x = static_cast<TestCtx*>(c);
    ++x->calls;
    ++x->by_kind[static_cast<int>(k)];
    if (x->decline) return false;
    switch (k) {
    case SiteKind::Cpuid: {
        int o[4];
        __cpuidex(o, static_cast<int>(r->rax), static_cast<int>(r->rcx));
        r->rax = static_cast<std::uint32_t>(o[0]);
        r->rbx = static_cast<std::uint32_t>(o[1]);
        r->rcx = static_cast<std::uint32_t>(o[2]);
        r->rdx = static_cast<std::uint32_t>(o[3]);
        return true;
    }
    case SiteKind::Rdtsc:
        r->rax = 0x11;
        r->rdx = 0x22;
        return true;
    case SiteKind::Rdtscp:
        r->rax = 0x11;
        r->rdx = 0x22;
        r->rcx = 0x33;
        return true;
    case SiteKind::KuserRead:
        r->rax = 0xCAFE;
        return true;
    default:   // syscall, sysenter, int 2e
        r->rax = 7;
        return true;
    }
}

// The cpuid handler used for hypervisor-bit hiding: leaf 1 ECX bit 31 cleared.
bool HideHypervisorHandler(SiteKind k, ScanRegs* r, void* c) {
    if (k == SiteKind::Cpuid) {
        int o[4];
        std::uint32_t leaf = static_cast<std::uint32_t>(r->rax);
        __cpuidex(o, static_cast<int>(leaf), static_cast<int>(r->rcx));
        if (leaf == 1) o[2] &= 0x7FFFFFFF;
        r->rax = static_cast<std::uint32_t>(o[0]);
        r->rbx = static_cast<std::uint32_t>(o[1]);
        r->rcx = static_cast<std::uint32_t>(o[2]);
        r->rdx = static_cast<std::uint32_t>(o[3]);
        return true;
    }
    if (k == SiteKind::Rdtsc) {
        r->rax = 0x11111111;
        r->rdx = 0x22222222;
        return true;
    }
    return TestHandler(k, r, c);
}

struct CodeBuf {
    u8* mem = nullptr;
    std::size_t size = 0;
    std::vector<std::size_t> off;
    template <class F> F fn(std::size_t i) const { return reinterpret_cast<F>(mem + off[i]); }
};

CodeBuf MakeCode(const std::vector<Bytes>& funcs) {
    CodeBuf cb;
    std::size_t total = 0;
    for (auto& f : funcs) {
        cb.off.push_back(total);
        total += (f.size() + 15) & ~std::size_t(15);
    }
    cb.size = (total + 0xFFF) & ~std::size_t(0xFFF);
    cb.mem = static_cast<u8*>(VirtualAlloc(nullptr, cb.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    std::memset(cb.mem, 0xCC, cb.size);
    for (std::size_t i = 0; i < funcs.size(); ++i) std::memcpy(cb.mem + cb.off[i], funcs[i].data(), funcs[i].size());
    DWORD old;
    VirtualProtect(cb.mem, cb.size, PAGE_EXECUTE_READ, &old);
    return cb;
}
void FreeCode(CodeBuf& cb) {
    if (!cb.mem) return;
    ReleaseScanSites(cb.mem, cb.size);
    VirtualFree(cb.mem, 0, MEM_RELEASE);
    cb.mem = nullptr;
}

// Hand-assembled test functions (verified with llvm-mc).
const Bytes kFnInline    = {0x53, 0xB8, 1, 0, 0, 0, 0x31, 0xC9, 0x0F, 0xA2, 0x89, 0xC8, 0x5B, 0xC3};            // ecx of cpuid(1)
const Bytes kFnTrap      = {0x0F, 0x31, 0xC3};                                                                  // rdtsc; ret
const Bytes kFnRipRel    = {0x53, 0xB8, 1, 0, 0, 0, 0x31, 0xC9, 0x0F, 0xA2, 0x48, 0x8B, 0x15, 4, 0, 0, 0,
                            0x89, 0xD0, 0x5B, 0xC3, 0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0};                         // mov rdx,[rip+data]
const Bytes kFnJcc       = {0x53, 0x41, 0x89, 0xC8, 0xB8, 0, 0, 0, 0, 0x31, 0xC9, 0x45, 0x85, 0xC0, 0x0F, 0xA2,
                            0x74, 0x07, 0xB8, 0x6F, 0, 0, 0, 0x5B, 0xC3, 0xB8, 0xDE, 0, 0, 0, 0x5B, 0xC3};      // je after cpuid
const Bytes kFnTarget    = {0x53, 0x41, 0x89, 0xC8, 0xB8, 1, 0, 0, 0, 0x31, 0xC9, 0x45, 0x85, 0xC0, 0x75, 0x02,
                            0x0F, 0xA2, 0x89, 0xC8, 0x5B, 0xC3};                                                // branch into displaced bytes
const Bytes kFnRdtsc     = {0x0F, 0x31, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09, 0xD0, 0xC3};
const Bytes kFnKuser     = {0x8B, 0x04, 0x25, 0x00, 0x00, 0xFE, 0x7F, 0xC3};
const Bytes kFnSyscallT  = {0xB8, 0x34, 0x12, 0, 0, 0x0F, 0x05, 0xC3};                                          // syscall; ret (trap)
const Bytes kFnSyscallI  = {0xB8, 0x34, 0x12, 0, 0, 0x0F, 0x05, 0x31, 0xC9, 0x90, 0xC3};
const Bytes kFnInt2E     = {0xCD, 0x2E, 0x31, 0xC9, 0x90, 0xC3};
const Bytes kFnSysenter  = {0x0F, 0x34, 0x31, 0xC9, 0x90, 0xC3};
const Bytes kFnRdtscp    = {0x0F, 0x01, 0xF9, 0x89, 0xC8, 0xC3};

using FnU = unsigned (*)();
using FnU1 = unsigned (*)(unsigned);
using FnQ = std::uint64_t (*)();

unsigned RealEcx1() {
    int o[4];
    __cpuidex(o, 1, 0);
    return static_cast<unsigned>(o[2]);
}

}  // namespace

TEST(loader_scan_inline_and_trap) {
    CodeBuf cb = MakeCode({kFnInline, kFnTrap, kFnRipRel, kFnJcc, kFnTarget, kFnRdtsc});
    TestCtx ctx;
    ScanOptions opt;
    opt.handler = TestHandler;
    opt.handler_ctx = &ctx;
    ScanResult res;
    CHECK_EQ(ScanAndPatchRange(cb.mem, cb.size, opt, &res), kStatusSuccess);
    // inline: kFnInline, kFnRipRel, kFnJcc, kFnRdtsc.  trap: kFnTrap (ret follows), kFnTarget (branch target).
    CHECK_EQ(res.patched, std::size_t(4));
    CHECK_EQ(res.trapped, std::size_t(2));
    CHECK_EQ(res.skipped, std::size_t(0));
    CHECK_EQ(res.sites.size(), std::size_t(6));
    for (auto& s : res.sites) {
        bool is_trap = s.address == cb.mem + cb.off[1] || s.address == cb.mem + cb.off[4] + 16;
        CHECK_EQ(s.patched_inline, !is_trap);
        CHECK(s.length == 2);
    }

    // cpuid(1): handler returns the real ecx (the function returns it).
    CHECK_EQ(cb.fn<FnU>(0)(), RealEcx1());
    CHECK_EQ(ctx.by_kind[static_cast<int>(SiteKind::Cpuid)], 1);
    // trap path: rdtsc -> eax = 0x11
    CHECK_EQ(cb.fn<FnU>(1)(), 0x11u);
    // displaced rip-relative load keeps pointing at the data
    CHECK_EQ(cb.fn<FnU>(2)(), 0x12345678u);
    // displaced conditional jump + flags survive the handler
    CHECK_EQ(cb.fn<FnU1>(3)(0), 222u);    // test r8d,r8d -> ZF=1 -> je taken
    CHECK_EQ(cb.fn<FnU1>(3)(5), 111u);
    // branch into the displaced area: both paths work
    CHECK_EQ(cb.fn<FnU1>(4)(1), 0u);      // cpuid skipped, ecx = 0
    CHECK_EQ(cb.fn<FnU1>(4)(0), RealEcx1());
    CHECK_EQ(cb.fn<FnQ>(5)(), 0x2200000011ull);

    // Scanning again finds nothing new (patched regions are protected).
    int calls_before = ctx.calls;
    ScanResult res2;
    CHECK_EQ(ScanAndPatchRange(cb.mem, cb.size, opt, &res2), kStatusSuccess);
    CHECK_EQ(res2.sites.size(), std::size_t(0));
    CHECK_EQ(cb.fn<FnU1>(3)(0), 222u);
    CHECK_EQ(ctx.calls, calls_before + 1);

    // Protection of the patched code is restored.
    CHECK_EQ(ProtAt(cb.mem), static_cast<DWORD>(PAGE_EXECUTE_READ));
    FreeCode(cb);
}

namespace {
volatile float g_sink[64];
// Does a lot of floating point work so that xmm0-xmm5 and the volatile GPRs are certainly clobbered.
bool ClobberingHandler(SiteKind k, ScanRegs* r, void* c) {
    for (int i = 0; i < 64; ++i) g_sink[i] = static_cast<float>(i) * 1.5f + static_cast<float>(r->rax);
    __m128 a = _mm_set1_ps(3.0f), b = _mm_set1_ps(4.0f);
    _mm_storeu_ps(const_cast<float*>(g_sink), _mm_mul_ps(a, b));
    return TestHandler(k, r, c);
}
// push rbx/rsi/rdi/rbp/r12-r15; esi..r15d = 1..10, ebp = 11; xmm0 = 10, xmm9 = 9; cpuid(0);
// then eax = sum of all of them (66 + 10 + 9 = 85).
const Bytes kFnRegs = {
    0x53, 0x56, 0x57, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xBE, 0x01, 0x00, 0x00, 0x00, 0xBF, 0x02,
    0x00, 0x00, 0x00, 0x41, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x41, 0xB9, 0x04, 0x00, 0x00, 0x00, 0x41, 0xBA, 0x05, 0x00,
    0x00, 0x00, 0x41, 0xBB, 0x06, 0x00, 0x00, 0x00, 0x41, 0xBC, 0x07, 0x00, 0x00, 0x00, 0x41, 0xBD, 0x08, 0x00, 0x00,
    0x00, 0x41, 0xBE, 0x09, 0x00, 0x00, 0x00, 0x41, 0xBF, 0x0A, 0x00, 0x00, 0x00, 0xBD, 0x0B, 0x00, 0x00, 0x00, 0x66,
    0x41, 0x0F, 0x6E, 0xC7, 0x66, 0x45, 0x0F, 0x6E, 0xCE, 0x31, 0xC0, 0x31, 0xC9, 0x0F, 0xA2, 0x31, 0xC0, 0x01, 0xF0,
    0x01, 0xF8, 0x44, 0x01, 0xC0, 0x44, 0x01, 0xC8, 0x44, 0x01, 0xD0, 0x44, 0x01, 0xD8, 0x44, 0x01, 0xE0, 0x44, 0x01,
    0xE8, 0x44, 0x01, 0xF0, 0x44, 0x01, 0xF8, 0x01, 0xE8, 0x66, 0x0F, 0x7E, 0xC2, 0x01, 0xD0, 0x66, 0x44, 0x0F, 0x7E,
    0xCA, 0x01, 0xD0, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5F, 0x5E, 0x5B, 0xC3};
}  // namespace

TEST(loader_scan_thunk_preserves_gprs_and_xmm) {
    CodeBuf cb = MakeCode({kFnRegs});
    TestCtx ctx;
    ScanOptions opt;
    opt.handler = ClobberingHandler;
    opt.handler_ctx = &ctx;
    ScanResult res;
    CHECK_EQ(ScanAndPatchRange(cb.mem, cb.size, opt, &res), kStatusSuccess);
    CHECK_EQ(res.patched, std::size_t(1));
    CHECK_EQ(res.trapped, std::size_t(0));
    CHECK_EQ(cb.fn<FnU>(0)(), 85u);
    CHECK_EQ(ctx.calls, 1);
    FreeCode(cb);
}

TEST(loader_scan_handler_declines_executes_original) {
    CodeBuf cb = MakeCode({kFnInline, kFnTrap, kFnRdtscp, kFnRdtsc});
    TestCtx ctx;
    ctx.decline = true;
    ScanOptions opt;
    opt.handler = TestHandler;
    opt.handler_ctx = &ctx;
    ScanResult res;
    CHECK_EQ(ScanAndPatchRange(cb.mem, cb.size, opt, &res), kStatusSuccess);
    CHECK_EQ(res.patched + res.trapped, std::size_t(4));
    CHECK_EQ(cb.fn<FnU>(0)(), RealEcx1());             // inline path runs the real cpuid
    unsigned t0 = cb.fn<FnU>(1)();                       // trap path runs the real rdtsc
    unsigned t1 = cb.fn<FnU>(1)();
    CHECK(t0 != 0x11u || t1 != 0x11u);
    CHECK(cb.fn<FnQ>(3)() != 0x2200000011ull);
    CHECK(ctx.by_kind[static_cast<int>(SiteKind::Cpuid)] == 1);
    CHECK_EQ(ctx.by_kind[static_cast<int>(SiteKind::Rdtsc)], 3);
    // Declined rdtscp keeps ecx = IA32_TSC_AUX (just make sure it executes and returns).
    (void)cb.fn<FnU>(2)();
    FreeCode(cb);
}

TEST(loader_scan_kinds_and_options) {
    CodeBuf cb = MakeCode({kFnSyscallT, kFnSyscallI, kFnInt2E, kFnSysenter, kFnRdtscp, kFnKuser, kFnInline});
    TestCtx ctx;
    ScanOptions opt;
    opt.handler = TestHandler;
    opt.handler_ctx = &ctx;

    // Everything disabled: nothing happens, nothing executes patched.
    ScanOptions none = opt;
    none.patch_cpuid = none.patch_rdtsc = none.patch_syscall = false;
    ScanResult r0;
    CHECK_EQ(ScanAndPatchRange(cb.mem, cb.size, none, &r0), kStatusSuccess);
    CHECK_EQ(r0.sites.size(), std::size_t(0));

    // syscall-class only (kuser reads off by default).
    ScanOptions sc = opt;
    sc.patch_cpuid = sc.patch_rdtsc = false;
    ScanResult r1;
    CHECK_EQ(ScanAndPatchRange(cb.mem, cb.size, sc, &r1), kStatusSuccess);
    CHECK_EQ(r1.sites.size(), std::size_t(4));
    CHECK_EQ(r1.trapped, std::size_t(1));   // "syscall; ret"
    CHECK_EQ(r1.patched, std::size_t(3));
    CHECK_EQ(cb.fn<FnU>(0)(), 7u);
    CHECK_EQ(cb.fn<FnU>(1)(), 7u);
    CHECK_EQ(cb.fn<FnU>(2)(), 7u);
    CHECK_EQ(cb.fn<FnU>(3)(), 7u);
    CHECK_EQ(ctx.by_kind[static_cast<int>(SiteKind::Syscall)], 2);
    CHECK_EQ(ctx.by_kind[static_cast<int>(SiteKind::Int2E)], 1);
    CHECK_EQ(ctx.by_kind[static_cast<int>(SiteKind::Sysenter)], 1);

    // Now the rest, with KUSER reads on.
    ScanOptions rest = opt;
    rest.patch_kuser_reads = true;
    ScanResult r2;
    CHECK_EQ(ScanAndPatchRange(cb.mem, cb.size, rest, &r2), kStatusSuccess);
    CHECK_EQ(r2.sites.size(), std::size_t(3));   // rdtscp, kuser read, cpuid
    bool saw_kuser = false;
    for (auto& s : r2.sites) if (s.kind == SiteKind::KuserRead) { saw_kuser = true; CHECK_EQ(s.length, 7); }
    CHECK(saw_kuser);
    CHECK_EQ(cb.fn<FnU>(4)(), 0x33u);
    CHECK_EQ(cb.fn<FnU>(5)(), 0xCAFEu);
    CHECK_EQ(cb.fn<FnU>(6)(), RealEcx1());

    ScanOptions bad;   // no handler
    ScanResult r3;
    CHECK_EQ(ScanAndPatchRange(cb.mem, cb.size, bad, &r3), kStatusInvalidParameter);
    CHECK_EQ(ScanAndPatchRange(nullptr, 0, opt, &r3), kStatusInvalidParameter);
    FreeCode(cb);
}

namespace {
int g_foreign_breakpoints = 0;
LONG CALLBACK ForeignBreakpointVeh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    ++g_foreign_breakpoints;
    return EXCEPTION_CONTINUE_EXECUTION;
}
}  // namespace

TEST(loader_scan_ignores_unrelated_breakpoints) {
    EnsureTrapHandlerInstalled();
    EnsureTrapHandlerInstalled();   // idempotent
    // A handler registered *after* ours (first=0) only sees the exception if ours returned CONTINUE_SEARCH.
    PVOID h = AddVectoredExceptionHandler(0, ForeignBreakpointVeh);
    CHECK(h != nullptr);
    RaiseException(EXCEPTION_BREAKPOINT, 0, 0, nullptr);
    RemoveVectoredExceptionHandler(h);
    CHECK_EQ(g_foreign_breakpoints, 1);
}

#if defined(_MSC_VER)
// SEH flavour of the same check (clang-mingw cannot compile __try, so MSVC only):
// a breakpoint that is not one of our sites must reach the frame-based handler.
TEST(loader_scan_unrelated_breakpoint_reaches_seh) {
    EnsureTrapHandlerInstalled();
    int caught = 0;
    __try {
        RaiseException(EXCEPTION_BREAKPOINT, 0, 0, nullptr);
    } __except (GetExceptionCode() == EXCEPTION_BREAKPOINT ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        caught = 1;
    }
    CHECK_EQ(caught, 1);
}
#endif

TEST(loader_scan_guest_dll_hide_hypervisor_and_fix_tsc) {
    MappedImage img;
    NTSTATUS st = MapImageFromFile(GuestPath(L"guest_dll.dll").c_str(), kDefaultMapFlags, nullptr, &img);
    CHECK(st >= 0);
    if (st < 0) return;
    auto cpuid_ecx1 = reinterpret_cast<unsigned (*)()>(FindExport(img.base, "cpuid_ecx1"));
    auto read_tsc = reinterpret_cast<std::uint64_t (*)()>(FindExport(img.base, "read_tsc"));
    CHECK(cpuid_ecx1 && read_tsc);
    if (!cpuid_ecx1 || !read_tsc) return;
    unsigned before = cpuid_ecx1();
    CHECK_EQ(before, RealEcx1());
    TestCtx ctx;
    ScanOptions opt;
    opt.handler = HideHypervisorHandler;
    opt.handler_ctx = &ctx;
    ScanResult res;
    CHECK_EQ(ScanAndPatch(img, opt, &res), kStatusSuccess);
    CHECK(res.patched + res.trapped >= 2);
    CHECK_EQ(res.sites.size(), res.patched + res.trapped);
    // Every reported site lies in the image's executable code.
    for (auto& s : res.sites) {
        CHECK(s.address >= img.base && s.address < static_cast<u8*>(img.base) + img.size);
    }
    unsigned after = cpuid_ecx1();
    CHECK_EQ(after & 0x80000000u, 0u);
    CHECK_EQ(after, before & 0x7FFFFFFFu);
    CHECK_EQ(read_tsc(), 0x2222222211111111ull);
    CHECK_EQ(read_tsc(), 0x2222222211111111ull);
    auto add = reinterpret_cast<int (*)(int, int)>(FindExport(img.base, "add"));
    CHECK(add && add(3, 4) == 7);
    // Idempotent.
    ScanResult res2;
    CHECK_EQ(ScanAndPatch(img, opt, &res2), kStatusSuccess);
    CHECK_EQ(res2.sites.size(), std::size_t(0));
    CHECK_EQ(read_tsc(), 0x2222222211111111ull);
    std::printf("  guest_dll: %zu inline, %zu trapped, %zu skipped\n", res.patched, res.trapped, res.skipped);
    UnmapImage(img);
}

// A jump table inside the code section, which is where MSVC puts it without optimisation: the bytes of its entries
// decode as instructions, one of which is `rdtsc` here (the entry 0x310F is 0F 31 00 00). Before the scanner learned
// to leave such tables out, the patch wrote an INT3 over an entry and the switch jumped into the middle of another
// instruction (a Debug build of tests/guest/sqlite/lockfile.c: printf of a %lu died with an access violation).
TEST(loader_scan_leaves_jump_tables_in_code_alone) {
    SynthPE pe;
    pe.entry = 0x1000;
    Bytes code(0x100, 0xCC);
    const u8 head[] = {
        0x48, 0x8D, 0x0D, 0xF9, 0xEF, 0xFF, 0xFF,         // lea rcx, [rip-0x1007]      ; rcx = image base
        0x8B, 0x84, 0x81, 0x20, 0x10, 0x00, 0x00,         // mov eax, [rcx+rax*4+0x1020]
        0x48, 0x01, 0xC8,                                 // add rax, rcx
        0xFF, 0xE0,                                       // jmp rax
        0xC3,                                             // ret
    };
    std::memcpy(code.data(), head, sizeof head);
    const std::uint32_t entries[4] = {0x1100, 0x310F, 0x1110, 0x1120};
    std::memcpy(code.data() + 0x20, entries, sizeof entries);
    const u8 real[] = {0x0F, 0x31, 0xC3};                 // a genuine rdtsc, after the table
    std::memcpy(code.data() + 0x80, real, sizeof real);
    pe.secs.push_back({".text", 0x1000, 0x4000, code, kRX});
    Bytes f = pe.Build();
    MappedImage img;
    CHECK(MapImageFromMemory(f.data(), f.size(), kDefaultMapFlags, nullptr, &img) >= 0);
    if (!img.base) return;
    TestCtx ctx;
    ScanOptions opt;
    opt.handler = TestHandler;
    opt.handler_ctx = &ctx;
    ScanResult res;
    CHECK_EQ(ScanAndPatch(img, opt, &res), kStatusSuccess);
    CHECK_EQ(res.sites.size(), std::size_t(1));
    if (res.sites.size() == 1) CHECK(res.sites[0].address == static_cast<u8*>(img.base) + 0x1080);
    CHECK(std::memcmp(static_cast<u8*>(img.base) + 0x1020, entries, sizeof entries) == 0);
    UnmapImage(img);
}

// A big real image, when the Wine PE build is present (not in CI): stub
// rewriting must find the Wine-form thunks and the scanner must neither crash
// nor touch them.
TEST(loader_wine_ntdll_if_present) {
    std::string src = WIN10XBOX_SOURCE_DIR;
    std::string p = src + "/wine-pe/out/builtin/ntdll.dll";
    wchar_t wp[1024];
    MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, wp, 1024);
    if (GetFileAttributesW(wp) == INVALID_FILE_ATTRIBUTES) {
        std::printf("  (skipped: %s not built)\n", p.c_str());
        return;
    }
    MappedImage img;
    NTSTATUS st = MapImageFromFile(wp, MapFlags::ReserveOnly, nullptr, &img);   // no unwind registration: keep the host clean
    CHECK(st >= 0);
    if (st < 0) return;
    std::vector<StubPatch> patched;
    std::vector<std::string> unmatched;
    void* target = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x00007FF000001000ull));
    std::size_t n = RewriteSyscallStubs(img, target, &patched, &unmatched);
    std::printf("  ntdll: %zu stubs rewritten, %zu unmatched\n", n, unmatched.size());
    CHECK(n >= 150);
    CHECK_EQ(n, patched.size());
    for (auto& u : unmatched) std::printf("    unmatched: %s\n", u.c_str());
    TestCtx ctx;
    ScanOptions opt;
    opt.handler = TestHandler;
    opt.handler_ctx = &ctx;
    opt.patch_kuser_reads = true;
    ScanResult res;
    CHECK_EQ(FinalizeProtections(img), kStatusSuccess);
    CHECK_EQ(ScanAndPatch(img, opt, &res), kStatusSuccess);
    std::printf("  ntdll scan: %zu inline, %zu trapped, %zu skipped\n", res.patched, res.trapped, res.skipped);
    for (auto& s : res.sites)
        for (auto& pt : patched)
            CHECK(!(static_cast<u8*>(s.address) >= static_cast<u8*>(pt.address) && static_cast<u8*>(s.address) < static_cast<u8*>(pt.address) + 32));
    // The rewritten thunks are still intact after the scan.
    static const u8 jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
    for (auto& pt : patched) CHECK(std::memcmp(static_cast<u8*>(pt.address) + 8, jmp, 6) == 0);
    UnmapImage(img);
}

// Robustness sweep over every Wine PE builtin that is present: map, scan with
// every site kind enabled, unmap. Looks for crashes and for sites that fall
// outside executable sections; prints the totals.
TEST(loader_wine_builtins_scan_sweep_if_present) {
    std::wstring dir;
    {
        std::string src = std::string(WIN10XBOX_SOURCE_DIR) + "/wine-pe/out/builtin/*.dll";
        wchar_t wp[1024];
        MultiByteToWideChar(CP_UTF8, 0, src.c_str(), -1, wp, 1024);
        dir = wp;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(dir.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("  (skipped: no Wine PE builtins)\n");
        return;
    }
    std::wstring base_dir = dir.substr(0, dir.size() - 5);
    std::size_t files = 0, inl = 0, trap = 0, skip = 0, kuser = 0;
    do {
        if (files >= 60) break;
        MappedImage img;
        std::wstring full = base_dir + fd.cFileName;
        NTSTATUS st = MapImageFromFile(full.c_str(), MapFlags::ReserveOnly, nullptr, &img);
        if (st < 0) {
            if (st != kStatusInvalidImageFormat && st != kStatusInvalidImageWin32)
                std::printf("  map %ls failed 0x%08lx\n", fd.cFileName, static_cast<unsigned long>(st));
            continue;
        }
        ++files;
        TestCtx ctx;
        ScanOptions opt;
        opt.handler = TestHandler;
        opt.handler_ctx = &ctx;
        opt.patch_kuser_reads = true;
        ScanResult res;
        CHECK_EQ(FinalizeProtections(img), kStatusSuccess);
        CHECK_EQ(ScanAndPatch(img, opt, &res), kStatusSuccess);
        inl += res.patched;
        trap += res.trapped;
        skip += res.skipped;
        for (auto& s : res.sites) {
            if (s.kind == SiteKind::KuserRead) ++kuser;
            CHECK(s.address >= img.base && s.address < static_cast<u8*>(img.base) + img.size);
        }
        UnmapImage(img);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::printf("  %zu builtin DLLs scanned: %zu inline, %zu trapped, %zu skipped (%zu kuser reads)\n", files, inl, trap, skip, kuser);
}
