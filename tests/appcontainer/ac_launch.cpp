// ac_launch.cpp - run one program inside a GENUINE AppContainer and record what the platform did.
//
//   ac_launch [options] -- <image> [args...]
//
// WHY THIS EXISTS. The two worst bugs of this project were AppContainer-specific and could only be seen on the Xbox:
// GetFinalPathNameByHandleW is refused there (the root cause of the kernel32/kernelbase forwarder bug), and dbghelp.dll
// could not be bound (so no crash.dmp). CI has covered the first only by SIMULATION (WIN10XBOX_TEST_NO_HOST_PATHS=1,
// core/dispatch/nt_file.cpp). This program puts the real thing on a windows-latest runner: a real AppContainer token, a
// real sandbox boundary. It installs no package. An AppContainer is a token that carries a package SID, and CreateProcess
// builds that token when it is given PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES; that works on Server SKUs too.
//
// WHY A C++ PROGRAM AND NOT POWERSHELL. The job is Win32 structures and handle plumbing: SECURITY_CAPABILITIES, a
// PROC_THREAD_ATTRIBUTE_LIST, an inheritable log handle, ACL editing, a job object. In PowerShell every one of them is an
// Add-Type P/Invoke declaration, which is this same C++ inside a here-string with worse error reporting. And this program
// reads the TOKEN back from the child while it is still suspended, so a launch that silently produced an ordinary process
// can never be mistaken for a container run (the provenance block below; the process is killed if it does not match).
// The same binary also runs the CONTROL (--plain: identical plumbing, ACLs untouched, no container), so the container is
// the only difference between a control run and a container run.
//
// WHAT IT DOES, in order. Every step prints what it did and the Win32 error text of whatever failed: a CI log must be
// enough to diagnose a failure without a second run.
//   1. profile   CreateAppContainerProfile, or DeriveAppContainerSidFromAppContainerName when it already exists. The SID is
//                a hash of the name, so it is the same on every run.
//   2. grants    The container's SID is given the file system access it needs, and ONLY that (see --grant-rx and
//                --grant-modify). Without a grant the run fails for uninteresting reasons: an AppContainer needs an ACE for
//                its SID (or for ALL APPLICATION PACKAGES) on everything it touches, and process creation itself fails with
//                ACCESS_DENIED if the image is not readable by the container.
//                Every object of each granted tree is then CHECKED for the ACE and given it explicitly if inheritance did
//                not deliver it, and the counts are printed. A failed grant on the directory that was named ends the run
//                (exit status 3); a failure below it is a printed warning, because the run is still worth taking.
//   3. launch    CreateProcessW + STARTUPINFOEX carrying the SECURITY_CAPABILITIES (container SID + capabilities), the
//                handle list (stdin = NUL, stdout = stderr = the log file, opened by this program: a container process
//                may use a handle it inherited to a file it could not open itself), suspended, detached from any console.
//                CreateProcessAsUser is not used: the container token is derived from the caller's token by the kernel, and
//                CreateProcessAsUser would only add a need for SeAssignPrimaryTokenPrivilege.
//   4. provenance The child's token is read while it is suspended and printed: TokenIsAppContainer, the container SID,
//                less-privileged flag, integrity level, capabilities, privileges.
//   5. wait      With a timeout; the child runs in a kill-on-close job object, so nothing outlives this program.
//   The child's exit code goes to --rc (decimal) and to the log of this program (hex and decoded).
//
// OPTIONS
//   --name <profile>      AppContainer profile name (default win10xbox.ci.ac)
//   --cap <name>          capability to put in the token, repeatable. The SID is derived with DeriveCapabilitySidsFromName
//                         (internetClient, internetClientServer, privateNetworkClientServer, codeGeneration, ...)
//   --lpac                also opt out of ALL APPLICATION PACKAGES: a Less Privileged AppContainer (PROC_THREAD_ATTRIBUTE_
//                         ALL_APPLICATION_PACKAGES_POLICY). Stricter than a store UWP app; only a data point.
//   --plain               CONTROL: same plumbing, no container, no ACL changes
//   --inherit-console     do not detach the child from the console (default: DETACHED_PROCESS, i.e. ConsoleHandle NULL, which
//                         is what a UWP app has). Only for a control that must see the console situation of a shell.
//   --grant-rx <dir>      container SID: read + execute on <dir>, inherited by everything below it
//   --grant-modify <dir>  container SID: modify on <dir>, inherited, AND a Low-integrity no-write-up label on it. An
//                         AppContainer runs at Low integrity, and the default label of a file is Medium, so without the
//                         label mandatory integrity control could block writes the DACL allows.
//   --log <file>          the child's stdout and stderr (one handle, like "> file 2>&1"); created by this program
//   --rc <file>           the child's exit code, decimal
//   --env NAME=VALUE      set a variable for the child (repeatable); everything else is inherited from this program
//   --cwd <dir>           the child's current directory (default: the directory of --log)
//   --timeout <seconds>   default 120
//   --delete-profile      delete the profile when the run is over (default: keep it; CI runners are thrown away)
//
// EXIT STATUS of this program: 0 the child ran and exited (its code is in --rc); 1 the child did not finish in time and
// was killed; 2 bad arguments; 3 profile/grants failed; 4 CreateProcess failed; 5 the child's token is not what was asked.
//
// By hand, on any Windows 10+ machine (the same thing host-win32-appcontainer.yml does, with the probe):
//   cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 ac_launch.cpp /link advapi32.lib
//   cl /nologo /O2 /MT /std:c11 /utf-8 ac_probe.c /link advapi32.lib
//   md C:\acwork
//   ac_launch --grant-rx %CD% --grant-modify C:\acwork --log C:\acwork\probe.log -- %CD%\ac_probe.exe C:\acwork C:\acwork %CD%\ac_launch.cpp
//   type C:\acwork\probe.log        (compare with the same command plus --plain: that is the control)
//
// Build (CI, MSVC):  cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /utf-8 ac_launch.cpp /link advapi32.lib
// Build (check):     x86_64-w64-mingw32-clang++ -std=c++17 -municode -static ac_launch.cpp -o ac_launch.exe -ladvapi32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
// The container attributes of CreateProcess are declared for Windows 10 and later only (MSVC's SDK defaults to that;
// mingw's headers do not, and CMake sets the same value for the rest of the tree).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------------------------------------------------

void Say(const char* fmt, ...) {
    char buf[2400];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof buf) - 3) n = static_cast<int>(sizeof buf) - 3;
    buf[n] = '\0';
    std::fputs("ac_launch: ", stdout);
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::string Hex32(unsigned long v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", v);
    return b;
}

// "5 (0x00000005) Access is denied."
std::string Win32Text(DWORD e) {
    char head[48];
    std::snprintf(head, sizeof head, "error %lu (0x%08lX)", static_cast<unsigned long>(e), static_cast<unsigned long>(e));
    std::string s = head;
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, e, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    if (n != 0 && msg != nullptr) {
        std::wstring m(msg, n);
        while (!m.empty() && (m.back() == L'\r' || m.back() == L'\n' || m.back() == L' ' || m.back() == L'.')) m.pop_back();
        s += " ";
        s += Narrow(m);
    }
    if (msg != nullptr) LocalFree(msg);
    return s;
}

// An HRESULT; a Win32 error wrapped in one is decoded.
std::string HrText(HRESULT hr) {
    const unsigned long u = static_cast<unsigned long>(hr);
    if ((u >> 16) == 0x8007) return Hex32(u) + " = " + Win32Text(u & 0xFFFF);
    return Hex32(u);
}

// The NTSTATUS-like exit codes a crashed or refused process leaves behind.
const char* ExitCodeName(DWORD c) {
    switch (c) {
        case 0xC0000005: return "STATUS_ACCESS_VIOLATION";
        case 0xC0000022: return "STATUS_ACCESS_DENIED";
        case 0xC000007B: return "STATUS_INVALID_IMAGE_FORMAT";
        case 0xC0000135: return "STATUS_DLL_NOT_FOUND (a dependent DLL is missing or not readable by this security context)";
        case 0xC0000139: return "STATUS_ENTRYPOINT_NOT_FOUND";
        case 0xC0000142: return "STATUS_DLL_INIT_FAILED (a DLL's DllMain failed while the process started)";
        case 0xC000013A: return "STATUS_CONTROL_C_EXIT";
        case 0xC000001D: return "STATUS_ILLEGAL_INSTRUCTION";
        case 0xC00000FD: return "STATUS_STACK_OVERFLOW";
        case 0xC000000D: return "STATUS_INVALID_PARAMETER";
        case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (fail-fast)";
        case 0x80000003: return "STATUS_BREAKPOINT";
        default: return "";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------------------------------------------------

struct Grant {
    std::wstring path;
    bool modify;   // else read+execute
};

struct Options {
    std::wstring name = L"win10xbox.ci.ac";
    std::vector<std::wstring> caps;
    bool plain = false;
    bool lpac = false;
    bool inherit_console = false;
    bool delete_profile = false;
    std::vector<Grant> grants;
    std::wstring log, rc, cwd;
    std::vector<std::wstring> env;
    DWORD timeout_s = 120;
    std::vector<std::wstring> cmd;
};

void Usage() {
    std::fputs(
        "usage: ac_launch [--name <profile>] [--cap <name>]... [--lpac | --plain] [--grant-rx <dir>]... [--grant-modify <dir>]...\n"
        "                 [--log <file>] [--rc <file>] [--env NAME=VALUE]... [--cwd <dir>] [--timeout <s>] [--inherit-console]\n"
        "                 [--delete-profile]\n"
        "                 -- <image> [args...]\n",
        stderr);
}

std::wstring AbsPath(const std::wstring& p) {
    std::wstring q = p;
    for (auto& c : q) if (c == L'/') c = L'\\';
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(q.c_str(), MAX_PATH * 4, buf, nullptr);
    if (n == 0 || n >= MAX_PATH * 4) return q;
    std::wstring s(buf, n);
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

bool ParseArgs(int argc, wchar_t** argv, Options* o, std::string* err) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](const wchar_t** out) {
            if (i + 1 >= argc) { *err = Narrow(a) + " needs a value"; return false; }
            *out = argv[++i];
            return true;
        };
        const wchar_t* v = nullptr;
        if (a == L"--") {
            for (++i; i < argc; ++i) o->cmd.push_back(argv[i]);
            break;
        }
        if (a == L"--plain") o->plain = true;
        else if (a == L"--lpac") o->lpac = true;
        else if (a == L"--inherit-console") o->inherit_console = true;
        else if (a == L"--delete-profile") o->delete_profile = true;
        else if (a == L"--name") { if (!value(&v)) return false; o->name = v; }
        else if (a == L"--cap") { if (!value(&v)) return false; o->caps.push_back(v); }
        else if (a == L"--grant-rx") { if (!value(&v)) return false; o->grants.push_back({AbsPath(v), false}); }
        else if (a == L"--grant-modify") { if (!value(&v)) return false; o->grants.push_back({AbsPath(v), true}); }
        else if (a == L"--log") { if (!value(&v)) return false; o->log = AbsPath(v); }
        else if (a == L"--rc") { if (!value(&v)) return false; o->rc = AbsPath(v); }
        else if (a == L"--cwd") { if (!value(&v)) return false; o->cwd = AbsPath(v); }
        else if (a == L"--env") { if (!value(&v)) return false; o->env.push_back(v); }
        else if (a == L"--timeout") { if (!value(&v)) return false; o->timeout_s = static_cast<DWORD>(_wtoi(v)); }
        else { *err = "unknown option " + Narrow(a); return false; }
    }
    if (o->cmd.empty()) { *err = "no program after --"; return false; }
    if (o->plain && o->lpac) { *err = "--plain and --lpac exclude each other"; return false; }
    o->cmd[0] = AbsPath(o->cmd[0]);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The AppContainer API. Resolved at run time: the import library of DeriveCapabilitySidsFromName differs between SDKs,
// and a missing export then becomes a readable message instead of a link error that costs a CI round trip. --plain never
// touches any of it, which is what lets the plumbing be tried under Wine.
// ---------------------------------------------------------------------------------------------------------------------

struct AcApi {
    HRESULT (WINAPI* CreateProfile)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID*) = nullptr;
    HRESULT (WINAPI* DeriveSid)(PCWSTR, PSID*) = nullptr;
    HRESULT (WINAPI* DeleteProfile)(PCWSTR) = nullptr;
    BOOL (WINAPI* DeriveCapSids)(PCWSTR, PSID**, DWORD*, PSID**, DWORD*) = nullptr;
};

FARPROC FindExport(const char* name, const wchar_t* const* dlls, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        HMODULE m = LoadLibraryW(dlls[i]);
        if (m == nullptr) continue;
        if (FARPROC p = GetProcAddress(m, name)) return p;
    }
    return nullptr;
}

bool LoadAcApi(AcApi* api) {
    static const wchar_t* const kUserenv[] = {L"userenv.dll"};
    static const wchar_t* const kCaps[] = {L"kernel32.dll", L"kernelbase.dll", L"advapi32.dll"};
    api->CreateProfile = reinterpret_cast<decltype(api->CreateProfile)>(
        reinterpret_cast<void*>(FindExport("CreateAppContainerProfile", kUserenv, 1)));
    api->DeriveSid = reinterpret_cast<decltype(api->DeriveSid)>(
        reinterpret_cast<void*>(FindExport("DeriveAppContainerSidFromAppContainerName", kUserenv, 1)));
    api->DeleteProfile = reinterpret_cast<decltype(api->DeleteProfile)>(
        reinterpret_cast<void*>(FindExport("DeleteAppContainerProfile", kUserenv, 1)));
    api->DeriveCapSids = reinterpret_cast<decltype(api->DeriveCapSids)>(
        reinterpret_cast<void*>(FindExport("DeriveCapabilitySidsFromName", kCaps, 3)));
    bool ok = true;
    if (!api->CreateProfile) { Say("FATAL: userenv.dll has no CreateAppContainerProfile (this Windows has no AppContainer support)"); ok = false; }
    if (!api->DeriveSid) { Say("FATAL: userenv.dll has no DeriveAppContainerSidFromAppContainerName"); ok = false; }
    if (!api->DeleteProfile) Say("note: userenv.dll has no DeleteAppContainerProfile (--delete-profile will do nothing)");
    if (!api->DeriveCapSids) Say("note: no DeriveCapabilitySidsFromName in kernel32/kernelbase/advapi32 (--cap cannot be used)");
    return ok;
}

std::string SidText(PSID sid) {
    if (sid == nullptr) return "(null)";
    LPWSTR s = nullptr;
    if (!ConvertSidToStringSidW(sid, &s) || s == nullptr) return "(unprintable)";
    std::string r = Narrow(s);
    LocalFree(s);
    return r;
}

// A SID kept in memory of our own.
struct OwnedSid {
    std::vector<BYTE> bytes;
    PSID get() { return bytes.empty() ? nullptr : reinterpret_cast<PSID>(bytes.data()); }
};

OwnedSid CopyOf(PSID sid) {
    OwnedSid o;
    const DWORD n = GetLengthSid(sid);
    o.bytes.resize(n);
    CopySid(n, o.get(), sid);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// Grants
// ---------------------------------------------------------------------------------------------------------------------

constexpr DWORD kReadExecute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;                                       // icacls (RX)
constexpr DWORD kModify = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;              // icacls (M)
constexpr DWORD kInheritBoth = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
constexpr size_t kMaxObjects = 50000;
constexpr int kMaxDepth = 32;

struct Tally {
    size_t objects = 0;       // files and directories visited
    size_t had = 0;           // already carried an ACE covering the access (inheritance delivered it)
    size_t added = 0;         // needed an explicit ACE
    size_t failed = 0;        // the ACE could not be added
    size_t labelled = 0;      // Low label set explicitly (modify grants only)
    size_t label_ok = 0;      // already Low or lower
    size_t label_failed = 0;
    size_t skipped_reparse = 0;
    bool capped = false;
    bool root_failed = false;   // the DACL grant on the object the caller named failed: nothing below it can be trusted
};

// Does `dacl` hold ALLOW ACEs for `sid` that together cover every bit of `need`?
bool DaclCovers(PACL dacl, PSID sid, DWORD need) {
    if (dacl == nullptr) return true;   // a NULL DACL grants everybody everything
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof info, AclSizeInformation)) return false;
    DWORD have = 0;
    for (DWORD i = 0; i < info.AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace) || ace == nullptr) continue;
        const auto* hdr = static_cast<ACE_HEADER*>(ace);
        if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        if (hdr->AceFlags & INHERIT_ONLY_ACE) continue;   // applies to what is created below, not to this object
        auto* a = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if (EqualSid(reinterpret_cast<PSID>(&a->SidStart), sid)) have |= a->Mask;
    }
    return (have & need) == need;
}

DWORD AddAllowAce(const std::wstring& path, PSID sid, DWORD mask, DWORD inherit) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL old_dacl = nullptr;
    DWORD e = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl,
                                    nullptr, &sd);
    if (e != ERROR_SUCCESS) return e;
    EXPLICIT_ACCESS_W ea{};
    ea.grfAccessPermissions = mask;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = inherit;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    PACL new_dacl = nullptr;
    e = SetEntriesInAclW(1, &ea, old_dacl, &new_dacl);
    if (e == ERROR_SUCCESS)
        e = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                  nullptr, new_dacl, nullptr);
    if (new_dacl != nullptr) LocalFree(new_dacl);
    LocalFree(sd);
    return e;
}

// The RID of the mandatory label on `path`, or -1 when it has none (which means Medium).
long LabelRid(const std::wstring& path) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL sacl = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, &sacl,
                              &sd) != ERROR_SUCCESS)
        return -2;
    long rid = -1;
    if (sacl != nullptr) {
        ACL_SIZE_INFORMATION info{};
        if (GetAclInformation(sacl, &info, sizeof info, AclSizeInformation)) {
            for (DWORD i = 0; i < info.AceCount; ++i) {
                void* ace = nullptr;
                if (!GetAce(sacl, i, &ace) || ace == nullptr) continue;
                if (static_cast<ACE_HEADER*>(ace)->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
                PSID s = reinterpret_cast<PSID>(&static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace)->SidStart);
                rid = static_cast<long>(*GetSidSubAuthority(s, static_cast<DWORD>(*GetSidSubAuthorityCount(s)) - 1));
            }
        }
    }
    LocalFree(sd);
    return rid;
}

// Low integrity, no-write-up. A directory's label is inherited by what is created in it later.
DWORD SetLowLabel(const std::wstring& path, bool dir) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(dir ? L"S:(ML;OICI;NW;;;LW)" : L"S:(ML;;NW;;;LW)",
                                                              SDDL_REVISION_1, &sd, nullptr))
        return GetLastError();
    BOOL present = FALSE, defaulted = FALSE;
    PACL sacl = nullptr;
    DWORD e = ERROR_INVALID_DATA;
    if (GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted) && present && sacl != nullptr)
        e = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr,
                                  nullptr, nullptr, sacl);
    LocalFree(sd);
    return e;
}

void NoteFailure(const Tally& t, const std::wstring& path, const char* what, DWORD e) {
    if (t.failed + t.label_failed <= 5)
        Say("  could not %s %s: %s", what, Narrow(path).c_str(), Win32Text(e).c_str());
}

void Ensure(const std::wstring& path, bool dir, PSID sid, DWORD mask, bool label, Tally* t, int depth) {
    if (t->objects >= kMaxObjects) { t->capped = true; return; }
    ++t->objects;

    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    const DWORD re = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                           nullptr, &sd);
    const bool covered = re == ERROR_SUCCESS && DaclCovers(dacl, sid, mask);
    if (sd != nullptr) LocalFree(sd);
    if (covered) {
        ++t->had;
    } else {
        const DWORD e = AddAllowAce(path, sid, mask, dir ? kInheritBoth : NO_INHERITANCE);
        if (e == ERROR_SUCCESS) { ++t->added; }
        else { ++t->failed; if (depth == 0) t->root_failed = true; NoteFailure(*t, path, "grant on", e); }
    }

    if (label) {
        const long rid = LabelRid(path);
        if (rid >= 0 && rid <= SECURITY_MANDATORY_LOW_RID) {
            ++t->label_ok;
        } else {
            const DWORD e = SetLowLabel(path, dir);
            if (e == ERROR_SUCCESS) { ++t->labelled; }
            else { ++t->label_failed; NoteFailure(*t, path, "label", e); }
        }
    }

    if (!dir || depth >= kMaxDepth) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++t->skipped_reparse; continue; }   // never leave the tree
        Ensure(path + L"\\" + name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, sid, mask, label, t, depth + 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool ApplyGrant(const Grant& g, PSID sid) {
    const DWORD attrs = GetFileAttributesW(g.path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        Say("FATAL: grant target %s does not exist: %s", Narrow(g.path).c_str(), Win32Text(GetLastError()).c_str());
        return false;
    }
    const bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const DWORD mask = g.modify ? kModify : kReadExecute;
    Tally t;
    Ensure(g.path, dir, sid, mask, g.modify, &t, 0);
    Say("grant %-6s mask 0x%08lX on %s%s", g.modify ? "modify" : "rx", static_cast<unsigned long>(mask), Narrow(g.path).c_str(),
        dir ? " (inherited by everything below)" : "");
    Say("  objects=%zu  already-had-the-ACE=%zu  ACE-added-explicitly=%zu  failed=%zu%s%s", t.objects, t.had, t.added, t.failed,
        t.skipped_reparse ? "  (reparse points skipped)" : "", t.capped ? "  (object cap reached)" : "");
    if (g.modify)
        Say("  Low-integrity no-write-up label: already=%zu set=%zu failed=%zu", t.label_ok, t.labelled, t.label_failed);
    // Only a failed grant on the named root is fatal. A child that could not be granted, or a label that could not be set, is
    // reported and the run goes on: the log then says what to suspect if the container run fails for a reason of its own.
    if (t.failed != 0 || t.label_failed != 0)
        Say("  WARNING: %zu grant(s) and %zu label(s) failed below %s; continuing, but a failure of the run may be about this, not the platform",
            t.failed - (t.root_failed ? 1 : 0), t.label_failed, Narrow(g.path).c_str());
    return !t.root_failed;
}

// ---------------------------------------------------------------------------------------------------------------------
// The token of a process
// ---------------------------------------------------------------------------------------------------------------------

struct TokenFacts {
    bool read = false;
    bool is_appcontainer = false;
    bool lpac = false;
    bool lpac_known = false;   // the less-privileged flag could be queried (Windows 10 1703 and later)
    std::string ac_sid;
    unsigned long integrity = 0;
    std::vector<std::string> caps;
    std::string privileges;
    std::string user;
    bool elevated = false;
};

template <typename T>
bool QueryToken(HANDLE tok, TOKEN_INFORMATION_CLASS cls, std::vector<BYTE>* buf, T** out) {
    DWORD need = 0;
    GetTokenInformation(tok, cls, nullptr, 0, &need);
    if (need == 0) return false;
    buf->assign(need + 8, 0);
    if (!GetTokenInformation(tok, cls, buf->data(), need, &need)) return false;
    *out = reinterpret_cast<T*>(buf->data());
    return true;
}

const char* IntegrityName(unsigned long rid) {
    if (rid >= SECURITY_MANDATORY_PROTECTED_PROCESS_RID) return "protected";
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID) return "System";
    if (rid >= SECURITY_MANDATORY_HIGH_RID) return "High";
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID) return "Medium";
    if (rid >= SECURITY_MANDATORY_LOW_RID) return "Low";
    return "Untrusted";
}

TokenFacts ReadTokenFacts(HANDLE process) {
    TokenFacts f;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &tok)) {
        f.ac_sid = "OpenProcessToken failed: " + Win32Text(GetLastError());
        return f;
    }
    f.read = true;
    DWORD v = 0, n = 0;
    if (GetTokenInformation(tok, static_cast<TOKEN_INFORMATION_CLASS>(TokenIsAppContainer), &v, sizeof v, &n))
        f.is_appcontainer = v != 0;
    v = 0;
    // TokenIsLessPrivilegedAppContainer = 46 (spelled as a number so that an older SDK still compiles it)
    if (GetTokenInformation(tok, static_cast<TOKEN_INFORMATION_CLASS>(46), &v, sizeof v, &n)) { f.lpac = v != 0; f.lpac_known = true; }
    std::vector<BYTE> b;
    if (TOKEN_APPCONTAINER_INFORMATION* ac = nullptr; QueryToken(tok, TokenAppContainerSid, &b, &ac))
        f.ac_sid = SidText(ac->TokenAppContainer);
    if (TOKEN_MANDATORY_LABEL* il = nullptr; QueryToken(tok, TokenIntegrityLevel, &b, &il))
        f.integrity = *GetSidSubAuthority(il->Label.Sid, static_cast<DWORD>(*GetSidSubAuthorityCount(il->Label.Sid)) - 1);
    if (TOKEN_GROUPS* g = nullptr; QueryToken(tok, TokenCapabilities, &b, &g))
        for (DWORD i = 0; i < g->GroupCount; ++i) f.caps.push_back(SidText(g->Groups[i].Sid));
    if (TOKEN_PRIVILEGES* p = nullptr; QueryToken(tok, TokenPrivileges, &b, &p)) {
        for (DWORD i = 0; i < p->PrivilegeCount; ++i) {
            char name[128];
            DWORD cch = sizeof name;
            if (!LookupPrivilegeNameA(nullptr, &p->Privileges[i].Luid, name, &cch)) std::snprintf(name, sizeof name, "luid-%lu", p->Privileges[i].Luid.LowPart);
            if (!f.privileges.empty()) f.privileges += ", ";
            f.privileges += name;
            f.privileges += (p->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) ? "(on)" : "(off)";
        }
        if (f.privileges.empty()) f.privileges = "(none)";
    }
    if (TOKEN_USER* u = nullptr; QueryToken(tok, TokenUser, &b, &u)) f.user = SidText(u->User.Sid);
    TOKEN_ELEVATION el{};
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof el, &n)) f.elevated = el.TokenIsElevated != 0;
    CloseHandle(tok);
    return f;
}

void PrintTokenFacts(const char* who, const TokenFacts& f) {
    if (!f.read) { Say("%s token: %s", who, f.ac_sid.c_str()); return; }
    Say("%s token: AppContainer=%d  lessPrivilegedAppContainer=%d  user=%s  elevated=%d", who, f.is_appcontainer ? 1 : 0,
        f.lpac ? 1 : 0, f.user.c_str(), f.elevated ? 1 : 0);
    Say("%s token: integrity=0x%04lX (%s)  appcontainer-sid=%s", who, f.integrity, IntegrityName(f.integrity),
        f.ac_sid.empty() ? "(none)" : f.ac_sid.c_str());
    std::string caps;
    for (const auto& c : f.caps) caps += (caps.empty() ? "" : " ") + c;
    Say("%s token: capabilities(%zu)=%s", who, f.caps.size(), caps.empty() ? "(none)" : caps.c_str());
    Say("%s token: privileges=%s", who, f.privileges.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------
// The child's command line and environment
// ---------------------------------------------------------------------------------------------------------------------

std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

std::wstring EnvName(const std::wstring& entry) {
    const size_t eq = entry.find(L'=', 1);   // a leading '=' belongs to the name ("=C:=C:\x")
    return eq == std::wstring::npos ? entry : entry.substr(0, eq);
}

bool EnvNameLess(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(EnvName(a).c_str(), -1, EnvName(b).c_str(), -1, TRUE) == CSTR_LESS_THAN;
}

// This program's environment with the --env overrides applied, sorted the way the system keeps it. Double-NUL terminated.
std::vector<wchar_t> BuildEnvironment(const std::vector<std::wstring>& overrides) {
    std::vector<std::wstring> entries;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p != L'\0'; p += wcslen(p) + 1) entries.emplace_back(p);
        FreeEnvironmentStringsW(block);
    }
    for (const auto& o : overrides) {
        bool replaced = false;
        for (auto& e : entries) {
            if (CompareStringOrdinal(EnvName(e).c_str(), -1, EnvName(o).c_str(), -1, TRUE) == CSTR_EQUAL) { e = o; replaced = true; break; }
        }
        if (!replaced) entries.push_back(o);
    }
    std::sort(entries.begin(), entries.end(), EnvNameLess);
    std::vector<wchar_t> flat;
    for (const auto& e : entries) { flat.insert(flat.end(), e.begin(), e.end()); flat.push_back(L'\0'); }
    flat.push_back(L'\0');
    if (flat.size() == 1) flat.push_back(L'\0');
    return flat;
}

std::wstring DirOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// ---------------------------------------------------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------------------------------------------------

struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~HandleGuard() { if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h); }
};

HANDLE OpenInheritable(const wchar_t* path, DWORD access, DWORD share, DWORD disposition) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    return CreateFileW(path, access, share, &sa, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Say("note: cannot write %s: %s", Narrow(path).c_str(), Win32Text(GetLastError()).c_str()); return; }
    DWORD w = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &w, nullptr);
    CloseHandle(h);
}

int Run(const Options& o) {
    // A crashing child must not raise a dialog nobody can click, and the child inherits this mode.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    const char* mode = o.plain ? "NONE (control: --plain)" : (o.lpac ? "LESS-PRIVILEGED AppContainer (LPAC)" : "AppContainer");
    Say("=== %s ===", Narrow(o.cmd[0]).c_str());
    Say("container: %s; console: %s", mode, o.inherit_console ? "inherited from this program" : "none (DETACHED_PROCESS)");
    {
        OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof vi;
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
            if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion"))))
                if (fn(&vi) == 0) Say("os: Windows %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    }
    PrintTokenFacts("launcher", ReadTokenFacts(GetCurrentProcess()));

    // ---- 1. profile and capabilities ------------------------------------------------------------------------------
    AcApi api;
    OwnedSid ac_sid;
    std::vector<OwnedSid> cap_sids;
    std::vector<SID_AND_ATTRIBUTES> cap_attrs;
    if (!o.plain) {
        if (!LoadAcApi(&api)) return 3;
        PSID raw = nullptr;
        HRESULT hr = api.CreateProfile(o.name.c_str(), L"win10xbox CI", L"AppContainer used by the win10xbox CI to measure the platform",
                                       nullptr, 0, &raw);
        if (SUCCEEDED(hr)) {
            Say("profile '%s' created", Narrow(o.name).c_str());
        } else if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            hr = api.DeriveSid(o.name.c_str(), &raw);
            if (SUCCEEDED(hr)) Say("profile '%s' already exists; its SID is derived from the name", Narrow(o.name).c_str());
        }
        if (FAILED(hr) || raw == nullptr) {
            Say("FATAL: no AppContainer profile '%s': %s", Narrow(o.name).c_str(), HrText(hr).c_str());
            return 3;
        }
        ac_sid = CopyOf(raw);
        FreeSid(raw);
        Say("AppContainer SID: %s", SidText(ac_sid.get()).c_str());

        for (const auto& name : o.caps) {
            if (!api.DeriveCapSids) { Say("FATAL: --cap %s needs DeriveCapabilitySidsFromName, which this Windows lacks", Narrow(name).c_str()); return 3; }
            PSID* groups = nullptr;
            PSID* sids = nullptr;
            DWORD ngroups = 0, nsids = 0;
            if (!api.DeriveCapSids(name.c_str(), &groups, &ngroups, &sids, &nsids) || nsids == 0) {
                Say("FATAL: cannot derive the capability SID of '%s': %s", Narrow(name).c_str(), Win32Text(GetLastError()).c_str());
                return 3;
            }
            cap_sids.push_back(CopyOf(sids[0]));
            Say("capability %-28s -> %s", Narrow(name).c_str(), SidText(cap_sids.back().get()).c_str());
            for (DWORD i = 0; i < ngroups; ++i) LocalFree(groups[i]);
            for (DWORD i = 0; i < nsids; ++i) LocalFree(sids[i]);
            LocalFree(groups);
            LocalFree(sids);
        }
        for (auto& c : cap_sids) {
            SID_AND_ATTRIBUTES sa{};
            sa.Sid = c.get();
            sa.Attributes = SE_GROUP_ENABLED;
            cap_attrs.push_back(sa);
        }

        // ---- 2. grants ---------------------------------------------------------------------------------------------
        Say("grants to %s (nothing else is granted):", SidText(ac_sid.get()).c_str());
        bool granted = true;
        for (const auto& g : o.grants) granted = ApplyGrant(g, ac_sid.get()) && granted;
        if (o.grants.empty()) Say("  (none: the container will only see what ALL APPLICATION PACKAGES can see)");
        if (!granted) { Say("FATAL: a grant failed; the run would fail for reasons that say nothing about the platform"); return 3; }
    } else {
        Say("--plain: no profile, no ACL changes, no container");
    }

    // ---- 3. handles, environment, attribute list -------------------------------------------------------------------
    HandleGuard nul, log;
    nul.h = OpenInheritable(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING);
    if (nul.h == INVALID_HANDLE_VALUE) { Say("FATAL: cannot open NUL: %s", Win32Text(GetLastError()).c_str()); return 4; }
    if (!o.log.empty()) {
        log.h = OpenInheritable(o.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS);
        if (log.h == INVALID_HANDLE_VALUE) { Say("FATAL: cannot create the log %s: %s", Narrow(o.log).c_str(), Win32Text(GetLastError()).c_str()); return 4; }
    } else {
        log.h = OpenInheritable(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING);
    }
    std::vector<wchar_t> env = BuildEnvironment(o.env);
    for (const auto& e : o.env) Say("env for the child: %s", Narrow(e).c_str());

    std::wstring cmdline = QuoteArg(o.cmd[0]);
    for (size_t i = 1; i < o.cmd.size(); ++i) cmdline += L" " + QuoteArg(o.cmd[i]);
    std::wstring cwd = !o.cwd.empty() ? o.cwd : (!o.log.empty() ? DirOf(o.log) : DirOf(o.cmd[0]));
    Say("command line: %s", Narrow(cmdline).c_str());
    Say("current directory: %s", Narrow(cwd).c_str());
    Say("stdout+stderr -> %s", o.log.empty() ? "NUL" : Narrow(o.log).c_str());

    HANDLE inherit[2] = {nul.h, log.h};
    SECURITY_CAPABILITIES sc{};
    if (!o.plain) {
        sc.AppContainerSid = ac_sid.get();
        sc.Capabilities = cap_attrs.empty() ? nullptr : cap_attrs.data();
        sc.CapabilityCount = static_cast<DWORD>(cap_attrs.size());
    }
    DWORD all_app_packages_policy = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;

    const DWORD attr_count = 1 + (o.plain ? 0 : 1) + (o.lpac ? 1 : 0);
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, attr_count, 0, &attr_size);   // fails by design: it reports the size
    std::vector<BYTE> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (attr_size == 0 || !InitializeProcThreadAttributeList(attrs, attr_count, 0, &attr_size)) {
        Say("FATAL: InitializeProcThreadAttributeList: %s", Win32Text(GetLastError()).c_str());
        return 4;
    }
    bool attrs_ok = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr) != FALSE;
    if (attrs_ok && !o.plain)
        attrs_ok = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &sc, sizeof sc, nullptr, nullptr) != FALSE;
    if (attrs_ok && o.lpac)
        attrs_ok = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY, &all_app_packages_policy,
                                             sizeof all_app_packages_policy, nullptr, nullptr) != FALSE;
    if (!attrs_ok) {
        Say("FATAL: UpdateProcThreadAttribute: %s", Win32Text(GetLastError()).c_str());
        DeleteProcThreadAttributeList(attrs);
        return 4;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul.h;
    si.StartupInfo.hStdOutput = log.h;
    si.StartupInfo.hStdError = log.h;
    si.lpAttributeList = attrs;

    // DETACHED_PROCESS: no console, so the child's PEB has ConsoleHandle NULL, exactly like a UWP app. It gets its standard
    // handles from STARTUPINFO, like "> file 2>&1", and no conhost is involved, which a container may not be allowed to
    // attach to. (host-win32 copies the host's ConsoleHandle into the guest's process parameters, core/dispatch/process.cpp,
    // so this is a real condition of the run and not just plumbing: --inherit-console exists to tell the two apart.)
    // Suspended: the token is checked before the first instruction runs.
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                        (o.inherit_console ? 0 : DETACHED_PROCESS);
    std::vector<wchar_t> cmd_mut(cmdline.begin(), cmdline.end());
    cmd_mut.push_back(L'\0');
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(o.cmd[0].c_str(), cmd_mut.data(), nullptr, nullptr, TRUE, flags, env.data(),
                                        cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    const DWORD create_error = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (!created) {
        Say("FATAL: CreateProcessW failed: %s", Win32Text(create_error).c_str());
        if (create_error == ERROR_ACCESS_DENIED && !o.plain)
            Say("  (a container cannot start an image it may not read: is %s covered by a --grant-rx?)", Narrow(DirOf(o.cmd[0])).c_str());
        return 4;
    }
    HandleGuard proc, thread;
    proc.h = pi.hProcess;
    thread.h = pi.hThread;
    Say("created process %lu (suspended)", static_cast<unsigned long>(pi.dwProcessId));

    // ---- 4. provenance ---------------------------------------------------------------------------------------------
    const TokenFacts child = ReadTokenFacts(pi.hProcess);
    PrintTokenFacts("child", child);
    bool genuine = true;
    if (!o.plain) {
        const std::string want = SidText(ac_sid.get());
        // The less-privileged flag is only compared when this Windows can report it.
        genuine = child.read && child.is_appcontainer && child.ac_sid == want && (child.lpac == o.lpac || !child.lpac_known);
        if (genuine) Say("PROVENANCE OK: the child runs in AppContainer %s%s", want.c_str(), o.lpac ? " (less privileged)" : "");
        else Say("PROVENANCE FAILURE: the child is NOT in the requested %s (AppContainer=%d sid=%s, wanted %s); killing it: a measurement taken here would not be a container measurement",
                 o.lpac ? "LPAC" : "AppContainer", child.is_appcontainer ? 1 : 0, child.ac_sid.c_str(), want.c_str());
    } else {
        Say("PROVENANCE: control run, the child is %s", child.is_appcontainer ? "unexpectedly an AppContainer" : "an ordinary process (AppContainer=0), as intended");
    }
    if (!genuine) {
        TerminateProcess(pi.hProcess, 0xAC0005);
        if (!o.rc.empty()) WriteTextFile(o.rc, "provenance-failure\n");
        return 5;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) || !AssignProcessToJobObject(job, pi.hProcess))
            Say("note: the child is not in a kill-on-close job (%s); the timeout still terminates it", Win32Text(GetLastError()).c_str());
    }

    // ---- 5. run and wait -------------------------------------------------------------------------------------------
    const ULONGLONG t0 = GetTickCount64();
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        Say("FATAL: ResumeThread: %s", Win32Text(GetLastError()).c_str());
        TerminateProcess(pi.hProcess, 0xAC0004);
        return 4;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, o.timeout_s * 1000);
    const double secs = static_cast<double>(GetTickCount64() - t0) / 1000.0;
    int result = 0;
    if (waited == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        const char* name = ExitCodeName(code);
        Say("RESULT exit code %lu (0x%08lX)%s%s after %.1f s", static_cast<unsigned long>(code), static_cast<unsigned long>(code),
            *name ? " = " : "", name, secs);
        if (!o.rc.empty()) WriteTextFile(o.rc, std::to_string(static_cast<unsigned long>(code)) + "\n");
    } else {
        Say("RESULT TIMEOUT: the child was still running after %lu s; terminating it", static_cast<unsigned long>(o.timeout_s));
        TerminateProcess(pi.hProcess, 0xAC0001);
        WaitForSingleObject(pi.hProcess, 10000);
        if (!o.rc.empty()) WriteTextFile(o.rc, "timeout\n");
        result = 1;
    }
    if (job != nullptr) CloseHandle(job);

    // The log is the child's; its size says whether anything was written at all.
    if (!o.log.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(o.log.c_str(), GetFileExInfoStandard, &fa))
            Say("log: %s, %llu bytes", Narrow(o.log).c_str(),
                (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow);
    }
    if (o.delete_profile && !o.plain && api.DeleteProfile) {
        const HRESULT hr = api.DeleteProfile(o.name.c_str());
        Say("profile '%s' deleted: %s", Narrow(o.name).c_str(), SUCCEEDED(hr) ? "yes" : HrText(hr).c_str());
    }
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options o;
    std::string err;
    if (!ParseArgs(argc, argv, &o, &err)) {
        std::fprintf(stderr, "ac_launch: %s\n", err.c_str());
        Usage();
        return 2;
    }
    return Run(o);
}
