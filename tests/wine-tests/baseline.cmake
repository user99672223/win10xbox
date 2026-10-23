# Wine conformance baseline (measured under Wine 9.0 / llvm-mingw, Debug build).
#   win10xbox_wine_test(<dll> <test> <run|crash|timeout> <max_failures> [NEEDS_GEN])
# run: completes, failures (Wine counts todo-failures too) must stay <= max.
# crash: does not complete (unhandled exception / guest main thread ended early).
# timeout: still running at the per-test cap. The measured numbers are in
# docs/TESTING.md; keep the two in sync.

# --- ntdll: Rtl* library code (pure computation, no syscalls) -----------------
win10xbox_wine_test(ntdll rtlstr    run 0)
win10xbox_wine_test(ntdll string    run 0)
win10xbox_wine_test(ntdll rtlbitmap run 0)
win10xbox_wine_test(ntdll error     run 0)
win10xbox_wine_test(ntdll env       run 0)
# --- ntdll: M2 subsystems (VFS paths/directories, object manager, registry, clock)
win10xbox_wine_test(ntdll path      run 0)
win10xbox_wine_test(ntdll directory run 556)
win10xbox_wine_test(ntdll file      crash 0)
win10xbox_wine_test(ntdll om        crash 0)
win10xbox_wine_test(ntdll reg       timeout 0)
win10xbox_wine_test(ntdll info      crash 0)
win10xbox_wine_test(ntdll time      run 0)
# --- kernel32 -----------------------------------------------------------------
win10xbox_wine_test(kernel32 atom      run 0)
win10xbox_wine_test(kernel32 codepage  run 0)
win10xbox_wine_test(kernel32 environ   run 10)
win10xbox_wine_test(kernel32 path      run 2)
win10xbox_wine_test(kernel32 directory run 0)
win10xbox_wine_test(kernel32 file      timeout 0)
win10xbox_wine_test(kernel32 heap      crash 0)
win10xbox_wine_test(kernel32 time      run 38)
win10xbox_wine_test(kernel32 locale    run 297)
win10xbox_wine_test(kernel32 version   run 54)
win10xbox_wine_test(kernel32 virtual   crash 0)
win10xbox_wine_test(kernel32 sync      crash 0)
win10xbox_wine_test(kernel32 module    crash 0)
# --- advapi32: registry (REGF reader/writer + overlay) ------------------------
win10xbox_wine_test(advapi32 registry  crash 0 NEEDS_GEN)
