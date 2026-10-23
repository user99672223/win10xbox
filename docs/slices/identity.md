status: ready

# Slice: machine identity (D18)

One gated guest, `tests/guest/identity.c` (`integration.guest_identity`, on the builtin-only machine folder, both Windows
configs and linux-cross), proves the CPU and version identity is the same on every path a program can read it from. It
prints every value as `path=value` and returns 42 only when all of them agree.

Run: https://github.com/user99672223/win10xbox/actions/runs/39937344377 (linux-cross, windows Debug, windows RelWithDebInfo all green on 96bbd7a)

## Values (Windows runner log)

What `guest_identity` printed on the windows (Debug) runner of that run (`build/bin/guest_identity.log` in the `windows-Debug-logs` artifact; the runner's own CPU
reports a different family, model, brand and build, so every line below is the layer's answer). Core and thread counts are the host's.

```
cpuid.vendor=AuthenticAMD
cpuid.1.eax=00880f81
cpuid.1.ecx=7efa3203
cpuid.family=23
cpuid.model=136
cpuid.stepping=1
cpuid.hypervisor_bit=0
cpuid.rdrand_bit=1
cpuid.brand=AMD 4700S 8-Core Processor
cpuid.80000001.eax=00880f81
cpuid.40000000=00000000 00000000 00000000 00000000
rdrand.success=16/16
registry.brand=AMD 4700S 8-Core Processor
registry.identifier=AMD64 Family 23 Model 136 Stepping 1
registry.family=23
registry.model=136
registry.stepping=1
registry.vendor=AuthenticAMD
registry.mhz=2596
smbios.size=237
smbios.version=3.3
smbios.length=229
smbios.types=bios:1 system:1 processor:1 end:1
smbios.brand=AMD 4700S 8-Core Processor
smbios.vendor=AuthenticAMD
smbios.processor_id=178bfbff00880f81
smbios.family=23
smbios.model=136
smbios.stepping=1
smbios.cores=2
smbios.threads=4
smbios.family_code=107
wmi.CoInitializeEx=00000000
wmi.CoInitializeSecurity=00000000
wmi.CoCreateInstance=00000000
wmi.ConnectServer=00000000
wmi.ExecQuery=00000000
wmi.Next=00000000 count=1
wmi.status=available
wmi.name=AMD 4700S 8-Core Processor
wmi.manufacturer=AuthenticAMD
wmi.cores=2
version.RtlGetVersion=10.0.19045
version.RtlGetVersion.platform=2
version.RtlGetVersion.product_type=1
version.GetVersionExW=10.0.19045
version.RtlGetNtVersionNumbers=10.0.19045
version.VerifyVersionInfoW.equal=1
version.VerifyVersionInfoW.build_plus_one_ge=0
version.registry.CurrentBuild=19045
version.registry.CurrentMajorVersionNumber=10
version.registry.CurrentMinorVersionNumber=0
version.registry.UBR=3803
version.registry.DisplayVersion=22H2
version.KUSER_SHARED_DATA=10.0.19045
version.expected=10.0.19045.3803
sysinfo.level=23
sysinfo.revision=8801
identity=consistent
```

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
