# host-uwp-lite first Xbox run — diagnosis

Build: `host-uwp-lite` v1.0.0.2, source commit `6468741` on branch
`claude/youthful-cerf-l7xxsb`. Package was downloaded from the CI
`uwp-lite.yml` run (id `39647136661`, artifact `host-uwp-lite-msix`) and
installed on `XBOX` (SystemOS build `10.0.26100.9445`, Game bucket).

## Verdict

`RESULT: INCOMPLETE`. The host didn't crash in its own address space (no
`CRASH:` line in `result.txt`). The guest's smoke.exe loaded, Wine ntdll
rewrote its syscall stubs, finished the loader's import-fixup pass, then
`NtContinue` transferred execution to the guest's CRT entry point. After that
the trace ends and the process is gone.

`result.txt` is frozen at `phase=guest running`; the final-verdict writer in
`smoke.cpp` never ran, consistent with the whole host process dying when the
in-process guest died (per `README.md`:
`process::Launch runs the guest in this process and temporarily takes over
PEB fields`).

## Root cause (direct, from the trace)

The guest's IAT fixup pass for `C:\guest\smoke.exe` resolves **zero** of the
92 symbols it imports from `KERNEL32.dll`. Every single one is logged as

```
0b2c:warn:wine:module:import_dll No implementation for KERNEL32.dll.<name>
  imported from L"C:\\guest\\smoke.exe", setting to 0000020427E90XXX
```

The fallback addresses `0000020427E905E8`..`0000020427E90CCC` are contiguous
in the IAT fixup region — Wine's `import_dll` has pointed every IAT slot at
the same Wine stub-not-found fallback.

### What smoke.exe imports and loses

92 KERNEL32 names, including everything the UCRT startup needs before `main`:

```
HeapAlloc, HeapFree, HeapReAlloc, HeapSize, GetProcessHeap,
GetStdHandle, SetStdHandle, WriteFile, WriteConsoleW, ReadFile, ReadConsoleW,
GetCommandLineW, GetCommandLineA, GetEnvironmentStringsW,
GetModuleHandleW, GetModuleHandleExW, GetModuleFileNameW, GetProcAddress,
LoadLibraryW, LoadLibraryExW, FreeLibrary, FindResourceW, LoadResource,
LockResource, SizeofResource,
CreateFileW, GetFileType, FindFirstFileExW, FindNextFileW, FindClose,
CloseHandle, GetFileSizeEx, SetFilePointerEx, FlushFileBuffers,
ExitProcess, TerminateProcess, GetCurrentProcess, RaiseException,
UnhandledExceptionFilter, SetUnhandledExceptionFilter,
GetSystemInfo, GetNativeSystemInfo, GetVersionExW,
IsProcessorFeaturePresent, IsDebuggerPresent, QueryPerformanceCounter,
QueryPerformanceFrequency, Sleep, GetSystemTime, GetSystemTimeAsFileTime,
GetCurrentProcessId, GetCurrentThreadId, GetCurrentThread, GetLastError,
SetLastError, InterlockedPushEntrySList, InterlockedFlushSList,
InitializeSListHead, FlsAlloc, FlsGetValue, FlsSetValue, FlsFree,
EnterCriticalSection, LeaveCriticalSection, InitializeCriticalSectionEx,
DeleteCriticalSection, EncodePointer,
GetACP, GetOEMCP, GetCPInfo, IsValidCodePage, GetStringTypeW,
GetLocaleInfoW, LCMapStringW, CompareStringW, MultiByteToWideChar,
WideCharToMultiByte, IsValidLocale, GetUserDefaultLCID,
EnumSystemLocalesW, GetDateFormatW, GetTimeFormatW,
GetTempPathW, IsThreadAFiber, VirtualProtect, OutputDebugStringW,
SetEnvironmentVariableW, SetConsoleCtrlHandler, SetStdHandle,
GetConsoleOutputCP, GetConsoleMode, GetStartupInfoW
```

### Trail of what we observed

- `kernel32.dll` and `kernelbase.dll` were both staged intact; sizes match
  the `wine-pe/MANIFEST.txt` (`kernel32.dll` 1 691 648 B,
  `kernelbase.dll` 5 656 576 B).
- VFS translation lines prove the loader followed `C:\guest\smoke.exe` and
  the forwarder lookups through `C:\windows\system32\kernel32.dll` straight
  into `machine\Windows\builtin\kernel32.dll`:

  ```
  0b2c:trace:vfs:TranslateNtPath nt '\??\C:\windows\system32\kernel32.dll' ->
    '\??\Q:\...LocalState\machine\Windows\builtin\kernel32.dll' (00000000)
  ```

- Last meaningful syscalls before silence:

  ```
  0b2c:trace:relay: NtProtectVirtualMemory(...) ret=00000000 (3.0us)
  0b2c:trace:relay: NtQueryInformationProcess(..., ProcessDebugPort=0x7, ...)
                    ret=00000000 (1.7us)
  0b2c:trace:relay: NtContinue(000000a0669ff780, 00000001) (may not return)
  ```

  `NtContinue` is the loader's "jump to the newly-mapped image's entry
  point" transition. The CRT then dereferences the first of its stubbed
  imports (`GetProcessHeap`/`InitializeSListHead`/`GetSystemTimeAsFileTime`
  during `__scrt_initialize_crt`) and crashes without the host's VEH seeing
  it, because `NtContinue` already switched into guest context.

## Suspected root causes

Wine's `kernel32.dll` is almost entirely a *forwarder* module to
`kernelbase.dll`. If `import_dll` can resolve nothing in `kernel32`, one of
these is true at that moment:

1. **`kernelbase.dll` is not yet loaded when `smoke.exe`'s imports are
   resolved.** The dependency order the Wine loader walked did not pull
   `kernelbase` in before fixing up the guest's IAT. Follow-ups:
   - Add a trace line to the loader just before the `import_dll` pass that
     dumps the current loaded-module list for this process; expect
     `kernelbase.dll` to be absent.
   - If confirmed, make `smoke.cpp`'s pre-launch step force-load the Wine
     builtins in dependency order (`ntdll` → `kernelbase` → `kernel32` →
     anything else) before `process::Launch`, so forwarders can resolve.

2. **The staged `kernel32.dll` and `kernelbase.dll` forwarder targets do
   not line up at the export-name level.** Both DLLs are tagged `wine-11.9`
   in `wine-pe/MANIFEST.txt`, but the forwarder redirection in a Wine
   kernel32 PE build is a per-build string in the export table
   (`"KERNELBASE.<name>"`); if the two were cross-pollinated from different
   intermediate builds, each `kernel32` forwarder points at a name that
   does not exist in this `kernelbase`. Follow-ups:
   - On a laptop, `dumpbin /exports kernel32.dll` from the staged copy and
     grep the forward names, then `dumpbin /exports kernelbase.dll` and
     check that every forward target resolves.

## Not the cause

- **Xbox sandbox denials.** The run reached the guest CRT entry inside its
  own in-process guest address space; the host's AppContainer caps have no
  bearing on an in-process `NtContinue`.
- **Missing NLS data.** The first run crashed on `l_intl.nls` with
  `NtGetNlsSectionPtr` → `STATUS_OBJECT_NAME_NOT_FOUND` and the host's
  unhandled-exception filter did fire (`CRASH:` line in a prior
  `result.txt`); after `l_intl.nls` and `c_10000.nls` were uploaded by hand
  into `LocalState\machine\Windows\System32\` the trace advances past that
  codepath cleanly. The remaining failure is unrelated.
- **Missing System32 names on Xbox.** 18 of 47 inventoried names are not
  readable by name (see `SYSTEM32_INVENTORY.md`), including
  `kernel32.dll`, but `host-uwp-lite` reads none of those at runtime: it
  stages its own Wine builtins instead and reads only `ucrtbase.dll` and
  `msvcrt.dll` by name (both OK on Xbox).

## Thread IDs seen

- `0b2c` — the host-side probe worker that calls `Initialize` → `Launch` and
  owns the guest's CRT entry context after `NtContinue`.
- `0d2c` — a loader worker that iterated `RewriteSyscallStubs` across every
  Nt* syscall id from 0x00 to past 0xcf.

Both IDs appear in `trace.log`; cross-referencing by thread id separates the
host-side setup from the guest's loader activity.
