# Design decisions — console data folded in

The capability probe in `tools/probe/` has now run on an Xbox Series S in Dev
Mode (build `10.0.26100.9445`, Game bucket). Evidence lives in
[`tools/probe/results/REPORT.md`](../tools/probe/results/REPORT.md) and
`tools/probe/results/xbox-uwp.json`. Rows below marked **Settled by probe**
cite the report section and the probe record key.

Each entry records the **current default** (what the code does), the **flag**
that selects it (all in `core/include/win10xbox/config.h`,
`win10xbox::config`), and **what would still flip it**. Probe module letters
refer to `tools/probe/probe-core/probe_*.cpp`.

Headline: all four load-bearing capabilities are confirmed on the console —
RWX + execute, `NtCreateSection(SEC_IMAGE)` mapping, VEH trap-and-emulate
(including `rdmsr` `#GP` → adjust RIP → continue), and canonical ntdll syscall
stubs. See report §6.

| # | Fork | Default | Flag | Status / evidence |
|---|------|---------|------|-------------------|
| D1 | Process model: real child processes vs in-process virtual processes | **Real child processes.** The console's Game bucket creates children of `cmd.exe`, `dllhost.exe` and the app's own exe. In-process virtual processes become the fallback for programs that must not fork. M3 `services.exe`/`CreateProcess` paths are designed for real children. | `config::process_model = ProcessModel::RealChild` | **Settled by probe** ([§5.12](../tools/probe/results/REPORT.md), `L/createprocess-self`, `L/createprocess-cmd`, `L/createprocess-dllhost`). Caveat: children were created **suspended** only; resume is a probe-v2 item. |
| D2 | Image mapping: `NtCreateSection(SEC_IMAGE)` (kernel maps + relocates; we patch the CoW copy) vs manual map | **SEC_IMAGE-assisted.** The kernel maps and relocates; we do import redirection and stub/CPUID patching on the copy-on-write pages. Manual mapping (MemoryModule-derived) stays as the fallback. | `config::image_mapping = ImageMapping::SecImage` | **Settled by probe** ([§5.1](../tools/probe/results/REPORT.md), `A/sec-image-map`). Retrofit into the M1 loader only if cheap; otherwise scheduled for M6. |
| D3 | Which Nt* calls pass through to the host kernel | **Dispatch-by-name is mandatory, not optional.** The console is build 26100 (Windows 11 24H2-era kernel); a 22H2 guest ntdll's syscall *numbers* do not match the host's, so every pass-through resolves the host export **by name**, never by number. Per-syscall policy in `docs/SYSCALLS.md`. | `syscall_policy.inc`; `config::syscall_override[]` | **Settled by probe** ([§2](../tools/probe/results/REPORT.md) build id; `C/*` all OK by name). |
| D4 | Syscall stub rewriting vs real `syscall` with our number table | **Rewrite** every stub to jump into the dispatcher; the console stubs are the canonical x64 `mov r10,rcx; mov eax,id; test [7FFE0308h],1; syscall` shape, `SystemCall` mode 0 (no `int 2e`). | `config::stub_strategy = StubStrategy::Rewrite` | **Settled by probe** ([§5.2](../tools/probe/results/REPORT.md), `B/stub:*`). |
| D5 | Memory budget per guest | **Guest ceiling 3.5 GiB, configurable.** ~5 GiB physical is visible to the Game-bucket process on this build; treat as a per-build value, not a constant. GPU budget via `QueryVideoMemoryInfo` is untested (probe-v2). | `config::memory_budget_bytes = 3.5 GiB` | **Settled by probe** ([§2](../tools/probe/results/REPORT.md), `M/memory-status`; §8 note 4). |
| D6 | 32-bit code (Heaven's-Gate far jump, WoW64-like) vs no 32-bit | **No 32-bit support**; 32-bit PEs rejected at load. | `config::wow64 = Wow64::Off` | Open. The probe did not test a 32-bit far jump or `ProcessWow64Information`; probe-v2 adds it. Deferred past M5 regardless. |
| D7 | Inbound sockets | **Allowed** (inbound listen, loopback, raw, UDP all succeed in the Game bucket). No inbound workaround needed. | `config::inbound_sockets = true` | **Settled by probe** ([§5.11](../tools/probe/results/REPORT.md), `K/tcp-listen-inbound`, `K/loopback-connect`, `K/socket-raw`). Non-loopback bind was not explicitly tested but Dev Mode networking grants server sockets. |
| D8 | Display backend | **Headless off-screen backend** now (`winexbox/headless`); the real D3D12/CoreWindow backend is M6. Graphics plan set by the probe: `d3d9.dll`, `opengl32.dll`, `gdiplus.dll`, `vulkan-1.dll` are **absent**; `d3d9on12.dll` present but its `d3d9` runtime is missing. Plan: bundle Mesa GLOn12 (`opengl32.dll`), Dozen + a Khronos `vulkan-1` loader; `gdiplus` from the user's 22H2 copy or Wine builtin; `d3d9` = user's 22H2 `d3d9.dll` over system `d3d9on12` (test), fallback Microsoft's open-source D3D9On12. | `config::display_backend = DisplayBackend::Headless` | **Settled by probe** ([§5.9](../tools/probe/results/REPORT.md), `I/*`). **Open item:** D3D11 and D3D12 both report feature level **11_0** in this sandbox (laptop: 12_1); probe-v2 adds a `CheckFeatureSupport` sweep. |
| D9 | Virtual clock time source | **Host QPC** scaled/offset; `rdtsc` patched to the virtual clock. | `config::clock_source = ClockSource::HostQpc` | Probe shows `rdtsc` runs free (~1.745 GHz) and the hypervisor bit is clear ([§2](../tools/probe/results/REPORT.md)), but invariant-TSC was not confirmed, so the default stays HostQpc. |
| D10 | CPUID handling | **Patch** every `cpuid` site. On the console, leaf-1 bit 31 is already 0 but leaf `0x40000000` returns `Microsoft Xb` — which must be hidden — and the brand string reads `Microsoft Xbox Series S CPU`, which must be rewritten. See D18 for the single identity story. | `config::patch_cpuid = true` | **Settled by probe** ([§2](../tools/probe/results/REPORT.md), `E/cpuid.*`). |
| D11 | Dynamic code: RWX vs write-then-protect | **Write-then-protect** default; RWX is also permitted on the console, so `JitPolicy::Rwx` is available for JITs that insist on it. | `config::jit_policy = JitPolicy::WriteThenProtect` | **Settled by probe** ([§5.1](../tools/probe/results/REPORT.md), `A/valloc-rwx`, `A/exec-wx-flip`). |
| D12 | VEH trap-and-emulate for privileged instructions | **VEH** first-handler, Zydis-decoded, continue with modified context. | `config::trap_engine = TrapEngine::Veh` | **Settled by probe** ([§5.4](../tools/probe/results/REPORT.md), `D/veh-privileged-insn`: `rdmsr` `#GP` delivered to VEH, RIP advanced, continued). |
| D13 | Registry backend | **Own REGF reader/writer over supplied hives** with an in-memory overlay; all guest writes land in the overlay (the console denies every real write). Reads of HKLM SOFTWARE/SYSTEM and HKU DEFAULT succeed on the console, so a `HostReadThrough` fallback can fill gaps. | `config::registry_backend = RegistryBackend::Regf` | **Settled by probe** ([§5.7](../tools/probe/results/REPORT.md), `G/read:*` OK, `G/write:*` DENIED). (Flag renamed from `Hivex`; see M2 — we ship our own REGF code, not hivex.) |
| D14 | Guest DLL source | **Native Microsoft DLLs** from the machine folder; **builtin** Wine PE DLLs for anything missing/overridden. | `machine.json: "dll_overrides"`, default `native,builtin` | Not probe-gated, but see D17: the guest DLL set must be a **static manifest** — the console denies directory enumeration of System32. |
| D15 | Where the UWP host puts the machine folder | **`ApplicationData::Current().LocalFolder()/machine`** (the only writable location besides TEMP). | `config::machine_root` | **Settled by probe** ([§5.6](../tools/probe/results/REPORT.md), `F/*`: only LocalState and TEMP writable). |
| D16 | Driver host: in-process vs separate instance | **In-process** `ntoskrnl.exe` DLL; `\\.\Device` opens routed to the IRP engine. | `config::driver_host = DriverHost::InProcess` | Real children work (D1), so a separate driver instance is now *possible*; the in-process default stays until M4 shows a reason to split. |
| D17 | Guest DLL discovery | **Static manifest.** The console allows opening individual System32 files by name but **denies directory listing**, so the loader never enumerates — it consults `machine/guest_dlls.txt`. | n/a (loader behavior) | **Settled by probe** ([§5.6](../tools/probe/results/REPORT.md), `F/read:system32` MISSING; `A/map-system-dll-as-data` OK). |
| D18 | CPU identity (single source of truth) | **One `machine_identity` config** consumed everywhere. The guest must see brand `AMD 4700S 8-Core Processor` (same Zen 2 silicon; family 23 / model 136 / stepping 1 matches the console), leaf `0x40000000` all zeros, and RDRAND **advertised** (absent on the console; emulated via `#UD` trap → `BCryptGenRandom`). The same name is served by (a) the CPUID patch, (b) the registry overlay `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\*\ProcessorNameString`, (c) a synthetic SMBIOS type-4 table (firmware tables are DENIED, so `GetSystemFirmwareTable` is emulated), and (d) wbemprox `Win32_Processor`. | `config::machine_identity` | **Settled by probe** ([§2](../tools/probe/results/REPORT.md) CPU id, `E/*`; [§5.13](../tools/probe/results/REPORT.md) `M/firmware-*` DENIED). Implemented in M2 if the version/clock spoof is still open, else as an M3 task. |

Token note (subsystem, not a flag): the console token is Low integrity /
AppContainer with 2 privileges ([§5.8](../tools/probe/results/REPORT.md),
`H/*`); the token subsystem presents a normal Medium-integrity user, and
`Global\` named objects go through our object manager. Registry overlay design
(D13) stands.

## Probe-v2 queue (console-run later by the owner; built into `tools/probe`)

Status: specified here; module implementation is **deferred**. The content of
item 3 (a 32-bit far-jump / privileged-instruction detection probe) repeatedly
tripped the model content classifier during automated authoring, so these
modules are left for a careful later pass or for the owner to add directly.
The specification below is complete.

New probe modules to ship in the same `.msix`:

1. Resume a created child process and confirm it runs (D1 caveat).
2. Launch an EXE from LocalState (root-cause `A/loadlibrary-out-of-package`).
3. 32-bit code-segment far jump (Heaven's Gate) for future WoW64 (D6).
4. `QueryVideoMemoryInfo` for the GPU budget (D5).
5. D3D12 `CheckFeatureSupport` sweep for 12_x features (D8 open item).

## UWP host (M5) lessons from the probe

- `D3D11CreateDevice` blocks the UI thread in a Game-bucket app — create
  devices on a worker thread after `CoreWindow` activation.
- The Xbox launcher refuses an exe without matching WinRT metadata, silently —
  copy `tools/probe/uwp-host`'s `.vcxproj` + `App.idl` + C++/WinRT setup as the
  `host-uwp` template.

## How to use this file

- When a probe result lands, update the row, flip the flag default if needed,
  and cite the probe record key that justified it.
- New forks discovered during development get a new row, never a silent
  branch in code.

## Engineering decisions (not console-gated)

These are settled by engineering facts, not by the probe, but are recorded
here because they shape everything above.

| # | Decision | Choice | Why |
|---|----------|--------|-----|
| E1 | Which ntdll/kernelbase/kernel32 the guest runs on | **Wine's PE builds (builtin), always.** Native 22H2 copies of these three are accepted into the machine folder but are never loaded; `dll_overrides` for them is ignored with a warning. Every other DLL defaults to `native,builtin`. | A native ntdll cannot be initialised inside a host process that already has one: `LdrpInitializeProcess` assumes a fresh PEB (loader lists, process heap, csrss connection, KnownDlls). Native kernel32/kernelbase depend on native-ntdll internals and on csrss ALPC. Wine's ntdll is *designed* to boot on top of a foreign "unix side" through a 258-entry syscall table plus 8 unix calls (`dlls/ntdll/unixlib.h`), which is exactly the surface our dispatcher implements. Wine itself documents that native ntdll/kernel32 are unsupported. The thunks in Wine 11's x64 PE ntdll are byte-identical to Windows' (`mov r10,rcx; mov eax,id; test [7FFE0308h],1; ...`) with real Windows 10 syscall numbers, so the stub rewriter treats both identically. Fidelity-sensitive behaviour (version, user, paths) is virtualised below ntdll regardless. |
| E2 | Process-wide state shared with the host (PEB/TEB) | **Single PEB/TEB, host-owned, selectively taken over.** Wine's `loader_init` is allowed to install its loader lists (`Peb->Ldr`), TLS bitmaps (over the PEB's own bit storage, so host/guest `TlsAlloc` coordinate) and loader lock. The process heap is *not* replaced (patch `wine-pe/patches/0001-keep-host-process-heap.patch`) and all guest `Rtl*Heap*`, critical-section, SRW, condition-variable, exception-dispatch, unwind, VEH and function-table functions are **forwarded at function level (inline jmp) to the host ntdll** so that one implementation owns each process-wide structure. The list is `core/dispatch/rtl_forward.inc`. | x64 Windows pins the TEB (gs base) per thread in the kernel; a second TEB/PEB cannot be installed from user mode. Exceptions are delivered by the kernel to the *host* `KiUserExceptionDispatcher`, so unwind tables and VEH lists must be the host's. |
| E3 | Image mapping ownership | **Our mapper is the "unix side" of `NtCreateSection(SEC_IMAGE)`/`NtMapViewOfSection`.** Wine's `loader.c` keeps import/apiset/forwarder/delay-load/TLS/DllMain logic. `\KnownDlls` is reported absent so Wine falls back to file search through the VFS, which applies `dll_overrides` (native vs builtin directories). | Avoids re-implementing a PE loader we already have in Wine, while keeping control of mapping (needed to rewrite stubs and scan code before pages go executable). |
| E4 | Guest threads | Each guest thread is a host thread. `NtCreateThreadEx` (guest) is translated: the host thread starts in a wrapper that marks the thread as guest, extends static TLS, then enters Wine's `LdrInitializeThunk` per-thread path. Host-created threads (WinRT, D3D) never run guest code. | Keeps scheduling, waits and APCs in the real kernel. |
| E5 | Win32k (win32u) | **Wine's win32u ported as a builtin PE whose "unix side" (window manager, GDI, message queues) is compiled into our emulation layer**, with the relevant wineserver object model (`server/window.c`, `queue.c`, `class.c`, `hook.c`, `clipboard.c`, `region.c`) ported in-process under a single server lock. Draft interface in `winexbox/`; the port itself starts in M3 with a headless backend. | `win32u.dll` in Wine is thunks only; the implementation lives in `win32u.so` + wineserver. There is no other open implementation that native user32 can sit on. |
