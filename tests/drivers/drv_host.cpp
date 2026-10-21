// drv_host: win10xbox host-win32 plus the driver host, for the driver end-to-end tests.
//   drv_host --machine <dir> --driver <path\to\x.sys> [--driver ...] [--debug <spec>] <guest program>
// Same bootstrap as host-win32/main.cpp (win10xbox::Initialize + process::Launch) with
// drivers::Initialize() and drivers::LoadDriver() before the guest starts. When the orchestrator
// wires the driver host into win10xbox.exe this file becomes a thin wrapper or goes away.
#include "win10xbox/core.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/drivers.h"
#include "win10xbox/trace.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    win10xbox::config::Config cfg;
    std::vector<std::wstring> drivers, rest;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--machine" && i + 1 < argc) cfg.machine_root = argv[++i];
        else if (a == L"--driver" && i + 1 < argc) drivers.emplace_back(argv[++i]);
        else if (a == L"--debug" && i + 1 < argc) SetEnvironmentVariableW(L"WIN10XBOX_DEBUG", argv[++i]);
        else { for (; i < argc; ++i) rest.emplace_back(argv[i]); }
    }
    if (rest.empty() || cfg.machine_root.empty()) {
        std::fputs("usage: drv_host --machine <dir> [--driver <x.sys>]... [--debug <spec>] <guest program>\n", stderr);
        return 64;
    }
    wchar_t full[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(cfg.machine_root.c_str(), MAX_PATH * 2, full, nullptr);
    if (n && n < MAX_PATH * 2) cfg.machine_root = full;
    while (cfg.machine_root.size() > 3 && cfg.machine_root.back() == L'\\') cfg.machine_root.pop_back();
    if (int rc = win10xbox::Initialize(cfg)) { std::fprintf(stderr, "initialise failed: %d\n", rc); return 2; }

    NTSTATUS st = win10xbox::drivers::Initialize();
    if (!NT_SUCCESS(st)) { std::fprintf(stderr, "drivers::Initialize failed: %08lx\n", static_cast<unsigned long>(st)); return 3; }
    for (const std::wstring& d : drivers) {
        win10xbox::drivers::DriverInfo info;
        st = win10xbox::drivers::LoadDriver(d.c_str(), nullptr, &info);
        if (!NT_SUCCESS(st)) {
            std::fprintf(stderr, "LoadDriver(%ls) failed: %08lx\n", d.c_str(), static_cast<unsigned long>(st));
            win10xbox::drivers::Shutdown();
            return 4;
        }
        std::printf("driver loaded: %ls (%zu stubbed imports)\n", info.name.c_str(), info.stubbed_imports);
    }
    std::fflush(stdout);

    win10xbox::process::LaunchOptions opts;
    opts.image_path = rest[0];
    opts.command_line = L"\"" + rest[0] + L"\"";
    for (size_t i = 1; i < rest.size(); ++i) opts.command_line += L" " + rest[i];
    opts.run_on_new_thread = true;
    opts.wait = true;
    int rc = win10xbox::process::Launch(opts);
    if (rc < 0) std::fprintf(stderr, "drv_host: could not start %ls: status %08x\n", opts.image_path.c_str(), rc);
    win10xbox::drivers::PoolStats before = win10xbox::drivers::GetPoolStats();
    win10xbox::drivers::Shutdown();
    std::printf("driver host: %llu pool blocks live at guest exit, shut down cleanly\n",
                static_cast<unsigned long long>(before.blocks_outstanding));
    std::fflush(stdout);
    win10xbox::dispatch::Shutdown();
    win10xbox::Shutdown();
    return rc;
}
