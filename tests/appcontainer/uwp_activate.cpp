// uwp_activate.cpp - start an INSTALLED packaged (UWP) app by AppUserModelId, wait for it, and say exactly what happened.
//
//   uwp_activate <AUMID> [--package-full-name <name>] [--timeout <seconds>] [--args <string>]
//
// Used by host-uwp.yml's `run` job: after Add-AppxPackage, "can this runner LAUNCH the app?" must end in a concrete HRESULT or
// a PID, never in silence. Start-Process shell:AppsFolder\<aumid> reports nothing when it fails; this calls the same shell
// service (IApplicationActivationManager::ActivateApplication) and prints its HRESULT.
//
//   --package-full-name   (best effort) switch the package's lifecycle management off first, like a debugger attach does
//                         (IPackageDebugSettings::EnableDebugging, the call behind plmdebug.exe /enableDebug). Without it a
//                         UWP app that is not visibly in the foreground is SUSPENDED a few seconds after activation, and
//                         host-uwp's worker thread, which runs the guest, would be frozen mid-run: a headless runner would
//                         then report "no verdict" for a reason that has nothing to do with the layer.
//
// Exit status: 0 the app was activated and its process ended (the process's own exit code is printed);
//              10 activation failed (HRESULT printed); 11 the app was still running when --timeout expired (left running);
//              12 COM could not be set up.
//
// Build (CI, MSVC):  cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /utf-8 uwp_activate.cpp /link ole32.lib
// Build (check):     x86_64-w64-mingw32-clang++ -std=c++17 -municode -static uwp_activate.cpp -o uwp_activate.exe -lole32 -luuid
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

void Say(const char* fmt, ...) {
    char buf[2000];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    std::fputs("uwp_activate: ", stdout);
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

// "0x80270254 = ... message ..." for an HRESULT.
std::string HrText(HRESULT hr) {
    char head[32];
    std::snprintf(head, sizeof head, "0x%08lX", static_cast<unsigned long>(hr));
    std::string s = head;
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                   static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    if (n != 0 && msg != nullptr) {
        std::wstring m(msg, n);
        while (!m.empty() && (m.back() == L'\r' || m.back() == L'\n' || m.back() == L' ')) m.pop_back();
        s += " = " + Narrow(m);
    }
    if (msg != nullptr) LocalFree(msg);
    return s;
}

// CLSID_PackageDebugSettings / IID_IPackageDebugSettings (shobjidl_core.idl). Declared here, only as far as EnableDebugging.
const CLSID kClsidPackageDebugSettings = {0xB1AEC16F, 0x2383, 0x4852, {0xB0, 0xE9, 0x8F, 0x0B, 0x1D, 0xC6, 0x6B, 0x4D}};
const IID kIidPackageDebugSettings = {0xF27C3930, 0x8029, 0x4AD1, {0x94, 0xE3, 0x3D, 0xBA, 0x41, 0x78, 0x10, 0xC1}};
struct IPackageDebugSettingsLite : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnableDebugging(LPCWSTR packageFullName, LPCWSTR debuggerCommandLine, WCHAR* environment) = 0;
    virtual HRESULT STDMETHODCALLTYPE DisableDebugging(LPCWSTR packageFullName) = 0;
};

// CLSID_ApplicationActivationManager / IID_IApplicationActivationManager
const CLSID kClsidActivationManager = {0x45BA127D, 0x10A8, 0x46EA, {0x8A, 0xB7, 0x56, 0xEA, 0x90, 0x78, 0x94, 0x3C}};
const IID kIidActivationManager = {0x2E941141, 0x7F97, 0x4756, {0xBA, 0x1D, 0x9D, 0xEC, 0xDE, 0x89, 0x4A, 0x3D}};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring aumid, full_name, args;
    DWORD timeout_s = 240;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--timeout" && i + 1 < argc) timeout_s = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (a == L"--package-full-name" && i + 1 < argc) full_name = argv[++i];
        else if (a == L"--args" && i + 1 < argc) args = argv[++i];
        else if (aumid.empty() && a.rfind(L"--", 0) != 0) aumid = a;
        else { std::fprintf(stderr, "uwp_activate: unexpected argument %s\n", Narrow(a).c_str()); return 12; }
    }
    if (aumid.empty()) {
        std::fputs("usage: uwp_activate <AUMID> [--package-full-name <name>] [--timeout <s>] [--args <string>]\n", stderr);
        return 12;
    }

    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Say("AUMID %s; this process runs in session %lu (session 0 is the services session: no desktop, no UWP shell)",
        Narrow(aumid).c_str(), static_cast<unsigned long>(session));

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { Say("FATAL: CoInitializeEx: %s", HrText(hr).c_str()); return 12; }

    if (!full_name.empty()) {
        IPackageDebugSettingsLite* pds = nullptr;
        hr = CoCreateInstance(kClsidPackageDebugSettings, nullptr, CLSCTX_ALL, kIidPackageDebugSettings,
                              reinterpret_cast<void**>(&pds));
        if (SUCCEEDED(hr) && pds != nullptr) {
            hr = pds->EnableDebugging(full_name.c_str(), nullptr, nullptr);
            Say("lifecycle management switched off for %s (EnableDebugging): %s", Narrow(full_name).c_str(),
                SUCCEEDED(hr) ? "yes" : HrText(hr).c_str());
            pds->Release();
        } else {
            Say("note: IPackageDebugSettings is not available (%s); the app may be suspended when it is not in the foreground",
                HrText(hr).c_str());
        }
    }

    IApplicationActivationManager* aam = nullptr;
    hr = CoCreateInstance(kClsidActivationManager, nullptr, CLSCTX_LOCAL_SERVER, kIidActivationManager, reinterpret_cast<void**>(&aam));
    if (FAILED(hr) || aam == nullptr) {
        Say("FATAL: no ApplicationActivationManager (CoCreateInstance): %s", HrText(hr).c_str());
        return 12;
    }
    CoAllowSetForegroundWindow(aam, nullptr);

    DWORD pid = 0;
    hr = aam->ActivateApplication(aumid.c_str(), args.empty() ? nullptr : args.c_str(), AO_NOERRORUI, &pid);
    aam->Release();
    if (FAILED(hr)) {
        Say("ACTIVATION FAILED: ActivateApplication returned %s", HrText(hr).c_str());
        return 10;
    }
    Say("ACTIVATED: process %lu", static_cast<unsigned long>(pid));

    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        Say("cannot open process %lu to wait for it (error %lu); waiting is not possible, sleeping %lu s instead",
            static_cast<unsigned long>(pid), static_cast<unsigned long>(GetLastError()), static_cast<unsigned long>(timeout_s));
        Sleep(timeout_s * 1000);
        return 11;
    }
    const ULONGLONG t0 = GetTickCount64();
    const DWORD waited = WaitForSingleObject(proc, timeout_s * 1000);
    const double secs = static_cast<double>(GetTickCount64() - t0) / 1000.0;
    if (waited != WAIT_OBJECT_0) {
        Say("STILL RUNNING after %.0f s (--timeout); leaving it running", secs);
        CloseHandle(proc);
        return 11;
    }
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    Say("EXITED: process %lu ended with exit code %lu (0x%08lX) after %.1f s", static_cast<unsigned long>(pid),
        static_cast<unsigned long>(code), static_cast<unsigned long>(code), secs);
    CloseHandle(proc);
    return 0;
}
