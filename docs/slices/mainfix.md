status: wip (run URL below once watched)

# Slice mainfix: main red after the merge batch of 2c579b5..78a0e47

Run that was red: https://github.com/user99672223/win10xbox/actions/runs/39971364492 (ci.yml on 78a0e47).

## Cause (a): windows jobs, integration.winetest_ntdll (parity gate on `file`)
* The merge of the registry-persistence slice left two `set(_cmd ...)` lines in tests/winetest/run_ntdll.cmake; the second
  (without `${_platform_arg}`) overrode the first, so no parity unit ran with `--platform=windows`.
* Wine's harness then took the guest for Wine: 101 failures instead of 26, among them the seven `broken()`-dependent
  "expected STATUS_PENDING" lines (file.c:635, 4301, 4331, 4357, 5415, 5628, 5672) and file.c:6716 that the native run passes.
* Fix: the duplicate line is removed; no allowlist or parity entry changed. Nothing in core/ was at fault.

## Cause (b): linux-cross, integration.winetest_kernel32 REGRESSION console, drive
* Both logs held one line, `wine client error:0: recvmsg: Connection reset by peer`, exit 1 after 0 s: Wine itself lost its
  wineserver (the server of the previous 3 s unit shutting down as the next one connects) before the guest started.
* Locally under Wine both units complete (console 1 failure, drive 2), so the layer is not involved; 2947159 and 654dabd
  only remove a file and a duplicate line.
* Fix: wine-pe/tests/kernel32.sh starts a unit again (at most 3 attempts, after `wineserver -w`) only when its whole log is
  that wine line; any output of the unit itself is never retried. Baseline lowered for the IMPROVED change, fiber, file.

Run: pending.
