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
// HOW THE PROCESS ENDS. Once the layer is shut down this host has nothing left to clean up, so it ends with
// TerminateProcess(GetCurrentProcess(), exit code) (EndProcess below) instead of returning into the CRT. The normal
// way out (return from wmain -> exit() -> ExitProcess -> ntdll's LdrShutdownProcess) first terminates every other thread
// and then runs DllMain(DLL_PROCESS_DETACH) of every DLL in the process, and on the first real-Windows run that got
// that far (host-win32-smoke on 532a3bf) that is where it died: combase.dll, which only dbghelp.dll pulls in, raised
// STATUS_INVALID_PARAMETER from inside ntdll while detaching, the unhandled exception replaced the guest's exit code
// (Git Bash showed 127 instead of 42), and the crash capture then waited 180 s for a crash thread that ExitProcess had
// already killed. The guest's result is final the moment Launch returns; nothing the DLLs do at detach may change it.
// WIN10XBOX_NORMAL_EXIT=1 restores the normal way out, for diagnosing exactly that (host-win32-smoke runs it once).
//
// MSVC builds also carry crash capture (crash.cpp, see CMakeLists.txt). It leaves crash.txt (annotated exception
// records: stack words, registers, module tables), crash.dmp (a minidump) and entry-reached.txt in
// %WIN10XBOX_CRASH_DIR% (default: the directory of win10xbox.exe, else %TEMP%). A guest run also starts with a few
// "host-win32:" lines on stderr: that directory, the host image's load base / size / preferred base / timestamp (the
// last two tie an address to win10xbox.map, the linker map) and the base and size of every host module. The module
// list of a crash record is the GUEST's (Wine's ntdll replaces PEB->Ldr), so crash.cpp also saves the host's own
// list before that happens and attributes addresses against both.
#include "win10xbox/core.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/gamepad.h"
#include "win10xbox/trace.h"
#include "win10xbox/user.h"
#include "win10xbox/vfs.h"
#include "winexbox/driver.h"
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <set>
#include <string>
#include <vector>
#if defined(WIN10XBOX_HOST_CRASH_CAPTURE)
#include "crash.h"
#endif

namespace {

void Usage() {
    std::fputs("usage: win10xbox [--machine <dir>] [--debug <spec>] [--cwd <guestdir>] [--version] <program.exe> [args...]\n"
               "  program: guest path (C:\\dir\\app.exe), or a host path; see docs/DISPATCHER.md\n"
               "  (set by the layer when a guest starts a child: --cmdline <raw guest command line>, --guest-env)\n",
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
    // 1b. A guest path the VFS serves from elsewhere in the machine folder: C:\Windows\System32\cmd.exe is
    // Windows\builtin\cmd.exe. It must not fall through to step 3, which would run the host's own file of that name.
    if (drive_form) {
        std::wstring served;
        // The VFS is brought up by process::Launch, after this; it is cheap to do twice (Launch replaces the state).
        if (win10xbox::vfs::Initialize(machine) == 0 && win10xbox::vfs::TranslateWin32Path(a, &served) == 0 &&
            FileExists(served))
            return a;
    }
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
        HostLog(DescribeHostImage());
    }
    // Right before process::Launch: from then on the PEB's loader lists belong to the guest. DescribeHostModules()
    // records the host's module paths for crash.txt, refreshes crash.cpp's table of the host's modules (base, size,
    // path: the table every crash record carries) and returns it as lines, which are logged here as well.
    void BeforeLaunch() {
        for (const std::string& line : uwp_lite::DescribeHostModules()) HostLog(line);
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

// ---------------------------------------------------------------------------
// How the process ends, and the test switches that exercise it (see the header comment).
// ---------------------------------------------------------------------------

bool EnvFlag(const wchar_t* name) {
    wchar_t v[8];
    const DWORD n = GetEnvironmentVariableW(name, v, 8);
    return n != 0 && n < 8 && v[0] != L'0';
}

std::wstring g_shot_path;   // WIN10XBOX_SCREENSHOT, for the inject script's `shot`
bool g_guest_ran = false;   // a guest was launched: EndProcess says how the process ends (stderr; stdout stays the guest's)

// Leaves without running ExitProcess's DLL detach. TerminateProcess on the current process ends every thread at once and
// does not return; the exit code is exactly `rc`, the same value the CRT would have handed to ExitProcess. C stdio is
// flushed by hand first (the CRT's exit() would have done it): stdout may be a pipe or a file that a test harness reads.
[[noreturn]] void EndProcess(int rc) {
    std::fflush(nullptr);
    if (g_guest_ran)
        std::fprintf(stderr, "host-win32: guest exit code %d (0x%08x); ending the process with TerminateProcess, so the DLL "
                             "detach of ExitProcess cannot change it\n", rc, static_cast<unsigned>(rc));
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
    ExitProcess(static_cast<UINT>(rc));   // not reached: TerminateProcess does not return for the current process
}

// WIN10XBOX_TEST_EXIT_PROBE_DLL=<path>: load a DLL whose DllMain(DLL_PROCESS_DETACH) ends the process with exit code 99
// (tests/host_exit/exit_probe.c). It stands in for any system DLL whose detach misbehaves at exit: with EndProcess it
// never runs and the exit code stays the guest's; with WIN10XBOX_NORMAL_EXIT=1 it does run and the code is 99.
void LoadExitProbe() {
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_TEST_EXIT_PROBE_DLL", path, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return;
    HMODULE h = LoadLibraryW(path);
    std::fprintf(stderr, "host-win32: test: exit probe %ls %s\n", path, h ? "loaded" : "NOT loaded");
}

// WIN10XBOX_TEST_HOST_ACTCTX=1 (Wine hosts only): plants a marker in the host's PEB.ActivationContextData before the guest
// launches and checks it afterwards. Wine's loader_init overwrites that field in the guest, so this fails if the take-over
// stops restoring it. Wine only because a Microsoft ntdll would try to interpret the marker as a context.
class ActctxProbe {
public:
    ActctxProbe() {
        if (!EnvFlag(L"WIN10XBOX_TEST_HOST_ACTCTX")) return;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) {
            std::fputs("host-win32: test: PEB.ActivationContextData check skipped (not a Wine host)\n", stderr);
            return;
        }
        field_ = reinterpret_cast<void**>(static_cast<std::uintptr_t>(__readgsqword(0x60)) + kPebActivationContextData);
        original_ = *field_;
        planted_ = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 256);   // readable and zero: nobody's context magic
        *field_ = planted_;
        std::fprintf(stderr, "host-win32: test: PEB.ActivationContextData set to %p (was %p)\n", planted_, original_);
    }
    // After Launch has returned: the PEB is the host's again.
    void Check() {
        if (!field_) return;
        if (*field_ == planted_)
            std::fputs("host-win32: test: PEB.ActivationContextData intact after the guest\n", stderr);
        else
            std::fprintf(stderr, "host-win32: test: PEB.ActivationContextData CLOBBERED by the guest: %p, expected %p\n",
                         *field_, planted_);
        *field_ = original_;   // leave the host as it was found
    }

private:
    static constexpr std::uintptr_t kPebActivationContextData = 0x2F8;   // x64, wine-src/include/winternl.h
    void** field_ = nullptr;
    void* planted_ = nullptr;
    void* original_ = nullptr;
};

// WIN10XBOX_TEST_INJECT_INPUT=<script>: a scripted user for the GUI tests (tests/guest/window_input.c). Commands are
// separated by ';': sleep N (ms), move X,Y (screen coordinates), ldown / lup / rdown / rup (at the last moved position),
// wheel D (WHEEL_DELTA units), waitwin N (until a window is painted, at most N ms), waitflush F N (until the guest has flushed F frames, at most N ms), shot [FILE] (screenshot now + distinct
// colour count; FILE is an ASCII path, default WIN10XBOX_SCREENSHOT), quit N (end the process with exit code N), key 0xVK (key down, then up; the scan code is set 1 for 'A'..'Z', else 0),
// pad BUTTONS LX LY RX RY LT RT (the host's gamepad 0, which the guest's XInput reads: XINPUT_GAMEPAD_* bits in hex,
// four signed 16-bit sticks, two trigger bytes; the first one connects the pad), padwait N (until the guest has
// polled the pad, at most N ms). Each command
// logs one "host-win32: inject" line on stderr. The thread touches no guest state: the loader initialisation it runs
// on creation completes before Launch takes over the PEB, and after that it only calls the driver, whose InjectInput
// is thread-safe. It is detached and no exception leaves it.
struct InjectJob {
    winexbox::IWinexboxDriver* display = nullptr;
    std::string script;
    HANDLE loaded = nullptr;   // set by the thread once its own loader initialisation is done
    HANDLE go = nullptr;       // set by the host once Launch has started the guest
};

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// Decimal, or hex with 0x (strtol base 0); the whole string must be a number.
bool ParseInt(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtol(s.c_str(), &end, 0);
    return end != nullptr && *end == '\0';
}

std::uint16_t ScanOfVk(long vk) {
    static const std::uint16_t kLetters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                               0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    return (vk >= 'A' && vk <= 'Z') ? kLetters[vk - 'A'] : 0;
}

// Runs one command; false (nothing sent) when it is not understood.
bool RunInjectCommand(winexbox::IWinexboxDriver* display, const std::string& cmd, long& x, long& y) {
    const size_t sp = cmd.find(' ');
    const std::string verb = cmd.substr(0, sp);
    const std::string arg = sp == std::string::npos ? std::string() : Trim(cmd.substr(sp + 1));
    long a = 0, b = 0;
    bool ok = true;
    DWORD sleep_ms = 0;
    std::vector<winexbox::InputEvent> events;
    winexbox::InputEvent ev;
    if (verb == "sleep") {
        ok = ParseInt(arg, a) && a >= 0;
        sleep_ms = static_cast<DWORD>(a);
    } else if (verb == "move") {
        const size_t comma = arg.find(',');
        ok = comma != std::string::npos && ParseInt(arg.substr(0, comma), a) && ParseInt(arg.substr(comma + 1), b);
        if (ok) {
            x = a;
            y = b;
            ev.kind = winexbox::InputKind::MouseMove;
            ev.x = static_cast<std::int32_t>(x);
            ev.y = static_cast<std::int32_t>(y);
            events.push_back(ev);
        }
    } else if (verb == "ldown" || verb == "lup" || verb == "rdown" || verb == "rup") {
        ev.kind = verb.find("down") != std::string::npos ? winexbox::InputKind::MouseButtonDown
                                                         : winexbox::InputKind::MouseButtonUp;
        ev.button = verb[0] == 'l' ? winexbox::MouseButton::Left : winexbox::MouseButton::Right;
        ev.x = static_cast<std::int32_t>(x);
        ev.y = static_cast<std::int32_t>(y);
        events.push_back(ev);
    } else if (verb == "wheel") {
        ok = ParseInt(arg, a);
        ev.kind = winexbox::InputKind::MouseWheel;
        ev.wheel_delta = static_cast<std::int32_t>(a);
        events.push_back(ev);
    } else if (verb == "key") {
        ok = ParseInt(arg, a) && a >= 0 && a <= 0xFF;
        ev.vk = static_cast<std::uint16_t>(a);
        ev.scan = ScanOfVk(a);
        ev.kind = winexbox::InputKind::KeyDown;
        events.push_back(ev);
        ev.kind = winexbox::InputKind::KeyUp;
        events.push_back(ev);
    } else if (verb == "pad") {
        // pad BUTTONS LX LY RX RY LT RT: seven numbers, the first in hex (with or without 0x), the rest in C syntax.
        long v[7] = {0, 0, 0, 0, 0, 0, 0};
        size_t pos = 0;
        int n = 0;
        while (n < 7 && pos < arg.size()) {
            const size_t end = arg.find_first_of(" \t", pos);
            const std::string tok = arg.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? arg.size() : arg.find_first_not_of(" \t", end);
            if (pos == std::string::npos) pos = arg.size();
            char* stop = nullptr;
            v[n] = std::strtol(tok.c_str(), &stop, n == 0 ? 16 : 0);
            if (tok.empty() || *stop != '\0') { ok = false; break; }
            ++n;
        }
        ok = ok && n == 7 && v[0] >= 0 && v[0] <= 0xFFFF && v[1] >= -32768 && v[1] <= 32767 && v[2] >= -32768 &&
             v[2] <= 32767 && v[3] >= -32768 && v[3] <= 32767 && v[4] >= -32768 && v[4] <= 32767 && v[5] >= 0 &&
             v[5] <= 255 && v[6] >= 0 && v[6] <= 255;
        if (ok) {
            W10xPadState st{};
            st.buttons = static_cast<std::uint16_t>(v[0]);
            st.thumb_lx = static_cast<std::int16_t>(v[1]);
            st.thumb_ly = static_cast<std::int16_t>(v[2]);
            st.thumb_rx = static_cast<std::int16_t>(v[3]);
            st.thumb_ry = static_cast<std::int16_t>(v[4]);
            st.left_trigger = static_cast<std::uint8_t>(v[5]);
            st.right_trigger = static_cast<std::uint8_t>(v[6]);
            win10xbox::input::SetGamepadState(0, st);
        }
    } else if (verb == "padwait") {
        // padwait N: until the guest has read the pad (an XInputGetState that found it connected), at most N ms.
        ok = ParseInt(arg, a) && a >= 0;
        if (ok) {
            const DWORD t0 = GetTickCount();
            while (win10xbox::input::GamepadPolls() == 0 && GetTickCount() - t0 < static_cast<DWORD>(a)) Sleep(20);
            std::fprintf(stderr, "host-win32: padwait: polls=%llu after %lu ms%s\n",
                         static_cast<unsigned long long>(win10xbox::input::GamepadPolls()),
                         static_cast<unsigned long>(GetTickCount() - t0),
                         win10xbox::input::GamepadPolls() == 0 ? " (TIMEOUT: the guest never polled)" : "");
        }
    } else if (verb == "waitwin") {
        // waitwin N: until the guest has created a window and flushed pixels, at most N ms (the first top-level
        // window of a real GUI program, tests integration.realgui_*). Logs how it ended; `shot` follows regardless.
        ok = ParseInt(arg, a) && a >= 0;
        if (ok) {
            const DWORD t0 = GetTickCount();
            win10xbox::user::Stats st = win10xbox::user::GetStats();
            while ((st.windows_created == 0 || st.flushes == 0) && GetTickCount() - t0 < static_cast<DWORD>(a)) {
                Sleep(50);
                st = win10xbox::user::GetStats();
            }
            std::fprintf(stderr, "host-win32: waitwin: windows=%llu flushes=%llu paints=%llu after %lu ms%s\n",
                         static_cast<unsigned long long>(st.windows_created), static_cast<unsigned long long>(st.flushes),
                         static_cast<unsigned long long>(st.paints), static_cast<unsigned long>(GetTickCount() - t0),
                         (st.windows_created == 0 || st.flushes == 0) ? " (TIMEOUT: nothing painted)" : "");
        }
        events.clear();
        std::fprintf(stderr, "host-win32: inject %s\n", cmd.c_str());
        return ok;
    } else if (verb == "waitflush") {
        // waitflush F N: until the guest's windows have flushed F surfaces in total (a program that draws a frame per
        // tick, such as a game, is then F frames in whatever the speed of the machine), at most N ms. Logs how it ended.
        const size_t sep = arg.find(' ');
        long frames = 0, limit_ms = 0;
        ok = sep != std::string::npos && ParseInt(arg.substr(0, sep), frames) && ParseInt(Trim(arg.substr(sep + 1)), limit_ms) &&
             frames >= 0 && limit_ms >= 0;
        if (ok) {
            const DWORD t0 = GetTickCount();
            win10xbox::user::Stats st = win10xbox::user::GetStats();
            while (st.flushes < static_cast<unsigned long long>(frames) && GetTickCount() - t0 < static_cast<DWORD>(limit_ms)) {
                Sleep(10);
                st = win10xbox::user::GetStats();
            }
            std::fprintf(stderr, "host-win32: waitflush: flushes=%llu after %lu ms%s\n", static_cast<unsigned long long>(st.flushes),
                         static_cast<unsigned long>(GetTickCount() - t0),
                         st.flushes < static_cast<unsigned long long>(frames) ? " (TIMEOUT: too few frames)" : "");
        }
        events.clear();
        std::fprintf(stderr, "host-win32: inject %s\n", cmd.c_str());
        return ok;
    } else if (verb == "shot") {
        // shot [FILE]: write the last painted frame to FILE (default WIN10XBOX_SCREENSHOT) now (the guest keeps running) and log how many
        // distinct colours it holds ("shot: distinct=N"; a blank desktop is 1).
        std::int32_t w = 0, h = 0;
        win10xbox::user::ScreenSize(&w, &h);
        size_t distinct = 0;
        if (w > 0 && h > 0) {
            std::vector<std::uint32_t> px(static_cast<size_t>(w) * h);
            if (win10xbox::user::ComposeLastPaintedFrame(px.data(), w, h, w * 4)) {
                std::set<std::uint32_t> seen;
                for (std::uint32_t p : px) { seen.insert(p); if (seen.size() >= 1000) break; }
                distinct = seen.size();
            }
        }
        std::wstring shot_path = g_shot_path;
        if (!arg.empty()) shot_path.assign(arg.begin(), arg.end());   // `shot FILE`: an ASCII path
        const bool wrote = !shot_path.empty() && win10xbox::user::WriteScreenshot(shot_path);
        std::fprintf(stderr, "host-win32: shot: distinct=%zu written=%d\n", distinct, wrote ? 1 : 0);
        return true;
    } else if (verb == "quit") {
        // quit N: end the process with exit code N (a real program has no reason to exit by itself).
        ok = ParseInt(arg, a);
        if (ok) {
            std::fprintf(stderr, "host-win32: inject %s\n", cmd.c_str());
            EndProcess(static_cast<int>(a));
        }
    } else {
        ok = false;
    }
    if (!ok) {
        std::fprintf(stderr, "host-win32: inject %s: not understood, skipped\n", cmd.c_str());
        return false;
    }
    std::fprintf(stderr, "host-win32: inject %s\n", cmd.c_str());
    if (verb == "sleep") Sleep(sleep_ms);
    for (const winexbox::InputEvent& e : events) display->InjectInput(e);
    return true;
}

void RunInjectScript(winexbox::IWinexboxDriver* display, const std::string& script) {
    long x = 0, y = 0;   // the last moved position: the buttons act there
    size_t pos = 0;
    while (pos <= script.size()) {
        size_t semi = script.find(';', pos);
        if (semi == std::string::npos) semi = script.size();
        const std::string cmd = Trim(script.substr(pos, semi - pos));
        pos = semi + 1;
        if (!cmd.empty()) RunInjectCommand(display, cmd, x, y);
    }
}

DWORD WINAPI InjectThreadProc(LPVOID arg) {
    InjectJob* job = static_cast<InjectJob*>(arg);
    SetEvent(job->loaded);                    // loader initialisation of this thread is done: the take-over may go ahead
    WaitForSingleObject(job->go, INFINITE);   // Launch has started the guest
    try {
        RunInjectScript(job->display, job->script);
    } catch (...) {
        std::fputs("host-win32: inject: aborted\n", stderr);
    }
    // Never returns: a thread that exits while the guest runs makes ntdll send DLL_THREAD_DETACH to the modules on
    // PEB->Ldr, which by then is the guest's list. The process ends by TerminateProcess (EndProcess), which takes it along.
    for (;;) Sleep(INFINITE);
}

// Starts the thread (detached) and returns once it has passed its own loader initialisation, which must not overlap
// the PEB take-over that Launch does next. Returns nullptr when no thread could be started.
InjectJob* StartInjectThread(winexbox::IWinexboxDriver* display, const std::string& script) {
    InjectJob* job = new InjectJob;
    job->display = display;
    job->script = script;
    job->loaded = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    job->go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = (job->loaded && job->go) ? CreateThread(nullptr, 0, InjectThreadProc, job, 0, nullptr) : nullptr;
    if (!t) {
        std::fputs("host-win32: inject: no thread, the script is not run\n", stderr);
        return nullptr;
    }
    CloseHandle(t);   // detached
    WaitForSingleObject(job->loaded, INFINITE);
    return job;
}

}  // namespace

// Everything the host does, up to and including the layer's shutdown. Returns the process exit code.
static int HostMain(int argc, wchar_t** argv) {
    LoadExitProbe();   // test switch; loaded first so that nothing below can end the process before it is in place
    // First thing: crash.txt / crash.dmp for everything below (a no-op without crash.cpp). Nothing is printed here, so
    // --version / --help / the hello run keep their exact output (integration.host_version anchors on it).
    HostCrash crash;
    win10xbox::config::Config cfg;
    std::vector<std::wstring> rest;
    std::wstring cwd, cmdline;
    bool guest_env = false, version_only = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--machine" && i + 1 < argc) cfg.machine_root = argv[++i];
        else if (a == L"--debug" && i + 1 < argc) SetEnvironmentVariableW(L"WIN10XBOX_DEBUG", argv[++i]);
        else if (a == L"--cwd" && i + 1 < argc) cwd = argv[++i];
        else if (a == L"--cmdline" && i + 1 < argc) cmdline = argv[++i];   // the layer: a child started by a guest
        else if (a == L"--guest-env") guest_env = true;                    // the layer: environment is the guest's
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
    cfg.child_processes = true;   // a plain Win32 program can start the host executable again (D1)
    // A guest will run: trace sink for the entry marker, +modules, the startup lines on stderr (before Initialize,
    // which reads WIN10XBOX_DEBUG; not for --version / --help / hello).
    if (!rest.empty()) crash.BeforeInitialize();
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
    if (!cmdline.empty()) opts.command_line = cmdline;   // raw, as the parent guest wrote it
    if (guest_env) opts.environment = win10xbox::process::InheritedGuestEnvironment();
    opts.current_directory = cwd;
    opts.run_on_new_thread = true;
    opts.wait = false;   // Launch returns once the guest runs; HostMain waits for it itself (below)
    std::fflush(stdout);
    // The display: the headless winexbox backend (a virtual 1920x1080 monitor, system-memory window surfaces).
    // core/user allocates window surfaces from it and composes the desktop; WIN10XBOX_SCREENSHOT=<file.bmp> writes
    // what was on that virtual screen when the guest exited (the only way to see a GUI guest on a CI runner).
    // Leaked on purpose like everything else the guest may still reference at EndProcess.
    winexbox::IWinexboxDriver* const display = winexbox::SelectBackend(winexbox::BackendConfig{}).release();
    win10xbox::user::SetDisplayDriver(display);
    display->SetEventSink(win10xbox::user::InputSink());   // driver input -> the guest's message queues
    wchar_t screenshot[MAX_PATH * 2] = {};
    const DWORD screenshot_len = GetEnvironmentVariableW(L"WIN10XBOX_SCREENSHOT", screenshot, MAX_PATH * 2);
    if (screenshot_len && screenshot_len < MAX_PATH * 2) g_shot_path.assign(screenshot, screenshot_len);
    // WIN10XBOX_TEST_INJECT_INPUT=<script>: scripted input on a host thread (see InjectThreadProc). Read and started
    // before Launch, like the screenshot switch: the thread runs its loader initialisation on the host's PEB, and the
    // script waits for `go`, which is set once Launch has started the guest.
    wchar_t inject_env[4096];
    const DWORD inject_len = GetEnvironmentVariableW(L"WIN10XBOX_TEST_INJECT_INPUT", inject_env, 4096);
    InjectJob* inject = nullptr;
    if (inject_len && inject_len < 4096) {
        std::string script(inject_len, '\0');
        for (DWORD i = 0; i < inject_len; ++i) script[i] = static_cast<char>(inject_env[i]);
        inject = StartInjectThread(display, script);
    }
    crash.BeforeLaunch();   // the host's module table, while PEB->Ldr is still the host's
    ActctxProbe actctx;     // test switch, off by default
    g_guest_ran = true;
    int rc = win10xbox::process::Launch(opts);   // returns once the guest runs (opts.wait is false)
    if (rc == 0) {
        if (inject) SetEvent(inject->go);
        rc = win10xbox::process::WaitForGuest(INFINITE);   // the second half of Launch(wait = true)
    }
    actctx.Check();
    if (rc < 0) std::fprintf(stderr, "win10xbox: could not start %ls: status %08x\n", opts.image_path.c_str(), rc);
    if (screenshot_len && screenshot_len < MAX_PATH * 2) {
        const bool ok = win10xbox::user::WriteScreenshot(screenshot);
        std::fprintf(stderr, "host-win32: screenshot %ls: %s\n", screenshot, ok ? "written" : "NOT written (nothing on screen)");
    }
    win10xbox::dispatch::Shutdown();
    win10xbox::Shutdown();
    return rc;
}

int wmain(int argc, wchar_t** argv) {
    // Read now: once a guest has launched, the PEB's environment block is the guest's.
    const bool normal_exit = EnvFlag(L"WIN10XBOX_NORMAL_EXIT");
    const int rc = HostMain(argc, argv);   // the crash capture's destructor has run by the time this returns
    if (normal_exit) return rc;            // diagnostic: the CRT -> ExitProcess -> DLL detach path
    EndProcess(rc);
}
