# win10xbox

A Windows 10 22H2 compatibility layer for Xbox Series S|X Dev Mode: a
UWP/AppContainer app that runs unmodified Windows x64 programs (and software
drivers) natively on the console's Zen 2 CPU. No CPU emulation; the OS the
program sees is virtualised.

- `docs/ARCHITECTURE.md` - the layer stack and control flow
- `docs/DECISIONS.md` - every design fork the Xbox capability probe will settle
- `docs/BUILD.md`, `docs/TESTING.md`, `docs/STATUS.md`, `docs/SYSCALLS.md`
- `tools/probe/` - the Dev Mode capability probe (separate deliverable)

Licensing: MIT for our code; Wine-derived files LGPL-2.1+; ReactOS-derived
files GPL-2.0+; see `LICENSE` and `third_party/`. No Microsoft binaries are
in this repository; see `machine/import_windows_dlls.ps1`.
