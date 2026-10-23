// win10xbox virtual clock. See core/include/win10xbox/clock.h for the contract.
//
// Model
// -----
// Everything the guest sees is a linear function of the HOST performance
// counter (D9: ClockSource::HostQpc):
//
//     guest_x(now) = x_ref + (host_qpc(now) - host_ref) * rate_x      (rate in Q32.32)
//
// for x in { QPC counter, interrupt time (100ns), system time (100ns) } and, for
// the virtual TSC, the same formula over the host's raw rdtsc so that two
// back-to-back rdtsc executions differ (QPC alone only has 100ns = ~380 cycle
// granularity). rate_x = x_units_per_second * scale / host_units_per_second.
//
// Any change of scale / frequency / freeze / offset REBASES: the current guest
// values are computed with the old parameters, stored as the new *_ref together
// with the current host counters, and the new rates apply from there on. This
// is what keeps QPC, interrupt time and TSC continuous (never backwards) across
// SetSettings. Only a change of `offset_100ns` moves SystemTime discontinuously
// (that is its purpose: it is the wall-clock offset, NtSetSystemTime adjusts it).
//
// While frozen, reads return the *_ref values (time stands still). Wait
// timeouts are NOT frozen (a frozen game must not deadlock in Sleep): they are
// scaled by `scale` only.
//
// The host clock is read through kernel32/ntdll only (QueryPerformanceCounter,
// GetSystemTimePreciseAsFileTime, __rdtsc, host NtDelayExecution); nothing here
// goes through the win10xbox dispatcher.
#include "win10xbox/clock.h"
#include "win10xbox/config.h"
#include "win10xbox/trace.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>

namespace win10xbox::clock {

namespace {

constexpr NTSTATUS kStatusSuccess              = 0;
constexpr NTSTATUS kStatusAccessViolation      = static_cast<NTSTATUS>(0xC0000005);
constexpr NTSTATUS kStatusInvalidParameter     = static_cast<NTSTATUS>(0xC000000D);
constexpr NTSTATUS kStatusInfoLengthMismatch   = static_cast<NTSTATUS>(0xC0000004);
constexpr NTSTATUS kStatusInsufficientResources = static_cast<NTSTATUS>(0xC000009A);

constexpr std::uint64_t kQ32 = 1ull << 32;
constexpr std::uint64_t k100nsPerSec = 10'000'000ull;
constexpr std::uint64_t kTickUnit100ns = 156250ull;       // 15.625 ms: KUSER_SHARED_DATA.TickCount granularity
constexpr std::uint32_t kTickMultiplier = 0x0FA00000u;    // 15.625 << 24

// ---- 128-bit helpers ---------------------------------------------------------
// floor(a*b/c) with a 128-bit intermediate; saturates to UINT64_MAX. rem (optional)
// receives the remainder when the result did not saturate.
inline std::uint64_t MulDiv64(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t* rem = nullptr) {
    if (rem) *rem = 0;
    if (c == 0) return UINT64_MAX;
#if defined(_MSC_VER) && !defined(__clang__)
    std::uint64_t hi = 0;
    const std::uint64_t lo = _umul128(a, b, &hi);
    if (hi >= c) return UINT64_MAX;
    std::uint64_t r = 0;
    const std::uint64_t q = _udiv128(hi, lo, c, &r);
    if (rem) *rem = r;
    return q;
#else
    const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
    const unsigned __int128 q = p / c;
    if (q > UINT64_MAX) return UINT64_MAX;
    if (rem) *rem = static_cast<std::uint64_t>(p % c);
    return static_cast<std::uint64_t>(q);
#endif
}

inline std::uint64_t MulDiv64Ceil(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    std::uint64_t r = 0;
    const std::uint64_t q = MulDiv64(a, b, c, &r);
    return (r != 0 && q != UINT64_MAX) ? q + 1 : q;
}

// ---- host time ---------------------------------------------------------------
inline std::uint64_t HostQpc() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<std::uint64_t>(li.QuadPart);
}

inline std::int64_t HostSystemTime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

std::uint64_t HostQpcFrequency() {
    static const std::uint64_t f = [] {
        LARGE_INTEGER li;
        li.QuadPart = 0;
        QueryPerformanceFrequency(&li);
        return li.QuadPart > 0 ? static_cast<std::uint64_t>(li.QuadPart) : k100nsPerSec;
    }();
    return f;
}

// Raw TSC rate of the host, measured once against the host QPC (~20 ms spin).
// Returns 0 when the measurement is implausible; callers then fall back to a
// QPC-derived virtual TSC.
std::uint64_t HostTscFrequency() {
    static std::atomic<std::uint64_t> cached{0};
    static std::atomic<bool> done{false};
    if (done.load(std::memory_order_acquire)) return cached.load(std::memory_order_relaxed);
    const std::uint64_t hf = HostQpcFrequency();
    const std::uint64_t q0 = HostQpc();
    const std::uint64_t t0 = __rdtsc();
    const std::uint64_t target = q0 + hf / 50;
    std::uint64_t q1 = q0;
    while ((q1 = HostQpc()) < target) { /* spin */ }
    const std::uint64_t t1 = __rdtsc();
    std::uint64_t f = (q1 > q0 && t1 > t0) ? MulDiv64(t1 - t0, hf, q1 - q0) : 0;
    if (f < 100'000'000ull || f > 10'000'000'000ull) f = 0;
    cached.store(f, std::memory_order_relaxed);
    done.store(true, std::memory_order_release);
    TRACE(clock, "host tsc frequency %llu Hz (host qpc %llu Hz)", (unsigned long long)f, (unsigned long long)hf);
    return f;
}

// ---- state -------------------------------------------------------------------
struct State {
    Settings      s;                    // sanitised; tsc_frequency resolved to a concrete value
    std::uint64_t hfreq = 1;            // host QPC frequency
    std::uint64_t host_tsc_freq = 0;    // 0 = host rdtsc unusable
    std::uint64_t scale_fp = kQ32;      // scale * 2^32
    std::uint64_t host_ref = 0;         // host QPC at last rebase
    std::uint64_t host_tsc_ref = 0;     // host rdtsc at last rebase
    std::uint64_t q_ref = 0;            // guest QPC counter at last rebase
    std::uint64_t int_ref = 0;          // guest interrupt time (100ns)
    std::int64_t  sys_ref = 0;          // guest system time (100ns, offset included)
    std::uint64_t tsc_ref = 0;          // guest TSC
    std::uint64_t q_rate = kQ32;        // guest qpc ticks per host qpc tick, Q32
    std::uint64_t t_rate = kQ32;        // guest 100ns per host qpc tick, Q32
    std::uint64_t tsc_rate = kQ32;      // guest tsc per host rdtsc (or per host qpc tick when !host_tsc_freq), Q32
};

struct Vals { std::uint64_t q, intr, tsc; std::int64_t sys; };

SRWLOCK g_lock = SRWLOCK_INIT;
State g_st;
std::atomic<bool> g_inited{false};

struct ReadLock {
    ReadLock() { AcquireSRWLockShared(&g_lock); }
    ~ReadLock() { ReleaseSRWLockShared(&g_lock); }
};
struct WriteLock {
    WriteLock() { AcquireSRWLockExclusive(&g_lock); }
    ~WriteLock() { ReleaseSRWLockExclusive(&g_lock); }
};

Settings Sanitize(Settings s, std::uint64_t host_tsc_freq) {
    if (!(s.scale >= 1e-6)) s.scale = (std::isnan(s.scale) || s.scale <= 0) ? 1.0 : 1e-6;
    if (s.scale > 1e6) s.scale = 1e6;
    if (s.qpc_frequency == 0) s.qpc_frequency = 10'000'000;
    if (s.qpc_frequency > 4'000'000'000ull) s.qpc_frequency = 4'000'000'000ull;
    if (s.tsc_frequency == 0) {
        // Host-derived, rounded to 1 MHz (real CPUs report a round figure via CPUID 0x15/0x16).
        s.tsc_frequency = host_tsc_freq ? ((host_tsc_freq + 500'000) / 1'000'000) * 1'000'000
                                        : 3'800'000'000ull;   // Zen 2 (Series X) nominal
    }
    return s;
}

void ComputeRates(State& st) {
    st.scale_fp = static_cast<std::uint64_t>(st.s.scale * 4294967296.0);
    if (st.scale_fp == 0) st.scale_fp = 1;
    st.q_rate = MulDiv64(st.s.qpc_frequency, st.scale_fp, st.hfreq);
    st.t_rate = MulDiv64(k100nsPerSec, st.scale_fp, st.hfreq);
    st.tsc_rate = st.host_tsc_freq ? MulDiv64(st.s.tsc_frequency, st.scale_fp, st.host_tsc_freq)
                                   : MulDiv64(st.s.tsc_frequency, st.scale_fp, st.hfreq);
}

inline std::uint64_t Delta(std::uint64_t now, std::uint64_t ref) { return now > ref ? now - ref : 0; }
inline std::uint64_t Apply(std::uint64_t d, std::uint64_t rate) { return MulDiv64(d, rate, kQ32); }

std::uint64_t ComputeTsc(const State& st, std::uint64_t hq, std::uint64_t ht) {
    if (st.s.frozen) return st.tsc_ref;
    return st.host_tsc_freq ? st.tsc_ref + Apply(Delta(ht, st.host_tsc_ref), st.tsc_rate)
                            : st.tsc_ref + Apply(Delta(hq, st.host_ref), st.tsc_rate);
}

Vals Compute(const State& st, std::uint64_t hq, std::uint64_t ht) {
    Vals v;
    if (st.s.frozen) {
        v.q = st.q_ref; v.intr = st.int_ref; v.sys = st.sys_ref; v.tsc = st.tsc_ref;
        return v;
    }
    const std::uint64_t d = Delta(hq, st.host_ref);
    v.q = st.q_ref + Apply(d, st.q_rate);
    const std::uint64_t dt = Apply(d, st.t_rate);
    v.intr = st.int_ref + dt;
    v.sys = st.sys_ref + static_cast<std::int64_t>(dt);
    v.tsc = ComputeTsc(st, hq, ht);
    return v;
}

// Caller holds the exclusive lock (or is the one-time initialiser).
void InitializeLocked(const Settings& in, std::uint64_t host_tsc_freq) {
    State st;
    st.hfreq = HostQpcFrequency();
    st.host_tsc_freq = host_tsc_freq;
    st.s = Sanitize(in, host_tsc_freq);
    ComputeRates(st);
    st.host_ref = HostQpc();
    st.host_tsc_ref = __rdtsc();
    const std::uint64_t up = st.s.initial_uptime_100ns;
    st.q_ref = MulDiv64(up, st.s.qpc_frequency, k100nsPerSec);
    st.int_ref = up;
    st.tsc_ref = MulDiv64(up, st.s.tsc_frequency, k100nsPerSec);
    st.sys_ref = HostSystemTime() + st.s.offset_100ns;
    g_st = st;
    g_inited.store(true, std::memory_order_release);
}

inline void EnsureInit() {
    if (g_inited.load(std::memory_order_acquire)) return;
    const std::uint64_t tf = HostTscFrequency();
    WriteLock w;
    if (!g_inited.load(std::memory_order_relaxed)) InitializeLocked(Settings{}, tf);
}

// One consistent snapshot of every guest clock.
Vals Snapshot() {
    EnsureInit();
    ReadLock r;
    const std::uint64_t hq = HostQpc();
    const std::uint64_t ht = g_st.host_tsc_freq ? __rdtsc() : 0;
    return Compute(g_st, hq, ht);
}

// ---- host ntdll --------------------------------------------------------------
using NtDelayExecutionFn = NTSTATUS(NTAPI*)(BOOLEAN, PLARGE_INTEGER);
using NtSetTimerResolutionFn = NTSTATUS(NTAPI*)(ULONG, BOOLEAN, PULONG);

// The HOST ntdll, latched before the guest take-over. GetModuleHandleW resolves
// through PEB->Ldr, and Wine's loader_init replaces that with the GUEST loader
// list, so after the take-over "ntdll.dll" is the guest's ntdll -- whose
// NtDelayExecution is a stub we rewrote to jump into the dispatcher. Resolving
// lazily from a guest call therefore made Clk_NtDelayExecution call itself until
// the stack ran out. Initialize() primes this while the host PEB is still in
// place; the lazy path only survives for direct unit-test use.
HMODULE HostNtdll() {
    static HMODULE const m = GetModuleHandleW(L"ntdll.dll");
    return m;
}

FARPROC HostNtdllExport(const char* name) {
    HMODULE m = HostNtdll();
    return m ? GetProcAddress(m, name) : nullptr;
}

// SYSTEM_TIMEOFDAY_INFORMATION as of Windows 10 (0x30 bytes). winternl.h only
// has an opaque version of it.
struct TimeOfDayInfo {
    LARGE_INTEGER BootTime;
    LARGE_INTEGER CurrentTime;
    LARGE_INTEGER TimeZoneBias;
    ULONG         TimeZoneId;
    ULONG         Reserved;
    ULONGLONG     BootTimeBias;
    ULONGLONG     SleepTimeBias;
};
static_assert(sizeof(TimeOfDayInfo) == 0x30, "SYSTEM_TIMEOFDAY_INFORMATION layout");

constexpr ULONG kTimerResCoarsest = 156250;  // MinimumResolution (largest period), 15.625 ms
constexpr ULONG kTimerResFinest   = 5000;    // MaximumResolution (smallest period), 0.5 ms
std::atomic<ULONG> g_timer_res{kTimerResCoarsest};

// ---- shared data -------------------------------------------------------------
std::uint8_t*  g_page = nullptr;
HANDLE         g_thread = nullptr;
HANDLE         g_stop = nullptr;
SRWLOCK        g_thread_lock = SRWLOCK_INIT;

inline void Store32(std::uint8_t* base, std::uint32_t off, std::uint32_t v) {
    *reinterpret_cast<volatile std::uint32_t*>(base + off) = v;
}
inline void Store64(std::uint8_t* base, std::uint32_t off, std::uint64_t v) {
    *reinterpret_cast<volatile std::uint64_t*>(base + off) = v;
}

// Windows' KSYSTEM_TIME publication protocol: High2Time first, then LowPart and
// High1Time together, so a reader doing {read High1; read Low; read High2; retry
// until High1 == High2} never sees a torn value, and a reader that loads the
// first 8 bytes (what x64 Windows code does) sees the old or the new value.
// The 8-byte store is a LOCK-ed exchange so it is atomic even at the 4-aligned
// SystemTime offset (0x14; it stays inside one cache line).
void WriteKSystemTime(std::uint8_t* base, std::uint32_t off, std::uint64_t v) {
    Store32(base, off + 8, static_cast<std::uint32_t>(v >> 32));                      // High2Time
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(base + off), static_cast<LONG64>(v)); // LowPart | High1Time
}

void InitStaticSharedFields(std::uint8_t* base) {
    const auto& cfg = config::Current();
    Store32(base, usd::kTickCountMultiplier, kTickMultiplier);
    WriteKSystemTime(base, usd::kTimeZoneBias, 0);
    Store32(base, usd::kTimeZoneId, 0);
    Store32(base, usd::kNtBuildNumber, cfg.os_build);
    Store32(base, usd::kNtProductType, 1);
    base[usd::kProductTypeIsValid] = 1;
    *reinterpret_cast<volatile std::uint16_t*>(base + usd::kNativeProcessorArchitecture) = 9;
    Store32(base, usd::kNtMajorVersion, cfg.os_major);
    Store32(base, usd::kNtMinorVersion, cfg.os_minor);
    Store64(base, usd::kQpcFrequency, g_st.s.qpc_frequency);
    Store64(base, usd::kInterruptTimeBias, 0);
    Store64(base, usd::kQpcBias, 0);
    base[usd::kQpcBypassEnabled] = 0;
}

DWORD WINAPI SharedDataThread(LPVOID) {
    while (WaitForSingleObject(g_stop, 1) == WAIT_TIMEOUT) UpdateSharedDataNow();
    return 0;
}

}  // namespace

// =============================================================================
// Public API
// =============================================================================

NTSTATUS Initialize(const Settings& s) {
    // Latch the host ntdll while the host PEB is still installed: process::Launch
    // calls us before it maps the guest ntdll and takes the PEB over, and after
    // that GetModuleHandleW(L"ntdll.dll") answers with the guest's.
    HostNtdll();
    const std::uint64_t tf = HostTscFrequency();
    {
        WriteLock w;
        InitializeLocked(s, tf);
        TRACE(clock, "initialised: scale=%g offset=%lld frozen=%d tsc=%llu qpc=%llu uptime=%llu",
              g_st.s.scale, (long long)g_st.s.offset_100ns, (int)g_st.s.frozen,
              (unsigned long long)g_st.s.tsc_frequency, (unsigned long long)g_st.s.qpc_frequency,
              (unsigned long long)g_st.s.initial_uptime_100ns);
    }
    g_timer_res.store(kTimerResCoarsest);
    if (g_page) {   // re-initialised while the page exists: refresh it
        {
            ReadLock r;
            InitStaticSharedFields(g_page);
        }
        UpdateSharedDataNow();
    }
    return kStatusSuccess;
}

void SetSettings(const Settings& in) {
    EnsureInit();
    WriteLock w;
    const State old = g_st;
    const std::uint64_t hq = HostQpc();
    const std::uint64_t ht = old.host_tsc_freq ? __rdtsc() : 0;
    const Vals cur = Compute(old, hq, ht);

    State st = old;
    st.s = Sanitize(in, old.host_tsc_freq);
    st.s.initial_uptime_100ns = old.s.initial_uptime_100ns;   // Initialize-only
    ComputeRates(st);
    st.host_ref = hq;
    st.host_tsc_ref = ht;
    // Counters keep their value; if a frequency changed the counter is converted
    // so the guest clock stays continuous in *time*.
    st.q_ref = (st.s.qpc_frequency == old.s.qpc_frequency) ? cur.q
             : MulDiv64(cur.q, st.s.qpc_frequency, old.s.qpc_frequency);
    st.tsc_ref = (st.s.tsc_frequency == old.s.tsc_frequency) ? cur.tsc
               : MulDiv64(cur.tsc, st.s.tsc_frequency, old.s.tsc_frequency);
    st.int_ref = cur.intr;
    st.sys_ref = cur.sys + (st.s.offset_100ns - old.s.offset_100ns);
    g_st = st;
    TRACE(clock, "settings: scale=%g offset=%lld frozen=%d", st.s.scale, (long long)st.s.offset_100ns, (int)st.s.frozen);
}

Settings GetSettings() {
    EnsureInit();
    ReadLock r;
    return g_st.s;
}

void Freeze(bool frozen) {
    Settings s = GetSettings();
    if (s.frozen == frozen) return;
    s.frozen = frozen;
    SetSettings(s);
}

std::uint64_t Qpc() { return Snapshot().q; }

std::uint64_t QpcFrequency() {
    EnsureInit();
    ReadLock r;
    return g_st.s.qpc_frequency;
}

std::uint64_t Rdtsc() {
    EnsureInit();
    ReadLock r;
    if (g_st.s.frozen) return g_st.tsc_ref;
    const std::uint64_t ht = g_st.host_tsc_freq ? __rdtsc() : 0;
    const std::uint64_t hq = g_st.host_tsc_freq ? 0 : HostQpc();
    return ComputeTsc(g_st, hq, ht);
}

std::uint32_t RdtscpAux() {
    // IA32_TSC_AUX as Windows programs it: processor number in the low 12 bits
    // (node number in the bits above; we report node 0).
    return GetCurrentProcessorNumber() & 0xFFFu;
}

std::uint64_t TscFrequency() {
    EnsureInit();
    ReadLock r;
    return g_st.s.tsc_frequency;
}

std::int64_t SystemTime100ns() { return Snapshot().sys; }
std::uint64_t InterruptTime100ns() { return Snapshot().intr; }
std::uint64_t TickCountMs() { return Snapshot().intr / 10'000ull; }

std::int64_t BootTime100ns() {
    const Vals v = Snapshot();
    return v.sys - static_cast<std::int64_t>(v.intr);
}

PLARGE_INTEGER TranslateTimeout(PLARGE_INTEGER in, LARGE_INTEGER* storage) {
    if (!in) return nullptr;
    if (!storage) return in;
    EnsureInit();
    std::uint64_t scale_fp;
    {
        ReadLock r;
        scale_fp = g_st.scale_fp;
    }
    const std::int64_t v = in->QuadPart;
    if (v == 0 || v == INT64_MIN || v == INT64_MAX) {
        *storage = *in;           // poll / infinite: nothing to scale
        return storage;
    }
    if (v < 0) {
        // Relative: guest 100ns -> host 100ns, rounded up so a non-zero wait stays non-zero.
        const std::uint64_t mag = static_cast<std::uint64_t>(-(v + 1)) + 1;   // |v| without overflow
        std::uint64_t host = MulDiv64Ceil(mag, kQ32, scale_fp);
        if (host == 0) host = 1;
        if (host > static_cast<std::uint64_t>(INT64_MAX)) storage->QuadPart = INT64_MIN;
        else storage->QuadPart = -static_cast<std::int64_t>(host);
        return storage;
    }
    // Absolute guest system time -> absolute host system time.
    const std::int64_t guest_now = Snapshot().sys;
    const std::int64_t host_now = HostSystemTime();
    if (v <= guest_now) {
        storage->QuadPart = host_now;     // already expired: any past host time works; use "now"
        return storage;
    }
    const std::uint64_t remaining = static_cast<std::uint64_t>(v) - static_cast<std::uint64_t>(guest_now);
    const std::uint64_t host_rem = MulDiv64Ceil(remaining, kQ32, scale_fp);
    const std::uint64_t cap = static_cast<std::uint64_t>(INT64_MAX) - static_cast<std::uint64_t>(host_now);
    storage->QuadPart = host_now + static_cast<std::int64_t>(host_rem > cap ? cap : host_rem);
    return storage;
}

// =============================================================================
// NT entry points
// =============================================================================

NTSTATUS Clk_NtQueryPerformanceCounter(PLARGE_INTEGER Counter, PLARGE_INTEGER Frequency) {
    if (!Counter && !Frequency) return kStatusAccessViolation;
    if (Counter) Counter->QuadPart = static_cast<LONGLONG>(Qpc());
    if (Frequency) Frequency->QuadPart = static_cast<LONGLONG>(QpcFrequency());
    return kStatusSuccess;
}

NTSTATUS Clk_NtQuerySystemTime(PLARGE_INTEGER SystemTime) {
    if (!SystemTime) return kStatusAccessViolation;
    SystemTime->QuadPart = SystemTime100ns();
    return kStatusSuccess;
}

NTSTATUS Clk_NtSetSystemTime(PLARGE_INTEGER NewTime, PLARGE_INTEGER OldTime) {
    // Only the virtual offset moves; the host clock is never touched. Windows
    // would demand SeSystemtimePrivilege here; we do not consult the token
    // module (the guest cannot hurt anything by moving its private clock).
    if (!NewTime) return kStatusAccessViolation;
    const std::int64_t want = NewTime->QuadPart;
    if (want <= 0) return kStatusInvalidParameter;
    EnsureInit();
    WriteLock w;
    const std::uint64_t hq = HostQpc();
    const std::uint64_t ht = g_st.host_tsc_freq ? __rdtsc() : 0;
    const Vals cur = Compute(g_st, hq, ht);
    if (OldTime) OldTime->QuadPart = cur.sys;
    const std::int64_t delta = want - cur.sys;
    // Rebase so the counters stay continuous; only system time jumps.
    g_st.host_ref = hq;
    g_st.host_tsc_ref = ht;
    g_st.q_ref = cur.q;
    g_st.int_ref = cur.intr;
    g_st.tsc_ref = cur.tsc;
    g_st.sys_ref = want;
    g_st.s.offset_100ns += delta;
    TRACE(clock, "NtSetSystemTime: offset now %lld (delta %lld)", (long long)g_st.s.offset_100ns, (long long)delta);
    return kStatusSuccess;
}

NTSTATUS Clk_NtGetTickCount(ULONG* Out) {
    if (!Out) return kStatusAccessViolation;
    *Out = static_cast<ULONG>(TickCountMs());
    return kStatusSuccess;
}

NTSTATUS Clk_NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER Timeout) {
    static const NtDelayExecutionFn host = reinterpret_cast<NtDelayExecutionFn>(HostNtdllExport("NtDelayExecution"));
    if (!host) return kStatusInsufficientResources;
    LARGE_INTEGER tmp;
    PLARGE_INTEGER t = TranslateTimeout(Timeout, &tmp);
    return host(Alertable, t);
}

NTSTATUS Clk_NtQueryTimerResolution(PULONG Min, PULONG Max, PULONG Current) {
    if (Min) *Min = kTimerResCoarsest;
    if (Max) *Max = kTimerResFinest;
    if (Current) *Current = g_timer_res.load();
    return kStatusSuccess;
}

NTSTATUS Clk_NtSetTimerResolution(ULONG Requested, BOOLEAN Set, PULONG Actual) {
    // The reported resolution is emulated (stable, independent of the host); the
    // request is also forwarded best-effort so the host kernel's real sleeps get
    // the same accuracy the guest asked for. A host refusal is ignored.
    static const NtSetTimerResolutionFn host = reinterpret_cast<NtSetTimerResolutionFn>(HostNtdllExport("NtSetTimerResolution"));
    ULONG cur;
    if (Set) {
        cur = Requested;
        if (cur < kTimerResFinest) cur = kTimerResFinest;
        if (cur > kTimerResCoarsest) cur = kTimerResCoarsest;
    } else {
        cur = kTimerResCoarsest;
    }
    g_timer_res.store(cur);
    if (host) {
        ULONG hostActual = 0;
        (void)host(Requested, Set, &hostActual);
    }
    if (Actual) *Actual = cur;
    return kStatusSuccess;
}

NTSTATUS Clk_NtQuerySystemInformation_TimeOfDay(PVOID Buffer, ULONG Length, PULONG ReturnLength) {
    // Windows 10 accepts the 0x30-byte structure; older callers pass the 0x20-byte
    // prefix. Anything shorter is a length mismatch (nothing is written).
    constexpr ULONG kMinLen = 0x20;
    if (Length < kMinLen) {
        if (ReturnLength) *ReturnLength = sizeof(TimeOfDayInfo);
        return kStatusInfoLengthMismatch;
    }
    if (!Buffer) return kStatusAccessViolation;
    const Vals v = Snapshot();
    TimeOfDayInfo info{};
    info.CurrentTime.QuadPart = v.sys;
    info.BootTime.QuadPart = v.sys - static_cast<std::int64_t>(v.intr);
    info.TimeZoneBias.QuadPart = 0;
    info.TimeZoneId = 0;
    info.BootTimeBias = 0;
    info.SleepTimeBias = 0;
    const ULONG n = Length < sizeof(info) ? Length : static_cast<ULONG>(sizeof(info));
    std::memcpy(Buffer, &info, n);
    if (ReturnLength) *ReturnLength = n;
    return kStatusSuccess;
}

// =============================================================================
// Virtual KUSER_SHARED_DATA
// =============================================================================

const void* VirtualSharedData() { return g_page; }
void* MutableSharedData() { return g_page; }

void UpdateSharedDataNow() {
    std::uint8_t* base = g_page;
    if (!base) return;
    const Vals v = Snapshot();
    const std::uint64_t ticks = v.intr / kTickUnit100ns;
    WriteKSystemTime(base, usd::kInterruptTime, v.intr);
    WriteKSystemTime(base, usd::kSystemTime, static_cast<std::uint64_t>(v.sys));
    WriteKSystemTime(base, usd::kTickCount, ticks);
    Store32(base, usd::kTickCountLowDeprecated, static_cast<std::uint32_t>(ticks));
}

// The page and its static fields (version, product type, architecture), written once, with the time fields as of
// now. Caller holds g_thread_lock.
static NTSTATUS EnsurePageLocked() {
    if (!g_page) {
        void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) {
            ERR(clock, "cannot allocate the shared data page (%lu)", GetLastError());
            return kStatusInsufficientResources;
        }
        g_page = static_cast<std::uint8_t*>(p);
    }
    {
        ReadLock r;
        InitStaticSharedFields(g_page);
    }
    UpdateSharedDataNow();
    return kStatusSuccess;
}

const void* EnsureSharedDataPage() {
    EnsureInit();
    AcquireSRWLockExclusive(&g_thread_lock);
    EnsurePageLocked();
    ReleaseSRWLockExclusive(&g_thread_lock);
    return g_page;
}

NTSTATUS StartSharedDataThread() {
    EnsureInit();
    AcquireSRWLockExclusive(&g_thread_lock);
    NTSTATUS st = EnsurePageLocked();
    if (st != kStatusSuccess) {
        ReleaseSRWLockExclusive(&g_thread_lock);
        return st;
    }
    if (!g_thread) {
        g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_thread = g_stop ? CreateThread(nullptr, 0, SharedDataThread, nullptr, 0, nullptr) : nullptr;
        if (!g_thread) {
            if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
            st = kStatusInsufficientResources;
            ERR(clock, "cannot start the shared data thread (%lu)", GetLastError());
        } else {
            TRACE(clock, "shared data page %p, updater running", g_page);
        }
    }
    ReleaseSRWLockExclusive(&g_thread_lock);
    return st;
}

void StopSharedDataThread() {
    AcquireSRWLockExclusive(&g_thread_lock);
    if (g_thread) {
        SetEvent(g_stop);
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        CloseHandle(g_stop);
        g_thread = nullptr;
        g_stop = nullptr;
    }
    ReleaseSRWLockExclusive(&g_thread_lock);
}

}  // namespace win10xbox::clock
