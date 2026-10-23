// ws2_32's "unix side" (dlls/ws2_32/ws2_32_private.h, enum ws_unix_funcs): the host-resolver entry points Wine's PE
// ws2_32 reaches through WINE_UNIX_CALL. Each one runs on the host's Winsock (docs/DECISIONS.md, N1) and fills the
// caller's buffer in the layout Wine's own unix side produces; every host failure is returned as the real WSA error.
// Sockets themselves do not come through here: they are \Device\Afd ioctls (core/afd).
//
// Everything here is reached from guest threads in the host view (UnixCallDispatcher), so ordinary host code is fine,
// but there are no function-local statics with dynamic initialisation on these paths (docs/DISPATCHER.md).
#include <winsock2.h>
#include <ws2tcpip.h>
#include "internal.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_AFD
#include "win10xbox/afd.h"
#endif
#include <cstring>

namespace win10xbox::dispatch::detail {
namespace {

constexpr NTSTATUS kWsaEOpNotSupp = 10045;   // only without core/afd
constexpr NTSTATUS kInsufficientBuffer = 122;   // ERROR_INSUFFICIENT_BUFFER: ws2_32 retries with *size bytes

struct GetAddrInfoParams {
    const char* node; const char* service; const addrinfo* hints; addrinfo* info; unsigned int* size;
};
struct GetHostByAddrParams { const void* addr; int len; int family; hostent* host; unsigned int* size; };
struct GetHostByNameParams { const char* name; hostent* host; unsigned int* size; };
struct GetNameInfoParams {
    const sockaddr* addr; int addr_len; char* host; DWORD host_len; char* serv; DWORD serv_len; int flags;
};

#if WIN10XBOX_HAVE_AFD

// Wine's layout: header, alias array, address array, addresses, alias strings, then the name.
NTSTATUS CopyHostent(const hostent* src, hostent* dst, unsigned int* size) {
    unsigned int need = sizeof(hostent), aliases = 0, addrs = 0;
    need += static_cast<unsigned int>(std::strlen(src->h_name)) + 1;
    for (; src->h_aliases && src->h_aliases[aliases]; ++aliases)
        need += sizeof(char*) + static_cast<unsigned int>(std::strlen(src->h_aliases[aliases])) + 1;
    need += sizeof(char*);
    for (; src->h_addr_list && src->h_addr_list[addrs]; ++addrs) need += sizeof(char*) + src->h_length;
    need += sizeof(char*);
    if (*size < need) { *size = need; return kInsufficientBuffer; }
    std::memset(dst, 0, need);
    dst->h_addrtype = src->h_addrtype;
    dst->h_length = src->h_length;
    char* p = reinterpret_cast<char*>(dst + 1);
    dst->h_aliases = reinterpret_cast<char**>(p);
    p += (aliases + 1) * sizeof(char*);
    dst->h_addr_list = reinterpret_cast<char**>(p);
    p += (addrs + 1) * sizeof(char*);
    for (unsigned i = 0; i < addrs; ++i) {
        dst->h_addr_list[i] = p;
        std::memcpy(p, src->h_addr_list[i], src->h_length);
        p += src->h_length;
    }
    for (unsigned i = 0; i < aliases; ++i) {
        const size_t len = std::strlen(src->h_aliases[i]) + 1;
        dst->h_aliases[i] = p;
        std::memcpy(p, src->h_aliases[i], len);
        p += len;
    }
    dst->h_name = p;
    std::strcpy(p, src->h_name);
    return 0;
}

NTSTATUS WINAPI U_getaddrinfo(void* args) {
    auto* p = static_cast<GetAddrInfoParams*>(args);
    afd::EnsureWinsock();
    addrinfo hints{};
    if (p->hints) {
        hints = *p->hints;
        hints.ai_canonname = nullptr; hints.ai_addr = nullptr; hints.ai_next = nullptr;
    }
    const char* service = p->service && !p->service[0] ? "0" : p->service;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(p->node, service, p->hints ? &hints : nullptr, &res);
    if (rc) {
        addrinfo num_hints{}, *num = nullptr;
        num_hints.ai_flags = AI_NUMERICHOST;
        const int num_rc = ::getaddrinfo("127.0.0.1", nullptr, &num_hints, &num);   // diagnostic: is the host resolver alive?
        if (num) ::freeaddrinfo(num);
        TRACE(dispatch, "ws2_32 unix getaddrinfo(%s, %s) -> host error %d (numeric 127.0.0.1 -> %d, GetLastError %lu)",
              p->node ? p->node : "(null)", service ? service : "(null)", rc, num_rc, GetLastError());
        return rc;
    }
    size_t need = 0;
    for (addrinfo* a = res; a; a = a->ai_next)
        need += sizeof(addrinfo) + (a->ai_canonname ? std::strlen(a->ai_canonname) + 1 : 0) + a->ai_addrlen;
    if (*p->size < need) {
        *p->size = static_cast<unsigned>(need);
        ::freeaddrinfo(res);
        return kInsufficientBuffer;
    }
    std::memset(p->info, 0, need);
    addrinfo* dst = p->info;
    addrinfo* prev = nullptr;
    for (addrinfo* a = res; a; a = a->ai_next) {
        char* next = reinterpret_cast<char*>(dst + 1);
        dst->ai_flags = a->ai_flags;
        dst->ai_family = a->ai_family;
        dst->ai_socktype = p->hints ? p->hints->ai_socktype : a->ai_socktype;
        dst->ai_protocol = p->hints ? p->hints->ai_protocol : a->ai_protocol;
        if (a->ai_canonname) {
            const size_t l = std::strlen(a->ai_canonname) + 1;
            dst->ai_canonname = next;
            std::memcpy(next, a->ai_canonname, l);
            next += l;
        }
        dst->ai_addrlen = a->ai_addrlen;
        dst->ai_addr = reinterpret_cast<sockaddr*>(next);
        std::memcpy(next, a->ai_addr, a->ai_addrlen);
        next += a->ai_addrlen;
        if (prev) prev->ai_next = dst;
        prev = dst;
        dst = reinterpret_cast<addrinfo*>(next);
    }
    ::freeaddrinfo(res);
    return 0;
}

// The host's gethostbyname/gethostbyaddr return a per-thread buffer; it is copied out before returning.
NTSTATUS WINAPI U_gethostbyaddr(void* args) {
    auto* p = static_cast<GetHostByAddrParams*>(args);
    afd::EnsureWinsock();
    const hostent* h = ::gethostbyaddr(static_cast<const char*>(p->addr), p->len, p->family);
    if (!h) return WSAGetLastError();
    return CopyHostent(h, p->host, p->size);
}

NTSTATUS WINAPI U_gethostbyname(void* args) {
    auto* p = static_cast<GetHostByNameParams*>(args);
    afd::EnsureWinsock();
    const hostent* h = ::gethostbyname(p->name);
    if (!h) return WSAGetLastError();
    return CopyHostent(h, p->host, p->size);
}

NTSTATUS WINAPI U_getnameinfo(void* args) {
    auto* p = static_cast<GetNameInfoParams*>(args);
    afd::EnsureWinsock();
    return ::getnameinfo(p->addr, p->addr_len, p->host, p->host_len, p->serv, p->serv_len, p->flags);
}

#else

NTSTATUS WINAPI U_getaddrinfo(void*) { return kWsaEOpNotSupp; }
NTSTATUS WINAPI U_gethostbyaddr(void*) { return kWsaEOpNotSupp; }
NTSTATUS WINAPI U_gethostbyname(void*) { return kWsaEOpNotSupp; }
NTSTATUS WINAPI U_getnameinfo(void*) { return kWsaEOpNotSupp; }

#endif

}  // namespace

NTSTATUS WINAPI Ws2UnixGetAddrInfo(void* args) { return U_getaddrinfo(args); }
NTSTATUS WINAPI Ws2UnixGetHostByAddr(void* args) { return U_gethostbyaddr(args); }
NTSTATUS WINAPI Ws2UnixGetHostByName(void* args) { return U_gethostbyname(args); }
NTSTATUS WINAPI Ws2UnixGetNameInfo(void* args) { return U_getnameinfo(args); }

}  // namespace win10xbox::dispatch::detail
