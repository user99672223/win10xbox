// Wine "unix side" calls (__wine_unix_call_dispatcher) and the Wine debug
// channel functions we patch over the guest ntdll's exports.
//
// Wine's PE ntdll keeps its debug-channel table at PEB+0x2000 and a per-thread
// output buffer at TEB+0x3000 (its unix side allocates PEB/TEB with that extra
// room). A host-owned PEB/TEB has neither, so __wine_dbg_get_channel_flags,
// __wine_dbg_header, __wine_dbg_output and __wine_dbg_strdup are redirected
// here (rtl_forward.inc, WINE_OVERRIDE) and print through our `wine` channel:
//   0024:fixme:wine:ntdll:LdrFoo message
// Classes: fixme/err follow the `wine` channel (WIN10XBOX_DEBUG); warn/trace
// for individual Wine channels come from WIN10XBOX_WINEDEBUG, which uses
// WINEDEBUG syntax ("+loaddll,warn+module,-all").
#include "internal.h"
#include "win10xbox/config.h"
#if WIN10XBOX_HAVE_CLOCK
#include "win10xbox/clock.h"
#endif
#if WIN10XBOX_HAVE_TOKEN
#include "win10xbox/token.h"
#endif
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>

namespace win10xbox::dispatch {
namespace detail {
namespace {

struct UnixLib { std::wstring name; const UnixCallFn* table; std::size_t count; };
SRWLOCK g_lib_lock = SRWLOCK_INIT;
std::vector<UnixLib>& Libs() { static std::vector<UnixLib> v; return v; }
std::uint64_t g_ntdll_handle = 0;

std::wstring NormalizeLibName(std::wstring_view n) {
    std::size_t slash = n.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) n.remove_prefix(slash + 1);
    std::size_t dot = n.find(L'.');
    if (dot != std::wstring_view::npos) n = n.substr(0, dot);
    std::wstring out(n);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

// ---- ntdll's unix functions (dlls/ntdll/unixlib.h order) ----------------------
struct DbgWriteParams { const char* str; unsigned int len; };
struct ServerRequestHeader { int req; unsigned int request_size; unsigned int reply_size; };

NTSTATUS WINAPI U_load_so_dll(void*) { return st::DllNotFound; }   // no .so libraries on Windows
NTSTATUS WINAPI U_unwind_builtin_dll(void*) { return st::Unsuccessful; }

NTSTATUS WINAPI U_wine_dbg_write(void* args) {
    auto* p = static_cast<DbgWriteParams*>(args);
    if (!p->str || !p->len) return 0;
    unsigned len = p->len;
    while (len && (p->str[len - 1] == '\n' || p->str[len - 1] == '\r')) --len;
    trace::Log(&__win10xbox_dbch_wine, trace::Class::Err, "", "%.*s", static_cast<int>(len), p->str);
    return static_cast<NTSTATUS>(p->len);
}

SRWLOCK g_req_lock = SRWLOCK_INIT;
std::map<int, std::uint64_t>& ServerRequests() { static std::map<int, std::uint64_t> m; return m; }

NTSTATUS WINAPI U_wine_server_call(void* args) {
    int req = args ? static_cast<ServerRequestHeader*>(args)->req : -1;
    AcquireSRWLockExclusive(&g_req_lock);
    bool first = ServerRequests()[req]++ == 0;
    ReleaseSRWLockExclusive(&g_req_lock);
    if (first) FIXME(wine, "wine_server_call: request %d not implemented (no wineserver)", req);
    return st::NotImplemented;
}

NTSTATUS WINAPI U_fd_to_handle(void*) { return st::NotSupported; }
NTSTATUS WINAPI U_handle_to_fd(void*) { return st::NotSupported; }
NTSTATUS WINAPI U_spawnvp(void*) { return st::NotSupported; }

NTSTATUS WINAPI U_system_time_precise(void* args) {
#if WIN10XBOX_HAVE_CLOCK
    *static_cast<LONGLONG*>(args) = clock::SystemTime100ns();
#else
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    *static_cast<LONGLONG*>(args) = (static_cast<LONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#endif
    return st::Success;
}

const UnixCallFn kNtdllFuncs[] = {
    &U_load_so_dll, &U_unwind_builtin_dll, &U_wine_dbg_write, &U_wine_server_call,
    &U_fd_to_handle, &U_handle_to_fd, &U_spawnvp, &U_system_time_precise,
};

// ---- ws2_32's unix functions (dlls/ws2_32/ws2_32_private.h, enum ws_unix_funcs) ------------------
// ws2_32's DllMain loads this library, so it must exist for any program that links winsock. The layer has no network:
// name resolution answers WSAHOST_NOT_FOUND (what a resolver without any reachable DNS server reports), and
// gethostname answers the machine identity's computer name.
constexpr NTSTATUS kWsaEFault = 10014;
constexpr NTSTATUS kWsaHostNotFound = 11001;

struct WsGetHostnameParams { char* name; unsigned int size; };

NTSTATUS WINAPI U_ws_name_resolution(void*) { return kWsaHostNotFound; }

NTSTATUS WINAPI U_ws_gethostname(void* args) {
    auto* p = static_cast<WsGetHostnameParams*>(args);
#if WIN10XBOX_HAVE_TOKEN
    const std::wstring computer = token::ComputerName();
#else
    const std::wstring computer = L"WIN10XBOX";
#endif
    if (!p->name || computer.size() >= p->size) return kWsaEFault;
    for (std::size_t i = 0; i < computer.size(); ++i) p->name[i] = static_cast<char>(computer[i]);
    p->name[computer.size()] = 0;
    return 0;
}

const UnixCallFn kWs2Funcs[] = {
    &U_ws_name_resolution,   // getaddrinfo
    &U_ws_name_resolution,   // gethostbyaddr
    &U_ws_name_resolution,   // gethostbyname
    &U_ws_gethostname,
    &U_ws_name_resolution,   // getnameinfo
};

// ---- Wine debug channels ---------------------------------------------------------
constexpr unsigned char kInitBit = 1u << 7;
struct WineSpec { std::string channel; unsigned char mask; bool on; };
std::vector<WineSpec>& Specs() { static std::vector<WineSpec> v; return v; }
bool g_specs_loaded = false;

void LoadWineDebugSpec() {
    if (g_specs_loaded) return;
    g_specs_loaded = true;
    char buf[1024];
    DWORD n = GetEnvironmentVariableA("WIN10XBOX_WINEDEBUG", buf, sizeof buf);
    if (!n || n >= sizeof buf) return;
    std::string s(buf, n);
    std::size_t pos = 0;
    while (pos <= s.size()) {
        std::size_t end = s.find_first_of(",;", pos);
        std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? s.size() + 1 : end + 1;
        if (item.empty()) continue;
        std::size_t sep = item.find_first_of("+-");
        WineSpec sp{item, 0x0f, true};
        if (sep != std::string::npos) {
            std::string cls = item.substr(0, sep);
            sp.on = item[sep] == '+';
            sp.channel = item.substr(sep + 1);
            if (cls == "fixme") sp.mask = 1;
            else if (cls == "err") sp.mask = 2;
            else if (cls == "warn") sp.mask = 4;
            else if (cls == "trace") sp.mask = 8;
        }
        Specs().push_back(sp);
    }
}

unsigned char FlagsFor(const char* name) {
    unsigned char flags = 0;
    if (trace::Enabled(&__win10xbox_dbch_wine, trace::Class::Fixme)) flags |= 1;
    if (trace::Enabled(&__win10xbox_dbch_wine, trace::Class::Err)) flags |= 2;
    for (const WineSpec& sp : Specs()) {
        if (sp.channel != "all" && sp.channel != name) continue;
        if (sp.on) flags = static_cast<unsigned char>(flags | sp.mask);
        else flags = static_cast<unsigned char>(flags & ~sp.mask);
    }
    return flags;
}

constexpr std::uint8_t kNoClass = 0xff;   // MESSAGE(): no header
char g_fallback_strings[1024];
std::uint32_t g_fallback_str_pos = 0;

void FlushLine(ThreadState* t) {
    std::uint32_t len = t->dbg_pos;
    while (len && (t->dbg_line[len - 1] == '\n' || t->dbg_line[len - 1] == '\r')) --len;
    trace::Class cls = t->dbg_class <= 3 ? static_cast<trace::Class>(t->dbg_class) : trace::Class::Err;
    trace::Log(&__win10xbox_dbch_wine, cls, t->dbg_class <= 3 ? t->dbg_prefix : "", "%.*s", static_cast<int>(len),
               t->dbg_line);
    t->dbg_pos = 0;
    t->dbg_class = kNoClass;
}

}  // namespace

std::uint64_t NtdllUnixLibHandle() { return g_ntdll_handle; }

void RegisterNtdllUnixLib() {
    LoadWineDebugSpec();
    if (!g_ntdll_handle) RegisterUnixLib(L"ntdll", kNtdllFuncs, std::size(kNtdllFuncs));
    RegisterUnixLib(L"ws2_32", kWs2Funcs, std::size(kWs2Funcs));
}

std::uint64_t UnixLibHandleByName(std::wstring_view module_name) {
    std::wstring n = NormalizeLibName(module_name);
    AcquireSRWLockShared(&g_lib_lock);
    std::uint64_t h = 0;
    for (std::size_t i = 0; i < Libs().size(); ++i)
        if (Libs()[i].name == n) { h = i + 1; break; }
    ReleaseSRWLockShared(&g_lib_lock);
    return h;
}

extern "C" unsigned char __cdecl w10x_wine_dbg_get_channel_flags(WineDebugChannel* ch) {
    if (!(ch->flags & kInitBit)) return ch->flags;
    HostModeScope host;
    unsigned char flags = FlagsFor(ch->name);
    ch->flags = flags;
    return flags;
}

extern "C" int __cdecl w10x_wine_dbg_header(int cls, WineDebugChannel* ch, const char* function) {
    if (cls < 0 || cls > 3 || !(w10x_wine_dbg_get_channel_flags(ch) & (1 << cls))) return -1;
    HostModeScope host;
    ThreadState* t = CurrentThreadState();
    if (!t) return 0;
    if (t->dbg_pos) return 0;   // continuing a line
    t->dbg_class = static_cast<std::uint8_t>(cls);
    int n = std::snprintf(t->dbg_prefix, sizeof t->dbg_prefix, "%.15s:%s", ch->name, function ? function : "");
    return n > 0 ? n : 0;
}

extern "C" int __cdecl w10x_wine_dbg_output(const char* str) {
    HostModeScope host;
    ThreadState* t = CurrentThreadState();
    int total = static_cast<int>(std::strlen(str));
    if (!t) {
        trace::Log(&__win10xbox_dbch_wine, trace::Class::Err, "", "%s", str);
        return total;
    }
    if (!t->dbg_pos && t->dbg_class > 3) t->dbg_class = kNoClass;
    for (const char* p = str; *p; ++p) {
        if (t->dbg_pos >= sizeof t->dbg_line - 1) FlushLine(t);
        t->dbg_line[t->dbg_pos++] = *p;
        if (*p == '\n') FlushLine(t);
    }
    return total;
}

extern "C" const char* __cdecl w10x_wine_dbg_strdup(const char* str) {
    HostModeScope host;
    ThreadState* t = CurrentThreadState();
    char* ring = t ? t->dbg_strings : g_fallback_strings;
    std::uint32_t* pos = t ? &t->dbg_str_pos : &g_fallback_str_pos;
    std::size_t n = std::strlen(str) + 1;
    if (n > sizeof g_fallback_strings) n = sizeof g_fallback_strings;
    if (*pos + n > sizeof g_fallback_strings) *pos = 0;
    char* out = ring + *pos;
    std::memcpy(out, str, n);
    out[n - 1] = 0;
    *pos += static_cast<std::uint32_t>(n);
    return out;
}

}  // namespace detail

using namespace detail;

void RegisterUnixLib(const wchar_t* module_name, const UnixCallFn* table, std::size_t count) {
    std::wstring n = NormalizeLibName(module_name);
    AcquireSRWLockExclusive(&g_lib_lock);
    std::uint64_t handle = 0;
    for (std::size_t i = 0; i < Libs().size(); ++i)
        if (Libs()[i].name == n) { Libs()[i] = UnixLib{n, table, count}; handle = i + 1; }
    if (!handle) {
        Libs().push_back(UnixLib{n, table, count});
        handle = Libs().size();
    }
    ReleaseSRWLockExclusive(&g_lib_lock);
    if (n == L"ntdll") g_ntdll_handle = handle;
    TRACE(dispatch, "unix lib %ls: handle %llu, %zu functions", n.c_str(), static_cast<unsigned long long>(handle), count);
}

NTSTATUS WINAPI UnixCallDispatcher(std::uint64_t handle, unsigned int code, void* args) {
    HostModeScope host;
    AcquireSRWLockShared(&g_lib_lock);
    const UnixLib* lib = handle && handle <= Libs().size() ? &Libs()[handle - 1] : nullptr;
    UnixCallFn fn = lib && code < lib->count ? lib->table[code] : nullptr;
    std::wstring name = lib ? lib->name : L"?";
    ReleaseSRWLockShared(&g_lib_lock);
    if (!fn) {
        FIXME(wine, "unix call %ls:%u (handle %llu) not implemented", name.c_str(), code,
              static_cast<unsigned long long>(handle));
        return st::NotImplemented;
    }
    return fn(args);
}

}  // namespace win10xbox::dispatch
