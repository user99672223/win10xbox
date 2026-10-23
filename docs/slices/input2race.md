status: ready

# Slice: input2race (integration.guest_window_input2 intermittent failure, second round)

## Root cause

The failures are the one fixed by input2fix (a678e9d), seen on heads that do not contain that fix. Checked with
`git merge-base --is-ancestor a678e9d <head>`: 6d6449d, ec42f12 and 0bfecc3 (the heads of the four failed CI
attempts) are all "NO-fix". The commit order, not a second bug, made the failure look like it survived the fix.

Evidence in the retrieved linux-cross logs (run 39881262411 attempt 1, run 39881821163 attempt 1), +relay trace:

- every guest `Sleep` returned at once: `0114:trace:relay: NtDelayExecution(00000000,00007fffff04fc90) ret=c000009a
  (0.2us)`, 3499 times in the first log and 4939 times in the second, no call with another status;
- the first of them is made by the main thread right after `CreateThread(thread3)`, i.e. the `while (!ready &&
  5 s) Sleep(10)` wait of the test was a hot spin that wrote one trace line per iteration and competed with thread 3
  (the new thread had to register its class and create its window) on 2 cores;
- `0114:trace:user:RouteMouse mouse message 0200 at 950,550 dropped: no window of ours under the cursor` comes BEFORE
  `0118:trace:user:NtUserCreateWindowEx created 0000000000010026` (thread 3's window), so the 5 s bound of the wait ran
  out before thread 3 had finished and the test injected the move into an empty spot. `g_t3_ready` is only set after
  `CreateWindowEx` returns, so this order is only possible when the wait gave up. Routing was correct: the later click
  found the window ("PASS another thread's window: click").

Routing in core/user was read for an ordering bug and has none: `RouteMouse` takes the window and thread from
`WindowsFromPoint` under the module lock, `QueueInput` pushes on the owner's queue and `WakeThread` signals the
auto-reset `UserThread::wake` event, which stays signalled until the thread waits, so a message queued before the
owner is blocked in GetMessage is not lost; the owner thread's record exists before its first window.

## Why the post-fix test is still worth changing

The test still synchronised with thread 3 by polling with `Sleep(10)` under a 5 s bound plus a `Sleep(100)` fudge. Any
regression that makes Sleep cheap again (or a runner slow enough that thread 3 needs more than 5 s) turns this into
the same intermittent failure. The new wait does not use the clock at all.

## Fix (tests/guest/window_input2.c only)

- thread 3 publishes its window in `g_t3_hwnd` and does `SetEvent(g_t3_ready)` after `CreateWindowEx` returns;
- the main thread blocks in `MsgWaitForMultipleObjectsEx(1, &g_t3_ready, 60000, QS_ALLINPUT)` (pumping messages when
  they arrive, because creating a window on one thread may send messages to the other) -- no `Sleep`, no spin, the 60 s
  is only the bound for a hang;
- two new checks before the move: "thread 3 created its window" and "thread 3's window is visible, owned by thread 3
  and is the window at (950,550)" (`IsWindowVisible`, `GetWindowThreadProcessId`, `WindowFromPoint`), so a premature
  injection is reported as its cause instead of as a missing message;
- the `Sleep(100)` is gone; the delivery waits (`pump(30)` until the log shows the message) are bounded by 10 s, not 3 s.

## Local evidence (`taskset -c 0` or `0,1`, `yes` CPU hogs on the same cores, `WIN10XBOX_DEBUG=+user,+relay`, the
## ctest command line of the test run by hand with the host script `WIN10XBOX_TEST_INJECT_INPUT`)

Locally the fixed tree never failed the old test either. To get a "before", the input2fix fix was undone in the
working tree only (`g_host_delay = nullptr` in `clock::Initialize`, the pre-fix behaviour), not committed:

- old test, input2fix fix undone, 2 cores + 2 hogs: 30/30 passed (1 to 2686 failed Sleeps per run);
- old test, input2fix fix undone, 2 cores + 8 hogs: 20/20 passed;
- old test, input2fix fix undone, 1 core + 10 hogs: 7/8 passed, run 1 FAILED with exactly the CI signature: `FAIL
  another thread's window: delivered on that thread`, `RouteMouse mouse message 0200 at 950,550 dropped` at trace
  line 4595, thread 3's `NtUserCreateWindowEx created 0000000000010026` at line 4628, 380 x `NtDelayExecution ...
  ret=c000009a` before it;
- new test, input2fix fix still undone, 1 core + 10 hogs: 16/16 passed (no NtDelayExecution call at all);
- old test, current main (fixed clock), 2 cores + 2 hogs: 30/30 passed.
- new test, current main, 1 core + 10 hogs: 16/16 passed (`badsleep` 0, as no Sleep is called). ctest -R guest_window_input2 and guest_sleep pass.

## CI

Run: https://github.com/user99672223/win10xbox/actions/runs/39933337736 (head f3b7eeb): linux-cross, windows (Debug),
windows (RelWithDebInfo) all success; "ctest gate: 73 registered, 73 ran and passed, 0 DISABLED";
`Test #23: integration.guest_window_input2 ... Passed 6.37 sec`. The linux-cross job needed two re-runs (not pushes) for
causes outside this slice: two `apt-get install` (Wine .deb download) stalls in "Install Wine + ninja" (3 attempts of 7
min each, twice), and once `integration.winetest_kernel32` at 313 s over the 300 s ctest timeout on a starved runner
(actctx/comm/debugger children crash or time out as recorded in wine-pe/tests/kernel32.baseline; unrelated to input).

## What runs through the layer

`guest_window_input2.exe` (build-mingw/bin/guest, run by `integration.guest_window_input2`): 45 checks now, including
`PASS thread 3 created its window`, `PASS thread 3's window is visible, owned by thread 3 and is the window at
(950,550)`, `window_input2: thread 3 log [3:MM(50,50;own-thread=1) ]`, `window_input2: 45 passed, 0 failed`.
Syscalls added or changed: none. DLLs added to wine-pe/dlls.txt: none.

## What still fails

Nothing known in this test. The residual risk is a Sleep regression, which `integration.guest_sleep` gates (took >= 90 ms).
`integration.winetest_kernel32` runs close to the 300 s ctest timeout on a slow runner (someone should look at it).
