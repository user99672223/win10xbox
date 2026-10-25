status: ready https://github.com/user99672223/win10xbox/actions/runs/40146150890

# Slice: laptop-programs

A sweep of real, unmodified Windows programs installed on a laptop (Windows 11 24H2, build 26100, Intel GPU), run under
the layer to measure "most programs run" on a real machine. 25 classic x64 Win32 programs from `C:\Program Files` and
`C:\Program Files (x86)`; names, versions and import tables in `docs/game/laptop-programs.txt`. Nothing of the programs
enters the repository.

## Result

Run: https://github.com/user99672223/win10xbox/actions/runs/40146150890 (head f5556a2; linux-cross 114/114, windows
(Debug) 124/124, windows (RelWithDebInfo) 124/124, "nothing skipped" on all three; wine-pe run 40146150892 built every
new module). Push 1 (673eff7, run 40143672901): both Windows jobs 124/124, linux-cross failed only
wine_conformance.kernel32.environ and integration.winetest_kernel32 (userenv, see Fix 1).

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
(its PE part loads Windows ODBC drivers; the unix part is absent), bcryptprimitives, mswsock + wsock32, authz,
msvcp140_2, comctl32_v6 (Fix 2). patches/0007 has a hunk for each (the generator's own Makefile.in transform) and
version_strings.txt a line, status `host`: the FileDescription read from this laptop's System32 (Windows 11 26100), not
recalled. Not added: icuuc (Wine's is a data-only stub; CMake GUI needs ICU's functions), msi (pulls in sxs, mspatcha,
odbccp32), opengl32 and d3d9 (wined3d / a GL unix side), wmp, uiautomationcore (the Flutter programs also need opengl32).

**userenv: added in push 1, taken out in push 2.** With it built, linux-cross failed `wine_conformance.kernel32.environ`
and `integration.winetest_kernel32` (run 40143672901): kernel32's environ test calls `GetUserProfileDirectoryA`
(environ.c:74), which fails with ERROR_NONE_MAPPED, one failure above the baseline (`environ run 10`). dlls.txt already
said so (docs/slices/wt-misc.md); the baseline is not raised. Two things are missing for it: Wine's `LookupAccountSidW`
names only `S-1-5-21-0-0-0-1000` as the current user (`ADVAPI_GetComputerSid` is a constant), not machine.json's
`user_sid` (`...-1001`), and the machine registry has no `ProfileList\ProfilesDirectory` for `GetProfilesDirectoryW`.
Node.js (which imports userenv) therefore stays at "userenv.dll not found"; in push 1's after-sweep, with userenv
present, it got past the loader and failed fast on `FlsGetValue2` (below).

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

Same 25 programs, same method, head f5556a2 (push 2) with the wine-pe artifact of that commit (run 40146150892) and the
machine folders rebuilt by prepare_machine (Windows\WinSxS staged; push 1's userenv.dll moved out of them by hand, since
prepare_machine only adds files).

| Program | Class | Exit | First error | Last trace lines |
|---|---|---|---|---|
| 7-Zip File Manager 26.02 | RUNNING | 0x00000103 | - (window painted, version-6 controls now; fixme: unimplemented syscall NtUserBeginDeferWindowPos) | unimplemented syscall NtUserTranslateAccelerator / fixme:dispatch:win10xbox_dispatc... |
| WinRAR 7.23.0 | FAIL | 0x00000103 | no window in 15 s (up, fixme NtUserSetObjectInformation); riched20.dll (no builtin) never mapped | fixme:gdi:ResolveDrawTarget drawing into a 1 bpp bitmap is not supported / fixme:gdi:ResolveDrawTarget drawing into a... |
| Audacity 4.0.1 | FAIL | 0x00000103 | no window in 15 s (up); USERENV.dll (no builtin, see Fix 1) never mapped; with userenv (push 1) Qt's platfo... | Xl_NtCreateSection section 0000000000000468: Qt6Qml.dll (5335776 bytes, host path C:\Program Files\Auda... |
| Brave Browser 156.1.98.60 | FAIL | 0x00000103 | no window in 15 s; crashpad: "CreateFile: Access denied. (0x5)" (registration pipe to its handler child) | MapOurs map C:\Windows\System32\kernel.appcore.dll at 000001CE026F0000, 110592 bytes, relocated / trace... |
| Google Chrome 155.0.8059.86 | FAIL | 0x00000103 | no window in 15 s; crashpad: "CreateFile: Access denied. (0x5)" (registration pipe to its handler child) | MapOurs map C:\Windows\System32\kernel.appcore.dll at 000001E454030000, 110592 bytes, relocated / trace... |
| Mozilla Firefox 158.0.1 | FAIL | 0x00000103 | no window in 15 s, no error in the trace (XPCOM now loads) | Xl_NtCreateSection section 0000000000000430: xul.dll (179697792 bytes, host path C:\Program Files\Mozil... |
| Microsoft Edge 156.0.4314.42 | FAIL | 0x00000103 | no window in 15 s, no error in the trace (msedge_elf.dll now loads) | Xl_NtCreateSection section 00000000000003C0: msedge.dll (355157320 bytes, host path C:\Program Files (x... |
| Internet Explorer 11.00.26100.9168 | FAIL | 0x80000100 | fixme:wine: wine: Unimplemented function iertutil.dll.798 called at address 000000017002F0E8 (thread 2630),... | NoteGuestEntry guest entry: TEB.TlsSlots[19] still holds our ThreadState / fixme:wine: wine: Unimplemen... |
| CMake GUI 4.4.3 | FAIL | 0xC0000135 | library icuuc.dll not found (every search path; c0000135) | C:\Windows\System32\ole32.dll at 0000018863C60000 vs C:\Windows\S... |
| HxD Hex Editor 2.5.0.0 | FAIL | 0x00000103 | only a message box, placed mostly above the screen (text not readable); a second HxD process was started | MapOurs map C:\Windows\System32\uxtheme.dll at 000001EA2E580000, 180224 bytes, relocated / fixme:gdi:Pe... |
| MediaInfo 26.05.0.0 | FAIL | 0xC0000135 | library MSVFW32.dll not found (every search path; c0000135) | C:\Windows\System32\combase.dll at 00000205BE590000 vs C:\Windows... |
| Wireshark 4.6.9 | FAIL | 0xC0000135 | exit c0000135 after 13.8 s, no window; USERENV.dll, icuuc.dll (no builtin) never mapped | C:\Windows\System32\secur32.dll at 0000014FB8700000 vs C:\Windows... |
| WizTree 4.32.0.0 | FAIL | 0x00000103 | its own box: "Windows Imaging Component (WIC) is not installed on this PC" | fixme:gdi:ResolveDrawTarget drawing into a 1 bpp bitmap is not supported / fixme:gdi:PenOf styled pens (dash/dot) are... |
| YUView (no version resource) | FAIL | 0xC0000135 | library USERENV.dll not found (c0000135); with userenv (push 1): dcomp.dll | MapImageFromMemory mapped image at 0000024B22F10000 size 0x582000 (preferred 0x180000000) RELOCATED / tr... |
| iTunes 12.13.10.3 | FAIL | 0xC0000135 | library USP10.dll not found (every search path; c0000135) | C:\Windows\System32\netapi32.dll at 000001ECA06B0000 vs C:\Window... |
| Windows Media Player 12.0.26100.8457 | EXITED | 0x00000000 | exit 0 after 0.6 s, no window: wmp.dll (no builtin) not found | MapImageFromMemory mapped image at 000002410D490000 size 0x13000 (preferred 0x180000000) RELOCATED / tra... |
| Oracle VirtualBox Manager 7.2.16.174877 | FAIL | 0x00000103 | no window in 15 s (up); USERENV.dll (no builtin, see Fix 1) never mapped; with userenv (push 1) Qt's platfo... | Xl_NtCreateSection section 00000000000003F4: Qt6GuiVBox.dll (8975024 bytes, host path C:\Program Files\... |
| LocalSend 1.17.0+58 | FAIL | 0x00000103 | no window in 15 s; OPENGL32, UIAutomationCore, d3d9 (no builtin) never mapped | Xl_NtCreateSection section 000000000000040C: flutter_windows.dll (18181632 bytes, host path C:\Program ... |
| FlutterFlow 6.5.3 | FAIL | 0xC0000135 | library d3d9.dll not found (every search path; c0000135) | MapImageFromMemory mapped image at 00000209512B0000 size 0x124d000 (preferred 0x180000000) RELOCATED / t... |
| MuseHub 3.4.3.2360 | CRASH | 0xC0000409 | .NET: Unhandled exception COMException (0x8007007E, module not found) (WinRT activation) | MapImageFromMemory mapped image at 000001F89D430000 size 0xe3000 (preferred 0x180000000) RELOCATED / tra... |
| Epic Games Launcher 20.3.6-0+UE5 | FAIL | 0xC0000135 | library msi.dll not found (every search path; c0000135) | C:\Windows\System32\crypt32.dll at 000002C892420000 vs C:\Windows... |
| Node.js 24.19.0 | FAIL | 0xC0000135 | library USERENV.dll not found (c0000135); with userenv (push 1) it got further and failed fast on kernelbas... | C:\Windows\System32\nsi.dll at 000001B607BD0000 vs C:\Windows\Sys... |
| OpenJDK (Eclipse Adoptium) 25.0.4.1 | EXITED | 0x00000001 | - (prints its usage, exit 1) | trace:process:TerminateOtherGuestThreads terminating guest thread 3a70 / trace:process:TerminateOtherGuestThreads ter... |
| Git for Windows 2.55.0.windows.5 | EXITED | 0x00000001 | - (prints its usage, exit 1) | DumpGuestModules guest module list (guest entry): 18 modules; ntdll=1 kernel32=1 kernelbase=1 / trace:m... |
| GitHub CLI 2.103.1 | CRASH | 0x00000002 | Go runtime: "fatal error: unexpected signal during runtime execution" [signal 0x80000003], panic before mal... | C:\Windows\System32\ucrtbase.dll at 000001B741920000 vs C:\Window... |

Counts: RUNNING 1, EXITED 3, CRASH 2, FAIL 19 (of 25). Before: RUNNING 1, EXITED 3, CRASH 4, FAIL 17.

The class counts do not improve, and that is the honest headline: no further program reached a usable main window in
15 s. What moved is how far the programs get:

* Ended by the loader (exit c0000135 / c0150014): 14 before, 8 after. No c0150014 is left.
* Still up at 14 s: 5 before (7-Zip, Firefox, Wireshark, LocalSend, FlutterFlow), 11 after (7-Zip, WinRAR, Audacity,
  Brave, Chrome, Firefox, Edge, HxD, WizTree, VirtualBox, LocalSend). Wireshark and FlutterFlow now end with c0000135
  after 13 s instead (a later load: userenv/icuuc, d3d9).
* Two programs now get far enough to show their own error box: WizTree ("WIC is not installed") and HxD (text above the
  screen edge).
* Nothing that worked before regressed: 7-Zip paints the same window, now with Wine's version-6 controls through its
  manifest; git and java print their usage as before.
* With userenv built (push 1, run 40143672901's artifact; that sweep's table is not reproduced here), Audacity and
  VirtualBox reached Qt's "no Qt platform plugin could be initialized" box and Node.js got past the loader (then failed
  fast on kernelbase `FlsGetValue2`). userenv is the single most valuable missing builtin now (Fix 1).

First-error kinds after:

| Kind | Programs | n |
|---|---|---|
| DLL not available | Node.js, YUView (userenv), Audacity, VirtualBox (userenv, no window), Wireshark (userenv, icuuc), CMake GUI (icuuc), MediaInfo (msvfw32), iTunes (usp10), FlutterFlow (d3d9), LocalSend (opengl32, d3d9, UIAutomationCore), Epic (msi), WinRAR (riched20), Windows Media Player (wmp) | 13 |
| The program's own refusal | WizTree (WIC check), Chrome, Brave (crashpad registration pipe: Access denied) | 3 |
| No window, nothing traced | Firefox, Edge | 2 |
| Missing export | Internet Explorer (iertutil #798) | 1 |
| Runtime crash | GitHub CLI (Go runtime, breakpoint during runtime init) | 1 |
| .NET / WinRT activation | MuseHub (COMException 0x8007007E) | 1 |
| Message box, text off-screen | HxD | 1 |
| none | 7-Zip File Manager, git, java | 3 |
| Activation context | (all six past it) | 0 |

The most common first error is still a DLL with no builtin. By count of programs that never mapped it: userenv (7),
UIAutomationCore (4), d3d9 (3), icuuc (3), opengl32 (3). userenv needs the two things Fix 1 names (Wine's
LookupAccountSidW for machine.json's user SID, ProfileList in the machine registry); UIAutomationCore is a plain Wine PE
DLL; d3d9 and opengl32 need a GPU path Wine's PE side does not have here (wined3d needs Wine's unix GL/Vulkan).

## Not done / next first errors

* host-uwp's staged machine folders (host-uwp/stage.cmake, the console payload) do not get the WinSxS assembly yet.
* Chrome, Brave: crashpad's registration pipe to its handler child answers Access denied (a named pipe across two host
  processes, each a layer instance); Edge, Firefox: up without a window and nothing traced, not looked at. (With only
  patch 0035's effect emulated and no Common-Controls 6 in WinSxS, the three Chromium browsers stopped at a breakpoint
  inside chrome_elf.dll instead; with the whole of Fix 2 they get further.)
* Audacity, VirtualBox: slow to load, close to the 15 s limit. In a check run right after the core fix (old builtins)
  both were still mapping their DLLs at 15 s (Audacity with `+relay` reached vorbisfile/FLAC++ only after 60 s); with
  userenv built (push 1) both reached Qt's platform-plugin error box within 15 s, without it (push 2) they show nothing.
  The layer's per-image mapping cost (Qt6Core is 10 MB) is worth a look of its own.
* Node.js (seen with userenv built, push 1): `FlsGetValue2` (kernelbase, Windows 11) is not exported by Wine's
  kernelbase; the CRT's fallback then fails fast (0xc0000409). An export with clear semantics, the next candidate for a
  fix of the "missing export" kind once userenv is in.
* The local 120/122 below was measured with push 1's builtins (userenv included); push 2 only takes userenv out.
* 7-Zip File Manager: `NtUserBeginDeferWindowPos` / `NtUserTranslateAccelerator` unimplemented (fixme only; it paints).
* Local `ctest -j 4` (this laptop): 121/122 with the core fix on the old builtins, 120/122 with the new builtins and
  WinSxS (realgui_putty, whose manifest names Common-Controls 6, passes with Wine's version-6 controls). The failures
  are not this slice's: `integration.guest_regpersist_read` fails under -j 4 and passes alone (another guest test on the
  builtin-only folder ran between write and read and deleted the registry overlay, run_guest.cmake), and
  `integration.ninja_build` builds 1 step instead of 3 on a second run over the same machine folder (its comment says
  so: ninja_out is left from the first run). CI runs ctest without -j on fresh folders.
