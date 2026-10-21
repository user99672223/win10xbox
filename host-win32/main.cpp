// win10xbox console host.
//   win10xbox [--machine <dir>] [--debug <spec>] [--cwd <guestdir>] [--version] <program.exe> [args...]
//
// <program.exe> is a guest path ("C:\guest\app.exe"), or a host path that lies
// under the machine folder (it is untranslated to the guest view), or any other
// host path: then its directory is mapped as drive Z: for this run (only
// without the VFS module; with it, use machine.json "drives").
// Without --machine the machine folder is <directory of win10xbox.exe>\machine.
// The process exit code is the guest's.
#include "win10xbox/core.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/trace.h"
#include <windows.h>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

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
