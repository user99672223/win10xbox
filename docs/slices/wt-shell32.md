status: wip

# wt-shell32: Wine dlls/shell32/tests through the layer

Script: wine-pe/tests/shell32.sh (copy of advapi32.sh; builds the 19 START_TEST units plus rsrc.rc into
shell32_test.exe, runs each with a 30 s cap). Baseline: wine-pe/tests/shell32.baseline.
Registered as integration.winetest_shell32 (TIMEOUT 900) in tests/winetest/CMakeLists.txt.

Test IMPORTS (Makefile.in): shell32 ole32 oleaut32 user32 gdi32 advapi32 shlwapi; all are in wine-pe/dlls.txt,
so no DLL was added. The earlier commit's ci failure (run 39849670418) was integration.guest_window_input2,
not the generic builder; build_winetest.sh was unused and is deleted.

Push 1 is the measurement: every unit has baseline `run 999999`, so crashes and timeouts show as REGRESSION.
