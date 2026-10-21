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
std::int64_t  BootTime100ns();            // virtual boot time (system time at Initialize minus offset)

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
NTSTATUS Clk_NtQuerySystemInformation_TimeOfDay(PVOID Buffer, ULONG Length, PULONG ReturnLength); // SYSTEM_TIMEOFDAY_INFORMATION

// Virtual KUSER_SHARED_DATA page: a private RW page kept in sync by a
// background thread (period ~1ms) so scanner-redirected absolute reads of
// 0x7FFE0000.. can be served from here. nullptr until StartSharedDataThread.
const void* VirtualSharedData();
NTSTATUS StartSharedDataThread();
void StopSharedDataThread();

}  // namespace win10xbox::clock
