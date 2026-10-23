status: in progress

# Slice: fastfetch

fastfetch 2.70.0 (the official prebuilt `fastfetch-windows-amd64.zip`, SHA-256 pinned and verified on every run by
`tools/ci/fetch_fastfetch.ps1`, cached with `actions/cache`) is a real guest program that is not ours and that we do
not build. It runs on the builtin-only machine folder (`bin/machine-builtin`, the console's DLL set) and is gated twice
in ci.yml's windows job:

* `integration.fastfetch` (ctest): `fastfetch.exe --pipe --logo none -s OS:Kernel:CPU:Memory:Locale:Uptime`, exit 0 and
  `OS: `, `Kernel: `, `CPU: `, `Memory: ` on stdout.
* step "Gate - fastfetch, each working module prints its line": one process per module for OS, Host, Kernel, Uptime,
  Shell, Terminal, CPU, Memory, Swap, Disk, Locale; each must exit 0 and print its line.

CI run: RUN_URL_PENDING

fastfetch's output through the layer: OUTPUT_PENDING

## What had to change

* `tools/ci/fetch_fastfetch.ps1` + the ci.yml steps (cache, fetch), `tests/integration/CMakeLists.txt`
  (`integration.fastfetch`, registered under `MSVC OR WIN10XBOX_REAL_PROGRAMS` like `integration.lua_*`).
* It must run on `bin/machine-builtin`. On the shared Windows machine folder the first attempt loaded the runner's
  native advapi32/sechost/rpcrt4 through Wine's builtin user32 and died on `EXCEPTION_WINE_STUB`
  (`api-ms-win-core-crt-l2-1-0!_initterm_e` in sechost's DllMain), the failure docs/STATUS.md describes for the GUI tests.
* Wine builtins added to `wine-pe/dlls.txt` (fastfetch's import closure, computed from the PE import tables of the
  built artifact): `cfgmgr32 coml2 cryptbase dnsapi hid iphlpapi nsi secur32 shcore winbrand wtsapi32`. Nothing is
  added to `machine/guest_dlls.txt`. `winbrand` is Wine's semi-stub (`BrandingFormatString` answers a fixed
  "Windows 10 Pro" edition).
* `core/registry/registry.cpp`: `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` gained `ProcessorNameString`
  (`machine_identity.cpu_brand`, D18), `Identifier` (`AMD64 Family 23 Model 136 Stepping 1`, same identity) and `~MHz`
  (the host's own value from its `CentralProcessor\0`; on a Wine host the same key carries the /proc/cpuinfo figure; no
  host answer leaves it out). fastfetch's CPU module reads all three plus `VendorIdentifier` and prints nothing if one
  is missing. Unit test: `registry_hardware_cpu_identity`.
* `core/dispatch/unix_calls.cpp` gained a **ws2_32 unix library**: Wine's ws2_32 `DllMain` returns FALSE unless
  `__wine_init_unix_call` finds a unix library named `ws2_32`, which failed the load of every program importing ws2_32
  (fastfetch directly and through iphlpapi). Only `gethostname` is implemented (the layer's computer name); the
  resolver entry points (`getaddrinfo`, `gethostbyaddr`, `gethostbyname`, `getnameinfo`) return `WSAEOPNOTSUPP`.
* No syscall was added or changed.

## Todo (measured with one process per module; none of these is gated)

| module | result | cause |
|---|---|---|
| Display | exit `0x80000100` (`EXCEPTION_WINE_STUB`) | `USER32.dll!IsThreadDesktopComposited`: fastfetch.exe imports it and Wine 11.9's user32 builtin does not export it, so the first call hits Wine's import stub (read from the exception parameters, which point at the DLL and function name strings inside fastfetch.exe) |
| Sound | exit 1722 (`RPC_S_SERVER_UNAVAILABLE`) | `err:wine:ole:com_get_class_object class {bcde0395-e52f-467c-8e3d-c4579291692e} not registered`: CLSID_MMDeviceEnumerator, no mmdevapi in the builtin set |
| GPU, Packages, LocalIp, Battery, Bluetooth | exit 0, no line | their detectors fail quietly (D3D/DXGI, package managers, adapter enumeration through iphlpapi/nsi, battery and bluetooth device queries); not traced further |
