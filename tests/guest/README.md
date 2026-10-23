# Guest test programs

Real Windows x64 PE files, built with the same toolchain as the rest of the
repo (MSVC `/MT` on CI, llvm-mingw `-static` locally) and written to
`<build>/bin/guest/`. They do **not** link against `win10xbox_core`: they stand
in for the unmodified programs the layer will eventually load.

| Target | File | What it is for |
|--------|------|----------------|
| `guest_hello` | `hello.c` | Prints `hello from guest` and returns **42**. First end-to-end target for the process bootstrap (exit code propagation). |
| `guest_cpuid` | `cpuid.c` | Prints `cpuid.1.ecx=<hex>` and `vendor=<12 chars>`, returns 0. Shows what CPUID patching makes the guest see (hypervisor bit 31 of ECX, vendor string). |
| `guest_spawn` | `spawn.c` | `CreateProcessW` through the layer: runs `C:\guest\guest_hello.exe` (exit 42, waits, `GetExitCodeProcess`), then itself as `child` with a quoted command line, a working directory and an extra environment variable (exit 7); prints `spawn: child exit 42`, `spawn: child2 exit 7` and returns **42**. Only meaningful through `win10xbox.exe` (`integration.guest_spawn`): it names the guests by their guest paths. |
| `guest_identity` | `identity.c` (+ `identity.manifest`, `identity.rc`) | The machine identity (DECISIONS.md D18) through every path a program has: CPUID leaves 0/1/0x80000001..4/0x40000000 and RDRAND, the registry's `CentralProcessor\0`, the SMBIOS table of `GetSystemFirmwareTable('RSMB')`, WMI `Win32_Processor` through COM, `GetNativeSystemInfo`, and the version through `RtlGetVersion`, `GetVersionExW`, `VerifyVersionInfoW`, `RtlGetNtVersionNumbers`, the registry and `KUSER_SHARED_DATA`. Prints `path=value`, then `identity=consistent` and returns **42** only when all agree (`integration.guest_identity`). Carries a Windows 10 compatibility manifest (without one `GetVersionExW` answers 6.2.9200, as on Windows 10 itself). WMI is required unless the host sets `WIN10XBOX_TEST_NO_WMI=1`. |
| `guest_notells` | `notells.c` (+ `identity.manifest`, `identity.rc`) | What a program that looks for Wine finds (docs/slices/notells.md): `GetProcAddress` and the export tables for `wine_` names, the version resources of ntdll/kernel32/user32 through `GetFileVersionInfoW`, the Wine registry keys, `WINE*` / `WIN10XBOX_*` variables, `Z:` and the OS version. Prints `notells: <check> ok` per check, `info` lines for what is measured but not gated, and returns **42** (`integration.guest_notells`, `integration.guest_notells_shared` with `--shared-folder`). |
| `guest_dll` | `dll.c` | `int add(int,int)`, `unsigned long long read_tsc(void)` (`__rdtsc`), `unsigned int cpuid_ecx1(void)` (`__cpuidex`), plus a TLS directory (a `__declspec(thread)` variable and a TLS callback that bumps `g_tls_callback_hits`), exported as `tls_callback_hits()`, `tls_callback_reason()` and `tls_value()`. Used by `tests/unit/test_loader.cpp` to test the mapper (relocation, exports, protections, relocated TLS callback array) and the scanner (the cpuid/rdtsc sites). |

CRT linkage: MSVC builds use `/MT` (static CRT). llvm-mingw has no static UCRT,
so there the programs import `api-ms-win-crt-*` (resolved to Wine's/Windows'
UCRT by the guest loader); `-static` only removes the libgcc/libc++ DLLs.

`ctest` smoke-runs `guest_hello` and `guest_cpuid` directly (Wine locally).
`guest_hello` intentionally exits with 42, so its test passes on the output
regex, not on the exit code. The unit test locates the DLL relative to its own
executable: `<dir of test_core.exe>\guest\guest_dll.dll`.

The DLL is never run through `DllMain`/TLS by the loader tests - the layer does
not do that (Wine's `loader.c` does). The test only maps it, patches it and
calls exports.
