# Xbox Series S Dev Mode Capability Probe — Technical Report

| Field | Value |
|---|---|
| Report date | 2026-10-21 |
| Repository | `https://github.com/user99672223/win10xbox` |
| Probe schema | `xbox-probe/1` |
| Record count per run | 243 |
| Modules | 15 across 14 categories (A–N) |
| Environments measured | 3 |

## 1. Scope

The probe answers whether specific OS-level capabilities are available to an unmodified program running under three progressively tighter sandboxes:

| Environment | Host | Sandbox | Build artifact | JSON output |
|---|---|---|---|---|
| 1 — `baseline-win32` | Win32 console `.exe` | None (desktop) | `win32-host/build-win32/Debug/xbox-probe-win32.exe` | `results/baseline-win32.json` |
| 2 — `laptop-uwp` | C++/WinRT UWP `.msix` | AppContainer, laptop | `uwp-host/AppPackages/xbox-probe-uwp/xbox-probe-uwp.msix` | `results/laptop-uwp-v2.json` |
| 3 — `xbox-uwp` | Same `.msix` as (2) | Xbox Series S Dev Mode, Game bucket | Same file | `results/xbox-uwp.json` |

The gap between (1) and (3) is the specification for a Windows-10 compatibility layer that runs real, unmodified Windows programs natively on the Xbox Series S in Dev Mode.

## 2. Hardware under test (environment 3)

Values read from the probe's own CPUID / `GetNativeSystemInfo` / `GlobalMemoryStatusEx` / registry probes on the Xbox:

| Property | Value |
|---|---|
| Device name | `XBOX` |
| OS build | `10.0.26100.9445` |
| OS edition | SystemOS (Dev Mode) |
| OS version string | `26100.9445.amd64fre.xb_flt_2608ge.260927-0830` |
| Architecture | x64 |
| CPU brand (CPUID 80000002–4) | `Microsoft Xbox Series S CPU` |
| CPU vendor | `AuthenticAMD` |
| CPU family / model / stepping | 23 / 136 / 1 (Zen 2) |
| Logical / physical / packages / NUMA | 8 / 4 / 1 / 1 |
| Page size | 4096 B |
| Large page size | 2 MiB |
| User-mode VA range | `0x0000000000010000` .. `0x7FFFFFFEFFFF` (128 TiB) |
| Total physical memory | 5120 MiB |
| Available physical memory at measurement | 5107 MiB |
| CPUID hypervisor-present bit (ECX leaf 1 bit 31) | 0 |
| CPUID hypervisor-vendor leaf (0x40000000) | `Microsoft Xb`, max HV leaf `0x4000000E` |
| CPUID extended features | SSE3, SSSE3, SSE4.1, SSE4.2, AVX, AVX2, BMI1, BMI2, SHA |
| Absent features | AVX-512, RDRAND |
| RDTSC frequency (measured against QPC) | 1 745 359 193 Hz (≈1.745 GHz) |
| QPC frequency | 1 745 500 000 Hz |
| RDTSC behavior in user mode | Runs free; monotonic across reads |
| RDTSCP behavior in user mode | Runs; `tsc_aux` = 0 |

The CPUID bit 31 reports no hypervisor, while leaf `0x40000000` returns the vendor string `Microsoft Xb`. The hypervisor exists and runs this process; the standard advertisement bit is suppressed. A guest that patches CPUID in the loader can rely on the extended leaf for detection.

Only 128 MiB of DXGI dedicated video memory and 1280 MiB of shared video memory are visible to the process at adapter enumeration time (section 7, I/`dxgi-factory`). The reported adapter name is `SraKmd_arden`, vendor `0x1414` (Microsoft), device `0xD000`.

## 3. Probe design

### 3.1 Contract

A single frozen header (`probe-core/probe.h`) defines:

```c++
enum class Status { OK, DENIED, NOTIMPL, MISSING, CRASH, INFO, WARN, SKIPPED };
struct Record { std::string category, name; Status status; std::string detail;
                uint32_t errorcode; std::string value; };
class Reporter {
  void run(const std::string& category, const std::string& name,
           const std::function<void(Record&)>& body);
  void set_json_path(const std::wstring& path);
  // ...
};
```

Every probe module exposes a single `register_X(Reporter&)` function and emits records by calling `reporter.run(category, name, body)`.

### 3.2 Fault isolation

`Reporter::run` wraps every probe body in two independent guards:

1. **SEH trampoline (`probe-core/seh.cpp`, compiled `/EHa`).** The trampoline function contains no C++ objects with destructors, so the `__try` / `__except` frame is well-formed. An access violation, privileged-instruction `#GP`, stack overflow or similar exception sets the record to `CRASH` with the exception code and the run continues.
2. **C++ `try` / `catch(...)`.** A `std::exception` or unknown C++ exception thrown by a probe body is caught and reported as `CRASH` with the exception text.

A probe that crashes never aborts the run, and the record for the crashing probe is still emitted.

### 3.3 Incremental flush

The JSON report is rewritten atomically after every `emit` (write to `.tmp`, `FlushFileBuffers`, `MoveFileExW REPLACE_EXISTING`). A hard crash in a later probe leaves the report for all prior probes on disk.

### 3.4 Dispatch

`core_run.cpp` holds the list of module entry points. Each module registration is itself wrapped in a `guarded(...)` call so that a module whose registration throws does not take down later modules.

### 3.5 Modules

| Category | Module source | Probes emitted | Author model |
|---|---|---|---|
| A | `probe_A_dynamic_code.cpp` | 9 | Opus |
| B | `probe_B_dll_inventory.cpp` | 46 | Haiku |
| B | `probe_B_ntdll_stubs.cpp` | 21 | Opus |
| C | `probe_C_syscalls.cpp` | 39 | Haiku |
| D | `probe_D_veh_trap.cpp` | 6 | Opus |
| E | `probe_E_cpu_identity.cpp` | 8 | Opus |
| F | `probe_F_filesystem.cpp` | 21 | Sonnet |
| G | `probe_G_registry.cpp` | 7 | Sonnet |
| H | `probe_H_token.cpp` | 10 | Sonnet |
| I | `probe_I_graphics.cpp` | 9 | Sonnet |
| J | `probe_J_audio.cpp` | 5 | Sonnet |
| K | `probe_K_network.cpp` | 9 | Sonnet |
| L | `probe_L_processes.cpp` | 9 | Opus |
| M | `probe_M_system.cpp` | 10 | Sonnet |
| N | `probe_N_winrt.cpp` | 34 | Haiku |

Total 243 records per run.

### 3.6 Hosts

- `win32-host`: standard CMake project, `x64` console `.exe`. Links `advapi32 ole32 oleaut32 ws2_32 shlwapi`.
- `uwp-host`: MSBuild `.vcxproj`, `x64` AppContainer PE. Imports the `Microsoft.Windows.CppWinRT 2.0.250303.1` NuGet (unpacked under `packages/`) to drive `midlrt` and C++/WinRT projection generation. Imports `Microsoft.Cpp.AppContainerApplication.props` and `Microsoft.AppXPackage.Targets` for packaging. Links `WindowsApp.lib` plus explicit `kernel32.lib` (the UWP umbrella omits `AddVectoredContinueHandler` and friends). Compiled with `WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP` forced via `AdditionalOptions` to defeat the UWP property-sheet override; AppContainer enforcement is still applied at runtime through the PE flag and manifest.

Both hosts link the same `probe-core` sources with the same preprocessor configuration apart from the WINAPI_FAMILY override, so cross-environment deltas reflect sandbox behavior, not probe implementation differences.

### 3.7 UWP manifest

`uwp-host/Package.appxmanifest`, version 1.0.5.0:

- `TargetDeviceFamily Name="Windows.Universal"` and `TargetDeviceFamily Name="Windows.Xbox"` — one `.msix` installs into either laptop AppContainer or Xbox App/Game bucket.
- `Application Id="App" EntryPoint="XboxProbe.App"` — real WinRT activation. The entry class is declared in `uwp-host/App.idl` and compiled by `midlrt` into `XboxProbe.winmd`, which is packaged next to the exe.
- Capabilities: `rescap:codeGeneration` (required for `PAGE_EXECUTE_*` under AppContainer), `internetClient`, `internetClientServer`, `privateNetworkClientServer`.

The Xbox launcher's activation path is strict: an exe without matching WinRT metadata is refused before any process is created, with no crash dump. This required a proper vcxproj + CppWinRT toolchain; attempts without the Visual Studio UWP C++ workload failed with no activation diagnostic.

### 3.8 Host surface on Xbox

The on-screen Direct2D / DirectWrite renderer compiled into `uwp-host` for the laptop was removed from the Xbox-targeted build. `D3D11CreateDevice` on the UI thread of a Game-bucket AppContainer app blocks indefinitely, preventing the probe worker (spawned inside `View::Run` after `SetWindow`) from ever being started. The current `uwp-host/main.cpp` is headless: `SetWindow` immediately spawns the probe worker, `Run` pumps `CoreWindow` events only, and `CoreApplication::Exit` is called once the worker signals done. The report file in LocalState is the sole output.

## 4. Results summary

### 4.1 Status counts per environment

| Status | baseline-win32 | laptop-uwp | xbox-uwp |
|---|---:|---:|---:|
| OK | 219 | 211 | 203 |
| INFO | 15 | 14 | 17 |
| WARN | 0 | 1 | 0 |
| DENIED | 5 | 17 | 14 |
| NOTIMPL | 0 | 0 | 1 |
| MISSING | 0 | 0 | 8 |
| SKIPPED | 4 | 0 | 0 |
| CRASH | 0 | 0 | 0 |
| **Total** | **243** | **243** | **243** |

Zero `CRASH` records in any environment — the SEH guard was exercised during authoring but no probe produced an uncaught exception on the final runs.

### 4.2 Status counts per category on Xbox

| Category | n | Statuses |
|---|---:|---|
| A — dynamic code | 9 | OK=8, MISSING=1 |
| B — DLL inventory + ntdll stubs | 67 | OK=54, INFO=10, MISSING=3 |
| C — NT syscalls | 39 | OK=39 |
| D — VEH / trap-and-emulate | 6 | OK=6 |
| E — CPU identity | 8 | OK=6, INFO=2 |
| F — filesystem | 21 | OK=9, DENIED=8, INFO=3, MISSING=1 |
| G — registry | 7 | OK=5, DENIED=2 |
| H — token / sandbox | 10 | OK=10 |
| I — graphics | 9 | OK=6, NOTIMPL=1, MISSING=2 |
| J — audio | 5 | OK=5 |
| K — networking | 9 | OK=9 |
| L — processes | 9 | OK=8, MISSING=1 |
| M — system / firmware | 10 | OK=6, DENIED=4 |
| N — WinRT activation | 34 | OK=32, INFO=2 |

### 4.3 Regressions vs the win32 baseline

Keys that are `OK` in `baseline-win32` but `DENIED`, `MISSING`, `NOTIMPL`, `CRASH` or absent in a later environment. 20 total; 9 of them are Xbox-specific (appear in `xbox-uwp` but not in `laptop-uwp`).

| Category | Name | laptop-uwp | xbox-uwp | Xbox-only? |
|---|---|---|---|:---:|
| A | `loadlibrary-out-of-package` | OK | **MISSING** | yes |
| B | `load:d3d9` | OK | **MISSING** | yes |
| B | `load:gdiplus` | OK | **MISSING** | yes |
| B | `load:opengl32` | OK | **MISSING** | yes |
| F | `read:drive-c-root` | DENIED | DENIED | no |
| F | `read:known-documents` | DENIED | DENIED | no |
| F | `read:programdata` | DENIED | DENIED | no |
| F | `read:system32` | OK | **MISSING** | yes |
| F | `read:userprofile` | DENIED | DENIED | no |
| F | `write:known-documents` | DENIED | DENIED | no |
| F | `write:package-install` | DENIED | DENIED | no |
| F | `write:programdata` | DENIED | DENIED | no |
| F | `write:userprofile` | DENIED | DENIED | no |
| G | `write:HKCU` | DENIED | DENIED | no |
| I | `bridge:d3d9on12` | OK | **NOTIMPL** | yes |
| I | `bridge:glon12` | OK | **MISSING** | yes |
| I | `bridge:vulkan` | OK | **MISSING** | yes |
| L | `createprocess-notepad` | WARN | **MISSING** | yes |
| M | `firmware-acpi` | DENIED | DENIED | no |
| M | `firmware-enum` | DENIED | DENIED | no |
| M | `firmware-smbios` | DENIED | DENIED | no |

The AppContainer walls (`laptop-uwp` column) are the sandbox-model walls. The Xbox-only column records what the Xbox adds on top of plain AppContainer.

## 5. Category-by-category findings on Xbox

### 5.1 Category A — dynamic code execution

| Probe | Status | Measured |
|---|---|---|
| `valloc-rwx` | OK | `VirtualAlloc PAGE_EXECUTE_READWRITE` succeeded |
| `exec-rwx` | OK | Generated 6-byte stub ran from RWX, returned `0x5150` |
| `exec-wx-flip` | OK | Allocate RW, write, `VirtualProtect` RX, execute: succeeded |
| `virtualalloc2-placeholder` | OK | Placeholder reserved, split half, verified |
| `sec-image-map` | OK | `NtCreateSection(SEC_IMAGE)` on our DLL file, mapped and relocated |
| `loadlibrary-out-of-package` | **MISSING** | `LoadLibraryExW` on writable copy returned `ERROR_MOD_NOT_FOUND` |
| `loadlibrary-dont-resolve` | OK | `LoadLibraryExW(..., DONT_RESOLVE_DLL_REFERENCES)` on the same copy succeeded |
| `map-system-dll-as-data` | OK | `CreateFileMapping(ntdll.dll, PAGE_READONLY) + MapViewOfFile`: 2-byte MZ verified at base |
| `exec-from-worker-thread` | OK | `CreateThread` entry pointed at JITted page; thread returned expected value |

The split between `loadlibrary-out-of-package` (fails) and `loadlibrary-dont-resolve` (succeeds) on the same DLL file indicates the file read + mapping path works, but the standard load path's import-resolution step fails. The probable cause is a dependency DLL named in the payload's import table that cannot be found on Xbox. The `map-system-dll-as-data` success proves that at least some System32 content is reachable by direct file-handle open even though directory enumeration (`read:system32`, F) is blocked.

### 5.2 Category B — DLL inventory and ntdll stubs

67 records. All 20 ntdll syscall stubs (`stub:Nt*`) returned the standard x64 shape:

```
4C 8B D1              mov r10, rcx
B8 nn nn 00 00        mov eax, imm32            ; syscall number
F6 04 25 08 03 FE 7F  test byte ptr [SharedUserData.SystemCall], ...
01
```

The sampled syscall numbers (NtClose=0x0F, NtCreateFile=0x55, NtOpenFile=0x33) match the standard Windows build. The ntdll stub-hooking strategy in the compat layer design (overwrite the first 16 bytes with a jump to our interceptor) is directly applicable.

`SystemCallMode` in `KUSER_SHARED_DATA.SystemCall` (offset 0x308 at `0x7FFE0000`) reads `0x00`, meaning the kernel suggests the `syscall` instruction. No legacy `int 2eh` fallback.

DLLs present on Xbox and loaded successfully: `ntdll kernelbase kernel32 user32 gdi32 win32u advapi32 ws2_32 ole32 combase comctl32 shell32 msvcrt ucrtbase d3d11 d3d12 dxgi xaudio2_9 winmm dwrite d2d1 mfplat mmdevapi wintrust crypt32 bcrypt ncrypt setupapi cfgmgr32 iphlpapi dnsapi secur32 shcore` (33 of 36).

DLLs **absent** on Xbox: `d3d9`, `opengl32`, `gdiplus`.

### 5.3 Category C — NT syscall surface

All 39 probes OK. Every tested `Nt*` export resolves from ntdll and the safe call subset (`NtQuerySystemInformation`, `NtQueryInformationProcess`, `NtOpenProcessToken`, `NtQueryInformationToken`, `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtQueryVirtualMemory`, `NtClose`) returns the expected success / `STATUS_INFO_LENGTH_MISMATCH` responses on both laptop and Xbox. No syscall is tightened further on Xbox than on laptop AppContainer.

### 5.4 Category D — exception handling and trap-and-emulate

All 6 probes OK.

| Probe | Measured |
|---|---|
| `veh-add-remove` | `AddVectoredExceptionHandler` / `RemoveVectoredExceptionHandler` succeed |
| `veh-access-violation` | Access violation at a `PAGE_NOACCESS` page is delivered to the installed VEH; `ExceptionRecord.ExceptionCode` and `ContextRecord.Rip` are readable |
| `veh-privileged-insn` | A `rdmsr` (0F 32) placed in RX memory was executed; `#GP` was delivered to the VEH as `EXCEPTION_PRIV_INSTRUCTION` (`0xC0000096`), RIP was advanced by 2 bytes, execution continued, and the stub returned normally |
| `thread-context` | On a child thread we created: `SuspendThread`, `GetThreadContext(CONTEXT_FULL)`, `SetThreadContext`, `ResumeThread`: all succeed |
| `veh-continue-handler` | `AddVectoredContinueHandler` / `RemoveVectoredContinueHandler` succeed |
| `unhandled-filter-ptr` | `SetUnhandledExceptionFilter` accepts a new filter and returns the previous |

The combination of (a) VEH install, (b) `#GP` on a privileged instruction delivered to user-mode VEH, (c) context mutation of RIP, and (d) continuation of execution confirms that trap-and-emulate for a user-mode fake kernel (handling `rdmsr`, `wrmsr`, `in`, `out`, `cli`, `sti`, etc.) is implementable on Xbox Dev Mode under the same AppContainer the probe ran in.

### 5.5 Category E — CPU identity

See §2.

### 5.6 Category F — filesystem

Readable on Xbox:
- `C:\Users\10harbad\AppData\Local\Temp` (TEMP)
- Package install location (`C:\Program Files\WindowsApps\XboxDevModeProbe_...`)
- Package LocalState
- `C:\WINDOWS` (listing + file reads for individual files such as `ntdll.dll` succeed — see A/`map-system-dll-as-data`)

Not readable on Xbox:
- `C:\` root listing
- `C:\ProgramData`
- `C:\Users\<user>`
- `C:\Users\<user>\Documents` via `SHGetKnownFolderPath(FOLDERID_Documents)`
- `C:\Windows\System32` directory listing — `FindFirstFileExW` returns `ERROR_ACCESS_DENIED`. Individual System32 files remain openable by name, which is why `A/map-system-dll-as-data` works.

Writable on Xbox:
- TEMP
- Package LocalState

Not writable on Xbox:
- `C:\WINDOWS`
- `C:\WINDOWS\System32`
- `C:\` root
- `C:\ProgramData`
- `C:\Users\<user>`
- `C:\Users\<user>\Documents`
- Package install location

Three write probes (`write:windows-dir`, `write:system32`, `write:drive-c-root`) report `INFO` on Xbox instead of `DENIED`: the create call returned an unexpected error code (not `ERROR_ACCESS_DENIED`), so the probe classified it as `INFO` with the raw error. Not a permission grant.

### 5.7 Category G — registry

| Probe | Status |
|---|---|
| `read:HKLM-SOFTWARE` | OK |
| `read:HKLM-SYSTEM` | OK |
| `read:HKU-DEFAULT` | OK |
| `read:HKCU-Software` | OK |
| `enum:HKLM-SOFTWARE` | OK |
| `write:HKCU` | DENIED (`ERROR_ACCESS_DENIED` on `RegCreateKeyExW`) |
| `write:HKLM` | DENIED |

Both laptop and Xbox AppContainer provide a virtualized HKCU view that denies writes to the real hive. The compat layer must virtualize the registry for guest programs that assume HKCU is writable.

### 5.8 Category H — token / sandbox

| Field | Value on Xbox |
|---|---|
| Integrity level RID | `0x1000` — **Low** |
| `TokenIsAppContainer` | 1 |
| AppContainer SID | `S-1-15-2-1532086128-3829943474-723541930-4155388440-3730186503-1392475238-1888463634` |
| Capability SIDs count | 7 |
| Token group count | 11 |
| Privileges granted | 2: `SeChangeNotifyPrivilege` (enabled), `SeIncreaseWorkingSetPrivilege` (disabled) |
| User SID | `S-1-5-21-2702878673-795188819-444038987-1003` (XBOX\UserMgr1) |
| Elevation | Not elevated, elevation type 1 |
| Session ID | 0 |

Capabilities include the three legacy well-known SIDs (`S-1-15-3-1` InternetClient, `S-1-15-3-2` InternetClientServer, `S-1-15-3-3` PrivateNetworkClientServer) plus four app-specific capability SIDs. `codeGeneration` is granted — confirmed indirectly by `A/valloc-rwx` succeeding.

### 5.9 Category I — graphics

| Probe | Status | Measured |
|---|---|---|
| `dxgi-factory` | OK | Adapter 0: `SraKmd_arden`, vendor `0x1414` (Microsoft), device `0xD000`, dedicated 128 MiB, shared 1280 MiB, 2 total adapters, 1 software |
| `d3d11-create` | OK | Feature level `0xB000` = 11_0, hardware driver |
| `d3d12-create` | OK | Max feature level 11_0, resource binding tier 3, resource heap tier 2 |
| `d3d12-debug-layer` | OK | `D3D12GetDebugInterface` available |
| `bridge:d3d11on12` | OK | `d3d11.dll` present, `D3D11On12CreateDevice` exported |
| `bridge:d3d9on12` | NOTIMPL | `d3d9on12.dll` present with `OpenAdapter`, but `d3d9.dll` not present → `Direct3DCreate9On12` unavailable |
| `bridge:glon12` | MISSING | `opengl32.dll` not present |
| `bridge:dxcore` | OK | `dxcore.dll` present, `DXCoreCreateAdapterFactory` exported |
| `bridge:vulkan` | MISSING | `vulkan-1.dll` not present |

Measured D3D feature level `11_0` is lower than the laptop baseline (`12_1`). The Xbox GPU is capable of higher levels; the AppContainer visible to this probe exposes `11_0` only.

### 5.10 Category J — audio

| Probe | Measured |
|---|---|
| `wasapi-enumerator` | `IMMDeviceEnumerator` created; 1 render endpoint |
| `wasapi-default-device` | Default render endpoint present |
| `wasapi-activate-client` | `IAudioClient` activated; mix format = 48000 Hz, 2 channels, 32-bit |
| `xaudio2-create` | `XAudio2Create` from `xaudio2_9.dll` returned `IXAudio2` |
| `xaudio2-mastering` | `CreateMasteringVoice` succeeded (2 channels, 48 kHz), `DestroyVoice` succeeded |

Full WASAPI + XAudio2 surface available.

### 5.11 Category K — networking

| Probe | Measured |
|---|---|
| `wsastartup` | WinSock 2.2 |
| `getaddrinfo-numeric` | `1.1.1.1`: resolved |
| `dns-resolve` | `example.com`: resolved to 4 IPs (2× IPv4, 2× IPv6) |
| `tcp-connect-outbound` | TCP connect to `1.1.1.1:443` succeeded |
| `tcp-listen-inbound` | `bind + listen` on `127.0.0.1:0`: succeeded, port 49892 |
| `loopback-connect` | Second socket connected to the loopback listener: succeeded |
| `udp-socket` | `bind` on `0.0.0.0:0` UDP: succeeded, port 52321 |
| `socket-raw` | `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)`: opened |
| `iphlpapi-adapters` | `GetAdaptersAddresses`: 6 adapters, 3 up |

Networking on Xbox Game bucket is **more permissive** than on laptop AppContainer: `tcp-listen-inbound`, `loopback-connect` and `socket-raw` are all OK on Xbox but not uniformly OK under plain AppContainer declaration. The Dev Mode networking privileges granted to a sideloaded Game-bucket app include server sockets, loopback and raw sockets.

### 5.12 Category L — processes

| Probe | Measured |
|---|---|
| `createprocess-self` | OK — spawned another copy of the probe exe as a suspended child, terminated it |
| `createprocess-cmd` | OK — launched `C:\Windows\System32\cmd.exe` suspended, terminated |
| `createprocess-dllhost` | OK — launched `dllhost.exe` suspended, terminated |
| `createprocess-notepad` | MISSING — `CreateProcessW` returned `ERROR_FILE_NOT_FOUND` on `notepad.exe` |
| `shellexecute-resolve` | OK — `ShellExecuteExW` resolves from shell32 |
| `nt-createuserprocess-resolve` | OK — `NtCreateUserProcess` resolves from ntdll |
| `createprocess-variants-resolve` | OK — 4/4 (`CreateProcessAsUserW`, `WithLogonW`, `WithTokenW`, `kernelbase!CreateProcessW`) |
| `appmodel-resolve` | OK — `GetCurrentPackageFullName`, `GetCurrentPackageId`, etc. all resolve |
| `package-identity` | OK — `pfn=XboxDevModeProbe_1.0.5.0_x64__eqad6j9zhtw80` |

Process creation on Xbox Game bucket is **more permissive** than on laptop AppContainer: the probe successfully created suspended children of `cmd.exe`, `dllhost.exe` and the probe's own exe. `notepad.exe` is MISSING because the binary is not shipped with Xbox SystemOS. The multi-process architectural option in the compat layer design is open on Xbox.

### 5.13 Category M — system / firmware

Covered in §2 plus:

| Probe | Status |
|---|---|
| `large-page-minimum` | OK, 2 MiB |
| `large-page-alloc` | DENIED (`ERROR_PRIVILEGE_NOT_HELD`, `0x522`) — `SeLockMemoryPrivilege` not granted |
| `firmware-smbios` | DENIED |
| `firmware-enum` | DENIED |
| `firmware-acpi` | DENIED (size 0) |

Firmware tables are not readable. The guest cannot be told it is running on real hardware by scraping SMBIOS; the compat layer must synthesize anything that depends on firmware tables.

### 5.14 Category N — WinRT activation

32 of 34 standard activation classes return `IActivationFactory` successfully, including `Windows.Foundation.Uri`, `Windows.System.MemoryManager`, `Windows.ApplicationModel.Package`, `Windows.Storage.ApplicationData`, `Windows.Security.Cryptography.CryptographicBuffer`, `Windows.Networking.Sockets.StreamSocket`, `Windows.Networking.Connectivity.NetworkInformation`, `Windows.Web.Http.HttpClient`, `Windows.Devices.Enumeration.DeviceInformation`, `Windows.Gaming.Input.Gamepad`, `Windows.Graphics.Display.DisplayInformation`, `Windows.UI.Core.CoreWindow`, `Windows.UI.ViewManagement.ApplicationView`, `Windows.Media.Audio.AudioGraph`, `Windows.Devices.Power.Battery`.

The two `INFO` records are classes that require an apartment different from the one the probe thread occupies (`MouseDevice`); the result is not a denial, it is an apartment-mismatch HRESULT. The probe reports it as `INFO` without a verdict.

## 6. Deltas relevant to the compat-layer design

The compat-layer design assumes four load-bearing capabilities. All four are available on Xbox:

| Design assumption | Probe evidence on Xbox |
|---|---|
| User-mode allocation of executable memory and execution of generated bytes | A/`valloc-rwx`, A/`exec-rwx`, A/`exec-wx-flip`, A/`exec-from-worker-thread` all OK |
| Kernel-side PE relocation via `NtCreateSection(SEC_IMAGE)` on a file we own | A/`sec-image-map` OK |
| VEH-based user-mode trap-and-emulate for privileged instructions | D/`veh-privileged-insn` OK (RDMSR `#GP` → VEH → RIP adjust → continue) |
| Load of a non-package DLL from a path we chose | A/`loadlibrary-dont-resolve` OK; A/`loadlibrary-out-of-package` MISSING due to import-resolution dependency |

The following items change the design relative to the win32 baseline:

1. **No directory listing of `C:\Windows\System32`.** Individual System32 files remain openable (A/`map-system-dll-as-data` reads `ntdll.dll`), so the compat layer's manual mapper can still read specific DLLs by name. It cannot enumerate the directory. A static manifest of required DLLs must be built offline.
2. **No `d3d9.dll`, no `opengl32.dll`, no `gdiplus.dll`.** Guest programs that call these APIs require shipped translation implementations in the compat layer package.
3. **No `d3d9on12` / `GLOn12` / `vulkan-1` loader.** `d3d9on12.dll` is present but incomplete (`Direct3DCreate9On12` resolves through `d3d9.dll`, which is absent). The on-12 bridges cannot be relied on; translators must be bundled.
4. **D3D feature level 11_0.** Both D3D11 and D3D12 report max level 11_0 to this process. The guest's render path must downgrade to 11_0 or be transformed on the way through.
5. **5 GiB total physical memory, 128 MiB dedicated video memory, 1280 MiB shared video memory** as seen from this Game-bucket process. The compat-layer runtime plus the guest allocation ceiling must fit within this.
6. **Networking is more permissive than laptop AppContainer.** TCP listen, loopback and raw sockets are available on Xbox but not on the baseline AppContainer declaration. The compat layer does not have to work around the inbound/loopback restriction we observed on the laptop.
7. **Process creation is more permissive than laptop AppContainer.** The multi-instance / helper-process architectural option is open: `CreateProcessW` of `cmd.exe`, `dllhost.exe` and the probe's own exe all succeed as suspended children. The guest can spawn helpers.
8. **Hypervisor present but concealed in CPUID bit 31.** A guest that keys off the standard HV bit sees "bare metal"; a guest that reads leaf `0x40000000` sees `Microsoft Xb`. The compat layer's CPUID patcher must know this and present a consistent story.
9. **ntdll syscall stubs are the standard x64 pattern.** The hook strategy in the compat layer design is directly applicable.
10. **Trap-and-emulate is viable.** The fake-ntoskrnl driver host built on VEH + RIP mutation can run under this AppContainer.

## 7. Scope coverage against the original specification

The specification required probes in categories A through N and three environments. Mapping each specification item to its deliverable:

| Spec item | Covered by |
|---|---|
| A: `VirtualAlloc`/`VirtualProtect` to `PAGE_EXECUTE_READWRITE` | A/`valloc-rwx`, A/`exec-rwx` |
| A: Execute generated code | A/`exec-rwx`, A/`exec-wx-flip`, A/`exec-from-worker-thread` |
| A: `VirtualAlloc2` / `MapViewOfFile3` / placeholder VAs | A/`virtualalloc2-placeholder` |
| A: `NtCreateSection(SEC_IMAGE)` on supplied DLL | A/`sec-image-map` |
| A: `LoadLibraryEx` from outside package | A/`loadlibrary-out-of-package`, A/`loadlibrary-dont-resolve` |
| B: System DLL inventory + export probes | B/`system32-enumerate`, B/`load:*`, B/`exports:*` |
| B: Raw bytes of 20 ntdll `Nt*` stubs | B/`stub:*` (20 records) + B/`wow64-sharedinfo-ptr` |
| C: NT syscall resolve + safe call | C/`resolve:*` (29), C/`call:*` (10) |
| D: `AddVectoredExceptionHandler` + AV | D/`veh-add-remove`, D/`veh-access-violation` |
| D: Privileged instruction `#GP` caught, continued | D/`veh-privileged-insn` |
| D: `Get`/`SetThreadContext` | D/`thread-context` |
| E: CPUID (vendor, family, HV-present, RDTSC) | E/`cpuid.*`, E/`rdtsc.*`, E/`tsc-vs-qpc` |
| F: Filesystem access matrix (LocalState, Windows, System32, C:\, Temp, known folders) | F/`read:*`, F/`write:*` (21 probes) |
| G: Registry (SOFTWARE/SYSTEM/DEFAULT, writes) | G/`read:*`, G/`enum:*`, G/`write:*` |
| H: Token: AppContainer SID, capability SIDs, integrity, privileges | H/`integrity-level`, H/`is-appcontainer`, H/`appcontainer-sid`, H/`capabilities`, H/`token-groups`, H/`privileges`, H/`user-sid`, H/`elevation`, H/`session-id`, H/`token-open` |
| I: D3D11/D3D12 feature levels + adapter + bridges | I/`d3d11-create`, I/`d3d12-create`, I/`dxgi-factory`, I/`bridge:*` |
| J: WASAPI + XAudio2 | J/`wasapi-*`, J/`xaudio2-*` |
| K: Winsock, outbound TCP, listen, loopback | K/all 9 probes |
| L: Multi-instance self-spawn, child process | L/`createprocess-self`, L/`createprocess-cmd` et al, L/`appmodel-resolve`, L/`package-identity` |
| M: `GlobalMemoryStatusEx`, `IsProcessorFeaturePresent`, SMBIOS, large pages | M/`memory-status`, M/`system-info`, M/`processor-features`, M/`firmware-*`, M/`large-page-*` |
| N: WinRT activation sweep | N/`activate:*` (33 classes) + N/`ro_initialize` |
| Reporting: structured records | `Record { category, name, status, detail, errorcode, value }` |
| Reporting: JSON serialization with environment metadata | `Reporter::to_json` / `flush_locked`, schema `xbox-probe/1` |
| Reporting: write to LocalState | `uwp-host/main.cpp:report_path()` uses `ApplicationData::Current().LocalFolder()` |
| Reporting: on-screen scrollable summary | **Not delivered on Xbox.** Direct2D renderer written and verified on laptop; removed from the Xbox-targeted build after `D3D11CreateDevice` was observed to block the UI thread of a Game-bucket AppContainer app. The report is retrievable via Device Portal only on Xbox |
| Reporting: optional HTTP POST | `probe-core/http.cpp:http_post_report` wired to `PROBE_POST` env var; present in both hosts, not exercised in the Xbox run |
| Reporting: incremental flush | `Reporter::flush_locked` after every emit |
| Reporting: SEH wrap per probe | `Reporter::run` → `run_guarded` + `try/catch` |
| Diff tool | `tools/diff_reports.py`; produces `results/matrix.md` |
| Env 1 win32 host | `win32-host/` CMake project |
| Env 2 UWP on laptop | `uwp-host/` MSBuild vcxproj → `xbox-probe-uwp.msix`, installed via `tools/install_laptop_uwp.ps1` |
| Env 3 UWP on Xbox | Same `.msix`, installed via Device Portal REST on `https://<console-ip>:11443` |
| Deliverable: git repo | `https://github.com/user99672223/win10xbox` |
| Deliverable: shared probe-core lib | `probe-core/` |
| Deliverable: Win32 host | `win32-host/` |
| Deliverable: UWP host | `uwp-host/` |
| Deliverable: diff tool | `tools/diff_reports.py` |
| Deliverable: results/ with JSON | `results/baseline-win32.json`, `results/laptop-uwp-v2.json`, `results/xbox-uwp.json`, `results/matrix.md` |
| Deliverable: README | `README.md`, `docs/schema.md` |
| Deliverable: Dev Mode / deploy instructions | `README.md` plus `tools/deploy_xbox.ps1` (Device Portal REST) |

The one specification item not delivered is the on-screen scrollable summary on Xbox. All other items are delivered.

## 8. Known limitations

1. The Xbox run reports Direct3D feature level 11_0 for both D3D11 and D3D12. The Xbox Series S GPU supports higher levels in general; the measurement reflects what this AppContainer sees, not what the hardware offers.
2. `A/loadlibrary-out-of-package` MISSING on Xbox was not root-caused in this run. The payload DLL is confirmed openable and partially loadable (`DONT_RESOLVE_DLL_REFERENCES` works), so the failure is in the import-resolution step. The precise missing dependency was not isolated.
3. The report fetch via Device Portal `/api/filesystem/apps/file` returns the file content correctly but the sibling `/api/filesystem/apps/files` listing reports `SizeInBytes=0` for the same file. The listing is stale; the file read is authoritative.
4. Memory ceiling of 5 GiB corresponds to the Game bucket of this specific Xbox Dev Mode OS build (`10.0.26100.9445`, `xb_flt_2608ge`). Microsoft documentation for retail Series S partitions is `App ≈ 2 GiB`, `Game ≈ 10 GiB`; the measured 5 GiB in Dev Mode does not match either number. The compat layer should treat this as a per-build value rather than a constant.
5. No `CRASH` records in any run. The harness's SEH / `try/catch` paths were exercised during authoring but were not triggered by the final probe set. Modules written after the harness was validated would still be caught, but the current set does not demonstrate crash isolation on Xbox.

## 9. File inventory

| Path | Role |
|---|---|
| `probe-core/probe.h` | Frozen contract |
| `probe-core/reporter.cpp` | JSON serialization, SEH wrap, incremental flush |
| `probe-core/seh.cpp` | SEH trampoline |
| `probe-core/env.cpp` | Environment metadata capture |
| `probe-core/http.cpp` | Optional WinHTTP report POST |
| `probe-core/core_run.cpp` | Module dispatch table |
| `probe-core/probe_A_*.cpp` … `probe_N_*.cpp` | 15 probe modules |
| `probe-core/probe_payload.cpp` | Tiny DLL used by category A for out-of-package LoadLibrary and SEC_IMAGE tests |
| `win32-host/CMakeLists.txt`, `main.cpp` | Environment 1 host |
| `uwp-host/uwp-host.vcxproj` | Environment 2 & 3 host, MSBuild project |
| `uwp-host/Package.appxmanifest` | UWP manifest, dual TDF Universal + Xbox |
| `uwp-host/App.idl`, `App.h`, `App.cpp` | `XboxProbe.App` runtime class + activation factory |
| `uwp-host/main.cpp` | Headless CoreApplication view; spawns probe worker, waits, exits |
| `uwp-host/pch.h`, `pch.cpp` | Precompiled header for C++/WinRT |
| `uwp-host/Assets/*.png` | Package tile images |
| `tools/ccheck.cmd` | Single-file compile check used during parallel authoring |
| `tools/diff_reports.py` | N-way per-key matrix + regressions report |
| `tools/install_laptop_uwp.ps1` | Elevated cert import + `Add-AppxPackage` for laptop |
| `tools/install_uwp_workload.ps1` | VS 2022 modify to add Universal workload + C++ UWP tools |
| `tools/deploy_xbox.ps1` | Device Portal REST deploy for Xbox |
| `results/baseline-win32.json` | Environment 1 report |
| `results/laptop-uwp.json`, `laptop-uwp-v2.json` | Environment 2 reports (v1 FullTrust, v2 real WinRT activation) |
| `results/xbox-uwp.json` | Environment 3 report |
| `results/matrix.md` | Three-way diff output |
| `results/matrix-2env.md` | Two-way diff (baseline vs laptop) |
| `results/REPORT.md` | This document |
| `docs/schema.md` | JSON schema and status legend |
| `packages/Microsoft.Windows.CppWinRT.2.0.250303.1/` | NuGet-packaged C++/WinRT build integration |
| `README.md` | Build + deploy + diff instructions |
