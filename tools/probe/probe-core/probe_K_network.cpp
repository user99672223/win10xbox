// probe_K_network.cpp - category K: networking.
//
// Question answered: what can an AppContainer (UWP / Xbox Dev Mode) process do
// on the network versus the desktop baseline? Key architectural facts probed:
//   * outbound TCP (needs internetClient) and DNS
//   * inbound/server sockets (needs privateNetworkClientServer)
//   * loopback (AppContainer blocks 127.0.0.1 by default)
//   * UDP bind, raw sockets (admin-only), adapter enumeration
//
// Non-destructive: no payload is ever sent, no connection is accept()ed, every
// socket is closed on every path, and every blocking step is bounded (<= 3 s).
// Links only ws2_32 (already linked by both hosts); iphlpapi is resolved
// dynamically. Sonnet
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Winsock must precede windows.h (which probe.h pulls in).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "probe.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace probe {
namespace {

constexpr int kConnectTimeoutMs  = 3000;  // outbound connect
constexpr int kLoopbackTimeoutMs = 2000;  // loopback connect (blocked => dropped)
constexpr int kDnsTimeoutMs      = 3000;  // getaddrinfo worker wait

// Shared across probes within one register_K_network() call.
bool g_wsa_ok = false;
int  g_wsa_err = 0;
bool g_dns_thread_outstanding = false;  // a timed-out getaddrinfo is still running

// ---------------------------------------------------------------- helpers ----

std::string wsa_detail(const std::string& what, int err) {
    return what + ": WSA error " + std::to_string(err) + " (" +
           error_text(static_cast<uint32_t>(err)) + ")";
}

// Generic mapping of a Winsock error to a probe Status.
Status classify(int err) {
    switch (err) {
        case 0:
            return Status::OK;
        case WSAEACCES:
        case ERROR_ACCESS_DENIED:
            return Status::DENIED;
        case WSAETIMEDOUT:
        case WSAECONNREFUSED:   // reached the network, nothing listening
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
        case WSAENETDOWN:
        case WSAECONNRESET:
            return Status::INFO;
        case WSAEAFNOSUPPORT:
        case WSAEPROTONOSUPPORT:
        case WSAESOCKTNOSUPPORT:
        case WSAEPROTOTYPE:
        case WSAEOPNOTSUPP:
            return Status::NOTIMPL;
        default:
            return Status::WARN;
    }
}

// Record a failure: status from classify(), WSA number in errorcode, text in detail.
void fail(Record& rec, const std::string& what, int err) {
    rec.errorcode = static_cast<uint32_t>(err);
    rec.status = classify(err);
    rec.detail = wsa_detail(what, err);
    if (rec.status == Status::DENIED)
        rec.detail += " - blocked by sandbox (missing network capability?)";
}

void skip_no_wsa(Record& rec) {
    rec.status = Status::SKIPPED;
    rec.errorcode = static_cast<uint32_t>(g_wsa_err);
    rec.detail = "WSAStartup failed (error " + std::to_string(g_wsa_err) +
                 "); network probe skipped";
}

// RAII socket so no path leaks a handle.
struct Sock {
    SOCKET s;
    explicit Sock(SOCKET x = INVALID_SOCKET) : s(x) {}
    ~Sock() { reset(); }
    Sock(const Sock&) = delete;
    Sock& operator=(const Sock&) = delete;
    bool ok() const { return s != INVALID_SOCKET; }
    void reset() {
        if (s != INVALID_SOCKET) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
};

sockaddr_in make_v4(const char* ip, unsigned short port) {
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (ip) inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

// Non-blocking connect bounded by select(). Returns 0 when connected,
// WSAETIMEDOUT on timeout, otherwise the WSA error. Never sends data.
int connect_timed(SOCKET s, const sockaddr* sa, int len, int ms) {
    u_long nb = 1;
    if (ioctlsocket(s, FIONBIO, &nb) == SOCKET_ERROR) return WSAGetLastError();
    if (connect(s, sa, len) == 0) return 0;
    int e = WSAGetLastError();
    if (e != WSAEWOULDBLOCK) return e;

    fd_set wf, ef;
    FD_ZERO(&wf);
    FD_ZERO(&ef);
    FD_SET(s, &wf);
    FD_SET(s, &ef);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int n = select(0, nullptr, &wf, &ef, &tv);
    if (n == SOCKET_ERROR) return WSAGetLastError();
    if (n == 0) return WSAETIMEDOUT;

    int soerr = 0;
    int sl = sizeof(soerr);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &sl) ==
        SOCKET_ERROR)
        return WSAGetLastError();
    if (soerr != 0) return soerr;
    return FD_ISSET(s, &wf) ? 0 : WSAECONNREFUSED;
}

// TCP socket bound to 127.0.0.1:0 and listening. On success s is valid and
// port is the ephemeral port; on failure s is closed and (stage, err) say where.
struct ListenResult {
    SOCKET s = INVALID_SOCKET;
    int port = 0;
    int err = 0;
    const char* stage = "";
};

ListenResult make_listener() {
    ListenResult lr;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        lr.err = WSAGetLastError();
        lr.stage = "socket";
        return lr;
    }
    sockaddr_in a = make_v4("127.0.0.1", 0);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        lr.err = WSAGetLastError();
        lr.stage = "bind";
        closesocket(s);
        return lr;
    }
    if (listen(s, 4) == SOCKET_ERROR) {
        lr.err = WSAGetLastError();
        lr.stage = "listen";
        closesocket(s);
        return lr;
    }
    sockaddr_in b = {};
    int bl = sizeof(b);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&b), &bl) == 0) lr.port = ntohs(b.sin_port);
    lr.s = s;
    return lr;
}

// Render up to four resolved addresses as "a,b,c,d"; count = total entries.
std::string format_addrs(const ADDRINFOA* res, int& count) {
    std::string out;
    count = 0;
    for (const ADDRINFOA* p = res; p; p = p->ai_next) {
        ++count;
        if (count > 4) continue;
        char buf[INET6_ADDRSTRLEN] = {};
        bool ok = false;
        if (p->ai_family == AF_INET && p->ai_addr) {
            in_addr a = reinterpret_cast<const sockaddr_in*>(p->ai_addr)->sin_addr;
            ok = inet_ntop(AF_INET, &a, buf, sizeof(buf)) != nullptr;
        } else if (p->ai_family == AF_INET6 && p->ai_addr) {
            in6_addr a = reinterpret_cast<const sockaddr_in6*>(p->ai_addr)->sin6_addr;
            ok = inet_ntop(AF_INET6, &a, buf, sizeof(buf)) != nullptr;
        }
        if (ok) {
            if (!out.empty()) out += ",";
            out += buf;
        }
    }
    return out;
}

// getaddrinfo cannot be cancelled and may block for many seconds when no DNS
// is reachable, so run it on a worker thread and wait at most `ms`.
// Returns false on timeout (the worker is left to finish on its own).
struct GaiState {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    int rc = -1;
    int count = 0;
    std::string addrs;
};

bool gai_timed(const std::string& host, const std::string& svc, int family, int flags,
               int ms, int& rc, int& count, std::string& addrs) {
    auto st = std::make_shared<GaiState>();
    auto work = [st, host, svc, family, flags]() {
        ADDRINFOA hints = {};
        hints.ai_family = family;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = flags;
        ADDRINFOA* res = nullptr;
        int r = 0, n = 0;
        std::string a;
        try {
            r = getaddrinfo(host.c_str(), svc.c_str(), &hints, &res);
            if (r == 0) a = format_addrs(res, n);
        } catch (...) {
            r = WSASYSCALLFAILURE;
        }
        if (res) freeaddrinfo(res);
        std::lock_guard<std::mutex> lk(st->mu);
        st->rc = r;
        st->count = n;
        st->addrs = a;
        st->done = true;
        st->cv.notify_all();
    };
    try {
        std::thread(work).detach();
    } catch (...) {
        work();  // could not spawn a thread: fall back to a direct call
    }
    std::unique_lock<std::mutex> lk(st->mu);
    if (!st->cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return st->done; })) {
        g_dns_thread_outstanding = true;
        return false;
    }
    rc = st->rc;
    count = st->count;
    addrs = st->addrs;
    return true;
}

}  // namespace

// --------------------------------------------------------------- register ----

void register_K_network(Reporter& r) {
    g_wsa_ok = false;
    g_wsa_err = 0;
    g_dns_thread_outstanding = false;

    // 1. Winsock init. Everything below SKIPs if this fails.
    r.run("K", "wsastartup", [](Record& rec) {
        WSADATA wsa = {};
        int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            g_wsa_ok = false;
            g_wsa_err = rc;
            fail(rec, "WSAStartup(2.2)", rc);
            return;
        }
        g_wsa_ok = true;
        rec.status = Status::OK;
        rec.value = "version=" + std::to_string(LOBYTE(wsa.wVersion)) + "." +
                    std::to_string(HIBYTE(wsa.wVersion)) +
                    ";high=" + std::to_string(LOBYTE(wsa.wHighVersion)) + "." +
                    std::to_string(HIBYTE(wsa.wHighVersion)) +
                    ";desc=" + std::string(wsa.szDescription);
        rec.detail = "WSAStartup(2.2) succeeded";
    });

    // 2. Numeric getaddrinfo: pure parse, no DNS involved.
    r.run("K", "getaddrinfo-numeric", [](Record& rec) {
        if (!g_wsa_ok) { skip_no_wsa(rec); return; }
        ADDRINFOA hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST;
        ADDRINFOA* res = nullptr;
        int rc = getaddrinfo("1.1.1.1", "443", &hints, &res);
        if (rc != 0) {
            fail(rec, "getaddrinfo(1.1.1.1, AI_NUMERICHOST)", rc);
            return;
        }
        int n = 0;
        std::string a = format_addrs(res, n);
        freeaddrinfo(res);
        rec.status = Status::OK;
        rec.value = "addr=" + a + ";count=" + std::to_string(n);
        rec.detail = "numeric host resolved without DNS";
    });

    // 3. Real DNS resolution (needs a reachable resolver + network capability).
    r.run("K", "dns-resolve", [](Record& rec) {
        if (!g_wsa_ok) { skip_no_wsa(rec); return; }
        int rc = 0, n = 0;
        std::string addrs;
        if (!gai_timed("example.com", "443", AF_UNSPEC, 0, kDnsTimeoutMs, rc, n, addrs)) {
            rec.status = Status::INFO;
            rec.errorcode = WSAETIMEDOUT;
            rec.value = "timeout";
            rec.detail = "getaddrinfo(example.com) did not return within " +
                         std::to_string(kDnsTimeoutMs) + " ms";
            return;
        }
        if (rc == 0) {
            rec.status = Status::OK;
            rec.value = "ip=" + addrs + ";count=" + std::to_string(n);
            rec.detail = "DNS resolution works";
            return;
        }
        fail(rec, "getaddrinfo(example.com)", rc);
        if (rc == WSAHOST_NOT_FOUND || rc == WSANO_DATA || rc == WSANO_RECOVERY) {
            rec.status = Status::MISSING;
            rec.detail += " - name not resolvable (no DNS or capability missing)";
        } else if (rc == WSATRY_AGAIN) {
            rec.status = Status::INFO;
            rec.detail += " - DNS temporarily unavailable (no resolver reachable)";
        }
    });

    // 4. Outbound TCP. Numeric target so DNS is not a factor. Handshake only.
    r.run("K", "tcp-connect-outbound", [](Record& rec) {
        if (!g_wsa_ok) { skip_no_wsa(rec); return; }
        Sock s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!s.ok()) {
            fail(rec, "socket(AF_INET, SOCK_STREAM)", WSAGetLastError());
            return;
        }
        sockaddr_in a = make_v4("1.1.1.1", 443);
        int e = connect_timed(s.s, reinterpret_cast<sockaddr*>(&a), sizeof(a),
                              kConnectTimeoutMs);
        if (e == 0) {
            rec.status = Status::OK;
            rec.value = "connected=1;peer=1.1.1.1:443";
            rec.detail = "outbound TCP handshake completed (no payload sent)";
        } else if (e == WSAETIMEDOUT) {
            rec.status = Status::INFO;
            rec.errorcode = static_cast<uint32_t>(e);
            rec.value = "timeout";
            rec.detail = "connect to 1.1.1.1:443 timed out after " +
                         std::to_string(kConnectTimeoutMs) + " ms";
        } else {
            fail(rec, "connect(1.1.1.1:443)", e);
            rec.value = "connected=0;peer=1.1.1.1:443";
        }
    });

    // 5. Inbound: can we create a server socket at all? (key AppContainer fact)
    r.run("K", "tcp-listen-inbound", [](Record& rec) {
        if (!g_wsa_ok) { skip_no_wsa(rec); return; }
        ListenResult lr = make_listener();
        if (lr.s == INVALID_SOCKET) {
            fail(rec, std::string(lr.stage) + "(127.0.0.1:0)", lr.err);
            rec.value = std::string("failed_stage=") + lr.stage;
            return;
        }
        Sock s(lr.s);
        rec.status = Status::OK;
        rec.value = "bound_port=" + std::to_string(lr.port);
        rec.detail = "bind+listen on 127.0.0.1:0 succeeded (no accept performed)";
    });

    // 6. Loopback: listener + connect from a second socket in the same process.
    r.run("K", "loopback-connect", [](Record& rec) {
        if (!g_wsa_ok) { skip_no_wsa(rec); return; }
        ListenResult lr = make_listener();
        if (lr.s == INVALID_SOCKET) {
            rec.status = Status::SKIPPED;
            rec.errorcode = static_cast<uint32_t>(lr.err);
            rec.detail = std::string("listen step failed at ") + lr.stage + ": " +
                         wsa_detail("WSA", lr.err) + "; loopback not testable";
            return;
        }
        Sock listener(lr.s);
        Sock c(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!c.ok()) {
            fail(rec, "socket(AF_INET, SOCK_STREAM)", WSAGetLastError());
            return;
        }
        sockaddr_in a = make_v4("127.0.0.1", static_cast<unsigned short>(lr.port));
        int e = connect_timed(c.s, reinterpret_cast<sockaddr*>(&a), sizeof(a),
                              kLoopbackTimeoutMs);
        if (e == 0) {
            rec.status = Status::OK;
            rec.value = "loopback=ok;port=" + std::to_string(lr.port);
            rec.detail = "connect to own 127.0.0.1 listener succeeded (no data sent)";
        } else if (e == WSAETIMEDOUT) {
            rec.status = Status::INFO;
            rec.errorcode = static_cast<uint32_t>(e);
            rec.value = "loopback=blocked;reason=timeout";
            rec.detail = "loopback connect timed out after " +
                         std::to_string(kLoopbackTimeoutMs) +
                         " ms (packets dropped - typical AppContainer loopback isolation)";
        } else {
            fail(rec, "connect(127.0.0.1:" + std::to_string(lr.port) + ")", e);
            rec.value = "loopback=blocked;err=" + std::to_string(e);
        }
    });

    // 7. UDP socket bound to the wildcard address, ephemeral port.
    r.run("K", "udp-socket", [](Record& rec) {
        if (!g_wsa_ok) { skip_no_wsa(rec); return; }
        Sock s(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (!s.ok()) {
            fail(rec, "socket(AF_INET, SOCK_DGRAM)", WSAGetLastError());
            return;
        }
        sockaddr_in a = make_v4("0.0.0.0", 0);
        if (bind(s.s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
            fail(rec, "bind(0.0.0.0:0, UDP)", WSAGetLastError());
            return;
        }
        sockaddr_in b = {};
        int bl = sizeof(b);
        int port = 0;
        if (getsockname(s.s, reinterpret_cast<sockaddr*>(&b), &bl) == 0) port = ntohs(b.sin_port);
        rec.status = Status::OK;
        rec.value = "bound_port=" + std::to_string(port);
        rec.detail = "UDP socket created and bound to 0.0.0.0:0";
    });

    // 8. Raw ICMP socket: expected DENIED for non-admin / AppContainer.
    r.run("K", "socket-raw", [](Record& rec) {
        if (!g_wsa_ok) { skip_no_wsa(rec); return; }
        Sock s(socket(AF_INET, SOCK_RAW, IPPROTO_ICMP));
        if (!s.ok()) {
            fail(rec, "socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)", WSAGetLastError());
            return;
        }
        rec.status = Status::OK;
        rec.value = "raw=opened";
        rec.detail = "raw ICMP socket was created (elevated / unsandboxed); closed, nothing sent";
    });

    // 9. Adapter enumeration via iphlpapi (resolved dynamically, no link lib).
    r.run("K", "iphlpapi-adapters", [](Record& rec) {
        if (!g_wsa_ok) { skip_no_wsa(rec); return; }
        using GetAdaptersAddressesFn =
            ULONG(WINAPI*)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
        HMODULE loaded = nullptr;
        auto fn = reinterpret_cast<GetAdaptersAddressesFn>(
            resolve("iphlpapi.dll", "GetAdaptersAddresses"));
        if (!fn) {
            // resolve() uses LoadLibraryA, which UWP may refuse; retry with the W form.
            loaded = LoadLibraryExW(L"iphlpapi.dll", nullptr, 0);
            if (loaded)
                fn = reinterpret_cast<GetAdaptersAddressesFn>(
                    GetProcAddress(loaded, "GetAdaptersAddresses"));
        }
        if (!fn) {
            DWORD le = GetLastError();
            rec.status = Status::MISSING;
            rec.errorcode = le;
            rec.detail = "GetAdaptersAddresses not resolvable from iphlpapi.dll: " +
                         error_text(le);
            if (loaded) FreeLibrary(loaded);
            return;
        }

        const ULONG flags =
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buf;
        ULONG rc = ERROR_BUFFER_OVERFLOW;
        for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
            buf.assign(size, 0);
            rc = fn(AF_UNSPEC, flags, nullptr,
                    reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &size);
        }

        if (rc == NO_ERROR) {
            int total = 0, up = 0;
            for (auto* p = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()); p; p = p->Next) {
                ++total;
                if (p->OperStatus == IfOperStatusUp) ++up;
            }
            rec.status = Status::OK;
            rec.value = "adapters=" + std::to_string(total) + ";up=" + std::to_string(up);
            rec.detail = "GetAdaptersAddresses(AF_UNSPEC) succeeded";
        } else if (rc == ERROR_NO_DATA) {
            rec.status = Status::INFO;
            rec.errorcode = rc;
            rec.value = "adapters=0";
            rec.detail = "GetAdaptersAddresses reported no adapters";
        } else {
            rec.errorcode = rc;
            rec.status = (rc == ERROR_ACCESS_DENIED) ? Status::DENIED : Status::WARN;
            rec.detail = "GetAdaptersAddresses failed: " + error_text(rc);
        }
        if (loaded) FreeLibrary(loaded);
    });

    // Balance the single WSAStartup. If a timed-out DNS worker is still inside
    // getaddrinfo, skip cleanup so it cannot be pulled out from under it.
    if (g_wsa_ok && !g_dns_thread_outstanding) WSACleanup();
    g_wsa_ok = false;
}

}  // namespace probe
