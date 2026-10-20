// win32-host: baseline. Runs the probe core as an ordinary desktop process
// (environment 1 - "full Windows"). Writes the JSON report next to the exe (or
// to %PROBE_OUT%), streams each record to stdout, and optionally POSTs the final
// report to a laptop listener (PROBE_POST=http://host:port/path).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include "probe.h"

namespace probe { void http_post_report(const std::string& url, const std::string& json); }

static std::wstring out_path() {
    char buf[1024];
    DWORD n = GetEnvironmentVariableA("PROBE_OUT", buf, sizeof buf);
    if (n && n < sizeof buf) return probe::widen(std::string(buf, n));
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    size_t slash = p.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
    return dir + L"\\probe-report.json";
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    probe::Reporter reporter;
    probe::EnvInfo env = probe::capture_env("win32");
    if (env.environment == "baseline-win32" || env.environment.empty()) {
        // keep default label
    }
    reporter.set_env(env);

    std::wstring json = out_path();
    reporter.set_json_path(json);

    int total = 0;
    reporter.set_on_emit([&](const probe::Record& r) {
        ++total;
        std::printf("[%-7s] %-2s %-28s %s\n", probe::status_name(r.status),
                    r.category.c_str(), r.name.c_str(), r.detail.c_str());
        std::fflush(stdout);
    });

    std::printf("== Xbox Dev Mode capability probe (win32 baseline) ==\n");
    std::printf("env=%s device=%s os=%s arch=%s\n", env.environment.c_str(),
                env.device.c_str(), env.os_build.c_str(), env.arch.c_str());
    std::printf("report -> %ls\n\n", json.c_str());

    probe::run_all(reporter);
    reporter.flush();

    std::printf("\n== done: %d records -> %ls ==\n", total, json.c_str());

    char posturl[1024];
    DWORD pn = GetEnvironmentVariableA("PROBE_POST", posturl, sizeof posturl);
    if (pn && pn < sizeof posturl) {
        std::string url(posturl, pn);
        std::printf("POST -> %s\n", url.c_str());
        probe::http_post_report(url, reporter.to_json());
    }
    return 0;
}
