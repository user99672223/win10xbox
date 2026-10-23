// Route B (config gui = Native): native DLLs as guest code next to Wine's builtins -- the native user32/gdi32/
// win32u and, for Direct3D (docs/DECISIONS.md G1), the native d3d11/dxgi/d3dcompiler_47/d3d10warp. They import
// functions from kernelbase (directly, or through an api-ms-win-* set the host's apiset schema resolves to
// kernelbase: the guest shares the host's PEB.ApiSetMap) that Wine 11.9's kernelbase does not export:
// GlobalLock & co. (Wine keeps them in kernel32), WTSGetServiceSessionId, SetProcessValidCallTargets,
// GetPersistedRegistryLocationW, ... Wine's loader binds such an import to a stub that raises
// EXCEPTION_WINE_STUB (0x80000100) when called, which is how the native user32's DllMain died.
//
// So, in native GUI mode only, the guest kernelbase's export table is extended right after it is mapped (the same
// technique as ExtendGuestNtdllExports in rtl_forward.cpp): every CODE export of the host kernelbase the guest
// kernelbase lacks is added, as a jump to
//   1. Wine's kernel32 export of that name, when the guest kernel32 (mapped before kernelbase: Wine's loader
//      loads kernel32 first) has it as code -- Wine's own implementation, consistent with Wine's heap and handles;
//   2. otherwise the host kernelbase's function itself (it runs as guest code: no host logging, no host TLS).
// The new directory and the jumps live in one allocation within 4 GiB above the image (export RVAs are 32-bit).
#include "internal.h"

#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace win10xbox::dispatch::detail {
namespace {

constexpr std::size_t kJumpSlot = 16;   // FF 25 00000000 <abs64>, padded with int3

struct Exports {
    const std::uint8_t* base = nullptr;
    const IMAGE_EXPORT_DIRECTORY* dir = nullptr;
    std::uint32_t dir_rva = 0, dir_size = 0;
    const DWORD* functions = nullptr;
    const DWORD* names = nullptr;
    const WORD* ordinals = nullptr;
    bool Open(const void* image) {
        if (!image) return false;
        base = static_cast<const std::uint8_t*>(image);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!d.VirtualAddress || !d.Size) return false;
        dir_rva = d.VirtualAddress;
        dir_size = d.Size;
        dir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir_rva);
        functions = reinterpret_cast<const DWORD*>(base + dir->AddressOfFunctions);
        names = reinterpret_cast<const DWORD*>(base + dir->AddressOfNames);
        ordinals = reinterpret_cast<const WORD*>(base + dir->AddressOfNameOrdinals);
        return true;
    }
    const char* Name(std::uint32_t i) const { return reinterpret_cast<const char*>(base + names[i]); }
    // Index into names[] of `name`, or -1 (names are sorted).
    long Find(const char* name) const {
        std::uint32_t lo = 0, hi = dir->NumberOfNames;
        while (lo < hi) {
            const std::uint32_t mid = (lo + hi) / 2;
            const int c = std::strcmp(Name(mid), name);
            if (!c) return static_cast<long>(mid);
            if (c < 0) lo = mid + 1; else hi = mid;
        }
        return -1;
    }
    bool IsCode(std::uint32_t rva) const {
        if (rva >= dir_rva && rva < dir_rva + dir_size) return false;   // forwarder string
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
                return (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        return false;
    }
    // Address of the code export `name`, or null (absent, forwarder or data).
    const void* Code(const char* name) const {
        const long i = Find(name);
        if (i < 0) return nullptr;
        const std::uint32_t rva = functions[ordinals[i]];
        return IsCode(rva) ? base + rva : nullptr;
    }
};

// Namespace scope, trivially initialised; written by MapImageView (one mapping at a time per image, and the guest's
// loader maps kernel32 before kernelbase on the same thread).
const void* g_guest_kernel32 = nullptr;
const void* g_guest_gdi32 = nullptr;   // Wine's gdi32 (machine-emu-d3d)

bool EndsWithI(const std::wstring& s, const wchar_t* tail) {
    const std::size_t n = std::wcslen(tail);
    return s.size() >= n && !_wcsicmp(s.c_str() + s.size() - n, tail);
}

// Completes the export table of `img` from the host module `host_module`: every export of the host module the image
// lacks is added as a jump to
//   1. the same-named CODE export of `wine` (a guest image already mapped: kernel32 for kernelbase, Wine's gdi32 for the
//      gdi32full stand-in), when it has one; else
//   2. the host module's function (a forwarder resolved with GetProcAddress, D3DKMT* names only; other forwarders are
//      skipped).
// `replace_stubs`: a Wine spec `stub` export of the image (it raises EXCEPTION_WINE_STUB) is pointed at the host's function.
// `only_ddi`: only the host's D3DKMT* exports; `resolve_forwarders`: host forwarders are added too (GetProcAddress).
std::size_t ExtendExports(const loader::MappedImage& img, const wchar_t* label, HMODULE host_kb, const void* wine,
                          bool only_ddi, bool resolve_forwarders, bool replace_stubs = true) {
    Exports guest, host, k32;
    if (!guest.Open(img.base) || !host.Open(host_kb)) {
        WARN(dispatch, "gui native: guest %ls exports not extended (%s)", label,
             host_kb ? "the guest image has no export directory" : "the host module is not loaded");
        return 0;
    }
    const bool have_k32 = k32.Open(wine);
    struct Add { const char* name; const void* target; bool wine; long fn; };
    std::vector<Add> add;   // names the image lacks
    std::vector<Add> rep;   // Wine `stub` exports of the image (they raise EXCEPTION_WINE_STUB when called): fn = index
    std::size_t from_wine = 0;
    // A Wine spec `stub` export is a tiny function that loads (lea reg,[rip+disp32]) the strings of its dll name and
    // of its OWN name and raises EXCEPTION_WINE_STUB.
    auto is_wine_stub = [&](const char* name, std::uint32_t rva) {
        if (!guest.IsCode(rva)) return false;
        const std::uint8_t* code = guest.base + rva;
        const std::size_t n = std::strlen(name) + 1;
        for (int k = 0; k + 7 <= 48; ++k) {
            if ((code[k] == 0x48 || code[k] == 0x4C) && code[k + 1] == 0x8D && (code[k + 2] & 0xC7) == 0x05) {
                std::int32_t d;
                std::memcpy(&d, code + k + 3, 4);
                const std::uint8_t* tgt = code + k + 7 + d;
                if (tgt >= guest.base && tgt + n <= guest.base + img.size && std::memcmp(tgt, name, n) == 0) return true;
            }
        }
        return false;
    };
    for (std::uint32_t i = 0; i < host.dir->NumberOfNames; ++i) {
        const char* name = host.Name(i);
        const std::uint32_t rva = host.functions[host.ordinals[i]];
        const bool ddi = std::strncmp(name, "D3DKMT", 6) == 0;
        if (only_ddi && !ddi) continue;
        const long gi = guest.Find(name);
        long fn = -1;
        if (gi >= 0) {
            if (!replace_stubs) continue;
            fn = guest.ordinals[gi];
            if (!is_wine_stub(name, guest.functions[fn])) continue;
        }
        const void* target = nullptr;
        bool from_w = false;
        if (const void* w = have_k32 ? k32.Code(name) : nullptr) {
            target = w;
            from_w = true;
        } else if (host.IsCode(rva)) {
            target = host.base + rva;
        } else if (ddi || resolve_forwarders) {   // a forwarder: the function the host's loader resolves it to
            target = reinterpret_cast<const void*>(GetProcAddress(host_kb, name));
        }
        if (!target) continue;
        if (from_w) ++from_wine;
        (fn >= 0 ? rep : add).push_back({name, target, from_w, fn});
    }
    if (add.empty() && rep.empty()) return 0;
    const std::uint32_t old_funcs = guest.dir->NumberOfFunctions, old_names = guest.dir->NumberOfNames;
    std::size_t strings = 0;
    for (auto& a : add) strings += std::strlen(a.name) + 1;
    // Wine's kernelbase has forwarders (to ntdll): their strings must move into the new directory's range, or the
    // loader would take the old string's address for code.
    for (std::uint32_t i = 0; i < old_funcs; ++i)
        if (guest.functions[i] >= guest.dir_rva && guest.functions[i] < guest.dir_rva + guest.dir_size)
            strings += std::strlen(reinterpret_cast<const char*>(guest.base + guest.functions[i])) + 1;
    const std::size_t thunks = (add.size() + rep.size()) * kJumpSlot;
    const std::size_t total = thunks + sizeof(IMAGE_EXPORT_DIRECTORY) + 4 * (old_funcs + add.size()) +
                              4 * (old_names + add.size()) + 2 * (old_names + add.size()) + strings + 64;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(img.base);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    std::uint8_t* mem = nullptr;
    for (std::uintptr_t a = (base + img.size + gran - 1) & ~(gran - 1); a + total - base < 0xFFFF0000ull && !mem;
         a += gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
        mem = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(a), total, MEM_RESERVE | MEM_COMMIT,
                                                      PAGE_READWRITE));
    }
    if (!mem) {
        WARN(dispatch, "gui native: no room within 4 GiB of the guest kernelbase for %zu more exports", add.size());
        return 0;
    }
    auto rva_of = [&](const void* p) { return static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(p) - base); };
    std::uint8_t* t = mem;
    std::vector<DWORD> fn_rva, rep_rva;
    for (int pass = 0; pass < 2; ++pass)
    for (auto& a : pass ? rep : add) {
        const std::uint8_t code[6] = {0xFF, 0x25, 0, 0, 0, 0};   // jmp [rip+0]
        std::memcpy(t, code, sizeof code);
        const std::uint64_t target = reinterpret_cast<std::uint64_t>(a.target);
        std::memcpy(t + 6, &target, 8);
        std::memset(t + 14, 0xCC, kJumpSlot - 14);
        (pass ? rep_rva : fn_rva).push_back(rva_of(t));
        t += kJumpSlot;
    }
    auto* dir = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(mem + thunks);
    *dir = *guest.dir;
    auto* functions = reinterpret_cast<DWORD*>(dir + 1);
    std::memcpy(functions, guest.functions, 4 * old_funcs);
    for (std::size_t k = 0; k < add.size(); ++k) functions[old_funcs + k] = fn_rva[k];
    for (std::size_t k = 0; k < rep.size(); ++k) functions[rep[k].fn] = rep_rva[k];   // the Wine stubs become the host's
    auto* names = functions + old_funcs + add.size();
    auto* ords = reinterpret_cast<WORD*>(names + old_names + add.size());
    char* str = reinterpret_cast<char*>(ords + old_names + add.size());
    std::uint32_t gi = 0, out = 0;
    std::size_t ai = 0;
    while (gi < old_names || ai < add.size()) {   // merge two sorted name lists
        const bool take_guest = ai == add.size() || (gi < old_names && std::strcmp(guest.Name(gi), add[ai].name) < 0);
        if (take_guest) {
            names[out] = guest.names[gi];
            ords[out] = guest.ordinals[gi];
            ++gi;
        } else {
            const std::size_t n = std::strlen(add[ai].name) + 1;
            std::memcpy(str, add[ai].name, n);
            names[out] = rva_of(str);
            ords[out] = static_cast<WORD>(old_funcs + ai);
            str += n;
            ++ai;
        }
        ++out;
    }
    std::size_t old_forwarders = 0;
    for (std::uint32_t i = 0; i < old_funcs; ++i) {
        if (guest.functions[i] < guest.dir_rva || guest.functions[i] >= guest.dir_rva + guest.dir_size) continue;
        const char* fwd = reinterpret_cast<const char*>(guest.base + guest.functions[i]);
        const std::size_t n = std::strlen(fwd) + 1;
        std::memcpy(str, fwd, n);
        functions[i] = rva_of(str);
        str += n;
        ++old_forwarders;
    }
    dir->NumberOfFunctions = static_cast<DWORD>(old_funcs + add.size());
    dir->NumberOfNames = static_cast<DWORD>(old_names + add.size());
    dir->AddressOfFunctions = rva_of(functions);
    dir->AddressOfNames = rva_of(names);
    dir->AddressOfNameOrdinals = rva_of(ords);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(static_cast<std::uint8_t*>(img.base) + dos->e_lfanew);
    auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(&d, &mbi, sizeof mbi);
    DWORD old = 0;
    const bool flip = !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    if (flip) VirtualProtect(&d, sizeof d, PAGE_READWRITE, &old);
    d.VirtualAddress = rva_of(dir);
    d.Size = static_cast<DWORD>(reinterpret_cast<std::uint8_t*>(str) - reinterpret_cast<std::uint8_t*>(dir));
    if (flip) VirtualProtect(&d, sizeof d, old, &old);
    // One protection for the whole block: the jump slots and the directory share a page, and a second call on the
    // directory part would take the execute right from the last slots' page (the first native run died executing
    // a jump slot in a read-only page).
    VirtualProtect(mem, total, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, thunks);
    std::string wine_names, host_names;
    std::size_t nw = 0, nh = 0;
    for (auto& a : add) {
        std::string& s = a.wine ? wine_names : host_names;
        std::size_t& n = a.wine ? nw : nh;
        if (n++ < 12) (s += s.empty() ? "" : ", ") += a.name;
    }
    if (!rep.empty()) {
        std::string r;
        for (std::size_t k = 0; k < rep.size() && k < 12; ++k) (r += r.empty() ? "" : ", ") += rep[k].name;
        TRACE(dispatch, "gui native: %ls: %zu Wine stub export(s) replaced by the host's function (%s%s)", label, rep.size(),
              r.c_str(), rep.size() > 12 ? ", ..." : "");
    }
    TRACE(dispatch, "gui native: guest kernelbase: %zu exports added, %zu as jumps to Wine's kernel32 (%s%s), %zu to "
                    "the host kernelbase (%s%s); %zu forwarders moved",
          add.size(), from_wine, wine_names.c_str(), nw > 12 ? ", ..." : "", add.size() - from_wine,
          host_names.c_str(), nh > 12 ? ", ..." : "", old_forwarders);
    return add.size() + rep.size();
}

}  // namespace

void NoteNativeGuestImage(const loader::MappedImage& img, const std::wstring& host_path,
                          const std::wstring& export_name) {
    if (!NativeGuiActive() && !HostD3dkmtActive()) return;
    // ntdll, kernelbase and kernel32 are always Wine's builtins (docs/DECISIONS.md E1), so the name identifies them.
    // The host path is the better source where it exists; in an AppContainer GetFinalPathNameByHandleW is refused
    // (the host path is empty) and the image's own export-directory name is all there is.
    const std::wstring& id = host_path.empty() ? export_name : host_path;
    if (id.empty()) return;
    // The native dxgi / d3d11 (not Wine's builtins): the layer sits between them and the guest (dxgi_interpose.cpp).
    if (IsDxgiInterposedModule(id) && (host_path.empty() || (host_path.find(L"\\builtin\\") == std::wstring::npos &&
                                                             host_path.find(L"/builtin/") == std::wstring::npos))) {
        InterposeDxgiImage(img, id);
        return;
    }
    // machine-emu-d3d: the gdi32full.dll stand-in (tests/guest/gdi32full_stub.c) gets the host's gdi32full exports.
    if (HostD3dkmtActive() && EndsWithI(id, L"gdi32full.dll") &&
        (id.size() == 13 || id[id.size() - 14] == L'\\' || id[id.size() - 14] == L'/')) {
        const std::size_t n = ExtendExports(img, L"gdi32full.dll", HostD3dMods().gdi32full, g_guest_gdi32, false, true);
        TRACE(dispatch, "d3d native: gdi32full stand-in: %zu export(s) added from the host's gdi32full (Wine gdi32 %s)", n,
              g_guest_gdi32 ? "mapped" : "NOT mapped yet");
        return;
    }
    if (!host_path.empty() && host_path.find(L"\\builtin\\") == std::wstring::npos &&
        host_path.find(L"/builtin/") == std::wstring::npos)
        return;
    if (EndsWithI(id, L"kernel32.dll") && (id.size() == 12 || id[id.size() - 13] == L'\\' || id[id.size() - 13] == L'/'))
        g_guest_kernel32 = img.base;
    else if (EndsWithI(id, L"kernelbase.dll") &&
             (id.size() == 14 || id[id.size() - 15] == L'\\' || id[id.size() - 15] == L'/'))
        ExtendExports(img, L"kernelbase.dll", GetModuleHandleW(L"kernelbase.dll"), g_guest_kernel32, false, false, false);
    else if (HostD3dkmtActive()) {
        // machine-emu-d3d: Wine's user32/gdi32/win32u next to the native dxgi/d3d11/d3d10warp, which import functions of
        // the host's builds that Wine's lack (user32!GetProcessUIContextInformation, D3DKMT* ...): every export of the
        // host's module the Wine one lacks is added, as a jump to the host's function.
        const HostD3dModules& m = HostD3dMods();
        auto leaf = [&](const wchar_t* n) {
            const std::size_t k = std::wcslen(n);
            return EndsWithI(id, n) && (id.size() == k || id[id.size() - k - 1] == L'\\' || id[id.size() - k - 1] == L'/');
        };
        if (leaf(L"gdi32.dll")) {
            g_guest_gdi32 = img.base;
            const std::size_t n = ExtendExports(img, L"gdi32.dll", m.gdi32, nullptr, false, true);
            TRACE(dispatch, "d3d native: guest gdi32: %zu export(s) added from the host's gdi32", n);
        } else if (leaf(L"user32.dll")) {
            const std::size_t n = ExtendExports(img, L"user32.dll", m.user32, nullptr, false, true);
            TRACE(dispatch, "d3d native: guest user32: %zu export(s) added from the host's user32", n);
        } else if (leaf(L"win32u.dll")) {
            const std::size_t n = ExtendExports(img, L"win32u.dll", m.win32u, nullptr, false, true);
            TRACE(dispatch, "d3d native: guest win32u: %zu export(s) added from the host's win32u", n);
        }
    }
}

}  // namespace win10xbox::dispatch::detail
