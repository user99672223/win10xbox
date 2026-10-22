// Tracing implementation. Modelled on Wine's dlls/ntdll/unix/debug.c channel
// scheme (re-implemented, not copied: Wine's is tied to its TEB layout).
#include "win10xbox/trace.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

WIN10XBOX_DECLARE_CHANNEL(core);
WIN10XBOX_DECLARE_CHANNEL(relay);
WIN10XBOX_DECLARE_CHANNEL(loader);
WIN10XBOX_DECLARE_CHANNEL(dispatch);
WIN10XBOX_DECLARE_CHANNEL(vfs);
WIN10XBOX_DECLARE_CHANNEL(registry);
WIN10XBOX_DECLARE_CHANNEL(clock);
WIN10XBOX_DECLARE_CHANNEL(token);
WIN10XBOX_DECLARE_CHANNEL(cpu);
WIN10XBOX_DECLARE_CHANNEL(driver);

namespace win10xbox::trace {

namespace {
std::vector<Channel*>& Channels() { static std::vector<Channel*> v; return v; }
struct SinkEntry { Sink fn; void* ctx; };
std::vector<SinkEntry>& Sinks() { static std::vector<SinkEntry> v; return v; }
std::mutex& Mutex() { static std::mutex m; return m; }
std::uint8_t g_default_mask = 0;
bool g_initialized = false;
// Trivially initialised on purpose: read on paths where no lazy initialisation
// may happen (see SetUnsafeProbe in trace.h).
UnsafeProbe g_unsafe_probe = nullptr;
volatile std::uint64_t g_dropped_unsafe = 0;

constexpr std::uint8_t Bit(Class c) { return std::uint8_t(1u << std::uint8_t(c)); }
constexpr std::uint8_t kAllMask = Bit(Class::Fixme) | Bit(Class::Err) | Bit(Class::Warn) | Bit(Class::Trace);
constexpr std::uint8_t kQuietMask = Bit(Class::Fixme) | Bit(Class::Err);

// Parse "+relay,-heap,warn+vfs,+all" WINEDEBUG syntax into per-channel masks.
void ApplySpec(const char* spec) {
    std::string s(spec);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? s.size() + 1 : comma + 1;
        if (item.empty()) continue;
        bool on = true;
        std::uint8_t mask = kAllMask;
        size_t sep = item.find_first_of("+-");
        std::string cls = sep == std::string::npos ? "" : item.substr(0, sep);
        std::string name = sep == std::string::npos ? item : item.substr(sep + 1);
        if (sep != std::string::npos) on = item[sep] == '+';
        if (cls == "fixme") mask = Bit(Class::Fixme);
        else if (cls == "err") mask = Bit(Class::Err);
        else if (cls == "warn") mask = Bit(Class::Warn);
        else if (cls == "trace") mask = Bit(Class::Trace);
        if (name == "all") {
            if (on) g_default_mask |= mask; else g_default_mask &= std::uint8_t(~mask);
            for (auto* ch : Channels()) { if (on) ch->enabled |= mask; else ch->enabled &= std::uint8_t(~mask); }
        } else {
            for (auto* ch : Channels())
                if (name == ch->name) { if (on) ch->enabled |= mask; else ch->enabled &= std::uint8_t(~mask); }
        }
    }
}
}  // namespace

bool RegisterChannel(Channel* ch) {
    std::lock_guard<std::mutex> lock(Mutex());
    ch->enabled = g_initialized ? g_default_mask : 0;
    Channels().push_back(ch);
    return true;
}

void Initialize(bool default_on) {
    std::lock_guard<std::mutex> lock(Mutex());
    g_default_mask = default_on ? kAllMask : kQuietMask;
    for (auto* ch : Channels()) ch->enabled = g_default_mask;
    g_initialized = true;
    char buf[1024];
    DWORD n = GetEnvironmentVariableA("WIN10XBOX_DEBUG", buf, sizeof buf);
    if (n && n < sizeof buf) ApplySpec(buf);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(Mutex());
    Sinks().clear();
}

bool Enabled(const Channel* ch, Class cls) {
    return (ch->enabled & Bit(cls)) != 0;
}

void AddSink(Sink sink, void* ctx) {
    std::lock_guard<std::mutex> lock(Mutex());
    Sinks().push_back({sink, ctx});
}

void SetUnsafeProbe(UnsafeProbe probe) { g_unsafe_probe = probe; }
std::uint64_t DroppedUnsafe() { return g_dropped_unsafe; }

void LogV(const Channel* ch, Class cls, const char* func, const char* fmt, va_list ap) {
    static const char* const kClassName[] = {"fixme", "err", "warn", "trace"};
    // Before anything with a lazily initialised static (Mutex, Sinks) or a CRT
    // lock: on this thread, right now, that would fault rather than log.
    if (UnsafeProbe p = g_unsafe_probe; p && p()) {
        ++g_dropped_unsafe;
        return;
    }
    char line[2048];
    int n = std::snprintf(line, sizeof line, "%04lx:%s:%s:%s ", GetCurrentThreadId(),
                          kClassName[std::uint8_t(cls)], ch->name, func);
    if (n < 0) return;
    if (size_t(n) < sizeof line) n += std::vsnprintf(line + n, sizeof line - n, fmt, ap);
    if (n < 0) return;
    size_t len = size_t(n) < sizeof line - 2 ? size_t(n) : sizeof line - 2;
    line[len] = '\n'; line[len + 1] = 0;
    std::lock_guard<std::mutex> lock(Mutex());
    std::fputs(line, stderr);
    std::fflush(stderr);
    OutputDebugStringA(line);
    for (auto& s : Sinks()) s.fn(line, s.ctx);
}

void Log(const Channel* ch, Class cls, const char* func, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    LogV(ch, cls, func, fmt, ap);
    va_end(ap);
}

}  // namespace win10xbox::trace
