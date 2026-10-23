// Image sections (E3): NtCreateSection(SEC_IMAGE) on a file returns an
// emulated handle holding the file bytes; NtMapViewOfSection maps it with
// core/loader (stub rewriting + cpuid/rdtsc scanning happen while the pages
// are still writable). Non-image sections pass through to the host.
#include "internal.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_CLOCK
#include "win10xbox/clock.h"
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>

WIN10XBOX_DECLARE_CHANNEL(modules);

namespace win10xbox::dispatch::detail {
namespace {

constexpr ULONG kSecImage = 0x1000000;
constexpr ULONG kSecFile = 0x800000;
constexpr std::size_t kMaxImageFile = 1ull << 30;
constexpr DWORD kComImage32BitRequired = 0x2, kComImage32BitPreferred = 0x20000, kComImageIlOnly = 0x1;

SRWLOCK g_view_lock = SRWLOCK_INIT;
std::vector<std::unique_ptr<ImageView>>& Views() {
    NoteHostStateUnderGuestView("dispatch::Views");
    static std::vector<std::unique_ptr<ImageView>> v;
    return v;
}
std::shared_ptr<ImageSection> g_main_image;
volatile LONG64 g_section_ids = 0;

template <class T> bool ReadAt(const std::uint8_t* file, std::size_t size, std::size_t off, T* out) {
    if (off > size || size - off < sizeof(T)) return false;
    std::memcpy(out, file + off, sizeof(T));
    return true;
}

// RVA -> file offset through the section table (headers map 1:1).
bool RvaToOffset(const std::uint8_t* file, std::size_t size, std::size_t sections_off, unsigned nsec,
                 std::uint32_t rva, std::uint32_t size_of_headers, std::size_t* out) {
    if (rva < size_of_headers) { *out = rva; return rva < size; }
    for (unsigned i = 0; i < nsec; ++i) {
        IMAGE_SECTION_HEADER sh;
        if (!ReadAt(file, size, sections_off + i * sizeof(sh), &sh)) return false;
        std::uint32_t vsize = sh.Misc.VirtualSize ? sh.Misc.VirtualSize : sh.SizeOfRawData;
        if (rva >= sh.VirtualAddress && rva < sh.VirtualAddress + vsize) {
            std::uint32_t delta = rva - sh.VirtualAddress;
            if (delta >= sh.SizeOfRawData) return false;
            *out = std::size_t(sh.PointerToRawData) + delta;
            return *out < size;
        }
    }
    return false;
}

// DLL name from the export directory of a raw PE32+ file ("kernel32.dll"), empty
// when there is none (EXEs) or the file is not a well-formed x64 image. Used to
// label sections whose host path is unknown.
std::wstring ExportNameFromFile(const std::uint8_t* file, std::size_t size) {
    IMAGE_DOS_HEADER dos;
    if (!ReadAt(file, size, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return {};
    const std::size_t nt_off = static_cast<std::size_t>(dos.e_lfanew);
    DWORD sig;
    IMAGE_FILE_HEADER fh;
    IMAGE_OPTIONAL_HEADER64 oh;
    if (!ReadAt(file, size, nt_off, &sig) || sig != IMAGE_NT_SIGNATURE || !ReadAt(file, size, nt_off + 4, &fh)) return {};
    const std::size_t opt_off = nt_off + 4 + sizeof(fh);
    if (!ReadAt(file, size, opt_off, &oh) || oh.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return {};
    if (oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return {};
    const IMAGE_DATA_DIRECTORY dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return {};
    const std::size_t sec_off = opt_off + fh.SizeOfOptionalHeader;
    std::size_t off = 0;
    IMAGE_EXPORT_DIRECTORY ed;
    if (!RvaToOffset(file, size, sec_off, fh.NumberOfSections, dir.VirtualAddress, oh.SizeOfHeaders, &off) ||
        !ReadAt(file, size, off, &ed) || !ed.Name)
        return {};
    if (!RvaToOffset(file, size, sec_off, fh.NumberOfSections, ed.Name, oh.SizeOfHeaders, &off)) return {};
    std::wstring name;
    for (std::size_t i = 0; i < 64 && off + i < size && file[off + i]; ++i) name.push_back(static_cast<wchar_t>(file[off + i]));
    return name;
}

bool IEqualsW(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}

void CloseSectionObject(const EmulatedObject& o) {
    delete static_cast<std::shared_ptr<ImageSection>*>(o.extra);
}

std::shared_ptr<ImageSection> SectionFromHandle(HANDLE h) {
    EmulatedObject o;
    if (!LookupEmulatedObject(h, &o) || o.kind != ObjectKind::ImageSection || !o.extra) return nullptr;
    return *static_cast<std::shared_ptr<ImageSection>*>(o.extra);
}

HANDLE NewSectionHandle(const std::shared_ptr<ImageSection>& sec) {
    auto* holder = new std::shared_ptr<ImageSection>(sec);
    HANDLE h = RegisterEmulatedObject(ObjectKind::ImageSection,
                                      static_cast<std::uint64_t>(InterlockedIncrement64(&g_section_ids)), holder);
    if (!h) delete holder;
    return h;
}

}  // namespace

NTSTATUS BuildImageInformation(const std::uint8_t* file, std::size_t size, std::uint8_t out[0x40],
                               std::uint32_t* size_of_image) {
    std::memset(out, 0, 0x40);
    IMAGE_DOS_HEADER dos;
    if (!ReadAt(file, size, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return st::InvalidImageNotMz;
    const std::size_t nt_off = static_cast<std::size_t>(dos.e_lfanew);
    DWORD sig;
    IMAGE_FILE_HEADER fh;
    WORD magic;
    if (dos.e_lfanew < 0 || !ReadAt(file, size, nt_off, &sig) || sig != IMAGE_NT_SIGNATURE ||
        !ReadAt(file, size, nt_off + 4, &fh) || !ReadAt(file, size, nt_off + 4 + sizeof(fh), &magic))
        return st::InvalidImageFormat;
    const std::size_t opt_off = nt_off + 4 + sizeof(fh);
    const std::size_t sec_off = opt_off + fh.SizeOfOptionalHeader;

    std::uint64_t image_base, stack_reserve, stack_commit;
    std::uint32_t entry, subsystem, sub_minor, sub_major, os_major, os_minor, dll_chars, checksum, loader_flags;
    std::uint32_t size_of_code, section_alignment, headers_size, image_size, ndirs;
    std::size_t dirs_off;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 o;
        if (!ReadAt(file, size, opt_off, &o)) return st::InvalidImageFormat;
        image_base = o.ImageBase; stack_reserve = o.SizeOfStackReserve; stack_commit = o.SizeOfStackCommit;
        entry = o.AddressOfEntryPoint; subsystem = o.Subsystem; sub_minor = o.MinorSubsystemVersion;
        sub_major = o.MajorSubsystemVersion; os_major = o.MajorOperatingSystemVersion;
        os_minor = o.MinorOperatingSystemVersion; dll_chars = o.DllCharacteristics; checksum = o.CheckSum;
        loader_flags = o.LoaderFlags; size_of_code = o.SizeOfCode; section_alignment = o.SectionAlignment;
        headers_size = o.SizeOfHeaders; image_size = o.SizeOfImage; ndirs = o.NumberOfRvaAndSizes;
        dirs_off = opt_off + offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 o;
        if (!ReadAt(file, size, opt_off, &o)) return st::InvalidImageFormat;
        image_base = o.ImageBase; stack_reserve = o.SizeOfStackReserve; stack_commit = o.SizeOfStackCommit;
        entry = o.AddressOfEntryPoint; subsystem = o.Subsystem; sub_minor = o.MinorSubsystemVersion;
        sub_major = o.MajorSubsystemVersion; os_major = o.MajorOperatingSystemVersion;
        os_minor = o.MinorOperatingSystemVersion; dll_chars = o.DllCharacteristics; checksum = o.CheckSum;
        loader_flags = o.LoaderFlags; size_of_code = o.SizeOfCode; section_alignment = o.SectionAlignment;
        headers_size = o.SizeOfHeaders; image_size = o.SizeOfImage; ndirs = o.NumberOfRvaAndSizes;
        dirs_off = opt_off + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    } else {
        return st::InvalidImageFormat;
    }
    (void)loader_flags;
    auto dir = [&](unsigned i, IMAGE_DATA_DIRECTORY* d) {
        return i < ndirs && ReadAt(file, size, dirs_off + i * sizeof(*d), d) && d->VirtualAddress && d->Size;
    };
    IMAGE_DATA_DIRECTORY clr{}, relocs{};
    const bool has_clr = dir(IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR, &clr);
    const bool has_relocs = dir(IMAGE_DIRECTORY_ENTRY_BASERELOC, &relocs) && !(fh.Characteristics & IMAGE_FILE_RELOCS_STRIPPED);

    bool contains_code = size_of_code || entry || (section_alignment & 0xfff);
    for (unsigned i = 0; i < fh.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh;
        if (!ReadAt(file, size, sec_off + i * sizeof(sh), &sh)) return st::InvalidImageFormat;
        if (sh.Characteristics & IMAGE_SCN_MEM_EXECUTE) contains_code = true;
    }
    std::uint8_t flags = 0;
    if (section_alignment & 0xfff) flags |= 1u << 3;   // ImageMappedFlat
    else if ((dll_chars & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) && (has_relocs || contains_code) && !has_clr)
        flags |= 1u << 2;                               // ImageDynamicallyRelocated
    if (has_clr) {
        std::size_t off;
        IMAGE_COR20_HEADER cor;
        if (RvaToOffset(file, size, sec_off, fh.NumberOfSections, clr.VirtualAddress, headers_size, &off) &&
            ReadAt(file, size, off, &cor) && (cor.Flags & kComImageIlOnly)) {
            flags |= 1u << 1;                           // ComPlusILOnly
            if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
                if (!(cor.Flags & kComImage32BitRequired)) flags |= 1u << 0;   // ComPlusNativeReady
                if (cor.Flags & kComImage32BitPreferred) flags |= 1u << 5;     // ComPlusPrefer32bit
            }
        }
    }

    auto put = [&](std::size_t off, auto v) { std::memcpy(out + off, &v, sizeof(v)); };
    put(0x00, static_cast<std::uint64_t>(image_base + entry));          // TransferAddress
    put(0x08, static_cast<std::uint32_t>(0));                           // ZeroBits
    put(0x10, static_cast<std::uint64_t>(stack_reserve));               // MaximumStackSize
    put(0x18, static_cast<std::uint64_t>(stack_commit));                // CommittedStackSize
    put(0x20, static_cast<std::uint32_t>(subsystem));                   // SubSystemType
    put(0x24, static_cast<std::uint16_t>(sub_minor));
    put(0x26, static_cast<std::uint16_t>(sub_major));
    put(0x28, static_cast<std::uint16_t>(os_major));
    put(0x2A, static_cast<std::uint16_t>(os_minor));
    put(0x2C, static_cast<std::uint16_t>(fh.Characteristics));          // ImageCharacteristics
    put(0x2E, static_cast<std::uint16_t>(dll_chars));                   // DllCharacteristics
    put(0x30, static_cast<std::uint16_t>(fh.Machine));                  // Machine
    put(0x32, static_cast<std::uint8_t>(contains_code ? 1 : 0));        // ImageContainsCode
    put(0x33, flags);                                                   // ImageFlags
    put(0x34, static_cast<std::uint32_t>(has_clr ? 1 : 0));             // LoaderFlags (Windows/Wine: COR image)
    put(0x38, static_cast<std::uint32_t>(size));                        // ImageFileSize
    put(0x3C, static_cast<std::uint32_t>(checksum));                    // CheckSum
    if (size_of_image) *size_of_image = image_size;
    return st::Success;
}

NTSTATUS CreateImageSectionFromFile(HANDLE file, std::shared_ptr<ImageSection>* out) {
    LARGE_INTEGER fsize{};
    if (!GetFileSizeEx(file, &fsize)) return st::InvalidHandle;
    if (fsize.QuadPart <= 0 || static_cast<std::uint64_t>(fsize.QuadPart) > kMaxImageFile) return st::InvalidImageFormat;
    auto sec = std::make_shared<ImageSection>();
    sec->bytes.resize(static_cast<std::size_t>(fsize.QuadPart));
    std::size_t done = 0;
    while (done < sec->bytes.size()) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(done);
        ov.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(done) >> 32);
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(sec->bytes.size() - done, 1u << 24)), got = 0;
        if (!ReadFile(file, sec->bytes.data() + done, chunk, &got, &ov) || !got) {
            WARN(dispatch, "image read failed at %zu: error %lu", done, GetLastError());
            return st::InvalidImageFormat;
        }
        done += got;
    }
    if (NTSTATUS s = BuildImageInformation(sec->bytes.data(), sec->bytes.size(), sec->info, &sec->size_of_image))
        return s;
    sec->host_path = HostPathOfHandle(file, &sec->host_path_error);
    if (!UntranslateHostWin32Path(sec->host_path, &sec->guest_path)) sec->guest_path.clear();
    sec->name = ExportNameFromFile(sec->bytes.data(), sec->bytes.size());
    if (sec->host_path.empty()) {
        // The console's AppContainer: GetFinalPathNameByHandleW is refused. Image
        // identity then rests on the file contents (SameImageFile), not on paths.
        static volatile LONG reported = 0;
        if (InterlockedIncrement(&reported) <= 3)
            WARN(modules, "host path of image file %ls unavailable (Win32 error %lu): sections are told apart by contents",
                 sec->name.empty() ? L"<exe>" : sec->name.c_str(), sec->host_path_error);
    }
    *out = std::move(sec);
    return st::Success;
}

std::wstring ImageLabel(const ImageSection* sec) {
    if (!sec) return L"<no section>";
    if (!sec->guest_path.empty()) return sec->guest_path;
    if (!sec->name.empty()) return sec->name;
    if (!sec->host_path.empty()) return sec->host_path;
    return L"<unnamed image>";
}

bool SameImageFile(const ImageSection& a, const ImageSection& b, const char** how) {
    const char* dummy = nullptr;
    if (!how) how = &dummy;
    if (&a == &b) { *how = "same section object"; return true; }
    if (!a.host_path.empty() && !b.host_path.empty()) {
        const bool eq = CompareStringOrdinal(a.host_path.c_str(), -1, b.host_path.c_str(), -1, TRUE) == CSTR_EQUAL;
        *how = eq ? "same host path" : "different host paths";
        return eq;
    }
    // At least one path is unknown. Two unknowns are not "equal" (that made every
    // Wine builtin look like ntdll.dll on the console): compare what we hold.
    if (a.bytes.size() != b.bytes.size()) { *how = "host path unknown, file sizes differ"; return false; }
    const bool eq = a.bytes.empty() || std::memcmp(a.bytes.data(), b.bytes.data(), a.bytes.size()) == 0;
    *how = eq ? "host path unknown, contents identical" : "host path unknown, contents differ";
    return eq;
}

void DumpGuestModules(const char* why) {
    const char* w = why ? why : "";
    auto* peb = reinterpret_cast<PPEB>(__readgsqword(0x60));   // x64 TEB->ProcessEnvironmentBlock
    if (!peb || !peb->Ldr) { WARN(modules, "guest module list (%s): no PEB->Ldr yet", w); return; }
    PLIST_ENTRY head = &peb->Ldr->InMemoryOrderModuleList;
    bool ntdll = false, k32 = false, kbase = false;
    unsigned n = 0;
    for (PLIST_ENTRY e = head->Flink; e && e != head && n < 512; e = e->Flink, ++n) {
        auto* m = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        std::wstring_view name(m->FullDllName.Buffer ? m->FullDllName.Buffer : L"",
                               m->FullDllName.Buffer ? m->FullDllName.Length / sizeof(wchar_t) : 0);
        TRACE(modules, "  [%u] %p %.*ls", n, m->DllBase, static_cast<int>(name.size()), name.data());
        std::wstring_view bn = name;
        if (auto pos = bn.find_last_of(L"\\/"); pos != std::wstring_view::npos) bn = bn.substr(pos + 1);
        if (IEqualsW(bn, L"ntdll.dll")) ntdll = true;
        else if (IEqualsW(bn, L"kernel32.dll")) k32 = true;
        else if (IEqualsW(bn, L"kernelbase.dll")) kbase = true;
    }
    TRACE(modules, "guest module list (%s): %u modules; ntdll=%d kernel32=%d kernelbase=%d%s", w, n, ntdll, k32, kbase,
          kbase ? "" : "  <-- KERNELBASE MISSING (kernel32 forwarders will dangle)");
}

void NoteGuestEntry(const CONTEXT* ctx) {
    if (!ctx) return;
    void* thunk = GuestEntryThunk();
    DWORD64 rip = 0;
    if (!thunk || probe::Load<DWORD64>(&ctx->Rip, &rip) != st::Success ||   // the host's NtContinue reports a bad context
        rip != reinterpret_cast<DWORD64>(thunk))
        return;                                                             // not the main-thread launch continuation
    static volatile LONG done = 0;
    if (InterlockedExchange(&done, 1)) return;                            // once per process
    TRACE(modules, "guest entry reached (RtlUserThreadStart at %p)", thunk);
    DumpGuestModules("guest entry");
    // Wine's loader_init has finished here and the guest's own code has not run
    // yet: the last moment at which our TEB.TlsSlots entry is still provably
    // untouched. Both facts are asserted by integration.guest_hello_tls_slot.
    ReserveGuestTlsSlot("guest entry");
    TestClobberThreadSlot("guest entry");   // no-op unless WIN10XBOX_TEST_TLS_CLOBBER is set
    if (ThreadSlotIntact())
        TRACE(modules, "guest entry: TEB.TlsSlots[%lu] still holds our ThreadState", ThreadSlotIndex());
    else
        ERR(modules, "guest entry: TEB.TlsSlots[%lu] no longer holds our ThreadState (%llu clobbers)",
            ThreadSlotIndex(), static_cast<unsigned long long>(ThreadSlotClobbers()));
}

NTSTATUS MapImageView(const std::shared_ptr<ImageSection>& sec, const MapOptions& opts, ImageView** out) {
    auto view = std::make_unique<ImageView>();
    view->section = sec;
    loader::MapFlags flags = loader::MapFlags::ReserveOnly | loader::MapFlags::RegisterUnwind;
    if (opts.preferred) flags = flags | loader::MapFlags::NoRelocate;
    NTSTATUS s = loader::MapImageFromMemory(sec->bytes.data(), sec->bytes.size(), flags, opts.preferred, &view->image);
    if (!NtSuccess(s)) {
        WARN(dispatch, "mapping %ls failed: %08lx", sec->host_path.c_str(), static_cast<unsigned long>(s));
        return s;
    }
    view->image.path = sec->guest_path.empty() ? sec->host_path : sec->guest_path;
    auto* base = static_cast<std::uint8_t*>(view->image.base);

    // Windows reports the actual base in the mapped header after relocating;
    // Wine's perform_relocations() relies on that to not relocate twice.
    if (view->image.relocated) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            nt->OptionalHeader.ImageBase = reinterpret_cast<std::uint64_t>(base);
    }

    std::vector<loader::StubPatch> patched;
    std::vector<std::string> unmatched;
    view->stubs = loader::RewriteSyscallStubs(view->image, SyscallEntryAddress(), &patched, &unmatched);
    view->stubs_rewritten = view->stubs != 0;
    // Dispatch by name: a native DLL's thunks carry its own build's ids (no-op for Wine's builtins).
    if (!patched.empty()) RenumberSyscallThunks(patched, view->image.path.c_str());
    if (view->stubs_rewritten || !unmatched.empty())
        TRACE(dispatch, "%ls: %zu syscall stubs rewritten, %zu unmatched", view->image.path.c_str(), view->stubs,
              unmatched.size());
    for (auto& u : unmatched) WARN(dispatch, "%ls: stub %s not rewritten", view->image.path.c_str(), u.c_str());
    // What an unmatched export actually is (a forwarder, or which bytes), for the first few: on a native win32u
    // these are the calls that would reach the kernel without the dispatcher.
    for (std::size_t i = 0; i < unmatched.size() && i < 6 && !patched.empty(); ++i) {
        std::string fwd;
        const auto* p = static_cast<const std::uint8_t*>(loader::FindExport(view->image.base, unmatched[i].c_str(), &fwd));
        if (!fwd.empty()) { TRACE(dispatch, "  %s: forwarder to %s", unmatched[i].c_str(), fwd.c_str()); continue; }
        if (!p) continue;
        char hex[32 * 3 + 1] = {};
        for (int b = 0; b < 32; ++b) std::snprintf(hex + 3 * b, 4, "%02X ", p[b]);
        TRACE(dispatch, "  %s at %p: %s", unmatched[i].c_str(), static_cast<const void*>(p), hex);
    }

    loader::ScanOptions so;
    so.patch_cpuid = config::Current().patch_cpuid;
    so.patch_rdtsc = true;
    so.trap_random = ForceRandomTrap();
    so.patch_syscall = !view->stubs_rewritten;   // rewritten thunks no longer hold `syscall`
#if WIN10XBOX_HAVE_CLOCK
    if (clock::VirtualSharedData()) {   // D18: the version block of KUSER_SHARED_DATA says 10.0.19045, not the host's build
        so.patch_kuser_reads = true;
        so.kuser_lo = 0x7FFE0000ull + clock::usd::kNtBuildNumber;
        so.kuser_hi = 0x7FFE0000ull + clock::usd::kNtMinorVersion + 4;
        so.kuser_addresses = true;
    }
#endif
    so.handler = &CpuSiteHandler;
    loader::ScanResult sr;
    NTSTATUS ss = loader::ScanAndPatch(view->image, so, &sr);
    if (!NtSuccess(ss)) WARN(dispatch, "%ls: scan failed %08lx", view->image.path.c_str(), static_cast<unsigned long>(ss));
    else if (!sr.sites.empty())
        TRACE(dispatch, "%ls: %zu sites (%zu inline, %zu trapped, %zu skipped)", view->image.path.c_str(),
              sr.sites.size(), sr.patched, sr.trapped, sr.skipped);

    if (!opts.keep_writable) {
        if (NTSTATUS fs = loader::FinalizeProtections(view->image); !NtSuccess(fs)) {
            loader::UnmapImage(view->image);
            return fs;
        }
    }
    TRACE(dispatch, "mapped %ls at %p (%zx bytes)%s", view->image.path.c_str(), view->image.base, view->image.size,
          view->image.relocated ? " relocated" : "");
    AcquireSRWLockExclusive(&g_view_lock);
    *out = view.get();
    Views().push_back(std::move(view));
    ReleaseSRWLockExclusive(&g_view_lock);
    return s;
}

NTSTATUS FinalizeView(ImageView* view) { return loader::FinalizeProtections(view->image); }

bool FindView(const void* address, ImageView* copy_out) {
    auto a = reinterpret_cast<std::uintptr_t>(address);
    AcquireSRWLockShared(&g_view_lock);
    bool found = false;
    for (auto& v : Views()) {
        auto b = reinterpret_cast<std::uintptr_t>(v->image.base);
        if (a >= b && a < b + v->image.size) {
            if (copy_out) *copy_out = *v;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_view_lock);
    return found;
}

bool FindViewByBase(const void* base, ImageView* copy_out) {
    ImageView v;
    if (!FindView(base, &v) || v.image.base != base) return false;
    if (copy_out) *copy_out = std::move(v);
    return true;
}

static bool UnmapView(const void* base) {
    std::unique_ptr<ImageView> victim;
    AcquireSRWLockExclusive(&g_view_lock);
    auto& views = Views();
    for (auto it = views.begin(); it != views.end(); ++it) {
        if ((*it)->image.base == base) {
            victim = std::move(*it);
            views.erase(it);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_view_lock);
    if (!victim) return false;
    TRACE(dispatch, "unmapping %ls at %p", victim->image.path.c_str(), base);
    TRACE(modules, "unmap %ls at %p", ImageLabel(victim->section.get()).c_str(), base);
    loader::UnmapImage(victim->image);
    return true;
}

void SetMainImage(const std::shared_ptr<ImageSection>& sec) { g_main_image = sec; }
const std::uint8_t* MainImageInfo() { return g_main_image ? g_main_image->info : nullptr; }

// ---- Translators ----------------------------------------------------------------

NTSTATUS NTAPI Xl_NtCreateSection(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PLARGE_INTEGER size,
                                  ULONG protect, ULONG attributes, HANDLE file) {
    if (!(attributes & kSecImage) || !file)
        return CreateNamedHostObject(L"Section", handle, attr, [&](POBJECT_ATTRIBUTES o, PHANDLE out) {
            return Host().NtCreateSection(out, access, o, size, protect, attributes, file);
        });
    // The attributes are optional, the handle is not; both are caller memory.
    if (NTSTATUS ps = probe::ProbeWrite(handle, sizeof(HANDLE))) return ps;
    probe::ObjectAttributes cap;
    if (attr)
        if (NTSTATUS cs = probe::CaptureObjectAttributes(attr, &cap)) return cs;
    const std::wstring_view name = cap.name.view();
    if (name.size()) FIXME(dispatch, "named image section %.*ls not supported, creating it unnamed",
                           static_cast<int>(name.size()), name.data());
    std::shared_ptr<ImageSection> sec;
    if (NTSTATUS s = CreateImageSectionFromFile(file, &sec)) return s;
    HANDLE h = NewSectionHandle(sec);
    if (!h) return st::NoMemory;
    if (NTSTATUS ws = probe::Store<HANDLE>(handle, h)) {
        Xl_NtClose(h);   // the caller's slot vanished: do not leak the section
        return ws;
    }
    TRACE(dispatch, "image section %p for %ls", h, sec->host_path.c_str());
    TRACE(modules, "section %p: %ls (%zu bytes, host path %ls)", h, ImageLabel(sec.get()).c_str(), sec->bytes.size(),
          sec->host_path.empty() ? L"<unknown>" : sec->host_path.c_str());
    return st::Success;
}

static NTSTATUS NTAPI Xl_NtCreateSectionEx(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attr,
                                           PLARGE_INTEGER size, ULONG protect, ULONG attributes, HANDLE file,
                                           PVOID ext, ULONG ext_count) {
    if (!(attributes & kSecImage) || !file)
        return CreateNamedHostObject(L"Section", handle, attr, [&](POBJECT_ATTRIBUTES o, PHANDLE out) {
            return Host().NtCreateSectionEx(out, access, o, size, protect, attributes, file, ext, ext_count);
        });
    if (ext_count) FIXME(dispatch, "image section extended parameters ignored (%lu)", ext_count);
    return Xl_NtCreateSection(handle, access, attr, size, protect, attributes, file);
}

static NTSTATUS MapOurs(const std::shared_ptr<ImageSection>& sec, HANDLE process, PVOID* base, PSIZE_T view_size) {
    if (!IsCurrentProcess(process)) {
        FIXME(dispatch, "mapping an image section into another process");
        return st::NotSupported;
    }
    // The base address is read and written back, the view size is optional: probed before anything is mapped.
    PVOID preferred = nullptr;
    if (NTSTATUS ps = probe::ProbeWrite(base, sizeof(PVOID))) return ps;
    if (NTSTATUS ps = probe::Load<PVOID>(base, &preferred)) return ps;
    if (view_size)
        if (NTSTATUS ps = probe::ProbeWrite(view_size, sizeof(SIZE_T))) return ps;
    MapOptions opts;
    opts.preferred = preferred;
    ImageView* view = nullptr;
    NTSTATUS s = MapImageView(sec, opts, &view);
    if (!NtSuccess(s)) {
        WARN(modules, "map of %ls failed: %08lx", ImageLabel(sec.get()).c_str(), static_cast<unsigned long>(s));
        return s;
    }
    void* const mapped_base = view->image.base;
    const SIZE_T mapped_size = view->image.size;
    const bool relocated = view->image.relocated;
    NTSTATUS ws = probe::Store<PVOID>(base, mapped_base);
    if (ws == st::Success && view_size) ws = probe::Store<SIZE_T>(view_size, mapped_size);
    if (ws != st::Success) {
        UnmapView(mapped_base);   // the caller's output vanished: do not leave an unreachable view behind
        return ws;
    }
    TRACE(modules, "map %ls at %p, %zu bytes%s", ImageLabel(sec.get()).c_str(), mapped_base, mapped_size,
          relocated ? ", relocated" : "");
    return relocated ? st::ImageNotAtBase : st::Success;
}

NTSTATUS NTAPI Xl_NtMapViewOfSection(HANDLE section, HANDLE process, PVOID* base, ULONG_PTR zero_bits,
                                     SIZE_T commit, PLARGE_INTEGER offset, PSIZE_T view_size, ULONG inherit,
                                     ULONG alloc_type, ULONG protect) {
    if (auto sec = SectionFromHandle(section)) return MapOurs(sec, process, base, view_size);
    return Host().NtMapViewOfSection(section, process, base, zero_bits, commit, offset, view_size, inherit, alloc_type,
                                     protect);
}

// MEM_EXTENDED_PARAMETER { DWORD64 Type:8, Reserved:56; union { ... ULONG ULong; } } is 16 bytes. An image machine
// (MemExtendedParameterImageMachine) other than this process's (AMD64) cannot be honoured: Windows answers
// STATUS_NOT_SUPPORTED for it on an x64 process, 0 meaning "the section's own machine".
static NTSTATUS CheckImageMachineParameters(PVOID ext, ULONG ext_count) {
    constexpr ULONG kMemExtendedParameterImageMachine = 6;
    if (!ext) return st::Success;
    for (ULONG i = 0; i < ext_count; ++i) {
        std::uint64_t e[2];
        if (NTSTATUS s = probe::CopyFromUser(e, static_cast<const std::uint8_t*>(ext) + 16 * static_cast<size_t>(i), sizeof e))
            return s;
        if ((e[0] & 0xFF) == kMemExtendedParameterImageMachine && static_cast<ULONG>(e[1]) != 0 &&
            static_cast<ULONG>(e[1]) != IMAGE_FILE_MACHINE_AMD64)
            return st::NotSupported;
    }
    return st::Success;
}

static NTSTATUS NTAPI Xl_NtMapViewOfSectionEx(HANDLE section, HANDLE process, PVOID* base, PLARGE_INTEGER offset,
                                              PSIZE_T view_size, ULONG alloc_type, ULONG protect, PVOID ext,
                                              ULONG ext_count) {
    if (auto sec = SectionFromHandle(section)) {
        if (NTSTATUS s = CheckImageMachineParameters(ext, ext_count)) return s;
        return MapOurs(sec, process, base, view_size);
    }
    return Host().NtMapViewOfSectionEx(section, process, base, offset, view_size, alloc_type, protect, ext, ext_count);
}

static NTSTATUS NTAPI Xl_NtUnmapViewOfSection(HANDLE process, PVOID base) {
    if (IsCurrentProcess(process) && FindViewByBase(base, nullptr)) return UnmapView(base) ? st::Success : st::NotMappedView;
    return Host().NtUnmapViewOfSection(process, base);
}

static NTSTATUS NTAPI Xl_NtUnmapViewOfSectionEx(HANDLE process, PVOID base, ULONG flags) {
    if (IsCurrentProcess(process) && FindViewByBase(base, nullptr)) return UnmapView(base) ? st::Success : st::NotMappedView;
    return Host().NtUnmapViewOfSectionEx(process, base, flags);
}

static NTSTATUS NTAPI Xl_NtQuerySection(HANDLE section, ULONG klass, PVOID buffer, SIZE_T length, PSIZE_T ret_len) {
    auto sec = SectionFromHandle(section);
    if (!sec) return Host().NtQuerySection(section, klass, buffer, length, ret_len);
    // Both caller buffers are probed before either is written.
    if (NTSTATUS ps = probe::ProbeWrite(buffer, length)) return ps;
    if (ret_len)
        if (NTSTATUS ps = probe::ProbeWrite(ret_len, sizeof(SIZE_T))) return ps;
    switch (klass) {
    case 0: {   // SectionBasicInformation
        struct { PVOID BaseAddress; ULONG Attributes; LARGE_INTEGER Size; } info{};
        info.Attributes = kSecImage | kSecFile;
        info.Size.QuadPart = sec->size_of_image;
        if (length < sizeof(info)) return st::InfoLengthMismatch;
        if (NTSTATUS ws = probe::CopyToUser(buffer, &info, sizeof(info))) return ws;
        return probe::StoreIfPresent<SIZE_T>(ret_len, sizeof(info));
    }
    case 1:     // SectionImageInformation
        if (length < sizeof(sec->info)) return st::InfoLengthMismatch;
        if (NTSTATUS ws = probe::CopyToUser(buffer, sec->info, sizeof(sec->info))) return ws;
        return probe::StoreIfPresent<SIZE_T>(ret_len, sizeof(sec->info));
    default:
        FIXME(dispatch, "NtQuerySection class %lu on an image section", klass);
        return st::InvalidInfoClass;
    }
}

// Wine's find_existing_module() calls this for every loaded module whose
// TimeDateStamp and CheckSum match a freshly mapped DLL; all of Wine's PE
// builtins from one build share both, so this answer alone decides whether
// kernel32.dll "is" ntdll.dll. SUCCESS makes Wine unmap the new view and reuse
// the old module; it must only be returned for the same file (SameImageFile).
static NTSTATUS NTAPI Xl_NtAreMappedFilesTheSame(PVOID a, PVOID b) {
    ImageView va, vb;
    bool ha = FindView(a, &va), hb = FindView(b, &vb);
    if (!ha && !hb) return Host().NtAreMappedFilesTheSame(a, b);
    const char* how = "not two image views of ours";
    const bool same = ha && hb && va.section && vb.section && SameImageFile(*va.section, *vb.section, &how);
    TRACE(modules, "same file? %ls at %p vs %ls at %p -> %s (%s)", ImageLabel(va.section.get()).c_str(), a,
          ImageLabel(vb.section.get()).c_str(), b, same ? "SAME" : "different", how);
    return same ? st::Success : st::NotSameDevice;
}

std::shared_ptr<ImageSection> ImageSectionFromHandle(HANDLE h) { return SectionFromHandle(h); }

// Called by NtClose/NtDuplicateObject routing (nt_process.cpp).
void CloseImageSection(const EmulatedObject& o) { CloseSectionObject(o); }
HANDLE DuplicateImageSection(const EmulatedObject& o) {
    auto sec = *static_cast<std::shared_ptr<ImageSection>*>(o.extra);
    return NewSectionHandle(sec);
}

const ImplDef* SectionImpls() {
    static const ImplDef k[] = {
        {"NtCreateSection", reinterpret_cast<void*>(&Xl_NtCreateSection)},
        {"NtCreateSectionEx", reinterpret_cast<void*>(&Xl_NtCreateSectionEx)},
        {"NtMapViewOfSection", reinterpret_cast<void*>(&Xl_NtMapViewOfSection)},
        {"NtMapViewOfSectionEx", reinterpret_cast<void*>(&Xl_NtMapViewOfSectionEx)},
        {"NtUnmapViewOfSection", reinterpret_cast<void*>(&Xl_NtUnmapViewOfSection)},
        {"NtUnmapViewOfSectionEx", reinterpret_cast<void*>(&Xl_NtUnmapViewOfSectionEx)},
        {"NtQuerySection", reinterpret_cast<void*>(&Xl_NtQuerySection)},
        {"NtAreMappedFilesTheSame", reinterpret_cast<void*>(&Xl_NtAreMappedFilesTheSame)},
        {nullptr, nullptr},
    };
    return k;
}

}  // namespace win10xbox::dispatch::detail
