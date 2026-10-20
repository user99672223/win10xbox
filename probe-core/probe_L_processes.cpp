// probe_L_processes.cpp - category L: process creation / multi-instance.
//
// Big architectural fork: can the compat layer spawn helper processes at all?
// An AppContainer typically forbids CreateProcess of arbitrary binaries, but a
// UWP package CAN request multi-instance activation and launch extra instances
// of its OWN package through the AppModel APIs. This module probes:
//   - CreateProcess of a benign Microsoft binary (cmd / notepad / dllhost).
//   - CreateProcess of our own exe path (same-package self-spawn).
//   - Resolution of multi-instance activation APIs (CoreApplication /
//     AppInstance.RecommendedInstance, etc.) without actually launching another
//     instance - that's a UWP-only runtime effect we only confirm is reachable.
//   - NtCreateUserProcess resolution (direct syscall path a loader might use).
//
// Non-destructive: any child process we successfully launch is immediately
// terminated with TerminateProcess + handle close; we never let one keep
// running.
#include "probe.h"
#include <cstdio>

namespace probe {

namespace {

std::string hex32(uint32_t v) {
    char b[16]; std::snprintf(b, sizeof b, "0x%08X", v); return b;
}

Status classify_gle(DWORD gle) {
    if (gle == ERROR_ACCESS_DENIED) return Status::DENIED;
    if (gle == ERROR_NOT_SUPPORTED || gle == ERROR_CALL_NOT_IMPLEMENTED) return Status::NOTIMPL;
    if (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND ||
        gle == ERROR_MOD_NOT_FOUND) return Status::MISSING;
    return Status::WARN;
}

std::wstring exe_path() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

// Launch a process by command line, record outcome, kill it immediately on
// success. Keeps probes idempotent and non-destructive.
void try_launch(Record& rec, const std::wstring& cmdline) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // Buffer must be writable (CreateProcessW requirement).
    std::wstring buf = cmdline;
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                             nullptr, &si, &pi);
    if (!ok) {
        DWORD gle = GetLastError();
        rec.status = classify_gle(gle);
        rec.errorcode = gle;
        rec.detail = "CreateProcessW denied: " + error_text(gle);
        return;
    }
    rec.status = Status::OK;
    rec.value = "created=1;pid=" + std::to_string(pi.dwProcessId);
    rec.detail = "child process created suspended, then terminated";
    // Immediate termination - no code of the child runs beyond the loader stub
    // because we spawned CREATE_SUSPENDED.
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

std::wstring sys_path(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\" + name;
}

}  // namespace

void register_L_processes(Reporter& r) {
    // L1: self-spawn - launch another copy of the host exe (CREATE_SUSPENDED)
    // and kill it. This is the pattern a multi-instance package would use.
    r.run("L", "createprocess-self", [](Record& rec) {
        std::wstring self = exe_path();
        // --probe-child flag ignored by the host (not acted on) - just a marker
        // so a human inspecting a process tree sees intent.
        try_launch(rec, L"\"" + self + L"\" --probe-child");
    });

    // L2: launch cmd.exe from System32 - usually DENIED under AppContainer
    // without broadFileSystemAccess, but confirmed in baseline.
    r.run("L", "createprocess-cmd", [](Record& rec) {
        try_launch(rec, L"\"" + sys_path(L"cmd.exe") + L"\" /c exit");
    });

    // L3: launch dllhost.exe - the system COM surrogate. More likely to be
    // reachable from a sandbox than cmd.exe in some policies.
    r.run("L", "createprocess-dllhost", [](Record& rec) {
        try_launch(rec, L"\"" + sys_path(L"dllhost.exe") + L"\" /?");
    });

    // L4: launch notepad.exe - usually a UX app blocked from sandboxes.
    r.run("L", "createprocess-notepad", [](Record& rec) {
        try_launch(rec, L"\"" + sys_path(L"notepad.exe") + L"\"");
    });

    // L5: ShellExecuteExW - the API UWP typically exposes to launch an
    // associated handler. Resolve dynamically; we test with a benign file:
    // scheme URL that won't actually do anything if blocked. On UWP this is
    // usually gated through Launcher class, which we don't invoke here (that's
    // category N).
    r.run("L", "shellexecute-resolve", [](Record& rec) {
        void* p = resolve("shell32.dll", "ShellExecuteExW");
        if (!p) {
            rec.status = Status::MISSING;
            rec.detail = "ShellExecuteExW not exported";
            return;
        }
        rec.status = Status::OK;
        rec.value = "addr=" + std::string("resolved");
        rec.detail = "ShellExecuteExW resolvable; not invoked (would side-effect)";
    });

    // L6: NtCreateUserProcess resolution - the native syscall path a manual
    // loader might call directly. We only resolve, never call (invoking it
    // requires a thoroughly populated RTL_USER_PROCESS_PARAMETERS and is
    // non-trivially destructive).
    r.run("L", "nt-createuserprocess-resolve", [](Record& rec) {
        void* p = resolve("ntdll.dll", "NtCreateUserProcess");
        if (!p) {
            rec.status = Status::MISSING;
            rec.detail = "NtCreateUserProcess not exported";
            return;
        }
        rec.status = Status::OK;
        rec.detail = "NtCreateUserProcess resolvable from ntdll";
    });

    // L7: CreateProcessAsUser / CreateProcessWithTokenW - privileged variants.
    // Only resolve; calling them without proper tokens is a non-starter.
    r.run("L", "createprocess-variants-resolve", [](Record& rec) {
        struct E { const char* mod; const char* sym; } apis[] = {
            {"advapi32.dll", "CreateProcessAsUserW"},
            {"advapi32.dll", "CreateProcessWithLogonW"},
            {"advapi32.dll", "CreateProcessWithTokenW"},
            {"kernelbase.dll", "CreateProcessW"},
        };
        std::string v;
        int n_present = 0;
        for (const auto& a : apis) {
            void* p = resolve(a.mod, a.sym);
            v += std::string(a.sym) + "=" + (p ? "1" : "0") + ";";
            if (p) ++n_present;
        }
        rec.status = (n_present > 0) ? Status::OK : Status::MISSING;
        rec.value = v;
        rec.detail = std::to_string(n_present) + "/" +
                     std::to_string((int)(sizeof apis / sizeof apis[0])) +
                     " process-creation variants resolvable";
    });

    // L8: AppModel / multi-instance activation surface. Resolve these from
    // kernel32/kernelbase; we do NOT invoke them (they only have meaning inside
    // a packaged app's lifecycle and could change activation state).
    r.run("L", "appmodel-resolve", [](Record& rec) {
        struct E { const char* mod; const char* sym; } apis[] = {
            {"kernel32.dll", "GetCurrentPackageFullName"},
            {"kernel32.dll", "GetCurrentPackageId"},
            {"kernel32.dll", "GetPackageFullName"},
            {"kernelbase.dll", "AppPolicyGetWindowingModel"},
            {"api-ms-win-core-processenvironment-l1-1-0.dll", "GetCurrentDirectoryW"},
        };
        std::string v;
        int present = 0;
        for (const auto& a : apis) {
            void* p = resolve(a.mod, a.sym);
            v += std::string(a.sym) + "=" + (p ? "1" : "0") + ";";
            if (p) ++present;
        }
        rec.status = (present > 0) ? Status::OK : Status::MISSING;
        rec.value = v;
        rec.detail = "AppModel surface resolution";
    });

    // L9: GetCurrentPackageFullName - what package IS this process? (Returns
    // APPMODEL_ERROR_NO_PACKAGE=15700 on desktop baseline, a package name on
    // UWP. Already a strong environment signal.)
    r.run("L", "package-identity", [](Record& rec) {
        using Fn = LONG(WINAPI*)(UINT32*, PWSTR);
        auto p = (Fn)resolve("kernel32.dll", "GetCurrentPackageFullName");
        if (!p) {
            rec.status = Status::MISSING;
            rec.detail = "GetCurrentPackageFullName not exported";
            return;
        }
        UINT32 cc = 0;
        LONG rv = p(&cc, nullptr);
        if (rv == 15700 /*APPMODEL_ERROR_NO_PACKAGE*/) {
            rec.status = Status::INFO;
            rec.errorcode = (uint32_t)rv;
            rec.value = "has_package=0";
            rec.detail = "process has no package identity (desktop baseline)";
            return;
        }
        if (rv == 122 /*ERROR_INSUFFICIENT_BUFFER*/ && cc > 0) {
            std::wstring name(cc, L'\0');
            rv = p(&cc, name.data());
            if (rv == 0) {
                while (!name.empty() && name.back() == L'\0') name.pop_back();
                rec.status = Status::OK;
                rec.value = "pfn=" + narrow(name);
                rec.detail = "running inside a packaged app";
                return;
            }
        }
        rec.status = Status::WARN;
        rec.errorcode = (uint32_t)rv;
        rec.detail = "unexpected return from GetCurrentPackageFullName: " +
                     error_text((uint32_t)rv);
    });
}

}  // namespace probe
