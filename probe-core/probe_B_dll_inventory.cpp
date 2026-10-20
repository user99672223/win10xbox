// probe_B_dll_inventory.cpp - category B: system DLL inventory (read-only).
//
//   1. system32-enumerate : list the real System directory (GetSystemDirectoryW),
//                           count *.dll names and non-directory entries.
//   2. load:<name>        : LoadLibraryExW each system DLL in a fixed set; record
//                           base address, MISSING, or DENIED + error.
//   3. exports:<module>   : for each module that loaded, GetProcAddress a set of
//                           key exports and record presence (1 / 0).
//
// Read-only: nothing is written and core modules (ntdll/kernel32/kernelbase) are
// never freed. Every probe is dispatched through r.run.
// Haiku
#include "probe.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <wchar.h>

namespace probe {
namespace {

// Case-insensitive test for a ".dll" suffix on a file name.
bool has_dll_suffix(const wchar_t* name) {
    const size_t n = wcslen(name);
    return n >= 4 && _wcsicmp(name + n - 4, L".dll") == 0;
}

// "0x" + lowercase hex, for base-address values.
std::string hex_addr(uintptr_t v) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), digits[v & 0xF]);
        v >>= 4;
    } while (v != 0);
    return "0x" + s;
}

// Modules passed to LoadLibraryExW (".dll" is appended). Report order.
const char* const kLoadSet[] = {
    "ntdll",    "kernelbase", "kernel32", "user32",   "gdi32",    "win32u",
    "advapi32", "ws2_32",     "ole32",    "combase",  "comctl32", "shell32",
    "msvcrt",   "ucrtbase",   "d3d11",    "d3d12",    "d3d9",     "dxgi",
    "opengl32", "xaudio2_9",  "winmm",    "gdiplus",  "dwrite",   "d2d1",
    "mfplat",   "mmdevapi",   "wintrust", "crypt32",  "bcrypt",   "ncrypt",
    "setupapi", "cfgmgr32",   "iphlpapi", "dnsapi",   "secur32",  "shcore",
};

struct ExportSpec {
    const char* module;
    std::vector<const char*> symbols;
};

const std::vector<ExportSpec>& export_specs() {
    static const std::vector<ExportSpec> specs = {
        {"ntdll", {"NtCreateSection", "NtMapViewOfSection", "NtProtectVirtualMemory",
                   "NtAllocateVirtualMemory", "NtCreateUserProcess", "LdrLoadDll",
                   "RtlCreateUserThread", "NtQuerySystemInformation"}},
        {"kernelbase", {"VirtualAlloc2", "MapViewOfFile3", "LoadLibraryExW",
                        "CreateFileW", "VirtualProtect"}},
        {"kernel32", {"CreateProcessW", "VirtualAllocEx", "CreateRemoteThread",
                      "LoadLibraryW", "GetProcAddress"}},
        {"user32", {"CreateWindowExW", "RegisterClassW", "GetMessageW"}},
        {"win32u", {"NtUserCreateWindowEx", "NtUserGetMessage"}},
        {"d3d12", {"D3D12CreateDevice", "D3D12GetDebugInterface"}},
        {"d3d11", {"D3D11CreateDevice"}},
        {"dxgi", {"CreateDXGIFactory1", "CreateDXGIFactory2"}},
        {"combase", {"RoActivateInstance", "RoGetActivationFactory", "CoCreateInstance"}},
    };
    return specs;
}

}  // namespace

void register_B_dll_inventory(Reporter& r) {
    // ---- 1. System32 enumeration -------------------------------------------
    r.run("B", "system32-enumerate", [](Record& rec) {
        const UINT need = GetSystemDirectoryW(nullptr, 0);  // chars incl. NUL
        if (need == 0) {
            rec.status = Status::DENIED;
            rec.errorcode = GetLastError();
            rec.detail = "GetSystemDirectoryW failed: " + error_text(rec.errorcode);
            return;
        }
        std::vector<wchar_t> buf(need);
        const UINT got = GetSystemDirectoryW(buf.data(), need);  // chars excl. NUL
        if (got == 0 || got >= need) {
            rec.status = Status::DENIED;
            rec.errorcode = GetLastError();
            rec.detail = "GetSystemDirectoryW returned an unexpected length";
            return;
        }
        const std::wstring dir(buf.data(), got);
        const std::string dir_utf8 = narrow(dir);

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            rec.status = Status::DENIED;
            rec.errorcode = GetLastError();
            rec.value = "path=" + dir_utf8;
            rec.detail = "DENIED: cannot list System32: " + error_text(rec.errorcode);
            return;
        }
        uint32_t total = 0;
        uint32_t dlls = 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ++total;
            if (has_dll_suffix(fd.cFileName)) ++dlls;
        } while (FindNextFileW(h, &fd));
        const DWORD tail = GetLastError();  // ERROR_NO_MORE_FILES on a clean finish
        FindClose(h);

        rec.value = "path=" + dir_utf8 + ";dll_count=" + std::to_string(dlls) +
                    ";total_files=" + std::to_string(total);
        if (tail != ERROR_NO_MORE_FILES) {
            rec.status = Status::WARN;
            rec.errorcode = tail;
            rec.detail = "partial listing: FindNextFileW stopped early: " + error_text(tail);
        } else {
            rec.status = Status::INFO;
            rec.detail = "System32 listed; dll_count = names ending in .dll (case-insensitive), "
                         "total_files = non-directory entries";
        }
    });

    // ---- 2. LoadLibraryExW sweep -------------------------------------------
    // Base handles of modules that loaded. Read by the export probes below.
    // Reporter::run executes its body synchronously, so this map is filled
    // before the export loop starts.
    std::map<std::string, HMODULE> loaded;
    for (const char* mod : kLoadSet) {
        const std::string name = mod;
        r.run("B", "load:" + name, [&loaded, name](Record& rec) {
            const std::wstring file = widen(name + ".dll");
            const HMODULE h = LoadLibraryExW(file.c_str(), nullptr, 0);
            const DWORD gle = GetLastError();
            if (h) {
                loaded[name] = h;
                rec.status = Status::OK;
                rec.value = "base=" + hex_addr(reinterpret_cast<uintptr_t>(h));
                rec.detail = "loaded " + name + ".dll";
            } else if (gle == ERROR_MOD_NOT_FOUND) {
                rec.status = Status::MISSING;
                rec.errorcode = gle;
                rec.detail = name + ".dll not present (ERROR_MOD_NOT_FOUND)";
            } else {
                rec.status = Status::DENIED;
                rec.errorcode = gle;
                rec.detail = error_text(gle);
            }
        });
    }

    // ---- 3. Export presence on modules that loaded -------------------------
    for (const ExportSpec& spec : export_specs()) {
        const std::string mod = spec.module;
        if (loaded.find(mod) == loaded.end()) continue;  // load:<mod> already records why
        r.run("B", "exports:" + mod, [&loaded, mod, spec](Record& rec) {
            const HMODULE h = loaded.at(mod);
            std::string value;
            size_t present = 0;
            for (size_t i = 0; i < spec.symbols.size(); ++i) {
                const bool found = GetProcAddress(h, spec.symbols[i]) != nullptr;
                if (found) ++present;
                if (i != 0) value += ',';
                value += spec.symbols[i];
                value += found ? "=1" : "=0";
            }
            rec.status = Status::INFO;
            rec.value = value;
            rec.detail = std::to_string(present) + "/" + std::to_string(spec.symbols.size()) +
                         " present";
        });
    }
}

}  // namespace probe
