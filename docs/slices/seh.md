status: ready

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

ci.yml run: https://github.com/user99672223/win10xbox/actions/runs/39884195071 (head f7981d9). All three jobs green
including "Gate - nothing skipped" (`ctest gate: PASS (nothing skipped, nothing missing)` on each). linux-cross hit an
apt mirror stall in "Install Wine + ninja" on its first attempt (no test ran) and passed on `gh run rerun --failed`;
the two Windows jobs were green on the first attempt.

`integration.guest_seh` (and the native smoke test `guest.seh`) passes on linux-cross (llvm-mingw guest, Wine host),
windows (Debug) and windows (RelWithDebInfo) (MSVC guest, real Windows host). The guest exits 42 and prints, copied
from the ctest-junit.xml of the windows jobs (MSVC `__try`):

    seh: start (msvc __try)
    seh: seh ok
    seh: veh ok
    seh: uef ok
    seh: throw_catch ok
    seh: dll_boundary ok
    seh: done failures=0

On llvm-mingw the guest prints the same section lines; the line is `seh: start (clang __try)` there (the configure-time
probe accepts `-fms-extensions`, so `__try/__except` is native; seen in the log of the earlier, failing linux-cross
run of the same build). The passing linux-cross run does not keep stdout, but `integration.guest_seh` requires every
`seh: <name> ok` line and `seh: done failures=0` by regex, fails on any `seh: <name> FAILED`, and requires exit 42.

What runs through the layer: `__try/__except` around an access violation (read and write), a divide by zero,
`RaiseException` with parameters and an access violation raised in a DLL frame; `AddVectoredExceptionHandler` seeing
an access violation and passing it on, a handler continuing execution after `RaiseException`,
`RemoveVectoredExceptionHandler`; the unhandled-exception filter called from an `__except` filter; C++ throw/catch,
rethrow, `std::exception_ptr`, throw out of and through a DLL frame (its destructor runs), catch inside the DLL.

Syscalls added or changed: none. Layer change: `RegisterHostUnwindTables()` in core/dispatch/process.cpp (above).
DLLs added to wine-pe/dlls.txt: none.

Cut from the source guest because other slices own them: threads, TLS callbacks, `thread_local`, static-init order,
`std::filesystem`, the stack-overflow section and the `/MD` variant.

What still fails: nothing in this slice. `guest_seh` is not part of the host-uwp package or tools/ci/smoke_guests.ps1
(gated by its ctest entries only).
