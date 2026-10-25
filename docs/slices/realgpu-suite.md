status: in progress (ci pending)

# Slice `-realgpu-suite`: intermittent failures of the native Windows suite on a real GPU

Adapter (every row): **Intel(R) Graphics**, driver 32.0.101.8247 (Core Ultra 5 225U, 14 logical CPUs, 1920x1200),
Windows 11 26200. Base 3911033, RelWithDebInfo, `WIN10XBOX_DEBUG=+all` (as CI).

| test | failures (base 3911033) | first error line | guest's last stdout | layer's last trace | hypothesis | after |
|---|---|---|---|---|---|---|
| `integration.guest_d3d11_swap` | **28/55 (51 %)**: 3/5 full -j4, 10/20 subset, 15/30 alone | `CRASH: host unhandled exception code=0xc0000005 address=0x00007ffce4e0fa7d ... info0=0x1 info1=0x24` (`exit code Access violation, expected 42`); the same address and info in all 28 | `swap: presented 60 frames` / `swap: done` (the guest finished and called exit 42) | `trace:process:GuestExit guest exit, code 42` / `RestorePebFields PEB.ActivationContextData ... (the guest's) -> ... (the host's)` / `host PEB fields restored` | Exit teardown. GuestExit TerminateThreads the guest threads and writes the host's PEB fields back while Intel driver threads (not guest threads, so never stopped) still run in ntdll's activation-context and condition-variable code, and one writes through NULL+0x24. Diagnosed and fixed in `-swapgpu` (6cc83a2, 9d27338, DECISIONS E9): on a Windows host, guest exit is process exit (freeze, screenshot, TerminateProcess; no TerminateThread of guest threads, no PEB write-back). | **0/30** with `-swapgpu` merged (fix 1) |
| `integration.guest_gamelike_native` | **20/55 (36 %)**: 1/5, 8/20, 11/30 | `exit code Process terminated due to timeout, expected 42` (120 s) | `gamelike: frame 60, pad=none, audio=playing, present ok` / `present-limit: 90 frames presented, 640x360, Intel(R) Graphics` | `TerminateOtherGuestThreads terminating guest thread ...` (x22) / `RestorePebFields host PEB fields restored` / then only `trace:dispatch:NoteFirst native d3d: first D3DKMT call NtGdiDdDDIEvict` from a driver thread | Same teardown as the swap row, the other outcome. A guest thread killed by TerminateThread mid-call leaves a loader, heap or driver lock held; the main thread blocks on it after the PEB restore and never reaches the process exit. The audio and D3D worker threads in this guest make the hang more likely than the crash. | **0/30** (fix 1) |
| `integration.guest_regpersist_read` | **4/5 full -j4**, **30/30** stress (below); 0 serial | `regpersist: read: RegOpenKeyExW error 2` | `regpersist: read: RegOpenKeyExW error 2` / `regpersist: FAILED` | `trace:registry:OpenCommon open Software\win10xbox\persist (under \REGISTRY\USER\S-1-5-21-...-1001) failed: c0000034` / `GuestExit guest exit, code 1` | Every guest test on one machine folder shared `<machine>\registry-overlay.bin`. Under `ctest -j`, `run_guest.cmake` deleted it at the start of each other test, and the layer's exit-time save replaces it whole ("last writer wins"). Between the write run and the read run, another test on `machine-builtin` wiped the registry the write run left. Serial ctest runs the pair back to back, so CI never saw it. | **0/30** stress, 0/1 full -j4 (fix 2) |
| `integration.guest_regpersist_write` | **2/5 full -j4**, 0/30 stress | `regpersist: child: RegOpenKeyExW error 2` | `regpersist: write ok` / `regpersist: child: RegOpenKeyExW error 2` / `regpersist: child FAILED` | `OpenCommon open Software\win10xbox\persist ... failed: c0000034` (child) / `GuestExit guest exit, code 1` | The same shared file, a shorter window: the parent saves before CreateProcess, and the child host loads it a few hundred ms later. A test starting in that window deletes the file. | **0/30** stress (fix 2) |
| `integration.ninja_build` | **4/5 full -j4**: every pass after the first on the same build tree, 0 on a fresh machine folder | `stdout does not match '\[1/3\]'` | `ninja: Entering directory 'C:\guest'` / `[1/1] CONCAT ninja_out/ab.txt` | `RestorePebFields host PEB fields restored` / `host-win32: guest exit code 0 (0x00000000)` | Not a layer flake. `C:\guest\ninja_out` and `.ninja_log` from the previous pass survive in `build\bin\machine`, so ninja rebuilds one edge, not three. The comment at `tests/integration/CMakeLists.txt` ("a fresh machine folder: a rerun on the same folder finds ninja_out and builds less") already says so. Fails every local rerun and every `--repeat`; CI builds a fresh folder. Fix: a setup step that removes `ninja_out` and `.ninja_log` before `ninja_build`. Not done here (for Session A to assign). | not fixed |
| `integration.guest_xaudio2_native` | skipped 5/5 (not a failure on CI) | `SKIP: WIN10XBOX_CI_AUDIO_ENDPOINT is not 1: this test needs the device the CI workflow installs` | none (not started) | none | By design: the laptop lacks CI's virtual audio endpoint. Listed only because the CI gate (`check_ctest_run.py`) counts a skip as a failure, so a local gate run without the variable is red. | n/a |

No other test failed: 121 tests x 5 full passes at -j4, plus the 24 GUI/D3D tests
(`guest_window|d3d11|swap|imgui|gamelike|display_mode|cursor|dialog|menu`) x 20 serial passes, where 22 were 20/20 green.

## Fix 1: exit teardown (d3d11_swap, gamelike_native)

The root cause and fix are `-swapgpu`'s (ci green on 9d27338, run 40131935073; not yet merged into
`claude/youthful-cerf-l7xxsb` when this slice started). This slice merges `origin/claude/youthful-cerf-l7xxsb-swapgpu`
(5187d12; only conflict: docs/COORDINATION.md, both sides kept) and measures it on this GPU:

| test (30 serial passes, alone) | before (3911033) | after (merge) |
|---|---|---|
| `integration.guest_d3d11_swap` | 15/30 | 0/30 |
| `integration.guest_gamelike_native` | 11/30 | 0/30 |

## Fix 2: per-test registry overlay file (regpersist)

`registry::EnablePersistence(file)` loads and saves that file and locks `<file>.lock`, falling back to
`<machine>\registry-overlay.bin` as before. The dispatcher passes `WIN10XBOX_REGISTRY_OVERLAY`, and a child host
inherits it (a host-owned `WIN10XBOX_` variable, nt_spawn.cpp). `win10xbox_guest_test` passes `-DREGISTRY_OVERLAY=<machine>/registry-overlay.<test>.bin`
to `run_guest.cmake`, which deletes that file (unless `KEEP_REGISTRY_OVERLAY`) and sets the variable; the regpersist
pair shares `registry-overlay.regpersist.bin`. No gate changed: same expected lines, same exit codes, same DEPENDS.
Unit test `registry_overlay_persist_named_file`. docs/DISPATCHER.md "Registry persistence" point 1.

Stress set (30 passes, `-j 4`): `regpersist|guest_(identity|rdrand|tebframe|clipboard|gdi_bitmap|nowine|comctl)$|real_python_(json|platform)|tcc_(compile|run|hello_runs)$|psinfo|coreinfo`
(the pair plus 14 short tests on `machine-builtin`, and the `prepare_machine` fixture).

| test | before (3911033) | after |
|---|---|---|
| `integration.guest_regpersist_read` | 30/30 | 0/30 |
| `integration.guest_regpersist_write` | 0/30 (2/5 in full passes) | 0/30 |

After both fixes, one full pass at -j4: 119/121 passed; the failures were `ninja_build` (rerun state, above) and the
`xaudio2_native` skip.

## Method and caveats

* `ctest --repeat until-fail:N` stops a test at its first failure, which gives no rate. Instead each pass is a separate
  ctest run that keeps its junit and LastTest.log: 5 full passes `-j 4 --timeout 300`, then 20 serial subset passes.
  Script: `C:\win10xbox-dev\rg-scripts\passes.ps1` (outside the repo); logs in `C:\win10xbox-dev\rg-runs\<tag>\`.
* From about 01:35 local time, another agent's `win10xbox.exe` processes (`wt-programs` worktree) ran on the same laptop.
  The teardown "before" baseline and every "after" run overlap with that load.
* linux-cross keeps the old kill-and-restore teardown (9d27338: Wine hosts only); fix 2 converts the overlay path to
  `Z:` for Wine as `run_guest.cmake` does for `--machine`.
