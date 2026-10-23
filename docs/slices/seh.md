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

## Layer fix (core/dispatch/process.cpp, Wine host only)

The first linux-cross run (84425ad, llvm-mingw guest under Wine) died in the very first guest exception: `err:unwind:
RtlUnwindEx invalid frame 00007FFFFEC22769` and, for the C++ sections, libc++abi's `Assertion failed: actions &
(_UA_SEARCH_PHASE | _UA_FORCE_UNWIND)` (the personality was not told it had reached the handler frame). Wine's
`RtlLookupFunctionEntry` only consults the dynamic function tables for a pc that is in no module of PEB->Ldr
(`lookup_function_info`, `LdrFindEntryForAddress`), and after the PEB take-over PEB->Ldr is the guest's list. Phase 1
of a dispatch starts at the faulting guest context and never needs host frames, but phase 2 (`RtlUnwindEx` called by
`__C_specific_handler` or libunwind's `_GCC_specific_handler`) starts inside the host ntdll and walks `RtlUnwindEx`,
`call_handler`, `dispatch_exception` and `KiUserExceptionDispatcher`, which had no unwind info any more; they were
unwound as leaf frames and the callee-saved registers (rbp held the unwind target) were never restored from the
exception CONTEXT. On real Windows the lookup does not go through PEB->Ldr, which is why the MSVC jobs were green.
`RegisterHostUnwindTables()` registers each host module's `.pdata` with the host `RtlAddFunctionTable` before the
take-over (only when the host ntdll is Wine's); the entries are unused until PEB->Ldr is swapped.

## Result

Pending the ci run of the head commit.
