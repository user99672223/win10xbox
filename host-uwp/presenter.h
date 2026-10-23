// presenter.h - puts the composed desktop of core/user (win10xbox::user::ComposeFrame) on
// the Xbox display: D3D11 device + a DXGI flip-sequential swap chain on the app's CoreWindow,
// one BGRA texture the size of the virtual screen, upload + copy + Present each frame
// (blit.h). Also writes LocalState\screen.bmp. Plain Win32 + D3D11/DXGI, no WinRT: main.cpp
// hands the CoreWindow over as an IUnknown (winrt::get_unknown).
//
// Thread rules (docs/DECISIONS.md "UWP host (M5) lessons": D3D11CreateDevice HANGS when called
// on the UI thread of a Game-bucket app, measured on the console):
//   * StartDevice() spawns a worker thread that calls D3D11CreateDevice (hardware, then WARP).
//     The UI thread never calls it. If it never returns, the thread is simply abandoned and the
//     app stays headless.
//   * Everything else runs on the UI thread: the swap chain is created for the CoreWindow on the
//     UI thread (as in the DirectX UWP DeviceResources pattern) the first Tick() after the device
//     is ready, and Tick() composes, uploads, copies and Presents from the shell's 60 Hz pump.
//   * main.cpp runs StartDevice and waits (pumping CoreWindow events) for the device and the first
//     Present BEFORE the Host's worker starts, so the D3D/DXGI DLLs are loaded before the guest
//     takes over the PEB's loader fields (host_diag.h "PEB note").
// Nothing here throws or ends the process: every failure leaves a note (TakeNotes) with the
// HRESULT, and the app keeps running headless.
#pragma once

#include <windows.h>
#include <unknwn.h>   // IUnknown (WIN32_LEAN_AND_MEAN leaves it out)

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace w10x_uwp
{
    // An optional source of frames that replaces the composed desktop while Active() (the
    // launcher's list / result screens, launcher.h). All three are called on the UI thread, from
    // Presenter::Tick.
    class FrameSource
    {
    public:
        virtual ~FrameSource() = default;
        virtual bool Active() = 0;                    // draw this source instead of user::ComposeFrame
        virtual std::uint64_t Serial() = 0;           // changes whenever Draw would produce another picture
        virtual void Draw(std::uint32_t* bgra, int width, int height) = 0;   // top-down, width * 4 bytes per row
    };

    class Presenter
    {
    public:
        enum class Device
        {
            NotStarted,
            Pending,   // the device thread has not returned yet
            Ready,     // a device exists (hardware or WARP)
            Failed,    // both driver types failed: headless
        };

        Presenter();
        ~Presenter();
        Presenter(const Presenter&) = delete;
        Presenter& operator=(const Presenter&) = delete;

        // UI thread, once: the CoreWindow (as IUnknown) and its size in physical pixels, and the
        // size of the virtual screen ComposeFrame composes for (normally the same).
        void Configure(IUnknown* core_window, int window_width, int window_height, int screen_width,
                       int screen_height);
        // Opens LocalState\screen.bmp for the periodic screenshots. Call while no guest runs: the
        // file is rewritten through this handle later, never re-opened by path while the guest
        // owns the PEB.
        void OpenScreenshot(const std::wstring& path);

        void SetFrameSource(FrameSource* source);   // UI thread; nullptr = none (the default)

        void StartDevice();          // spawns the device thread (UI thread, once)
        Device DeviceState() const;  // any thread
        bool Presenting() const;     // a swap chain exists and the last Present succeeded

        // One pump round (UI thread). `live`: the guest may still change the screen; compose
        // when FrameSerial() moved or every ~500 ms, present the frame, and write screen.bmp every
        // 5 s. Not live (the guest has exited): keep presenting the last frame, compose no more.
        void Tick(bool live);

        // Once, after the guest has exited (UI thread): LocalState\screen.bmp via
        // user::WriteScreenshot, or the last presented frame if that has nothing to compose.
        void FinalScreenshot();

        // Lines that describe what happened since the last call (device created / failed with the
        // HRESULT, swap chain, Present failures, screenshots), for trace.log / status.txt.
        std::vector<std::string> TakeNotes();
        // One line for status.txt: the current state.
        std::string Describe() const;

        struct Impl;

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace w10x_uwp
