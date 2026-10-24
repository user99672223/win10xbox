status: ready (merged into main by Session A; ci 40131935073, host-uwp 40140009189; process-level teardown on Windows hosts)

# swapgpu: integration.guest_d3d11_swap intermittent host access violation

## Hypothesis from the previous run (PEB.ActivationContextData restore) - REFUTED
`env WIN10XBOX_TEST_LEAK_GUEST_ACTCTX=1 ctest --test-dir build -R '^integration.guest_d3d11_swap$' --repeat until-fail:20`
still fails (iteration 2, 5 and 2 in three runs: "exit code Access violation, expected 42"), with the restore skipped
(the log shows the hook WARN, the host's saved value was non-NULL 0x17C40000000, not NULL).

Bisect: a temporary patch making the hook skip RestorePebFields completely (no PEB field written back, `saved=false`)
also fails (iteration 5). So no PEB write-back is the trigger; the planned fix (never write NULL back / keep the block
mapped) would not remove this crash and was not applied. The TEMP edits are reverted; the tree has no code change.

## What the crash looks like
- crash thread: a host thread-pool worker (TpSetWait... frames in ntdll) with NO layer log lines at all (never entered
  the guest, not in the guest list), fault at ntdll+0xfa7d `inc dword [rax+0x24]` with rax=0, called from
  RtlDeactivateActivationContextUnsafeFast+0x14e; rdi=0x17c44a1cff8 (a heap address 8 bytes below a page end).
- it happens after `guest exit, code 42` and RestorePebFields, i.e. while TerminateOtherGuestThreads has just
  NtTerminateThread'ed ~19 guest threads (arbitrary points, possibly inside host heap / ntdll code) and the host is
  still alive for the rest of its exit path.
- a run with the thread kills skipped (temp hook) exited 42 but failed on a screenshot pixel check instead, one sample:
  inconclusive.

## Next steps (not done)
1. Test the theory "threads killed mid host-heap/ntdll operation tear state a surviving host thread then trips over":
   freeze the non-guest, non-main host threads (non-Wine hosts only) before TerminateOtherGuestThreads, or end the
   process from the exiting guest thread right after the host has flushed, shrinking the window.
2. Get a real debugger trace (the crash thread's activation-context stack, frame at 0x17c44a1cff8).
3. Failure rate: about 1 in 3 before; no change from any experiment here (no fix applied).

Environment note: a bash shell lacks the VS environment; building works with
`env INCLUDE=... LIB=... PATH=...` using forward-slash paths (MSVC 14.44.35207, Windows Kit 10.0.26100.0).

## Fix (process-level teardown, DECISIONS E9)
The crash thread is a host thread-pool worker running a native dxcore wait callback while the guest exits (guest PEB
still installed; dbghelp's minidump fails with 0x800736b2 for the same reason). Killing guest threads one by one was
not the (only) cause: suspending them instead and parking the exiting thread still crashed (3 runs). Freezing every
thread created after the launch (guest threads and the pool workers) before the guest's DLL detach, then screenshot,
then TerminateProcess from the host's main thread: 30/30 passes of `ctest -R '^integration.guest_d3d11_swap$' --repeat until-fail:30`.
Other failures on this machine are the same with and without the change: ninja_build, guest_d3d11_swap_limit
(Present -> DXGI_STATUS_OCCLUDED), winetest_ntdll (timeout).
