status: ready

# wt-msvcrt

Run: https://github.com/user99672223/win10xbox/actions/runs/39881821163 (head ec42f12; windows Debug and
RelWithDebInfo green, linux-cross green on the re-run of the job: its first attempt failed only
`integration.guest_window_input2` "another thread's window: delivered on that thread", an unrelated test that ran
before this one and passed on the re-run, while integration.winetest_msvcrt passed in both). wine-pe, uwp-lite,
host-win32-smoke, host-win32-appcontainer and host-uwp are green for the same head (host-uwp 39881821138).

Wine 11.9 msvcrt conformance units (dlls/msvcrt/tests, fetched on the linux-cross job, never copied here) built with
llvm-mingw as one guest PE (`msvcrt_test.exe`, linked against an import library of Wine's own msvcrt.dll made from
msvcrt.spec, `-D_MSVCR_VER=0`) and run unit by unit through win10xbox.exe: `wine-pe/tests/msvcrt.sh`, ctest
`integration.winetest_msvcrt`, label `wine_conformance` (linux-cross only; MSVC registers nothing, so the skip gate
holds). Gated on `wine-pe/tests/msvcrt.baseline`: `run <max>`, `crash`, `timeout`; a regression, or an improvement
that is not recorded (IMPROVED is printed), shows in the job log. The deliverable is the gated baseline, not layer
fixes.

## Measured (linux-cross, run 39878660644, head 90be5b2)

| unit | status | executed | failures | first failing assertion / how it ended |
|---|---|---|---|---|
| cpp | run | 624 | 0 | |
| data | run | 14 | 0 | 1 skipped |
| dir | run | 367 | 0 | |
| heap | run | 635 | 0 | |
| printf | run | 1018 | 0 | |
| scanf | run | 151 | 0 | |
| signal | run | 15 | 0 | |
| string | run | 410075 | 0 | |
| time | run | 314 | 11 | `time.c:352: GetTimeZoneInformation failed`, then `time.c:374: mktime returned 1767340800, expected 1767312000` (the 8 h offset of the runner's zone vs the layer's) |
| environ | crash | - | - | `environ.c:84: Expected system to return 0, got -1` (`system("echo OK")` cannot run cmd), parent then ends with exit code 1 |
| file | crash | - | - | `file.c:1657: Couldn't write in child process on 3 (Bad file descriptor)`, `file.c:1779: _spawnvp returned 3, errno 2`; a spawned `msvcrt_test.exe` ends with `win10xbox: could not start ...: status c0000005`; parent exit code 5 |
| locale | crash | - | - | after `locale.c:791: Tests skipped: Skipping __p___mb_cur_max tests`: guest exit code 0xc0000005 |
| misc | crash | - | - | `wine: Call from ... to unimplemented function advapi32.dll.SystemFunction036, aborting` (rand_s, status 0x80000100) |

The first run's parser took the first summary line of the log; a unit that spawns the test exe again (environ, file)
prints the child's summary first, so environ and file looked like "ok" (2 executed / 0 failures, 3 executed /
3 failures). msvcrt.sh now takes the last summary and requires the exit code to equal its failure count (Wine's
harness exits with the failure count); otherwise the parent did not finish and the unit counts as `crash`.
Total executed over the completing units: 413218 assertions.

Whole suite wall time on linux-cross: 23 s (per-unit cap 45 s).

## What runs through the layer

`win10xbox.exe --machine <dir> C:\guest-wine\msvcrt_test.exe <unit>` once per unit (on linux-cross under Wine as the
host), output lines of the first run (run 39878660644):

    WINETEST msvcrt:cpp status=ok executed=624 todo=2 flaky=0 failures=0 skipped=0 exit=0 time=1s
    WINETEST msvcrt:string status=ok executed=410075 todo=4 flaky=0 failures=0 skipped=0 exit=0 time=1s
    WINETEST msvcrt:time status=ok executed=314 todo=0 flaky=0 failures=11 skipped=1 exit=11 time=1s
    WINETEST msvcrt total executed=413218

Allowlist (0 failures, only grows): cpp data dir heap printf scanf signal string. `time` completes with 11 failures.
`environ file locale misc` do not complete (`crash`). ctest only prints a passing test's output, so the lines above are
from the one run where the gate failed (all units at `run 999999`, which flagged environ/file as wrongly parsed, see
above); the committed baseline held in the green run. A unit that completes while recorded as `crash` prints
IMPROVED but does not fail: lower the baseline then.

Syscalls added or changed: none. DLLs added to wine-pe/dlls.txt: none.

## What still fails

time (11: timezone/mktime), environ (`system()` cannot start cmd; parent ends with exit 1), file (a spawned test exe
dies with 0xc0000005; pipe/inherit tests), locale (0xc0000005 after the __p___mb_cur_max skip), misc (advapi32
SystemFunction036 is unimplemented in the machine's advapi32: rand_s aborts).
