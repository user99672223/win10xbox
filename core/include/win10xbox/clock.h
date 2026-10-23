// win10xbox virtual clock: all time the guest sees comes from here.
// Owned by core/clock/.
//
// Sources virtualised: QueryPerformanceCounter/Frequency, system time
// (NtQuerySystemTime, KUSER_SHARED_DATA.SystemTime), interrupt time / tick
// count (NtGetTickCount, KUSER_SHARED_DATA.TickCount), rdtsc/rdtscp (via the
// scanner), timer-based waits (NtDelayExecution, NtWaitFor* timeouts are
// scaled), and Ke*/KeQuery* for drivers. Controls: scale (slow-mo / fast),
// offset, freeze. The host clock is read through the host ntdll only.
#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstdint>

namespace win10xbox::clock {

struct Settings {
    double        scale = 1.0;        // guest seconds per host second
    std::int64_t  offset_100ns = 0;   // added to guest system time (wall clock)
    bool          frozen = false;     // guest time stands still
    std::uint64_t tsc_frequency = 0;  // 0 = derive from host QPC (reported to guest via cpuid 0x15/0x16)
    std::uint64_t qpc_frequency = 10'000'000;  // Windows 10 reports 10 MHz when using invariant TSC/HPET; keep fixed
    // Extension: uptime the guest sees at Initialize (InterruptTime, TickCount
    // and QPC start here instead of 0). Only read by Initialize().
    std::uint64_t initial_uptime_100ns = 0;
};

NTSTATUS Initialize(const Settings& s);
void     SetSettings(const Settings& s);
Settings GetSettings();
void     Freeze(bool frozen);

// Guest-visible reads (monotonic unless scale/offset change).
std::uint64_t Qpc();                      // virtual performance counter
std::uint64_t QpcFrequency();
std::uint64_t Rdtsc();                    // virtual TSC for patched rdtsc/rdtscp sites
std::uint32_t RdtscpAux();                // IA32_TSC_AUX (cpu number)
std::int64_t  SystemTime100ns();          // FILETIME-style
std::uint64_t InterruptTime100ns();       // since virtual boot
std::uint64_t TickCountMs();              // GetTickCount64 semantics
std::int64_t  BootTime100ns();            // virtual boot time = SystemTime - InterruptTime (shifts with NtSetSystemTime like KeBootTime)
// Extension: effective virtual TSC frequency in Hz (Settings::tsc_frequency, or
// the host-derived value when that is 0). The dispatcher's cpuid handler reports
// it through leaf 0x15 (TSC/crystal ratio) and 0x16 (MHz).
std::uint64_t TscFrequency();

// Scale a guest-relative timeout (negative LARGE_INTEGER = relative 100ns) to
// host units for pass-through waits. Absolute timeouts are converted from
// guest system time to host system time. Returns pointer to `storage` or
// nullptr when `in` is nullptr.
PLARGE_INTEGER TranslateTimeout(PLARGE_INTEGER in, LARGE_INTEGER* storage);

// NT-signature entry points wired by the dispatcher.
NTSTATUS Clk_NtQueryPerformanceCounter(PLARGE_INTEGER Counter, PLARGE_INTEGER Frequency);
NTSTATUS Clk_NtQuerySystemTime(PLARGE_INTEGER SystemTime);
NTSTATUS Clk_NtSetSystemTime(PLARGE_INTEGER NewTime, PLARGE_INTEGER OldTime);   // adjusts offset; never touches host
NTSTATUS Clk_NtGetTickCount(ULONG* Out);     // Wine ntdll syscall shape: returns ULONG via rax; dispatcher adapts
NTSTATUS Clk_NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS Clk_NtQueryTimerResolution(PULONG Min, PULONG Max, PULONG Current);
NTSTATUS Clk_NtSetTimerResolution(ULONG Requested, BOOLEAN Set, PULONG Actual);
NTSTATUS Clk_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter(BOOLEAN ToPerformanceCounter, PULONGLONG Value,
                                                                  PULONGLONG Converted, PULONGLONG Error);
NTSTATUS Clk_NtQuerySystemInformation_TimeOfDay(PVOID Buffer, ULONG Length, PULONG ReturnLength); // SYSTEM_TIMEOFDAY_INFORMATION

// Virtual KUSER_SHARED_DATA page: a private RW page kept in sync by a
// background thread (period ~1ms) so scanner-redirected absolute reads of
// 0x7FFE0000.. can be served from here. nullptr until StartSharedDataThread.
const void* VirtualSharedData();
NTSTATUS StartSharedDataThread();       // allocates the page, writes it once synchronously, starts the ~1ms updater
void StopSharedDataThread();            // joins the updater; the page stays allocated and readable (frozen at last update)

// Extensions for the version/PEB-spoof module and for tests:
void* MutableSharedData();              // same page, writable (version module fills the non-time fields it owns)
void  UpdateSharedDataNow();            // refresh the time fields synchronously (no-op before the page exists)

// KUSER_SHARED_DATA offsets this module writes (x64 Windows 10 22H2, verified
// against third_party/phnt/ntexapi.h static_asserts). KSYSTEM_TIME is
// { ULONG LowPart; LONG High1Time; LONG High2Time; } (12 bytes).
namespace usd {
constexpr std::uint32_t kTickCountLowDeprecated = 0x000;  // ULONG, low 32 bits of TickCountQuad
constexpr std::uint32_t kTickCountMultiplier    = 0x004;  // ULONG, 0x0FA00000 (15.625 ms << 24)
constexpr std::uint32_t kInterruptTime          = 0x008;  // KSYSTEM_TIME
constexpr std::uint32_t kSystemTime             = 0x014;  // KSYSTEM_TIME
constexpr std::uint32_t kTimeZoneBias           = 0x020;  // KSYSTEM_TIME (0 = UTC)
constexpr std::uint32_t kTimeZoneId             = 0x240;  // ULONG (0)
constexpr std::uint32_t kNtBuildNumber          = 0x260;  // ULONG, config::os_build
constexpr std::uint32_t kNtProductType          = 0x264;  // ULONG, 1 = NtProductWinNt
constexpr std::uint32_t kProductTypeIsValid     = 0x268;  // BOOLEAN
constexpr std::uint32_t kNativeProcessorArchitecture = 0x26a; // USHORT, 9 = AMD64
constexpr std::uint32_t kNtMajorVersion         = 0x26c;  // ULONG, config::os_major
constexpr std::uint32_t kNtMinorVersion         = 0x270;  // ULONG, config::os_minor
constexpr std::uint32_t kQpcFrequency           = 0x300;  // ULONGLONG
constexpr std::uint32_t kTickCount              = 0x320;  // KSYSTEM_TIME; the first 8 bytes alias TickCountQuad
constexpr std::uint32_t kInterruptTimeBias      = 0x3b0;  // ULONGLONG (0)
constexpr std::uint32_t kQpcBias                = 0x3b8;  // ULONGLONG (0)
constexpr std::uint32_t kQpcBypassEnabled       = 0x3c6;  // UCHAR, always 0: guest QPC goes through NtQueryPerformanceCounter
}

}  // namespace win10xbox::clock
