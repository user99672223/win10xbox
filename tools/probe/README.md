# Xbox Dev Mode Capability Probe

A read-only, non-destructive probe harness that enumerates what an AppContainer
app can and cannot do, run across three environments and diffed. The gap
between **(1) plain win32 baseline** and **(3) Xbox Series S in Dev Mode** is
the design input for a Windows-10 compatibility layer that will run on top of
whatever the Xbox sandbox allows.

This repo is the deliverable; it is **not** that compat layer.

## Layout

```
probe-core/        shared static library (raw Win32 + ntdll via GetProcAddress)
  probe.h            frozen contract: Record, Status, Reporter.run(), helpers
  reporter.cpp       JSON serializer, incremental flush, SEH + try/catch wrap
  seh.cpp            __try/__except trampoline (compiled /EHa, no C++ unwind inside)
  env.cpp            environment metadata capture (OS build, device, arch)
  http.cpp           WinHTTP report POST (optional)
  core_run.cpp       module dispatch table
  probe_A_dynamic_code.cpp     dynamic code exec, SEC_IMAGE, VirtualAlloc2
  probe_B_dll_inventory.cpp    System32 + LoadLibrary + export sweep
  probe_B_ntdll_stubs.cpp      raw bytes of 20 Nt* syscall stubs
  probe_C_syscalls.cpp         NT syscall resolve + safe call
  probe_D_veh_trap.cpp         VEH, access violation, RDMSR #GP, Get/SetThreadContext
  probe_E_cpu_identity.cpp     CPUID, RDTSC, hypervisor-present
  probe_F_filesystem.cpp       read/write matrix across standard paths
  probe_G_registry.cpp         hive read/write
  probe_H_token.cpp            TokenIntegrity, AppContainer SID, capabilities
  probe_I_graphics.cpp         D3D11/12 + d3d9on12/d3d11on12/GLOn12/Dozen bridges
  probe_J_audio.cpp            WASAPI + XAudio2 init
  probe_K_network.cpp          Winsock, outbound, inbound, loopback
  probe_L_processes.cpp        CreateProcess, multi-instance, AppModel
  probe_M_system.cpp           memory, procfeatures, SMBIOS, ACPI
  probe_N_winrt.cpp            Windows.* activation sweep (33 classes)
  probe_payload.cpp            tiny DLL used by A for out-of-package LoadLibrary

win32-host/        desktop baseline (environment 1): console .exe
  main.cpp, CMakeLists.txt

uwp-host/          UWP host (environments 2 & 3): C++/WinRT CoreApplication
  main.cpp                   D2D/DWrite scrolling renderer on CoreWindow
  CMakeLists.txt             store CRT, WindowsApp.lib, /APPCONTAINER PE flag
  Package.appxmanifest       capabilities incl. codeGeneration, broadFileSystemAccess
  package.cmd                stage + makepri + makeappx + signtool
  Assets/                    tile PNGs (placeholders)

tools/
  ccheck.cmd                 per-module /c compile check (parallel-author safe)
  install_laptop_uwp.ps1     cert + Add-AppxPackage (admin, one-time)
  deploy_xbox.ps1            Device Portal REST deploy to Xbox
  diff_reports.py            per-key matrix + regression summary across runs

docs/
  schema.md                  JSON schema (xbox-probe/1) and status legend

results/
  baseline-win32.json
  laptop-uwp.json
  xbox-uwp.json              (added by deploy_xbox.ps1)
  matrix.md                  (produced by diff_reports.py)
```

## Prerequisites

- Visual Studio 2022 with **"Desktop development with C++"** (Native Desktop).
  The UWP VS workload is **not** required - everything packages via
  `makeappx`/`makepri`/`signtool` from the Windows 10 SDK (10.0.26100.0 here).
- CMake >= 3.20.
- Windows 10/11 x64 (laptop) and an Xbox Series S or X in **Dev Mode** for the
  console target.

## Build

```powershell
# From the repo root in a developer command prompt (or any shell that has
# cmake on PATH; CMake invokes the MSVC x64 toolchain itself).

# 1) Baseline (environment 1) - plain win32 console exe.
cmake -S win32-host -B build-win32 -A x64
cmake --build build-win32 --config Debug

# 2) UWP (environments 2 and 3) - the same .msix for both.
cmake -S uwp-host    -B build-uwp   -A x64
cmake --build build-uwp   --config Debug
.\uwp-host\package.cmd Debug
# produces: uwp-host\AppPackages\xbox-probe-uwp.msix (+ xbox-probe.cer dev cert)
```

## Run

### Environment 1: baseline (win32)

```powershell
# Writes build-win32\Debug\probe-report.json next to the exe.
$env:PROBE_ENVIRONMENT = "baseline-win32"
.\build-win32\Debug\xbox-probe-win32.exe
copy .\build-win32\Debug\probe-report.json .\results\baseline-win32.json
```

### Environment 2: laptop UWP (AppContainer, same sandbox as Xbox except for the device policy)

```powershell
# One-time (requires admin): trust the self-signed cert and install the msix.
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install_laptop_uwp.ps1

# Launch, wait, pull the report.
explorer.exe shell:AppsFolder\XboxDevModeProbe_eqad6j9zhtw80!App
# The probe writes probe-report.json to its LocalState incrementally;
# the host also prints each record to the on-screen scrolling view and to the
# debugger. Give it ~5s on a laptop.
copy "$env:LOCALAPPDATA\Packages\XboxDevModeProbe_eqad6j9zhtw80\LocalState\probe-report.json" `
     .\results\laptop-uwp.json
```

### Environment 3: Xbox Series S / X Dev Mode

**One-time console setup**

1. Install the "Dev Mode Activation" app from the Microsoft Store and run it;
   follow the activation steps (requires a free Partner Center registration).
2. Reboot into Dev Mode. The Dev Home dashboard appears.
3. In Dev Home: set a Device Portal username + password; note the
   console's IP (also shown there). Enable "Remote access".

**Deploy and fetch the report**

```powershell
# Uploads the .cer (installs to the Xbox's trusted roots), uploads and installs
# the .msix via Device Portal, launches the app, then pulls probe-report.json
# out of the app's LocalState back to results\xbox-uwp.json. You will be
# prompted once for the Device Portal credentials.
.\tools\deploy_xbox.ps1 `
  -Console 192.168.1.42 `
  -Msix .\uwp-host\AppPackages\xbox-probe-uwp.msix `
  -Cert .\uwp-host\xbox-probe.cer
```

You can also pull the file manually from a browser at
`https://<console-ip>:11443` → File Explorer → Apps → *XboxDevModeProbe* →
LocalState → `probe-report.json`.

## Diff: the capability matrix

```powershell
python -I .\tools\diff_reports.py `
  .\results\baseline-win32.json `
  .\results\laptop-uwp.json `
  .\results\xbox-uwp.json `
  --only-changed --format md --out .\results\matrix.md
```

The output has:

- a per-key table showing status (`OK`, `DENIED`, `MISSING`, `NOTIMPL`,
  `CRASH`, `INFO`, `WARN`, `SKIPPED`) in each environment;
- a per-environment status count;
- a **regressions vs baseline** section grouped by category - these are the
  capabilities present in a desktop that disappear in AppContainer or on Xbox.
  That list is the architectural input for the compat layer.

See `docs/schema.md` for the JSON schema and status legend.

## Early results (laptop baseline vs laptop AppContainer)

243 probe records; the diff produced 11 regressions, all in categories
F (filesystem), G (registry), and M (firmware tables). **Zero regressions
in A (dynamic code execution) or D (VEH / trap-and-emulate).** The loader
and driver-host designs stand on this.

## Attribution

Modules were authored in parallel by Claude Opus / Sonnet / Haiku against a
single frozen contract (`probe-core/probe.h`); each author owned exactly one
`.cpp`, re-verified by the shared `ccheck` script before integration.
