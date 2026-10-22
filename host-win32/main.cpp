// win10xbox console host.
//   win10xbox [--machine <dir>] [--debug <spec>] [--cwd <guestdir>] [--version] <program.exe> [args...]
//
// <program.exe> is a guest path ("C:\guest\app.exe"), or a host path that lies
// under the machine folder (it is untranslated to the guest view), or any other
// host path: then its directory is mapped as drive Z: for this run (only
// without the VFS module; with it, use machine.json "drives").
// Without --machine the machine folder is <directory of win10xbox.exe>\machine.
// The process exit code is the guest's.
//
// MSVC builds also carry crash capture (crash.cpp, see CMakeLists.txt). It leaves crash.txt (annotated exception
// records: stack words, registers, the guest's module list), crash.dmp (a minidump) and entry-reached.txt in
// %WIN10XBOX_CRASH_DIR% (default: the directory of win10xbox.exe, else %TEMP%). A guest run also starts with a few
// "host-win32:" lines on stderr: that directory, and the load base and size of the host executable and of every
// other module of this process. crash.txt's own module list is the GUEST's (Wine's ntdll replaces PEB->Ldr), so an
// address in the host image is attributed with those lines and win10xbox.map (the linker map), not with crash.txt.
#include "win10xbox/core.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/trace.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>
#if defined(WIN10XBOX_HOST_CRASH_CAPTURE)
#include <tlhelp32.h>
#include "crash.h"
#endif

namespace {

void Usage() {
    std::fputs("usage: win10xbox [--machine <dir>] [--debug <spec>] [--cwd <guestdir>] [--version] <program.exe> [args...]\n"
               "  program: guest path (C:\\dir\\app.exe), or a host path; see docs/DISPATCHER.md\n",
               stderr);
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, buf, nullptr);
    std::wstring s = (n && n < MAX_PATH * 2) ? std::wstring(buf, n) : p;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

bool IStartsWith(const std::wstring& s, const std::wstring& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::towlower(s[i]) != std::towlower(prefix[i])) return false;
    return true;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

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

// Resolves the program argument to a guest Win32 path. Empty on failure
// (message already printed).
std::wstring ResolveProgram(const std::wstring& arg, const std::wstring& machine) {
    std::wstring a = arg;
    for (auto& c : a) if (c == L'/') c = L'\\';
    const bool drive_form = a.size() > 2 && a[1] == L':' && a[2] == L'\\';
    // 1. Guest C: path whose file exists in the machine folder.
    if (drive_form && std::towupper(a[0]) == L'C' && FileExists(machine + a.substr(2))) return a;
    // 2. Host path under the machine folder -> guest C:.
    std::wstring host = FullPath(a);
    if (IStartsWith(host, machine + L"\\") && FileExists(host)) return L"C:" + host.substr(machine.size());
    // 3. Any other existing host file: map its directory as Z:.
    if (FileExists(host)) {
        size_t slash = host.find_last_of(L'\\');
        std::wstring dir = host.substr(0, slash), name = host.substr(slash + 1);
        if (win10xbox::process::MapHostDirectory(L'Z', dir)) return L"Z:\\" + name;
        std::fprintf(stderr,
                     "win10xbox: %ls is outside the machine folder %ls and this build cannot map extra drives at "
                     "run time; add it to machine.json \"drives\" or copy it under the machine folder\n",
                     host.c_str(), machine.c_str());
        return {};
    }
    // 4. Guest path on another drive: let the VFS decide.
    if (drive_form) return a;
    std::fprintf(stderr, "win10xbox: %ls not found (neither a guest path under %ls nor a host file)\n", arg.c_str(),
                 machine.c_str());
    return {};
}

#if defined(WIN10XBOX_HOST_CRASH_CAPTURE)

// ---------------------------------------------------------------------------
// Crash capture. crash.cpp / crash.h are byte-identical copies of host-uwp-lite's (the namespace is still
// uwp_lite so the copies stay diffable); everything specific to this console host is below.
// ---------------------------------------------------------------------------

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// Every host-side line this file prints on stderr (the core's own trace goes there too). The gate step of
// host-win32-smoke.yml greps for the prefix; never print the strings it counts ("guest entry reached", ...).
void HostLog(const std::string& line) { std::fprintf(stderr, "host-win32: %s\n", line.c_str()); }

bool EnsureDir(const std::wstring& dir, int depth = 0) {
    if (dir.empty() || depth > 32) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) EnsureDir(dir.substr(0, slash), depth + 1);
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Can a file be created in `dir`? (A read-only install directory, say.)
bool DirWritable(const std::wstring& dir) {
    const std::wstring probe = dir + L"\\crash-probe-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Where crash.txt, crash.dmp and entry-reached.txt go: %WIN10XBOX_CRASH_DIR% (created if need be), else the
// directory of win10xbox.exe, else %TEMP%. crash.cpp does not create directories and drops what it cannot write,
// so a directory that is not writable is skipped here, and `notes` collects why (logged once a guest runs).
std::wstring CrashDirectory(std::vector<std::string>* notes) {
    wchar_t env[MAX_PATH * 2];
    const DWORD en = GetEnvironmentVariableW(L"WIN10XBOX_CRASH_DIR", env, MAX_PATH * 2);
    if (en != 0 && en < MAX_PATH * 2) {
        const std::wstring dir = FullPath(std::wstring(env, en));
        if (EnsureDir(dir) && DirWritable(dir)) return dir;
        notes->push_back("WIN10XBOX_CRASH_DIR=" + Narrow(dir) + " is not a writable directory; using the default");
    }
    std::wstring exe_dir;
    wchar_t exe[MAX_PATH * 2];
    const DWORD xn = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    if (xn != 0 && xn < MAX_PATH * 2) {
        const std::wstring path(exe, xn);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) exe_dir = path.substr(0, slash);
    }
    if (!exe_dir.empty() && DirWritable(exe_dir)) return exe_dir;
    wchar_t tmp[MAX_PATH * 2];
    const DWORD tn = GetTempPathW(MAX_PATH * 2, tmp);
    if (tn != 0 && tn < MAX_PATH * 2) {
        const std::wstring dir = FullPath(std::wstring(tmp, tn));
        if (DirWritable(dir)) {
            notes->push_back("the directory of win10xbox.exe is not writable; crash files go to " + Narrow(dir));
            return dir;
        }
    }
    notes->push_back("no writable directory for the crash files (tried WIN10XBOX_CRASH_DIR, the directory of "
                     "win10xbox.exe, %TEMP%): crash.txt and crash.dmp will not be written");
    // Never an empty path: crash.cpp would build "\crash.txt", the root of the current drive.
    return exe_dir.empty() ? FullPath(L".") : exe_dir;
}

// Every line the core's trace emits comes through here (the core prints it to stderr itself): the one that
// announces the guest's entry writes entry-reached.txt. Runs under the trace mutex, on any thread.
void CrashTraceSink(const char* line, void*) { uwp_lite::CrashOnTraceLine(line); }

// That marker is logged on the core's `modules` channel, and a --debug / WIN10XBOX_DEBUG spec that leaves the channel
// off would silently turn it off. So +modules is appended to whatever the spec is (a few dozen lines per run).
// Returns what the variable is now, for the log.
std::string EnsureModulesChannel() {
    wchar_t cur[1024];
    const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_DEBUG", cur, 1024);
    if (n >= 1024) return "WIN10XBOX_DEBUG is too long for the core to read; +modules was not added";
    std::wstring spec = n != 0 ? std::wstring(cur, n) : std::wstring();
    spec += spec.empty() ? L"+modules" : L",+modules";
    SetEnvironmentVariableW(L"WIN10XBOX_DEBUG", spec.c_str());
    return "WIN10XBOX_DEBUG=" + Narrow(spec) + " (+modules is always added: the guest-entry marker rides on that channel)";
}

// The host executable itself: load base, SizeOfImage, the preferred base the linker map is relative to, and the PE
// timestamp (the map's "Timestamp is" line must show the same value). Read from the image's own headers, so it does
// not depend on any loader list. RVA = address - base; the map's Rva+Base column = preferred + RVA.
std::string DescribeHostImage() {
    const HMODULE self = GetModuleHandleW(nullptr);
    wchar_t path[MAX_PATH * 2] = L"";
    GetModuleFileNameW(self, path, MAX_PATH * 2);
    const BYTE* base = reinterpret_cast<const BYTE*>(self);
    unsigned long long size = 0, preferred = 0;
    unsigned long stamp = 0;
    if (base != nullptr) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                size = nt->OptionalHeader.SizeOfImage;
                preferred = nt->OptionalHeader.ImageBase;
                stamp = nt->FileHeader.TimeDateStamp;
            }
        }
    }
    char buf[1536];
    std::snprintf(buf, sizeof buf, "host-image base=0x%016llx size=0x%08llx preferred=0x%016llx timestamp=0x%08lx path=%s",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(base)), size, preferred, stamp,
                  Narrow(path).c_str());
    return buf;
}

// Every module of THIS process with its base and size. Once the guest's ntdll has run, the PEB's loader lists
// describe the guest (that is the module list crash.txt prints), so an address inside the host executable or one of
// the host's DLLs shows up in crash.txt only as a bare memory region. This table is taken before the guest launches
// and is what attributes such an address.
std::vector<std::string> DescribeAllHostModules() {
    std::vector<std::string> lines;
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5; ++attempt) {   // ERROR_BAD_LENGTH: the module list changed under the snapshot
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snap == INVALID_HANDLE_VALUE) {
        lines.push_back("host-module snapshot failed, error " + std::to_string(GetLastError()));
        return lines;
    }
    MODULEENTRY32W e;
    e.dwSize = sizeof e;
    for (BOOL ok = Module32FirstW(snap, &e); ok; ok = Module32NextW(snap, &e)) {
        char buf[1536];
        std::snprintf(buf, sizeof buf, "host-module base=0x%016llx size=0x%08lx path=%s",
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(e.modBaseAddr)),
                      static_cast<unsigned long>(e.modBaseSize), Narrow(e.szExePath).c_str());
        lines.push_back(buf);
    }
    CloseHandle(snap);
    return lines;
}

// Installed first thing in wmain, so everything after it (Initialize, Launch, the guest) is covered.
class HostCrash {
public:
    HostCrash() {
        const win10xbox::Version v = win10xbox::GetVersion();
        uwp_lite::CrashConfig cc;
        cc.local_state = CrashDirectory(&notes_);
        cc.build = "host-win32, core " + std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
                   std::to_string(v.patch) + " (" + (v.git ? v.git : "?") + ")";
        // cc.result_path stays empty: there is no result.txt in this host (the exit code is the result).
        dir_ = cc.local_state;
        // trace_log is this process's stderr, i.e. the job's captured output: crash.cpp appends one summary line
        // per fatal-looking exception to it with raw WriteFile (the full record goes to crash.txt). The handle is
        // borrowed: crash.cpp never closes it and neither do we.
        state_ = uwp_lite::InstallCrashCapture(cc, GetStdHandle(STD_ERROR_HANDLE));
    }
    // The borrowed stderr handle is not used past wmain.
    ~HostCrash() { uwp_lite::DetachCrashTraceLog(); }
    HostCrash(const HostCrash&) = delete;
    HostCrash& operator=(const HostCrash&) = delete;

    // A guest will run: after the arguments are parsed, before Initialize (which reads WIN10XBOX_DEBUG).
    void BeforeInitialize() {
        const std::string spec = EnsureModulesChannel();
        win10xbox::trace::AddSink(&CrashTraceSink, nullptr);
        for (const std::string& n : notes_) HostLog(n);
        HostLog("crash.txt, crash.dmp and entry-reached.txt go to " + Narrow(dir_));
        HostLog(state_);
        HostLog(spec);
        for (const std::string& line : uwp_lite::DescribeHostModules()) HostLog(line);
        HostLog(DescribeHostImage());
    }
    // Right before process::Launch: from then on the PEB's loader lists belong to the guest.
    void BeforeLaunch() {
        for (const std::string& line : DescribeAllHostModules()) HostLog(line);
    }

private:
    std::vector<std::string> notes_;
    std::wstring dir_;
    std::string state_;
};

#else  // llvm-mingw (ci.yml linux-cross): crash.cpp is MSVC-only (__try, delayimp.lib), nothing to do

class HostCrash {
public:
    void BeforeInitialize() {}
    void BeforeLaunch() {}
};

#endif  // WIN10XBOX_HOST_CRASH_CAPTURE

}  // namespace

int wmain(int argc, wchar_t** argv) {
    win10xbox::config::Config cfg;
    std::vector<std::wstring> rest;
    std::wstring cwd;
    bool version_only = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--machine" && i + 1 < argc) cfg.machine_root = argv[++i];
        else if (a == L"--debug" && i + 1 < argc) SetEnvironmentVariableW(L"WIN10XBOX_DEBUG", argv[++i]);
        else if (a == L"--cwd" && i + 1 < argc) cwd = argv[++i];
        else if (a == L"--version") version_only = true;
        else if (a == L"--help" || a == L"-h") { Usage(); return 0; }
        else { for (; i < argc; ++i) rest.emplace_back(argv[i]); }
    }
    auto v = win10xbox::GetVersion();
    if (version_only) { std::printf("win10xbox %d.%d.%d (%s)\n", v.major, v.minor, v.patch, v.git); return 0; }
    if (cfg.machine_root.empty()) {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring exe(buf);
        cfg.machine_root = exe.substr(0, exe.find_last_of(L"\\/")) + L"\\machine";
    }
    cfg.machine_root = FullPath(cfg.machine_root);
    if (int rc = win10xbox::Initialize(cfg)) { std::fprintf(stderr, "initialise failed: %d\n", rc); return 2; }
    if (rest.empty()) {
        std::printf("win10xbox %d.%d.%d (%s): hello from host-win32\n", v.major, v.minor, v.patch, v.git);
        win10xbox::Shutdown();
        return 0;
    }

    win10xbox::process::LaunchOptions opts;
    opts.image_path = ResolveProgram(rest[0], cfg.machine_root);
    if (opts.image_path.empty()) { win10xbox::Shutdown(); return 3; }
    opts.command_line = QuoteArg(opts.image_path);
    for (size_t i = 1; i < rest.size(); ++i) opts.command_line += L" " + QuoteArg(rest[i]);
    opts.current_directory = cwd;
    opts.run_on_new_thread = true;
    opts.wait = true;
    std::fflush(stdout);
    int rc = win10xbox::process::Launch(opts);
    if (rc < 0) std::fprintf(stderr, "win10xbox: could not start %ls: status %08x\n", opts.image_path.c_str(), rc);
    win10xbox::dispatch::Shutdown();
    win10xbox::Shutdown();
    return rc;
}
