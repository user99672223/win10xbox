status: wip

# Slice wt-ntdll-rest: Wine's ntdll `exception`, `info`, `pipe`, `sync`, `threadpool`, `virtual`, `wow64` units through the layer

Order of the row: exception, info, pipe, sync, threadpool, virtual, wow64. A unit joins `wine-pe/tests/ntdll.allowlist`
(0 failures) or `wine-pe/tests/ntdll.parity` (Windows fails some assertions itself; the layer must fail none that a
native run of the same binary on the same runner passes) only when measured on both Windows jobs. Start:
docs/slices/wt-ntdll.md (exception/info/pipe/sync/threadpool/virtual crashed with an access violation, wow64 completed
with 137 failures); the parity harness of docs/slices/wt-ntdll-file.md and the registry slice are merged in.

## Harness changes (tests/winetest/run_ntdll.cmake, wine-pe/tests/ntdll.sh, ntdll.parity)

* `ntdll_test.exe <unit> --platform=windows` left the argument in `argv`, and `virtual`, `info`, `sync`, `pipe` read any
  argument past the unit name as "I am the child process" and ran nothing (`0 tests executed`, natively too). The guest now
  has a `main` of its own that sets `WINETEST_PLATFORM` and removes the argument before Wine's `main` runs.
* A unit that starts children (`info`, `pipe`, `virtual`) shares its stdout with them, and each child prints a summary
  line of its own. The harness took the first one; it takes the one with the most assertions (the parent's).
* `ntdll.parity` accepts `<unit> <floor> measure`: run as on Windows and compared with a native run, printed as
  `WINETEST parity ntdll:<unit> (measured, not a gate) -> ...`, never failing the test. Units that are not within parity
  yet stay measured, so every run of this slice keeps telling what is left.
* The units of this slice that do not complete get a second run with `WIN10XBOX_DEBUG=-all,+relay,+dispatch,+process`
  (tail in `<unit>.relay.log`) and the host's `crash.txt` (`<unit>.crash.txt`) in the `windows-<config>-logs` artifact.

## Measurements (ci.yml runs 39952147759 and 39954832693, Debug and RelWithDebInfo; `--platform=windows`)

| unit | layer | native, same runner | verdict |
|---|---|---|---|
| exception | dies, exit 0x80000002 | dies, exit 0x80000002 | not comparable (below) |
| info | 62356 asserts, 91 failures, then hangs on a child (cap) | 60023 asserts, 2 failures | measured |
| pipe | 2353 asserts, 18 failures | 2353 asserts, 7 failures | measured |
| sync | 429 asserts, 1 failure (`sync.c:857`) | completes | measured |
| threadpool | 4721 asserts, 0 failures | 4721, 0 | **gate (parity, floor 2300)** |
| virtual | access violation after `virtual.c:1743` | 3047 asserts, 1 failure (`virtual.c:1743`) | measured |
| wow64 | 1103 asserts, 135 failures | 1201 asserts, 40 failures | measured |
| reg (coordinator's addition) | 1185 asserts, 13 failures | 28832 asserts, 0 failures | measured |

### exception

The layer and a native run end the same way: after `NtSetContextThread` (the hardware-breakpoint test, `exception.c:3363`) the
next test, `exception.c:3394` (EFLAGS.AC set, a misaligned access, `align_check_handler`), raises
STATUS_DATATYPE_MISALIGNMENT that nothing handles on this runner image, and the process exits with 0x80000002 before
it prints a line. A native run is the yardstick for parity and it does not complete, so there is nothing to compare;
the layer is not the cause (the relay trace shows no syscall between the two).

### info (lines the layer fails and a native run passes)

Fixed here: `info.c:392`, `394` (SystemEmulationProcessorInformation reported the host's level/revision, the layer's
identity must apply to it as to class 1), `515`, `516`, `520`, `521`, `531` (SystemTimeOfDayInformation: the kernel
copies what the caller has room for, a length of 0 succeeds with 0 bytes, more than 0x30 is a mismatch),
`1154`, `1157` (SystemTimeAdjustmentInformation: size check before the privilege check).
Left: `2109`, `2484`, `2707`-`2798` (debugger: `CreateProcess` with DEBUG flags, `DebugActiveProcess`: no debugger
support), `2205`, `3906`, `3930`, `3936` (`NtCreateDebugObject` is STATUS_NOT_SUPPORTED), `3201`, `4168`-`4256`
(`NtSetInformationProcess(ProcessAccessToken)` on a child, `ProcessImageFileName` of a child). The native run fails
`info.c:687` (LsaIso.exe TebBase) only.

### pipe

Lines native passes and the layer fails: `2271`, `2286` (Wine's kernelbase `DisconnectNamedPipe` reports
STATUS_PENDING of a disconnect on an overlapped handle as ERROR_IO_PENDING; kernel32 waits), `2595`, `2596`, `2654`, `2663`,
`2664` (the pipe's owner/group SID comes from the host's token while `GetTokenInformation` answers the layer's
token; creating a pipe with the layer's SID as owner is INVALID_OWNER on the host). Native fails `1117` (x5), `1754` (x2) too.

### sync

`sync.c:857`: `WaitForSingleObject(child, 1000)` on the child `ntdll_test.exe sync subprocess`; the child is a new host
process running a new guest and does not exit within a second on the runner.

### virtual

`RtlCreateUserStack` commit sizes (`virtual.c:1049`-`1344`, Wine's implementation, `todo_wine` on the Wine platform)
and `NtMapViewOfSection` of an image section into another process (`virtual.c:1697`-`1728`: STATUS_NOT_SUPPORTED, the
section is the layer's, the target a host process); the unit then reads through the NULL result (`virtual.c:1743`).

### wow64

Left, native passes: `wow64.c:1333` (LDT selectors: Wine's `RtlWow64GetThreadSelectorEntry` ends in
`NtQueryInformationThread(ThreadDescriptorTableEntry)`, which the host kernel answers STATUS_NOT_IMPLEMENTED on x64; Windows' ntdll
answers STATUS_UNSUCCESSFUL/NO_LDT itself), `1492`, `1495`, `1502`, `1508` (`C:\Windows\SysWOW64\version.dll` does not exist
in the machine folder: no 32-bit builtins). Fixed here: `wow64.c:318` (`ThreadQuerySetWin32StartAddress` of a guest thread
is the start address the guest asked for, not the host thread's `GuestThreadProc`), `1423`, `1447`, `1453`
(`NtMapViewOfSectionEx` with a `MemExtendedParameterImageMachine` other than AMD64 is STATUS_NOT_SUPPORTED).

### reg

The unit completes (the registry slice). Natively it runs 28832 assertions with the runner's SeBackup/SeRestore
privileges; the layer's unprivileged token skips `test_NtRegLoadKeyEx` (`reg.c:2548`). Run as Windows, the layer fails
`reg.c:2671`, `2674`, `3066`, `3070`, `3133`, `3137`: `RtlQueryRegistryValues`, user-mode code of Wine's ntdll that is
`todo_wine` on the Wine platform. Not a layer fault, not gateable by parity.

## Syscalls changed

* `NtQuerySystemInformation`: class 63 (SystemEmulationProcessorInformation) carries the machine identity as class 1 does;
  class 3 (SystemTimeOfDayInformation) length rules.
* `NtSetSystemInformation`: class 28 checks the length (8) before the privilege.
* `NtQueryInformationThread`: `ThreadQuerySetWin32StartAddress` for the current guest thread (new `ThreadState::win32_start`).
* `NtMapViewOfSectionEx`: image-machine extended parameter other than AMD64 for the layer's image sections.

## What still fails

See the sections above.
