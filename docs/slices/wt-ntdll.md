# Slice wt-ntdll: Wine's ntdll conformance tests through the layer

Wine's own `dlls/ntdll/tests` (wine-pe/wine-src, never copied here) built as one guest, `ntdll_test.exe`, and run
unit by unit on the Windows jobs of ci.yml as `integration.winetest_ntdll` (ctest label `winetest_ntdll`).

## How it runs

* ci.yml (windows jobs, before Configure): llvm-mingw's Windows build, `wine-pe/fetch-wine.sh`, then
  `wine-pe/tests/ntdll.sh build --out build/winetest/ntdll` from Git Bash (`ntdll_test.exe` + `ntdll_test.units`).
  MSVC cannot build Wine's tests; the test is registered only where that guest exists, and the "Gate - nothing
  skipped" step requires the `winetest_ntdll` label, so a skip is a red job.
* `tests/winetest/run_ntdll.cmake` runs every unit as `win10xbox --machine bin/machine-builtin
  C:\guest-wine\ntdll_test.exe <unit>`. The builtin-only machine folder (the console's DLL set) is needed: on a Windows
  runner the shared one carries the runner's own advapi32/sechost/rpcrt4/ucrtbase and the guest dies on Wine's
  `0x80000100` import stubs before its first assertion.
* The gate is `wine-pe/tests/ntdll.allowlist` (`<unit> <floor>`): each listed unit must print Wine's summary line,
  with 0 failures and at least the floor of assertions. Grow it, never shrink it. Everything else is only measured
  (20 s cap each); the step "Wine ntdll conformance summary" prints the table and the list below for every run, and
  the logs are in the `windows-<config>-logs` artifact (`winetest/ntdll/results`).
* Locally (Linux, Wine as emulator): build the guest with `wine-pe/tests/ntdll.sh build --out
  <build>/winetest/ntdll`, reconfigure, `ctest -R winetest_ntdll`. A Wine host and a Windows host differ in places
  (e.g. `directory`: 556 failures under Wine, 19 on Windows); the numbers below are the Windows runners'.

## Allowlist today

alpc, env, error, large_int, path, rtlbitmap, string. All 0 failures in both Debug and RelWithDebInfo.

* `path` was the unit fixed for this slice: 21 failures under Wine, 1 on the Windows runners. `TranslateObjectAttributes`
  (core/vfs) used to normalise `\??\C:\...` names lexically, so `C:\\\windows`, `.`/`..` components, `/` and a trailing
  `\` on a file opened; it now validates them as the object manager and NTFS do (STATUS_OBJECT_NAME_INVALID,
  `C:/` is no device: NAME_NOT_FOUND / PATH_NOT_FOUND). Names relative to a directory handle (`RootDirectory`) used to
  go to the host untouched, which bypassed the builtin DLL rule (`windows\system32\kernel32.dll` under a root of
  `\??\C:\`) and let the host answer `.\x` with its own status; core/dispatch/nt_file.cpp now turns them into the
  absolute name from the directory's path (`GetFinalPathNameByHandle`) and applies the same checks. Where the path of
  the handle is unknown (the console's AppContainer) the host still resolves it.
* `change` passes on the Windows runners (82/82) but is not listed: `change.c:241` waits 0 ms for a directory
  notification right after `RemoveDirectory`, and under a Wine host it fails one run in two. Candidate once it has
  run stably on the runners.
* `rtlstr` is not listed: 2178627 assertions, 1 failure on the runners:
  `rtlstr.c:761: RtlDowncaseUnicodeString works wrong: U+037F is converted to U+037F, expected U+03F3`. The case
  table is Windows' own (`l_intl.nls` from the runner, served by `NtGetNlsSectionPtr`); Windows leaves U+037F alone,
  the Wine test expects Wine's newer table. A data difference, not a layer fault; do not fake the mapping.
* `port` (1 assertion, a todo) and `generated` (no assertions) complete but prove nothing.

## Todo: units that are not on the allowlist (Windows runners)

Measured by the run that introduced the slice (path shown as fixed by this branch).

| unit | gate | outcome | executed | failed | todo | skipped | exit | s |
|---|---|---|---:|---:|---:|---:|---:|---:|
| alpc | yes | ok | 1530 | 0 | 0 | 0 | 0 | 2 |
| atom | no | crashed | 0 | 0 | 0 | 0 | Access violation | 3 |
| change | no | ok | 82 | 0 | 0 | 0 | 0 | 3 |
| directory | no | ok | 8756 | 19 | 0 | 1 | 19 | 2 |
| env | yes | ok | 604 | 0 | 0 | 0 | 0 | 3 |
| error | yes | ok | 1448 | 0 | 0 | 0 | 0 | 3 |
| exception | no | crashed | 0 | 0 | 0 | 0 | -2147483646 | 2 |
| file | no | ok | 2854 | 281 | 39 | 2 | 255 | 4 |
| generated | no | ok | 0 | 0 | 0 | 0 | 0 | 3 |
| info | no | crashed | 0 | 0 | 0 | 0 | Access violation | 3 |
| large_int | yes | ok | 612 | 0 | 0 | 16 | 0 | 2 |
| om | no | crashed | 0 | 0 | 0 | 0 | Access violation | 3 |
| path | yes | ok | 989 | 0 | 0 | 0 | 0 | 2 |
| pipe | no | crashed | 0 | 0 | 0 | 0 | Access violation | 3 |
| port | no | ok | 1 | 0 | 1 | 0 | 0 | 4 |
| reg | no | timeout | 0 | 0 | 0 | 0 | Process terminated due to timeout | 35 |
| rtl | no | crashed | 0 | 0 | 0 | 0 | Access violation | 4 |
| rtlbitmap | yes | ok | 378 | 0 | 0 | 0 | 0 | 2 |
| rtlstr | no | ok | 2178627 | 1 | 3 | 0 | 1 | 3 |
| string | yes | ok | 1974183 | 0 | 0 | 0 | 0 | 3 |
| sync | no | crashed | 0 | 0 | 0 | 0 | Access violation | 3 |
| thread | no | crashed | 0 | 0 | 0 | 0 | Access violation | 3 |
| threadpool | no | crashed | 0 | 0 | 0 | 0 | Access violation | 4 |
| time | no | crashed | 0 | 0 | 0 | 0 | Access violation | 3 |
| unwind | no | crashed | 0 | 0 | 0 | 0 | Access violation | 3 |
| virtual | no | crashed | 0 | 0 | 0 | 0 | Access violation | 3 |
| wow64 | no | ok | 1063 | 137 | 0 | 1 | 137 | 3 |


### atom: crashed, 0 executed, 0 failed, 0 distinct failing lines in the first 300 kB

### directory: ok, 8756 executed, 19 failed, 5 distinct failing lines in the first 300 kB
- `directory.c:620: Test failed: mask L"<": .a, got 0.`
- `directory.c:654: Test failed: ShortName is empty`
- `directory.c:837: Test failed: wrong status c000000f`
- `directory.c:867: Test failed: wrong status c000000d`
- `directory.c:1449: Test failed: got 0`

### exception: crashed, 0 executed, 0 failed, 0 distinct failing lines in the first 300 kB

### file: ok, 2854 executed, 281 failed, 54 distinct failing lines in the first 300 kB
- `file.c:5415: Test failed: expected STATUS_PENDING, got 0.`
- `file.c:5628: Test failed: expected STATUS_PENDING, got 0.`
- `file.c:5672: Test failed: Unexpected result 0x1, GetLastError() 3735928559.`
- `file.c:4931: Test failed: 2: expected 0 got 0xc0000035`
- `file.c:4935: Test failed: 4: expected 0 got 0x1`
- `file.c:400: Test failed: open L"\\??\\C:\\windows\\SYSTEM32\\cmd.exe" failed c0000034`

### info: crashed, 0 executed, 0 failed, 10 distinct failing lines in the first 300 kB
- `info.c:515: Test failed: Expected STATUS_SUCCESS, got c0000004`
- `info.c:516: Test failed: ReturnLength should be 0, it is (48)`
- `info.c:520: Test failed: Expected STATUS_SUCCESS, got c0000004`
- `info.c:521: Test failed: ReturnLength should be 24, it is (48)`
- `info.c:531: Test failed: Expected STATUS_INFO_LENGTH_MISMATCH, got 00000000`
- `info.c:687: Test failed: extended 1: i 8 (L"LsaIso.exe"): Got unexpected TebBase 0000000000000000, expected 00007FF5CEE60000.`
- stopped after: `info.c:2372: Test succeeded inside todo block: Expected some handles, got 0`
- host: `win10xbox: could not start C:\guest-wine\ntdll_test.exe: status c0000005`

### om: crashed, 0 executed, 0 failed, 2 distinct failing lines in the first 300 kB
- `om.c:2941: Test failed: got c0000002`
- `om.c:2942: Test failed: NtQueryObject failed c0000008`
- stopped after: `om.c:2942: Test failed: got (null), expected L"\\Sessions\\2\\BaseNamedObjects\\wine_test"`
- host: `win10xbox: could not start C:\guest-wine\ntdll_test.exe: status c0000005`

### pipe: crashed, 0 executed, 0 failed, 15 distinct failing lines in the first 300 kB
- `pipe.c:403: Test failed: can't open pipe, GetLastError: e7`
- `pipe.c:1074: Test failed: CreateNamedPipe failed`
- `pipe.c:1080: Test failed: CreateFile failed (2)`
- `pipe.c:1085: Test failed: Change mode`
- `pipe.c:1514: Test failed: status = c0000024`
- `pipe.c:1518: Test failed: FileIoCompletionNotificationInformation not supported`
- stopped after: `pipe.c:1757: Test failed: pipe is not signaled`

### reg: timeout, 0 executed, 0 failed, 134 distinct failing lines in the first 300 kB
- `reg.c:390: Test failed: Expected STATUS_ACCESS_VIOLATION, got: 0xc000000d`
- `reg.c:399: Test failed: Expected STATUS_ACCESS_VIOLATION, got: 0xc000000d`
- `reg.c:405: Test failed: Expected STATUS_ACCESS_VIOLATION or STATUS_ACCESS_DENIED, got: 0xc000000d`
- `reg.c:411: Test failed: Expected STATUS_INVALID_PARAMETER, got: 0x00000000`
- `reg.c:422: Test failed: NtCreateKey failed: 0x00000000`
- `reg.c:427: Test failed: NtCreateKey failed: 0x00000000`
- stopped after: `reg.c:1987: Test failed:`

### rtl: crashed, 0 executed, 0 failed, 7 distinct failing lines in the first 300 kB
- `rtl.c:237: Test failed: RtlQueryProcessDebugInformation returned c0000005`
- `rtl.c:240: Test failed: RtlQueryProcessDebugInformation returned c0000005`
- `rtl.c:241: Test failed: unexpected InfoClassMask 0`
- `rtl.c:242: Test failed: unexpected HeapInformation 0000000000000000`
- `rtl.c:252: Test failed: RtlQueryProcessDebugInformation returned c0000005`
- `rtl.c:253: Test failed: unexpected InfoClassMask 0`
- stopped after: `rtl.c:2758: Running test 23 (compressed_size=5, uncompressed_size=0, status=0xc0000242)`

### sync: crashed, 0 executed, 0 failed, 0 distinct failing lines in the first 300 kB

### thread: crashed, 0 executed, 0 failed, 0 distinct failing lines in the first 300 kB

### threadpool: crashed, 0 executed, 0 failed, 0 distinct failing lines in the first 300 kB
- stopped after: `threadpool.c:652: Test marked todo: TpSimpleTryPost unexpectedly returned status 0`
- host: `win10xbox: could not start C:\guest-wine\ntdll_test.exe: status c0000005`

### time: crashed, 0 executed, 0 failed, 1 distinct failing lines in the first 300 kB
- `time.c:129: Test failed: expected STATUS_ACCESS_VIOLATION, got 00000000`
- stopped after: `time.c:129: Test failed: expected STATUS_ACCESS_VIOLATION, got 00000000`
- host: `win10xbox: could not start C:\guest-wine\ntdll_test.exe: status c0000005`

### unwind: crashed, 0 executed, 0 failed, 0 distinct failing lines in the first 300 kB
- stopped after: `unwind.c:2993: Line has been silenced after 42 occurrences`
- host: `win10xbox: could not start C:\guest-wine\ntdll_test.exe: status c0000005`

### virtual: crashed, 0 executed, 0 failed, 3 distinct failing lines in the first 300 kB
- `virtual.c:165: Test failed: NtAllocateVirtualMemory with 20 zero_bits returned c0000018`
- `virtual.c:267: Test failed: wrong size 10000`
- `virtual.c:1187: Test failed: unexpected RegionSize 4000, expected 1000`
- stopped after: `virtual.c:1277: Test marked todo: unexpected stack committed size c000, expected 6000`
- host: `win10xbox: could not start C:\guest-wine\ntdll_test.exe: status c0000005`

### wow64: ok, 1063 executed, 137 failed, 9 distinct failing lines in the first 300 kB
- `wow64.c:318: Test failed: current: wrong win32 entry 00007FF7C8FBF510 / 00000001400013A0`
- `wow64.c:1333: Test failed: 4: wrong status c0000002`
- `wow64.c:1423: Test failed: NtMapViewOfSection returned 40000003`
- `wow64.c:1447: Test failed: NtMapViewOfSection returned 40000003`
- `wow64.c:1453: Test failed: NtMapViewOfSection returned 40000003`
- `wow64.c:1492: Test failed: Failed to open version.dll`

### Notes on the crashed units

* time: `time.c:129` `NtQueryPerformanceCounter(NULL, &frequency)` must answer STATUS_ACCESS_VIOLATION, and an
  unreadable output pointer (`0xdeadbee0`) too; the dispatcher has no user-pointer probing, so the host faults and the
  guest dies (`could not start ...: status c0000005`). A shared "probe the guest pointer, else STATUS_ACCESS_VIOLATION"
  helper for the Emulate handlers would clear this and most of the `Access violation` exits below.
* atom, sync, thread, unwind, virtual, rtl, info, om, pipe, threadpool: host `Access violation` after the assertion
  lines above (nothing printed before it for atom, sync, thread). exception exits with 0x80000002 (datatype
  misalignment) before its first assertion.
* reg: loops for ever on `NtEnumerateKey` (`reg.c:1987`), killed by the 20 s cap.
