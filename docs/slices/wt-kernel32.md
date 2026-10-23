status: ready

kernel32 conformance slice: Wine 11.9's dlls/kernel32/tests (fetched on the linux-cross job into wine-pe/wine-src, never copied into
the repository) built with llvm-mingw into one PE, `kernel32_test.exe`, and run unit by unit through `win10xbox.exe`.

- wine-pe/tests/kernel32.sh: `build` / `run` / `all`, modelled on wine-pe/tests/advapi32.sh.
- wine-pe/tests/kernel32.baseline: the measured gate. `run <max>` = the unit completes with at most that many failures,
  `crash` / `timeout` = it must not complete; any improvement prints IMPROVED and the file is lowered by hand. The units with `run 0` are
  the allowlist and only grow. A unit counts as completed only when its own summary line is printed and the exit code equals the
  failure count (a unit that re-runs itself as a child prints the same summary name).
- tests/winetest/CMakeLists.txt: `integration.winetest_kernel32`, label `wine_conformance`, 30 s per unit; not registered with MSVC.
  Its results (kernel32.tsv, one log per unit) land in `build-mingw/bin/winetest-kernel32`, i.e. in the linux-cross-bin artifact.

Run: https://github.com/user99672223/win10xbox/actions/runs/39885424057 (ci.yml green on linux-cross, windows Debug and RelWithDebInfo
for the tree of this slice; the final push only rewrites the message trailers of earlier commits and tightens the completion check
below, tested on the artifact of that run). integration.winetest_kernel32 passed in about 265 s; `ctest gate: PASS (nothing skipped,
nothing missing)`. 4.54 million assertions executed over all units.

Syscalls added or changed: none. DLLs added to wine-pe/dlls.txt: none.

## Allowlist (completes with 0 failures, gated)

| unit | executed | note |
|---|---|---|
| atom | 1049715 | |
| codepage | 394761 | |
| comm | 8842 | 1 skipped |
| directory | 104 | |
| format_msg | 1552 | |
| path | 202588 | 10 todo, 1 skipped |
| power | 5 | |
| generated | 0 | completes, nothing to assert |

## Completes with failures (gated at the measured count)

| unit | executed | failures | first failing assertions |
|---|---|---|---|
| console | 4 | 1 | console.c:5920 Getting sb info |
| drive | 129 | 2 | drive.c:157 GetDiskFreeSpaceA error 3; drive.c:248 GetDiskFreeSpaceW error 3 |
| environ | 428 | 10 | environ.c:599 ExpandEnvironmentStrings `C:\Windows` vs `C:\windows`; environ.c:739 Expected ERROR_MORE_DATA, got 2 |
| file | 571274 | 191 (222 on another run, budget 222) | file.c:1403 no authority to access volumes; file.c:1931 expected failure; file.c:1932 got error 3735928559 |
| locale | 2306560 | 297 | locale.c:2308 (12, 14, 16 ...) got CSTR_LESS_THAN, expected CSTR_GREATER_THAN |
| mailslot | 126 | 15 | mailslot.c:97 wrong error 109; mailslot.c:266 dwMsgCount incorrect |
| profile | 578 | 4 | profile.c:95 / :101 ret 0x70 exp 0; profile.c:1217 got error 6 |
| resource | 559 | 7 | resource.c:468 / :471 / :474 resource not found |
| time | 2249 | 34 | time.c:262 / :294 / :421 GetTimeZoneInformation failed |
| version | 156 | 8 | version.c:759 NtQuerySystemInformation failed c0000002; :762 c0000004; :763 wrong len 16 |
| volume | 497 | 35 | volume.c:632 can't open c: drive 5; volume.c:715 expect ret 0x1, got 0; :716 expect err 0xdeadbeef, got 0x32 |

## Does not complete (gated: must not complete)

| unit | status | last line before the stop |
|---|---|---|
| actctx | crash | actctx.c:4575 detailed_info->ulAppDirPathType=5373952, expected 2 |
| change | crash | (no output) |
| debugger | timeout | debugger.c:706 waiting for the debugger (unhandled page fault, starting debugger) |
| fiber | crash | (no output) |
| heap | crash | heap.c:1084 Test marked todo: got count 4 |
| loader | crash | loader.c:1728 got 0x40 != expected 0x80, then a page fault at 0xF0 |
| module | crash | module.c:1834 got 00006FFFFFCBD4D0 |
| pipe | crash (timeout on an earlier run) | pipe.c:1424 test_NamedPipe_2 starting |
| process | crash (exit 0, no summary) | process.c:2494 CreateProcess error 2; :2498 CreateRemoteThread error 6; :2504 DuplicateHandle error 6 |
| sync | crash | sync.c:281 wrong error 3735928559 |
| thread | crash | thread.c:2377 Test marked todo: for info 16 expected STATUS_ACCESS_DENIED |
| timer | crash | (no output) |
| toolhelp | crash (timeout on an earlier run) | toolhelp.c:142 PID=408 win10xbox.exe |
| virtual | crash (timeout on an earlier run) | virtual.c:1599 / :1620 section attribute assertions, then status c0000005 |

Every "crash" ends with `win10xbox: could not start C:\guest-wine\kernel32_test.exe: status c0000005`, i.e. an access violation in
the guest that Wine's PE kernel32 does not handle yet (the unit's own summary is never printed).

What still fails: everything in the last two tables. The layer's gaps they point to: disk-free / volume queries on `C:\`, time zone
information, `NtQuerySystemInformation` classes, mailslot message counts, section attribute queries, named pipes, process creation from a
test unit and the unhandled page faults above. Fixing them is out of scope for this slice; the baseline only moves up.
