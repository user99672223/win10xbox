// Dedicated worker thread of the driver host: DPCs, kernel timers, work
// items and interrupt service routines run here, never on the thread that
// queued them (that thread stays at its own virtual IRQL).
//
// Real Windows runs a queued DPC when the processor's IRQL drops below
// DISPATCH_LEVEL. The host cannot interrupt a thread, so a DPC starts as
// soon as the worker thread picks it up; the observable differences are
// documented in drivers/README.md ("What differs from a real kernel").
#include "internal.h"
#include <algorithm>
#include <deque>
#include <vector>

namespace ntk {
VOID NTAPI KeInitializeEvent(PKEVENT, EVENT_TYPE, BOOLEAN);
}

namespace win10xbox::drivers::detail {
namespace {

struct Item {
    enum Kind { Dpc, Fn, FnDispatch } kind;
    PKDPC dpc = nullptr;
    PVOID a1 = nullptr, a2 = nullptr;
    std::function<void()> fn;
};

struct TimerRec {
    PKTIMER timer;
    std::uint64_t due_ms;      // host monotonic
    ULONG period_ms;
    PKDPC dpc;
};

SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
std::deque<Item> g_queue;
std::vector<TimerRec> g_timers;
HANDLE g_thread = nullptr;
DWORD g_tid = 0;
bool g_stop = false;

std::uint64_t NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<std::uint64_t>(c.QuadPart) * 1000ull / static_cast<std::uint64_t>(freq.QuadPart);
}

void RunDpc(PKDPC dpc, PVOID a1, PVOID a2) {
    PKDEFERRED_ROUTINE r = dpc->DeferredRoutine;
    if (!r) return;
    KIRQL old = SwapIrql(DISPATCH_LEVEL);
    TRACE(driver, "DPC %p routine=%p ctx=%p", static_cast<void*>(dpc), reinterpret_cast<void*>(r), dpc->DeferredContext);
    r(dpc, dpc->DeferredContext, a1, a2);
    SwapIrql(old);
}

void FireTimer(const TimerRec& t) {
    AcquireSRWLockExclusive(&g_disp_lock);
    t.timer->Header.SignalState = 1;
    t.timer->Header.Inserted = 0;
    ReleaseSRWLockExclusive(&g_disp_lock);
    DispSignalAll();
    if (t.dpc) RunDpc(t.dpc, nullptr, nullptr);
}

DWORD WINAPI WorkerMain(LPVOID) {
    for (;;) {
        std::vector<TimerRec> due;
        std::deque<Item> work;
        AcquireSRWLockExclusive(&g_lock);
        for (;;) {
            const std::uint64_t now = NowMs();
            for (auto it = g_timers.begin(); it != g_timers.end();) {
                if (it->due_ms <= now) {
                    due.push_back(*it);
                    if (it->period_ms) {
                        it->due_ms = now + it->period_ms;
                        ++it;
                    } else {
                        it = g_timers.erase(it);
                    }
                } else {
                    ++it;
                }
            }
            if (!due.empty() || !g_queue.empty() || g_stop) break;
            DWORD wait = INFINITE;
            for (const TimerRec& t : g_timers) {
                std::uint64_t d = t.due_ms > now ? t.due_ms - now : 0;
                if (d < wait) wait = static_cast<DWORD>(d);
            }
            SleepConditionVariableSRW(&g_cv, &g_lock, wait, 0);
        }
        work.swap(g_queue);
        const bool stop = g_stop;
        // Mark DPCs as taken (re-queueable from their own routine).
        for (Item& it : work)
            if (it.kind == Item::Dpc) it.dpc->DpcData = nullptr;
        ReleaseSRWLockExclusive(&g_lock);

        for (const TimerRec& t : due) FireTimer(t);
        for (Item& it : work) {
            switch (it.kind) {
            case Item::Dpc: RunDpc(it.dpc, it.a1, it.a2); break;
            case Item::Fn: it.fn(); break;
            case Item::FnDispatch: {
                KIRQL old = SwapIrql(DISPATCH_LEVEL);
                it.fn();
                SwapIrql(old);
                break;
            }
            }
        }
        if (stop && work.empty() && due.empty()) break;
    }
    return 0;
}

}  // namespace

bool WorkerStart() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_thread) { ReleaseSRWLockExclusive(&g_lock); return true; }
    g_stop = false;
    ReleaseSRWLockExclusive(&g_lock);
    HANDLE h = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, &g_tid);
    if (!h) return false;
    g_thread = h;
    TRACE(driver, "worker thread %lu started", g_tid);
    return true;
}

void WorkerStop() {
    if (!g_thread) return;
    AcquireSRWLockExclusive(&g_lock);
    g_stop = true;
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
    if (WaitForSingleObject(g_thread, 10000) != WAIT_OBJECT_0) WARN(driver, "worker thread did not stop in 10 s");
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_tid = 0;
    AcquireSRWLockExclusive(&g_lock);
    g_queue.clear();
    g_timers.clear();
    ReleaseSRWLockExclusive(&g_lock);
}

bool WorkerOnWorkerThread() { return g_thread && GetCurrentThreadId() == g_tid; }

bool WorkerQueueDpc(PKDPC dpc, PVOID a1, PVOID a2) {
    if (!g_thread) { ERR(driver, "KeInsertQueueDpc before the driver host was initialised"); return false; }
    AcquireSRWLockExclusive(&g_lock);
    if (dpc->DpcData) { ReleaseSRWLockExclusive(&g_lock); return false; }   // already queued
    dpc->DpcData = &g_queue;                                                  // "inserted" marker
    dpc->SystemArgument1 = a1;
    dpc->SystemArgument2 = a2;
    Item it;
    it.kind = Item::Dpc;
    it.dpc = dpc;
    it.a1 = a1;
    it.a2 = a2;
    g_queue.push_back(std::move(it));
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
    return true;
}

bool WorkerRemoveDpc(PKDPC dpc) {
    bool removed = false;
    AcquireSRWLockExclusive(&g_lock);
    if (dpc->DpcData) {
        for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
            if (it->kind == Item::Dpc && it->dpc == dpc) { g_queue.erase(it); removed = true; break; }
        if (removed) dpc->DpcData = nullptr;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return removed;
}

void WorkerPost(std::function<void()> fn) {
    if (!g_thread) { ERR(driver, "work posted before the driver host was initialised"); return; }
    Item it;
    it.kind = Item::Fn;
    it.fn = std::move(fn);
    AcquireSRWLockExclusive(&g_lock);
    g_queue.push_back(std::move(it));
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
}

void WorkerPostDispatch(std::function<void()> fn) {
    if (!g_thread) { ERR(driver, "work posted before the driver host was initialised"); return; }
    Item it;
    it.kind = Item::FnDispatch;
    it.fn = std::move(fn);
    AcquireSRWLockExclusive(&g_lock);
    g_queue.push_back(std::move(it));
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
}

void WorkerFlushDpcs() {
    if (!g_thread || WorkerOnWorkerThread()) return;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return;
    WorkerPost([ev] { SetEvent(ev); });
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
}

bool WorkerSetTimer(PKTIMER t, std::int64_t due_100ns_relative, ULONG period_ms, PKDPC dpc) {
    bool was_queued = false;
    std::uint64_t ms = due_100ns_relative <= 0 ? 0 : static_cast<std::uint64_t>((due_100ns_relative + 9999) / 10000);
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = g_timers.begin(); it != g_timers.end(); ++it)
        if (it->timer == t) { g_timers.erase(it); was_queued = true; break; }
    g_timers.push_back(TimerRec{t, NowMs() + ms, period_ms, dpc});
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
    return was_queued;
}

bool WorkerCancelTimer(PKTIMER t) {
    bool was_queued = false;
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = g_timers.begin(); it != g_timers.end(); ++it)
        if (it->timer == t) { g_timers.erase(it); was_queued = true; break; }
    ReleaseSRWLockExclusive(&g_lock);
    return was_queued;
}

// ---- interrupts ------------------------------------------------------------------------------

struct InterruptRec {
    PKSERVICE_ROUTINE routine;
    PVOID ctx;
    PKSPIN_LOCK lock;
    KSPIN_LOCK own_lock;
    ULONG vector;
    KIRQL irql, sync_irql;
    bool connected;
};

namespace {
SRWLOCK g_int_lock = SRWLOCK_INIT;
std::vector<InterruptRec*> g_interrupts;
}  // namespace

// Spin helpers shared with ke.cpp (declared there).
KIRQL SpinAcquireRaise(PKSPIN_LOCK lock, KIRQL raise_to);
void SpinReleaseLower(PKSPIN_LOCK lock, KIRQL old);

InterruptRec* InterruptConnect(PKSERVICE_ROUTINE routine, PVOID ctx, PKSPIN_LOCK lock, ULONG vector, KIRQL irql,
                               KIRQL sync_irql) {
    auto* r = new InterruptRec{};
    r->routine = routine;
    r->ctx = ctx;
    r->own_lock = 0;
    r->lock = lock ? lock : &r->own_lock;
    r->vector = vector;
    r->irql = irql;
    r->sync_irql = sync_irql ? sync_irql : irql;
    r->connected = true;
    AcquireSRWLockExclusive(&g_int_lock);
    g_interrupts.push_back(r);
    ReleaseSRWLockExclusive(&g_int_lock);
    TRACE(driver, "interrupt vector %lu connected (irql %u routine %p)", vector, irql, reinterpret_cast<void*>(routine));
    return r;
}

void InterruptDisconnect(InterruptRec* r) {
    if (!r) return;
    AcquireSRWLockExclusive(&g_int_lock);
    g_interrupts.erase(std::remove(g_interrupts.begin(), g_interrupts.end(), r), g_interrupts.end());
    r->connected = false;
    ReleaseSRWLockExclusive(&g_int_lock);
    // The record stays allocated until the worker drained anything in flight.
    if (WorkerOnWorkerThread()) {
        WorkerPost([r] { delete r; });
    } else {
        WorkerFlushDpcs();
        delete r;
    }
}

bool InterruptInject(ULONG vector) {
    std::vector<InterruptRec*> targets;
    AcquireSRWLockShared(&g_int_lock);
    for (InterruptRec* r : g_interrupts)
        if (r->vector == vector && r->connected) targets.push_back(r);
    ReleaseSRWLockShared(&g_int_lock);
    if (targets.empty()) return false;
    for (InterruptRec* r : targets) {
        WorkerPost([r] {
            KIRQL old = SpinAcquireRaise(r->lock, r->sync_irql);
            if (r->connected) r->routine(reinterpret_cast<PVOID>(r), r->ctx);
            SpinReleaseLower(r->lock, old);
        });
    }
    return true;
}

BOOLEAN InterruptSynchronize(InterruptRec* r, PKSYNCHRONIZE_ROUTINE routine, PVOID ctx) {
    KIRQL old = SpinAcquireRaise(r->lock, r->sync_irql);
    BOOLEAN res = routine(ctx);
    SpinReleaseLower(r->lock, old);
    return res;
}

}  // namespace win10xbox::drivers::detail
