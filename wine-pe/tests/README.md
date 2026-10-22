# wine-pe/tests — Wine conformance tests for whole DLLs, built as PE x64

Wine's own `dlls/<dll>/tests` (LGPL, compiled from `wine-pe/wine-src`, never copied
here) built with llvm-mingw into one `<dll>_test.exe` per DLL, the way Wine links
them, and run unit by unit through `win10xbox.exe`. Results: `docs/WINETEST.md`.

Work in progress (slice `wt-media`: windowscodecs, gdiplus, dwrite).
