# Slice: Sysinternals Coreinfo64 and PsInfo64

Two system-information programs that are not ours, run through the layer as gated CI tests
(`integration.coreinfo`, `integration.psinfo`, and rows in `tools/ci/smoke_guests.ps1` behind `-Sysinternals`).

Both are downloaded at CI time from https://live.sysinternals.com/ by `tools/ci/fetch_sysinternals.ps1`, verified
against pinned SHA-256 on every run, cached with `actions/cache` (key carries the hash prefixes) and left in
`build/bin/machine/guest`:

| file | SHA-256 |
|------|---------|
| Coreinfo64.exe | 4be1e87e17499e1237d0a444aae8e775ca876f983862c5fe27e376cd4bce4030 |
| PsInfo64.exe | 983270daa5a2ffa9c5c0c0a8d0f572bc5b6e68e5151ae84bdea7cf76a1d711b9 |

live.sysinternals.com always serves the current release: a new release fails the hash check loudly; re-pin on purpose.

## Machine folder

Both import comdlg32 and shell32. On the shared machine folder the runner's native comdlg32 pulls native shell32,
which needs the ro-typeresolution apiset (wintypes.dll) and a CRT apiset import (`api-ms-win-core-crt-l2-1-0!_initterm_e`)
that resolves to a stub: the guest dies on Wine's 0x80000100 import stub. They therefore run on the builtin-only
machine folder (`WIN10XBOX_BUILTIN_MACHINE_DIR`), like the real GUI guests. `wine-pe/dlls.txt` gained the builtins
they need: comdlg32, pdh, mpr, netapi32, netutils, iphlpapi, dnsapi, nsi, shcore, coml2, cryptbase.

## Anchors asserted (stable across runners)

* Coreinfo64 `-accepteula`, exit 0: `Logical to Physical Processor Map:` and `Logical Processor to Socket Map:`.
  The CPU identification line is not asserted: it is `Intel64 Family ...` or `AMD64 Family ...` depending on the runner.
  Coreinfo writes UTF-16 when stdout is redirected; ctest drops the NULs, the smoke step strips them.
* PsInfo64 `-accepteula`, exit 0: `System information for `, `Uptime:`, `Kernel version:`.

## Status when this was written

* Coreinfo64 runs to completion through the layer (exit 0, the topology maps print for the runner's 4 logical
  processors: socket, NUMA, cache and group maps). integration.coreinfo passed on windows (Debug).
* PsInfo64 does not start: `ws2_32.dll` fails to initialise because its Wine unix library is not registered
  (`NtQueryVirtualMemory(MemoryWineLoadUnixLib)` returns STATUS_DLL_NOT_FOUND, c0000135; see `core/dispatch/nt_process.cpp`).
  Next step: register a `ws2_32` unix lib whose calls answer from the host (gethostname and the name-resolution
  calls) before the guest reaches `Kernel version:`.

## Syscalls added or changed

None so far.
