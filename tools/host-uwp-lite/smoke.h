// smoke.h - the host-uwp-lite smoke harness. Plain Win32 + the win10xbox public
// API (core.h / config.h / dispatch.h / trace.h); deliberately no WinRT, so the
// WinRT view in main.cpp only has to find the paths and run this on a worker
// thread.
#pragma once
#include <string>

namespace uwp_lite
{
    struct HostInfo
    {
        std::wstring local_state;       // ApplicationData LocalFolder path (the only writable place)
        std::wstring package_dir;       // folder of the running exe; payload\ lives below it
        std::string  package_full_name; // only reported in result.txt; may be empty
    };

    // Runs the whole on-console M1/M2 smoke and leaves its evidence in
    // <local_state>:
    //   result.txt  verdict first (PASS / FAIL / INCOMPLETE), then key=value facts
    //   stdout.txt  everything the guest wrote to stdout/stderr
    //   trace.log   harness lines + the core's trace (WIN10XBOX_DEBUG=+all)
    //   machine\    the staged machine folder (the guest's C:\)
    // Blocks until the guest has exited (or a watchdog fires), so call it from a
    // worker thread, never the CoreWindow thread. Never throws. Returns true on
    // PASS (Initialize == 0 and the guest exited with code 42).
    //
    // If the guest hangs past the watchdog this function terminates the process
    // itself after writing the verdict: the PEB is still taken over by the guest
    // at that point, so returning into WinRT code would not be safe.
    bool RunSmoke(const HostInfo& host);
}
