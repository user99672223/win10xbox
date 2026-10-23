status: wip

# Slice `-seh`: guest SEH / vectored exception handling / C++ exceptions

Gate: `integration.guest_seh` (tests/guest/seh.cpp + the DLL tests/guest/seh_dll.cpp), on both Windows jobs and
linux-cross. Each section prints `seh: <name> ok`; the test fails on `seh: <name> FAILED` and on any missing line,
and must exit 42.

Sections: `throw_catch` (C++ throw/catch, rethrow, exception_ptr across frames), `dll_boundary` (throw out of and
through a DLL frame, destructor of the DLL frame runs, catch inside the DLL, DLL constructor), `seh` (access
violation read/write, divide by zero, `RaiseException` with parameters, an access violation from a DLL frame, all
caught by `__try/__except`), `veh` (a vectored handler that sees an access violation and passes it on, one that
continues execution after `RaiseException`, `RemoveVectoredExceptionHandler`), `uef` (the unhandled-exception
filter called from an `__except` filter).

`__try/__except` is native on MSVC. On llvm-mingw it is native when a configure-time compile+link check with
`-fms-extensions` passes (tests/guest/CMakeLists.txt, `SEH_NATIVE`), otherwise the same helper runs on a vectored
handler that restores a captured context. The guest prints which one it used on its first line.

## Result

Not yet run on the merged tree.
