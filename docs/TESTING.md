# Testing

| Suite | Where | Runs on |
|-------|-------|---------|
| `unit.*` | `tests/unit` | linux-cross (Wine) + windows |
| `integration.*` | `tests/integration` | windows (real EXEs through host-win32) |
| Wine conformance subset (`wine_conformance`) | `tests/wine-tests` | linux-cross (Wine), non-blocking; windows: skips (see below) |
| `drivers.*`, `integration.driver_*` | `tests/drivers` | linux-cross (Wine) + windows (see below) |
| kmtest subset | `tests/kmtest` | windows (M4, not yet started: `tests/drivers/kmtest.h` is a minimal in-driver harness) |

## GUI guests (core/user)

`integration.guest_window` (tests/guest/window.c: frameless popup, FillRect, GetPixel, timers) and
`integration.guest_window2` (tests/guest/window2.c: captioned window, memory-DC double buffering, Set/GetWindowText,
InvalidateRect, DestroyWindow, PeekMessage on MsgWaitForMultipleObjectsEx) run through host-win32 with the headless
display. Each checks its own pixels with GetPixel and exits 42; host-win32 writes the last painted frame of the
virtual screen to `WIN10XBOX_SCREENSHOT=<file.bmp>` and `run_guest.cmake` checks pixels of it given as
`WIN10XBOX_EXPECT_SCREENSHOT_PIXELS="x,y,aarrggbb|..."` (0xAARRGGBB as read from the BMP's B,G,R,A bytes). The
`user_*` cases of `unit.core` cover the GDI handle encoding, the stock objects' slots, HWND encoding, regions and
DIB reading. Trace channels: `WIN10XBOX_DEBUG=+user,+gdi` (with `+relay` for every win32u call). `guest.window` /
`guest.window2` (the same programs run natively) are registered only where there is a real win32k (not under the
cross-compiling emulator).

## Wine test subset (M3 fidelity baseline)

Wine's own conformance tests (`wine-pe/wine-src/dlls/<dll>/tests/*.c`, LGPL,
never copied into this repository) are compiled with llvm-mingw as guest PEs and
run through `win10xbox.exe` on the Wine PE ntdll booted by the layer. Wine's
test harness prints `<test>: N tests executed (T marked as todo, F as flaky, K
failures), S skipped.`; the runner parses it. Which tests and why:
`tests/wine-tests/SUBSET.md`. Most of these tests fail or crash on the
incomplete layer; the numbers below are the honest starting point, not a goal
of "all green".

Measured on Linux under Wine 9.0 (x86_64), Debug build, llvm-mingw 21 + Wine
11.9 sources, `WINETEST_PLATFORM` auto-detected (so the guest reports itself as
"wine" and `todo_wine` blocks expect Wine's known failures). Columns:
**executed** = assertions run, **todo** = assertions inside `todo_wine` that
failed as expected, **failures** = Wine's "K failures" (real failures plus
todo blocks that unexpectedly succeeded), **outcome**: `ok` = the test function
returned and printed its summary; `crashed` = no summary line (see below);
`timeout` = still running at the 60 s per-test cap. **exit** is the guest exit
code (Wine's tests exit with `min(failures, 255)`, so 255 means "255 or more").

| Test binary | outcome | executed | todo | failures | skipped | exit | where it stopped (last line printed) |
|---|---|---:|---:|---:|---:|---:|---|
| ntdll/rtlstr | ok | 2178627 | 3 | **0** | 0 | 0 | |
| ntdll/string | ok | 1974183 | 0 | **0** | 0 | 0 | |
| ntdll/rtlbitmap | ok | 378 | 0 | **0** | 0 | 0 | |
| ntdll/error | ok | 1448 | 0 | **0** | 0 | 0 | |
| ntdll/env | ok | 600 | 0 | **0** | 0 | 0 | |
| ntdll/path | ok | 989 | 0 | 21 | 0 | 21 | |
| ntdll/directory | ok | 6336 | 5 | 556 | 1 | 255 | |
| ntdll/file | crashed | - | - | - | - | 0 | file.c:413 (`C:\windows\SYSTEM32\cmd.exe` not found, STATUS_OBJECT_NAME_NOT_FOUND) |
| ntdll/om | crashed | - | - | - | - | 5 | om.c:2942 (`\Sessions\1\BaseNamedObjects` missing) |
| ntdll/reg | timeout | - | - | - | - | 124 | reg.c:1987 loops forever on `NtEnumerateKey` = STATUS_INVALID_PARAMETER (millions of failures) |
| ntdll/info | crashed | - | - | - | - | 5 | info.c:2372 (system handle information) |
| ntdll/time | crashed | - | - | - | - | 5 | time.c:129 (expects an access violation from a bad pointer) |
| kernel32/atom | ok | 1049715 | 0 | **0** | 0 | 0 | |
| kernel32/codepage | ok | 394761 | 0 | **0** | 0 | 0 | |
| kernel32/environ | ok | 428 | 0 | 10 | 3 | 10 | |
| kernel32/path | ok | 202588 | 10 | 2 | 1 | 2 | |
| kernel32/directory | ok | 104 | 0 | **0** | 0 | 0 | |
| kernel32/file | timeout | - | - | - | - | 124 | file.c:2354 (hung after 96 failures) |
| kernel32/heap | crashed | - | - | - | - | 5 | heap.c:1084 |
| kernel32/time | ok | 2249 | 0 | 38 | 0 | 38 | |
| kernel32/locale | ok | 2306560 | 17 | 297 | 0 | 255 | |
| kernel32/version | ok | 156 | 0 | 54 | 2 | 54 | |
| kernel32/virtual | crashed | - | - | - | - | 0 | virtual.c:1620 (after about 14 s) |
| kernel32/sync | crashed | - | - | - | - | 0 | (no failure line before it stopped; last syscalls are `NtQueueApcThread`) |
| kernel32/module | crashed | - | - | - | - | 0 | module.c:1793 |
| advapi32/registry | crashed | - | - | - | - | 5 | registry.c:4085 |

Totals: **25 binaries; 14 complete** (8 with zero failures: ntdll rtlstr,
string, rtlbitmap, error, env and kernel32 atom, codepage, directory; 6 with
failures), **9 crash**, **2 time out**.

How to read the crashes: `exit 5` = the host printed `win10xbox: could not
start ...: status c0000005`, i.e. an unhandled STATUS_ACCESS_VIOLATION escaped
the guest (the guest's Wine exception dispatch does not catch it yet). `exit 0`
with no summary = the guest's main thread ended without `ExitProcess`
(`WIN10XBOX_DEBUG=+process` logs `guest main thread ended without
ExitProcess`), also an abnormal end of the test. Both are `crashed` in the
table; the layer, not the test, is at fault in these cases. The earlier
unfiltered sweep of the same binaries also covered tests outside the subset;
for orientation: ntdll/large_int 612/0, ntdll/change 82/0, kernel32/format_msg
1552/0 are clean, kernel32/mailslot 126/15, kernel32/volume 458/35,
kernel32/drive 129/2, ntdll/unwind 455166/127 (executed/failures), and
ntdll/{atom,sync,thread,exception,virtual,rtl}, kernel32/{timer,thread,fiber,loader}
and advapi32/security crash like the above.

### What the ctest entries check

Each test is a ctest entry `wine_conformance.<dll>.<test>` with the label
`wine_conformance`. It passes while the recorded baseline holds
(`tests/wine-tests/baseline.cmake`):

* `run N`: must complete with `failures <= N` (a lower number prints
  `IMPROVED`; lower the baseline).
* `crash` / `timeout`: must still not complete (xfail). If it starts completing,
  it passes with an `IMPROVED` note: move it to `run <failures>`.
* A regression (a `run` test crashing or exceeding its failure budget) fails.
* Exit code 77 = SKIP (`SKIP_RETURN_CODE`) when the Wine builtins
  (`<machine>/Windows/builtin/ntdll.dll`) or the NLS data (`c_1252.nls`) are
  absent, or the guests were not built (no `wine-pe/wine-src`, or MSVC). A bare
  checkout and the Windows job therefore skip cleanly.

Each run also writes `<build>/wine-tests-results/<dll>_<test>.{tsv,log}`; the
`wine_conformance.report` test (and `cmake --build <build> --target
wine_tests_report`) prints the table above from the last run.

### Reproduce

Needs: llvm-mingw (`/opt/llvm-mingw`), Wine (to run the host), the Wine sources
(`wine-pe/fetch-wine.sh`, gitignored), the Wine PE builtins in
`wine-pe/out/builtin` (`wine-pe/build.sh`, or the CI artifact), and the NLS data
staged as for the guest e2e tests. `advapi32/registry` additionally needs the
`.idl`-generated headers in `wine-pe/build/include` (made by `wine-pe/build.sh`);
without them that one test is not built and SKIPs.

```sh
cmake -S . -B build-wt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake
cmake --build build-wt
export WINEDEBUG=-all WINEPREFIX=$HOME/.wine-wt
wineboot --init && wineserver -w
sys32=build-wt/bin/machine/Windows/System32; sort=build-wt/bin/machine/Windows/Globalization/Sorting
mkdir -p "$sys32" "$sort"
cp -f "$WINEPREFIX"/drive_c/windows/system32/*.nls "$sys32"/
cp -f "$sys32"/sortdefault.nls "$sort"/ 2>/dev/null || true
ctest --test-dir build-wt -L wine_conformance --output-on-failure      # about 3 minutes
ctest --test-dir build-wt -L wine_conformance -R wine_conformance.ntdll.om -V   # one test
cmake --build build-wt --target wine_tests_report                      # the table
```

CI (non-blocking, like `guest_e2e`): run the same `ctest -L wine_conformance`
as a separate step with `continue-on-error: true`, and exclude the label from
the blocking run (`ctest -LE wine_conformance`). The step needs
`wine-pe/fetch-wine.sh` (and the wine-pe artifact must also provide the
`include/` headers for `advapi32/registry`, or that test skips). Per-test cap:
`-DWIN10XBOX_WINE_TEST_TIMEOUT=<seconds>` (default 60).

### Expanding the subset

Add `win10xbox_wine_test(<dll> <test> run 9999)` to
`tests/wine-tests/baseline.cmake`, build, run that test with `-V`, read the
`WINE_RESULT` line and set the baseline to what it measured (`run <failures>`,
`crash 0` or `timeout 0`), then add the row above. Details and the selection
rationale are in `tests/wine-tests/SUBSET.md`. When M3/M4 work lowers a number,
lower the baseline in the same commit so the gain is locked in.

## Driver host tests (M4)

`tests/drivers` builds three WDM/KMDF test drivers (`OurTestDevice.sys`, `OurKmdfDevice.sys`, `OurStubProbe.sys`,
`OurFailDriver.sys`) as freestanding native-subsystem PEs, a guest client for each, and:

| ctest | label | what |
|-------|-------|------|
| `drivers.unit` | `drivers` | loads the real drivers into the host and drives them through the dispatcher entry points (27 cases) |
| `drivers.layout` | `drivers` | `ntk.h` / WDF shim structure layouts vs the mingw-w64 ddk and KMDF 1.33 headers (mingw only) |
| `integration.driver_ioctl` | `driver_e2e` | `drv_host.exe --driver OurTestDevice.sys C:\guest\drv_client.exe`: `CreateFile("\\.\OurTestDevice")` + `DeviceIoControl` through the Wine ntdll, the dispatcher and the IRP engine |
| `integration.driver_kmdf_ioctl` | `driver_e2e` | the same for the KMDF driver on the WDF shim |

The `driver_e2e` tests need the same machine folder as the guest tests (Wine builtins + NLS data) and SKIP (exit 77)
without it; `drivers.unit` needs nothing. Run: `ctest --test-dir <build> -L driver --output-on-failure`.

