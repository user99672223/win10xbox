/*
 * win10xbox loader: manual PE image mapper (x64 PE32+ only) and export lookup.
 *
 * Derived from MemoryModule.c (third_party/MemoryModule/MemoryModule.c):
 *
 *   Memory DLL loading code, Version 0.0.4
 *   Copyright (c) 2004-2015 by Joachim Bauch / mail@joachim-bauch.de
 *   http://www.joachim-bauch.de
 *
 *   The contents of this file are subject to the Mozilla Public License
 *   Version 2.0 (the "License"); you may not use this file except in
 *   compliance with the License. You may obtain a copy of the License at
 *   http://www.mozilla.org/MPL/
 *
 *   Software distributed under the License is distributed on an "AS IS"
 *   basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 *   License for the specific language governing rights and limitations under
 *   the License.
 *
 *   The Original Code is MemoryModule.c. The Initial Developer of the Original
 *   Code is Joachim Bauch. Portions created by Joachim Bauch are Copyright
 *   (C) 2004-2015 Joachim Bauch. All Rights Reserved.
 *   THeller: binary search in MemoryGetProcAddress, Copyright (C) 2013 Thomas
 *   Heller.
 *
 * What was kept from MemoryModule: the overall flow (validate headers, reserve
 * SizeOfImage, copy headers + sections, apply base relocations, finalise
 * per-section protections) and the export table walk. What differs: no import
 * resolution, no TLS and no DllMain (Wine's loader.c does those, see
 * docs/LOADER.md); strict bounds checking of every header field; page-union
 * protections for overlapping sections; headers kept readable; .pdata is
 * registered with the host ntdll; the export walk handles forwarders, unsorted
 * name tables and out-of-range RVAs. Modifications Copyright (C) the
 * win10xbox authors, distributed under the same MPL-2.0 terms.
 */
#include "internal.h"

#include <algorithm>
#include <cstring>
#include <numeric>

#include "win10xbox/trace.h"

namespace win10xbox::loader {

using detail::kPageSize;
using detail::RoundUp;

namespace {

// ---------------------------------------------------------------------------
// Header parsing of the *file* image
// ---------------------------------------------------------------------------
struct ParsedImage {
    IMAGE_FILE_HEADER         fh{};
    IMAGE_OPTIONAL_HEADER64   oh{};
    std::vector<IMAGE_SECTION_HEADER> secs;
    std::uint32_t             nt_offset = 0;
    std::uint32_t             headers_len = 0;   // bytes of file to copy to offset 0 of the image
    std::uint32_t             alloc_size = 0;    // SizeOfImage rounded to the page size

    IMAGE_DATA_DIRECTORY Dir(unsigned idx) const {
        if (idx >= oh.NumberOfRvaAndSizes || idx >= IMAGE_NUMBEROF_DIRECTORY_ENTRIES) return {0, 0};
        return oh.DataDirectory[idx];
    }
};

constexpr std::uint32_t kMaxImageSize = 0x70000000u;
constexpr unsigned      kMaxSections  = 96;
constexpr std::size_t   kOptHdrMin    = offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);

NTSTATUS ParseImage(const void* file, std::size_t size, ParsedImage* p) {
    const auto* f = static_cast<const std::uint8_t*>(file);
    if (size < sizeof(IMAGE_DOS_HEADER)) return kStatusInvalidImageFormat;
    IMAGE_DOS_HEADER dos;
    std::memcpy(&dos, f, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return kStatusInvalidImageFormat;
    if (dos.e_lfanew < 0) return kStatusInvalidImageFormat;
    std::uint64_t nt_off = static_cast<std::uint32_t>(dos.e_lfanew);
    // signature(4) + file header(20) + optional header magic(2)
    if (nt_off + 4 + sizeof(IMAGE_FILE_HEADER) + 2 > size) return kStatusInvalidImageFormat;
    std::uint32_t sig;
    std::memcpy(&sig, f + nt_off, 4);
    if (sig != IMAGE_NT_SIGNATURE) return kStatusInvalidImageFormat;
    std::memcpy(&p->fh, f + nt_off + 4, sizeof(IMAGE_FILE_HEADER));

    std::uint64_t opt_off = nt_off + 4 + sizeof(IMAGE_FILE_HEADER);
    std::uint16_t magic;
    std::memcpy(&magic, f + opt_off, 2);
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) return kStatusInvalidImageWin32;
    if (magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return kStatusInvalidImageFormat;
    if (p->fh.Machine != IMAGE_FILE_MACHINE_AMD64) return kStatusInvalidImageFormat;

    std::uint32_t opt_size = p->fh.SizeOfOptionalHeader;
    if (opt_size < kOptHdrMin || opt_off + opt_size > size) return kStatusInvalidImageFormat;
    std::memcpy(&p->oh, f + opt_off, std::min<std::size_t>(opt_size, sizeof(IMAGE_OPTIONAL_HEADER64)));
    std::uint32_t avail_dirs = static_cast<std::uint32_t>((opt_size - kOptHdrMin) / sizeof(IMAGE_DATA_DIRECTORY));
    p->oh.NumberOfRvaAndSizes = std::min({p->oh.NumberOfRvaAndSizes, avail_dirs,
                                          static_cast<std::uint32_t>(IMAGE_NUMBEROF_DIRECTORY_ENTRIES)});

    unsigned nsec = p->fh.NumberOfSections;
    if (nsec > kMaxSections) return kStatusInvalidImageFormat;
    std::uint64_t sec_off = opt_off + opt_size;
    std::uint64_t sec_end = sec_off + std::uint64_t(nsec) * sizeof(IMAGE_SECTION_HEADER);
    if (sec_end > size) return kStatusInvalidImageFormat;
    p->secs.resize(nsec);
    if (nsec) std::memcpy(p->secs.data(), f + sec_off, nsec * sizeof(IMAGE_SECTION_HEADER));

    // Alignment rules (same as the NT memory manager).
    std::uint32_t sa = p->oh.SectionAlignment, fa = p->oh.FileAlignment;
    if (sa == 0 || (sa & (sa - 1)) != 0) return kStatusInvalidImageFormat;
    if (sa < kPageSize && sa != fa) return kStatusInvalidImageFormat;

    std::uint32_t soi = p->oh.SizeOfImage, soh = p->oh.SizeOfHeaders;
    if (soi == 0 || soi > kMaxImageSize) return kStatusInvalidImageFormat;
    if (soh == 0 || soh > soi || soh > size) return kStatusInvalidImageFormat;
    p->nt_offset = static_cast<std::uint32_t>(nt_off);
    // The section table must be readable from the mapped copy (FinalizeProtections and
    // the scanner re-parse it), so it counts as header even if SizeOfHeaders is too small.
    std::uint64_t hl = std::max<std::uint64_t>(soh, sec_end);
    p->alloc_size = static_cast<std::uint32_t>(RoundUp(soi, kPageSize));
    if (hl > p->alloc_size || hl > size) return kStatusInvalidImageFormat;
    p->headers_len = static_cast<std::uint32_t>(hl);

    for (auto& s : p->secs) {
        std::uint64_t vsz = detail::SectionSize(s);
        if (std::uint64_t(s.VirtualAddress) + vsz > p->alloc_size) return kStatusInvalidImageFormat;
        std::uint64_t copy = std::min<std::uint64_t>(s.SizeOfRawData, vsz);
        if (copy && std::uint64_t(s.PointerToRawData) + copy > size) return kStatusInvalidImageFormat;
    }
    return kStatusSuccess;
}

// ---------------------------------------------------------------------------
// Base relocations
// ---------------------------------------------------------------------------
NTSTATUS ApplyRelocations(std::uint8_t* base, const ParsedImage& p, std::uint64_t delta) {
    IMAGE_DATA_DIRECTORY dir = p.Dir(IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (dir.Size == 0 || dir.VirtualAddress == 0) return kStatusSuccess;  // nothing to fix up
    if (std::uint64_t(dir.VirtualAddress) + dir.Size > p.alloc_size) return kStatusInvalidImageFormat;

    std::uint32_t off = 0;
    while (dir.Size - off >= sizeof(IMAGE_BASE_RELOCATION)) {
        IMAGE_BASE_RELOCATION blk;
        std::memcpy(&blk, base + dir.VirtualAddress + off, sizeof(blk));
        if (blk.SizeOfBlock == 0) break;                       // padding / terminator
        if (blk.SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) return kStatusInvalidImageFormat;
        // An odd-sized or truncated block contributes only its complete 16-bit entries;
        // the next block starts exactly SizeOfBlock bytes later (as ntdll does).
        std::uint32_t avail = std::min<std::uint32_t>(blk.SizeOfBlock, dir.Size - off);
        std::uint32_t nent = (avail - static_cast<std::uint32_t>(sizeof(IMAGE_BASE_RELOCATION))) / 2;
        const std::uint8_t* ents = base + dir.VirtualAddress + off + sizeof(IMAGE_BASE_RELOCATION);
        for (std::uint32_t i = 0; i < nent; ++i) {
            std::uint16_t e;
            std::memcpy(&e, ents + i * 2, 2);
            unsigned type = e >> 12;
            std::uint64_t rva = std::uint64_t(blk.VirtualAddress) + (e & 0xFFFu);
            switch (type) {
            case IMAGE_REL_BASED_ABSOLUTE:
                break;
            case IMAGE_REL_BASED_HIGHLOW: {
                if (rva + 4 > p.alloc_size) return kStatusInvalidImageFormat;
                std::uint32_t v;
                std::memcpy(&v, base + rva, 4);
                v += static_cast<std::uint32_t>(delta);
                std::memcpy(base + rva, &v, 4);
                break;
            }
            case IMAGE_REL_BASED_DIR64: {
                if (rva + 8 > p.alloc_size) return kStatusInvalidImageFormat;
                std::uint64_t v;
                std::memcpy(&v, base + rva, 8);
                v += delta;
                std::memcpy(base + rva, &v, 8);
                break;
            }
            default:
                WARN(loader, "unsupported relocation type %u at rva 0x%llx", type,
                     static_cast<unsigned long long>(rva));
                return kStatusInvalidImageFormat;
            }
        }
        std::uint64_t next = std::uint64_t(off) + blk.SizeOfBlock;
        if (next > dir.Size) break;
        off = static_cast<std::uint32_t>(next);
    }
    return kStatusSuccess;
}

// ---------------------------------------------------------------------------
// Protections
// ---------------------------------------------------------------------------
DWORD ProtectionFor(unsigned rwx) {
    static const DWORD table[8] = {
        PAGE_NOACCESS,          // ---
        PAGE_READONLY,          // R--
        PAGE_READWRITE,         // -W-
        PAGE_READWRITE,         // RW-
        PAGE_EXECUTE,           // --X
        PAGE_EXECUTE_READ,      // R-X
        PAGE_EXECUTE_READWRITE, // -WX
        PAGE_EXECUTE_READWRITE, // RWX
    };
    return table[rwx & 7];
}

unsigned FlagsFor(DWORD ch) {
    unsigned f = 0;
    if (ch & IMAGE_SCN_MEM_READ) f |= 1;
    if (ch & IMAGE_SCN_MEM_WRITE) f |= 2;
    if (ch & IMAGE_SCN_MEM_EXECUTE) f |= 4;
    return f;
}

// Per-page protection = union of the permissions of every section that touches
// the page (headers count as read-only); pages no section covers become
// PAGE_NOACCESS. This is what a SectionAlignment < page size image needs and
// also resolves overlapping sections deterministically.
NTSTATUS ApplyFinalProtections(std::uint8_t* base) {
    const IMAGE_NT_HEADERS64* nt = nullptr;
    const IMAGE_SECTION_HEADER* secs = nullptr;
    unsigned nsec = 0;
    if (!detail::GetNtHeaders(base, &nt) || !detail::GetSections(base, &secs, &nsec))
        return kStatusInvalidImageFormat;
    std::size_t size = detail::RoundUp(nt->OptionalHeader.SizeOfImage, kPageSize);
    std::size_t npages = size / kPageSize;
    constexpr std::uint8_t kCovered = 0x80;
    std::vector<std::uint8_t> flags(npages, 0);

    std::size_t hdr_end = reinterpret_cast<const std::uint8_t*>(secs + nsec) - base;
    hdr_end = std::max<std::size_t>(hdr_end, nt->OptionalHeader.SizeOfHeaders);
    for (std::size_t pg = 0; pg < std::min(npages, RoundUp(hdr_end, kPageSize) / kPageSize); ++pg)
        flags[pg] |= static_cast<std::uint8_t>(kCovered | 1);
    for (unsigned i = 0; i < nsec; ++i) {
        std::uint64_t vsz = detail::SectionSize(secs[i]);
        if (!vsz) continue;
        std::uint64_t b = secs[i].VirtualAddress / kPageSize;
        std::uint64_t e = RoundUp(secs[i].VirtualAddress + vsz, kPageSize) / kPageSize;
        for (std::uint64_t pg = b; pg < e && pg < npages; ++pg)
            flags[pg] |= static_cast<std::uint8_t>(kCovered | FlagsFor(secs[i].Characteristics));
    }

    bool any_exec = false;
    std::size_t pg = 0;
    while (pg < npages) {
        unsigned rwx = flags[pg] & 7;
        std::size_t run = pg + 1;
        while (run < npages && (flags[run] & 7) == rwx && (flags[run] & kCovered) == (flags[pg] & kCovered)) ++run;
        DWORD prot = (flags[pg] & kCovered) ? ProtectionFor(rwx) : PAGE_NOACCESS;
        if (rwx & 4) any_exec = true;
        DWORD old = 0;
        if (!VirtualProtect(base + pg * kPageSize, (run - pg) * kPageSize, prot, &old)) {
            ERR(loader, "VirtualProtect(%p, 0x%zx, 0x%lx) failed: %lu", base + pg * kPageSize,
                (run - pg) * kPageSize, static_cast<unsigned long>(prot), GetLastError());
            return kStatusUnsuccessful;
        }
        pg = run;
    }
    if (any_exec) FlushInstructionCache(GetCurrentProcess(), base, size);
    return kStatusSuccess;
}

std::uint8_t* AllocateImage(std::uint64_t want, std::size_t size, bool* got_want) {
    *got_want = false;
    if (want && (want & 0xFFFF) == 0) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(static_cast<std::uintptr_t>(want)), size,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) {
            *got_want = true;
            return static_cast<std::uint8_t*>(p);
        }
    }
    return static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

using RtlAddFunctionTableFn    = BOOLEAN(NTAPI*)(PRUNTIME_FUNCTION, DWORD, DWORD64);
using RtlDeleteFunctionTableFn = BOOLEAN(NTAPI*)(PRUNTIME_FUNCTION);

}  // namespace

// ---------------------------------------------------------------------------
// Public mapper API
// ---------------------------------------------------------------------------
NTSTATUS MapImageFromMemory(const void* file, std::size_t file_size, MapFlags flags, void* preferred,
                            MappedImage* out) {
    if (!out || !file) return kStatusInvalidParameter;
    *out = MappedImage{};
    ParsedImage p;
    NTSTATUS st = ParseImage(file, file_size, &p);
    if (st < 0) {
        TRACE(loader, "rejecting image: status 0x%08lx", static_cast<unsigned long>(st));
        return st;
    }
    const auto* f = static_cast<const std::uint8_t*>(file);
    const std::uint64_t header_base = p.oh.ImageBase;
    const bool relocs_stripped = (p.fh.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0;
    const bool no_reloc = (flags & MapFlags::NoRelocate) || relocs_stripped;

    std::uint64_t want = preferred ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(preferred))
                                   : header_base;
    // An image that must not be relocated can only live at its own ImageBase.
    if (no_reloc && (want != header_base || want == 0)) {
        TRACE(loader, "%s image cannot be placed at 0x%llx", relocs_stripped ? "relocs-stripped" : "no-relocate",
              static_cast<unsigned long long>(want));
        return kStatusConflictingAddresses;
    }
    bool got_want = false;
    std::uint8_t* base = AllocateImage(want, p.alloc_size, &got_want);
    if (!base) return kStatusNoMemory;
    if (no_reloc && reinterpret_cast<std::uintptr_t>(base) != header_base) {
        VirtualFree(base, 0, MEM_RELEASE);
        return kStatusConflictingAddresses;
    }

    auto fail = [&](NTSTATUS s) {
        VirtualFree(base, 0, MEM_RELEASE);
        return s;
    };

    // Headers, then sections in ascending RVA order so that overlaps resolve
    // the same way every time (the higher section wins).
    std::memcpy(base, f, p.headers_len);
    std::vector<unsigned> order(p.secs.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(),
                     [&](unsigned a, unsigned b) { return p.secs[a].VirtualAddress < p.secs[b].VirtualAddress; });
    for (unsigned i : order) {
        const IMAGE_SECTION_HEADER& s = p.secs[i];
        std::uint64_t copy = std::min<std::uint64_t>(s.SizeOfRawData, detail::SectionSize(s));
        // Memory from VirtualAlloc is already zero, which covers VirtualSize > SizeOfRawData.
        if (copy) std::memcpy(base + s.VirtualAddress, f + s.PointerToRawData, static_cast<std::size_t>(copy));
    }

    const std::uint64_t actual = reinterpret_cast<std::uintptr_t>(base);
    const std::uint64_t delta = actual - header_base;
    if (delta != 0) {
        st = ApplyRelocations(base, p, delta);
        if (st < 0) return fail(st);
    }
    // Like ntdll's LdrRelocateImage: the mapped header advertises the real base.
    std::memcpy(base + p.nt_offset + 4 + sizeof(IMAGE_FILE_HEADER) + offsetof(IMAGE_OPTIONAL_HEADER64, ImageBase),
                &actual, sizeof(actual));

    MappedImage img;
    img.base = base;
    img.size = p.alloc_size;
    img.relocated = delta != 0;
    img.is_dll = (p.fh.Characteristics & IMAGE_FILE_DLL) != 0;
    img.characteristics = p.fh.Characteristics;
    img.dll_characteristics = p.oh.DllCharacteristics;
    img.image_base_preferred = header_base;
    if (p.oh.AddressOfEntryPoint) {
        if (p.oh.AddressOfEntryPoint >= p.alloc_size) return fail(kStatusInvalidImageFormat);
        img.entry = base + p.oh.AddressOfEntryPoint;
    }

    if (flags & MapFlags::RegisterUnwind) {
        IMAGE_DATA_DIRECTORY ex = p.Dir(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
        std::uint32_t count = ex.Size / static_cast<std::uint32_t>(sizeof(RUNTIME_FUNCTION));
        if (count && ex.VirtualAddress && (ex.VirtualAddress & 3) == 0 &&
            std::uint64_t(ex.VirtualAddress) + std::uint64_t(count) * sizeof(RUNTIME_FUNCTION) <= p.alloc_size) {
            auto add = reinterpret_cast<RtlAddFunctionTableFn>(detail::HostNtdllProc("RtlAddFunctionTable"));
            auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(base + ex.VirtualAddress);
            if (add && add(table, count, actual)) {
                img.function_table = table;
                img.function_count = count;
            } else {
                WARN(loader, "RtlAddFunctionTable failed for %p (%u entries)", base, count);
            }
        }
    }

    if (!(flags & MapFlags::ReserveOnly)) {
        st = ApplyFinalProtections(base);
        if (st < 0) {
            MappedImage tmp = img;
            UnmapImage(tmp);
            return st;
        }
    }
    TRACE(loader, "mapped image at %p size 0x%zx (preferred 0x%llx)%s", base, img.size,
          static_cast<unsigned long long>(header_base), img.relocated ? " RELOCATED" : "");
    *out = img;
    return img.relocated ? kStatusImageNotAtBase : kStatusSuccess;
}

NTSTATUS MapImageFromFile(const wchar_t* host_path, MapFlags flags, void* preferred, MappedImage* out) {
    if (!host_path || !out) return kStatusInvalidParameter;
    HANDLE h = CreateFileW(host_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        TRACE(loader, "cannot open %ls: %lu", host_path, GetLastError());
        return static_cast<NTSTATUS>(0xC0000034);  // STATUS_OBJECT_NAME_NOT_FOUND
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 0x7FFFFFFF) {
        CloseHandle(h);
        return kStatusInvalidImageFormat;
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz.QuadPart));
    std::size_t done = 0;
    while (done < buf.size()) {
        DWORD got = 0;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(buf.size() - done, 0x10000000));
        if (!ReadFile(h, buf.data() + done, want, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(h);
    if (done != buf.size()) return kStatusInvalidImageFormat;
    NTSTATUS st = MapImageFromMemory(buf.data(), buf.size(), flags, preferred, out);
    if (st >= 0) out->path = host_path;
    return st;
}

NTSTATUS FinalizeProtections(const MappedImage& img) {
    if (!img.base) return kStatusInvalidParameter;
    return ApplyFinalProtections(static_cast<std::uint8_t*>(img.base));
}

void UnmapImage(MappedImage& img) {
    if (img.base) {
        if (img.function_table) {
            auto del = reinterpret_cast<RtlDeleteFunctionTableFn>(detail::HostNtdllProc("RtlDeleteFunctionTable"));
            if (del) del(img.function_table);
        }
        ReleaseScanSites(img.base, img.size);
        VirtualFree(img.base, 0, MEM_RELEASE);
    }
    img = MappedImage{};
}

// ---------------------------------------------------------------------------
// Export lookup
// ---------------------------------------------------------------------------
namespace {

struct ExportView {
    const std::uint8_t*  base = nullptr;
    std::uint32_t        image_size = 0;
    std::uint32_t        dir_rva = 0, dir_size = 0;
    std::uint32_t        ordinal_base = 0;
    std::uint32_t        nfuncs = 0, nnames = 0;
    const std::uint32_t* funcs = nullptr;
    const std::uint32_t* names = nullptr;
    const std::uint16_t* ords = nullptr;
};

bool RangeOk(std::uint64_t rva, std::uint64_t len, std::uint32_t image_size) { return rva + len <= image_size; }

bool GetExportView(const void* image_base, ExportView* v) {
    const IMAGE_NT_HEADERS64* nt = nullptr;
    if (!detail::GetNtHeaders(image_base, &nt)) return false;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    v->base = static_cast<const std::uint8_t*>(image_base);
    v->image_size = nt->OptionalHeader.SizeOfImage;
    if (!d.VirtualAddress || d.Size < sizeof(IMAGE_EXPORT_DIRECTORY) || !RangeOk(d.VirtualAddress, d.Size, v->image_size))
        return false;
    IMAGE_EXPORT_DIRECTORY e;
    std::memcpy(&e, v->base + d.VirtualAddress, sizeof(e));
    v->dir_rva = d.VirtualAddress;
    v->dir_size = d.Size;
    v->ordinal_base = e.Base;
    v->nfuncs = e.NumberOfFunctions;
    v->nnames = e.NumberOfNames;
    if (v->nfuncs > v->image_size / 4 || v->nnames > v->image_size / 4) return false;
    if (v->nfuncs && !RangeOk(e.AddressOfFunctions, std::uint64_t(v->nfuncs) * 4, v->image_size)) return false;
    if (v->nnames) {
        if (!RangeOk(e.AddressOfNames, std::uint64_t(v->nnames) * 4, v->image_size)) return false;
        if (!RangeOk(e.AddressOfNameOrdinals, std::uint64_t(v->nnames) * 2, v->image_size)) return false;
    }
    v->funcs = reinterpret_cast<const std::uint32_t*>(v->base + e.AddressOfFunctions);
    v->names = reinterpret_cast<const std::uint32_t*>(v->base + e.AddressOfNames);
    v->ords = reinterpret_cast<const std::uint16_t*>(v->base + e.AddressOfNameOrdinals);
    return true;
}

// NUL-terminated string inside the image; returns nullptr if it runs off the end.
const char* StringAt(const ExportView& v, std::uint32_t rva, std::size_t* len, std::uint32_t limit = 0) {
    std::uint32_t lim = limit ? limit : v.image_size;
    if (rva >= lim) return nullptr;
    const char* s = reinterpret_cast<const char*>(v.base + rva);
    const void* nul = std::memchr(s, 0, lim - rva);
    if (!nul) return nullptr;
    *len = static_cast<std::size_t>(static_cast<const char*>(nul) - s);
    return s;
}

// Resolve slot `idx` of the function table.
void* ResolveSlot(const ExportView& v, std::uint32_t idx, std::string* forwarder) {
    if (idx >= v.nfuncs) return nullptr;
    std::uint32_t rva = v.funcs[idx];
    if (rva == 0) return nullptr;                       // hole in a sparse ordinal range
    if (rva >= v.dir_rva && rva < v.dir_rva + v.dir_size) {   // forwarder string
        std::size_t len = 0;
        const char* s = StringAt(v, rva, &len, std::min<std::uint64_t>(v.image_size, std::uint64_t(v.dir_rva) + v.dir_size));
        if (s && forwarder) forwarder->assign(s, len);
        return nullptr;
    }
    if (rva >= v.image_size) return nullptr;            // out of range RVA
    return const_cast<std::uint8_t*>(v.base) + rva;
}

}  // namespace

namespace detail {
bool EnumNamedExports(const void* base, std::vector<NamedExport>* out) {
    ExportView v;
    if (!GetExportView(base, &v)) return false;
    out->clear();
    out->reserve(v.nnames);
    for (std::uint32_t i = 0; i < v.nnames; ++i) {
        std::size_t len = 0;
        const char* s = StringAt(v, v.names[i], &len);
        if (!s) continue;
        std::uint32_t idx = v.ords[i];
        if (idx >= v.nfuncs) continue;
        NamedExport ne;
        ne.name.assign(s, len);
        ne.ordinal = static_cast<std::uint16_t>(idx + v.ordinal_base);
        ne.rva = v.funcs[idx];
        ne.forwarder = ne.rva >= v.dir_rva && ne.rva < v.dir_rva + v.dir_size;
        out->push_back(std::move(ne));
    }
    return true;
}
}  // namespace detail

void* FindExport(const void* image_base, const char* name, std::string* forwarder) {
    if (forwarder) forwarder->clear();
    if (!name) return nullptr;
    ExportView v;
    if (!GetExportView(image_base, &v)) return nullptr;
    std::size_t nlen = std::strlen(name);
    auto cmp = [&](std::uint32_t i) -> int {   // <0: table name < wanted, 0 equal, >0 greater
        std::size_t len = 0;
        const char* s = StringAt(v, v.names[i], &len);
        if (!s) return 2;                      // unreadable: treat as "greater"; the linear pass will skip it
        int c = std::memcmp(s, name, std::min(len, nlen));
        if (c) return c;
        return len == nlen ? 0 : (len < nlen ? -1 : 1);
    };
    // Fast path: binary search, valid only if the name table is sorted (it
    // normally is). A miss falls through to a linear scan, so unsorted tables
    // are still found.
    std::uint32_t lo = 0, hi = v.nnames;
    while (lo < hi) {
        std::uint32_t mid = lo + (hi - lo) / 2;
        int c = cmp(mid);
        if (c == 0) return ResolveSlot(v, v.ords[mid], forwarder);
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    for (std::uint32_t i = 0; i < v.nnames; ++i)
        if (cmp(i) == 0) return ResolveSlot(v, v.ords[i], forwarder);
    return nullptr;
}

void* FindExportByOrdinal(const void* image_base, std::uint16_t ordinal, std::string* forwarder) {
    if (forwarder) forwarder->clear();
    ExportView v;
    if (!GetExportView(image_base, &v)) return nullptr;
    if (ordinal < v.ordinal_base) return nullptr;
    return ResolveSlot(v, ordinal - v.ordinal_base, forwarder);
}

}  // namespace win10xbox::loader
