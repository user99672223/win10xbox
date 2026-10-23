// Wine's TEB-frame exceptions (__TRY/__EXCEPT/__FINALLY in the PE builtins) on every host.
//
// Wine's PE DLLs are built without compiler SEH: wine/exception.h pushes an EXCEPTION_REGISTRATION_RECORD on
// TEB.NtTib.ExceptionList for each __TRY, and Wine's own ntdll walks that chain from call_seh_handlers (the "hack:
// call wine handlers registered in the tib list" loop) and from RtlUnwindEx. The kernel delivers every exception to the
// HOST ntdll (rtl_forward.inc), and Windows' x64 dispatcher knows table-based unwind info only: it never looks at
// ExceptionList. A __TRY in a guest builtin was therefore invisible on a Windows host, on every thread: sechost's
// device-notify thread (cfgmgr32, pulled in by the native dxgi/d3d11) raises RPC_S_SERVER_UNAVAILABLE in rpcrt4 inside
// its __TRY, the exception skipped it and ended in the thread-start handler's NtTerminateProcess(1722). A Wine host
// walks the chain itself, which is why linux-cross never showed it.
//
// Two pieces make the host do what Wine's ntdll does:
//  * TebFrameVeh, a vectored handler (last position, after the loader's trap handler) that unwinds from the exception
//    context with the host's RtlVirtualUnwind exactly like call_seh_handlers does and, when the first live TEB frame
//    belongs to a function with no table-based language handler below it, calls the frames' handlers (Wine's
//    __wine_exception_handler and friends) with the original exception record and context. A table-based handler
//    that sits below the first TEB frame keeps its place: the VEH steps aside and the host dispatcher offers the
//    exception to it first.
//  * w10x_RtlUnwind, the guest ntdll's RtlUnwind: Wine's handler ends in RtlUnwind(frame, trampoline, record, ...)
//    with `frame` a TEB frame, an address that is no unwind establisher and that the host's RtlUnwind rejects. For a
//    target on the TEB chain it runs the inner frames' unwind handlers (__FINALLY), pops them, and has the host's
//    RtlUnwindEx unwind the table-based frames up to the function that owns the target frame; every other target goes
//    to the host's RtlUnwind unchanged.
//
// Both run on the faulting guest thread, possibly under the guest's TLS/FLS view: nothing here may touch the CRT, a
// thread_local, a function-local static or TRACE. State is plain globals set up before the guest runs.
#include "internal.h"
#include <atomic>

namespace win10xbox::dispatch::detail {
namespace {

struct TebFrame { TebFrame* prev; void* handler; };
using TebHandler = EXCEPTION_DISPOSITION(NTAPI*)(EXCEPTION_RECORD*, TebFrame*, CONTEXT*, void*);
using HostRtlUnwind = void(NTAPI*)(PVOID, PVOID, PEXCEPTION_RECORD, PVOID);

PVOID g_veh = nullptr;
HostRtlUnwind g_host_unwind = nullptr;
DWORD64 g_host_ntdll = 0;

std::atomic<unsigned> g_seen{0};        // exceptions that found a live TEB frame
std::atomic<unsigned> g_handled{0};     // a TEB handler continued execution
std::atomic<unsigned> g_stepped{0};     // stepped aside for a table-based handler
std::atomic<unsigned> g_unwinds{0};     // RtlUnwind calls whose target is a TEB frame
std::atomic<unsigned> g_stale{0};       // dead frames dropped from the chain

NT_TIB* Tib() { return reinterpret_cast<NT_TIB*>(NtCurrentTeb()); }
TebFrame* Head() { return reinterpret_cast<TebFrame*>(Tib()->ExceptionList); }
void SetHead(TebFrame* f) { Tib()->ExceptionList = reinterpret_cast<decltype(Tib()->ExceptionList)>(f); }

DWORD64 StackBase() { return reinterpret_cast<DWORD64>(Tib()->StackBase); }
DWORD64 StackAllocBase() {   // TEB.DeallocationStack (x64 offset 0x1478): the lowest address of the thread's stack
    return *reinterpret_cast<const DWORD64*>(reinterpret_cast<const std::uint8_t*>(NtCurrentTeb()) + 0x1478);
}

// A chain element that lies above `sp` inside the thread's stack: the function that pushed it has not returned.
bool IsLive(const TebFrame* f, DWORD64 sp) {
    const auto a = reinterpret_cast<DWORD64>(f);
    return a >= sp && a + sizeof(TebFrame) <= StackBase() && (a & 7) == 0;
}

struct Step { bool ok; PEXCEPTION_ROUTINE handler; DWORD64 establisher; bool in_host_ntdll; };

// One RtlVirtualUnwind step (the same call Wine's call_seh_handlers makes) on the host's tables.
Step UnwindOne(CONTEXT& c) {
    Step s{};
    DWORD64 base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
    if (fn) {
        PVOID handler_data = nullptr;
        s.handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, c.Rip, fn, &c, &handler_data, &s.establisher, nullptr);
        s.in_host_ntdll = base == g_host_ntdll;
    } else {   // a leaf without unwind info: the return address is at the top of the stack
        if (c.Rsp >= StackBase()) return s;
        s.establisher = c.Rsp;
        c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
        c.Rsp += 8;
    }
    s.ok = c.Rip != 0 && c.Rsp < StackBase();
    return s;
}

// A thread that is inside TebFrameVeh's handler calls: a fault in a filter must not run the same frame again.
struct Busy { std::atomic<DWORD> tid; DWORD64 sp; };
Busy g_busy[64];

Busy* FindBusy(DWORD tid) {
    for (auto& b : g_busy)
        if (b.tid.load(std::memory_order_acquire) == tid) return &b;
    return nullptr;
}
bool EnterBusy(DWORD tid, DWORD64 sp) {
    for (auto& b : g_busy) {
        DWORD expected = 0;
        if (b.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) { b.sp = sp; return true; }
    }
    return false;
}
void LeaveBusy(DWORD tid) {
    if (Busy* b = FindBusy(tid)) b->tid.store(0, std::memory_order_release);
}

LONG CALLBACK TebFrameVeh(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    CONTEXT* ctx = ep->ContextRecord;
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return EXCEPTION_CONTINUE_SEARCH;
    TebFrame* frame = Head();
    if (!frame) return EXCEPTION_CONTINUE_SEARCH;
    // Frames below the exception's stack pointer belong to functions that already returned (a host unwind or a
    // longjmp skipped their pop; Wine's RtlRestoreContext removes them the same way).
    const DWORD64 alloc = StackAllocBase();
    bool dropped = false;
    while (frame && alloc && reinterpret_cast<DWORD64>(frame) >= alloc && reinterpret_cast<DWORD64>(frame) < ctx->Rsp &&
           (reinterpret_cast<DWORD64>(frame) & 7) == 0) {
        frame = frame->prev;
        dropped = true;
        g_stale.fetch_add(1, std::memory_order_relaxed);
    }
    if (dropped) SetHead(frame);
    if (!frame || !IsLive(frame, ctx->Rsp)) return EXCEPTION_CONTINUE_SEARCH;
    g_seen.fetch_add(1, std::memory_order_relaxed);

    const DWORD tid = GetCurrentThreadId();
    if (Busy* b = FindBusy(tid)) {
        if (ctx->Rsp < b->sp) return EXCEPTION_CONTINUE_SEARCH;   // raised by a handler of ours: nested
        b->tid.store(0, std::memory_order_release);               // left over from an unwind that never returned
    }

    // Walk the table-based frames from the exception context up to the function that owns the first TEB frame.
    CONTEXT c = *ctx;
    c.ContextFlags &= ~0x40u;   // CONTEXT_XSTATE: the copy has no extended area
    bool owns = false;
    for (int depth = 0; depth < 1024 && !owns; ++depth) {
        const Step s = UnwindOne(c);
        if (!s.ok) return EXCEPTION_CONTINUE_SEARCH;
        owns = reinterpret_cast<DWORD64>(frame) < c.Rsp;
        if (s.handler && !s.in_host_ntdll) {   // a table-based handler at or below the first TEB frame goes first
            g_stepped.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    if (!owns) return EXCEPTION_CONTINUE_SEARCH;

    volatile char marker = 0;   // below the faulting context's stack pointer: a nested fault is deeper than this
    if (!EnterBusy(tid, reinterpret_cast<DWORD64>(&marker))) return EXCEPTION_CONTINUE_SEARCH;
    LONG result = EXCEPTION_CONTINUE_SEARCH;
    for (int n = 0; n < 4096 && frame && IsLive(frame, ctx->Rsp) && reinterpret_cast<DWORD64>(frame) < c.Rsp; ++n) {
        TebFrame* const current = frame;
        void* dispatcher = nullptr;
        const EXCEPTION_DISPOSITION r = reinterpret_cast<TebHandler>(current->handler)(rec, current, ctx, &dispatcher);
        if (r == ExceptionContinueExecution) {
            if (!(rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) {
                g_handled.fetch_add(1, std::memory_order_relaxed);
                result = EXCEPTION_CONTINUE_EXECUTION;
            }
            break;
        }
        if (r == ExceptionNestedException) rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
        else if (r != ExceptionContinueSearch) break;   // an invalid disposition: leave it to the host
        frame = current->prev;
    }
    LeaveBusy(tid);
    return result;
}

bool OnChain(const void* target) {
    volatile char probe = 0;
    const DWORD64 sp = reinterpret_cast<DWORD64>(&probe);
    int n = 0;
    for (const TebFrame* f = Head(); f && IsLive(f, sp) && n < 4096; f = f->prev, ++n)
        if (f == target) return true;
    return false;
}

}  // namespace

// The guest ntdll's RtlUnwind (WINE_OVERRIDE in rtl_forward.inc).
extern "C" void NTAPI w10x_RtlUnwind(PVOID target_frame, PVOID target_ip, PEXCEPTION_RECORD rec, PVOID retval) {
    if (!g_host_unwind) g_host_unwind = reinterpret_cast<HostRtlUnwind>(HostExport("RtlUnwind"));
    if (!g_veh || !target_frame || !OnChain(target_frame)) {
        g_host_unwind(target_frame, target_ip, rec, retval);
        return;
    }
    g_unwinds.fetch_add(1, std::memory_order_relaxed);
    LeaveBusy(GetCurrentThreadId());

    CONTEXT here;
    RtlCaptureContext(&here);
    EXCEPTION_RECORD local{};
    if (!rec) {
        local.ExceptionCode = 0x80000018;   // STATUS_UNWIND
        rec = &local;
    }
    rec->ExceptionFlags |= EXCEPTION_UNWINDING;

    // The TEB frames below the target: their unwind handlers (Wine's __FINALLY) run, then they are gone.
    for (TebFrame* f = Head(); f && f != target_frame;) {
        void* dispatcher = nullptr;
        reinterpret_cast<TebHandler>(f->handler)(rec, f, &here, &dispatcher);
        f = f->prev;
        SetHead(f);
    }

    // The table-based frames between here and the function that owns the target frame: the host unwinds them
    // (destructors, __finally) and resumes at target_ip with that function's registers.
    CONTEXT c = here;
    DWORD64 owner = 0;
    for (int depth = 0; depth < 1024; ++depth) {
        const Step s = UnwindOne(c);
        if (!s.ok) break;
        owner = s.establisher;
        if (c.Rsp > reinterpret_cast<DWORD64>(target_frame)) break;
    }
    CONTEXT scratch;
    RtlUnwindEx(reinterpret_cast<PVOID>(owner), target_ip, rec, retval, &scratch, nullptr);
    // RtlUnwindEx does not return; if it did, the frame's setjmp point can no longer be reached.
    for (;;) RaiseException(0xC0000029 /* STATUS_INVALID_UNWIND_TARGET */, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void InstallTebFrameDispatch() {
    if (g_veh) return;
    g_host_ntdll = reinterpret_cast<DWORD64>(HostNtdll());
    g_host_unwind = reinterpret_cast<HostRtlUnwind>(HostExport("RtlUnwind"));
    wchar_t off[4];   // diagnostics: WIN10XBOX_TEB_FRAMES=0 leaves the frames to the host (a Wine host walks them itself)
    const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_TEB_FRAMES", off, 4);
    if (n == 1 && off[0] == L'0') { TRACE(dispatch, "TEB-frame dispatch disabled by WIN10XBOX_TEB_FRAMES=0"); return; }
    using AddVeh = PVOID(NTAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    auto add = reinterpret_cast<AddVeh>(HostExport("RtlAddVectoredExceptionHandler"));
    g_veh = add ? add(0, TebFrameVeh) : AddVectoredExceptionHandler(0, TebFrameVeh);
    if (!g_veh || !g_host_unwind) WARN(dispatch, "TEB-frame dispatch not installed (VEH %p, host RtlUnwind %p)", g_veh,
                                       reinterpret_cast<void*>(g_host_unwind));
    else TRACE(dispatch, "TEB-frame dispatch installed: Wine __TRY frames are dispatched by a vectored handler");
}

void LogTebFrameStats() {
    if (!g_veh) return;
    TRACE(dispatch, "TEB-frame dispatch: %u exceptions with a live frame, %u continued, %u stepped aside for a "
          "table-based handler, %u unwinds to a frame, %u dead frames dropped", g_seen.load(), g_handled.load(),
          g_stepped.load(), g_unwinds.load(), g_stale.load());
}

}  // namespace win10xbox::dispatch::detail
