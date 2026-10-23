status: wip

# Slice: realtcc

Tiny C Compiler 0.9.27 (the official prebuilt `tcc-0.9.27-win64-bin.zip`, SHA-256 `34a72194...abb2` pinned and verified
on every run by `tools/ci/fetch_tcc.ps1`, cached with `actions/cache`) compiles a hello-world through the layer, and the
exe it wrote runs through the layer too. Gated ctest tests (`MSVC OR WIN10XBOX_REAL_PROGRAMS`, shared machine folder):

* `integration.tcc_version`: `C:\guest\tcc\tcc.exe -v`, exit 0, `tcc version 0.9.27`.
* `integration.tcc_compile`: `tcc.exe -o C:\Users\User\tcc_hello.exe C:\guest\tcc_hello.c`, exit 0, the exe must exist.
* `integration.tcc_hello_runs`: `C:\Users\User\tcc_hello.exe`, exit 42, prints `hello from tcc 42` (depends on tcc_compile).
* `integration.tcc_run`: `tcc.exe -run C:\guest\tcc_hello.c`, exit 42 (in-memory JIT).

An earlier attempt (c924f64, MSVC build of the tarball) failed at CMake configure, not at the compiler: its test line
lost a space in a merge (`sim"C:\\guest..."`), so `set_tests_properties` named a test that did not exist. It is replaced
by the prebuilt zip; nothing of it was ever run.

Run 39883508387 (83e1888): linux-cross green; both Windows jobs failed only on the four tcc tests (everything else passed),
all with `tcc: error: memory full (malloc)` on the first allocation. tcc.exe and libtcc.dll import `malloc` from Wine's
msvcrt.dll, whose DllMain does `heap = HeapCreate(0, 0, 0)` (the only Wine C runtime with a private heap; ucrtbase uses
the process heap). RtlCreateHeap is forwarded to the host ntdll, and Wine's RtlCreateHeap adds HEAP_GROWABLE when no
maximum size is given where the host's does not (kernelbase does it on Windows), so the heap msvcrt got could not serve
malloc. `w10x_RtlCreateHeap` (core/dispatch/rtl_forward.cpp, WINE_OVERRIDE in rtl_forward.inc) adds the flag. Run 39889944784
(first fix): tcc_version, tcc_compile, tcc_run passed on both Windows jobs and the produced exe printed `hello from tcc 42`
and exited 42, but tcc_hello_runs failed the generic `no builtin user32.dll in the modules trace` check (the exe imports
only kernel32 and msvcrt): the test now sets NO_USER32. That version also logged from inside the override in host mode,
and linux-cross (Wine host) then failed ten tests with `msvcrt.dll failed to initialize`; the override is now a plain
wrapper with no logging.

Results: to be filled in from the CI run.
