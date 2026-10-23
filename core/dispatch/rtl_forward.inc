// Guest ntdll (Wine) exports redirected at function level, see DECISIONS.md E2.
// Owned by core/dispatch. Applied by rtl_forward.cpp while the guest ntdll is
// still writable (before FinalizeProtections): a 5-byte `jmp rel32` at the
// export's entry, through a near trampoline when the target is out of range.
//
//   RTL_FORWARD(name)            -> the same-named host ntdll export. If the
//                                   host lacks it, WARN and leave Wine's own.
//   WINE_OVERRIDE(name, ourfn)   -> a core/dispatch function (unix_calls.cpp).
//   WINE_OVERRIDE_ORD(ordinal, what, ourfn)
//                                -> the same for an export that has no name (wine-pe patch 0006).
//
// Rule: anything that owns a process-wide structure which host code also
// touches gets ONE implementation, the host's. Everything else stays Wine.
#ifndef RTL_FORWARD
#define RTL_FORWARD(name)
#endif
#ifndef WINE_OVERRIDE
#define WINE_OVERRIDE(name, fn)
#endif
#ifndef WINE_OVERRIDE_ORD
#define WINE_OVERRIDE_ORD(ordinal, what, fn)
#endif

// ---- Heap ---------------------------------------------------------------------
// PEB->ProcessHeap is the host's (wine-pe patch 0001). Blocks cross the
// boundary in both directions (the host frees what RtlCreateProcessParameters
// or the guest allocated and vice versa), and heap handles from
// GetProcessHeap()/RtlGetProcessHeaps() must mean the same heap to both.
// RtlCreateHeap goes through core: Wine's kernelbase HeapCreate (msvcrt's private heap, git's malloc) passes
// flags 0 and no maximum and relies on Wine's RtlCreateHeap making that heap growable; the host's would make it a
// fixed 1 MB heap that runs dry (git: "deflateInit: out of memory").
WINE_OVERRIDE(RtlCreateHeap, w10x_RtlCreateHeap)
RTL_FORWARD(RtlDestroyHeap)
RTL_FORWARD(RtlAllocateHeap)
RTL_FORWARD(RtlFreeHeap)
RTL_FORWARD(RtlReAllocateHeap)
RTL_FORWARD(RtlSizeHeap)
RTL_FORWARD(RtlLockHeap)
RTL_FORWARD(RtlUnlockHeap)
RTL_FORWARD(RtlValidateHeap)
RTL_FORWARD(RtlWalkHeap)
RTL_FORWARD(RtlGetProcessHeaps)
RTL_FORWARD(RtlQueryHeapInformation)
RTL_FORWARD(RtlSetHeapInformation)
RTL_FORWARD(RtlCompactHeap)
RTL_FORWARD(RtlGetUserInfoHeap)
RTL_FORWARD(RtlSetUserValueHeap)
RTL_FORWARD(RtlSetUserFlagsHeap)

// ---- Critical sections ---------------------------------------------------------
// PEB->FastPebLock and the heap locks are entered by host and guest code; the
// host's implementation owns the wait mechanism (keyed event / LockSemaphore,
// RTL_CRITICAL_SECTION_DEBUG list). Wine's statically initialised sections
// use the Windows layout, so the host code works on them unchanged.
RTL_FORWARD(RtlInitializeCriticalSection)
RTL_FORWARD(RtlInitializeCriticalSectionEx)
RTL_FORWARD(RtlInitializeCriticalSectionAndSpinCount)
RTL_FORWARD(RtlDeleteCriticalSection)
RTL_FORWARD(RtlEnterCriticalSection)
RTL_FORWARD(RtlLeaveCriticalSection)
RTL_FORWARD(RtlTryEnterCriticalSection)
RTL_FORWARD(RtlIsCriticalSectionLocked)
RTL_FORWARD(RtlIsCriticalSectionLockedByThread)
RTL_FORWARD(RtlSetCriticalSectionSpinCount)

// ---- SRW locks, condition variables, run-once -----------------------------------
// The lock word encodings and wait-block chains differ between Wine (waits on
// addresses) and Windows (wait blocks on the waiter's stack); one owner keeps
// them consistent for objects shared through host APIs (e.g. loader/heap
// internals) and keeps the host's fairness and alerting semantics.
RTL_FORWARD(RtlInitializeSRWLock)
RTL_FORWARD(RtlAcquireSRWLockExclusive)
RTL_FORWARD(RtlAcquireSRWLockShared)
RTL_FORWARD(RtlReleaseSRWLockExclusive)
RTL_FORWARD(RtlReleaseSRWLockShared)
RTL_FORWARD(RtlTryAcquireSRWLockExclusive)
RTL_FORWARD(RtlTryAcquireSRWLockShared)
RTL_FORWARD(RtlInitializeConditionVariable)
RTL_FORWARD(RtlSleepConditionVariableCS)
RTL_FORWARD(RtlSleepConditionVariableSRW)
RTL_FORWARD(RtlWakeConditionVariable)
RTL_FORWARD(RtlWakeAllConditionVariable)
RTL_FORWARD(RtlRunOnceInitialize)
RTL_FORWARD(RtlRunOnceBeginInitialize)
RTL_FORWARD(RtlRunOnceComplete)
RTL_FORWARD(RtlRunOnceExecuteOnce)
// Keyed events (NtCreateKeyedEvent/NtWaitForKeyedEvent/...) are syscalls and
// pass through in syscall_policy.inc; nothing to forward.

// ---- Exceptions, unwinding, function tables, VEH ----------------------------------
// The kernel delivers every exception to the HOST KiUserExceptionDispatcher,
// so the dispatch path, the dynamic function tables (guest images are
// registered with the host by core/loader) and the VEH lists must be the
// host's. KiUserExceptionDispatcher itself is deliberately NOT forwarded: the
// kernel never calls the guest's copy.
RTL_FORWARD(RtlAddFunctionTable)
RTL_FORWARD(RtlDeleteFunctionTable)
RTL_FORWARD(RtlInstallFunctionTableCallback)
RTL_FORWARD(RtlLookupFunctionEntry)
// RtlUnwind: ours; targets that are Wine TEB frames are unwound here, every other target goes to the host's RtlUnwind
// (teb_frames.cpp).
WINE_OVERRIDE(RtlUnwind, w10x_RtlUnwind)
RTL_FORWARD(RtlUnwindEx)
RTL_FORWARD(RtlVirtualUnwind)
RTL_FORWARD(RtlCaptureContext)
RTL_FORWARD(RtlRestoreContext)
RTL_FORWARD(RtlRaiseException)
RTL_FORWARD(RtlRaiseStatus)
RTL_FORWARD(RtlAddVectoredExceptionHandler)
RTL_FORWARD(RtlRemoveVectoredExceptionHandler)
RTL_FORWARD(RtlAddVectoredContinueHandler)
RTL_FORWARD(RtlRemoveVectoredContinueHandler)
RTL_FORWARD(RtlCaptureStackBackTrace)

// ---- Not forwarded, on purpose ---------------------------------------------------
// RtlQueryPerformanceCounter / RtlQueryPerformanceFrequency: guest time comes
//   from core/clock through NtQueryPerformanceCounter (Emulate).
// RtlFlsAlloc/RtlFlsFree/RtlFlsGetValue/RtlFlsSetValue/RtlProcessFlsData:
//   guest and host keep separate FLS universes; core/asm swaps TEB.FlsData
//   (and TEB.ThreadLocalStoragePointer) between the guest and host views.
// KiUserExceptionDispatcher, KiUserApcDispatcher, KiUserCallbackDispatcher:
//   only reached through the host's copies.

// ---- Wine debug channels (Wine keeps their state past the end of PEB/TEB) ----------
// (The builtins are built without debug output, so nothing in them calls these; they stay patched for modules
// that were not built that way.)
WINE_OVERRIDE_ORD(wine_ordinal::kDbgGetChannelFlags, "dbg_get_channel_flags", w10x_wine_dbg_get_channel_flags)
WINE_OVERRIDE_ORD(wine_ordinal::kDbgHeader, "dbg_header", w10x_wine_dbg_header)
WINE_OVERRIDE_ORD(wine_ordinal::kDbgOutput, "dbg_output", w10x_wine_dbg_output)
WINE_OVERRIDE_ORD(wine_ordinal::kDbgStrdup, "dbg_strdup", w10x_wine_dbg_strdup)

#undef RTL_FORWARD
#undef WINE_OVERRIDE
#undef WINE_OVERRIDE_ORD
