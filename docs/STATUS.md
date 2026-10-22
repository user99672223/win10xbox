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

### A real bug, found and fixed — but NOT the cause of the real-Windows exit-139: recursion in `core/clock/clock.cpp`

> **Correction.** Commit `b97f7f0` is titled as though this fixed the real-Windows exit-139 crash. It did not.
> `host-win32-smoke` run #3 on `b97f7f0` still fails with exit 139. The bug below is genuine, reproduced, and now
> covered by a gating test, but it is a *Wine-host* defect: on real Windows the guest never reaches a `Sleep` at all
> (see the next section). The commit title stands in history; this is the correction.

`Clk_NtDelayExecution` resolved its "host" `NtDelayExecution` through a **lazy** `GetModuleHandleW(L"ntdll.dll")` inside
a function-local static (`clock.cpp:243-245`, called from `:502`). The first call therefore happened on the guest's
first `Sleep` — i.e. *after* the PEB takeover, when `PEB->Ldr` is Wine's — so it resolved the **guest** ntdll's
`NtDelayExecution`, which is a stub we rewrote to jump into the dispatcher. `Clk_NtDelayExecution` then called itself
until the stack ran out. Reproduced under Wine with the fix reverted: exit **139** plus
`err:virtual:virtual_setup_exception stack overflow 1792 bytes`.

**The real differential is which guest program runs, not which DLL set is staged.** `host-uwp-lite/smoke_guest.c:233`
calls `Sleep(20)`; no `guest_e2e` guest sleeps at all. So `host-win32-smoke` (which runs `smoke_cui.exe`) dies, while
linux-cross and the `ci.yml` Windows end-to-end step (which run `guest_hello`/`guest_cpuid`) pass. `integration.guest_sleep`
plus `tests/guest/sleep.c` now cover this as a **gating** linux-cross test, and it fails with exit 139 if the fix is
reverted. Fixed by latching the host ntdll in `clock::Initialize`, before the takeover; the same hazard in
`core/loader/internal.cpp` was latched too, and `dispatcher.cpp`'s `HostNtdll()` now errors if first resolved after
`Initialize`.

### Three claims about `TEB.TlsSlots` made here earlier, now WITHDRAWN

They were investigated because they are a genuine hazard class, but none of them caused this crash:

1. *"Wine's `loader_init` re-initialises the TLS bitmap, destroying the record that our index is in use."* **False.**
   `RtlInitializeBitMap` sets only `SizeOfBitMap` and `Buffer` and does not clear the buffer
   (`wine-src/dlls/ntdll/rtlbitmap.c:73-78`), and the buffer it is given is `peb->TlsBitmapBits` — storage *inside* the
   PEB that Microsoft's ntdll backs `PEB.TlsBitmap` with too (`loader.c:4450`). The allocators therefore share the bits.
   Proven on the real takeover path, not argued: `guest TLS bitmap 0x17009D3E8 (bits 0x7FFD0080, PEB.TlsBitmapBits
   0x7FFD0080, 64 slots): slot 19 already in use, allocators share the PEB bits`. This is already recorded as
   intentional in `docs/DECISIONS.md:97` (E2).
2. *"Wine claims and writes `TlsSlots[NTDLL_TLS_ERRNO]` = 16, so it clobbers our `ThreadState*`."* The three source
   lines are real (`loader.c:4455`, `ntdll_misc.h:36`, `thread.c:445` returning
   `(int *)&NtCurrentTeb()->TlsSlots[16]`), **but nothing in our configuration reaches that write**: no shipped builtin
   imports `ntdll._errno` (all 23 import tables scanned — `msvcrt`/`ucrtbase` export their own), and Wine's ntdll has no
   internal caller. Forcing index 16 under Wine (`WIN10XBOX_TEST_TLS_INDEX=!16`) runs to completion, exit 42, slot intact.
3. *"With the runner's genuine System32 DLLs, Wine's `loader_init` never runs."* **False.**
   `core/dispatch/process.cpp:288-299` always loads the guest ntdll from `<machine>\Windows\builtin` and fails if it is
   absent, while `import_windows_dlls.ps1` stages into `Windows\System32`, which the guest ntdll is never taken from.
   `loader_init` runs identically in both configurations.

The `TEB.TlsSlots` work was nevertheless kept, as **hardening and diagnostics rather than a fix**: `TlsSlots` really is
an inline TEB array that cannot be swapped per universe the way `ThreadLocalStoragePointer` and `FlsData` are
(`dispatcher.cpp:99-110`), so the slot is now taken at an index ≥ 19 (never 0 or 16), reserved in the guest's bitmap,
stored **tagged** so the pointer is validated arithmetically with no memory access before any dereference, backed by a
`ThreadState::magic` cookie, and repaired-and-reported if ever clobbered. The chosen index is now always traced on the
`dispatch` channel. `integration.guest_tls_slot_clobber` simulates the 4-byte store through `&TEB.TlsSlots[i]` and fails
if the tag check is disabled, confirming the clobber leaves a **non-NULL** survivor
(`FE7D7FFFFE7DF380` → `FE7D7FFF00000000`) that the old null check would have waved through.

**Process note, worth keeping.** The `clock.cpp` recursion had already been reported by an earlier subagent, and was
wrongly demoted here on the grounds that the CI log showed no `NtDelayExecution` relay lines after guest entry — absence
of evidence in a log truncated by the crash itself. Two successive misattributions of the console fault followed. The
corrective that worked was reproducing the failure locally under Wine rather than reasoning further from the report.

### Root cause: MSVC's thread-safe static guard reads TLS under the guest's TLS view (REPRODUCED)

This is the live bug, and it is now reproducible on CI without the console. `host-win32-smoke` run #3 on `b97f7f0`
(i.e. *with* the clock fix) still fails. From its `smoke-console.log` artifact, the trace ends:

```
NtContinue(000000b2732ff310,00000001) (may not return)
NoteGuestEntry guest entry reached (RtlUserThreadStart at 00000001700568B4)
DumpGuestModules [0] 0000000140000000 C:\guest\smoke_cui.exe
DumpGuestModules [1] 0000000170000000 C:\windows\system32\ntdll.dll
DumpGuestModules [2] 0000000178000000 C:\windows\system32\kernel32.dll
DumpGuestModules [3] 0000000174000000 C:\windows\system32\kernelbase.dll
NoteGuestEntry guest entry: TEB.TlsSlots[19] still holds our ThreadState
```

…and stops. What that establishes:

* **Zero syscalls after `NtContinue`.** The guest faults within its first instructions of real execution, before
  issuing a single syscall. So it is neither the `Sleep` path nor dispatcher recursion.
* Imports and module loading are **fine**: 0 unresolved, all four modules present at their expected bases.
* `TEB.TlsSlots[19]` is **intact**, so the TlsSlots collision theory is dead on real Windows too, not just under Wine.
* It fails **identically** with and without `WIN10XBOX_TEST_NO_HOST_PATHS=1`, so it is not AppContainer-specific.
* Under Wine the same path is **green** (`guest_e2e` 8/8). Real-Windows-only.

**This is very probably the same defect as the Xbox access violation**, which also showed no syscalls after guest entry
and a load off a near-null base (`mov eax,[rdx+rax]`, `rdx=4`, `rax=0`). Treat the console report and the smoke failure
as one bug until evidence separates them — which consolidates two unreproducible reports into one CI-reproducible one.

**The cause, reproduced** by building the real tree MSVC-ABI with clang-cl and running the smoke guest under Wine,
which reproduces the CI trace exactly:

* **Fault site:** `win10xbox::dispatch::detail::S()` — the function-local `static State s;` at
  `core/dispatch/dispatcher.cpp:69` — reached via `Host()` (`dispatcher.cpp:218`) from `Xl_NtContinue`
  (`core/dispatch/nt_process.cpp:289`).
* **Faulting bytes:** `48 8b 0c ca` then `3b 81 04 00 00 00`, with `rcx=0`: `rcx = TlsArray[tls_index]` is NULL, then
  `cmp eax,[rcx+4]`.
* **Mechanism:** MSVC's thread-safe function-local-static guard reads `_Init_thread_epoch`, which lives at **offset 4
  of the module's TLS block**, through `gs:[0x58]` (`TEB.ThreadLocalStoragePointer`). `Xl_NtContinue` calls
  `LeaveToGuestView()` *first*, installing the **guest's** TLS array — which has no block for the host exe. The guard
  read therefore hits `NULL+4`.
* **Why Wine/mingw is green:** mingw uses `__cxa_guard`, which touches no TLS at all. That one difference accounts for
  the entire real-Windows-only divergence, and for why our gating job could never see it.
* **The console AV is the same bug:** `mov eax,[rdx+rax]` with `rdx=4`, `rax=0` is the same guard sequence in MSVC
  codegen. Two unreproducible reports collapse into one CI-reproducible defect.
* **Confirmed by construction:** the same tree rebuilt with only `/Zc:threadSafeInit-` runs the guest to exit 42.
* The same ordering exists at `Xl_NtContinueEx`, `Xl_NtRaiseException` and `Xl_NtSetContextThread`, which also call
  `Host()` right after `LeaveToGuestView()`.

**The hazard is general, so the fix must close the class, not the instances.** Any host code reached while the guest's
TLS view is installed that touches a function-local static — or any other TLS — faults under MSVC, with a near-null
load that is very hard to attribute. Four call sites today; an unbounded number otherwise. The deeper problem is that
mingw *cannot* reproduce it, so the gating linux-cross job is structurally blind to this class; an invariant check that
fires under Wine (host state touched while the guest view is active is a bug by construction) is worth more than the
fix itself.

**Confirmed on the real `cl`/`link` toolchain**, `host-win32-smoke` on `a24b57a` (windows-latest). The crash record now
attributes the fault instead of leaving it unattributable:

```
at 0x00007ff674e8976c  = host module win10xbox.exe+0x3976c
access violation: reading 0x0000000000000104  = <unattributed; null pointer + 0x104>
bytes at rip-8 .. rip+16: 04 01 00 00 48 8b 04 c8 | 8b 04 02 39 05 bf c7 28 00 7f 0c ...
```

`48 8b 04 c8` is `mov rax,[rax+rcx*8]` — the TLS-array index — immediately followed by `8b 04 02`
(`mov eax,[rdx+rax]`), which is **byte-for-byte the instruction the console reported**. Symbolized from the uploaded
MSVC map: `S()+0x1c` (fault) ← `Host()+0x9` ← `CallWithArgs()+0x15d` ← `win10xbox_dispatch+0x345`, under
`Xl_NtContinue()+0x26`. The faulting read is `0x104` here and `0x4` on the console — the same mechanism at a different
TLS-block offset, so the exact offset is not significant.

The precise ordering, `core/dispatch/nt_process.cpp:289-295`:

```c
static NTSTATUS NTAPI Xl_NtContinue(PCONTEXT ctx, BOOLEAN alert) {
    NoteGuestEntry(ctx);                          // 290 — completes here, still the HOST view
    std::uint32_t d = LeaveToGuestView();         // 291 — installs the GUEST's TLS array
    NTSTATUS s = Host().NtContinue(ctx, alert);   // 292 — Host() → S() → guard → FAULT
```

`NoteGuestEntry` also appears in the stack scan, and it is instrumentation added in `eb52c50`, so it was checked as a
possible self-inflicted cause: it is **not**. It returns before the swap, so its return address is merely left on the
stack — the raw-x64-stack-scan caveat. The faulting ordering is 291 then 292 and predates all of this work. The same
`LeaveToGuestView()` → `Host()` ordering is at `Xl_NtContinueEx` (`:297-303`), `Xl_NtRaiseException` (`:305-309`) and
`Xl_NtSetContextThread`.

### FIXED, and the guest now runs to completion on real Windows

Fixed in `532a3bf` by closing the class rather than the four call sites: the dispatcher's process state is
`constinit State g_state;` at namespace scope (no guard, no TLS, and `constinit` makes the compiler reject any future
member that would reintroduce dynamic initialisation), and `CallInGuestView(Host().NtX, ...)` makes the host-view
resolution happen *by language rule* — arguments are sequenced before the call — rather than by the author remembering
an ordering. `/Zc:threadSafeInit-` was rejected on the merits: `Threads()`, `Objects()`, `ExportCache()` and the three
`trace` singletons are first touched from guest threads created at arbitrary times, so dropping the guard there is a
data race, and being process-wide it would mask the next instance instead of revealing it.

**Result on real Windows** (`host-win32-smoke` on `532a3bf`, windows-latest), in order from `smoke-console.log`:

```
hello from guest
guest: command line 'C:\guest\smoke_cui.exe'
trace:process:GuestExit guest exit, code 42
trace:process:RestorePebFields host PEB fields restored
trace:relay:Shutdown statistics: 188 calls to 34 syscalls
trace:core:Shutdown shutdown
```

98 relay lines after guest entry, where previously there were **zero**. An unmodified Windows x64 PE is loaded, its
imports bound, run to completion returning its own exit code, and the layer torn down cleanly — on real Windows, not
under Wine. That is the project's core claim, demonstrated.

**The hazard class is now gated where it was previously invisible.** mingw uses `__cxa_guard` and touches no TLS, so
linux-cross can never reproduce the fault itself — that structural blindness, not the single bug, is what let this
reach the console. `LeaveToGuestView` now sets `kThreadGuestView`, and `GuestViewActive()` reports the window open only
while that flag is set **and** `ThreadState::depth == 0` (that second condition is load-bearing: keying on the flag
alone left it set for the thread's life, because `NtContinue` never returns, and silently suppressed 645 trace lines).
`NoteHostStateUnderGuestView()` covers `dispatch::S`, `ExportCache`, `Objects`, `Threads`, `process::G`,
`dispatch::Views`, `dispatch::CpuPolicy` and a probe `trace::Log` consults, so even a stray `TRACE` in the window is
dropped and counted rather than faulting. Verified gating by reinstating the original ordering: `integration.guest_view_clean`
then **fails** on linux-cross with `err:dispatch: host state reached under the guest TLS view 1 times (last:
dispatch::S (Host, Lookup, ...))`. Suite is 21/21, `guest_e2e` 10/10 (21 tests, from 14).

### OPEN: the host's own exit code is 127, not the guest's 42

`host-win32-smoke` is still red, but for a different and much narrower reason: after the clean shutdown above, the main
thread raises an **unhandled `0xc000000d` (`STATUS_INVALID_PARAMETER`)** at `ntdll.dll+0x7e55f`, with
`combase`/`KERNELBASE`/`RPCRT4` on the stack and `runs guest code: no`, and `win10xbox.exe` exits **127** instead of
42. The bytes at RIP are `eb 00` right after an `e8` call, and `0x00000080c000000d` sits on the stack, which looks like
`RtlRaiseStatus` machinery rather than a memory fault. The step also took ~3 minutes, consistent with
`MiniDumpWriteDump` running now that dbghelp is available (it was refused inside the console AppContainer).

This is a **host exit-path** defect, not a layer defect, and it is newly *reachable* rather than newly broken — before
`532a3bf` the guest never got far enough to reach teardown. Note that 127 needs its own explanation: Git Bash reports a
Windows status modulo 256, and `0xC000000D mod 256` is 13, not 127.

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
