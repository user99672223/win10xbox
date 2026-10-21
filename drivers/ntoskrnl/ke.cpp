// ntoskrnl Ke* surface: virtual IRQL, spin locks, dispatcher objects and
// waits, DPCs, timers, time queries, bug checks. MIT.
//
// IRQL is a virtual per-thread number kept in a TLS slot; it never touches
// hardware. Spin locks really exclude other host threads (a CAS loop), they
// additionally raise/restore the virtual IRQL. Dispatcher objects (KEVENT,
// KSEMAPHORE, KMUTANT, KTIMER, pseudo KTHREAD) are the driver's own memory;
// all waits share one lock + condition variable, so there are no host
// handles behind them and they can live anywhere.
#include "internal.h"
#include "win10xbox/clock.h"
#include <atomic>
#include <unordered_map>
#include <vector>

namespace win10xbox::drivers {
namespace {
BugCheckHandler g_bugcheck = nullptr;
void* g_bugcheck_ctx = nullptr;
}  // namespace

void SetBugCheckHandler(BugCheckHandler h, void* ctx) {
    g_bugcheck = h;
    g_bugcheck_ctx = ctx;
}
}  // namespace win10xbox::drivers

namespace win10xbox::drivers::detail {

SRWLOCK g_disp_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_disp_cv = CONDITION_VARIABLE_INIT;

namespace {
DWORD g_irql_tls = TLS_OUT_OF_INDEXES;
DWORD g_thread_tls = TLS_OUT_OF_INDEXES;
bool g_strict = false;
SRWLOCK g_thread_map_lock = SRWLOCK_INIT;
std::unordered_map<DWORD, PseudoThread*>& ThreadMap() {
    static std::unordered_map<DWORD, PseudoThread*> m;
    return m;
}

void* TlsFast(DWORD idx) {
    if (idx < 64) return reinterpret_cast<void*>(__readgsqword(0x1480 + idx * 8));   // TEB.TlsSlots
    return TlsGetValue(idx);
}

void Bugcheck(ULONG code, ULONG_PTR p1 = 0, ULONG_PTR p2 = 0, ULONG_PTR p3 = 0, ULONG_PTR p4 = 0) {
    ERR(driver, "*** BUGCHECK 0x%08lx (%p, %p, %p, %p)", code, reinterpret_cast<void*>(p1),
        reinterpret_cast<void*>(p2), reinterpret_cast<void*>(p3), reinterpret_cast<void*>(p4));
    if (win10xbox::drivers::g_bugcheck) {
        const std::uint64_t p[4] = {p1, p2, p3, p4};
        win10xbox::drivers::g_bugcheck(code, p, win10xbox::drivers::g_bugcheck_ctx);
        return;
    }
    ExitProcess(0xDEAD0000u | (code & 0xFFFF));
}

std::uint64_t HostNowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<std::uint64_t>(c.QuadPart) * 1000ull / static_cast<std::uint64_t>(freq.QuadPart);
}
}  // namespace

bool IrqlInit() {
    if (g_irql_tls == TLS_OUT_OF_INDEXES) g_irql_tls = TlsAlloc();
    if (g_thread_tls == TLS_OUT_OF_INDEXES) g_thread_tls = TlsAlloc();
    return g_irql_tls != TLS_OUT_OF_INDEXES && g_thread_tls != TLS_OUT_OF_INDEXES;
}

void IrqlShutdown() {
    if (g_irql_tls != TLS_OUT_OF_INDEXES) TlsFree(g_irql_tls);
    if (g_thread_tls != TLS_OUT_OF_INDEXES) TlsFree(g_thread_tls);
    g_irql_tls = g_thread_tls = TLS_OUT_OF_INDEXES;
    AcquireSRWLockExclusive(&g_thread_map_lock);
    for (auto& kv : ThreadMap()) {
        ObjForget(kv.second);
        PoolFree(kv.second, 0, false);
    }
    ThreadMap().clear();
    ReleaseSRWLockExclusive(&g_thread_map_lock);
}

void SetStrictIrql(bool strict) { g_strict = strict; }
bool StrictIrql() { return g_strict; }

// TLS value: bits 0-7 = virtual IRQL, bit 8 = ExGetPreviousMode() (1 = UserMode),
// bit 9 = the thread is inside a driver dispatch routine called for a top-level request.
KIRQL CurrentIrql() {
    if (g_irql_tls == TLS_OUT_OF_INDEXES) return PASSIVE_LEVEL;
    return static_cast<KIRQL>(reinterpret_cast<std::uintptr_t>(TlsFast(g_irql_tls)) & 0xFF);
}

KIRQL SwapIrql(KIRQL n) {
    if (g_irql_tls == TLS_OUT_OF_INDEXES) return PASSIVE_LEVEL;
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(TlsFast(g_irql_tls));
    TlsSetValue(g_irql_tls, reinterpret_cast<void*>((v & ~std::uintptr_t{0xFF}) | n));
    return static_cast<KIRQL>(v & 0xFF);
}

KPROCESSOR_MODE PreviousMode() {
    if (g_irql_tls == TLS_OUT_OF_INDEXES) return KernelMode;
    return (reinterpret_cast<std::uintptr_t>(TlsFast(g_irql_tls)) & 0x100) ? UserMode : KernelMode;
}

bool InDriverCall() {
    if (g_irql_tls == TLS_OUT_OF_INDEXES) return false;
    return (reinterpret_cast<std::uintptr_t>(TlsFast(g_irql_tls)) & 0x200) != 0;
}

bool SwapInDriverCall(bool v) {
    if (g_irql_tls == TLS_OUT_OF_INDEXES) return false;
    std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(TlsFast(g_irql_tls));
    TlsSetValue(g_irql_tls, reinterpret_cast<void*>((cur & ~std::uintptr_t{0x200}) | (v ? 0x200u : 0u)));
    return (cur & 0x200) != 0;
}

KPROCESSOR_MODE SwapPreviousMode(KPROCESSOR_MODE m) {
    if (g_irql_tls == TLS_OUT_OF_INDEXES) return KernelMode;
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(TlsFast(g_irql_tls));
    TlsSetValue(g_irql_tls, reinterpret_cast<void*>((v & ~std::uintptr_t{0x100}) | (m == UserMode ? 0x100u : 0u)));
    return (v & 0x100) ? UserMode : KernelMode;
}

void IrqlViolation(const char* what, KIRQL current, KIRQL wanted) {
    ERR(driver, "IRQL violation: %s (current %u, requested %u)", what, current, wanted);
    if (g_strict) Bugcheck(wanted < current ? IRQL_NOT_GREATER_OR_EQUAL : IRQL_NOT_LESS_OR_EQUAL, current, wanted);
}

// ---- pseudo threads ---------------------------------------------------------------------------

PseudoThread* PseudoThreadForTid(DWORD tid, bool create) {
    AcquireSRWLockExclusive(&g_thread_map_lock);
    auto& m = ThreadMap();
    auto it = m.find(tid);
    PseudoThread* t = it != m.end() ? it->second : nullptr;
    if (!t && create) {
        t = static_cast<PseudoThread*>(PoolAlloc(sizeof(PseudoThread), NTK_TAG('K', 'T', 'h', 'r')));
        if (t) {
            t->hdr.Type = NTK_TYPE_THREAD;
            t->hdr.Size = sizeof(DISPATCHER_HEADER) / 4;
            InitializeListHead(&t->hdr.WaitListHead);
            t->tid = tid;
            m[tid] = t;
            ObjRegister(t, ObjType::Thread, nullptr, 1);
        }
    }
    ReleaseSRWLockExclusive(&g_thread_map_lock);
    return t;
}

PseudoThread* CurrentPseudoThread() {
    if (g_thread_tls != TLS_OUT_OF_INDEXES) {
        void* v = TlsFast(g_thread_tls);
        if (v) return static_cast<PseudoThread*>(v);
    }
    PseudoThread* t = PseudoThreadForTid(GetCurrentThreadId(), true);
    if (g_thread_tls != TLS_OUT_OF_INDEXES) TlsSetValue(g_thread_tls, t);
    return t;
}

std::int64_t GuestSystemTime100ns() { return win10xbox::clock::SystemTime100ns(); }
std::uint64_t GuestInterruptTime100ns() { return win10xbox::clock::InterruptTime100ns(); }

void DispSignalAll() { WakeAllConditionVariable(&g_disp_cv); }

// ---- spin lock primitives (shared with worker.cpp) ------------------------------------------------

KIRQL SpinAcquireRaise(PKSPIN_LOCK lock, KIRQL raise_to) {
    KIRQL old = CurrentIrql();
    if (raise_to < old) { IrqlViolation("spin lock acquired below the current IRQL", old, raise_to); raise_to = old; }
    SwapIrql(raise_to);
    unsigned spins = 0;
    while (InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(lock), 1, 0) != 0) {
        if (++spins < 200) YieldProcessor();
        else { SwitchToThread(); spins = 0; }
    }
    return old;
}

void SpinReleaseLower(PKSPIN_LOCK lock, KIRQL old) {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(lock), 0);
    KIRQL cur = CurrentIrql();
    if (old > cur) IrqlViolation("spin lock released to a higher IRQL than the current one", cur, old);
    SwapIrql(old);
}

// ---- dispatcher object waits ---------------------------------------------------------------------------

namespace {

bool IsDispatcherType(UCHAR t) {
    return t <= NTK_TYPE_SEMAPHORE || t == NTK_TYPE_THREAD || t == NTK_TYPE_NOTIFICATION_TIMER ||
           t == NTK_TYPE_SYNCHRONIZATION_TIMER;
}

// Lock held. Returns 0 = not satisfied, 1 = satisfied, 2 = satisfied (abandoned mutant).
int TrySatisfy(DISPATCHER_HEADER* h, PseudoThread* self, bool consume) {
    switch (h->Type) {
    case NTK_TYPE_NOTIFICATION_EVENT:
    case NTK_TYPE_NOTIFICATION_TIMER:
    case NTK_TYPE_THREAD:
    case NTK_TYPE_PROCESS:
        return h->SignalState != 0 ? 1 : 0;
    case NTK_TYPE_SYNCHRONIZATION_EVENT:
    case NTK_TYPE_SYNCHRONIZATION_TIMER:
        if (h->SignalState > 0) { if (consume) h->SignalState = 0; return 1; }
        return 0;
    case NTK_TYPE_SEMAPHORE:
        if (h->SignalState > 0) { if (consume) --h->SignalState; return 1; }
        return 0;
    case NTK_TYPE_MUTANT: {
        auto* m = reinterpret_cast<KMUTANT*>(h);
        if (h->SignalState > 0) {
            if (consume) { h->SignalState = 0; m->OwnerThread = reinterpret_cast<PKTHREAD>(self); }
            return 1;
        }
        if (m->OwnerThread == reinterpret_cast<PKTHREAD>(self)) {
            if (consume) --h->SignalState;
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

DWORD TimeoutToMs(const LARGE_INTEGER* timeout, bool* poll) {
    *poll = false;
    if (!timeout) return INFINITE;
    LONGLONG q = timeout->QuadPart;
    if (q == 0) { *poll = true; return 0; }
    LONGLONG rel100;
    if (q < 0) rel100 = -q;
    else rel100 = q - GuestSystemTime100ns();     // absolute system time
    if (rel100 <= 0) { *poll = true; return 0; }
    LONGLONG ms = (rel100 + 9999) / 10000;
    return ms >= static_cast<LONGLONG>(INFINITE) ? INFINITE - 1 : static_cast<DWORD>(ms);
}

}  // namespace

NTSTATUS WaitForObjects(ULONG count, PVOID* objects, WAIT_TYPE wait_type, PLARGE_INTEGER timeout) {
    bool poll;
    const DWORD total_ms = TimeoutToMs(timeout, &poll);
    KIRQL irql = CurrentIrql();
    if (irql >= DISPATCH_LEVEL && !poll) IrqlViolation("wait with a timeout at DISPATCH_LEVEL or above", irql, PASSIVE_LEVEL);
    for (ULONG i = 0; i < count; ++i) {
        auto* h = static_cast<DISPATCHER_HEADER*>(objects[i]);
        if (!h || !IsDispatcherType(h->Type)) {
            ERR(driver, "wait on %p which is not a dispatcher object (type %u)", objects[i], h ? h->Type : 0u);
            return STATUS_INVALID_PARAMETER;
        }
    }
    PseudoThread* self = CurrentPseudoThread();
    const std::uint64_t start = HostNowMs();
    AcquireSRWLockExclusive(&g_disp_lock);
    for (;;) {
        if (wait_type == WaitAll) {
            bool all = true;
            for (ULONG i = 0; i < count && all; ++i)
                all = TrySatisfy(static_cast<DISPATCHER_HEADER*>(objects[i]), self, false) != 0;
            if (all) {
                for (ULONG i = 0; i < count; ++i) TrySatisfy(static_cast<DISPATCHER_HEADER*>(objects[i]), self, true);
                ReleaseSRWLockExclusive(&g_disp_lock);
                return STATUS_WAIT_0;
            }
        } else {
            for (ULONG i = 0; i < count; ++i) {
                int r = TrySatisfy(static_cast<DISPATCHER_HEADER*>(objects[i]), self, true);
                if (r) {
                    ReleaseSRWLockExclusive(&g_disp_lock);
                    return static_cast<NTSTATUS>((r == 2 ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0) + i);
                }
            }
        }
        if (poll) { ReleaseSRWLockExclusive(&g_disp_lock); return STATUS_TIMEOUT; }
        DWORD remaining = INFINITE;
        if (total_ms != INFINITE) {
            std::uint64_t elapsed = HostNowMs() - start;
            if (elapsed >= total_ms) { ReleaseSRWLockExclusive(&g_disp_lock); return STATUS_TIMEOUT; }
            remaining = static_cast<DWORD>(total_ms - elapsed);
        }
        SleepConditionVariableSRW(&g_disp_cv, &g_disp_lock, remaining, 0);
    }
}

LONG DispSetEventLocked(PKEVENT ev) {
    LONG old = ev->Header.SignalState;
    ev->Header.SignalState = 1;
    return old;
}

}  // namespace win10xbox::drivers::detail

// ---------------------------------------------------------------------------------------------------
// Exported API
// ---------------------------------------------------------------------------------------------------
namespace ntk {
using namespace win10xbox::drivers::detail;
namespace clk = win10xbox::clock;

// ---- IRQL ----
KIRQL NTAPI KeGetCurrentIrql(void) { return CurrentIrql(); }

KIRQL NTAPI KfRaiseIrql(KIRQL n) {
    KIRQL old = CurrentIrql();
    if (n < old) IrqlViolation("KeRaiseIrql to a lower level", old, n);
    else SwapIrql(n);
    return old;
}
VOID NTAPI KeRaiseIrql(KIRQL n, PKIRQL old) { *old = KfRaiseIrql(n); }
VOID NTAPI KfLowerIrql(KIRQL n) {
    KIRQL cur = CurrentIrql();
    if (n > cur) { IrqlViolation("KeLowerIrql to a higher level", cur, n); return; }
    SwapIrql(n);
}
VOID NTAPI KeLowerIrql(KIRQL n) { KfLowerIrql(n); }
KIRQL NTAPI KeRaiseIrqlToDpcLevel(void) { return KfRaiseIrql(DISPATCH_LEVEL); }
KIRQL NTAPI KeRaiseIrqlToSynchLevel(void) { return KfRaiseIrql(SYNCH_LEVEL); }

// ---- spin locks ----
VOID NTAPI KeInitializeSpinLock(PKSPIN_LOCK l) { *l = 0; }
KIRQL NTAPI KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK l) { return SpinAcquireRaise(l, DISPATCH_LEVEL); }
VOID NTAPI KeAcquireSpinLock(PKSPIN_LOCK l, PKIRQL old) { *old = SpinAcquireRaise(l, DISPATCH_LEVEL); }
VOID NTAPI KeReleaseSpinLock(PKSPIN_LOCK l, KIRQL old) { SpinReleaseLower(l, old); }
VOID NTAPI KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK l) {
    KIRQL cur = CurrentIrql();
    if (cur < DISPATCH_LEVEL) IrqlViolation("KeAcquireSpinLockAtDpcLevel below DISPATCH_LEVEL", cur, DISPATCH_LEVEL);
    unsigned spins = 0;
    while (InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(l), 1, 0) != 0) {
        if (++spins < 200) YieldProcessor(); else { SwitchToThread(); spins = 0; }
    }
}
VOID NTAPI KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK l) { InterlockedExchange64(reinterpret_cast<volatile LONG64*>(l), 0); }
BOOLEAN NTAPI KeTryToAcquireSpinLockAtDpcLevel(PKSPIN_LOCK l) {
    return InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(l), 1, 0) == 0;
}
BOOLEAN NTAPI KeTestSpinLock(PKSPIN_LOCK l) { return *l == 0; }
VOID NTAPI KeAcquireInStackQueuedSpinLock(PKSPIN_LOCK l, PKLOCK_QUEUE_HANDLE h) {
    h->LockQueue.Next = nullptr;
    h->LockQueue.Lock = l;
    h->OldIrql = SpinAcquireRaise(l, DISPATCH_LEVEL);
}
VOID NTAPI KeReleaseInStackQueuedSpinLock(PKLOCK_QUEUE_HANDLE h) { SpinReleaseLower(h->LockQueue.Lock, h->OldIrql); }
VOID NTAPI KeAcquireInStackQueuedSpinLockAtDpcLevel(PKSPIN_LOCK l, PKLOCK_QUEUE_HANDLE h) {
    h->LockQueue.Next = nullptr;
    h->LockQueue.Lock = l;
    KeAcquireSpinLockAtDpcLevel(l);
}
VOID NTAPI KeReleaseInStackQueuedSpinLockFromDpcLevel(PKLOCK_QUEUE_HANDLE h) { KeReleaseSpinLockFromDpcLevel(h->LockQueue.Lock); }
KIRQL NTAPI KeAcquireInterruptSpinLock(PVOID interrupt) {
    (void)interrupt;
    FIXME(driver, "KeAcquireInterruptSpinLock is not implemented");
    return PASSIVE_LEVEL;
}

namespace {
KSPIN_LOCK g_queued_locks[34];
}
KIRQL NTAPI KeAcquireQueuedSpinLock(ULONG number) {
    if (number >= 34) { ERR(driver, "KeAcquireQueuedSpinLock(%lu)", number); return PASSIVE_LEVEL; }
    return SpinAcquireRaise(&g_queued_locks[number], DISPATCH_LEVEL);
}
VOID NTAPI KeReleaseQueuedSpinLock(ULONG number, KIRQL old) {
    if (number < 34) SpinReleaseLower(&g_queued_locks[number], old);
}

// ---- events ----
VOID NTAPI KeInitializeEvent(PKEVENT ev, EVENT_TYPE type, BOOLEAN state) {
    ev->Header.Type = static_cast<UCHAR>(type);
    ev->Header.Absolute = 0;
    ev->Header.Size = sizeof(KEVENT) / sizeof(LONG);
    ev->Header.Inserted = 0;
    ev->Header.SignalState = state ? 1 : 0;
    InitializeListHead(&ev->Header.WaitListHead);
}
LONG NTAPI KeSetEvent(PKEVENT ev, LONG increment, BOOLEAN wait) {
    (void)increment; (void)wait;
    AcquireSRWLockExclusive(&g_disp_lock);
    LONG old = DispSetEventLocked(ev);
    ReleaseSRWLockExclusive(&g_disp_lock);
    DispSignalAll();
    return old;
}
LONG NTAPI KeResetEvent(PKEVENT ev) {
    AcquireSRWLockExclusive(&g_disp_lock);
    LONG old = ev->Header.SignalState;
    ev->Header.SignalState = 0;
    ReleaseSRWLockExclusive(&g_disp_lock);
    return old;
}
VOID NTAPI KeClearEvent(PKEVENT ev) { ev->Header.SignalState = 0; }
LONG NTAPI KePulseEvent(PKEVENT ev, LONG increment, BOOLEAN wait) {
    (void)increment; (void)wait;
    AcquireSRWLockExclusive(&g_disp_lock);
    LONG old = ev->Header.SignalState;
    ev->Header.SignalState = 1;
    ReleaseSRWLockExclusive(&g_disp_lock);
    DispSignalAll();
    SwitchToThread();
    AcquireSRWLockExclusive(&g_disp_lock);
    ev->Header.SignalState = 0;
    ReleaseSRWLockExclusive(&g_disp_lock);
    return old;
}
LONG NTAPI KeReadStateEvent(PKEVENT ev) { return ev->Header.SignalState; }

// ---- semaphores ----
VOID NTAPI KeInitializeSemaphore(PKSEMAPHORE s, LONG count, LONG limit) {
    s->Header.Type = NTK_TYPE_SEMAPHORE;
    s->Header.Absolute = 0;
    s->Header.Size = sizeof(KSEMAPHORE) / sizeof(LONG);
    s->Header.Inserted = 0;
    s->Header.SignalState = count;
    InitializeListHead(&s->Header.WaitListHead);
    s->Limit = limit;
}
LONG NTAPI KeReleaseSemaphore(PKSEMAPHORE s, LONG increment, LONG adjustment, BOOLEAN wait) {
    (void)increment; (void)wait;
    AcquireSRWLockExclusive(&g_disp_lock);
    LONG old = s->Header.SignalState;
    if (old + adjustment > s->Limit || old + adjustment < old) {
        ReleaseSRWLockExclusive(&g_disp_lock);
        ERR(driver, "KeReleaseSemaphore: limit exceeded");
        RaiseException(0xC0000047 /* STATUS_SEMAPHORE_LIMIT_EXCEEDED */, 0, 0, nullptr);
        return old;
    }
    s->Header.SignalState = old + adjustment;
    ReleaseSRWLockExclusive(&g_disp_lock);
    DispSignalAll();
    return old;
}
LONG NTAPI KeReadStateSemaphore(PKSEMAPHORE s) { return s->Header.SignalState; }

// ---- mutexes ----
VOID NTAPI KeInitializeMutex(PKMUTEX m, ULONG level) {
    (void)level;
    m->Header.Type = NTK_TYPE_MUTANT;
    m->Header.Absolute = 0;
    m->Header.Size = sizeof(KMUTEX) / sizeof(LONG);
    m->Header.Inserted = 0;
    m->Header.SignalState = 1;
    InitializeListHead(&m->Header.WaitListHead);
    m->OwnerThread = nullptr;
    m->Abandoned = 0;
    m->ApcDisable = 1;
}
LONG NTAPI KeReleaseMutex(PKMUTEX m, BOOLEAN wait) {
    (void)wait;
    PseudoThread* self = CurrentPseudoThread();
    AcquireSRWLockExclusive(&g_disp_lock);
    if (m->OwnerThread != reinterpret_cast<PKTHREAD>(self)) {
        ReleaseSRWLockExclusive(&g_disp_lock);
        ERR(driver, "KeReleaseMutex by a thread that does not own %p", static_cast<void*>(m));
        RaiseException(0xC0000046 /* STATUS_MUTANT_NOT_OWNED */, 0, 0, nullptr);
        return 0;
    }
    LONG old = m->Header.SignalState;
    ++m->Header.SignalState;
    if (m->Header.SignalState == 1) m->OwnerThread = nullptr;
    ReleaseSRWLockExclusive(&g_disp_lock);
    DispSignalAll();
    return old;
}
LONG NTAPI KeReadStateMutex(PKMUTEX m) { return m->Header.SignalState; }

// ---- fast / guarded mutexes ----
namespace {
void MutexAcquire(PFAST_MUTEX m, bool raise) {
    PseudoThread* self = CurrentPseudoThread();
    if (m->Owner == reinterpret_cast<PKTHREAD>(self)) {
        ERR(driver, "recursive acquire of fast mutex %p", static_cast<void*>(m));
    }
    KIRQL old = raise ? KfRaiseIrql(APC_LEVEL) : CurrentIrql();
    AcquireSRWLockExclusive(&g_disp_lock);
    while (m->Count != 1) SleepConditionVariableSRW(&g_disp_cv, &g_disp_lock, INFINITE, 0);
    m->Count = 0;
    m->Owner = reinterpret_cast<PKTHREAD>(self);
    m->OldIrql = old;
    ReleaseSRWLockExclusive(&g_disp_lock);
}
void MutexRelease(PFAST_MUTEX m, bool lower) {
    AcquireSRWLockExclusive(&g_disp_lock);
    KIRQL old = static_cast<KIRQL>(m->OldIrql);
    m->Count = 1;
    m->Owner = nullptr;
    ReleaseSRWLockExclusive(&g_disp_lock);
    DispSignalAll();
    if (lower) KfLowerIrql(old);
}
}  // namespace
VOID NTAPI ExAcquireFastMutex(PFAST_MUTEX m) { MutexAcquire(m, true); }
VOID NTAPI ExReleaseFastMutex(PFAST_MUTEX m) { MutexRelease(m, true); }
VOID NTAPI ExAcquireFastMutexUnsafe(PFAST_MUTEX m) { MutexAcquire(m, false); }
VOID NTAPI ExReleaseFastMutexUnsafe(PFAST_MUTEX m) { MutexRelease(m, false); }
BOOLEAN NTAPI ExTryToAcquireFastMutex(PFAST_MUTEX m) {
    KIRQL old = KfRaiseIrql(APC_LEVEL);
    AcquireSRWLockExclusive(&g_disp_lock);
    bool ok = m->Count == 1;
    if (ok) { m->Count = 0; m->Owner = reinterpret_cast<PKTHREAD>(CurrentPseudoThread()); m->OldIrql = old; }
    ReleaseSRWLockExclusive(&g_disp_lock);
    if (!ok) KfLowerIrql(old);
    return ok;
}
VOID NTAPI KeInitializeGuardedMutex(PKGUARDED_MUTEX m) {
    m->Count = 1;
    m->Owner = nullptr;
    m->Contention = 0;
    KeInitializeEvent(&m->Event, SynchronizationEvent, FALSE);
    m->OldIrql = 0;
}
VOID NTAPI KeAcquireGuardedMutex(PKGUARDED_MUTEX m) { MutexAcquire(m, false); }
VOID NTAPI KeReleaseGuardedMutex(PKGUARDED_MUTEX m) { MutexRelease(m, false); }
VOID NTAPI KeAcquireGuardedMutexUnsafe(PKGUARDED_MUTEX m) { MutexAcquire(m, false); }
VOID NTAPI KeReleaseGuardedMutexUnsafe(PKGUARDED_MUTEX m) { MutexRelease(m, false); }
BOOLEAN NTAPI KeTryToAcquireGuardedMutex(PKGUARDED_MUTEX m) {
    AcquireSRWLockExclusive(&g_disp_lock);
    bool ok = m->Count == 1;
    if (ok) { m->Count = 0; m->Owner = reinterpret_cast<PKTHREAD>(CurrentPseudoThread()); }
    ReleaseSRWLockExclusive(&g_disp_lock);
    return ok;
}

// ---- waits ----
NTSTATUS NTAPI KeWaitForSingleObject(PVOID object, KWAIT_REASON reason, KPROCESSOR_MODE mode, BOOLEAN alertable,
                                     PLARGE_INTEGER timeout) {
    (void)reason; (void)mode; (void)alertable;
    return WaitForObjects(1, &object, WaitAny, timeout);
}
NTSTATUS NTAPI KeWaitForMultipleObjects(ULONG count, PVOID objects[], WAIT_TYPE type, KWAIT_REASON reason,
                                        KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER timeout,
                                        PVOID wait_block) {
    (void)reason; (void)mode; (void)alertable; (void)wait_block;
    if (count == 0 || count > 64) return STATUS_INVALID_PARAMETER;
    return WaitForObjects(count, objects, type, timeout);
}
NTSTATUS NTAPI KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER interval) {
    (void)mode; (void)alertable;
    KIRQL irql = CurrentIrql();
    if (irql >= DISPATCH_LEVEL) IrqlViolation("KeDelayExecutionThread at DISPATCH_LEVEL or above", irql, PASSIVE_LEVEL);
    if (!interval) return STATUS_INVALID_PARAMETER;
    LONGLONG q = interval->QuadPart;
    LONGLONG rel = q < 0 ? -q : q - GuestSystemTime100ns();
    Sleep(rel <= 0 ? 0 : static_cast<DWORD>((rel + 9999) / 10000));
    return STATUS_SUCCESS;
}
VOID NTAPI KeStallExecutionProcessor(ULONG microseconds) {
    LARGE_INTEGER f, s, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&s);
    const LONGLONG target = static_cast<LONGLONG>(microseconds) * f.QuadPart / 1000000;
    do { YieldProcessor(); QueryPerformanceCounter(&c); } while (c.QuadPart - s.QuadPart < target);
}

// ---- DPCs ----
VOID NTAPI KeInitializeDpc(PKDPC dpc, PKDEFERRED_ROUTINE routine, PVOID context) {
    std::memset(dpc, 0, sizeof(*dpc));
    dpc->Type = 0x13;       // DpcObject
    dpc->Importance = 1;    // MediumImportance
    dpc->DeferredRoutine = routine;
    dpc->DeferredContext = context;
}
VOID NTAPI KeInitializeThreadedDpc(PKDPC dpc, PKDEFERRED_ROUTINE routine, PVOID context) {
    KeInitializeDpc(dpc, routine, context);
    dpc->Type = 0x1A;       // ThreadedDpcObject
}
BOOLEAN NTAPI KeInsertQueueDpc(PKDPC dpc, PVOID a1, PVOID a2) { return WorkerQueueDpc(dpc, a1, a2); }
BOOLEAN NTAPI KeRemoveQueueDpc(PKDPC dpc) { return WorkerRemoveDpc(dpc); }
VOID NTAPI KeSetImportanceDpc(PKDPC dpc, int importance) { dpc->Importance = static_cast<UCHAR>(importance); }
VOID NTAPI KeSetTargetProcessorDpc(PKDPC dpc, CCHAR number) { dpc->Number = static_cast<USHORT>(number); }
NTSTATUS NTAPI KeSetTargetProcessorDpcEx(PKDPC dpc, PVOID processor_number) { (void)dpc; (void)processor_number; return STATUS_SUCCESS; }
VOID NTAPI KeFlushQueuedDpcs(void) { WorkerFlushDpcs(); }

// ---- timers ----
VOID NTAPI KeInitializeTimerEx(PKTIMER t, TIMER_TYPE type) {
    std::memset(t, 0, sizeof(*t));
    t->Header.Type = static_cast<UCHAR>(NTK_TYPE_NOTIFICATION_TIMER + (type == SynchronizationTimer ? 1 : 0));
    t->Header.Size = sizeof(KTIMER) / sizeof(LONG);
    InitializeListHead(&t->Header.WaitListHead);
    t->TimerType = static_cast<USHORT>(type);
}
VOID NTAPI KeInitializeTimer(PKTIMER t) { KeInitializeTimerEx(t, NotificationTimer); }
BOOLEAN NTAPI KeSetTimerEx(PKTIMER t, LARGE_INTEGER due, LONG period_ms, PKDPC dpc) {
    LONGLONG q = due.QuadPart;
    LONGLONG rel = q < 0 ? -q : q - GuestSystemTime100ns();
    AcquireSRWLockExclusive(&g_disp_lock);
    t->Header.SignalState = 0;
    t->Header.Inserted = 1;
    t->DueTime.QuadPart = static_cast<ULONGLONG>(q);
    t->Period = static_cast<ULONG>(period_ms);
    t->Dpc = dpc;
    ReleaseSRWLockExclusive(&g_disp_lock);
    return WorkerSetTimer(t, rel, static_cast<ULONG>(period_ms), dpc);
}
BOOLEAN NTAPI KeSetTimer(PKTIMER t, LARGE_INTEGER due, PKDPC dpc) { return KeSetTimerEx(t, due, 0, dpc); }
BOOLEAN NTAPI KeSetCoalescableTimer(PKTIMER t, LARGE_INTEGER due, ULONG period, ULONG tolerable, PKDPC dpc) {
    (void)tolerable;
    return KeSetTimerEx(t, due, static_cast<LONG>(period), dpc);
}
BOOLEAN NTAPI KeCancelTimer(PKTIMER t) {
    bool was = WorkerCancelTimer(t);
    AcquireSRWLockExclusive(&g_disp_lock);
    t->Header.Inserted = 0;
    ReleaseSRWLockExclusive(&g_disp_lock);
    return was;
}
BOOLEAN NTAPI KeReadStateTimer(PKTIMER t) { return t->Header.SignalState != 0; }

// ---- time ----
VOID NTAPI KeQuerySystemTime(PLARGE_INTEGER t) { t->QuadPart = GuestSystemTime100ns(); }
VOID NTAPI KeQuerySystemTimePrecise(PLARGE_INTEGER t) { t->QuadPart = GuestSystemTime100ns(); }
ULONGLONG NTAPI KeQueryInterruptTime(void) { return GuestInterruptTime100ns(); }
ULONGLONG NTAPI KeQueryUnbiasedInterruptTime(void) { return GuestInterruptTime100ns(); }
ULONGLONG NTAPI KeQueryInterruptTimePrecise(PULONG64 qpc) {
    if (qpc) *qpc = clk::Qpc();
    return GuestInterruptTime100ns();
}
ULONG NTAPI KeQueryTimeIncrement(void) { return 156250; }
VOID NTAPI KeQueryTickCount(PLARGE_INTEGER count) { count->QuadPart = static_cast<LONGLONG>(GuestInterruptTime100ns() / 156250); }
LARGE_INTEGER NTAPI KeQueryPerformanceCounter(PLARGE_INTEGER freq) {
    LARGE_INTEGER r;
    r.QuadPart = static_cast<LONGLONG>(clk::Qpc());
    if (freq) freq->QuadPart = static_cast<LONGLONG>(clk::QpcFrequency());
    return r;
}

// ---- processors ----
ULONG NTAPI KeGetCurrentProcessorNumber(void) { return GetCurrentProcessorNumber(); }
ULONG NTAPI KeGetCurrentProcessorNumberEx(PVOID number) {
    PROCESSOR_NUMBER pn;
    GetCurrentProcessorNumberEx(&pn);
    if (number) std::memcpy(number, &pn, sizeof(pn));
    return pn.Number;
}
ULONG NTAPI KeQueryActiveProcessorCount(PKAFFINITY active) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (active) *active = si.dwActiveProcessorMask;
    return si.dwNumberOfProcessors;
}
ULONG NTAPI KeQueryActiveProcessorCountEx(USHORT group) { (void)group; return KeQueryActiveProcessorCount(nullptr); }
ULONG NTAPI KeQueryMaximumProcessorCount(void) { return KeQueryActiveProcessorCount(nullptr); }
ULONG NTAPI KeQueryMaximumProcessorCountEx(USHORT group) { (void)group; return KeQueryActiveProcessorCount(nullptr); }
KAFFINITY NTAPI KeQueryActiveProcessors(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwActiveProcessorMask;
}
USHORT NTAPI KeQueryActiveGroupCount(void) { return 1; }
KAFFINITY NTAPI KeSetSystemAffinityThreadEx(KAFFINITY a) { (void)a; return 0; }
VOID NTAPI KeRevertToUserAffinityThreadEx(KAFFINITY a) { (void)a; }
VOID NTAPI KeSetSystemAffinityThread(KAFFINITY a) { (void)a; }
VOID NTAPI KeRevertToUserAffinityThread(void) {}

// ---- thread / region bookkeeping ----
PKTHREAD NTAPI KeGetCurrentThread(void) { return reinterpret_cast<PKTHREAD>(CurrentPseudoThread()); }
VOID NTAPI KeEnterCriticalRegion(void) {}
VOID NTAPI KeLeaveCriticalRegion(void) {}
VOID NTAPI KeEnterGuardedRegion(void) {}
VOID NTAPI KeLeaveGuardedRegion(void) {}
KPRIORITY NTAPI KeSetPriorityThread(PKTHREAD t, KPRIORITY p) { (void)t; (void)p; return 8; }
KPRIORITY NTAPI KeQueryPriorityThread(PKTHREAD t) { (void)t; return 8; }
BOOLEAN NTAPI KeAreApcsDisabled(void) { return FALSE; }

// ---- bug checks ----
VOID NTAPI KeBugCheckEx(ULONG code, ULONG_PTR p1, ULONG_PTR p2, ULONG_PTR p3, ULONG_PTR p4) { Bugcheck(code, p1, p2, p3, p4); }
VOID NTAPI KeBugCheck(ULONG code) { Bugcheck(code); }
BOOLEAN NTAPI KeRegisterBugCheckCallback(PVOID record, PVOID routine, PVOID buffer, ULONG length, PUCHAR component) {
    (void)record; (void)routine; (void)buffer; (void)length; (void)component;
    return TRUE;    // accepted; a bug check callback is never invoked because there is no crash dump
}
BOOLEAN NTAPI KeDeregisterBugCheckCallback(PVOID record) { (void)record; return TRUE; }

// ---- interrupts ----
NTSTATUS NTAPI IoConnectInterrupt(PKINTERRUPT* interrupt, PKSERVICE_ROUTINE routine, PVOID ctx, PKSPIN_LOCK lock,
                                  ULONG vector, KIRQL irql, KIRQL sync_irql, KINTERRUPT_MODE mode, BOOLEAN share,
                                  KAFFINITY affinity, BOOLEAN floating_save) {
    (void)mode; (void)share; (void)affinity; (void)floating_save;
    if (!interrupt || !routine) return STATUS_INVALID_PARAMETER;
    InterruptRec* r = InterruptConnect(routine, ctx, lock, vector, irql, sync_irql);
    if (!r) return STATUS_INSUFFICIENT_RESOURCES;
    *interrupt = reinterpret_cast<PKINTERRUPT>(r);
    return STATUS_SUCCESS;
}
VOID NTAPI IoDisconnectInterrupt(PKINTERRUPT interrupt) { InterruptDisconnect(reinterpret_cast<InterruptRec*>(interrupt)); }
BOOLEAN NTAPI KeSynchronizeExecution(PKINTERRUPT interrupt, PKSYNCHRONIZE_ROUTINE routine, PVOID ctx) {
    return InterruptSynchronize(reinterpret_cast<InterruptRec*>(interrupt), routine, ctx);
}

// ---- data exports ----
ULONG g_KeNumberProcessors = 1;
BOOLEAN g_KdDebuggerNotPresent = TRUE;
BOOLEAN g_KdDebuggerEnabled = FALSE;

}  // namespace ntk

namespace win10xbox::drivers::detail {
namespace {
const Export kKeTable[] = {
#define X(n) {#n, reinterpret_cast<void*>(&ntk::n)}
    X(KeGetCurrentIrql), X(KfRaiseIrql), X(KeRaiseIrql), X(KfLowerIrql), X(KeLowerIrql),
    X(KeRaiseIrqlToDpcLevel), X(KeRaiseIrqlToSynchLevel),
    X(KeInitializeSpinLock), X(KeAcquireSpinLockRaiseToDpc), X(KeAcquireSpinLock), X(KeReleaseSpinLock),
    X(KeAcquireSpinLockAtDpcLevel), X(KeReleaseSpinLockFromDpcLevel), X(KeTryToAcquireSpinLockAtDpcLevel),
    X(KeTestSpinLock), X(KeAcquireInStackQueuedSpinLock), X(KeReleaseInStackQueuedSpinLock),
    X(KeAcquireInStackQueuedSpinLockAtDpcLevel), X(KeReleaseInStackQueuedSpinLockFromDpcLevel),
    X(KeAcquireQueuedSpinLock), X(KeReleaseQueuedSpinLock),
    X(KeInitializeEvent), X(KeSetEvent), X(KeResetEvent), X(KeClearEvent), X(KePulseEvent), X(KeReadStateEvent),
    X(KeInitializeSemaphore), X(KeReleaseSemaphore), X(KeReadStateSemaphore),
    X(KeInitializeMutex), X(KeReleaseMutex), X(KeReadStateMutex),
    X(ExAcquireFastMutex), X(ExReleaseFastMutex), X(ExAcquireFastMutexUnsafe), X(ExReleaseFastMutexUnsafe),
    X(ExTryToAcquireFastMutex),
    X(KeInitializeGuardedMutex), X(KeAcquireGuardedMutex), X(KeReleaseGuardedMutex),
    X(KeAcquireGuardedMutexUnsafe), X(KeReleaseGuardedMutexUnsafe), X(KeTryToAcquireGuardedMutex),
    X(KeWaitForSingleObject), X(KeWaitForMultipleObjects), X(KeDelayExecutionThread), X(KeStallExecutionProcessor),
    X(KeInitializeDpc), X(KeInitializeThreadedDpc), X(KeInsertQueueDpc), X(KeRemoveQueueDpc), X(KeSetImportanceDpc),
    X(KeSetTargetProcessorDpc), X(KeSetTargetProcessorDpcEx), X(KeFlushQueuedDpcs),
    X(KeInitializeTimerEx), X(KeInitializeTimer), X(KeSetTimerEx), X(KeSetTimer), X(KeSetCoalescableTimer),
    X(KeCancelTimer), X(KeReadStateTimer),
    X(KeQuerySystemTime), X(KeQuerySystemTimePrecise), X(KeQueryInterruptTime), X(KeQueryUnbiasedInterruptTime),
    X(KeQueryInterruptTimePrecise), X(KeQueryTimeIncrement), X(KeQueryTickCount), X(KeQueryPerformanceCounter),
    X(KeGetCurrentProcessorNumber), X(KeGetCurrentProcessorNumberEx), X(KeQueryActiveProcessorCount),
    X(KeQueryActiveProcessorCountEx), X(KeQueryMaximumProcessorCount), X(KeQueryMaximumProcessorCountEx),
    X(KeQueryActiveProcessors), X(KeQueryActiveGroupCount), X(KeSetSystemAffinityThreadEx),
    X(KeRevertToUserAffinityThreadEx), X(KeSetSystemAffinityThread), X(KeRevertToUserAffinityThread),
    X(KeGetCurrentThread), X(KeEnterCriticalRegion), X(KeLeaveCriticalRegion), X(KeEnterGuardedRegion),
    X(KeLeaveGuardedRegion), X(KeSetPriorityThread), X(KeQueryPriorityThread), X(KeAreApcsDisabled),
    X(KeBugCheckEx), X(KeBugCheck), X(KeRegisterBugCheckCallback), X(KeDeregisterBugCheckCallback),
    X(IoConnectInterrupt), X(IoDisconnectInterrupt), X(KeSynchronizeExecution),
#undef X
    // Data exports: the symbol is the variable, the import resolves to its address.
    {"KeNumberProcessors", &ntk::g_KeNumberProcessors},
    {"KdDebuggerNotPresent", &ntk::g_KdDebuggerNotPresent},
    {"KdDebuggerEnabled", &ntk::g_KdDebuggerEnabled},
};
}  // namespace
ExportGroup KeExports() { return {kKeTable, sizeof(kKeTable) / sizeof(kKeTable[0])}; }
}  // namespace win10xbox::drivers::detail
