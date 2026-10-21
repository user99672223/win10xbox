# drivers/ - the in-process driver host (D16, milestone M4)

Loads kernel-mode driver images (`.sys`: WDM, and KMDF drivers through a small framework shim) into the
host process and runs them in user mode. A guest `CreateFile("\\.\Foo")` + `DeviceIoControl` reaches the
driver's `MajorFunction` table exactly as it would in Windows: through a `DRIVER_OBJECT`, a `DEVICE_OBJECT`,
an `IRP`, `IoCallDriver` and `IoCompleteRequest`. This is ordinary Windows driver-model infrastructure
(the same job Speakeasy and ReactOS' user-mode test harnesses do); nothing here touches the real kernel.

Everything in this directory is MIT. **No ReactOS code was ported**: ReactOS and Speakeasy were used as references
for behaviour and for the list of APIs drivers actually import. If ReactOS-derived code is ever added it goes in
`drivers/ntoskrnl/reactos/` with a GPL-2.0+ header (see `docs/ARCHITECTURE.md`, licensing boundaries).

## Layout

```
include/ntk/ntk.h            kernel types and constants (DEVICE_OBJECT, IRP, KEVENT ...), for drivers and the host
include/win10xbox/drivers.h  public host API
ntoskrnl/                    the ntoskrnl/hal export surface (one .cpp per family) + export table + loud stubs
host/                        object registry, namespace, driver loader, I/O manager (IRP engine), dispatcher hooks
kmdf/                        WDF function-table shim + WDFLDR bind protocol
sources.cmake, CMakeLists.txt  -> static library `win10xbox_drivers` (links win10xbox_core)
../tests/drivers/            test drivers, guest clients, unit tests, layout checks (see "Tests")
```

## The object / IRP model

```
guest: CreateFile("\\.\OurTestDevice") -> kernelbase -> NtCreateFile(\??\OurTestDevice)
dispatcher -> drivers::Drv_NtCreateFile
   namespace: \??\OurTestDevice --symlink--> \Device\OurTestDevice  -> DEVICE_OBJECT*
   FILE_OBJECT (real layout) + IRP_MJ_CREATE -> IoCallDriver(top of the device stack)
   success: guest HANDLE = dispatch::RegisterEmulatedObject(ObjectKind::Device, ...)
guest: DeviceIoControl -> NtDeviceIoControlFile -> drivers::Drv_NtDeviceIoControlFile
   IRP_MJ_DEVICE_CONTROL (buffered / IN_DIRECT / OUT_DIRECT / NEITHER set up like the I/O manager does:
   SystemBuffer, MDL, Type3InputBuffer) -> IoCallDriver -> driver dispatch -> IoCompleteRequest
   -> completion routines walk bottom-up -> top-level completion copies the output back, fills the IOSB,
   signals the event; a STATUS_PENDING request is waited for (synchronous handle) or completed later
   (overlapped handle: event + IOSB, like the file object event).
guest: CloseHandle -> drivers::Drv_NtClose -> pending IRPs cancelled, IRP_MJ_CLEANUP, IRP_MJ_CLOSE
```

* `DRIVER_OBJECT`, `DEVICE_OBJECT`, `FILE_OBJECT`, `IRP`, `IO_STACK_LOCATION`, `MDL`, `KDPC`, `KTIMER`,
  `KEVENT` ... have the Windows 10 x64 layouts. `tests/drivers/layout_check` compares 119 offsets/sizes of
  `ntk.h` with the mingw-w64 ddk headers and 58 of the WDF shim types with Microsoft's KMDF 1.33 headers.
* Namespace (`host/namespace.cpp`): named devices (`\Device\X`) and symbolic links, case-insensitive,
  `\??\`, `\DosDevices\` and `\GLOBAL??\` are the same directory, links are followed (8 hops), a name below a
  device (`\Device\X\sub`) opens the device with the remainder in `FileObject->FileName`. Only what drivers
  create lives here; every other path still goes to the VFS/host.
* IRP owners: `DriverOwned` (`IoAllocateIrp`), `IoMgrOwned` (`IoBuild*Request`: freed by completion, output
  copied back, `UserEvent`/`UserIosb` honoured), `TopLevel` (created by the host for guest requests).
  `IoCancelIrp`, cancel routines, `IoStartPacket`/`IoStartNextPacket` with device queues, `IoSetCompletionRoutine`
  semantics (`SL_INVOKE_ON_*`, `STATUS_MORE_PROCESSING_REQUIRED`, pending propagation) are implemented.
* Objects: `ObReferenceObjectByHandle/Pointer/Name`, `ObfDereferenceObject` over a reference-counted registry
  (`host/objects.cpp`); pseudo `PEPROCESS` (guest process, System) and `PETHREAD`; `PsCreateSystemThread`
  threads can be waited on.

## Threads, IRQL, DPCs

* **Virtual IRQL**: a per-thread number in a TLS slot (PASSIVE 0, APC 1, DISPATCH 2, DIRQL up to 15). It never
  touches hardware. Spin locks really exclude (CAS loop) and also raise/lower the virtual level; fast mutexes run at
  APC_LEVEL; waits with a timeout at DISPATCH_LEVEL, paged-pool allocation at DISPATCH_LEVEL, lowering below
  the current level and returning from a dispatch routine at a different level are reported as `IRQL violation`
  on the `driver` channel (`Options::strict_irql` makes them bug checks).
* **One worker thread** runs DPCs (at DISPATCH_LEVEL), kernel timers, work items (`ExQueueWorkItem`,
  `IoQueueWorkItem`) and interrupt service routines (at the connected DIRQL, under the interrupt lock). A DPC
  queued by a thread runs *asynchronously on the worker*, not "when the queuing thread's IRQL drops". Consequences
  (documented difference from a real kernel): a DPC can run while the thread that queued it is still at
  DISPATCH_LEVEL; there is no per-processor DPC queue, importance or target processor.
  `drivers::InjectInterrupt(vector)` delivers an interrupt to a service routine connected with `IoConnectInterrupt`.
* Dispatcher objects (events, semaphores, mutexes, timers, pseudo threads) live in the driver's own memory; all
  waits share one lock + condition variable, so there are no host handles behind them.
* `ExGetPreviousMode()` / `Irp->RequestorMode` are `UserMode` for requests from a guest thread, `KernelMode`
  for host threads and driver-originated `Zw*` calls.

## ntoskrnl surface

About 400 exports, grouped by file. Bound by name when a driver is loaded (`ntoskrnl.exe`, `hal.dll`, `wdfldr.sys`
imports all resolve against one table).

| Area | Implemented |
|------|-------------|
| Io (`ntoskrnl/io.cpp`) | `IoCreateDevice(Secure)`, `IoDeleteDevice`, `IoCreate(Unprotected)SymbolicLink`, `IoDeleteSymbolicLink`, `IoAttachDeviceToDeviceStack(Safe)`, `IoAttachDevice`, `IoDetachDevice`, `IoGetAttachedDevice(Reference)`, `IoGetLowerDeviceObject`, `IoAllocateIrp`, `IoInitializeIrp`, `IoFreeIrp`, `IoReuseIrp`, `IofCallDriver`/`IoCallDriver`, `IofCompleteRequest`/`IoCompleteRequest`, `IoMarkIrpPending`, `IoBuildDeviceIoControlRequest`, `IoBuild(A)SynchronousFsdRequest`, `IoCancelIrp`, `IoAcquire/ReleaseCancelSpinLock`, `KeXxxDeviceQueue`, `IoStartPacket`, `IoStartNextPacket(ByKey)`, `IoRequestDpc`, `IoInitialize/Start/StopTimer`, `IoAllocateMdl`, `IoFreeMdl`, `MmProbeAndLockPages`, `MmUnlockPages`, `MmBuildMdlForNonPagedPool`, `MmMapLockedPages(SpecifyCache)`, `IoConnectInterrupt`/`IoDisconnectInterrupt`, `IoGetDeviceObjectPointer`, `IoCreateDriver`/`IoDeleteDriver`, `IoCreateFile`, `IoRegisterShutdownNotification`, `IoGetStackLimits`, power `PoCallDriver`/`PoStartNextPowerIrp` |
| Ke (`ke.cpp`) | IRQL (`KeGetCurrentIrql`, `KfRaise/LowerIrql`, `KeRaiseIrql`, `KeRaiseIrqlToDpcLevel`), spin locks (plain, in-stack queued, queued-lock array), events, semaphores, mutexes, fast and guarded mutexes, `KeWaitForSingle/MultipleObjects`, `KeDelayExecutionThread`, `KeStallExecutionProcessor`, DPCs, timers (`KeSetTimer(Ex)`, coalescable), `KeSynchronizeExecution`, time (`KeQuerySystemTime`, `KeQueryInterruptTime`, `KeQueryTickCount`, `KeQueryPerformanceCounter` from the virtual clock), processors, `KeBugCheck(Ex)` |
| Ex (`pool_ex.cpp`) | `ExAllocatePool*` (incl. `ExAllocatePool2`, tags, double-free and tag-mismatch detection), `ExFreePool(WithTag)`, fast mutex, `ERESOURCE`, lookaside lists, `ExQueueWorkItem`, `IoAllocate/QueueWorkItem`, interlocked list helpers, time conversion, `ExRaiseStatus` |
| Ob / Ps / Mm / misc (`ob_ps_mm.cpp`) | `Ob*` (reference counting, `ObReferenceObjectByName`), `Ps*` ids and lookups, `PsCreateSystemThread`, `PsSetLoadImageNotifyRoutine` (really delivered for drivers loaded by the host), `IoCreateNotificationEvent`/`IoCreateSynchronizationEvent`, `MmIsAddressValid`, `MmGetSystemRoutineAddress`, contiguous/non-cached allocation, `MmCopyMemory` (virtual), `ProbeForRead/Write`, ETW/WMI/Cm callbacks as accepted-and-dropped sinks |
| Rtl / CRT / Dbg (`rtl_crt.cpp`) | `RtlInitUnicodeString`/`AnsiString`, `RtlCopyUnicodeString`, `RtlAppend*`, `RtlGetVersion` (from the configured 22H2 version), `Rtl*Memory`; ~140 pure `Rtl*` functions forwarded to the host ntdll (strings, bitmaps, SID/ACL/SD, time, generic tables, compression, unwind); CRT (`mem*`, `str*`, `wcs*`, `_snprintf`, `sprintf_s`, `_vsnwprintf` ... with the kernel `%wZ`/`%Z`); `DbgPrint(Ex)` to the `driver` trace channel |
| Registry helpers (`registry.cpp`) | `RtlQueryRegistryValues(Ex)` (table semantics incl. SUBKEY/TOPKEY/REQUIRED/DIRECT/DELETE/NOEXPAND), `RtlWriteRegistryValue`, `RtlDeleteRegistryValue`, `RtlCheckRegistryKey`, `RtlCreateRegistryKey` - built on the dispatcher's `NtOpenKey`..., so they see the emulated registry |
| Zw / Nt | every `Zw*`/`Nt*` import is bound to the dispatcher's implementation of the same syscall (or the host ntdll when the dispatcher is not initialised), so a driver sees the emulated registry, VFS and clock; `Todo` syscalls become loud stubs |
| Data exports | `IoFileObjectType` & co (opaque dummies), `PsInitialSystemProcess`, `PsLoadedModuleList` (one entry per loaded driver), `NtBuildNumber`, `KeNumberProcessors`, `KdDebuggerNotPresent`, `MmUserProbeAddress`, `MmSystemRangeStart` ... |

### Everything else is a loud stub

An import with no implementation is bound to a generated thunk that traces
`FIXME driver: unimplemented kernel API <Name> called by a driver, returning STATUS_NOT_IMPLEMENTED` and returns
`0xC0000002`; it never silently succeeds. `DriverInfo::stubbed_imports` and a `WARN` per import at load time list
them, `drivers::StubCallCount("Name")` counts calls (the test suite asserts it). Known unimplemented families:
PnP (`IoRegisterDeviceInterface`, `IoGetDeviceProperty`, `IoInvalidateDeviceRelations`, `IoRegisterPlugPlayNotification`),
power IRP creation, APCs, `Se*` access checks, cancel-safe queues (`IoCsq*`), HAL bus/port access
(`READ_PORT_*`, `HalGetBusData` ... see below), `MmMapIoSpace` (returns NULL: there is no physical address space),
DMA adapters, filter manager, NDIS, USB.

Intentionally implemented as *null sinks*, documented because they succeed without effect: ETW (`EtwRegister`,
`EtwWrite*`, `EtwEventEnabled` -> FALSE), `IoWMIRegistrationControl`, `WmiTraceMessage` (WPP/telemetry),
`CmRegisterCallback(Ex)`, `ObRegisterCallbacks`, `PsSetCreateProcess/ThreadNotifyRoutine` (registered, never fired - a
`WARN` is traced). `KeRegisterBugCheckCallback` accepts and never calls (there is no crash dump).

## KMDF decision

**A minimal WDF shim, not Microsoft's framework.** Building `third_party/Windows-Driver-Frameworks` as the
framework means compiling `WDF01000.sys` (about 250 C++ files) against the WDK `km` headers, the kernel CRT and
a PnP/power manager - none of which exist in this tree or on the Linux llvm-mingw toolchain - for a framework whose
value to the target workloads (control-device and software drivers) is its API, not its PnP state machine.
`drivers/kmdf/wdf_shim.cpp` instead implements the contract a *WDK-built* KMDF driver has with its framework:

* The driver's `FxDriverEntry` stub imports `WdfVersionBind`/`WdfVersionUnbind` from `WDFLDR.SYS`; they exist here
  (`wdf_shim.cpp`, protocol taken from `fxlibrarycommon.cpp`, BIND_INFO v1 and v2). Bind hands back a
  458-entry function table (KMDF 1.33; clients with fewer entries work) and a `WDF_DRIVER_GLOBALS`.
* The public KMDF headers' macros call `WdfFunctions[index](WdfDriverGlobals, ...)`. The indices are parsed at
  configure time from `wdffuncenum.h`; **every** slot is filled - implemented functions by name, the rest with
  a loud stub that names the WDF function (`unimplemented kernel API WdfTimerCreate ...`).
* Implemented (control-device / non-PnP model): `WdfDriverCreate`, `WdfControlDeviceInitAllocate`,
  `WdfDeviceInitSet{DeviceType,Exclusive,Characteristics,IoType,FileObjectConfig}`, `WdfDeviceInitAssignName`,
  `WdfDeviceCreate`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfIoQueueCreate`
  (sequential and parallel dispatch, default queue), request dispatch for create/close/cleanup/read/write/device
  control and internal device control, `WdfRequestRetrieve{Input,Output}Buffer`, `WdfRequestComplete(WithInformation)`,
  `WdfRequestGetParameters`, `WdfRequestWdmGetIrp`, object contexts (`WdfObjectGetTypedContextWorker`,
  `WdfObjectAllocateContext`), `WdfObjectDelete` with cleanup/destroy callbacks and parent/child deletion,
  `WdfSpinLock*`, `WdfWaitLock*`, driver unload (`EvtDriverUnload`).
* Not implemented: PnP/power (there is no PnP manager, so `EvtDriverDeviceAdd` is never called: a PnP KMDF driver
  loads but creates nothing and a `FIXME` says so), manual queues, I/O targets, timers, DPCs, work items, DMA, USB,
  interrupts, memory objects, registry/string objects, WDF verifier.
* `tests/drivers/OurKmdfDevice.c` is a real KMDF driver built against Microsoft's public headers; `kmdf_stub.c` plays
  `WdfDriverEntry.lib`. The structure layouts the shim reads are compared with the real headers by `layout_check`.

Revisit if a target driver needs PnP semantics: the next step would be a minimal PnP manager that builds a PDO and
calls `AddDevice`/`IRP_MN_START_DEVICE`, then lifting the shim's per-object classes (`WDevice`, `WQueue`, ...) onto it.

## Integrating with the dispatcher

Two supported ways; both use the same functions (`include/win10xbox/drivers.h`):

1. **Interposition (works today, no dispatcher edits)**: `drivers::Initialize()` (default
   `Options::install_dispatcher_hooks = true`) calls `dispatch::Initialize()` and replaces `Entry::impl` of
   `NtCreateFile`, `NtOpenFile`, `NtClose`, `NtDeviceIoControlFile`, `NtFsControlFile`, `NtReadFile`, `NtWriteFile`,
   `NtFlushBuffersFile`, `NtQueryVolumeInformationFile`, `NtOpenSymbolicLinkObject`, `NtQuerySymbolicLinkObject`
   with wrappers that serve driver paths/handles and chain to the previous implementation (`impl`, or the host
   export) for everything else. Idempotent; `RemoveDispatcherHooks()` restores the table. `process::Launch` calls
   `dispatch::Initialize()` again, which is a no-op.
2. **Native wiring** (what the dispatcher owner will want long term): call from the translators
   ```cpp
   if (drivers::OwnsPath(attr))                 return drivers::Drv_NtCreateFile(...);   // also NtOpenFile
   if (drivers::OwnsHandle(h))                  return drivers::Drv_NtClose(h);          // NtClose must be first
   if (drivers::OwnsHandle(file))               return drivers::Drv_NtDeviceIoControlFile(...);  // Fs, Read, Write,
                                                                                                  // FlushBuffers, QueryVolumeInformationFile
   if (drivers::OwnsSymbolicLinkPath(attr))     return drivers::Drv_NtOpenSymbolicLinkObject(...);
   if (drivers::OwnsHandle(h))                  return drivers::Drv_NtQuerySymbolicLinkObject(...);
   ```
   and set `Options::install_dispatcher_hooks = false`. The `Drv_*` functions have exactly the syscall signatures.

Host lifecycle:
```cpp
win10xbox::Initialize(cfg);
drivers::Initialize();                                    // pool, namespace, worker thread, hooks
drivers::LoadDriver(L"C:\\...\\Windows\\System32\\drivers\\x.sys", nullptr, &info);   // host path
process::Launch(...);                                     // guest runs; it can now open \\.\X
drivers::Shutdown();                                      // IRP_MJ_SHUTDOWN, close handles, DriverUnload, join worker
```
`tests/drivers/drv_host.cpp` is this sequence (it is what `integration.driver_ioctl` runs).
Caveat: `process::Launch` initialises the registry, clock, VFS and token after `dispatch::Initialize()`. A driver whose
`DriverEntry` reads the (emulated) registry or files must therefore be loaded *after* those are up - either from inside
`Launch` just before it enters the guest, or by letting `LoadDriver` run on first use. The clock self-initialises, so
time queries in `DriverEntry` are fine; the test drivers do not touch the registry in `DriverEntry`.

## Interface to the privileged-instruction handler (separate module)

The driver host does not decode or emulate privileged instructions. What that module can rely on / should know:

* `drivers::GetVirtualIrql()` / `SetVirtualIrql()` are the values to back `mov rax, cr8` / `mov cr8, rax`. Note that the
  WDK (and the mingw ddk) headers compile `KeGetCurrentIrql`, `KeRaiseIrql`, `KeLowerIrql` on x64 into **inline `cr8`
  accesses**, so any driver that calls them traps; the exported functions (`KfRaiseIrql`, ...) exist too.
  `OurKmdfDevice.c` avoids `KeGetCurrentIrql` for this reason.
* `drivers::SetImageLoadHook(hook, ctx)` runs after imports are bound and before final page protections (pages still
  writable) and before `DriverEntry`: the place to patch/arm a driver image.
* Other inline accesses real drivers make, all outside this module: `KeGetCurrentProcessorNumber()` (`gs:[0x184]`),
  `PsGetCurrentThread()`/`KeGetCurrentThread()` inline variants (`gs:[0x188]`), KPCR, `KeTickCount`/`SharedUserData`
  at `0xFFFFF78000000000`, MSR/port I/O. The `READ_PORT_*`/`WRITE_PORT_*`/`READ_REGISTER_*` HAL exports are loud stubs for
  the same reason.
* `drivers::SetBugCheckHandler` lets a test observe `KeBugCheckEx`; the default traces and terminates the process.

## Tests

| ctest | what | needs |
|-------|------|-------|
| `drivers.unit` (label `drivers`) | 27 cases: loads the real test drivers and drives them through `Drv_*` (all four buffering methods, pending/DPC/timer/ISR completion, async handle, cancel on close, read/write, open/close semantics, symbolic link objects, unload/leak checks, loud stubs, rollback of a failing `DriverEntry`, dispatcher chain, printf family, pool, waits, registry helpers, KMDF driver on the shim, WDF stub naming) | nothing |
| `drivers.layout` (label `drivers`, mingw) | `ntk.h` and the WDF shim types vs the ddk / KMDF headers | mingw ddk headers |
| `integration.driver_ioctl` (label `driver_e2e`) | a guest process through `win10xbox` machinery: `CreateFile("\\.\OurTestDevice")`, `DeviceIoControl` in every method, pending completion from a DPC, kernel timer, overlapped I/O, 50 in-kernel `kmtest`-style assertions | Wine builtins + NLS data (SKIP 77 otherwise) |
| `integration.driver_kmdf_ioctl` (label `driver_e2e`) | same for the KMDF driver | as above |

```sh
cmake -S . -B build-drv -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake
cmake --build build-drv
WINEDEBUG=-all WINEPREFIX=$HOME/.wine-drv ctest --test-dir build-drv -L driver --output-on-failure
```
For the end-to-end tests stage the NLS data first, exactly as in `docs/TESTING.md` (they SKIP when it is missing).
`WIN10XBOX_DEBUG=+driver` shows every device, IRP, DbgPrint and stub call. `tests/drivers/kmtest.h` is a minimal
ReactOS-kmtest-style harness (`ok`, `ok_eq_*`, `ok_irql`) used inside `OurTestDevice.sys`; it is written from scratch and
only mirrors the macro names.

Test drivers are freestanding C built against `ntk.h` and an import library generated from
`tests/drivers/ntoskrnl_imports.def` (dlltool; `lib.exe` for MSVC): `unit.drivers` asserts every name in it is
implemented. The KMDF driver compiles against the mingw ddk headers plus Microsoft's public KMDF 1.33 headers
(`kmdf_min.h` orders them for clang; five include-case mismatches are bridged by generated forwarders).

## Known differences from a real kernel

* DPCs/timers/work items/ISRs run on one worker thread, asynchronously (see above); there is one virtual processor.
* No paging, no verifier, no access checks (`IoCreateDeviceSecure` SDDL is ignored), no PnP, no power, no physical memory:
  `MmGetPhysicalAddress` returns the virtual address, `MmMapIoSpace` returns NULL.
* Pool memory is always zeroed; `ExAllocatePool*` ignores pool type apart from the paged-at-DISPATCH check.
* User-mode buffers are plain pointers (same address space): `METHOD_NEITHER`/direct I/O work on the caller's memory,
  `ProbeFor*` checks mappings, a bad pointer in a driver is an access violation in the host.
* A crash in a driver (access violation, unhandled exception) propagates as a host exception; there is no bug check
  screen. `KeBugCheck*` terminates the host unless a handler is installed.
* `Drv_*` only handles the requests listed above; other file syscalls on a driver handle go to the host and fail.
