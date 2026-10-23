status: ready

# wt-shell32: Wine dlls/shell32/tests through the layer

Script: wine-pe/tests/shell32.sh (copy of advapi32.sh; builds the 19 START_TEST units plus rsrc.rc into
shell32_test.exe with llvm-mingw, runs each unit with a 30 s cap). Baseline: wine-pe/tests/shell32.baseline.
Registered as integration.winetest_shell32 (TIMEOUT 900) in tests/winetest/CMakeLists.txt.

Runs: measurement ci.yml 39880509318 (red by design, placeholder baseline);
green with the measured baseline: https://github.com/user99672223/win10xbox/actions/runs/39883590488
(integration.winetest_shell32 Passed in 124.6 s). wine-pe.yml was not triggered: no wine-pe/ DLL change.

DLLs added to wine-pe/dlls.txt: none (Makefile.in IMPORTS = shell32 ole32 oleaut32 user32 gdi32 advapi32 shlwapi,
all already built). Syscalls added: none. The earlier commit's ci failure (run 39849670418) was
integration.guest_window_input2, not the builder; build_winetest.sh was unused and is deleted.

## Measured (WINETEST lines, run 39880509318)
    appbar         ok  executed=63   failures=36  (25 s)
    assoc          ok  executed=2    failures=2
    autocomplete   crashed
    brsfolder      ok  executed=2    failures=2  skipped=2
    ebrowser       ok  executed=2    failures=1
    generated      ok  executed=0    failures=0
    progman_dde    crashed (exit 5, c0000005)
    recyclebin     ok  executed=10   failures=6
    shelldispatch  crashed  last: shelldispatch.c:121: Test failed: Failed to c...
    shelllink      crashed  last: shelllink.c:819: Test failed: no IID_IShellLinkA (0x80040154)  (REGDB_E_CLASSNOTREG)
    shellole       crashed  last: shellole.c:914 (skip: need codepage 932)
    shellpath      ok  executed=612  failures=46 todo=2
    shfldr_special crashed  last: shfldr_special.c:120: Test failed: SHGetDesktopFolde...
    shlexec        timeout (30 s) last: shlexec.c:1627
    shlfileop      ok  executed=1366 failures=101 todo=33
    shlfolder      crashed
    shlview        crashed  last: shlview.c:627: Test failed: Got h...
    string         ok  executed=5    failures=0
    systray        ok  executed=10   failures=8
    total executed=2072

Units at 0 failures: string (5 assertions), generated (0 executed). Everything else is failing or crashing;
the common visible causes are missing COM class registrations (0x80040154) and no desktop/explorer.
The per-assertion first-failure lines are not in the CI log of a green run (ctest hides output); shell32.sh now
prints "first failure:" per unit, so any red run shows them.
