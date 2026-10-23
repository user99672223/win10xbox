status: wip

# Slice: wt-misc (small Wine conformance suites)

Suites, in priority order: psapi, userenv, cabinet, xmllite, mpr, bcrypt, setupapi, dbghelp, wintrust, ncrypt, crypt32
(msxml3 below). All run through one script, `wine-pe/tests/misc.sh --dll <dll>` (modelled on advapi32.sh: exe
`<dll>_test.exe`, units from the suite's Makefile.in SOURCES, link libraries from its IMPORTS plus the standard set,
baseline `wine-pe/tests/<dll>.baseline`), registered as `integration.winetest_<dll>` in tests/winetest/CMakeLists.txt.

Earlier commit 14a674e (only dlls.txt): its ci run 39849543008 failed in "Install Wine + ninja" (timed out after
12 minutes; infrastructure, no test ran). Its wine-pe run 39849543037 was green and built every listed DLL
(cabinet psapi userenv xmllite propsys dbghelp msxml3 setupapi cfgmgr32 urlmon wininet iertutil mpr crypt32 bcrypt
ncrypt wintrust: all `built`).

DLLs added to wine-pe/dlls.txt: the 14a674e list (minus cfgmgr32 and mpr, already in MAIN), plus cryptnet, cryptui,
imagehlp (delay imports of crypt32 and wintrust). propsys belongs to -wt-media and is only listed.

Not registered: msxml3 (needs widl for xmlparser.idl -> xmlparser.h, which linux-cross does not have).
Not tested here: urlmon, wininet (network), iertutil (no tests).

Measurements: ci 39893860706 (head e0d2046) recorded; crypt32 unix library table added, setupapi, wintrust and crypt32 re-measured next.
