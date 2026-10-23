status: wip

# Slice: machine identity (D18)

One gated guest, `tests/guest/identity.c` (`integration.guest_identity`, on the builtin-only machine folder, both Windows
configs and linux-cross), proves the CPU and version identity is the same on every path a program can read it from. It
prints every value as `path=value` and returns 42 only when all of them agree.

Run: (filled in when the head run is green)

## Values (Windows runner log)

(filled in from the Windows Debug log)

## Fixes

| path | before | fix |
|------|--------|-----|
| CPUID leaf 1 / 0x80000001 EAX | the host's own signature (a runner's Intel or AMD family/model) | `CpuPolicy` carries the machine identity's family/model/stepping (`core/dispatch/cpu_sites.cpp`, `process.cpp`): 0x00880F81 |
| CPUID leaves 0x80000002..4 | the host's brand string | the identity's brand, NUL padded (AMD style) |
| CPUID leaf 1 ECX bit 30 | the host's (absent on the console) | set; `#UD` from RDRAND/RDSEED on a host without them is emulated by a first-chance VEH (`EmulateRandomInstruction`: BCryptGenRandom, CF=1) |
| `GetSystemFirmwareTable('RSMB')` | the host's table (denied on the console) | `core/dispatch/firmware.cpp`: RawSMBIOSData 3.3 with type 0, 1, 4 (ProcessorVersion = brand, ProcessorId = CPUID.1 EAX/EDX, host core and thread counts) and 127; served by `NtQuerySystemInformation(SystemFirmwareTableInformation)` for provider `RSMB` (enumerate and get), other providers go to the host |
| WMI `Win32_Processor` | `CoCreateInstance` failed (REGDB_E_CLASSNOTREG) | wbemprox reads the SMBIOS table above; `registry.cpp` registers the WbemLocator, WbemContext and WbemAdministrativeLocator CLSIDs (InprocServer32 `wbemprox.dll`, ThreadingModel Both) unless the hive has them; `wine-pe/dlls.txt` gains `winspool.drv`, `setupapi`, `compstui` (wbemprox's import closure) and `prepare_machine.cmake` / `stage.cmake` copy `*.drv` builtins |
| `SystemCpuInformation` (GetSystemInfo level/revision) | the host's | family 23, revision `model << 8 \| stepping` |
| `KUSER_SHARED_DATA` NtBuildNumber / NtProductType / NtMajorVersion / NtMinorVersion | the host's page (a runner reports its own build) | the scanner patches absolute reads (`mov eax,[7FFE0260h]`) and address formations (`mov reg,7FFE0260h`, `lea`) of the version block `0x260..0x273` and the handler serves them from the clock module's virtual page (`loader::ScanOptions::kuser_lo/kuser_hi/kuser_addresses`, `loader::KuserSiteBytes`, `clock::EnsureSharedDataPage`); the rest of the page stays the host's |

Not covered: reads through a base register (`mov al,[rcx+0x7FFE0274]`, as the Wine builtins index `ProcessorFeatures`),
and `NtQuerySystemInformation(SystemProcessorBrandString)`.

## Notes

* `GetVersionExW` and `VerifyVersionInfoW` answer 6.2.9200 to a program without a Windows 10 manifest, on Windows 10 and
  in Wine's kernelbase alike; the guest carries the manifest (`identity.manifest`, merged by MSVC, compiled from
  `identity.rc` by windres).
* WMI works under Wine on linux-cross (it is Wine's wbemprox either way), so `WIN10XBOX_TEST_NO_WMI` is supported by the
  guest but set by no registration.
