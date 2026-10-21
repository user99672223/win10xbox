// Wine-style tracing: named channels, each independently enabled by the
// WIN10XBOX_DEBUG environment variable ("+relay,-heap,+all" like WINEDEBUG),
// a `relay` channel that logs every syscall with arguments and NTSTATUS, and
// `warn`/`err`/`fixme` classes. Output goes to stderr and OutputDebugString;
// hosts may add a sink (the UWP host renders to screen and writes LocalState).
#pragma once
#include <cstdarg>
#include <cstdint>

namespace win10xbox::trace {

enum class Class : std::uint8_t { Fixme = 0, Err = 1, Warn = 2, Trace = 3 };

struct Channel {
    const char* name;
    std::uint8_t enabled;   // bit per Class; set at Initialize from the env
};

// Declared once per channel by WIN10XBOX_DECLARE_CHANNEL(name) in a .cpp; use
// WIN10XBOX_DEFAULT_CHANNEL(name) to make it the file default for TRACE().
#define WIN10XBOX_DECLARE_CHANNEL(ch) \
    ::win10xbox::trace::Channel __win10xbox_dbch_##ch = { #ch, 0 }; \
    static const bool __win10xbox_dbch_reg_##ch = ::win10xbox::trace::RegisterChannel(&__win10xbox_dbch_##ch)
#define WIN10XBOX_DEFAULT_CHANNEL(ch) \
    static ::win10xbox::trace::Channel* const __win10xbox_default_channel = &__win10xbox_dbch_##ch
#define WIN10XBOX_EXTERN_CHANNEL(ch) extern ::win10xbox::trace::Channel __win10xbox_dbch_##ch

bool RegisterChannel(Channel* ch);
void Initialize(bool default_on);
void Shutdown();
bool Enabled(const Channel* ch, Class cls);
void Log(const Channel* ch, Class cls, const char* func, const char* fmt, ...);
void LogV(const Channel* ch, Class cls, const char* func, const char* fmt, va_list ap);

using Sink = void (*)(const char* line, void* ctx);
void AddSink(Sink sink, void* ctx);

}  // namespace win10xbox::trace

#define WIN10XBOX_LOG(ch, cls, ...) \
    do { if (::win10xbox::trace::Enabled(&__win10xbox_dbch_##ch, ::win10xbox::trace::Class::cls)) \
        ::win10xbox::trace::Log(&__win10xbox_dbch_##ch, ::win10xbox::trace::Class::cls, __func__, __VA_ARGS__); } while (0)

#define TRACE(ch, ...) WIN10XBOX_LOG(ch, Trace, __VA_ARGS__)
#define WARN(ch, ...)  WIN10XBOX_LOG(ch, Warn,  __VA_ARGS__)
#define ERR(ch, ...)   WIN10XBOX_LOG(ch, Err,   __VA_ARGS__)
#define FIXME(ch, ...) WIN10XBOX_LOG(ch, Fixme, __VA_ARGS__)

// Channels shared across the core. Each .cpp that owns one declares it with
// WIN10XBOX_DECLARE_CHANNEL; everyone else gets the extern from here.
WIN10XBOX_EXTERN_CHANNEL(core);
WIN10XBOX_EXTERN_CHANNEL(relay);
WIN10XBOX_EXTERN_CHANNEL(loader);
WIN10XBOX_EXTERN_CHANNEL(dispatch);
WIN10XBOX_EXTERN_CHANNEL(vfs);
WIN10XBOX_EXTERN_CHANNEL(registry);
WIN10XBOX_EXTERN_CHANNEL(clock);
WIN10XBOX_EXTERN_CHANNEL(token);
WIN10XBOX_EXTERN_CHANNEL(cpu);
WIN10XBOX_EXTERN_CHANNEL(driver);
