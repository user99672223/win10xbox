// probe_A_dynamic_code.cpp - category A: dynamic code execution.
//
// THE loader-critical module. If an AppContainer cannot allocate executable
// memory, run bytes it generated, let the real loader relocate a PE via a
// SEC_IMAGE section, or load a DLL we supply from outside the package, the
// whole compatibility-layer design has to route around that wall. Each question
// is probed independently and reported precisely (OK / DENIED / NOTIMPL /
// MISSING / CRASH) with the exact error code / NTSTATUS.
//
// Non-destructive: every allocation / section / mapping / temp file is released.
// The only code executed is a 6-byte stub we wrote ourselves (returns 0x5150)
// and - for the out-of-package test - a tiny payload DLL copied into the app's
// writable location before being loaded. All probes are safe on x64; the stub
// is x64 machine code.
#include "probe.h"
#include <cstdio>
#include <cstring>

namespace probe {

namespace {

// x64 stub: mov eax, 0x5150 ; ret   -> returns 20816.
const unsigned char kStub[] = {0xB8, 0x50, 0x51, 0x00, 0x00, 0xC3};
const int kStubAnswer = 0x5150;
using StubFn = int (*)();

// Dynamically resolved extended-VA entry points. kernelbase.dll exposes
// VirtualAlloc2 / MapViewOfFile3 on Windows 10 1803+; absent on older systems.
using PVirtualAlloc2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                      MEM_EXTENDED_PARAMETER*, ULONG);
using PMapViewOfFile3 = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T,
                                       ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using PVirtualFree    = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);

// NT section APIs (resolved from ntdll).
using NtCreateSectionFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, void*, PLARGE_INTEGER,
                                       ULONG, ULONG, HANDLE);
using NtMapViewFn = LONG(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T,
                                 PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
using NtUnmapViewFn = LONG(NTAPI*)(HANDLE, PVOID);

constexpr ULONG kSecImage      = 0x01000000;
constexpr ULONG kViewUnmap     = 2;
constexpr LONG  kStatusSuccess = 0;

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring(L".") : p.substr(0, s);
}

std::wstring writable_dir() {
    // Prefer the app's LocalState (UWP) if PROBE_LOCALSTATE is set by the host;
    // else fall back to %TEMP% which both hosts can use. On a Win32 baseline the
    // package LocalState doesn't exist and %TEMP% is the right place; on UWP the
    // host populates PROBE_LOCALSTATE for a known-good writable path.
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"PROBE_LOCALSTATE", buf, 1024);
    if (n && n < 1024) return std::wstring(buf, n);
    n = GetTempPathW(1024, buf);
    if (n && n < 1024) return std::wstring(buf, n);
    return exe_dir();
}

std::string hexptr(const void* p) {
    char b[32]; std::snprintf(b, sizeof b, "%p", p); return b;
}

std::string hex32(uint32_t v) {
    char b[16]; std::snprintf(b, sizeof b, "0x%08X", v); return b;
}

Status classify_gle(DWORD gle) {
    if (gle == ERROR_ACCESS_DENIED) return Status::DENIED;
    if (gle == ERROR_NOT_SUPPORTED || gle == ERROR_CALL_NOT_IMPLEMENTED) return Status::NOTIMPL;
    if (gle == ERROR_PROC_NOT_FOUND || gle == ERROR_MOD_NOT_FOUND) return Status::MISSING;
    return Status::WARN;
}

Status classify_ntstatus(LONG s) {
    switch ((uint32_t)s) {
        case 0x00000000: return Status::OK;
        case 0xC0000022: return Status::DENIED;         // STATUS_ACCESS_DENIED
        case 0xC0000002: return Status::NOTIMPL;        // STATUS_NOT_IMPLEMENTED
        case 0xC00000BB: return Status::NOTIMPL;        // STATUS_NOT_SUPPORTED
        case 0xC0000061: return Status::DENIED;         // STATUS_PRIVILEGE_NOT_HELD
    }
    return Status::WARN;
}

// Minimal valid x64 PE DLL (DOS header + NT headers + one section + relocation
// directory with one empty block) built in memory, synthesizing a DLL whose
// AddressOfEntryPoint returns kStubAnswer. We use this ONLY for the SEC_IMAGE
// test. Instead of a hand-synthesized PE (brittle), we take a different route:
// we copy an existing DLL (the probe payload if present, else kernel32.dll's
// in-memory image written to a temp file) and ask the kernel to open it as
// SEC_IMAGE. The point of the probe is "does the kernel accept SEC_IMAGE on a
// file WE opened?", not "did we hand-roll a correct PE".
std::wstring probe_payload_path() {
    // Shipped alongside the exe. The host CMakeLists copies probe_payload.dll
    // next to the main binary (and includes it in the AppX for UWP).
    return exe_dir() + L"\\probe_payload.dll";
}

bool copy_file_to_writable(const std::wstring& src, std::wstring* out_path, DWORD* gle) {
    *gle = 0;
    *out_path = writable_dir() + L"\\probe_payload_copy.dll";
    if (!CopyFileW(src.c_str(), out_path->c_str(), FALSE)) {
        *gle = GetLastError();
        return false;
    }
    return true;
}

}  // namespace

void register_A_dynamic_code(Reporter& r) {
    // A1: Can we reserve+commit PAGE_EXECUTE_READWRITE at all? (Does the
    // codeGeneration capability grant RWX?)
    r.run("A", "valloc-rwx", [](Record& rec) {
        void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
        if (p) {
            rec.status = Status::OK;
            rec.value = "addr=" + hexptr(p) + ";prot=RWX";
            rec.detail = "VirtualAlloc PAGE_EXECUTE_READWRITE succeeded "
                         "(codeGeneration grants RWX in this environment)";
            VirtualFree(p, 0, MEM_RELEASE);
        } else {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "RWX allocation refused: " + error_text(gle);
        }
    });

    // A2: Actually EXECUTE generated code from RWX memory.
    r.run("A", "exec-rwx", [](Record& rec) {
        void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
        if (!p) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "could not allocate RWX to execute: " + error_text(gle);
            return;
        }
        memcpy(p, kStub, sizeof kStub);
        FlushInstructionCache(GetCurrentProcess(), p, sizeof kStub);
        int result = reinterpret_cast<StubFn>(p)();
        rec.errorcode = (uint32_t)result;
        if (result == kStubAnswer) {
            rec.status = Status::OK;
            rec.value = "executed=1;result=" + hex32((uint32_t)result);
            rec.detail = "generated bytes ran from RWX and returned the expected value";
        } else {
            rec.status = Status::WARN;
            rec.detail = "stub ran but returned unexpected value";
        }
        VirtualFree(p, 0, MEM_RELEASE);
    });

    // A3: W^X path - alloc RW, write, flip to RX via VirtualProtect, execute.
    // This is the realistic path if a stricter policy denies RWX directly.
    r.run("A", "exec-wx-flip", [](Record& rec) {
        void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                               PAGE_READWRITE);
        if (!p) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "RW allocation failed: " + error_text(gle);
            return;
        }
        memcpy(p, kStub, sizeof kStub);
        DWORD old = 0;
        if (!VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old)) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "VirtualProtect RW->RX denied: " + error_text(gle);
            VirtualFree(p, 0, MEM_RELEASE);
            return;
        }
        FlushInstructionCache(GetCurrentProcess(), p, sizeof kStub);
        int result = reinterpret_cast<StubFn>(p)();
        rec.status = (result == kStubAnswer) ? Status::OK : Status::WARN;
        rec.value = "executed=1;result=" + hex32((uint32_t)result) + ";flip=RW->RX";
        rec.detail = "W^X execution works (alloc RW, write, VirtualProtect RX, run)";
        VirtualFree(p, 0, MEM_RELEASE);
    });

    // A4: VirtualAlloc2 placeholder reservation + split. Loader address-space
    // trick used to carve a specific VA range for a manual map.
    r.run("A", "virtualalloc2-placeholder", [](Record& rec) {
        auto pV2 = (PVirtualAlloc2)resolve("kernelbase.dll", "VirtualAlloc2");
        if (!pV2) pV2 = (PVirtualAlloc2)resolve("kernel32.dll", "VirtualAlloc2");
        if (!pV2) {
            rec.status = Status::MISSING;
            rec.detail = "VirtualAlloc2 not exported (pre-1803?)";
            return;
        }
        const SIZE_T sz = 0x4000;
        // MEM_RESERVE_PLACEHOLDER (0x40000) and MEM_PRESERVE_PLACEHOLDER (0x2)
        // are SDK macros from winnt.h on 10.0.17134+; redefining them locally
        // trips a macro-redefinition compile error. Use the SDK names directly.
        void* ph = pV2(GetCurrentProcess(), nullptr, sz,
                       MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                       PAGE_NOACCESS, nullptr, 0);
        if (!ph) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "VirtualAlloc2 placeholder reservation refused: " + error_text(gle);
            return;
        }
        // Split the placeholder in half.
        BOOL split = VirtualFree(ph, 0x2000,
                                 MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        if (!split) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "placeholder reserved but split denied: " + error_text(gle);
        } else {
            rec.status = Status::OK;
            rec.value = "base=" + hexptr(ph) + ";size=" + std::to_string(sz) +
                        ";split=1";
            rec.detail = "placeholder reserved and split - usable for custom VA layout";
        }
        VirtualFree(ph, 0, MEM_RELEASE);
        VirtualFree((char*)ph + 0x2000, 0, MEM_RELEASE);
    });

    // A5: NtCreateSection(SEC_IMAGE) on a DLL we own, then map it - letting the
    // real loader relocate a PE for us. If this works we can "load" DLLs
    // without LoadLibraryEx trusting the path.
    r.run("A", "sec-image-map", [](Record& rec) {
        auto pNtCreate = (NtCreateSectionFn)resolve("ntdll.dll", "NtCreateSection");
        auto pNtMap = (NtMapViewFn)resolve("ntdll.dll", "NtMapViewOfSection");
        auto pNtUnmap = (NtUnmapViewFn)resolve("ntdll.dll", "NtUnmapViewOfSection");
        if (!pNtCreate || !pNtMap || !pNtUnmap) {
            rec.status = Status::MISSING;
            rec.detail = "ntdll Nt{Create,MapView,UnmapView}Section not resolvable";
            return;
        }
        std::wstring src = probe_payload_path();
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
            rec.status = Status::SKIPPED;
            rec.detail = "probe_payload.dll not shipped next to host; cannot test";
            return;
        }
        // Copy to writable location first so this works identically on UWP
        // (where the exe directory may be read-only / cross-volume).
        std::wstring dst; DWORD cp_gle = 0;
        if (!copy_file_to_writable(src, &dst, &cp_gle)) {
            rec.status = classify_gle(cp_gle);
            rec.errorcode = cp_gle;
            rec.detail = "cannot copy payload to writable dir: " + error_text(cp_gle);
            return;
        }
        HANDLE hf = CreateFileW(dst.c_str(), GENERIC_READ | GENERIC_EXECUTE,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "CreateFile(GENERIC_READ|EXECUTE) failed: " + error_text(gle);
            DeleteFileW(dst.c_str());
            return;
        }
        HANDLE sect = nullptr;
        constexpr ACCESS_MASK SECTION_ALL = 0xF001F;
        LONG s = pNtCreate(&sect, SECTION_ALL, nullptr, nullptr,
                           PAGE_EXECUTE_READ, kSecImage, hf);
        CloseHandle(hf);
        if (s != kStatusSuccess) {
            rec.status = classify_ntstatus(s);
            rec.errorcode = (uint32_t)s;
            rec.detail = "NtCreateSection(SEC_IMAGE) refused: " + error_text((uint32_t)s);
            DeleteFileW(dst.c_str());
            return;
        }
        PVOID base = nullptr;
        SIZE_T view = 0;
        s = pNtMap(sect, GetCurrentProcess(), &base, 0, 0, nullptr, &view,
                   kViewUnmap, 0, PAGE_EXECUTE_READ);
        if (s == kStatusSuccess) {
            rec.status = Status::OK;
            rec.value = "section=1;base=" + hexptr(base) +
                        ";view_bytes=" + std::to_string(view);
            rec.detail = "the kernel loaded and relocated our PE via SEC_IMAGE - "
                         "the loader can delegate relocation to it";
            pNtUnmap(GetCurrentProcess(), base);
        } else {
            rec.status = classify_ntstatus(s);
            rec.errorcode = (uint32_t)s;
            rec.detail = "section created but NtMapViewOfSection refused: " +
                         error_text((uint32_t)s);
        }
        CloseHandle(sect);
        DeleteFileW(dst.c_str());
    });

    // A6: LoadLibraryEx on an out-of-package DLL path.
    r.run("A", "loadlibrary-out-of-package", [](Record& rec) {
        std::wstring src = probe_payload_path();
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
            rec.status = Status::SKIPPED;
            rec.detail = "probe_payload.dll not shipped; cannot test out-of-package load";
            return;
        }
        std::wstring dst; DWORD cp_gle = 0;
        if (!copy_file_to_writable(src, &dst, &cp_gle)) {
            rec.status = classify_gle(cp_gle);
            rec.errorcode = cp_gle;
            rec.detail = "copy to writable path failed: " + error_text(cp_gle);
            return;
        }
        HMODULE h = LoadLibraryExW(dst.c_str(), nullptr,
                                   LOAD_WITH_ALTERED_SEARCH_PATH);
        if (h) {
            FARPROC f = GetProcAddress(h, "probe_payload_answer");
            int val = f ? ((int(*)())f)() : -1;
            rec.status = Status::OK;
            rec.value = "loaded=1;base=" + hexptr((void*)h) +
                        ";export=" + (f ? "found" : "missing") +
                        ";value=" + hex32((uint32_t)val);
            rec.detail = "non-package DLL loaded by name - path-trust is loose enough";
            FreeLibrary(h);
        } else {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "LoadLibraryExW on writable copy denied: " + error_text(gle);
        }
        DeleteFileW(dst.c_str());
    });

    // A7: LoadLibraryEx with DONT_RESOLVE_DLL_REFERENCES on an out-of-package
    // DLL - weakest form that still maps the image. If A6 was denied, maybe this
    // one isn't.
    r.run("A", "loadlibrary-dont-resolve", [](Record& rec) {
        std::wstring src = probe_payload_path();
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
            rec.status = Status::SKIPPED;
            rec.detail = "probe_payload.dll not shipped";
            return;
        }
        std::wstring dst; DWORD cp_gle = 0;
        if (!copy_file_to_writable(src, &dst, &cp_gle)) {
            rec.status = classify_gle(cp_gle);
            rec.errorcode = cp_gle;
            rec.detail = "copy failed: " + error_text(cp_gle);
            return;
        }
        HMODULE h = LoadLibraryExW(dst.c_str(), nullptr,
                                   DONT_RESOLVE_DLL_REFERENCES);
        if (h) {
            rec.status = Status::OK;
            rec.value = "loaded=1;base=" + hexptr((void*)h);
            rec.detail = "DONT_RESOLVE_DLL_REFERENCES succeeded on non-package DLL";
            FreeLibrary(h);
        } else {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "even DONT_RESOLVE_DLL_REFERENCES denied: " + error_text(gle);
        }
        DeleteFileW(dst.c_str());
    });

    // A8: Can we read a real System32 DLL for manual mapping? (A read test is
    // under category F; here we additionally check we can map it as a DATA
    // SECTION - no SEC_IMAGE - which the manual mapper would use.)
    r.run("A", "map-system-dll-as-data", [](Record& rec) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring path = std::wstring(sys) + L"\\ntdll.dll";
        HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
        if (hf == INVALID_HANDLE_VALUE) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "cannot open System32\\ntdll.dll: " + error_text(gle);
            return;
        }
        HANDLE map = CreateFileMappingW(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(hf);
        if (!map) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "CreateFileMapping(PAGE_READONLY) refused: " + error_text(gle);
            return;
        }
        void* base = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        if (base) {
            rec.status = Status::OK;
            rec.value = "mapped=1;base=" + hexptr(base) +
                        ";first4=" + hex32(*(uint32_t*)base);
            rec.detail = "System32\\ntdll.dll mapped as DATA - manual-map path is open";
            UnmapViewOfFile(base);
        } else {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "MapViewOfFile denied: " + error_text(gle);
        }
        CloseHandle(map);
    });

    // A9: Thread-stack-local JIT - alloc, flip, execute from a thread we spawn.
    // Confirms execution is a per-page property, not a thread-origin property.
    r.run("A", "exec-from-worker-thread", [](Record& rec) {
        struct Ctx { int result; } ctx{0};
        auto thunk = (LPTHREAD_START_ROUTINE)nullptr;
        void* page = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
        if (!page) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "worker page alloc failed: " + error_text(gle);
            return;
        }
        memcpy(page, kStub, sizeof kStub);
        DWORD old = 0;
        if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old)) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "RW->RX denied for worker page: " + error_text(gle);
            VirtualFree(page, 0, MEM_RELEASE);
            return;
        }
        FlushInstructionCache(GetCurrentProcess(), page, sizeof kStub);
        thunk = reinterpret_cast<LPTHREAD_START_ROUTINE>(page);
        HANDLE th = CreateThread(nullptr, 0, thunk, &ctx, 0, nullptr);
        if (!th) {
            DWORD gle = GetLastError();
            rec.status = classify_gle(gle);
            rec.errorcode = gle;
            rec.detail = "CreateThread for JIT entry denied: " + error_text(gle);
            VirtualFree(page, 0, MEM_RELEASE);
            return;
        }
        WaitForSingleObject(th, 5000);
        DWORD ec = 0; GetExitCodeThread(th, &ec);
        CloseHandle(th);
        rec.status = (ec == (DWORD)kStubAnswer) ? Status::OK : Status::WARN;
        rec.value = "exit_code=" + hex32(ec);
        rec.detail = "spawned thread entered JITted bytes and returned";
        VirtualFree(page, 0, MEM_RELEASE);
    });
}

}  // namespace probe
