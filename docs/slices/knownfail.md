status: ready. code head bde6642. ci https://github.com/user99672223/win10xbox/actions/runs/40019301534 (3 jobs green). host-uwp https://github.com/user99672223/win10xbox/actions/runs/40019344112 (msix, run, launcher green: pass=36 knownfail=2 fail=0 timeout=1).
# Slice `-knownfail`: third outcome kind in expected_exit.txt
1. Syntax `KNOWNFAIL=<code>:<label>` (several allowed) in tests/guest/expected_exit.txt; parsed by ParseExpectedTable (ExpectedRow.known_fail).
2. ReportVerdict gives `KNOWN-FAIL (exit code 2 = uwp-no-hwnd(-uwpswap))`; any other code stays FAIL; never PASS.
3. host.cpp: report header has `knownfail=N`, summary counts include KNOWN-FAIL; launcher.cpp draws it in its own colour (kKnown, amber) on result and summary screens.
4. CI "Assert on report.txt": KNOWN-FAIL allowed only where the row has KNOWNFAIL; prints knownfail count and program list (step summary + log).
5. Rows: guest_d3d11_swap.exe (2) and guest_imgui_d3d11.exe (-2147483392). When `-uwpswap` lands, remove the KNOWNFAIL tokens.
6. host_uwp.selftest covers the form (source table; packaged copy byte-identical). Docs: report2.md items 11, table header.
7. Unproven: the amber colour on a real display (only the logic ran); the step-summary markdown line was not viewed on the Actions page.
