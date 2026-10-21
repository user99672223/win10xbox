// env.cpp - capture environment metadata once per run (OS build, device, arch).
// Uses only APIs available in both desktop and AppContainer. OS build is read
// from the registry (CurrentVersion) with a RtlGetVersion fallback.
#include "probe.h"
#include <cstdio>

namespace probe {

static std::string reg_sz(HKEY root, const wchar_t* sub, const wchar_t* val) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return {};
    wchar_t buf[256];
    DWORD cb = sizeof buf, type = 0;
    std::string out;
    if (RegQueryValueExW(k, val, nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        out = narrow(std::wstring(buf, (cb / sizeof(wchar_t)) ? (cb / sizeof(wchar_t)) - 1 : 0));
    }
    RegCloseKey(k);
    return out;
}

static std::string reg_dword(HKEY root, const wchar_t* sub, const wchar_t* val) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return {};
    DWORD data = 0, cb = sizeof data, type = 0;
    std::string out;
    if (RegQueryValueExW(k, val, nullptr, &type, (LPBYTE)&data, &cb) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        out = std::to_string(data);
    }
    RegCloseKey(k);
    return out;
}

EnvInfo capture_env(const std::string& host) {
    EnvInfo e;
    e.host = host;

    // Environment label and optional note overridable via env vars so the same
    // binary can tag itself per run without a rebuild.
    char buf[512];
    DWORD n = GetEnvironmentVariableA("PROBE_ENVIRONMENT", buf, sizeof buf);
    if (n && n < sizeof buf) e.environment.assign(buf, n);
    else e.environment = (host == "uwp") ? "uwp-unlabeled" : "baseline-win32";
    n = GetEnvironmentVariableA("PROBE_NOTE", buf, sizeof buf);
    if (n && n < sizeof buf) e.note.assign(buf, n);

    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string major = reg_dword(HKEY_LOCAL_MACHINE, cv, L"CurrentMajorVersionNumber");
    std::string minor = reg_dword(HKEY_LOCAL_MACHINE, cv, L"CurrentMinorVersionNumber");
    std::string build = reg_sz(HKEY_LOCAL_MACHINE, cv, L"CurrentBuildNumber");
    std::string ubr   = reg_dword(HKEY_LOCAL_MACHINE, cv, L"UBR");
    if (!major.empty() && !build.empty()) {
        e.os_build = major + "." + (minor.empty() ? "0" : minor) + "." + build;
        if (!ubr.empty()) e.os_build += "." + ubr;
    }
    std::string product = reg_sz(HKEY_LOCAL_MACHINE, cv, L"ProductName");
    std::string display = reg_sz(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion");
    if (!product.empty()) {
        e.note += (e.note.empty() ? "" : "; ");
        e.note += product;
        if (!display.empty()) e.note += " " + display;
    }

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: e.arch = "x64"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: e.arch = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: e.arch = "x86"; break;
        default: e.arch = "arch#" + std::to_string(si.wProcessorArchitecture);
    }

    wchar_t name[256];
    DWORD cc = 256;
    if (GetComputerNameW(name, &cc)) e.device = narrow(std::wstring(name, cc));

    // ISO-8601 UTC timestamp.
    SYSTEMTIME st;
    GetSystemTime(&st);
    char ts[32];
    std::snprintf(ts, sizeof ts, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    e.started_utc = ts;
    return e;
}

}  // namespace probe
