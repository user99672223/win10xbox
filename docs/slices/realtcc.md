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

Results: to be filled in from the CI run.
