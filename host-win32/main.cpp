// win10xbox console host.
//   win10xbox [--machine <dir>] [--debug <spec>] <program.exe> [args...]
// M0: initialises the core and prints its version. The loader (M1) and the
// dispatcher (M2) attach here.
#include "win10xbox/core.h"
#include "win10xbox/trace.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static void Usage() {
    std::fputs("usage: win10xbox [--machine <dir>] [--debug <spec>] [--version] <program.exe> [args...]\n", stderr);
}

int wmain(int argc, wchar_t** argv) {
    win10xbox::config::Config cfg;
    std::vector<std::wstring> rest;
    bool version_only = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--machine" && i + 1 < argc) cfg.machine_root = argv[++i];
        else if (a == L"--debug" && i + 1 < argc) SetEnvironmentVariableW(L"WIN10XBOX_DEBUG", argv[++i]);
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
    if (int rc = win10xbox::Initialize(cfg)) { std::fprintf(stderr, "initialise failed: %d\n", rc); return 2; }
    if (rest.empty()) {
        std::printf("win10xbox %d.%d.%d (%s): hello from host-win32\n", v.major, v.minor, v.patch, v.git);
        win10xbox::Shutdown();
        return 0;
    }
    std::fprintf(stderr, "win10xbox: loader not yet available (M1); cannot run %ls\n", rest[0].c_str());
    win10xbox::Shutdown();
    return 3;
}
