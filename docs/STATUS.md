# Status

## Probe results folded in (2026-10-21)

The capability probe ran on an Xbox Series S in Dev Mode (build 26100, Game
bucket). All four load-bearing capabilities are confirmed: RWX + execute,
`NtCreateSection(SEC_IMAGE)` mapping, VEH trap-and-emulate (`rdmsr` `#GP` →
RIP adjust → continue), and canonical ntdll syscall stubs. `docs/DECISIONS.md`
is updated with evidence links; the probe-settled defaults are now in
`core/include/win10xbox/config.h` (real child processes, SEC_IMAGE mapping,
dispatch-by-name mandatory on build 26100, inbound sockets allowed, 3.5 GiB
guest ceiling, the D18 single CPU-identity story). Full report:
`tools/probe/results/REPORT.md`. Open items routed to probe-v2 and M6
(child-process resume, feature-level 11_0 vs 12_1, GLOn12/Dozen/Vulkan
bundling, WoW64 far jump).

## Console run of host-uwp-lite v1.0.0.4 (source `e7f25de`)

The throwaway validator `host-uwp-lite` ran on an Xbox Series S in SystemOS Dev Mode, OS build 26100.9445. Its build
`v1.0.0.4` is `uwp-lite` run #4 on `e7f25de`. The kernel32/kernelbase forwarder fix (`eb52c50`) is confirmed on hardware:
0 of 92 `KERNEL32.dll` imports are unresolved (the first run had all 92 dangling), `kernelbase.dll` loads as its own
module, the guest entry is reached, and the guest CRT runs through the full NLS/codepage initialisation (`l_intl`,
`c_1252`, `c_437`, `sortdefault`, `normnfc` all resolve) and writes its locale registry values.

The run then stopped at a separate defect whose cause is not yet established: an `EXCEPTION_ACCESS_VIOLATION` reading
address `0x4` at RIP `0x7ff6cb75a36c`. The faulting instruction is `mov eax,[rdx+rax]` with `rdx=4` and `rax=0`: a 32-bit
load off a near-null base, the shape a corrupted thread-local-storage pointer produces. It is not in any guest module.

**The faulting image is NOT identified, and two attributions have already been made and withdrawn.** The console report
gave the surrounding image as "base ~`0x7ff6cb758000`, size ~`0x14b000`", but `crash.txt`'s `AnnotateAddress` falls back
to `VirtualQuery` and prints a *protection region* rather than a module base whenever the address is not in the guest's
loader list — and while the guest runs, that list holds only guest modules. So that pair is a region inside some image and
any RVA derived from it is meaningless. Checked and ruled out: it is not a Wine builtin (none of the 23 in
`wine-pe/out/builtin` carries the faulting bytes `8b 04 02` at the implied offset, and Wine's `ntdll.dll` is
`SizeOfImage=0x302000`, far larger than `0x14b000`); and it is not the host exe at that offset either
(`win10xbox-uwp-lite.exe` from the `.msix` that actually crashed has `SizeOfImage=0x24f000`, `.text` VirtualSize
`0x1736c8`, and `cc cc cc` padding at that offset). The console and its crash files are no longer accessible, so this
report cannot be resolved further; the real-Windows CI reproduction below is the route to an unambiguous answer. The
lesson for the tooling: a crash record must print every loaded module's base **and** `SizeOfImage`, or an absolute RIP
cannot be attributed at all.

**Leading hypothesis (UNPROVEN at time of writing): a guest/host collision over `TEB.TlsSlots`.** `TEB.TlsSlots`
(TEB+0x1480, `core/dispatch/internal.h:64`) is an *inline* 64-entry array in the TEB, so unlike
`ThreadLocalStoragePointer` and `FlsData` — which `SwapIn`/`SwapOut` swap per universe
(`core/dispatch/dispatcher.cpp:99-110`, and see `rtl_forward.inc:108-110` for FLS) — it cannot be given the guest and
the host separate copies. Our `ThreadState*` lives in exactly that array, at the index `TlsAlloc()` returns in
`dispatch::Initialize()` (`dispatcher.cpp:299-301`). Wine's `loader_init` then claims index
`NTDLL_TLS_ERRNO` = 16 *by fiat* with `RtlSetBits`, and reserves index 0
(`wine-pe/wine-src/dlls/ntdll/loader.c:4454-4455`; constant at `dlls/ntdll/ntdll_misc.h:36`) — it asserts those indices
rather than allocating them. If our index is one of those, the guest's `_errno()` writes straight over our
`ThreadState*`, and the entry trampoline trusts any non-NULL value it finds there
(`core/asm/syscall_entry.asm:33-36`), after which `SwapIn` loads a bogus `host_tls`/`host_fls` out of guest data and
writes them into the TEB — so the next host TLS access faults on a tiny address, which is the observed instruction shape.

This predicts all three observations: real Windows with the Wine builtins dies; real Windows with the runner's genuine
System32 DLLs runs to completion, because Wine's `loader_init` never runs and nothing claims index 16
(`.github/workflows/ci.yml:141-143`, and the comment at `:151-153`); and Wine-on-Linux is green, because there the host
ntdll *is* Wine's ntdll, so index 16 was already reserved before our `TlsAlloc` ran and it can never be handed to us.
The decisive test is cheap and not yet done: log the allocated index. `dispatcher.cpp:300-303` currently logs only when
the slot is *unusable*, so no CI log or console trace records which index we actually got. If it reports 16, this is
confirmed; if it reports an index well away from 0 and 16, the hypothesis is wrong.

No `crash.dmp` was written. The dump path reported HRESULT `0x800736b2`, the form `crash.txt` uses when `dbghelp.dll`
cannot be bound inside the AppContainer (to be confirmed against `crash.txt`). `dbghelp.dll` is delay-loaded, so the
package still ran, and `crash.txt` and `entry-reached.txt` were produced: the first on-console run to leave crash
evidence (crash capture was added in `e7f25de`, see `host-uwp-lite/README.md`).

A similar failure shows up on real Windows. `host-win32-smoke` run `39667248899` on `e7f25de` fails its gate: its
console-conditions run and its desktop sanity run both report `KERNEL32 unresolved: 0` and exit `139` (128+11, a SIGSEGV
as Git Bash reports it; the gate expects 42). It is believed to be the same defect, but the fault address on real Windows
is not recorded, so that link is not confirmed.

## Milestone commits

Annotated tags `m0`/`m1`/`m2` are created in the repo; if a tag is not visible
on the remote it is because this session's git credentials pushed branches but
were refused tag refs (HTTP 403) — re-push the tags with full credentials.
Milestone commits, so they stay pinpointable regardless:

| Tag | Commit | Milestone |
|-----|--------|-----------|
| m0  | `cbcfbab` | repo skeleton, submodules, core + host-win32 hello, CI |
| m1  | `fc18910` | loader (PE map, stub rewrite, scanner) — green CI |
| m2  | `fc18910` | dispatcher + subsystems (VFS/registry/clock/token/NLS) — green CI |
| m3  | `9e22180` | Wine PE set, winexbox interface + headless backend, Wine conformance baseline — green CI |
| m4  | `692037b` | driver host (user-mode ntoskrnl, object/IRP engine, KMDF shim) + privileged-instruction trap-and-emulate — green CI on both toolchains. The M4 content landed in `a8c4074`, whose `ci` run #19 was red; `692037b` is the first commit where CI went green with it, so that is the milestone point. |
| m5  | `7aac27a` | real UWP host (`host-uwp/`): app shell, Host, standalone build, signed `.msix` built by CI — green CI, not yet run on hardware |


## M5 - real UWP host (`host-uwp/`)

`host-uwp/` is the real win10xbox UWP host: a headless C++/WinRT `CoreApplication` shell (`main.cpp`, `App.cpp`, `App.idl`)
around the Host of `host.h`. The Host reads `LocalState\launch.json`, stages the machine folder in `LocalState\machine`,
calls `Initialize`, runs the configured guest through `process::Launch` on a worker thread, and writes its verdict to
`LocalState\status.txt`. Its evidence trail is plain files in `LocalState` (`trace.log`, `stdout.txt`, `crash.txt`,
`crash.dmp`, `entry-reached.txt`). It is a standalone CMake project: it `add_subdirectory()`s `third_party/`, `core/` and
`winexbox/` itself, and the top-level tree refuses it (`host-uwp/CMakeLists.txt:41` raises `FATAL_ERROR` if it is added
there), so `-DWIN10XBOX_BUILD_UWP=ON` is not a way to build it.

Commit `4db0789` added the app shell and build scaffolding as a WIP checkpoint marked unverified: `App.idl`, `App.cpp`,
`App.h`, `main.cpp`, `host_shell.cpp`/`.h`, `CMakeLists.txt`, `Package.appxmanifest`, `pch.h`, `sdk.cmake`, `stage.cmake`,
`package.cmd`, `crash.cpp`/`.h` (carried over from host-uwp-lite), `guest/hello.c`, `payload/launch.json`,
`payload/launch.example.json` and `.gitattributes`. The Host sources (`host.cpp`, `host_diag.cpp`, `host_stage.cpp`,
`host_util.cpp`, `launch_config.cpp` and `selftest/`, with their headers) came in the earlier WIP checkpoints `d223773` and
`295fae4`. Commit `7aac27a` added `.github/workflows/host-uwp.yml`. The package identity is `win10xbox.uwp` (exe
`win10xbox-uwp.exe`, WinRT namespace `Win10XboxUwp`), so it installs side by side with host-uwp-lite. See
`host-uwp/README.md`.

CI: workflow `host-uwp` run #1 (run `39681539555`) on `7aac27a` completed `success` on its first attempt. Configure, build,
package, the portable launch.json selftest and the artifact upload all passed. Artifact `host-uwp-msix` (21,794,185 bytes,
about 20.8 MiB) holds the signed `.msix` and the public `.cer` needed to trust it for side-loading. It is not yet run on
hardware. No tag is made for m5 here (tags cannot be pushed from this session); the SHA in the table is the record.

## M4 - driver host (`drivers/`)

`drivers/` loads kernel-mode drivers in user mode and routes guest device I/O to them. A WDM driver
(`OurTestDevice.sys`, freestanding C against `drivers/include/ntk/ntk.h`) is mapped by `core/loader`, its imports are
bound to a ~400-function user-mode ntoskrnl, `DriverEntry` builds `\Device\OurTestDevice` + `\??\OurTestDevice`, and a
guest `CreateFile`/`DeviceIoControl` (buffered, direct, neither, pending-from-DPC, timer, overlapped) travels
Wine ntdll -> dispatcher -> `Drv_NtDeviceIoControlFile` -> IRP -> `IoCallDriver` -> driver -> `IoCompleteRequest` -> output
copy-back, verified end to end under Wine (`integration.driver_ioctl`). KMDF drivers built with Microsoft's public
headers run on a minimal WDF function-table shim (`integration.driver_kmdf_ioctl`). `drivers.unit` has 27 cases.
Green on both CI jobs (commit `52d39be`): the whole `drivers/` tree + the freestanding test drivers build under
**MSVC** (windows-latest, Debug and RelWithDebInfo) and **llvm-mingw**; `drivers.unit` + `drivers.layout` gate on
both, and the IOCTL round-trip (`integration.driver_ioctl`, `integration.driver_kmdf_ioctl`, label `driver_e2e`)
is a hard gate on the Wine job and runs non-blocking on the Windows job (it asserts on guest stdout, the same
real-Windows capture gap as `guest_e2e`).
Privileged-instruction trap-and-emulate landed (`drivers/host/privileged.cpp`, commit `a8c4074`): a VEH armed via
`drivers::SetImageLoadHook` that, for a fault inside a loaded driver image, decodes with Zydis and emulates the
instructions privileged at CPL 3 — `mov cr8` <-> virtual IRQL (the inline `KeGetCurrentIrql`/`KeRaiseIrql`/
`KeLowerIrql`), `rdmsr`/`wrmsr`, `in`/`out`, `cli`/`sti`, `rdpmc`, `hlt`/`wbinvd`/`invd`/`invlpg` — then resumes.
`OurTestDevice`'s `IOCTL_OTD_PRIV` exercises it from kernel mode; verified under Wine (`cr8=1 msr=1 portio=1
clisti=1`), the same `#GP`->VEH->resume path the probe proved on the console. Gates on linux-cross, non-blocking on
the Windows job. Follow-ups (`drivers/README.md`): the kernel-VA `KUSER_SHARED_DATA` mirror read and gs-relative
KPCR fields — memory accesses needing the virtual KUSER/KPCR served, not a one-instruction emulation. Also not
done: PnP/power, real KMDF framework.

## M0 - repo skeleton (tag `m0`)

Layout created per `docs/ARCHITECTURE.md`; the capability probe moved to
`tools/probe/` untouched. Eleven third-party projects pinned as shallow
submodules, Wine and ReactOS as pinned-tag fetches. CMake builds `core/`
(static lib with config + tracing), `host-win32` ("hello"), and the unit/
integration test harness with both MSVC and llvm-mingw. CI: `linux-cross`
(llvm-mingw + Wine smoke) and `windows` (MSVC Debug/RelWithDebInfo, sccache,
machine folder populated from the runner's System32 at test time).

## M1 - loader (tag `m1`)

`core/loader` manually maps x64 PE images (sections, relocations, protections,
host `.pdata` registration), resolves exports/forwarders, rewrites the guest
ntdll's syscall thunks to enter our dispatcher, and scans executable code with
Zydis to redirect `cpuid`/`rdtsc`/`rdtscp`/`syscall` to trampolines (inline
near-jump where the displaced bytes relocate safely, `int3`+VEH otherwise).
Validated under Wine on the real Wine-built `ntdll.dll`: all 258 syscall
thunks matched and were rewritten. Guest test programs (`tests/guest`) build
with the host toolchain; `test_loader` has 21 cases. PE/loader corner cases
and the two scanner strategies are documented in `docs/LOADER.md`. SEH-based
tests are MSVC-only (clang-mingw has no `__try`), so the mingw smoke build
stays green.

## M2 - dispatcher + subsystems (tag `m2`)

`core/dispatch` routes every guest syscall through a table generated from
Wine's syscall numbers (`tools/gen_syscalls.py`, 258 entries) plus a
hand-maintained policy (`core/dispatch/syscall_policy.inc`): 163 pass-through
to the host ntdll by name, 29 translate (paths/handles/timeouts), 45 emulate,
21 todo (loud `STATUS_NOT_IMPLEMENTED`). Per-channel relay tracing logs every
call with args and NTSTATUS. `SEC_IMAGE` sections are emulated via the loader;
63 process-wide `Rtl*` routines are forwarded to the host ntdll (heap, locks,
exception/unwind - see `DECISIONS.md` E2); the guest's eight Wine unix calls
are answered. Subsystems: VFS (machine-folder path translation + native/
builtin DLL routing), registry (own bounds-checked REGF reader/writer + write
overlay over supplied hives), virtual clock (QPC/TSC/system time +
KUSER_SHARED_DATA page), token (interactive medium-integrity user identity).
`process::Launch` boots Wine's PE ntdll inside the host, loads kernelbase/
kernel32/ucrtbase from the machine folder, and runs the program.

End-to-end under Wine: `win10xbox C:\guest\guest_hello.exe` prints
`hello from guest` and exits 42; `guest_cpuid` sees `AuthenticAMD` with the
hypervisor-present bit cleared (natively the box is Intel with the bit set),
proving the scanner patches are live. `SYSCALLS.md` is generated; the
per-syscall status table is the fidelity ledger going forward.

### CI gating and real-Windows status

- The whole tree builds and links under **MSVC** (windows-latest, Debug and
  RelWithDebInfo) and under **llvm-mingw**; `unit.core` (109 cases across
  trace/vfs/registry/clock/token/loader/dispatch) is green on both.
- The guest **end-to-end** tests (`integration.guest_hello`, `guest_cpuid`:
  boot a real PE through the full layer, check output and exit code) are a
  **hard gate on the Wine job** (`linux-cross`), which downloads the Wine PE
  builtins and stages the NLS data the emulated `NtGetNlsSectionPtr` serves.
- On a **real-Windows** host the guest currently boots through the entire
  Wine-ntdll init (loader, NLS/locale, process params) and runs to
  completion with the correct exit code, but its **stdio is not yet captured**
  (Wine's kernelbase console path over a real-Windows pipe). These two tests
  therefore run **non-blocking** on the Windows CI job (visible, not gating)
  until that is resolved. This is the lead M3 bring-up item, tracked here;
  it is a real-Windows-only wiring gap, not an architectural one (the same
  path is proven on the Wine gate).
