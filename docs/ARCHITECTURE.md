# win10xbox architecture

win10xbox is a Windows 10 22H2 compatibility layer that runs unmodified x64
Windows programs (and software drivers) natively on the Xbox Series S|X Zen 2
CPU from inside an Xbox Dev Mode UWP/AppContainer package. There is no CPU
emulation: guest code executes directly. What is virtualised is the *system*
the program sees: the OS version, the file system, the registry, the clock,
the process/user identity, the window system and, for drivers, the kernel.

The guiding principle: every component is either Microsoft's own binary,
a mature open-source project (Wine, ReactOS, MemoryModule, Detours, Zydis,
hivex, KMDF), or thin glue between them. New code is written only where no
donor fits, and the commit that adds it says why.

## Layer stack

```
+-----------------------------------------------------------------------+
|  Program (unmodified EXE)                                             |
+-----------------------------------------------------------------------+
|  Guest DLLs, manually mapped by our loader (never the host's copies):  |
|   - "native": genuine Microsoft user-mode DLLs from a Windows 10 22H2  |
|     install, supplied by the user into the machine folder              |
|     (kernel32, kernelbase, ntdll, msvcrt, user32, gdi32, comctl32 ...) |
|   - "builtin": Wine's LGPL PE builds (llvm-mingw), per-DLL fallback    |
|     chosen in machine config, exactly like Wine's native/builtin       |
+-----------------------------------------------------------------------+
|  Wine-derived win32u (NtUser*/NtGdi*)  +  winexbox.drv display/input   |
|  backend on CoreWindow / Direct3D (interface + headless stub now;      |
|  real backend deferred until console probe data exists)                |
+-----------------------------------------------------------------------+
|  OUR LOADER  (core/loader)                                             |
|   manual PE mapping of the EXE and its whole DLL tree; import          |
|   resolution with apiset + forwarder + delay-load handling; TLS;       |
|   SEH/unwind table registration; relocations; entry-point call.        |
|   Rewrites every Nt*/Zw* syscall stub in guest ntdll and win32u to     |
|   jump into the dispatcher.  Scans guest code with Zydis for           |
|   cpuid / rdtsc / rdtscp / syscall and patches them to trampolines.    |
+-----------------------------------------------------------------------+
|  OUR DISPATCHER  (core/dispatch)                                       |
|   per-syscall table, generated from j00ru's 22H2 syscall numbers:      |
|   (a) PASS-THROUGH  -> host ntdll (the real Xbox NT kernel), by name   |
|   (b) TRANSLATE     -> rewrite args (paths, handles), then pass through |
|   (c) EMULATE       -> user-mode implementation (processes, registry,  |
|                        win32k, tokens, anything the sandbox denies)    |
|   Unimplemented -> loud trace + STATUS_NOT_IMPLEMENTED, never a silent  |
|   STATUS_SUCCESS.  Every call is traced (Wine-style channels, +relay).  |
+-----------------------------------------------------------------------+
|  Subsystems used by the dispatcher (core/)                             |
|   clock     virtual time: QPC, tick count, KUSER_SHARED_DATA, rdtsc,   |
|             freeze/scale/offset; all time the program sees is ours     |
|   vfs       machine-folder path rewriting, drive letters, \??\ and     |
|             \Device\ prefixes, reparse to host-accessible locations     |
|   registry  real REGF hives read through hivex, in-memory overlay for  |
|             writes, 22H2 fidelity values from config                   |
|   token     a normal interactive user (not an AppContainer): SIDs,     |
|             groups, privileges, integrity level, session, elevation    |
|   version   PEB/KUSER_SHARED_DATA spoof: 10.0.19045 (22H2)             |
|   trace     channels, relay log of every syscall with args + NTSTATUS  |
+-----------------------------------------------------------------------+
|  OUR DRIVER HOST  (drivers/, details in drivers/README.md)             |
|   user-mode ntoskrnl/hal export surface (Io/Ke/Ex/Mm/Ob/Ps/Rtl/CRT,    |
|   Zw* bound to the dispatcher's own syscalls), DRIVER_OBJECT/          |
|   DEVICE_OBJECT/FILE_OBJECT/IRP model, namespace (\Device\X, \??\X),  |
|   NtCreateFile/NtDeviceIoControlFile/NtClose on driver devices ->      |
|   IRP_MJ_* -> IoCallDriver -> completion, virtual per-thread IRQL, one  |
|   worker thread for DPCs/timers/work items/ISRs, a minimal KMDF (WDF)  |
|   function-table shim.  .sys files load through the same loader.       |
|   VEH trap-and-emulate for privileged instructions (in/out, rdmsr/     |
|   wrmsr, cli/sti, mov cr*) against virtual CPU state is a separate     |
|   module (not part of drivers/).                                       |
+-----------------------------------------------------------------------+
|  Machine folder  (machine/)                                            |
|   a Wine-prefix-like directory that IS the virtual PC's C:\ -           |
|   Windows\System32, Program Files\, Users\, config\ hives (SYSTEM,      |
|   SOFTWARE, NTUSER.DAT), machine.json (dll overrides, fidelity values)  |
+-----------------------------------------------------------------------+
|  Hosts                                                                 |
|   host-win32/  console host: runs core/ unsandboxed on Windows (CI)     |
|   host-uwp/    C++/WinRT shell: CoreWindow + D3D12 + XAudio2 + Winsock, |
|                codeGeneration capability, Xbox deployment manifest       |
+-----------------------------------------------------------------------+
|  Xbox Series S|X, Dev Mode, AppContainer, real NT kernel underneath     |
+-----------------------------------------------------------------------+
```

## Control flow for one syscall

1. Guest code (say kernel32!CreateFileW -> kernelbase -> ntdll!NtCreateFile)
   reaches the ntdll stub. The loader has replaced the stub bytes
   (`mov r10,rcx; mov eax,N; syscall; ret`) with `jmp dispatcher_entry_N`.
2. `dispatcher_entry_N` saves the guest register state, builds a
   `SyscallFrame` (number, up to 16 stack args) and calls
   `Dispatcher::Invoke`.
3. The dispatcher looks up entry N in the generated table
   (`core/dispatch/syscall_table.gen.inc`), traces the arguments through the
   channel `relay`, and executes the entry's policy:
   - `PassThrough`: `host_ntdll[N].fn(args...)`, resolved once by name at
     start-up from the host process's own ntdll.
   - `Translate`: the per-syscall translator rewrites arguments (for file
     syscalls: OBJECT_ATTRIBUTES paths through the VFS), calls the host,
     then rewrites the result (handle registration, path un-mapping).
   - `Emulate`: a user-mode implementation (ported from Wine ntdll/
     wineserver or ReactOS) that never touches the host kernel for the
     emulated resource.
   - `Todo`: trace loudly and return `STATUS_NOT_IMPLEMENTED`.
4. NTSTATUS and the frame are traced; the trampoline restores the guest
   state and returns to the stub's caller.

Patched instructions (`cpuid`, `rdtsc`, `rdtscp`, raw `syscall`) use the same
mechanism: the scanner replaces the instruction (plus enough following bytes,
relocated by Zydis) with a jump to a per-site trampoline that calls the
virtual-CPU/virtual-clock code and resumes.

Privileged instructions inside drivers are not patched ahead of time: they
fault (#GP -> EXCEPTION_PRIV_INSTRUCTION) and the vectored exception handler
decodes them with Zydis, applies them to the virtual CPU state, advances RIP
and continues.

## Repository layout

```
core/         static lib, pure Win32/NT C++20, zero UWP/WinRT dependencies
  loader/     PE mapper (MemoryModule-derived), import/apiset/forwarder/
              delay-load resolution, TLS, SEH tables, stub rewriter, scanner
  dispatch/   syscall table (generated), dispatcher, trampolines
  trace/      channels, relay, formatting of NT structures
  vfs/        machine-folder path translation, drive letters
  registry/   hivex over REGF files + overlay
  clock/      virtual time
  token/      identity emulation
  cpu/        cpuid/rdtsc virtualisation, virtual CPU state
host-win32/   console host for CI and desktop testing
host-uwp/     C++/WinRT UWP shell (Xbox target); build + package only
drivers/      driver host: user-mode ntoskrnl, object manager + IRP engine,
              KMDF shim (the VEH trap-and-emulate for privileged instructions
              is a separate module); test drivers live in tests/drivers
wine-pe/      scripts/patches to build Wine's PE DLLs with llvm-mingw
winexbox/     display/input backend interface for Wine's win32u + stubs
machine/      machine-folder layout, hive handling, config schema, DLL import
tests/        unit, integration (real EXEs through host-win32), Wine test
              subset runner, kmtest runner
third_party/  pinned submodules and pinned-tag fetch scripts, with licenses
tools/probe/  the Dev Mode capability probe (moved here, untouched)
docs/         this file, DECISIONS.md, SYSCALLS.md (generated), BUILD.md,
              TESTING.md, STATUS.md
.github/      ci.yml (windows + linux jobs), wine-pe.yml
```

## Licensing boundaries

- `core/`, `host-*/`, `machine/`, `tests/`, `tools/` (except `tools/probe`
  which keeps its own terms): MIT (`LICENSE`).
- Wine-derived code lives in `wine-pe/` (build scripts + patches, LGPL-2.1+
  output) and in clearly marked `*_wine.cpp` files whose header states the
  Wine origin; those files are LGPL-2.1+.
- ReactOS-derived code (none so far: the M4 driver host is original MIT code written from the documented
  Windows layouts) would live under `drivers/ntoskrnl/reactos/` and be GPL-2.0+;
  it is built into a separate DLL (`ntoskrnl.exe` for the driver host) so the
  GPL boundary is a process-visible module boundary.
- `third_party/` keeps each project's own license file.
- No Microsoft binaries are ever committed. CI copies guest DLLs from the
  runner's System32 at test time; users import their own 22H2 DLLs with
  `machine/import_windows_dlls.ps1`.

## What is deliberately deferred

See `DECISIONS.md`. Everything that depends on the console's capability
matrix (the probe in `tools/probe` has not yet run on an Xbox) is isolated
behind an interface plus a feature flag in `core/config.h`, with a default
chosen now and the condition that flips it recorded.
