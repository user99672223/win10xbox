// win10xbox AFD: the \Device\Afd that Wine's builtin ws2_32 drives, on top of host Winsock. See afd.h and
// docs/DECISIONS.md (N1).
//
// Wine 11's ws2_32 opens "\Device\Afd" with NtOpenFile and then issues NtDeviceIoControlFile with a handful of real
// AFD codes (BIND, LISTEN, POLL, GETSOCKNAME, ...) and Wine's own IOCTL_AFD_WINE_* set (CREATE, CONNECT, RECVMSG,
// SENDMSG, setsockopt/getsockopt, ...; include/wine/afd.h). Wine answers them in wineserver; there is none here, so
// each ioctl is executed on the calling thread against a host SOCKET and completes before the call returns (event
// signalled, APC queued, IOSB filled). Blocking sockets therefore block inside the call exactly as a synchronous
// Winsock call would. Not implemented, and answered with a real STATUS_NOT_SUPPORTED plus a FIXME: truly
// asynchronous (overlapped) completion, WSAEventSelect / WSAAsyncSelect, AcceptEx, TransmitFile, interface lists.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "win10xbox/afd.h"
#include "win10xbox/dispatch.h"
#include "win10xbox/trace.h"
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <map>
#include <memory>
#include <vector>

namespace win10xbox::afd {
namespace {

using NTSTATUS_ = LONG;
constexpr NTSTATUS_ kSuccess = 0;
constexpr NTSTATUS_ kTimeout = 0x00000102;
constexpr NTSTATUS_ kPending = 0x00000103;
constexpr NTSTATUS_ kBufferOverflow = static_cast<NTSTATUS_>(0x80000005);
constexpr NTSTATUS_ kNotImplemented = static_cast<NTSTATUS_>(0xC0000002);
constexpr NTSTATUS_ kInvalidHandle = static_cast<NTSTATUS_>(0xC0000008);
constexpr NTSTATUS_ kInvalidParameter = static_cast<NTSTATUS_>(0xC000000D);
constexpr NTSTATUS_ kAccessViolation = static_cast<NTSTATUS_>(0xC0000005);
constexpr NTSTATUS_ kBufferTooSmall = static_cast<NTSTATUS_>(0xC0000023);
constexpr NTSTATUS_ kObjectNameNotFound = static_cast<NTSTATUS_>(0xC0000034);
constexpr NTSTATUS_ kNotSupported = static_cast<NTSTATUS_>(0xC00000BB);
constexpr NTSTATUS_ kAccessDenied = static_cast<NTSTATUS_>(0xC0000022);
constexpr NTSTATUS_ kObjectTypeMismatch = static_cast<NTSTATUS_>(0xC0000024);
constexpr NTSTATUS_ kDeviceNotReady = static_cast<NTSTATUS_>(0xC00000A3);
constexpr NTSTATUS_ kInsufficientResources = static_cast<NTSTATUS_>(0xC000009A);
constexpr NTSTATUS_ kAddressAlreadyAssociated = static_cast<NTSTATUS_>(0xC0000328);

constexpr std::uint32_t Ctl(std::uint32_t dev, std::uint32_t fn, std::uint32_t method) {
    return (dev << 16) | (fn << 2) | method;
}
constexpr std::uint32_t kDevBeep = 1, kDevNetwork = 0x12;
constexpr std::uint32_t WineIoc(std::uint32_t n) { return Ctl(kDevNetwork, n, 0); }

// Real AFD codes Wine 11 uses (include/wine/afd.h).
constexpr std::uint32_t kAfdBind = Ctl(kDevBeep, 0x800, 3);
constexpr std::uint32_t kAfdListen = Ctl(kDevBeep, 0x802, 3);
constexpr std::uint32_t kAfdPoll = Ctl(kDevBeep, 0x809, 0);
constexpr std::uint32_t kAfdGetSockName = Ctl(kDevBeep, 0x80b, 3);
constexpr std::uint32_t kAfdEventSelect = Ctl(kDevBeep, 0x821, 3);
constexpr std::uint32_t kAfdGetEvents = Ctl(kDevBeep, 0x822, 3);
// Wine's own.
enum : std::uint32_t {
    kWineCreate = 200, kWineAccept, kWineAcceptInto, kWineConnect, kWineShutdown, kWineRecvMsg, kWineSendMsg,
    kWineTransmit, kWineAddressListChange, kWineFionbio, kWineCompleteAsync, kWineFionread, kWineSiocatmark,
    kWineGetInterfaceList, kWineKeepaliveVals, kWineMessageSelect, kWineGetPeerName, kWineDefer, kWineGetInfo,
    kFirstOption = 219, kLastOption = 304,
};

// AFD_POLL_* flags
constexpr int kPollRead = 0x1, kPollOob = 0x2, kPollWrite = 0x4, kPollHup = 0x8, kPollReset = 0x10,
              kPollClose = 0x20, kPollAccept = 0x80, kPollConnectErr = 0x100;

#pragma pack(push, 4)
struct PollSocket { std::uint64_t socket; int flags; int status; };
struct PollParams { std::int64_t timeout; unsigned count; BOOLEAN exclusive; BOOLEAN pad[3]; PollSocket sockets[1]; };
#pragma pack(pop)
struct CreateParams { int family, type, protocol; unsigned flags; };
struct ConnectParams { int addr_len; int synchronous; };
struct RecvMsgParams {
    std::uint64_t control_ptr, addr_ptr, addr_len_ptr, ws_flags_ptr;
    int force_async; unsigned count; std::uint64_t buffers_ptr;
};
struct SendMsgParams {
    std::uint64_t addr_ptr; unsigned addr_len, ws_flags; int force_async; unsigned count; std::uint64_t buffers_ptr;
};
struct ListenParams { int unknown1, backlog, unknown2; };
struct BindParams { int unknown; };   // followed by the sockaddr
struct InfoParams { int family, type, protocol; };

struct Sock {
    SOCKET s = INVALID_SOCKET;
    int family = 0, type = 0, protocol = 0;
    bool created = false;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::map<std::uintptr_t, std::shared_ptr<Sock>>& Socks() { static std::map<std::uintptr_t, std::shared_ptr<Sock>> m; return m; }

std::shared_ptr<Sock> Find(HANDLE h) {
    AcquireSRWLockShared(&g_lock);
    auto it = Socks().find(reinterpret_cast<std::uintptr_t>(h));
    std::shared_ptr<Sock> r = it != Socks().end() ? it->second : nullptr;
    ReleaseSRWLockShared(&g_lock);
    return r;
}

HANDLE Register(std::shared_ptr<Sock> s) {
    HANDLE h = dispatch::RegisterEmulatedObject(dispatch::ObjectKind::Device, 0x414644 /* 'AFD' */, nullptr);
    if (!h) return nullptr;
    AcquireSRWLockExclusive(&g_lock);
    Socks()[reinterpret_cast<std::uintptr_t>(h)] = std::move(s);
    ReleaseSRWLockExclusive(&g_lock);
    return h;
}

// Winsock error -> the NTSTATUS Wine's server would have produced (the reverse of ws2_32's NtStatusToWSAError).
NTSTATUS_ FromWsa(int e) {
    switch (e) {
    case 0: return kSuccess;
    case WSAEINTR: return static_cast<NTSTATUS_>(0xC0000120);              // REQUEST_ABORTED -> WSAEINTR
    case WSAEBADF: case WSAENOTSOCK: return kObjectTypeMismatch;
    case WSAEACCES: return kAccessDenied;
    case WSAEFAULT: return kAccessViolation;
    case WSAEINVAL: return kInvalidParameter;
    case WSAEMFILE: case WSAENOBUFS: return kInsufficientResources;
    case WSAEWOULDBLOCK: case WSAEINPROGRESS: return kDeviceNotReady;
    case WSAEALREADY: return kAddressAlreadyAssociated;
    case WSAEMSGSIZE: return kBufferOverflow;
    case WSAEOPNOTSUPP: case WSAEPROTONOSUPPORT: case WSAESOCKTNOSUPPORT: case WSAEAFNOSUPPORT: case WSAEPFNOSUPPORT:
        return kNotSupported;
    case WSAEADDRINUSE: return static_cast<NTSTATUS_>(0xC000020A);         // ADDRESS_ALREADY_EXISTS
    case WSAEADDRNOTAVAIL: return static_cast<NTSTATUS_>(0xC0000207);      // INVALID_ADDRESS_COMPONENT
    case WSAENETDOWN: return static_cast<NTSTATUS_>(0xC0000205);           // NETWORK_BUSY (-> WSAENETDOWN)
    case WSAENETUNREACH: return static_cast<NTSTATUS_>(0xC000023C);
    case WSAEHOSTUNREACH: return static_cast<NTSTATUS_>(0xC000023D);
    case WSAEHOSTDOWN: return static_cast<NTSTATUS_>(0xC0000350);
    case WSAECONNABORTED: return static_cast<NTSTATUS_>(0xC0000241);
    case WSAECONNRESET: return static_cast<NTSTATUS_>(0xC000020D);
    case WSAECONNREFUSED: return static_cast<NTSTATUS_>(0xC0000236);
    case WSAEISCONN: return static_cast<NTSTATUS_>(0xC000023B);            // CONNECTION_ACTIVE
    case WSAENOTCONN: return static_cast<NTSTATUS_>(0xC0000140);           // INVALID_CONNECTION
    case WSAESHUTDOWN: return static_cast<NTSTATUS_>(0xC00000AB);          // PIPE_DISCONNECTED
    case WSAETIMEDOUT: return static_cast<NTSTATUS_>(0xC00000B5);          // IO_TIMEOUT
    case WSAEDISCON: return static_cast<NTSTATUS_>(0xC0000237);
    default:
        FIXME(dispatch, "no NTSTATUS for Winsock error %d, answering STATUS_INVALID_PARAMETER", e);
        return kInvalidParameter;
    }
}
NTSTATUS_ LastWsa() { return FromWsa(WSAGetLastError()); }

// ---- individual operations --------------------------------------------------------------------------------------

NTSTATUS_ DoCreate(Sock& s, const void* in, ULONG in_len) {
    if (!in || in_len < sizeof(CreateParams)) return kInvalidParameter;
    auto* p = static_cast<const CreateParams*>(in);
    if (s.created) return kInvalidParameter;
    EnsureWinsock();
    SOCKET h = ::socket(p->family, p->type, p->protocol);
    if (h == INVALID_SOCKET) return LastWsa();
    s.s = h; s.family = p->family; s.type = p->type; s.protocol = p->protocol; s.created = true;
    return kSuccess;
}

NTSTATUS_ DoBind(Sock& s, const void* in, ULONG in_len, void* out, ULONG out_len, ULONG_PTR* info) {
    if (!in || in_len < sizeof(BindParams) + 2) return kInvalidParameter;
    auto* addr = reinterpret_cast<const sockaddr*>(static_cast<const char*>(in) + sizeof(BindParams));
    if (::bind(s.s, addr, static_cast<int>(in_len - sizeof(BindParams))) == SOCKET_ERROR) return LastWsa();
    if (out && out_len) {
        int len = static_cast<int>(out_len);
        if (::getsockname(s.s, static_cast<sockaddr*>(out), &len) == 0) *info = static_cast<ULONG_PTR>(len);
    }
    return kSuccess;
}

NTSTATUS_ DoConnect(Sock& s, const void* in, ULONG in_len) {
    if (!in || in_len < sizeof(ConnectParams)) return kInvalidParameter;
    auto* p = static_cast<const ConnectParams*>(in);
    if (p->addr_len < 2 || in_len < sizeof(ConnectParams) + static_cast<ULONG>(p->addr_len)) return kInvalidParameter;
    auto* addr = reinterpret_cast<const sockaddr*>(p + 1);
    if (::connect(s.s, addr, p->addr_len) == SOCKET_ERROR) return LastWsa();
    const char* data = reinterpret_cast<const char*>(p + 1) + p->addr_len;
    const ULONG data_len = in_len - sizeof(ConnectParams) - p->addr_len;
    if (data_len && ::send(s.s, data, static_cast<int>(data_len), 0) == SOCKET_ERROR) return LastWsa();
    return kSuccess;
}

NTSTATUS_ DoAccept(Sock& s, void* out, ULONG out_len, ULONG_PTR* info) {
    if (!out || out_len < sizeof(ULONG)) return kBufferTooSmall;
    SOCKET c = ::accept(s.s, nullptr, nullptr);
    if (c == INVALID_SOCKET) return LastWsa();
    auto ns = std::make_shared<Sock>();
    ns->s = c; ns->family = s.family; ns->type = s.type; ns->protocol = s.protocol; ns->created = true;
    HANDLE h = Register(ns);
    if (!h) { ::closesocket(c); return kInsufficientResources; }
    *static_cast<ULONG*>(out) = static_cast<ULONG>(reinterpret_cast<std::uintptr_t>(h));
    *info = sizeof(ULONG);
    return kSuccess;
}

NTSTATUS_ DoName(Sock& s, bool peer, void* out, ULONG out_len, ULONG_PTR* info) {
    if (!out) return kAccessViolation;
    sockaddr_storage ss{};
    int len = sizeof ss;
    if ((peer ? ::getpeername(s.s, reinterpret_cast<sockaddr*>(&ss), &len)
              : ::getsockname(s.s, reinterpret_cast<sockaddr*>(&ss), &len)) == SOCKET_ERROR) return LastWsa();
    *info = static_cast<ULONG_PTR>(len);
    if (out_len < static_cast<ULONG>(len)) { std::memcpy(out, &ss, out_len); return kBufferTooSmall; }
    std::memcpy(out, &ss, len);
    return kSuccess;
}

NTSTATUS_ DoRecvMsg(Sock& s, const void* in, ULONG in_len, ULONG_PTR* info) {
    if (!in || in_len < sizeof(RecvMsgParams)) return kInvalidParameter;
    auto* p = static_cast<const RecvMsgParams*>(in);
    if (p->control_ptr) { FIXME(dispatch, "recvmsg control data is not supported"); return kNotSupported; }
    auto* flags_ptr = reinterpret_cast<DWORD*>(p->ws_flags_ptr);
    DWORD flags = flags_ptr ? *flags_ptr : 0, got = 0;
    int rc = ::WSARecvFrom(s.s, reinterpret_cast<WSABUF*>(p->buffers_ptr), p->count, &got, &flags,
                           reinterpret_cast<sockaddr*>(p->addr_ptr), reinterpret_cast<INT*>(p->addr_len_ptr),
                           nullptr, nullptr);
    if (rc == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEMSGSIZE) { *info = got; if (flags_ptr) *flags_ptr = flags; return kBufferOverflow; }
        return FromWsa(e);
    }
    if (flags_ptr) *flags_ptr = flags;
    *info = got;
    return kSuccess;
}

NTSTATUS_ DoSendMsg(Sock& s, const void* in, ULONG in_len, ULONG_PTR* info) {
    if (!in || in_len < sizeof(SendMsgParams)) return kInvalidParameter;
    auto* p = static_cast<const SendMsgParams*>(in);
    DWORD sent = 0;
    int rc = ::WSASendTo(s.s, reinterpret_cast<WSABUF*>(p->buffers_ptr), p->count, &sent, p->ws_flags,
                         reinterpret_cast<const sockaddr*>(p->addr_ptr), static_cast<int>(p->addr_len), nullptr,
                         nullptr);
    if (rc == SOCKET_ERROR) return LastWsa();
    *info = sent;
    return kSuccess;
}

// AFD_POLL: select() over the requested sockets, then work out which of the requested AFD events really hold.
NTSTATUS_ DoPoll(PollParams* p, ULONG in_len, ULONG out_len, ULONG_PTR* info) {
    if (!p || in_len < offsetof(PollParams, sockets) || in_len < offsetof(PollParams, sockets[p->count])
        || out_len < in_len)
        return kInvalidParameter;
    EnsureWinsock();
    const unsigned n = p->count;
    auto make_set = [n]() {
        std::vector<char> buf(offsetof(fd_set, fd_array) + sizeof(SOCKET) * (n ? n : 1), 0);
        return buf;
    };
    std::vector<char> rbuf = make_set(), wbuf = make_set(), ebuf = make_set();
    auto* rs = reinterpret_cast<fd_set*>(rbuf.data());
    auto* ws = reinterpret_cast<fd_set*>(wbuf.data());
    auto* es = reinterpret_cast<fd_set*>(ebuf.data());
    std::vector<std::shared_ptr<Sock>> socks(n);
    unsigned live = 0;
    for (unsigned i = 0; i < n; ++i) {
        PollSocket& e = p->sockets[i];
        socks[i] = Find(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(e.socket)));
        if (!socks[i] || !socks[i]->created) { e.flags = kPollClose; continue; }
        const int want = e.flags;
        e.status = 0;
        ++live;
        if (want & (kPollRead | kPollAccept | kPollHup | kPollReset)) rs->fd_array[rs->fd_count++] = socks[i]->s;
        if (want & kPollWrite) ws->fd_array[ws->fd_count++] = socks[i]->s;
        if (want & (kPollOob | kPollConnectErr)) es->fd_array[es->fd_count++] = socks[i]->s;
    }
    if (live) {
        timeval tv{}, *ptv = nullptr;
        if (p->timeout < 0 && p->timeout != INT64_MIN) {
            const std::int64_t us = (-p->timeout) / 10;
            tv.tv_sec = static_cast<long>(us / 1000000);
            tv.tv_usec = static_cast<long>(us % 1000000);
            ptv = &tv;
        } else if (p->timeout >= 0 && p->timeout != INT64_MAX) {
            if (p->timeout) FIXME(dispatch, "absolute poll timeouts are treated as a zero timeout");
            ptv = &tv;
        }
        int rc = ::select(0, rs->fd_count ? rs : nullptr, ws->fd_count ? ws : nullptr, es->fd_count ? es : nullptr,
                          ptv);
        if (rc == SOCKET_ERROR) return LastWsa();
    }
    unsigned hits = 0;
    for (unsigned i = 0; i < n; ++i) {
        PollSocket& e = p->sockets[i];
        if (!socks[i] || !socks[i]->created) { ++hits; continue; }
        const int want = e.flags;
        const SOCKET s = socks[i]->s;
        int got = 0;
        if (__WSAFDIsSet(s, rs)) {
            int acceptconn = 0, l = sizeof acceptconn;
            ::getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&acceptconn), &l);
            if (acceptconn) got |= kPollAccept;
            else if (socks[i]->type == SOCK_STREAM) {
                char c;
                int r = ::recv(s, &c, 1, MSG_PEEK);
                if (r > 0) got |= kPollRead;
                else if (r == 0) got |= kPollHup;
                else if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAECONNABORTED) got |= kPollReset;
                else got |= kPollRead;
            } else got |= kPollRead;
        }
        if (__WSAFDIsSet(s, ws)) got |= kPollWrite;
        if (__WSAFDIsSet(s, es)) {
            int err = 0, l = sizeof err;
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &l);
            got |= err ? kPollConnectErr : kPollOob;
        }
        e.flags = got & (want | kPollClose);
        if (e.flags) ++hits;
    }
    *info = offsetof(PollParams, sockets[n]);
    return hits ? kSuccess : kTimeout;
}

struct OptionDef { std::uint32_t get, set; int level; int name; };
const OptionDef kOptions[] = {
    {219, 0, SOL_SOCKET, SO_ACCEPTCONN},    {220, 221, SOL_SOCKET, SO_BROADCAST},
    {222, 0, SOL_SOCKET, SO_ERROR},         {223, 224, SOL_SOCKET, SO_KEEPALIVE},
    {225, 226, SOL_SOCKET, SO_LINGER},      {227, 228, SOL_SOCKET, SO_OOBINLINE},
    {230, 229, SOL_SOCKET, SO_RCVBUF},      {232, 231, SOL_SOCKET, SO_RCVTIMEO},
    {233, 234, SOL_SOCKET, SO_REUSEADDR},   {236, 235, SOL_SOCKET, SO_SNDBUF},
    {237, 238, SOL_SOCKET, SO_SNDTIMEO},    {297, 298, SOL_SOCKET, SO_EXCLUSIVEADDRUSE},
    {284, 285, IPPROTO_TCP, TCP_NODELAY},   {258, 259, IPPROTO_IP, IP_TOS},
    {260, 261, IPPROTO_IP, IP_TTL},         {252, 253, IPPROTO_IP, IP_MULTICAST_TTL},
    {250, 251, IPPROTO_IP, IP_MULTICAST_LOOP}, {275, 276, IPPROTO_IPV6, IPV6_UNICAST_HOPS},
    {279, 280, IPPROTO_IPV6, IPV6_V6ONLY},
};

bool FindOption(std::uint32_t n, const OptionDef** def, bool* is_set) {
    for (const OptionDef& d : kOptions) {
        if (d.get == n) { *def = &d; *is_set = false; return true; }
        if (d.set == n) { *def = &d; *is_set = true; return true; }
    }
    return false;
}

NTSTATUS_ DoIoctl(Sock& s, std::uint32_t code, void* in, ULONG in_len, void* out, ULONG out_len, ULONG_PTR* info) {
    if (code == kAfdPoll) return kNotSupported;   // handled by the caller (needs no socket)
    if (code == WineIoc(kWineCreate)) return DoCreate(s, in, in_len);
    if (code == WineIoc(kWineCompleteAsync)) {   // ws2_32 also uses this as "is this a socket?" and "fail with this"
        return in && in_len >= sizeof(NTSTATUS_) ? *static_cast<NTSTATUS_*>(in) : kInvalidParameter;
    }
    if (!s.created) return kInvalidParameter;
    if (code == kAfdBind) return DoBind(s, in, in_len, out, out_len, info);
    if (code == kAfdListen) {
        if (!in || in_len < sizeof(ListenParams)) return kInvalidParameter;
        return ::listen(s.s, static_cast<ListenParams*>(in)->backlog) == SOCKET_ERROR ? LastWsa() : kSuccess;
    }
    if (code == kAfdGetSockName) return DoName(s, false, out, out_len, info);
    if (code == WineIoc(kWineGetPeerName)) return DoName(s, true, out, out_len, info);
    if (code == WineIoc(kWineConnect)) return DoConnect(s, in, in_len);
    if (code == WineIoc(kWineAccept)) return DoAccept(s, out, out_len, info);
    if (code == WineIoc(kWineRecvMsg)) return DoRecvMsg(s, in, in_len, info);
    if (code == WineIoc(kWineSendMsg)) return DoSendMsg(s, in, in_len, info);
    if (code == WineIoc(kWineShutdown)) {
        if (!in || in_len < sizeof(int)) return kInvalidParameter;
        return ::shutdown(s.s, *static_cast<int*>(in)) == SOCKET_ERROR ? LastWsa() : kSuccess;
    }
    if (code == WineIoc(kWineFionbio)) {
        if (!in || in_len < sizeof(u_long)) return kInvalidParameter;
        u_long v = *static_cast<u_long*>(in);
        return ::ioctlsocket(s.s, FIONBIO, &v) == SOCKET_ERROR ? LastWsa() : kSuccess;
    }
    if (code == WineIoc(kWineFionread) || code == WineIoc(kWineSiocatmark)) {
        if (!out || out_len < sizeof(u_long)) return kBufferTooSmall;
        u_long v = 0;
        if (::ioctlsocket(s.s, code == WineIoc(kWineFionread) ? FIONREAD : SIOCATMARK, &v) == SOCKET_ERROR)
            return LastWsa();
        *static_cast<u_long*>(out) = v;
        *info = sizeof(u_long);
        return kSuccess;
    }
    if (code == WineIoc(kWineGetInfo)) {
        if (!out || out_len < sizeof(InfoParams)) return kBufferTooSmall;
        *static_cast<InfoParams*>(out) = InfoParams{s.family, s.type, s.protocol};
        *info = sizeof(InfoParams);
        return kSuccess;
    }
    const OptionDef* od;
    bool is_set;
    if (FindOption((code >> 2) & 0xfff, &od, &is_set) && (code >> 16) == kDevNetwork) {
        if (is_set) {
            if (!in) return kAccessViolation;
            return ::setsockopt(s.s, od->level, od->name, static_cast<const char*>(in), static_cast<int>(in_len)) ==
                           SOCKET_ERROR ? LastWsa() : kSuccess;
        }
        if (!out) return kAccessViolation;
        int len = static_cast<int>(out_len);
        if (::getsockopt(s.s, od->level, od->name, static_cast<char*>(out), &len) == SOCKET_ERROR) return LastWsa();
        *info = static_cast<ULONG_PTR>(len);
        return kSuccess;
    }
    FIXME(dispatch, "unsupported ioctl %#x on a socket (device %#x, function %#x)", code, code >> 16,
          (code >> 2) & 0xfff);
    return kNotSupported;
}

}  // namespace

namespace {
constexpr std::uintptr_t kTebWinSockData = 0x1738;
thread_local void* t_host_winsock_data = nullptr;   // constant-initialised: no guard, safe in the host view
void*& TebWinSockData() {
    return *reinterpret_cast<void**>(static_cast<std::uintptr_t>(__readgsqword(0x30)) + kTebWinSockData);
}
}  // namespace

HostWinsockScope::HostWinsockScope() : guest_value_(TebWinSockData()) { TebWinSockData() = t_host_winsock_data; }
HostWinsockScope::~HostWinsockScope() {
    t_host_winsock_data = TebWinSockData();
    TebWinSockData() = guest_value_;
}

void EnsureWinsock() {
    static volatile LONG state = 0;   // 0 none, 1 starting, 2 ready
    if (state == 2) return;
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        InterlockedExchange(&state, 2);
    } else {
        while (state != 2) Sleep(1);
    }
}

void PrimeHost() {
    HostWinsockScope scope;
    EnsureWinsock();
    const struct { int family, type, protocol; } kinds[] = {
        {AF_INET, SOCK_STREAM, IPPROTO_TCP}, {AF_INET, SOCK_DGRAM, IPPROTO_UDP}, {AF_INET6, SOCK_STREAM, IPPROTO_TCP}};
    for (const auto& k : kinds) {
        SOCKET s = ::socket(k.family, k.type, k.protocol);
        if (s != INVALID_SOCKET) ::closesocket(s);
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo("localhost", "0", &hints, &res) == 0) {
        sockaddr_in lo{};
        lo.sin_family = AF_INET;
        lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        char host[NI_MAXHOST];
        ::getnameinfo(reinterpret_cast<sockaddr*>(&lo), sizeof lo, host, sizeof host, nullptr, 0, 0);
        ::freeaddrinfo(res);
    }
    TRACE(dispatch, "host Winsock primed");
}

bool IsAfdDeviceName(const OBJECT_ATTRIBUTES* attr) {
    if (!attr || !attr->ObjectName || !attr->ObjectName->Buffer) return false;
    static const wchar_t kName[] = L"\\Device\\Afd";
    const USHORT n = attr->ObjectName->Length / sizeof(wchar_t), k = static_cast<USHORT>(wcslen(kName));
    if (n < k || _wcsnicmp(attr->ObjectName->Buffer, kName, k) != 0) return false;
    return n == k || attr->ObjectName->Buffer[k] == L'\\';
}

NTSTATUS Open(PHANDLE handle, PIO_STATUS_BLOCK io) {
    HANDLE h = Register(std::make_shared<Sock>());
    if (!h) return kInsufficientResources;
    if (io) { io->Status = kSuccess; io->Information = 1; /* FILE_OPENED */ }
    *handle = h;
    return kSuccess;
}

bool IsSocketHandle(HANDLE h) { return Find(h) != nullptr; }

NTSTATUS Close(HANDLE h) {
    std::shared_ptr<Sock> s;
    AcquireSRWLockExclusive(&g_lock);
    auto it = Socks().find(reinterpret_cast<std::uintptr_t>(h));
    if (it != Socks().end()) { s = std::move(it->second); Socks().erase(it); }
    ReleaseSRWLockExclusive(&g_lock);
    if (!s) return kInvalidHandle;
    HostWinsockScope scope;
    if (s->s != INVALID_SOCKET) ::closesocket(s->s);
    dispatch::UnregisterEmulatedObject(h);
    return kSuccess;
}

NTSTATUS NTAPI Afd_NtDeviceIoControlFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apc_context,
                                         PIO_STATUS_BLOCK io, ULONG code, PVOID in, ULONG in_len, PVOID out,
                                         ULONG out_len) {
    using Fn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID,
                                ULONG);
    std::shared_ptr<Sock> sock = Find(file);
    if (!sock) {
        static Fn host = reinterpret_cast<Fn>(dispatch::HostExport("NtDeviceIoControlFile"));
        return host ? host(file, event, apc, apc_context, io, code, in, in_len, out, out_len) : kNotImplemented;
    }
    HostWinsockScope scope;
    ULONG_PTR info = 0;
    NTSTATUS_ st;
    if (code == kAfdPoll) st = DoPoll(static_cast<PollParams*>(in), in_len, out_len, &info);
    else if (code == kAfdEventSelect || code == kAfdGetEvents || code == WineIoc(kWineMessageSelect)) {
        FIXME(dispatch, "WSAEventSelect/WSAEnumNetworkEvents/WSAAsyncSelect are not supported yet (ioctl %#x)", code);
        st = kNotSupported;
    } else {
        st = DoIoctl(*sock, code, in, in_len, out, out_len, &info);
    }
    TRACE(dispatch, "socket %p ioctl %#x -> %#lx (%llu bytes)", file, code, static_cast<unsigned long>(st),
          static_cast<unsigned long long>(info));
    if (st == kPending) return st;
    if (io) { io->Status = st; io->Information = info; }
    // The operation is complete: do what the I/O manager does for a request that finished immediately.
    if (event) {
        using SetEventFn = NTSTATUS(NTAPI*)(HANDLE, PLONG);
        static SetEventFn set_event = reinterpret_cast<SetEventFn>(dispatch::HostExport("NtSetEvent"));
        if (set_event) set_event(event, nullptr);
    }
    if (apc) {
        using QueueApcFn = NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, PVOID, PVOID);
        static QueueApcFn queue = reinterpret_cast<QueueApcFn>(dispatch::HostExport("NtQueueApcThread"));
        if (queue) queue(reinterpret_cast<HANDLE>(-2), reinterpret_cast<PVOID>(apc), apc_context, io, nullptr);
    }
    return st;
}

}  // namespace win10xbox::afd
