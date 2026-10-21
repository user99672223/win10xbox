// http.cpp - optional report POSTer built on WinHTTP (resolved dynamically so
// neither host needs winhttp.lib and a missing DLL degrades gracefully). Used by
// both hosts to ship the final report to a laptop listener. Best-effort: any
// failure is reported to stdout/debug and swallowed - it must never abort a run.
#include "probe.h"
#include <cstdio>

// WinHTTP types we need, declared locally to avoid pulling winhttp.h into every
// TU. We resolve the entry points by name from winhttp.dll.
using HINTERNET_ = void*;

namespace probe {

namespace {
struct Url {
    bool https = false;
    std::wstring host;
    unsigned short port = 0;
    std::wstring path;
    bool ok = false;
};
Url parse_url(const std::string& s) {
    Url u;
    std::string rest = s;
    if (rest.rfind("https://", 0) == 0) { u.https = true; rest = rest.substr(8); }
    else if (rest.rfind("http://", 0) == 0) { u.https = false; rest = rest.substr(7); }
    else return u;
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    u.path = widen((slash == std::string::npos) ? "/" : rest.substr(slash));
    size_t colon = hostport.find(':');
    if (colon == std::string::npos) {
        u.host = widen(hostport);
        u.port = u.https ? 443 : 80;
    } else {
        u.host = widen(hostport.substr(0, colon));
        u.port = (unsigned short)atoi(hostport.substr(colon + 1).c_str());
    }
    u.ok = !u.host.empty();
    return u;
}
}  // namespace

void http_post_report(const std::string& url, const std::string& json) {
    Url u = parse_url(url);
    if (!u.ok) { std::printf("  http: bad url\n"); return; }

    HMODULE wh = LoadLibraryW(L"winhttp.dll");
    if (!wh) { std::printf("  http: winhttp.dll missing\n"); return; }

    auto pOpen = (HINTERNET_(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD))
        GetProcAddress(wh, "WinHttpOpen");
    auto pConnect = (HINTERNET_(WINAPI*)(HINTERNET_, LPCWSTR, unsigned short, DWORD))
        GetProcAddress(wh, "WinHttpConnect");
    auto pOpenReq = (HINTERNET_(WINAPI*)(HINTERNET_, LPCWSTR, LPCWSTR, LPCWSTR,
                                         LPCWSTR, LPCWSTR*, DWORD))
        GetProcAddress(wh, "WinHttpOpenRequest");
    auto pSend = (BOOL(WINAPI*)(HINTERNET_, LPCWSTR, DWORD, LPVOID, DWORD, DWORD,
                                DWORD_PTR))
        GetProcAddress(wh, "WinHttpSendRequest");
    auto pRecv = (BOOL(WINAPI*)(HINTERNET_, LPVOID)) GetProcAddress(wh, "WinHttpReceiveResponse");
    auto pSetOpt = (BOOL(WINAPI*)(HINTERNET_, DWORD, LPVOID, DWORD))
        GetProcAddress(wh, "WinHttpSetOption");
    auto pClose = (BOOL(WINAPI*)(HINTERNET_)) GetProcAddress(wh, "WinHttpCloseHandle");
    if (!pOpen || !pConnect || !pOpenReq || !pSend || !pRecv || !pClose) {
        std::printf("  http: winhttp entry points missing\n");
        FreeLibrary(wh);
        return;
    }

    const DWORD WINHTTP_FLAG_SECURE_ = 0x00800000;
    const DWORD WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY_ = 4;
    const DWORD WINHTTP_OPTION_SECURITY_FLAGS_ = 31;
    // Allow self-signed / cert mismatch for a laptop listener on the LAN.
    DWORD secFlags = 0x00000100 | 0x00000200 | 0x00001000 | 0x00002000;

    HINTERNET_ s = pOpen(L"xbox-probe/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY_,
                         nullptr, nullptr, 0);
    if (!s) { std::printf("  http: open failed %lu\n", GetLastError()); FreeLibrary(wh); return; }
    HINTERNET_ c = pConnect(s, u.host.c_str(), u.port, 0);
    HINTERNET_ rq = c ? pOpenReq(c, L"POST", u.path.c_str(), nullptr, nullptr,
                                 nullptr, u.https ? WINHTTP_FLAG_SECURE_ : 0) : nullptr;
    bool ok = false;
    if (rq) {
        if (u.https && pSetOpt)
            pSetOpt(rq, WINHTTP_OPTION_SECURITY_FLAGS_, &secFlags, sizeof secFlags);
        const wchar_t* hdr = L"Content-Type: application/json\r\n";
        BOOL sent = pSend(rq, hdr, (DWORD)-1L, (LPVOID)json.data(),
                          (DWORD)json.size(), (DWORD)json.size(), 0);
        if (sent && pRecv(rq, nullptr)) ok = true;
    }
    std::printf("  http: %s (gle=%lu)\n", ok ? "posted" : "failed", GetLastError());
    if (rq) pClose(rq);
    if (c) pClose(c);
    if (s) pClose(s);
    FreeLibrary(wh);
}

}  // namespace probe
