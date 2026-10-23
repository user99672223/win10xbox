# Testing

| Suite | Where | Runs on (every push, every branch; all gating) |
|-------|-------|---------|
| `unit.*`, `guest.*` | `tests/unit`, `tests/guest` | linux-cross (llvm-mingw + Wine) + windows (MSVC Debug and RelWithDebInfo) |
| `integration.*` (label `guest_e2e`: real guest PEs through `win10xbox.exe`) | `tests/integration`, `tests/host_exit` | linux-cross + windows |
| Wine conformance subset (`wine_conformance`) | `tests/wine-tests` | linux-cross, gated on the recorded baseline; windows: not registered (MSVC cannot build Wine's tests) |
| `drivers.unit`, `drivers.layout` (label `drivers`), `integration.driver_*` (label `driver_e2e`) | `tests/drivers` | linux-cross + windows (KMDF driver and layout check against the WDK on MSVC) |
| every bundled guest through the host, exit code checked (`tools/ci/smoke_guests.ps1`) | `ci.yml` windows, `host-win32-smoke.yml` | windows (desktop machine folder) + windows (console conditions) |
| real programs (Lua 5.4.7: `integration.lua_*`, smoke rows) | `tools/ci/build_lua.ps1`, `tests/guest/lua` | windows only (MSVC build; see "Real programs") |
| kmtest subset | `tests/kmtest` | windows (M4, not yet started: `tests/drivers/kmtest.h` is a minimal in-driver harness) |

## What gates where (CI)

Every workflow except `wine-pe.yml` (which only rebuilds Wine, on `wine-pe/**` changes and weekly) runs on every
push to every branch; nothing is `continue-on-error` except the diagnostics steps that only print.

* **`ci.yml` linux-cross** (llvm-mingw, guests under Wine): `wine-pe/fetch-wine.sh` (shallow clone of the pinned Wine
  tag, a few seconds), the Wine PE builtins **and** the widl-generated headers (`wine-pe-include`, for
  `advapi32/registry`), then the full `ctest` with nothing excluded (57 registered with `integration.guest_spawn`: 16 `guest_e2e`, 2
  `driver_e2e`, 2 `drivers`, 28 `wine_conformance`, ...; about 4 minutes of tests).
* **`ci.yml` windows** (MSVC, Debug and RelWithDebInfo, machine folder with the runner's System32 DLLs and NLS
  data): the full `ctest` (the guest's stdout is captured on real Windows; see `docs/STATUS.md`), then every bundled
  guest through `win10xbox.exe` / `drv_host.exe` with its exit code. The WDK is not on the image: its km headers come
  from the pinned NuGet package `Microsoft.Windows.WDK.x64` (`tools/ci/fetch_wdk_km.ps1`, SHA-256 checked).
* **`host-win32-smoke.yml`**: the KERNEL32-import proof (unchanged), then every bundled guest under console
  conditions (Wine builtins only, `WIN10XBOX_TEST_NO_HOST_PATHS=1`).
* **`host-win32-appcontainer.yml`**, **`host-uwp.yml`**, **`uwp-lite.yml`**: their own gates, unchanged.

The Windows jobs' log artifacts (`windows-<config>-logs`, `host-win32-smoke-logs`, `host-win32-appcontainer-logs`)
carry `build/bin/crash.txt`, `crash.dmp` and `entry-reached.txt` (written by `host-win32/crash.cpp` when a guest dies)
and `build/bin/*.bmp` (the GUI tests' screenshots), besides the ctest logs and the JUnit file.

Two mechanisms close the "100% tests passed, everything skipped" trap (`docs/HANDOFF.md`, trap 1):

1. **The Wine PE builtins are mandatory** (`tools/ci/fetch_wine_pe.py`, used by every job that runs a guest except
   host-uwp's own step): a `wine-pe` run of the same commit is waited for (so a change to `wine-pe/patches` is tested
   with the DLLs it produces); otherwise the newest successful run that still holds the artifacts; every GitHub query
   is retried; no builtins = red job.
2. **The ctest gate** (`tools/ci/check_ctest_run.py`, step "Gate - nothing skipped" after every `ctest`): compares
   `ctest --show-only=json-v1` (what is registered) with `ctest --output-junit` (what ran). Any skipped, not-run or
   failed test fails the job, and so does a label with fewer tests than its floor (`guest_e2e` >= 1, `driver_e2e` >= 2,
   `drivers` >= 2, and on linux-cross `wine_conformance` >= 1). Counts come from ctest, never from a list of names,
   so new tests are picked up automatically. A test registered `DISABLED` (none today) is reported as "not run on
   purpose", is not a skip and does not count towards a floor.

The guests the smoke step runs, with their exit codes, are the table in `tools/ci/smoke_guests.ps1`
(`guest_hello` 42, `guest_cpuid` 0, `guest_sleep` 7, `guest_smoke_cui` / `guest_smoke` 42, `drv_client` and
`drv_kmdf_client` through `drv_host` 0); a guest in the machine folder that is not in the table is listed and stays
gated by its ctest entry. `guest_dll.dll` is a DLL, exercised by `unit.core` (`test_loader.cpp`).

The exclusion: `wine_conformance` (`tests/wine-tests`) does not run on the Windows jobs. Its guests are Wine's own test
sources, which build only with clang against Wine's headers; with MSVC no test is registered. The one Wine suite that
does run there is ntdll's, as `integration.winetest_ntdll` (label `winetest_ntdll`, required by the gate above): a ci.yml
step installs llvm-mingw's Windows build, fetches the Wine sources and builds all of `dlls/ntdll/tests` as one guest
(`wine-pe/tests/ntdll.sh build`); `tests/winetest/run_ntdll.cmake` then runs every unit through `win10xbox.exe`. The
gate is `wine-pe/tests/ntdll.allowlist`: each listed unit must complete with 0 failures and at least its floor of
assertions (grow it, never shrink it). Units not on it are measured, never gating; their outcome and first failing
assertions are the "Wine ntdll conformance summary" step of each run and `docs/slices/wt-ntdll.md`.

## Real programs

Programs that are not ours, built from their upstream sources in a named step of the Windows jobs and run through
`win10xbox.exe` like every other guest. Registered only where that step exists (MSVC builds, or
`-DWIN10XBOX_REAL_PROGRAMS=ON` with the program already in `<machine>/guest`), so linux-cross never counts them
towards its skip floor.

| Program | Build | ctest (label `guest_e2e`) | Smoke row (`tools/ci/smoke_guests.ps1 -RealPrograms`) | Also under console conditions |
|---|---|---|---|---|
| Lua 5.4.7 (`lua.exe`) | `tools/ci/build_lua.ps1`: tarball pinned by SHA-256 `9fbf5e28...1e30` and verified on every run (cached with `actions/cache`, key carries the hash); `cl /O2 /MT`, every `.c` except `luac.c`; output `build/bin/machine/guest/lua.exe` | `integration.lua_hello` (`-e "print('hello from lua', 6*7) os.exit(42)"`, exit 42), `integration.lua_smoke` (`tests/guest/lua/smoke.lua` with arguments `alpha "beta gamma"`: argv, string.format, sort, io.write, os.time/date/clock, USERPROFILE, file write/read/rename/remove under the guest home, tmpname, io.lines, dofile, require, pcall/error, 1 MB `string.rep`, exit 42; every section line is gated), `integration.lua_unwind_tables` (no `RtlAddFunctionTable failed` in the `+loader` trace; guards the Wine host, a no-op on Windows) | `lua_hello`, `lua_smoke` | `host-win32-smoke.yml` |
| curl 8.11.1 (`curl.exe`) | `tools/ci/build_curl.ps1`: release tarball pinned by SHA-256 `a889ac9d...5e80`, verified on every run (cached with `actions/cache`); CMake + MSVC, libcurl and the CRT static, no TLS and no optional dependency; output `build/bin/machine/guest/curl.exe` | `integration.real_curl_version` (`--version`: banner, `Protocols:` has http and file, not https), `integration.real_curl_file` (`file:///C:/Users/User/curl_in.txt` to stdout), `integration.real_curl_file_copy` (`-o` of a 320000-byte file, `-w size=%{size_download}`, output file must exist), `integration.real_curl_file_missing` (exit 37). No network: nothing here opens a socket. The input files are written into `<machine>/Users/User` at configure time | none | none |
| CPython 3.12.10 embeddable (`python.exe` + `python312.dll`, `.pyd`s, `python312.zip` standard library, as shipped) | `tools/ci/fetch_python.ps1`: `python-3.12.10-embed-amd64.zip` pinned by SHA-256 `4acbed6d...a3c3`, verified on every run (cached), unpacked to `build/bin/machine/guest/python` | `integration.real_python_platform` (`-c "import sys, platform, os; print(sys.version); print(platform.uname()); print(os.cpu_count())"`: a `3.12.10 (tags/...` line, a `uname_result(system='Windows'` line, a number as the last line, exit 0), `integration.real_python_json` (`-m json.tool` on `tests/guest/python/sample.json`: re-indented document on stdout, exit 0); both on the builtin-only machine folder (Wine's `bcrypt`, `cryptbase`, `ws2_32`, `version`, ... , no runner DLLs; see `docs/slices/realpython.md`) | - | - |

Local reproduction under Wine (the interpreter cross-built, no MSVC): compile the tarball's `src/*.c` except `luac.c`
with `x86_64-w64-mingw32-clang -O2 -static` into `build-mingw/bin/machine/guest/lua.exe`, configure with
`-DWIN10XBOX_REAL_PROGRAMS=ON`, and run `ctest -R integration.lua`. Under the system Wine 9 as host the first Lua
error (`pcall`, which is `longjmp` through `RtlUnwindEx`) ends the guest ("stack overflow" from the host ntdll; see
`docs/STATUS.md`, "Real programs"). It does not happen on real Windows (the gate there is green), so the argv/file/os
sections are what Wine can check.

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

The two GUI tests run on their own machine folder, `bin/machine-builtin` (prepared by `integration.prepare_machine`
next to `bin/machine`, from the same Wine builtins, guest programs and NLS data, with no native DLLs and
`"dll_overrides": {"*": "builtin,native"}`): the console's DLL set, which is what host-uwp stages. They cannot use the
shared folder on the Windows jobs, where `import_windows_dlls.ps1` fills `System32` with the runner's own build-26100
DLLs and `machine.default.json` prefers native: Wine's builtin user32/gdi32 then loaded native advapi32, sechost and
rpcrt4, which import ntdll exports Wine 11.9 does not have (`RtlCreateBoundaryDescriptor`,
`TpSetDefaultPoolStackInformation`, ...) and whose CRT apiset (`api-ms-win-core-crt-l2-1-0`) the host's schema
resolves to `kernelbase.dll`, always Wine's in the guest. Wine's loader binds such imports to stubs, and the first
call (`_initterm_e` in sechost's DllMain) raised 0x80000100 (`EXCEPTION_WINE_STUB`). The other guest tests keep the
shared folder.

## A real program: the SQLite shell

The first guest this repository did not write. `tools/ci/build_sqlite.ps1` downloads the official amalgamation
(`sqlite-amalgamation-3540100.zip`, SQLite 3.54.1, `https://www.sqlite.org/2026/`), checks it against the SHA-256 pinned
in the script **on every run** (a cache hit is verified too; a mismatch is a hard failure, never a re-download), and
builds `sqlite3.exe` with `cl /O2 /MT shell.c sqlite3.c` into `build/bin/machine/guest/`. The windows job of `ci.yml`
and `host-win32-smoke.yml` each have a named step ("Build the SQLite shell (pinned, SHA-256 verified)") with an
`actions/cache` entry keyed on the pinned hash. The pin: sqlite.org publishes the SHA3-256 of the archive; the file was
downloaded once, its SHA3-256 matched the published one, and its SHA-256 is what the script holds. To move to a newer
release change `$Version`, `$Archive`, `$Url`, `$Sha256` and the cache key together.

| ctest (label `guest_e2e`) | what |
|---|---|
| `integration.sqlite_memory` | `sqlite3.exe -batch :memory: "select 6*7;"` prints `42`, exit 0 |
| `integration.sqlite_script` | `sqlite3.exe -batch C:\Users\User\smoke.db` with `tests/guest/sqlite/smoke.sql` on stdin: create table, inserts (one journal per statement), ordered and aggregate selects, an index, `.dump`, `date('now')`, `pragma integrity_check`, `.exit 42`; every expected output line is gated and `Users/User/smoke.db` must exist in the machine folder afterwards |
| `integration.guest_args` | `guest_args.exe one "two words" three`: argv, `GetCommandLineW()` and NUL-termination of the PEB's strings (regression for the first defect SQLite found) |

`sqlite_*` are registered on MSVC builds only (where the step above produces `sqlite3.exe`); a missing `sqlite3.exe` is a
failing test, not a skip. `-DWIN10XBOX_SQLITE_TESTS=ON` registers them elsewhere for a developer who staged a
`sqlite3.exe` by hand (e.g. cross-built with `x86_64-w64-mingw32-clang -O2 -static shell.c sqlite3.c`). `smoke_guests.ps1`
runs the same two command lines (plus `guest_args`) through `win10xbox.exe` on both Windows workflows.
`run_guest.cmake` takes `-DPROGRAM_ARGS=a;b` (`@SEMI@` for a `;` inside an argument), `-DINPUT_FILE=` (stdin) and
`-DGUEST_FILE=` (a machine-folder path deleted before the run and required afterwards).

`integration.guest_lockfile` (LockFileEx/UnlockFileEx/LockFile on two handles of one file) and `integration.guest_args`
are not MSVC-only: they run on linux-cross too, where the Wine 9.0 host and Wine 11.9 guest disagree about
`NtUnlockFile`'s `IoStatusBlock` (core/dispatch/nt_file.cpp handles both). A scanner regression for jump tables inside
the code section is in `unit.core` (`loader_scan_leaves_jump_tables_in_code_alone`).

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
| ntdll/path | ok | 989 | 0 | **0** | 0 | 0 | |
| ntdll/directory | ok | 6336 | 5 | 556 | 1 | 255 | |
| ntdll/file | crashed | - | - | - | - | 0 | file.c:413 (`C:\windows\SYSTEM32\cmd.exe` not found, STATUS_OBJECT_NAME_NOT_FOUND) |
| ntdll/om | crashed | - | - | - | - | 5 | om.c:2942 (`\Sessions\1\BaseNamedObjects` missing) |
| ntdll/reg | ok | 1186 | 13 | 1 | 0 | 1 | the one failure is `reg.c:2548`, `win_skip` of `test_NtRegLoadKeyEx` (the guest's token holds no SeBackup/SeRestore privilege; Wine's tests count a `win_skip` as a failure, docs/slices/wt-ntdll-reg.md) |
| ntdll/info | crashed | - | - | - | - | 5 | info.c:2372 (system handle information) |
| ntdll/time | ok | 10657 | 2 | **0** | 0 | 0 | |
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

Totals: **25 binaries; 15 complete** (9 with zero failures: ntdll rtlstr,
string, rtlbitmap, error, env, time and kernel32 atom, codepage, directory; 6 with
failures), **8 crash**, **2 time out**.

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

CI: gated on ci.yml's linux-cross job, in the same `ctest` run as everything
else (no label excluded). The job runs `wine-pe/fetch-wine.sh` before
configuring and downloads `wine-pe-include` (the generated headers
`advapi32/registry` needs) with the builtins. Per-test cap:
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
| `drivers.layout` | `drivers` | `ntk.h` / WDF shim structure layouts vs Microsoft's kernel headers (mingw-w64 ddk with llvm-mingw, the WDK's km headers with MSVC) and KMDF 1.33 |
| `integration.driver_ioctl` | `driver_e2e` | `drv_host.exe --driver OurTestDevice.sys C:\guest\drv_client.exe`: `CreateFile("\\.\OurTestDevice")` + `DeviceIoControl` through the Wine ntdll, the dispatcher and the IRP engine |
| `integration.driver_kmdf_ioctl` | `driver_e2e` | the same for the KMDF driver on the WDF shim |

The `driver_e2e` tests need the same machine folder as the guest tests (Wine builtins + NLS data) and SKIP (exit 77)
without it; `drivers.unit` needs nothing. Run: `ctest --test-dir <build> -L driver --output-on-failure`.

All four build and gate with both toolchains. With MSVC the KMDF driver, its client and `layout_check` need the WDK's
km headers: CMake finds them next to the Windows SDK (`Windows Kits\10\Include\<ver>\km`) or takes
`-DWIN10XBOX_DDK_INCLUDE=<km dir>`; CI uses the NuGet package (`tools/ci/fetch_wdk_km.ps1`). Two facts only the WDK
showed: `KMUTANT.Abandoned` is a bit field of `MutantFlags` (`OB()` in `layout_list.inc`), and `DEVICE_OBJECT` is
`DECLSPEC_ALIGN(16)`, size 0x150 (mingw-w64 omits the alignment: 0x148; `SA()`).

`WIN10XBOX_TEST_FORCE_RDRAND_TRAP=1` (latched in `process::Launch`): the scanner rewrites every register-form `rdrand`/`rdseed` in guest images
to `ud2`, so the #UD emulation (D18: `BCryptGenRandom`, CF=1) runs on a CPU that has the instructions. Set by `integration.guest_rdrand` and
`integration.guest_identity`.

`WIN10XBOX_AUDIO_NULL_ENDPOINT=1` (latched in `process::Launch` before `PrimeAudio`; unset/0 = no change): the host-side mmdevapi unix library
(`core/dispatch/unix_mmdevapi.cpp`) does no host COM and exposes exactly one render endpoint, "win10xbox null output" (48 kHz stereo float, no capture
endpoint). Its render client consumes the guest's buffers at real-time pace (by stream sample rate) and counts the frames; each stream release traces
`audio: null endpoint consumed N frames` (channel `+audio`). Used by `integration.guest_xaudio2_play` (exit 42, N within 10 % of 48000, i.e. about 1 s of the
XAudio2 engine's output); `integration.guest_xaudio2_play_noendpoint` runs the same guest with the option unset and requires the host's own verdict (43 on
a runner without an endpoint, 42 with one).

`WIN10XBOX_TEST_PRESENT_LIMIT=N` (latched in `process::Launch`, N=0/unset = off): the DXGI interposition (`core/dispatch/dxgi_interpose.cpp`) counts successful
`Present`/`Present1` calls (not `DXGI_PRESENT_TEST`) over all swap chains; after the N-th, once that frame is handed to the compositor, it traces and prints
`present-limit: N frames presented, <w>x<h>, <adapter>` and ends the guest with exit code 42 (`process::GuestExit`, the `NtTerminateProcess` path), so a
`WIN10XBOX_SCREENSHOT` still shows the last composed frame. A generic gate for any unmodified D3D11 program; used by `integration.guest_d3d11_swap_limit`.
