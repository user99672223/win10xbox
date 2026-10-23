status: ready

# Slice: Sysinternals Coreinfo64

A system-information program that is not ours, run through the layer as a gated CI test (`integration.coreinfo`, and a
row in `tools/ci/smoke_guests.ps1` behind `-Sysinternals`). Run: https://github.com/user99672223/win10xbox/actions/runs/39874860955 (windows Debug: integration.coreinfo passed; the head run is linked from the branch)

Downloaded at CI time from https://live.sysinternals.com/ by `tools/ci/fetch_sysinternals.ps1`, verified against a
pinned SHA-256 on every run, cached with `actions/cache` (key `sysinternals-coreinfo-4be1e87e`) and left in
`build/bin/machine/guest`. `-IncludePsInfo` also fetches PsInfo64.exe; nothing gated runs it.

| file | SHA-256 |
|------|---------|
| Coreinfo64.exe | 4be1e87e17499e1237d0a444aae8e775ca876f983862c5fe27e376cd4bce4030 |
| PsInfo64.exe (opt-in) | 983270daa5a2ffa9c5c0c0a8d0f572bc5b6e68e5151ae84bdea7cf76a1d711b9 |

live.sysinternals.com always serves the current release: a new release fails the hash check loudly; re-pin on purpose.

## Anchors asserted (`-accepteula`, exit 0)

`Logical to Physical Processor Map:` and `Logical Processor to Socket Map:`. The CPU identification line is not
asserted: it is `Intel64 Family ...` or `AMD64 Family ...` depending on the runner. Coreinfo writes UTF-16 when stdout is
redirected: ctest drops the NULs, the smoke step strips them.

## Machine folder and wine-pe/dlls.txt additions

Coreinfo64 imports comdlg32 and shell32. On the shared machine folder the runner's native comdlg32 pulls native shell32,
which needs the ro-typeresolution apiset (wintypes.dll) and an import of `api-ms-win-core-crt-l2-1-0!_initterm_e` that
resolves to Wine's 0x80000100 stub. It therefore runs on the builtin-only machine folder
(`WIN10XBOX_BUILTIN_MACHINE_DIR`), like the real GUI guests. Builtins added to `wine-pe/dlls.txt`: comdlg32, pdh, mpr,
netapi32, netutils, iphlpapi, dnsapi, nsi (PsInfo64's imports), and shcore, coml2, cryptbase (static imports of
shlwapi, ole32 and advapi32 that were missing: `import_dll ... shcore.dll not found`, `coml2.dll not found`,
`find_forwarded_export cryptbase.SystemFunction036`).

## Syscalls added or changed

None.

## PsInfo64 (moved to the -net slice)

PsInfo64 needs ws2_32 (MemoryWineLoadUnixLib for ws2_32 returns c0000135; exit 0xC0000142) - moved to -net.
Exact sequence seen on windows (Debug), after all of PsInfo64's static imports (pdh, netapi32, mpr, comdlg32, ole32,
iphlpapi, dnsapi, ws2_32) had mapped:

1. `NtQueryVirtualMemory(-1, <dnsapi base>, 0x3e8 /*MemoryWineLoadUnixLib*/, ...)` -> `c0000135`
   (dnsapi tolerates it: `err:wine:dnsapi:DllMain No libresolv support, expect problems`)
2. `NtQueryVirtualMemory(-1, <ws2_32 base>, 0x3e8, ...)` -> `c0000135`
   (`core/dispatch/nt_process.cpp` finds no registered unix lib for ws2_32; ws2_32's DllMain returns
   `!__wine_init_unix_call()`)
3. `err:wine:module:loader_init "ws2_32.dll" failed to initialize, aborting`, then
   `loader_init Initializing dlls for L"C:\\guest\\PsInfo64.exe" failed, status c0000142`
4. `NtTerminateProcess(-1, c0000142)`; host exit code `0xC0000142`.

Next step: register a ws2_32 unix lib whose calls answer from the host (gethostname and the name-resolution calls).
Anchors planned for it: `System information for `, `Uptime:`, `Kernel version:`.
