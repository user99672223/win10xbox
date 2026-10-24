// host.cpp - the host-uwp shell logic, see host.h.
//
// The worker's sequence is host-win32/main.cpp's wmain body, wrapped in what an
// AppContainer host additionally has to do (the same wrapping host-uwp-lite's
// smoke harness proved out on CI):
//   1. evidence trail first: trace.log, status.txt, crash recorder;
//   2. launch.json (LocalState, else the packaged default, else built-in defaults);
//   3. stage the machine folder in LocalState;
//   4. point the guest's stdout/stderr at LocalState\stdout.txt;
//   5. win10xbox::Initialize -> vfs validation -> preflight -> process::Launch;
//   6. wait for the guest (watchdog optional), write the verdict, Shutdown.
//
// Networking: this file contains none. The guest's own socket syscalls are
// passed through to the host kernel by core/dispatch (NtDeviceIoControlFile and
// friends are PassThrough, "\Device\Afd" paths are not rewritten by the VFS);
// the host process itself never opens a connection. The package manifest
// declares the network capabilities the guest's sockets need.
#include "host.h"

#include "host_diag.h"
#include "host_shell.h"
#include "host_stage.h"
#include "guest_sidecar.h"
#include "host_util.h"
#include "launch_config.h"
#include "launcher.h"

#include "win10xbox/config.h"
#include "win10xbox/core.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/trace.h"
#include "win10xbox/user.h"
#include "win10xbox/vfs.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((DWORD)0x00000102L)
#endif

namespace w10x_uwp
{
namespace
{
    constexpr DWORD kPollMs = 250;                       // granularity of the stop / watchdog checks
    constexpr std::uint64_t kMiB = 1ull << 20;
    constexpr std::size_t kMaxLaunchConfigBytes = 64u * 1024u;

    const char* KindName(winexbox::InputKind k)
    {
        switch (k)
        {
        case winexbox::InputKind::KeyDown: return "key-down";
        case winexbox::InputKind::KeyUp: return "key-up";
        case winexbox::InputKind::MouseMove: return "mouse-move";
        case winexbox::InputKind::MouseButtonDown: return "mouse-button-down";
        case winexbox::InputKind::MouseButtonUp: return "mouse-button-up";
        case winexbox::InputKind::MouseWheel: return "mouse-wheel";
        }
        return "?";
    }

    // Stands in for the win32u binding (winexbox.drv, deferred): the backend only
    // delivers events inside ProcessEvents(), to a sink. Until the binding takes
    // the sink over, the host's own sink drains the queue (so it cannot grow
    // without bound) and counts what arrived, which makes the CoreWindow ->
    // backend path observable in status.txt on a console.
    // Sets environment variables for one launch and puts the previous values back when the run ends, so a
    // sidecar's env applies to that launch only (the guest inherits the host's environment).
    class EnvScope
    {
    public:
        explicit EnvScope(const std::vector<std::pair<std::string, std::string>>& vars)
        {
            for (const auto& kv : vars)
            {
                Saved s;
                s.name = Widen(kv.first);
                const DWORD n = GetEnvironmentVariableW(s.name.c_str(), nullptr, 0);
                s.had = n != 0;
                if (s.had)
                {
                    s.value.resize(n);
                    s.value.resize(GetEnvironmentVariableW(s.name.c_str(), &s.value[0], n));
                }
                SetEnvironmentVariableW(s.name.c_str(), Widen(kv.second).c_str());
                saved_.push_back(std::move(s));
            }
        }
        ~EnvScope()
        {
            for (auto it = saved_.rbegin(); it != saved_.rend(); ++it)
                SetEnvironmentVariableW(it->name.c_str(), it->had ? it->value.c_str() : nullptr);
        }
        EnvScope(const EnvScope&) = delete;
        EnvScope& operator=(const EnvScope&) = delete;

    private:
        struct Saved
        {
            std::wstring name, value;
            bool had = false;
        };
        std::vector<Saved> saved_;
    };

    class CountingSink final : public winexbox::IEventSink
    {
    public:
        void OnInput(const winexbox::InputEvent& ev) override
        {
            switch (ev.kind)
            {
            case winexbox::InputKind::KeyDown: ++key_down_; break;
            case winexbox::InputKind::KeyUp: ++key_up_; break;
            case winexbox::InputKind::MouseMove: ++mouse_move_; break;
            case winexbox::InputKind::MouseButtonDown:
            case winexbox::InputKind::MouseButtonUp: ++mouse_button_; break;
            case winexbox::InputKind::MouseWheel: ++mouse_wheel_; break;
            }
            // A few lines so the first events are visible in trace.log; counts only after that.
            if (logged_.fetch_add(1) < 12)
                diag::Line("input: %s vk=0x%02x scan=0x%02x button=%d x=%d y=%d wheel=%d", KindName(ev.kind),
                           static_cast<unsigned>(ev.vk), static_cast<unsigned>(ev.scan), static_cast<int>(ev.button), ev.x,
                           ev.y, ev.wheel_delta);
            // ... and on to the guest's message queues (core/user posts WM_MOUSEMOVE / WM_KEYDOWN ...).
            win10xbox::user::InputSink()->OnInput(ev);
        }

        std::uint64_t Total() const
        {
            return key_down_ + key_up_ + mouse_move_ + mouse_button_ + mouse_wheel_;
        }

        std::string Summary() const
        {
            return Fmt("key-down=%llu key-up=%llu mouse-move=%llu mouse-button=%llu mouse-wheel=%llu",
                       static_cast<unsigned long long>(key_down_.load()), static_cast<unsigned long long>(key_up_.load()),
                       static_cast<unsigned long long>(mouse_move_.load()),
                       static_cast<unsigned long long>(mouse_button_.load()),
                       static_cast<unsigned long long>(mouse_wheel_.load()));
        }

    private:
        std::atomic<std::uint64_t> key_down_{0}, key_up_{0}, mouse_move_{0}, mouse_button_{0}, mouse_wheel_{0};
        std::atomic<std::uint32_t> logged_{0};
    };

    std::uint32_t Clamp(std::uint32_t v, std::uint32_t lo, std::uint32_t hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // The guest inherits the HOST's standard handles (BuildProcessParameters in
    // core/dispatch/process.cpp copies them out of the host's process parameters)
    // and SetStdHandle writes exactly those fields. Plain file handles sidestep
    // the real-Windows pipe-capture problem recorded in docs/STATUS.md.
    HANDLE OpenForGuest(const std::wstring& path, DWORD access, DWORD disposition, DWORD share)
    {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = nullptr;
        sa.bInheritHandle = TRUE;
        return CreateFileW(path.c_str(), access, share, &sa, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    std::wstring SystemPath(UINT (WINAPI *get)(LPWSTR, UINT), const wchar_t* fallback)
    {
        std::wstring p(MAX_PATH, L'\0');
        const UINT n = get(&p[0], MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return fallback;
        p.resize(n);
        return p;
    }

    // launch.json: LocalState first (the owner's), then the packaged default,
    // then the built-in defaults. A file that exists but cannot be read or parsed
    // is an error: running something other than what the owner wrote would be worse.
    bool LoadLaunchConfig(const std::wstring& local_state, const std::wstring& payload, LaunchConfig* lc,
                          std::string* source, std::string* error)
    {
        struct Candidate
        {
            std::wstring path;
            const char* name;
        };
        const Candidate candidates[] = {
            {local_state + L"\\launch.json", "LocalState\\launch.json"},
            {payload + L"\\launch.json", "the packaged default (payload\\launch.json)"},
        };
        for (const Candidate& c : candidates)
        {
            std::string text;
            DWORD err = 0;
            if (!ReadSmallFile(c.path, kMaxLaunchConfigBytes, &text, &err))
            {
                if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) continue;
                *error = Fmt("cannot read %s (error %lu)", c.name, static_cast<unsigned long>(err));
                return false;
            }
            LaunchConfig parsed;
            std::string perr;
            if (!ParseLaunchConfig(text, &parsed, &perr))
            {
                *error = std::string(c.name) + ": " + perr;
                return false;
            }
            *lc = std::move(parsed);
            *source = c.name;
            return true;
        }
        *lc = LaunchConfig();
        *source = "built-in defaults (no launch.json found)";
        return true;
    }

    std::wstring BuildCommandLine(const std::wstring& image, const LaunchConfig& lc)
    {
        std::wstring cmd = L"\"" + image + L"\"";
        for (const std::string& a : lc.args) cmd += L" " + QuoteArg(Widen(a));
        if (!lc.args_raw.empty()) cmd += L" " + Widen(lc.args_raw);
        return cmd;
    }
}  // namespace

// ----------------------------------------------------------------------
// Host::Impl
// ----------------------------------------------------------------------
struct Host::Impl
{
    explicit Impl(const HostInfo& i) : info(i), report(i.local_state + L"\\status.txt") { output_scaling = i.display.output_scaling; }
    std::mutex osc_mu;
    std::string output_scaling;   // under osc_mu
    std::string OutputScaling() { std::lock_guard<std::mutex> g(osc_mu); return output_scaling; }

    void RunWorker();
    void RunImpl();
    std::string CaptureScreen();
    void RecordAllRun(bool timed_out, long exit_code, const std::string& screen);
    void FinishAllChain();
    void AwaitRestartKill();
    std::string NextRestartArg() const;
    void ShowLauncherResult(const std::string& verdict, bool ran, long exit_code);
    void Fail(const std::string& what, const std::string& hint = std::string());
    void EndProcessAfterFinalStatus(const std::string& verdict, DWORD exit_code);
    std::string StopReason();

    HostInfo info;
    diag::Report report;
    std::unique_ptr<winexbox::IWinexboxDriver> display;
    CountingSink sink;
    std::string display_desc;

    std::thread worker;
    std::atomic<bool> finished{false};
    std::atomic<bool> teardown_on{false};      // a guest ran under process-level teardown: the process ends, it never returns into WinRT
    std::atomic<bool> final_shot_done{false};  // the shell has written the last frame after `finished`
    std::atomic<int> guest_code{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> guest_running{false};
    std::atomic<bool> exit_when_done{true};
    std::atomic<bool> input_enabled{true};
    std::atomic<HANDLE> stdout_handle{INVALID_HANDLE_VALUE};

    // Launcher (launcher.h): set per run by RunImpl.
    Launcher* launcher = nullptr;
    bool picked = false;            // this run's guest was chosen in the launcher
    bool autoselected = false;      // ... by launch.json "autoselect" (one run, then the host ends)
    std::string picked_name;
    // Restart-per-guest: set by the shell before the worker starts, see Host::SetRestart.
    int run_index = 0;                  // guests already run in this restart chain
    std::string restart_select;         // the cursor starts on this entry (the program that ran last)
    Host::RestartFn restart_fn;
    std::size_t autoselect_total = 0;   // entries in launch.json "autoselect" of this run
    bool chain_cfg_all = false;         // launch.json "autoselect": "all"
    bool restarted = false;             // this activation carried a restart argument
    int restart_all_k = -1;             // ... of a "Run every program" chain, position of the next program
    // "Run every program" chain state of this run (docs/HANDOFF.md "Launcher"), set by RunImpl.
    bool chain_all = false;             // this run is part of the all-programs chain (report.txt)
    bool chain_auto = false;            // the next program follows without input (list autoselect or all)
    bool chain_more = false;            // ... and there is a next one
    int all_k = 0;                      // position of this run in the all-programs list
    unsigned long long crash_off = 0;   // size of crash.txt when this run started

    std::mutex reason_mu;
    std::string stop_reason;
    ULONGLONG guest_start_tick = 0;
};

void Host::Impl::Fail(const std::string& what, const std::string& hint)
{
    diag::Line("FAILED: %s", what.c_str());
    report.Set("phase", "failed");
    if (!hint.empty()) report.Set("hint", hint);
    report.SetVerdict("FAIL (" + what + ")");
    report.Flush();
    ShowLauncherResult("FAIL (" + what + ")", false, 0);
}

namespace
{
std::string StemOf(const std::string& name)
{
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}
}  // namespace

std::string Host::Impl::NextRestartArg() const
{
    return FormatRestartArgs(run_index + 1, picked_name, chain_all ? all_k + 1 : -1);
}

// "Run every program": a copy of the screen as it is now, LocalState\<name>.bmp. user::WriteScreenshot composes the
// current (or last) frame; the periodic screen.bmp is the fallback.
std::string Host::Impl::CaptureScreen()
{
    const std::string name = StemOf(picked_name) + ".bmp";
    const std::wstring dst = info.local_state + L"\\" + Widen(name);
    if (win10xbox::user::WriteScreenshot(dst)) return name;
    const std::wstring src = info.local_state + L"\\screen.bmp";
    unsigned long long sz = 0;
    if (FileSize(src, &sz) && sz > 0 && CopyFileW(src.c_str(), dst.c_str(), FALSE)) return name;
    return "none";
}

// One block in LocalState\report.txt for the program of this run.
void Host::Impl::RecordAllRun(bool timed_out, long exit_code, const std::string& screen)
{
    ReportBlock b;
    b.program = picked_name;
    b.exit_code = exit_code;
    // The expected-exit table of the package (tests/guest/expected_exit.txt); the built-in fallback covers five programs.
    ExpectedTable table;
    std::string table_text, table_err;
    if (ReadTextFile(info.package_dir + L"\\payload\\expected_exit.txt", &table_text) && !ParseExpectedTable(table_text, &table, &table_err))
    {
        diag::Line("launcher: expected_exit.txt ignored (%s)", table_err.c_str());
        table.clear();
    }
    b.verdict = ReportVerdict(timed_out, exit_code, FindExpected(table, picked_name));
    b.screen = screen;
    b.stdout_lines = TailLines(info.local_state + L"\\stdout.txt", 10);
    std::string crash;
    if (ReadTextFile(info.local_state + L"\\crash.txt", &crash) && crash.size() > crash_off)
    {
        b.crash = true;
        std::size_t pos = static_cast<std::size_t>(crash_off);
        while (pos < crash.size() && b.crash_lines.size() < 5)
        {
            std::size_t e = crash.find('\n', pos);
            if (e == std::string::npos) e = crash.size();
            std::string line = crash.substr(pos, e - pos);
            pos = e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) b.crash_lines.push_back(line);
        }
    }
    if (!WriteTextFile(info.local_state + L"\\report.txt", FormatReportBlock(b), true))
        diag::Line("launcher: cannot append to report.txt (error %lu)", GetLastError());
    diag::Line("report: %s: %s", picked_name.c_str(), b.verdict.c_str());
}

// The end of an all-programs chain: the header of report.txt (once) and the summary screen.
// "major.minor.build" from ntdll!RtlGetVersion. Resolved once, and first from RunWorker before any guest runs: once a guest
// is loaded the process's "ntdll.dll" can be the guest's Wine one, which answered "unknown" in the all-programs report.
static std::string OsBuildString()
{
    static const std::string os = []
    {
        std::string r = "unknown";
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
            if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")))
            {
                RTL_OSVERSIONINFOW vi = {};
                vi.dwOSVersionInfoSize = sizeof vi;
                if (fn(&vi) == 0) r = Fmt("%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
            }
        return r;
    }();
    return os;
}

// After RequestRestartAsync returned a failure text: a granted restart ends this process a little later, so give it
// time. Only a process that is still alive after that has a restart that really failed (the summary may then be written).
void Host::Impl::AwaitRestartKill()
{
    diag::Line("launcher: restart reported a failure; waiting up to 45 s in case the process is ended anyway");
    diag::FlushLog();
    for (int i = 0; i < 450; ++i) Sleep(100);
}

void Host::Impl::FinishAllChain()
{
    const std::wstring path = info.local_state + L"\\report.txt";
    std::string body;
    ReadTextFile(path, &body);
    const std::vector<ReportBlock> blocks = ParseReport(body);
    unsigned pass = 0, knownfail = 0, fail = 0, exited = 0, timeout = 0, crash = 0, skipped = 0;
    std::vector<std::string> rows;
    for (const ReportBlock& b : blocks)
    {
        const std::string w = VerdictWord(b.verdict);
        if (w == "PASS") ++pass;
        else if (w == "KNOWN-FAIL") ++knownfail;
        else if (w == "FAIL") ++fail;
        else if (w == "EXITED") ++exited;
        else if (w == "TIMEOUT") ++timeout;
        else if (w == "CRASH") ++crash;
        else if (w == "SKIPPED") ++skipped;
        rows.push_back(b.program + "  " + w + "  exit " + std::to_string(b.exit_code));
    }
    const std::string counts = Fmt("%u programs: %u PASS, %u KNOWN-FAIL, %u FAIL, %u EXITED, %u TIMEOUT, %u CRASH, %u SKIPPED",
                                   static_cast<unsigned>(blocks.size()), pass, knownfail, fail, exited, timeout, crash, skipped);
    if (body.rfind("# win10xbox report", 0) != 0)
    {
        const std::string os = OsBuildString();
        std::string head = "# win10xbox report\n";
        head += "os_build=" + os + "\n";
        head += "package=" + info.package_full_name + "\n";
        head += "package_version=" + info.package_version + "\n";
        head += "display=" + DescribeDisplay(info.display) + "\n";
        head += "guest_display=" + DescribeGuestDisplay(info.display) + "\n";
        head += "output_scaling=" + OutputScaling() + "\n";
        head += "date_utc=" + TimestampUtc() + "\n";
        head += "programs=" + std::to_string(blocks.size()) + "\n";
        head += std::string("chain=") + (chain_more ? "aborted" : "done") + "\n";
        head += Fmt("pass=%u\nknownfail=%u\nfail=%u\nexited=%u\ntimeout=%u\ncrash=%u\nskipped=%u\n\n", pass, knownfail, fail, exited, timeout, crash, skipped);
        if (!WriteTextFile(path, head + body, false)) diag::Line("launcher: cannot write the report.txt header (error %lu)", GetLastError());
    }
    diag::Line("report: %s", counts.c_str());
    report.Set("report", counts);
    report.Flush();
    if (launcher != nullptr)
    {
        LauncherResult r;
        r.summary = true;
        r.name = "Run every program: summary";
        r.verdict = counts;
        r.lines = rows;
        launcher->ShowResult(r);
    }
}

// The launcher's result screen and LocalState\launcher_result.txt for the guest chosen in this run.
void Host::Impl::ShowLauncherResult(const std::string& verdict, bool ran, long exit_code)
{
    if (launcher == nullptr || !picked) return;
    LauncherResult r;
    r.name = picked_name;
    r.verdict = verdict;
    r.ran = ran;
    r.exit_code = exit_code;
    r.run = run_index + 1;
    if (ran) r.lines = TailLines(info.local_state + L"\\stdout.txt", 20);
    if (!WriteLauncherResult(info.local_state + L"\\launcher_result.txt", r))
        diag::Line("launcher: cannot write launcher_result.txt (error %lu)", GetLastError());
    diag::Line("launcher: result for %s: %s", r.name.c_str(), verdict.c_str());
    launcher->ShowResult(r);
}

std::string Host::Impl::StopReason()
{
    std::lock_guard<std::mutex> lock(reason_mu);
    return stop_reason.empty() ? std::string("stopped by the shell") : stop_reason;
}

// The guest still owns the PEB (loader lists, process parameters...): do not
// return into WinRT code, write the verdict through the held handle and end the
// process.
void Host::Impl::EndProcessAfterFinalStatus(const std::string& verdict, DWORD exit_code)
{
    report.Set("phase", "finished");
    report.Set("input.events", sink.Summary());
    report.SetVerdict(verdict);
    report.Flush();
    diag::Line("verdict: %s", verdict.c_str());
    diag::Line("terminating the process (the guest is still running)");
    diag::FlushLog();
    const HANDLE out = stdout_handle.load();
    if (out != INVALID_HANDLE_VALUE) FlushFileBuffers(out);
    TerminateProcess(GetCurrentProcess(), exit_code);
}

void Host::Impl::RunWorker()
{
    try
    {
        (void)OsBuildString();   // before any guest owns the process
        // With the launcher a guest is followed by a restart of the whole app (core allows one guest per
        // process, docs/HANDOFF.md "Launcher"): RequestRestartAsync ends this process on success. Only when
        // the restart is refused does the worker go round in this process (the old behaviour, whose next
        // Launch is refused with a message naming the restart). A failure before a guest was chosen, a
        // stop request, or the end of a launch.json driven chain end the loop.
        for (;;)
        {
            picked = false;
            RunImpl();
            if (!picked || launcher == nullptr || stop.load()) break;
            const bool from_config = autoselect_total != 0 || chain_cfg_all;   // launch.json drives the chain
            std::string arg;
            if (chain_auto && chain_more)
                arg = NextRestartArg();
            else
            {
                if (chain_all) FinishAllChain();
                if (chain_auto && from_config) break;   // the app ends (exit_when_done)
                if (!launcher->WaitDismiss(stop)) break;
                arg = FormatRestartArgs(run_index + 1, picked_name, -1);
            }

            std::string why;
            if (!restart_fn)
                why = "the shell offers no restart";
            else
            {
                diag::Line("launcher: restarting the app for the next program (argument \"%s\")", arg.c_str());
                report.Set("launcher.restart", "requested: " + arg);
                report.Flush();
                diag::FlushLog();
                why = restart_fn(arg);   // does not return when the restart is granted
                if (why.empty()) why = "the restart was granted but the process did not end";
                else if (chain_all) AwaitRestartKill();
            }
            diag::Line("launcher: restart failed: %s", why.c_str());
            report.Set("launcher.restart", "FAILED: " + why);
            report.Flush();
            if (chain_auto)
            {
                if (chain_all && chain_more) FinishAllChain();   // keep what was measured
                break;   // a chain cannot continue without the restart: the missing blocks say so
            }

            // Interactive: the reason on the result screen, then back to the list in this process.
            LauncherResult r;
            r.name = picked_name;
            r.verdict = "restart failed";
            r.ran = true;
            r.run = run_index + 1;
            r.note = "Cannot restart for the next program: " + why;
            launcher->ShowResult(r);
            if (!launcher->WaitDismiss(stop)) break;
            restart_select = picked_name;
            ++run_index;
        }
    }
    catch (const std::exception& e)
    {
        diag::Line("FATAL: C++ exception in the host: %s", e.what());
        report.Set("error", std::string("C++ exception: ") + e.what());
        report.SetVerdict("FAIL (host exception)");
        report.Flush();
    }
    catch (...)
    {
        diag::Line("FATAL: unknown exception in the host");
        report.SetVerdict("FAIL (host exception)");
        report.Flush();
    }
    guest_running.store(false);
    diag::FlushLog();
    finished.store(true);
    if (teardown_on.load())
    {
        // The frozen guest threads must never be resumed into CoreApplication::Exit: let the shell write the last frame
        // (it does so once `finished` is set), then end the process with the guest's code.
        for (int i = 0; i < 50 && !final_shot_done.load(); ++i) Sleep(100);
        diag::Line("terminating the process with the guest's exit code %d", guest_code.load());
        diag::FlushLog();
        const HANDLE out = stdout_handle.load();
        if (out != INVALID_HANDLE_VALUE) FlushFileBuffers(out);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(guest_code.load()));
    }
}

void Host::Impl::RunImpl()
{
    const std::wstring ls = info.local_state;
    std::wstring machine = ls + L"\\machine";   // re-pointed below when launch.json / the picked entry names another folder
    std::string machine_name = "machine";
    const std::wstring payload = info.package_dir + L"\\payload";

    // ---- 0. the evidence trail first, so everything below is captured -------
    diag::OpenLog(ls + L"\\trace.log", 64 * kMiB);
    diag::InstallCrashFilter(ls + L"\\status.txt");
    const win10xbox::Version ver = win10xbox::GetVersion();
    diag::Line("host-uwp (built %s %s) on core %d.%d.%d (%s)", __DATE__, __TIME__, ver.major, ver.minor, ver.patch,
               ver.git ? ver.git : "?");
    diag::Line("package=%s version=%s", info.package_full_name.c_str(), info.package_version.c_str());
    diag::Line("local_state=%s", Narrow(ls).c_str());
    diag::Line("package_dir=%s", Narrow(info.package_dir).c_str());
    diag::Line("display: %s", display_desc.c_str());
    diag::Line("display=%s (source %s)", DescribeDisplay(info.display).c_str(), info.display.source.c_str());
    diag::Line("guest_display=%s", DescribeGuestDisplay(info.display).c_str());
    for (const std::string& n : info.display.notes) diag::Line("display: %s", n.c_str());

    report.Set("tool", "win10xbox host-uwp (M5 shell, headless display backend, D3D11 presenter)");
    report.Set("started_utc", TimestampUtc());
    report.Set("build", Fmt("%s %s", __DATE__, __TIME__));
    report.Set("core", Fmt("%d.%d.%d (%s)", ver.major, ver.minor, ver.patch, ver.git ? ver.git : "?"));
    report.Set("package", info.package_full_name);
    report.Set("local_state", Narrow(ls));
    report.Set("display", DescribeDisplay(info.display));
    report.Set("guest_display", DescribeGuestDisplay(info.display));
    report.Set("output_scaling", OutputScaling());
    report.Set("display.source", info.display.source);
    report.Set("display.virtual", display_desc);
    report.SetVerdict("INCOMPLETE (still starting, or the process died: see 'phase' below and trace.log)");
    report.Set("phase", "start");
    report.Flush();

    // ---- 1. launch.json ---------------------------------------------------------
    report.Set("phase", "launch config");
    LaunchConfig lc;
    std::string source, cfg_error;
    if (!LoadLaunchConfig(ls, payload, &lc, &source, &cfg_error))
    {
        Fail("launch config: " + cfg_error,
             "fix or delete LocalState\\launch.json (a copy of the documented defaults is LocalState\\launch.example.json)");
        return;
    }
    exit_when_done.store(lc.exit_when_done);
    input_enabled.store(lc.input_pump);
    diag::SetLogCap(static_cast<std::uint64_t>(lc.trace_log_max_mb) * kMiB);
    diag::Line("launch config from %s:", source.c_str());
    {
        const std::string text = DescribeLaunchConfig(lc);
        std::size_t pos = 0;
        while (pos < text.size())
        {
            const std::size_t nl = text.find('\n', pos);
            diag::Line("  %s", text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos).c_str());
            pos = nl == std::string::npos ? text.size() : nl + 1;
        }
    }
    report.Set("launch.source", source);
    report.Set("launch.guest", lc.guest);
    report.Set("launch.args", [&] {
        std::string a;
        for (const std::string& x : lc.args) a += (a.empty() ? "" : " ") + QuoteArg(x);
        if (!lc.args_raw.empty()) a += (a.empty() ? "" : " ") + lc.args_raw;
        return a;
    }());
    if (lc.has_expect_exit_code) report.Set("launch.expect_exit_code", Fmt("%d", lc.expect_exit_code));
    report.Flush();

    // Tracing knobs reach the core through the environment it reads at Initialize.
    if (!lc.trace.empty()) SetEnvironmentVariableW(L"WIN10XBOX_DEBUG", Widen(lc.trace).c_str());
    if (!lc.winedebug.empty()) SetEnvironmentVariableW(L"WIN10XBOX_WINEDEBUG", Widen(lc.winedebug).c_str());
    // The "trace" line above REPLACED WIN10XBOX_DEBUG with the owner's spec; entry-reached.txt needs
    // the core's `modules` channel on whatever that spec says, so it is put back (host_shell.h).
    // Only here can this be done: after the replacement, before win10xbox::Initialize reads the
    // variable.
    diag::Line("%s", EnsureModulesChannel().c_str());

    if (stop.load())
    {
        Fail("stopped by the shell before the guest started (" + StopReason() + ")");
        return;
    }

    // ---- 1b. the launcher: choose the guest (launcher.h) -----------------------------------------------------
    // On when launch.json says "picker": true or names an "autoselect" entry, and by default when there is no
    // launch.json at all. The list is the guests bundled in the package plus any .exe in LocalState\guests.
    LauncherEntry chosen;
    std::vector<std::pair<std::string, std::string>> launch_env;   // chosen entry's env, applied by EnvScope below
    std::string entry_display;                                     // chosen entry's .display / display= ("" = none)
    autoselected = false;
    chain_all = chain_auto = chain_more = false;
    all_k = 0;
    autoselect_total = lc.autoselect.size();
    chain_cfg_all = lc.autoselect_all;
    const bool no_config = source.rfind("built-in defaults", 0) == 0;
    if (launcher != nullptr && (restarted || (lc.has_picker ? lc.picker : (no_config || !lc.autoselect.empty() || lc.autoselect_all))))
    {
        report.Set("phase", "launcher");
        report.Flush();
        std::vector<LauncherEntry> entries;
        auto has_ext = [](const std::wstring& n, const wchar_t* ext) {
            const std::size_t l = wcslen(ext);
            return n.size() > l && _wcsicmp(n.c_str() + n.size() - l, ext) == 0;
        };
        // Builds one entry from `f` (an .exe, or a .launch text file naming one) and its sidecars next to it.
        auto add_entry = [&](const std::wstring& dir, const std::wstring& f, bool from_ls) {
            LauncherEntry e;
            e.name = Narrow(f);
            e.path = dir + L"\\" + f;
            e.from_localstate = from_ls;
            const std::wstring stem = f.substr(0, f.find_last_of(L'.'));
            auto read = [&](const std::wstring& file, std::string* text) {
                DWORD err = 0;
                return ReadSmallFile(dir + L"\\" + file, 64 * 1024, text, &err);
            };
            std::string text, why;
            if (has_ext(f, L".launch"))
            {
                GuestLaunch gl;
                if (!read(f, &text) || !ParseLaunchFile(text, &gl, &why))
                {
                    diag::Line("launcher: ignoring %s (%s)", e.name.c_str(), why.empty() ? "cannot read" : why.c_str());
                    return;
                }
                e.exe = gl.exe;
                e.cwd = gl.cwd;
                e.title = gl.title;
                e.args = gl.args;
                e.env = gl.env;
                e.machine = gl.machine;
                e.display = gl.display;
                bool mok = true;
                if (e.display.empty() && read(stem + L".display", &text))
                {
                    e.display = ParseDisplayFile(text, &mok);
                    if (!mok) diag::Line("launcher: %s.display ignored (not auto or WxH@Hz)", Narrow(stem).c_str());
                    mok = true;
                }
                if (e.machine.empty() && read(stem + L".machine", &text))
                {
                    e.machine = ParseMachineFile(text, &mok);
                    if (!mok) diag::Line("launcher: %s.machine ignored (not a folder name)", Narrow(stem).c_str());
                }
            }
            else
            {
                e.exe = e.name;
                if (read(stem + L".args", &text)) ParseArgsFile(text, &e.args);
                if (read(stem + L".env", &text) && !ParseEnvFile(text, &e.env, &why))
                {
                    diag::Line("launcher: %s.env ignored (%s)", Narrow(stem).c_str(), why.c_str());
                    e.env.clear();
                }
                if (read(stem + L".title", &text)) e.title = ParseTitleFile(text);
                if (read(stem + L".cwd", &text))
                {
                    // First non-blank line: a directory relative to C:\guest ("." = C:\guest itself), for programs that open
                    // their data files by relative name (guest_d3dsample).
                    std::string line = ParseTitleFile(text), norm;
                    if (NormalizeRelativePath(line, true, &norm)) e.cwd = norm;
                    else diag::Line("launcher: %s.cwd ignored (not a relative directory)", Narrow(stem).c_str());
                }
                if (read(stem + L".display", &text))
                {
                    bool dok = true;
                    e.display = ParseDisplayFile(text, &dok);
                    if (!dok) diag::Line("launcher: %s.display ignored (not auto or WxH@Hz)", Narrow(stem).c_str());
                }
                if (read(stem + L".machine", &text))
                {
                    bool mok = true;
                    e.machine = ParseMachineFile(text, &mok);
                    if (!mok) diag::Line("launcher: %s.machine ignored (not a folder name)", Narrow(stem).c_str());
                }
            }
            for (LauncherEntry& o : entries)
                if (_stricmp(o.name.c_str(), e.name.c_str()) == 0) return;   // the bundled one wins
            entries.push_back(std::move(e));
        };
        // A subdirectory holding <dirname>.launch is ONE entry (a directory install): mapped as C:\guest\<dirname>,
        // exe= and cwd= relative to it. The entry is not offered when the program is not in the directory.
        auto add_dir_entry = [&](const std::wstring& parent, const std::wstring& d, bool from_ls) {
            const std::wstring dir = parent + L"\\" + d;
            std::string text, why;
            DWORD err = 0;
            if (!IsLeafName(d) || !ReadSmallFile(dir + L"\\" + d + L".launch", 64 * 1024, &text, &err)) return;
            GuestLaunch gl;
            if (!ParseLaunchFile(text, &gl, &why))
            {
                diag::Line("launcher: ignoring %s\\%s.launch (%s)", Narrow(d).c_str(), Narrow(d).c_str(), why.c_str());
                return;
            }
            if (GetFileAttributesW((dir + L"\\" + Widen(gl.exe)).c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                diag::Line("launcher: %s needs %s, which is not in the directory", Narrow(d).c_str(), gl.exe.c_str());
                return;
            }
            LauncherEntry e;
            e.name = Narrow(d);
            e.path = dir + L"\\" + d + L".launch";
            e.from_localstate = from_ls;
            e.is_dir = true;
            e.dir_path = dir;
            e.exe = gl.exe;
            e.cwd = gl.cwd;
            e.title = gl.title;
            e.args = gl.args;
            e.env = gl.env;
            e.machine = gl.machine;
            for (LauncherEntry& o : entries)
                if (_stricmp(o.name.c_str(), e.name.c_str()) == 0) return;   // the bundled one wins
            entries.push_back(std::move(e));
        };
        for (const std::wstring& f : ListFiles(payload + L"\\guest"))
            if (has_ext(f, L".exe") || has_ext(f, L".launch")) add_entry(payload + L"\\guest", f, false);
        for (const std::wstring& d : ListDirs(payload + L"\\guest")) add_dir_entry(payload + L"\\guest", d, false);
        for (const std::wstring& f : ListFiles(ls + L"\\guests"))
            if (has_ext(f, L".exe") || has_ext(f, L".launch")) add_entry(ls + L"\\guests", f, true);
        for (const std::wstring& d : ListDirs(ls + L"\\guests")) add_dir_entry(ls + L"\\guests", d, true);
        // A .launch entry whose program is not in the package (or in LocalState\guests) is not offered.
        {
            std::vector<LauncherEntry> kept;
            for (const LauncherEntry& e : entries)
            {
                bool ok = e.is_dir || e.exe == e.name;
                for (const LauncherEntry& o : entries)
                    if (!ok && _stricmp(o.name.c_str(), e.exe.c_str()) == 0) ok = true;
                if (ok) kept.push_back(e);
                else diag::Line("launcher: %s needs %s, which is not there", e.name.c_str(), e.exe.c_str());
            }
            entries.swap(kept);
        }
        std::sort(entries.begin(), entries.end(), [](const LauncherEntry& a, const LauncherEntry& b) {
            return _stricmp((a.title.empty() ? a.name : a.title).c_str(), (b.title.empty() ? b.name : b.title).c_str()) < 0;
        });
        diag::Line("launcher: %u entr%s", static_cast<unsigned>(entries.size()), entries.size() == 1 ? "y" : "ies");

        // The all-programs list: every entry in name order except those with a "<name>.exe.skip-auto" or
        // "<name>.skip-auto" file beside them (programs that need a person at the controller).
        std::vector<LauncherEntry> runlist;
        std::string skipped_blocks;
        auto build_runlist = [&]() {
            runlist.clear();
            skipped_blocks.clear();
            for (const LauncherEntry& e : entries)
            {
                const std::wstring a1 = e.path + L".skip-auto";
                const std::wstring a2 = e.path.substr(0, e.path.size() - 4) + L".skip-auto";
                if (GetFileAttributesW(a1.c_str()) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW(a2.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    ReportBlock b;
                    b.program = e.name;
                    b.verdict = "SKIPPED (skip-auto file: needs interactive input)";
                    b.screen = "none";
                    skipped_blocks += FormatReportBlock(b);
                }
                else
                    runlist.push_back(e);
            }
        };
        auto start_all = [&](int k) -> bool {
            chain_all = true;
            all_k = k;
            build_runlist();
            if (runlist.empty() || static_cast<std::size_t>(k) >= runlist.size())
            {
                Fail("the all-programs run has no program number " + std::to_string(k) + " (" + std::to_string(runlist.size()) + " to run)");
                return false;
            }
            chosen = runlist[static_cast<std::size_t>(k)];
            chain_more = static_cast<std::size_t>(k) + 1 < runlist.size();
            if (k == 0 && !WriteTextFile(ls + L"\\report.txt", skipped_blocks, false))
                diag::Line("launcher: cannot create report.txt (error %lu)", GetLastError());
            lc.timeout_sec = lc.autoselect_timeout_sec;
            return true;
        };

        if (lc.autoselect_all || restart_all_k >= 0)
        {
            if (!start_all(restart_all_k >= 0 ? restart_all_k : 0)) return;
        }
        else if (!lc.autoselect.empty() && static_cast<std::size_t>(run_index) < lc.autoselect.size())
        {
            autoselected = true;
            const std::string& want = lc.autoselect[static_cast<std::size_t>(run_index)];
            int idx = -1;
            for (std::size_t i = 0; i < entries.size(); ++i)
                if (_stricmp(entries[i].name.c_str(), want.c_str()) == 0) idx = static_cast<int>(i);
            if (idx < 0)
            {
                Fail("launch.json \"autoselect\" names " + want + ", which is not in the launcher list");
                return;
            }
            chosen = entries[static_cast<std::size_t>(idx)];
            chain_more = static_cast<std::size_t>(run_index) + 1 < lc.autoselect.size();
        }
        else
        {
            const std::size_t slash = lc.guest.find_last_of('\\');
            // The cursor starts on the program that just ran (restart argument), else on launch.json "guest".
            const std::string start_on =
                !restart_select.empty() ? restart_select : (slash == std::string::npos ? lc.guest : lc.guest.substr(slash + 1));
            std::vector<LauncherEntry> pick_entries;
            LauncherEntry all_entry;
            all_entry.name = kRunAllTitle;
            all_entry.run_all = true;
            pick_entries.push_back(all_entry);
            pick_entries.insert(pick_entries.end(), entries.begin(), entries.end());
            const int idx = launcher->Pick(pick_entries, start_on, stop);
            if (idx < 0)
            {
                Fail("stopped by the shell in the launcher (" + StopReason() + ")");
                return;
            }
            if (pick_entries[static_cast<std::size_t>(idx)].run_all)
            {
                if (!start_all(0)) return;
            }
            else
                chosen = pick_entries[static_cast<std::size_t>(idx)];
        }
        chain_auto = autoselected || chain_all;
        { unsigned long long cs = 0; crash_off = FileSize(ls + L"\\crash.txt", &cs) ? cs : 0; }
        launcher->SetRunning();
        picked = true;
        picked_name = chosen.name;
        // The settings written for another guest (arguments, expected exit code) do not belong to this one;
        // the autoselect test hook keeps them.
        const std::string configured = lc.guest.substr(lc.guest.find_last_of('\\') + 1);
        if (chain_all || (!autoselected && _stricmp(configured.c_str(), chosen.name.c_str()) != 0))
        {
            lc.args.clear();
            lc.args_raw.clear();
            lc.has_expect_exit_code = false;
            lc.machine.clear();
        }
        if (!chosen.machine.empty()) lc.machine = chosen.machine;   // the entry's .machine / machine= wins
        {
            // A directory entry lives in C:\guest\<dir>\; cwd= is relative to the entry's directory.
            const std::string base = chosen.is_dir ? "C:\\guest\\" + chosen.name : std::string("C:\\guest");
            lc.guest = base + "\\" + chosen.exe;
            if (!chosen.cwd.empty()) lc.cwd = chosen.cwd == "." ? base : base + "\\" + chosen.cwd;
            else if (chosen.is_dir) lc.cwd = base;
        }
        if (!chosen.args.empty())
        {
            lc.args = chosen.args;   // the sidecar's arguments replace launch.json's for this launch
            lc.args_raw.clear();
        }
        launch_env = chosen.env;
        entry_display = chosen.display;
        report.Set("launch.guest", lc.guest);
        report.Set("launcher.picked", chosen.name + (chosen.from_localstate ? " (LocalState\\guests)" : " (bundled)") +
                                          (chain_all ? " by the all-programs run" : autoselected ? " by autoselect" : ""));
        report.Flush();
        diag::Line("launcher: %s %s", autoselected ? "autoselected" : "chosen", chosen.name.c_str());
        if (stop.load())
        {
            Fail("stopped by the shell before the guest started (" + StopReason() + ")");
            return;
        }
    }

    // The picked entry's .env/env= pairs: in the host's environment (the guest inherits it) for this launch only.
    EnvScope launch_env_scope(launch_env);

    // The machine folder: launch.json "machine" / the picked entry's .machine, default "machine".
    if (!lc.machine.empty()) machine_name = lc.machine;
    machine = ls + L"\\" + Widen(machine_name);
    const bool emu_d3d_machine = _stricmp(machine_name.c_str(), kEmuD3dMachineName) == 0;
    const bool d3d_machine = emu_d3d_machine || _stricmp(machine_name.c_str(), kD3dMachineName) == 0;
    report.Set("machine.folder", machine_name);
    diag::Line("machine folder: %s", machine_name.c_str());

    // ---- 2. machine.json, then the layer ------------------------------------------
    report.Set("phase", "machine.json");
    report.Flush();
    if (!StageMachineJson(machine, payload, d3d_machine ? Widen(machine_name) + L"\\machine.json" : std::wstring(L"machine.json")))
        diag::Line("warning: no machine.json in the package payload and none in the machine folder: the layer uses its defaults");

    // stdout/stderr -> LocalState\stdout.txt, stdin -> an empty file.
    const std::wstring stdout_path = ls + L"\\stdout.txt";
    HANDLE h_out = OpenForGuest(stdout_path, GENERIC_WRITE, CREATE_ALWAYS, FILE_SHARE_READ | FILE_SHARE_WRITE);
    HANDLE h_in = OpenForGuest(ls + L"\\stdin.txt", GENERIC_READ, OPEN_ALWAYS, FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (h_out == INVALID_HANDLE_VALUE)
    {
        diag::Line("cannot create stdout.txt (error %lu): the guest's output will be lost", GetLastError());
    }
    else
    {
        SetStdHandle(STD_OUTPUT_HANDLE, h_out);
        SetStdHandle(STD_ERROR_HANDLE, h_out);
        stdout_handle.store(h_out);
    }
    if (h_in != INVALID_HANDLE_VALUE) SetStdHandle(STD_INPUT_HANDLE, h_in);

    report.Set("phase", "initialize");
    report.Flush();
    win10xbox::trace::AddSink(&diag::TraceSink, nullptr);
    win10xbox::config::Config cfg;
    cfg.machine_root = machine;
    cfg.trace_default_on = false;                                          // quiet (err + fixme); launch.json "trace" refines
    cfg.display_backend = win10xbox::config::DisplayBackend::Headless;     // D8: the D3D12 backend is M6
    cfg.inbound_sockets = true;                                            // D7 (core passes socket syscalls through regardless)
    cfg.wow64 = win10xbox::config::Wow64::Off;                             // D6
    const int init_rc = win10xbox::Initialize(cfg);
    report.Set("initialize", Fmt("%d", init_rc));
    diag::Line("win10xbox::Initialize -> %d", init_rc);
    if (init_rc != 0)
    {
        Fail(Fmt("win10xbox::Initialize failed (%d)", init_rc), "see the first lines of trace.log");
        return;
    }

    // machine.json is parsed by the VFS; a malformed file refuses to start (vfs.h).
    const long vfs_rc = static_cast<long>(win10xbox::vfs::Initialize(machine));
    if (vfs_rc != 0)
    {
        Fail("machine.json is malformed or its machine root is unusable (vfs::Initialize " + Hex32(vfs_rc) + ")",
             "fix LocalState\\<machine folder>\\machine.json (schema: machine/machine.schema.json) or delete it to restore the "
             "packaged default; the 'err:vfs' line in trace.log names the problem");
        return;
    }
    std::wstring c_root = machine;
    for (const win10xbox::vfs::DriveMapping& d : win10xbox::vfs::Drives())
        if (d.letter == L'C') c_root = d.host_root;
    const std::wstring profile = win10xbox::vfs::GuestUserProfile();   // C:\Users\<user>
    const std::wstring profile_rel = profile.size() > 3 ? profile.substr(3) : std::wstring(L"Users\\User");
    report.Set("machine.user", Narrow(profile));
    report.Set("machine.c_root", Narrow(c_root));

    if (stop.load())
    {
        Fail("stopped by the shell before the guest started (" + StopReason() + ")");
        return;
    }

    // ---- 3. stage the machine folder ---------------------------------------------------
    report.Set("phase", "staging");
    report.Flush();
    StagePlan plan;
    plan.c_root = c_root;
    plan.payload_dir = payload;
    plan.system32 = SystemPath(&GetSystemDirectoryW, L"C:\\Windows\\System32");
    plan.windows = SystemPath(&GetWindowsDirectoryW, L"C:\\Windows");
    plan.profile_rel = profile_rel;
    plan.stamp = Widen(info.package_full_name);
    for (const std::string& n : lc.stage_native) plan.native_names.push_back(Widen(n));
    if (d3d_machine)
    {
        // Route B + Direct3D 11: the native GUI and D3D runtime, read BY NAME from the console's System32
        // (tests/integration/prepare_machine.cmake D3D_MACHINE), plus the package's msctf.dll stand-in.
        // machine-emu-d3d: the same without the native user32/gdi32/win32u/imm32 (the emulated window manager stays).
        for (const std::string& n : emu_d3d_machine ? EmuD3dMachineNativeDlls() : D3dMachineNativeDlls())
            plan.native_names.push_back(Widen(n));
        plan.profile_dir = Widen(machine_name);
    }
    const StageResult st = StageMachineFiles(plan);
    report.Set("stage.builtin", st.builtin.Summary());
    report.Set("stage.guest", st.guest.Summary());
    report.Set("stage.nls", st.nls.Summary());
    if (d3d_machine) report.Set("stage.stubs", st.stubs.Summary());
    if (!lc.stage_native.empty()) report.Set("stage.native", st.native.Summary());
    if (!st.builtin.bad.empty()) report.Set("stage.builtin.not_staged", st.builtin.BadList());
    if (!st.nls.bad.empty()) report.Set("stage.nls.not_staged", st.nls.BadList());
    if (!st.native.bad.empty()) report.Set("stage.native.not_staged", st.native.BadList());
    report.Set("stage.package_refreshed", st.refreshed ? "yes" : "no");
    report.Set("builtin.ntdll", st.have_ntdll ? "present" : "MISSING");
    {
        std::string req;
        for (const std::string& m : st.required_missing) req += (req.empty() ? "" : ",") + m;
        report.Set("nls.required_missing", req.empty() ? "none" : req);
    }
    diag::Line("stage: done. builtin[%s] guest[%s] nls[%s]", st.builtin.Summary().c_str(), st.guest.Summary().c_str(),
               st.nls.Summary().c_str());
    report.Flush();

    // ---- 4. preflight: say precisely what is wrong instead of a bare NTSTATUS ----------------
    report.Set("phase", "preflight");
    if (!st.have_ntdll)
    {
        Fail("no Wine PE builtins in the machine folder (Windows\\builtin\\ntdll.dll is missing)",
             "the package was built without the wine-pe-builtin artifact: copy wine-pe/out/builtin/*.dll into "
             "LocalState\\machine\\Windows\\builtin (or install a package that bundles them) and relaunch");
        return;
    }
    if (!st.required_missing.empty())
        diag::Line("warning: required NLS data could not be staged from the console's System32 (%s): Wine's locale "
                   "initialisation is likely to fail",
                   st.nls.BadList().c_str());
    if (picked && chosen.is_dir)
    {
        // A directory install (bundled or from LocalState\guests) is copied whole into C:\guest\<dir>.
        if (!CopyTree(chosen.dir_path, c_root + L"\\guest\\" + Widen(chosen.name)))
            diag::Line("launcher: copying %s into C:\\guest failed (error %lu)", chosen.name.c_str(), GetLastError());
    }
    else if (picked && chosen.from_localstate)
    {
        // A guest from LocalState\guests runs from C:\guest: copy it (and the DLLs next to it) there.
        const std::wstring src_dir = ls + L"\\guests";
        EnsureDir(c_root + L"\\guest");
        for (const std::wstring& f : ListFiles(src_dir))
        {
            const bool dll = f.size() > 4 && _wcsicmp(f.c_str() + f.size() - 4, L".dll") == 0;
            if (dll || _wcsicmp(f.c_str(), Widen(chosen.exe).c_str()) == 0)
                if (!CopyFileW((src_dir + L"\\" + f).c_str(), (c_root + L"\\guest\\" + f).c_str(), FALSE))
                    diag::Line("launcher: cannot copy %s into C:\\guest (error %lu)", Narrow(f).c_str(), GetLastError());
        }
    }
    std::wstring guest_host_path;
    const long tr = static_cast<long>(win10xbox::vfs::TranslateWin32Path(Widen(lc.guest), &guest_host_path));
    unsigned long long guest_size = 0;
    if (tr != 0 || !FileSize(guest_host_path, &guest_size))
    {
        Fail("the guest program was not found: " + lc.guest +
                 (tr == 0 ? " (looked at " + Narrow(guest_host_path) + ")" : " (the path is not on a drive the VFS maps)"),
             "put the program into LocalState\\machine (C:\\ in the guest) or change \"guest\" in LocalState\\launch.json");
        return;
    }
    report.Set("guest.host_path", Narrow(guest_host_path));
    report.Set("guest.size", Fmt("%llu", guest_size));
    if (!lc.cwd.empty())
    {
        std::wstring cwd_host;
        if (static_cast<long>(win10xbox::vfs::TranslateWin32Path(Widen(lc.cwd), &cwd_host)) != 0 || !DirExists(cwd_host))
            diag::Line("warning: cwd %s does not exist in the machine folder", lc.cwd.c_str());
    }
    if (stop.load())
    {
        Fail("stopped by the shell before the guest started (" + StopReason() + ")");
        return;
    }

    // ---- 5. the display backend (created by the shell on the UI thread) ---------------------------
    {
        const winexbox::DisplayConfig dc = display->QueryDisplay();
        report.Set("display.backend", display->Name());
        report.Set("display.gpu", dc.gpu.name);
        if (!dc.monitors.empty())
        {
            const winexbox::MonitorInfo& m = dc.monitors[0];
            report.Set("display.monitor",
                       Fmt("%s %ux%u@%u", m.device_name.c_str(), m.current.width, m.current.height, m.current.refresh_hz));
        }
        diag::Line("display backend '%s' (%s), input pump %s", display->Name(), dc.gpu.name.c_str(),
                   lc.input_pump ? "on" : "off");
        // The guest's screen for THIS program: the entry's .display / display=, else launch.json guest_display. "auto" is
        // the output mode. The backend was created with launch.json's choice; a program's own choice changes it here
        // (the presenter follows the new size on its next tick).
        const std::string want = !entry_display.empty() ? entry_display : lc.guest_display;
        winexbox::DisplayMode gm;
        gm.width = info.display.width;
        gm.height = info.display.height;
        gm.refresh_hz = info.display.refresh_hz;
        if (want != "auto" && winexbox::ParseDisplayMode(want, &gm))
        {
            if (!winexbox::SetHeadlessMode(*display, gm)) diag::Line("display: cannot set the guest display %s", want.c_str());
        }
        else if (want == "auto" && !entry_display.empty())
        {
            winexbox::SetHeadlessMode(*display, gm);   // the entry asks for the output mode explicitly
        }
        report.Set("guest_display", Fmt("%ux%u@%u requested=%s", gm.width, gm.height, gm.refresh_hz, want.c_str()));
    }

    // ---- 6. launch ---------------------------------------------------------------------------------------------
    // Launch(wait = false) + WaitForGuest is what Launch(wait = true) does
    // internally (it just calls WaitForGuest(INFINITE)); the poll loop gives us
    // the stop and watchdog checks. The worker returns to the host after the
    // guest started (run_on_new_thread), the guest runs on its own threads.
    win10xbox::process::LaunchOptions opts;
    opts.image_path = Widen(lc.guest);
    opts.command_line = BuildCommandLine(opts.image_path, lc);
    opts.current_directory = Widen(lc.cwd);
    opts.run_on_new_thread = true;
    opts.wait = false;

    // From here on the guest may own PEB fields: keep status.txt open so the
    // verdict can still be written through the handle.
    report.HoldOpen();
    report.Set("phase", "launching the guest");
    report.SetVerdict("RUNNING (the guest is being launched; this line is replaced when it exits)");
    report.Set("guest.started_utc", TimestampUtc());
    report.Flush();
    diag::Line("process::Launch(%s) cmdline=%s", Narrow(opts.image_path).c_str(), Narrow(opts.command_line).c_str());
    // Process-level teardown (docs/DECISIONS.md E9), as in host-win32: a guest exit freezes the guest and pool threads
    // (never TerminateThread) and the process ends with TerminateProcess after the report and the restart request.
    // Before Launch: the baseline of host threads is taken there.
    win10xbox::process::SetProcessTeardown(true);
    const int launch_rc = win10xbox::process::Launch(opts);
    diag::Line("process::Launch -> %d (%s)", launch_rc, Hex32(launch_rc).c_str());
    if (launch_rc != 0)
    {
        report.ReleaseHold();
        report.Set("launch_status", Fmt("%d (%s)", launch_rc, Hex32(launch_rc).c_str()));
        win10xbox::dispatch::Shutdown();
        win10xbox::Shutdown();
        Fail(Fmt("process::Launch failed before the guest ran (%s)", Hex32(launch_rc).c_str()),
             "see the 'process' and 'loader' lines in trace.log; set \"trace\": \"+process,+loader,+vfs\" in "
             "LocalState\\launch.json for more");
        return;
    }

    teardown_on.store(true);   // a guest is running: from here the process ends by TerminateProcess
    guest_running.store(true);
    guest_start_tick = GetTickCount64();
    report.Set("phase", "guest running");
    report.SetVerdict("RUNNING (the guest has been launched; this line is replaced when it exits)");
    report.Flush();   // through the held handle
    const ULONGLONG deadline = lc.timeout_sec != 0 ? guest_start_tick + static_cast<ULONGLONG>(lc.timeout_sec) * 1000ull : 0;
    if (deadline != 0) diag::Line("watchdog: %u s", static_cast<unsigned>(lc.timeout_sec));

    int exit_code = 0;
    for (;;)
    {
        const ULONGLONG a = GetTickCount64();
        const int rc = win10xbox::process::WaitForGuest(kPollMs);
        const ULONGLONG waited = GetTickCount64() - a;
        // WaitForGuest reports a timeout as STATUS_TIMEOUT, which is also a possible
        // guest exit code: a real exit does not take the whole poll interval.
        const bool poll_timeout = static_cast<DWORD>(rc) == static_cast<DWORD>(STATUS_TIMEOUT) && waited + 20 >= kPollMs;
        if (!poll_timeout)
        {
            exit_code = rc;
            break;
        }
        if (stop.load())
        {
            const std::string why = StopReason();
            report.Set("guest.runtime_ms", Fmt("%llu", static_cast<unsigned long long>(GetTickCount64() - guest_start_tick)));
            EndProcessAfterFinalStatus("STOPPED (" + why + "; the guest was still running)", 0x57A9);
            return;   // not reached
        }
        if (deadline != 0 && GetTickCount64() >= deadline)
        {
            report.Set("guest.runtime_ms", Fmt("%llu", static_cast<unsigned long long>(GetTickCount64() - guest_start_tick)));
            report.Set("hint", "the guest did not exit within \"timeout_sec\"; the last 'relay' lines in trace.log (set "
                               "\"trace\": \"+relay\") show where it is stuck");
            if (chain_all)
            {
                // The block is written first (the screen shows the running guest), then the guest is ended and the host's
                // PEB restored: with the guest's PEB (ActivationContextData) installed the WinRT activation behind
                // RequestRestartAsync fails with 0x800736b2 (ERROR_SXS_INVALID_ACTCTXDATA_FORMAT). The verdict stays TIMEOUT.
                RecordAllRun(true, 0, CaptureScreen());
                win10xbox::process::AbortGuest(0x102);   // freezes the guest and pool threads, terminates nothing
                guest_code.store(0x102);
                win10xbox::process::RestoreHostPebAfterFreeze();   // all threads that could run guest code are frozen
                guest_running.store(false);
                diag::Line("launcher: guest ended after the timeout, PEB is %s",
                           win10xbox::process::GuestOwnsPeb() ? "still the guest's" : "the host's");
                if (chain_more && restart_fn)
                {
                    const std::string arg = NextRestartArg();
                    diag::Line("launcher: guest timed out, restarting the app for the next program (argument \"%s\")", arg.c_str());
                    report.Set("launcher.restart", "requested after the timeout: " + arg);
                    report.Flush();
                    diag::FlushLog();
                    const bool guest_peb = win10xbox::process::GuestOwnsPeb();
                    const std::string why = restart_fn(arg);   // does not return when granted
                    diag::Line("ERR launcher: restart after the timeout failed: %s (PEB at the call: %s)", why.c_str(),
                               guest_peb ? "guest" : "host");
                    // The restart may be granted although the call reports otherwise (RestartPending, or the 30 s poll
                    // ran out): the next process then starts while this one is still alive. Writing the summary now
                    // would put the header into report.txt under it, so wait to be ended first.
                    AwaitRestartKill();
                    report.Set("launcher.restart", "FAILED after the timeout: " + why);
                    report.Flush();
                }
                FinishAllChain();
            }
            EndProcessAfterFinalStatus(Fmt("FAIL (guest watchdog timeout after %u s)", static_cast<unsigned>(lc.timeout_sec)),
                                       0x7E57);
            return;   // not reached
        }
    }

    // ---- 7. wind down: the guest has exited, the PEB is the host's again ---------------------------------------
    const ULONGLONG runtime_ms = GetTickCount64() - guest_start_tick;
    guest_running.store(false);
    guest_code.store(exit_code);
    // The guest threads and the pool workers started meanwhile are frozen (core did it on the exit); only now does the
    // host's PEB view come back, for the result screen and RequestRestartAsync.
    win10xbox::process::RestoreHostPebAfterFreeze();
    diag::Line("guest exited with code %d (%s) after %llu ms", exit_code, Hex32(exit_code).c_str(),
               static_cast<unsigned long long>(runtime_ms));
    {
        // What the guest did through the win32k surface (core/user): all zero for a console guest.
        const win10xbox::user::Stats us = win10xbox::user::GetStats();
        const std::string text =
            Fmt("windows created=%llu destroyed=%llu, messages posted=%llu dispatched=%llu, callbacks=%llu, paints=%llu, "
                "flushes=%llu, gdi objects=%llu, unimplemented win32u calls=%llu",
                static_cast<unsigned long long>(us.windows_created), static_cast<unsigned long long>(us.windows_destroyed),
                static_cast<unsigned long long>(us.messages_posted), static_cast<unsigned long long>(us.messages_dispatched),
                static_cast<unsigned long long>(us.callbacks), static_cast<unsigned long long>(us.paints),
                static_cast<unsigned long long>(us.flushes), static_cast<unsigned long long>(us.gdi_objects),
                static_cast<unsigned long long>(us.unimplemented));
        report.Set("user", text);
        diag::Line("user: %s", text.c_str());
    }
    const std::string all_screen = chain_all ? CaptureScreen() : std::string();   // before the guest's windows go
    win10xbox::dispatch::Shutdown();   // dumps the per-syscall relay statistics into trace.log
    win10xbox::Shutdown();             // also detaches the trace sink
    report.ReleaseHold();
    if (h_out != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(h_out);
        stdout_handle.store(INVALID_HANDLE_VALUE);
        CloseHandle(h_out);
    }
    if (h_in != INVALID_HANDLE_VALUE) CloseHandle(h_in);

    unsigned long long out_bytes = 0;
    FileSize(stdout_path, &out_bytes);
    report.Set("guest.exit_code", Fmt("%d (%s)", exit_code, Hex32(exit_code).c_str()));
    report.Set("guest.runtime_ms", Fmt("%llu", static_cast<unsigned long long>(runtime_ms)));
    report.Set("stdout_bytes", Fmt("%llu", out_bytes));
    report.Set("input.events", sink.Summary());
    report.Set("finished_utc", TimestampUtc());
    report.Set("phase", "finished");
    std::string verdict;
    if (lc.has_expect_exit_code)
    {
        const bool pass = exit_code == lc.expect_exit_code;
        verdict = pass ? Fmt("PASS (the guest exited with the expected code %d)", exit_code)
                       : Fmt("FAIL (the guest exited with code %d, expected %d)", exit_code, lc.expect_exit_code);
        if (!pass)
            report.Set("hint", "the guest ran but did not exit with the expected code; read stdout.txt and trace.log, "
                               "then try \"guest\": \"hello_cui.exe\" (console subsystem) or set \"trace\": \"+all\"");
    }
    else
    {
        verdict = Fmt("EXITED (the guest exited with code %d; set \"expect_exit_code\" in launch.json for a PASS/FAIL verdict)",
                      exit_code);
    }
    report.SetVerdict(verdict);
    diag::Line("verdict: %s", verdict.c_str());
    report.Flush();
    if (chain_all) RecordAllRun(false, exit_code, all_screen);
    ShowLauncherResult(verdict, true, exit_code);
    diag::FlushLog();
}

// ----------------------------------------------------------------------
// Host
// ----------------------------------------------------------------------
void ReadDisplaySettings(const std::wstring& local_state, const std::wstring& package_dir, std::string* display_mode,
                         std::string* guest_display, std::string* scaling)
{
    LaunchConfig lc;
    std::string source, error;
    *display_mode = *guest_display = "auto";
    if (scaling) *scaling = "system";
    if (!LoadLaunchConfig(local_state, package_dir + L"\\payload", &lc, &source, &error)) return;
    if (!lc.display_mode.empty()) *display_mode = lc.display_mode;
    if (scaling) *scaling = lc.output_scaling.empty() ? std::string("system") : lc.output_scaling;
    if (!lc.guest_display.empty()) *guest_display = lc.guest_display;
}

Host::Host(const HostInfo& info) : impl_(new Impl(info))
{
    winexbox::BackendConfig bc;
    bc.backend = win10xbox::config::DisplayBackend::Headless;   // D8: the D3D12 / CoreWindow backend is M6
    // The backend's monitor is the GUEST's screen (guest_display); the output mode is the presenter's swap chain.
    bc.width = Clamp(info.display.guest_width, 640, 7680);
    bc.height = Clamp(info.display.guest_height, 360, 4320);
    bc.refresh_hz = Clamp(info.display.guest_refresh_hz, 24, 240);
    impl_->display = winexbox::SelectBackend(bc);
    impl_->display->SetEventSink(&impl_->sink);
    // core/user allocates window surfaces from this backend and composes the frame the shell's
    // presenter shows; it must be set before process::Launch (the worker does not exist yet).
    win10xbox::user::SetDisplayDriver(impl_->display.get());
    impl_->display_desc = Fmt("%s backend, virtual monitor %ux%u@%u (output %ux%u@%u)", impl_->display->Name(),
                              bc.width, bc.height, bc.refresh_hz, info.display.width, info.display.height, info.display.refresh_hz);
}

Host::~Host()
{
    // The shell leaks its Host on purpose (the process owns it until it exits);
    // if one is ever destroyed, make sure a joinable thread cannot call std::terminate.
    if (impl_ && impl_->worker.joinable()) impl_->worker.detach();
    win10xbox::user::SetDisplayDriver(nullptr);
}

bool Host::StartWorker()
{
    try
    {
        impl_->worker = std::thread([this] { impl_->RunWorker(); });
        return true;
    }
    catch (...)
    {
        impl_->report.SetVerdict("FAIL (the host worker thread could not be created)");
        impl_->report.Flush();
        return false;
    }
}

void Host::AttachLauncher(Launcher* launcher)
{
    impl_->launcher = launcher;
}

void Host::SetRestart(bool restarted, int run, const std::string& select, int all_k, RestartFn fn)
{
    impl_->restarted = restarted;
    impl_->restart_all_k = all_k;
    impl_->run_index = run;
    impl_->restart_select = select;
    impl_->restart_fn = std::move(fn);
}

bool Host::Finished() const { return impl_->finished.load(); }
void Host::NoteFinalShot() { impl_->final_shot_done.store(true); }
bool Host::ExitWhenDone() const { return impl_->exit_when_done.load(); }
bool Host::GuestRunning() const { return impl_->guest_running.load(); }
bool Host::StopRequested() const { return impl_->stop.load(); }

void Host::JoinWorker()
{
    if (impl_->worker.joinable()) impl_->worker.join();
}

void Host::RequestStop(const char* reason)
{
    {
        std::lock_guard<std::mutex> lock(impl_->reason_mu);
        if (impl_->stop_reason.empty() && reason != nullptr) impl_->stop_reason = reason;
    }
    impl_->stop.store(true);
    diag::Line("shell: stop requested (%s)", reason ? reason : "?");
}

void Host::EmergencyExit(const char* reason)
{
    diag::Line("shell: emergency exit (%s)", reason ? reason : "?");
    impl_->report.Set("phase", "finished");
    impl_->report.Set("input.events", impl_->sink.Summary());
    impl_->report.SetVerdict(std::string("STOPPED (") + (reason ? reason : "emergency exit") + ")");
    impl_->report.Flush();
    diag::FlushLog();
    TerminateProcess(GetCurrentProcess(), 0x57A9);
    for (;;) Sleep(1000);   // TerminateProcess does not return
}

winexbox::IWinexboxDriver& Host::Display()
{
    return *impl_->display;
}

bool Host::InputPumpEnabled() const
{
    return impl_->input_enabled.load();
}

void Host::InjectInput(const winexbox::InputEvent& ev)
{
    if (!impl_->input_enabled.load()) return;
    impl_->display->InjectInput(ev);
}

void Host::PumpDisplay()
{
    impl_->display->ProcessEvents(0);
}

void Host::NoteLifecycle(const char* what)
{
    diag::Line("shell: %s", what);
}

void Host::NoteOutputScaling(const std::string& state)
{
    {
        std::lock_guard<std::mutex> g(impl_->osc_mu);
        impl_->output_scaling = state;
    }
    impl_->report.Set("output_scaling", state);
    impl_->report.Flush();
}

void Host::NoteDisplay(const std::string& presenter_state)
{
    impl_->report.Set("display.presenter", presenter_state);
    impl_->report.Flush();
}

void Host::NoteSuspending()
{
    diag::Line("shell: suspending (input events so far: %llu)", static_cast<unsigned long long>(impl_->sink.Total()));
    diag::FlushLog();
    const HANDLE out = impl_->stdout_handle.load();
    if (out != INVALID_HANDLE_VALUE) FlushFileBuffers(out);
}
}  // namespace w10x_uwp
