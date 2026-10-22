// launcher.h - the on-console launcher of host-uwp: a list of guest programs drawn on the TV with
// the existing D3D11 presenter (it is a presenter.h FrameSource, so no new rendering path), chosen
// with a gamepad (Windows.Gaming.Input) or the keyboard, and, after the guest has exited, a
// result screen with its exit code and the last 20 lines of its stdout.
//
// Threads. The worker (host.cpp) calls Pick / SetRunning / ShowResult / WaitDismiss and blocks in
// the first and last; the UI thread (main.cpp) calls OnKey / PollGamepad from its pump, and the
// presenter calls the FrameSource methods from Tick, also on the UI thread. All state is behind
// one mutex.
//
// Screens. While a guest runs the launcher is inactive and the presenter shows the composed
// desktop of core/user as before; Pick() and ShowResult() make it active again.
//
// Text is drawn with a built-in 5x7 bitmap font (launcher.cpp), scaled to the screen.
//
// Keys: Up / Down move, Enter / Space / gamepad A launch, Esc / Backspace / gamepad B go back from
// the result screen. A gamepad is read with Windows.Gaming.Input; when none is ever seen the
// VK_GAMEPAD_* keys the CoreWindow reports are used instead.
#pragma once

#include "presenter.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace w10x_uwp
{
    struct LauncherEntry
    {
        std::string name;        // file name, "hello.exe"
        std::wstring path;       // host path of the file (package payload\guest or LocalState\guests)
        bool from_localstate = false;
    };

    struct LauncherResult
    {
        std::string name;
        std::string verdict;               // one line, "PASS (...)" / "EXITED (...)" / "FAIL (...)"
        bool ran = false;                  // the guest was started and exited (exit_code is valid)
        long exit_code = 0;
        std::vector<std::string> lines;    // last lines of the guest's stdout
    };

    // Last `n` non-empty-tail lines of a text file (the file's last 64 KiB are looked at), control
    // characters replaced by '?'. Empty when the file cannot be read.
    std::vector<std::string> TailLines(const std::wstring& path, std::size_t n);

    // LocalState\launcher_result.txt: the test oracle of the CI gate and a record for the owner.
    bool WriteLauncherResult(const std::wstring& path, const LauncherResult& r);

    class Launcher final : public FrameSource
    {
    public:
        Launcher();
        ~Launcher() override;
        Launcher(const Launcher&) = delete;
        Launcher& operator=(const Launcher&) = delete;

        // ---- worker thread ------------------------------------------------------------
        // Shows the list and blocks until an entry is chosen (returns its index) or `stop` is set
        // (returns -1). `preselect` is the entry the cursor starts on (a file name, may be empty).
        int Pick(const std::vector<LauncherEntry>& entries, const std::string& preselect, const std::atomic<bool>& stop);
        void SetRunning();                         // the list / result screen is dismissed: the guest owns the screen
        void ShowResult(const LauncherResult& r);  // result screen up (does not block)
        // Blocks until the result screen is dismissed (true) or `stop` is set (false).
        bool WaitDismiss(const std::atomic<bool>& stop);

        // ---- UI thread -----------------------------------------------------------------------
        bool Consumes();                           // a launcher screen is up: keys are for it, not the guest
        void OnKey(unsigned vk);                   // a CoreWindow key press (VirtualKey value)
        void PollGamepad();                        // Windows.Gaming.Input, once per pump round

        // ---- FrameSource (UI thread, from Presenter::Tick) -------------------------------------------
        bool Active() override;
        std::uint64_t Serial() override;
        void Draw(std::uint32_t* bgra, int width, int height) override;

    private:
        enum class Mode { Idle, List, Result };
        enum Action { kUp, kDown, kAccept, kBack };
        void Apply(Action a);   // mu_ held

        std::mutex mu_;
        Mode mode_ = Mode::Idle;
        std::uint64_t serial_ = 1;
        std::vector<LauncherEntry> entries_;
        int sel_ = 0;
        int chosen_ = -1;
        bool dismissed_ = false;
        LauncherResult result_;

        unsigned pad_prev_ = 0;     // UI thread only
        bool pad_seen_ = false;     // a gamepad was seen through Windows.Gaming.Input
    };
}  // namespace w10x_uwp
