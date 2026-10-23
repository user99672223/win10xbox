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

## Result

ci.yml green on all three jobs (windows Debug, windows RelWithDebInfo, linux-cross) for head a83a5b8:
https://github.com/user99672223/win10xbox/actions/runs/39888567531
(integration.winetest_shell32 Passed in 124.68 s on linux-cross; the artifact linux-cross-bin carries
build-mingw/winetest/shell32/*.log and shell32.tsv, which is where the lines below come from).

What runs through the layer: Wine's dlls/shell32/tests, 19 START_TEST units built as one PE
(shell32_test.exe, llvm-mingw) and run unit by unit through win10xbox.exe, label wine_conformance, gated on the
measured wine-pe/tests/shell32.baseline (a unit may not crash/time out where it ran, nor fail more assertions than
recorded). Total 2072 assertions executed.

Allowlist of units with 0 failures: string (5 executed), generated (0 executed, nothing to run).

| unit | status | executed | failures | first failing assertion / last line |
|---|---|---|---|---|
| appbar | ok | 63 | 36 | appbar.c:226: SHAppBarMessage returned 0 |
| assoc | ok | 2 | 2 | assoc.c:306: IQueryAssociations not supported, 0x80040154 |
| autocomplete | crashed (exit 0, no assertions) | 0 | 0 | no output; process ended with exit code 0 |
| brsfolder | ok | 2 | 2 | brsfolder.c:211: SHGetDesktopFolder failed with hr 0x8000ffff |
| ebrowser | ok | 2 | 1 | ebrowser.c:1769: No ExplorerBrowser control |
| generated | ok | 0 | 0 | |
| progman_dde | crashed | 0 | 0 | could not start shell32_test.exe: status c0000005 |
| recyclebin | ok | 10 | 6 | recyclebin.c:61: SHQueryRecycleBinA failed with error 0x80004005 |
| shelldispatch | crashed | 0 | 0 | shelldispatch.c:121: Failed to create ShellDispatch object: 0x80040154 |
| shelllink | crashed | 0 | 0 | shelllink.c:103: no IID_IShellLinkA (0x80040154) |
| shellole | crashed | 0 | 0 | shellole.c:914: skipped (codepage 932), then status c0000005 |
| shellpath | ok | 612 | 46 | shellpath.c:1389: got 0; shellpath.c:1457: SHGetFolderPathA(CSIDL_DESKTOP) failed 0x80070002 |
| shfldr_special | crashed | 0 | 0 | shfldr_special.c:50: SHGetDesktopFolder failed with error 0x8000ffff |
| shlexec | timeout (30 s) | 0 | 0 | shelllink.c:2881: no IID_IShellLinkA (0x80040154); stuck at shlexec.c:1627 |
| shlfileop | ok | 1366 | 101 | shlfileop.c:224: SHGetFileInfoA('' \| 0) should return 1, got 0x0 |
| shlfolder | crashed | 0 | 0 | could not start shell32_test.exe: status c0000005 |
| shlview | crashed | 0 | 0 | shlview.c:627: Got hr 0x8000ffff |
| string | ok | 5 | 0 | |
| systray | ok | 10 | 8 | systray.c:49: NIM_ADD failed! |

Common causes: COM classes not registered (0x80040154 REGDB_E_CLASSNOTREG: shell32's own CLSIDs, ShellLink,
ShellDispatch, QueryAssociations), SHGetDesktopFolder failing with 0x8000ffff, no shell tray window or desktop
folder, and c0000005 at start-up of the units that touch the desktop folder early. Fixing the registry/COM class
registration of shell32 is the next step.

Syscalls added or changed: none. DLLs added to wine-pe/dlls.txt: none.
