# Slice: fastfetch

fastfetch 2.70.0 (the official prebuilt `fastfetch-windows-amd64.zip`, pinned SHA-256, fetched by
`tools/ci/fetch_fastfetch.ps1`) runs through the layer as `integration.fastfetch`:

    fastfetch.exe --pipe --logo none -s OS:Kernel:CPU:Memory:Disk:Locale:Uptime

Gate: exit code 0 and `OS: `, `Kernel: `, `CPU: `, `Memory: ` on stdout.

Status: in progress (first push).

## Imports the prebuilt binary needs beyond the native guest set

`HID`, `IPHLPAPI`, `Secur32`, `WTSAPI32` (Wine builtins, `wine-pe/dlls.txt`), `WINBRAND` (native, no Wine
equivalent: `machine/guest_dlls.txt`).
