status: in progress (push 1: builtins, activation context; the after-sweep needs the wine-pe artifact of this push)

# Slice: laptop-programs

A sweep of real, unmodified Windows programs installed on a laptop (Windows 11 24H2, build 26100, Intel GPU), run under
the layer to measure "most programs run" on a real machine. 25 classic x64 Win32 programs from `C:\Program Files` and
`C:\Program Files (x86)`; names, versions and import tables in `docs/game/laptop-programs.txt`. Nothing of the programs
enters the repository.

## Method

* Build: this branch's worktree, `cmake --build build`, `tools/dev/prepare-windows.ps1 -Phase Post`, then
  `cmake --build build --target prepare_machine` (the machine folders of the integration tests).
* Machine folder: `build\bin\machine-emu-d3d` (Wine's builtins + the native Direct3D runtime, emulated GUI). The guest's
  `C:\` is that folder, so each program's install directory was linked in with a directory junction
  (`mklink /J <machine>\Program Files\7-Zip "C:\Program Files\7-Zip"`, 24 of them plus `Program Files (x86)\Microsoft`):
  the program runs from its real install path, unmodified, and sees the machine's own registry, profile and System32.
* Run: `win10xbox.exe --machine build\bin\machine-emu-d3d --cwd "<dir>" --debug "+modules,+process,+loader,warn+dispatch"
  "<exe>"` with `WIN10XBOX_TEST_PRESENT_LIMIT=300`, stdin from an empty file, no arguments. `warn+dispatch` is added to
  the brief's channels: it is the only place a DLL the loader could not find is named (`library ... not found ... an
  import of the module being loaded`; the builtins print nothing, docs/slices/nowine.md). A program still up after 14 s
  gets a screenshot and ends through the host's inject script (`WIN10XBOX_TEST_INJECT_INPUT="sleep 14000;shot;quit 259"`,
  exit 259 = still running), with a hard kill of the process tree at 30 s as a backstop (never needed).
* Classes: RUNNING (still up at 14 s with a painted window, or expected console output), EXITED (clean exit before 14 s,
  plausible code: 0, or 1 with its usage text), CRASH (an unhandled exception record, or an NTSTATUS exit), FAIL (a
  missing DLL or export, a hang without a window, the program's own error). A window that is only the program's error box
  counts as FAIL.

## Before (be78c45 + nothing)

| Program | Class | Exit | First error | Last trace lines |
|---|---|---|---|---|
| 7-Zip File Manager 26.02 | RUNNING | 0x00000103 | - (window painted; fixme: unimplemented syscall NtUserBeginDeferWindowPos) | unimplemented syscall NtUserTranslateAccelerator / fixme:dispatch:win10xbox_dispatc... |
| WinRAR 7.23.0 | FAIL | 0xC0000135 | library UxTheme.dll not found (every search path; c0000135) | MapImageFromMemory mapped image at 000001F1C7090000 size 0x2f000 (preferred 0x180000000) RELOCATED / tra... |
| Audacity 4.0.1 | CRASH | 0xC0150014 | 0xc0150014 STATUS_SXS_CORRUPT_ACTIVATION_STACK raised by the HOST ntdll (GetProcAddress forwarder) while gd... | Xl_NtCreateSection section 00000000000003BC: C:\Windows\System32\gdi32.dll (479232 bytes, host path <ma... |
| Brave Browser 156.1.98.60 | FAIL | 0xC0000135 | chrome_elf.dll not found: the private assembly (its .manifest lists it) never parsed | MapOurs map C:\Windows\System32\ucrtbase.dll at 0000020E9C660000, 839680 bytes, relocated / trace:modul... |
| Google Chrome 155.0.8059.86 | FAIL | 0xC0000135 | chrome_elf.dll not found: the private assembly 155.0.8059.86 (its .manifest lists it) never parsed | MapOurs map C:\Windows\System32\ucrtbase.dll at 00000278E7A70000, 839680 bytes, relocated / trace:modul... |
| Mozilla Firefox 158.0.1 | FAIL | 0x00000103 | message box "Couldn't load XPCOM.": xul.dll needs bcryptprimitives.dll, WSOCK32.dll (no builtin) | fixme:gdi:ResolveDrawTarget drawing into a 1 bpp bitmap is not supported / fixme:gdi:PenOf styled pens (dash/dot) are... |
| Microsoft Edge 156.0.4314.42 | FAIL | 0xC0000135 | msedge_elf.dll not found: the private assembly (its .manifest lists it) never parsed | MapOurs map C:\Windows\System32\kernelbase.dll at 0000000174000000, 2670592 bytes / Xl_Nt... |
| Internet Explorer 11.00.26100.9168 | FAIL | 0x80000100 | fixme:wine: wine: Unimplemented function iertutil.dll.798 called at address 000000017002F0E8 (thread 4d4c),... | NoteGuestEntry guest entry: TEB.TlsSlots[19] still holds our ThreadState / fixme:wine: wine: Unimplemen... |
| CMake GUI 4.4.3 | FAIL | 0xC0000135 | library icuuc.dll not found (every search path; c0000135) | C:\Windows\System32\shell32.dll at 000001E705DD0000 vs C:\Windows... |
| HxD Hex Editor 2.5.0.0 | FAIL | 0xC0000135 | library oleacc.dll not found (every search path; c0000135) | C:\Windows\System32\winspool.drv at 00000182C0750000 vs C:\Window... |
| MediaInfo 26.05.0.0 | FAIL | 0xC0000135 | library SHFOLDER.dll not found (every search path; c0000135) | C:\Windows\System32\oleaut32.dll at 0000024769760000 vs C:\Window... |
| Wireshark 4.6.9 | FAIL | 0x00000103 | no window in 15 s; never mapped: UxTheme, USERENV, AUTHZ, icuuc, MSVCP140_2 (no builtin) | Xl_NtCreateSection section 000000000000040C: libwireshark.dll (94091944 bytes, host path C:\Program Fil... |
| WizTree 4.32.0.0 | FAIL | 0xC0000135 | library SHFolder.dll not found (every search path; c0000135) | C:\Windows\System32\shcore.dll at 0000028F3F8F0000 vs C:\Windows\... |
| YUView (no version resource) | CRASH | 0xC0150014 | 0xc0150014 STATUS_SXS_CORRUPT_ACTIVATION_STACK raised by the HOST ntdll (GetProcAddress forwarder) while gd... | Xl_NtCreateSection section 00000000000003F8: C:\Windows\System32\gdi32.dll (479232 bytes, host path <ma... |
| iTunes 12.13.10.3 | FAIL | 0xC0000135 | library ODBC32.dll not found (every search path; c0000135) | C:\Windows\System32\ncrypt.dll at 0000019B51CF0000 vs C:\Windows\... |
| Windows Media Player 12.0.26100.8457 | EXITED | 0x00000000 | exit 0 after 0.7 s, no window: wmp.dll (no builtin) not found | C:\Windows\System32\ws2_32.dll at 0000026D02A40000 vs C:\Windows\... |
| Oracle VirtualBox Manager 7.2.16.174877 | CRASH | 0xC0150014 | 0xc0150014 STATUS_SXS_CORRUPT_ACTIVATION_STACK raised by the HOST ntdll (GetProcAddress forwarder) while gd... | Xl_NtCreateSection section 000000000000040C: C:\Windows\System32\gdi32.dll (479232 bytes, host path <ma... |
| LocalSend 1.17.0+58 | FAIL | 0x00000103 | no window in 15 s; never mapped: OPENGL32, OLEACC, UIAutomationCore, d3d9 (no builtin) | MapImageFromMemory mapped image at 000001FA54770000 size 0x18000 (preferred 0x180000000) RELOCATED / tra... |
| FlutterFlow 6.5.3 | FAIL | 0x00000103 | no window in 15 s; never mapped: UxTheme, OPENGL32, OLEACC, UIAutomationCore, d3d9, bcryptprimitives | Xl_NtCreateSection section 0000000000000448: flutter_windows.dll (19067904 bytes, host path C:\Program ... |
| MuseHub 3.4.3.2360 | CRASH | 0xC0000409 | .NET: Unhandled exception System.Runtime.InteropServices.COMException (0x80040150) (WinRT activation) | MapImageFromMemory mapped image at 000001E3993F0000 size 0xe3000 (preferred 0x180000000) RELOCATED / tra... |
| Epic Games Launcher 20.3.6-0+UE5 | FAIL | 0xC0000135 | library msi.dll not found (every search path; c0000135) | C:\Windows\System32\bcrypt.dll at 000001FBBDB40000 vs C:\Windows\... |
| Node.js 24.19.0 | FAIL | 0xC0000135 | library USERENV.dll not found (every search path; c0000135) | C:\Windows\System32\ncrypt.dll at 000002C653BE0000 vs C:\Windows\... |
| OpenJDK (Eclipse Adoptium) 25.0.4.1 | EXITED | 0x00000001 | - (prints its usage, exit 1) | trace:process:TerminateOtherGuestThreads terminating guest thread 1ae0 / trace:process:TerminateOtherGuestThreads ter... |
| Git for Windows 2.55.0.windows.5 | EXITED | 0x00000001 | - (prints its usage, exit 1) | DumpGuestModules guest module list (guest entry): 18 modules; ntdll=1 kernel32=1 kernelbase=1 / trace:m... |
| GitHub CLI 2.103.1 | FAIL | 0x00000002 | fatal error: bcryptprimitives.dll not found | DumpGuestModules guest module list (guest entry): 4 modules; ntdll=1 kernel32=1 kernelbase=1 / trace:mo... |

Counts: RUNNING 1, EXITED 3, CRASH 4, FAIL 17 (of 25).

First-error kinds:

| Kind | Programs | n |
|---|---|---|
| DLL not available (no builtin; static import -> c0000135, or a LoadLibrary the program needed) | WinRAR (uxtheme), CMake GUI (icuuc), HxD (oleacc), MediaInfo, WizTree (shfolder), iTunes (odbc32), Epic (msi), Node.js (userenv), GitHub CLI (bcryptprimitives), Firefox (bcryptprimitives, wsock32), Wireshark (uxtheme, userenv, authz, icuuc, msvcp140_2), LocalSend, FlutterFlow (opengl32, oleacc, uiautomationcore, d3d9), Windows Media Player (wmp) | 14 |
| Activation context (side-by-side) | Audacity, YUView, VirtualBox (0xc0150014 from the host's loader); Chrome, Brave, Edge (the private assembly that lists chrome_elf.dll never parsed) | 6 |
| Missing export (Wine stub) | Internet Explorer (iertutil.dll #798) | 1 |
| .NET / WinRT activation | MuseHub (COMException 0x80040150) | 1 |
| none | 7-Zip File Manager, git, java | 3 |

## Fix 1: missing builtin DLLs (wine-pe/dlls.txt)

Every DLL above that Wine builds without a unix side worth having is now a builtin: uxtheme, oleacc, shfolder, odbc32
(its PE part loads Windows ODBC drivers; the unix part is absent), userenv, bcryptprimitives, mswsock + wsock32, authz,
msvcp140_2, comctl32_v6 (Fix 2). patches/0007 has a hunk for each (the generator's own Makefile.in transform) and
version_strings.txt a line, status `host`: the FileDescription read from this laptop's System32 (Windows 11 26100), not
recalled. Not added: icuuc (Wine's is a data-only stub; CMake GUI needs ICU's functions), msi (pulls in sxs, mspatcha,
odbccp32), opengl32 and d3d9 (wined3d / a GL unix side), wmp, uiautomationcore (the Flutter programs also need opengl32).

## Fix 2: activation contexts

Three separate defects, all on the path from a manifest to a loaded DLL:

1. **The host's loader read the guest's activation context frames** (core). Wine's PE ntdll never sets
   `TEB.ActivationContextStackPointer` (its unix side would), so the guest pushes its frames onto the stack the host's
   ntdll made for the thread. When the layer, inside a syscall, calls a host loader function (mapping Wine's gdi32 on
   machine-emu-d3d resolves host gdi32 exports with `GetProcAddress`; a forwarder loads `ext-ms-win-dx-d3dkmt-dxcore-l1-1-5`),
   the host's loader found a frame whose ActivationContext is a Wine `ACTIVATION_CONTEXT` and raised
   STATUS_SXS_CORRUPT_ACTIVATION_STACK (0xc0150014; crash record: host ntdll+0x20561, under
   `LdrGetProcedureAddressForCaller` <- `GetProcAddressForCaller`). Any program whose DLL carries a manifest
   (Qt's DLLs do) has a frame active while its imports load. Now the host view has an activation context stack of its
   own per thread (`ThreadState::host_actctx`, empty), installed with TLS/FLS: `SwapIn`/`SwapOut` (HostModeScope, guest
   callbacks) and the whole of `win10xbox_dispatch` (whose TLS switch is in core/asm) via `ActCtxToHost`/`ActCtxToGuest`.
   Test: `integration.guest_actctx_stack` (tests/guest/actctx_stack.c on machine-emu-d3d: activate its own manifest's
   context, `LoadLibraryW(L"gdi32.dll")`; exit 42). Without the fix it ends with c0150014 (checked by disabling the swap);
   with it, it passes.
2. **No manifest file ever parsed** (wine-pe patch 0035). Wine's `get_manifest_in_manifest_file` sizes the file with
   `NtQueryInformationFile(FileEndOfFileInformation)`, a class Windows accepts only in NtSetInformationFile: the host
   answers STATUS_INVALID_INFO_CLASS. A private assembly (Chrome's `155.0.8059.86\155.0.8059.86.manifest`, which lists
   chrome_elf.dll) or a WinSxS manifest therefore never loaded, the program's whole activation context failed, and
   chrome_elf.dll / msedge_elf.dll were "not found". The patch reads EndOfFile from FileStandardInformation. Checked with
   a temporary layer-side answer to the query (not committed): Chrome, Edge and Brave then load *_elf.dll and stop later
   (0x80000003 inside chrome_elf, see below).
3. **No Common-Controls 6.0 assembly** (prepare_machine.cmake). Almost every GUI manifest names
   Microsoft.Windows.Common-Controls 6.0.0.0, which Wine looks up only in `C:\Windows\WinSxS\manifests`; a dependency
   that is not found fails the activation context (`parse_depend_manifests`: STATUS_SXS_CANT_GEN_ACTCTX), and with it
   the program's private assemblies. Wine's `comctl32_v6` is now built (dlls.txt) and `prepare_machine.cmake` installs it
   the way wineboot does: `Windows\WinSxS\manifests\amd64_microsoft.windows.common-controls_6595b64144ccf1df_6.0.2600.2982_none_deadbeef.manifest`
   (machine/winsxs/, Wine's comctl32.manifest with the architecture filled in) and `Windows\WinSxS\<same>\comctl32.dll`,
   in every machine folder it makes. Programs that name Common-Controls 6 now get Wine's version-6 controls, as under Wine.

## After

AFTER_SECTION

## Not done / next first errors

* host-uwp's staged machine folders (host-uwp/stage.cmake, the console payload) do not get the WinSxS assembly yet.
* Chrome, Edge, Brave (after Fix 2): breakpoint 0x80000003 inside chrome_elf.dll (a CHECK; chrome_elf patches ntdll
  and inspects the process early).
* Audacity, VirtualBox (after Fix 2): no window in 15 s; the last trace lines are Qt6Core(VBox) being mapped twice (the
  second time for its manifest), then nothing: a hang in the loader, not yet looked at.
* 7-Zip File Manager: `NtUserBeginDeferWindowPos` / `NtUserTranslateAccelerator` unimplemented (fixme only; it paints).
* Local `ctest -j 4`: `integration.guest_regpersist_read` failed once and passes alone: another guest test on the
  builtin-only folder ran between the write and the read and deleted the registry overlay (run_guest.cmake), a
  parallel-run order effect independent of this slice; CI runs ctest without -j.
