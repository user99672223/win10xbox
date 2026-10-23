# Slice: fastfetch

fastfetch 2.70.0 (the official prebuilt `fastfetch-windows-amd64.zip`, SHA-256 pinned and verified on every run by
`tools/ci/fetch_fastfetch.ps1`, cached with `actions/cache`) is a real guest program that is not ours and that we do
not build. `integration.fastfetch` runs

    fastfetch.exe --pipe --logo none -s OS:Kernel:CPU:Memory:Locale:Uptime

on the builtin-only machine folder (`bin/machine-builtin`, the console's DLL set) and gates on exit code 0 and
`OS: `, `Kernel: `, `CPU: `, `Memory: ` on stdout.

## Where it stands (ci.yml run 39875658547, head 195f538)

The binary loads, initialises and runs to completion; the gate is red on one line only: `CPU: ` is missing.

    OS: Windows 10 Pro (22H2) x86_64
    Kernel: Wine_11.9 10.0.19045.3803
    Memory: 2.72 GiB / 15.99 GiB (17%)
    Locale: en_US.UTF-8
    Uptime: 6 mins

Every module on its own (the information-only step "fastfetch, every module on its own" of ci.yml, one process each):

| module | result |
|---|---|
| OS, Kernel, Uptime, Memory, Swap, Locale, Disk (`C:\ ... NTFS`), Host, Shell, Terminal | exit 0, a line |
| CPU | exit 0, **no line** (see below) |
| GPU, Packages, LocalIp, Battery, Bluetooth | exit 0, no line |
| Display | exit `0x80000100` (`EXCEPTION_WINE_STUB`: a call through an import Wine's builtins do not resolve) |
| Sound | exit 1722: `com_get_class_object class {bcde0395-...} not registered` (MMDeviceEnumerator, no mmdevapi) |

### CPU: the one remaining gap

`cpu_windows.c` (`detectByRegistry`) opens `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` and reads
`ProcessorNameString`, `VendorIdentifier` and `~MHz` together; if any is missing it reports nothing. The key opens
(`trace:registry:OpenCommon ... CentralProcessor\0`), but `core/registry/registry.cpp` writes only
`VendorIdentifier` into the volatile HARDWARE overlay (line ~993). DECISIONS.md D18 already says the registry overlay
serves `ProcessorNameString` from `config::machine_identity.cpu_brand`; that half was never written. Missing:
`ProcessorNameString` (REG_SZ, the D18 brand), `~MHz` (REG_DWORD; the host's own base clock, no invented number), and
`Identifier` (REG_SZ, `AMD64 Family 23 Model 136 Stepping 1`, from the same identity) for the other readers of the key.

## What had to change to get this far

* `tools/ci/fetch_fastfetch.ps1` + the ci.yml steps (cache, fetch), `tests/integration/CMakeLists.txt`
  (`integration.fastfetch`, registered under `MSVC OR WIN10XBOX_REAL_PROGRAMS` like `integration.lua_*`).
* It must run on `bin/machine-builtin`. On the shared Windows machine folder the first attempt loaded the runner's
  native advapi32/sechost/rpcrt4 through Wine's builtin user32 and died on `EXCEPTION_WINE_STUB`
  (`api-ms-win-core-crt-l2-1-0!_initterm_e` in sechost's DllMain), the failure docs/STATUS.md describes for the GUI tests.
* Wine builtins added to `wine-pe/dlls.txt` (fastfetch's import closure, computed from the PE import tables of the
  built artifact): `cfgmgr32 coml2 cryptbase dnsapi hid iphlpapi nsi secur32 shcore winbrand wtsapi32`. Nothing is
  added to `machine/guest_dlls.txt`. `winbrand` is Wine's semi-stub (`BrandingFormatString` answers a fixed
  "Windows 10 Pro" edition), which is why OS shows `Windows 10 Pro` regardless of the machine.
* Syscalls: none added or changed. The one new piece in core/dispatch is a **unix library for ws2_32**
  (`core/dispatch/unix_calls.cpp`): Wine's ws2_32 `DllMain` returns FALSE unless `__wine_init_unix_call` finds a unix
  library named `ws2_32`, which failed the load of every program importing ws2_32 (fastfetch directly and via
  iphlpapi). Registered with the five resolver entry points: `gethostname` answers the layer's computer name,
  `getaddrinfo`, `gethostbyaddr`, `gethostbyname`, `getnameinfo` return `WSAEOPNOTSUPP` (no resolver behind them yet).

## Open

* CPU registry values (above): the fix that turns the gate green.
* Display: find the unresolved import (the crash record names it; the information step prints only the first lines).
* GPU/Packages/LocalIp/Battery print nothing (their detectors fail quietly); Sound needs mmdevapi.
