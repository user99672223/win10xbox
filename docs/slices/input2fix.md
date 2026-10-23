# Slice: input2fix (integration.guest_window_input2 intermittent failure)

Status: fixed in core/clock; ci.yml green on all three jobs (a678e9d).

## Root cause

1. `Clk_NtDelayExecution` resolved the host `NtDelayExecution` lazily (function-local static) on the first guest call.
   Under the Wine host, `GetProcAddress` on the latched host ntdll returns NULL once the PEB loader list is the guest's
   (it works inside `clock::Initialize`, before the take-over), so the lookup failed and every guest `Sleep()` returned
   at once with STATUS_INSUFFICIENT_RESOURCES (+relay shows `NtDelayExecution ... ret=c000009a`).
2. `window_input2.c` waits for its second thread with `while (!ready && 5 s) Sleep(10)`. With no real sleep that is a hot
   spin, so on a loaded runner (heavy trace output, 2 cores) the new thread needed more than 5 s to create its window.
3. The test then went on and `SendInput`ed the move at (950,550) before the window existed; `RouteMouse` correctly dropped
   it ("no window of ours under the cursor", seen in the CI trace). The later click found the window, so only the first
   check failed: "another thread's window: delivered on that thread".
4. Cross-thread routing itself (owning thread of the window, `wake`, per-thread queues) is correct; nothing was misrouted.
5. Fix: `Initialize()` resolves `NtDelayExecution` / `NtSetTimerResolution` with the host PEB in place and the clock
   calls those pointers (core/clock/clock.cpp). `guest_sleep` now measures that 4 x Sleep(25) takes at least 90 ms, which
   fails deterministically on the old code (3 ms) and passes now.

## Local repeat counts (`ctest -R guest_window_input2 --repeat until-fail:N -j1`, `yes` load on a shared 4-core box)

- Before: not reproducible locally (30 + 15 + 12 passes; 36 with six parallel loops); the CI failure needs a thread start
  slower than 5 s. Deterministic proxy: `integration.guest_sleep` FAILED ("took 3 ms").
- After: 30/30 passed under six `yes` loops; `guest_sleep` passes (took ~100 ms); full ctest 63/63.

## CI

Run: https://github.com/user99672223/win10xbox/actions/runs/39881138998 (linux-cross, windows Debug, windows RelWithDebInfo: success).
