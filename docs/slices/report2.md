status: ready. code head 19ae2d9. ci https://github.com/user99672223/win10xbox/actions/runs/40011679127 (3 jobs green). host-uwp https://github.com/user99672223/win10xbox/actions/runs/40011751136 (msix, run, launcher green: pass=36 fail=0 exited=0 timeout=1 crash=0, 37 programs).
# Slice `-report2`: expected-exit table for the all-programs report

1. `tests/guest/expected_exit.txt`: `<program> <code>[=<label>] [<code>=<label> ...]`, `#` comments, code = integer or `TIMEOUT`. 37 rows (every bundled program).
2. Every listed outcome is PASS and the verdict names the label: `PASS (exit code 3 = no-input)`; another code on a listed program is FAIL; no row = EXITED/CRASH.
3. Staged by `host-uwp/stage.cmake` as `payload/expected_exit.txt` (next to guests.txt); read by `RecordAllRun` (host.cpp) via `ParseExpectedTable`/`FindExpected`/`ReportVerdict` (launch_config.cpp). Code fallback: window.exe, hello_cui.exe, guest_hello.exe, guest_cpuid.exe, guest_xinput.exe.
4. `host_uwp.selftest` gets the source table (argv[2]) and the staged copy (argv[3], byte-identical when present).
5. CI "Assert on report.txt": every bundled program needs a row; PASS for each table program, TIMEOUT only where the row says so; prints the counts; `doom-timedemo.bmp` must have at least 8 distinct sampled pixels. Evidence now also has stdout.txt and trace.log (last program).
6. Doom root cause (inferred from the evidence, not traced): there was no crash. `CreateUpscaledTexture: Limited texture size to 0x0 (... max texture size 0x0)` is the software renderer's 0 = no limit and is printed on every run; the demo ran to its end (doom-timedemo.bmp of run 40009489508 is the last demo frame: status bar, health 0%). Chocolate Doom ends a timedemo through I_Error, exit(-1) (integration.doom_timedemo expects -1 too), and -1 has the NTSTATUS bits, so ReportVerdict called it CRASH. Table row: `doom-timedemo.launch -1=timedemo` (not 0: Doom never exits 0 here).
7. No layer change was needed; no window/display query under host-uwp returned 0 as far as the evidence shows.
8. Unproven: the stderr stats line (`timed N gametics ... fps`) was not read from stdout.txt; trace.log with +user,+gdi was not captured (not needed once the demo frame was seen).
9. Labels for 42=driven/clicked/child-ran/endpoint/pad are the console-side expectations, unseen on the runner.
10. Untouched: guest_d3d11_tri (`10=no-d3d11`, d3d11.dll error 126), separate slice.

## Format addition (`-knownfail`)
11. Third outcome kind `KNOWNFAIL=<code>:<label>` (several allowed), e.g. `guest_d3d11_swap.exe 42=presented KNOWNFAIL=2:uwp-no-hwnd(-uwpswap)`. Verdict `KNOWN-FAIL (exit code 2 = uwp-no-hwnd(-uwpswap))`, counted in the report header line `knownfail=` and the summary screen (own colour); any other code is FAIL. CI "Assert on report.txt" allows KNOWN-FAIL only where the table lists it and prints the knownfail count and program list in the step summary.
