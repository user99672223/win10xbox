status: in progress (push 2: userenv taken out again; the after-sweep is rerun on push 2's wine-pe artifact)

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

Same 25 programs, same method, head 673eff7 with the wine-pe artifact of that commit (run 40143672887: all 11 new
modules built) and the machine folders rebuilt by prepare_machine (Windows\WinSxS staged).

| Program | Class | Exit | First error | Last trace lines |
|---|---|---|---|---|
| 7-Zip File Manager 26.02 | RUNNING | 0x00000103 | - (window painted, version-6 controls now; fixme: unimplemented syscall NtUserBeginDeferWindowPos) | unimplemented syscall NtUserTranslateAccelerator / fixme:dispatch:win10xbox_dispatc... |
| WinRAR 7.23.0 | FAIL | 0x00000103 | no window in 15 s (up, fixme NtUserSetObjectInformation); riched20.dll (no builtin) never mapped | fixme:gdi:ResolveDrawTarget drawing into a 1 bpp bitmap is not supported / fixme:gdi:ResolveDrawTarget drawing into a... |
| Audacity 4.0.1 | FAIL | 0x00000103 | Qt's box "no Qt platform plugin could be initialized ... available platform plugins are: windows": qwindows... | trace:process:Xl_NtTerminateThread thread 3a2c: NtTerminateThread(FFFFFFFFFFFFFFFE, 00000000) / trace:process:Xl_NtTe... |
| Brave Browser 156.1.98.60 | FAIL | 0x00000103 | no window in 15 s; crashpad: "CreateFile: Access denied. (0x5)" (registration pipe to its handler child) | Xl_NtCreateSection section 0000000000000424: chrome.dll (333802064 bytes, host path C:\Program Files\Br... |
| Google Chrome 155.0.8059.86 | FAIL | 0x00000103 | no window in 15 s; crashpad: "CreateFile: Access denied. (0x5)" (registration pipe to its handler child) | MapOurs map C:\Windows\System32\kernel.appcore.dll at 000001F9D9B00000, 110592 bytes, relocated / trace... |
| Mozilla Firefox 158.0.1 | FAIL | 0x00000103 | no window in 15 s, no error in the trace (XPCOM now loads) | Xl_NtCreateSection section 0000000000000440: xul.dll (179697792 bytes, host path C:\Program Files\Mozil... |
| Microsoft Edge 156.0.4314.42 | FAIL | 0x00000103 | no window in 15 s, no error in the trace (msedge_elf.dll now loads) | Xl_NtCreateSection section 0000000000000418: msedge.dll (355157320 bytes, host path C:\Program Files (x... |
| Internet Explorer 11.00.26100.9168 | FAIL | 0x80000100 | fixme:wine: wine: Unimplemented function iertutil.dll.798 called at address 000000017002F0E8 (thread 125c),... | NoteGuestEntry guest entry: TEB.TlsSlots[19] still holds our ThreadState / fixme:wine: wine: Unimplemen... |
| CMake GUI 4.4.3 | FAIL | 0xC0000135 | library icuuc.dll not found (every search path; c0000135) | C:\Windows\System32\ole32.dll at 00000209085E0000 vs C:\Windows\S... |
| HxD Hex Editor 2.5.0.0 | FAIL | 0x00000103 | only a message box, placed mostly above the screen (text not readable); a second HxD process was started | C:\Windows\System32\wininet.dll at 000001BB37E80000 vs C:\Windows... |
| MediaInfo 26.05.0.0 | FAIL | 0xC0000135 | library MSVFW32.dll not found (every search path; c0000135) | C:\Windows\System32\combase.dll at 00000254E9780000 vs C:\Windows... |
| Wireshark 4.6.9 | FAIL | 0x00000103 | no window in 15 s; icuuc.dll (no builtin) never mapped | MapImageFromMemory mapped image at 000002BD16F60000 size 0xd6000 (preferred 0x180000000) RELOCATED / tra... |
| WizTree 4.32.0.0 | FAIL | 0x00000103 | its own box: "Windows Imaging Component (WIC) is not installed on this PC" | fixme:gdi:ResolveDrawTarget drawing into a 1 bpp bitmap is not supported / fixme:gdi:PenOf styled pens (dash/dot) are... |
| YUView (no version resource) | FAIL | 0xC0000135 | library dcomp.dll not found (every search path; c0000135) | C:\Windows\System32\oleaut32.dll at 0000023886F40000 vs C:\Window... |
| iTunes 12.13.10.3 | FAIL | 0xC0000135 | library USP10.dll not found (every search path; c0000135) | C:\Windows\System32\netapi32.dll at 000001A4F6D40000 vs C:\Window... |
| Windows Media Player 12.0.26100.8457 | EXITED | 0x00000000 | exit 0 after 0.6 s, no window: wmp.dll (no builtin) not found | MapImageFromMemory mapped image at 0000015120A80000 size 0x13000 (preferred 0x180000000) RELOCATED / tra... |
| Oracle VirtualBox Manager 7.2.16.174877 | FAIL | 0x00000103 | Qt's box "no Qt platform plugin could be initialized ... available platform plugins are: minimal, ...": qwi... | fixme:gdi:ResolveDrawTarget drawing into a 1 bpp bitmap is not supported / fixme:gdi:PenOf styled pens (dash/dot) are... |
| LocalSend 1.17.0+58 | FAIL | 0x00000103 | no window in 15 s; OPENGL32, UIAutomationCore, d3d9 (no builtin) never mapped | Xl_NtCreateSection section 0000000000000414: flutter_windows.dll (18181632 bytes, host path C:\Program ... |
| FlutterFlow 6.5.3 | FAIL | 0xC0000135 | library d3d9.dll not found (every search path; c0000135) | MapImageFromMemory mapped image at 000001B6B2750000 size 0x124d000 (preferred 0x180000000) RELOCATED / t... |
| MuseHub 3.4.3.2360 | CRASH | 0xC0000409 | .NET: Unhandled exception COMException (0x8007007E, module not found) (WinRT activation) | MapImageFromMemory mapped image at 0000018F1D420000 size 0xe3000 (preferred 0x180000000) RELOCATED / tra... |
| Epic Games Launcher 20.3.6-0+UE5 | FAIL | 0xC0000135 | library msi.dll not found (every search path; c0000135) | C:\Windows\System32\crypt32.dll at 000001F219660000 vs C:\Windows... |
| Node.js 24.19.0 | CRASH | 0xC0000409 | 0xc0000409 (fail-fast) after "guest export lookup failed: kernelbase.dll FlsGetValue2" (a Windows 11 export) | C:\Windows\System32\dbghelp.dll at 000002306EE40000 vs C:\Windows... |
| OpenJDK (Eclipse Adoptium) 25.0.4.1 | EXITED | 0x00000001 | - (prints its usage, exit 1) | trace:process:TerminateOtherGuestThreads terminating guest thread 5230 / trace:process:TerminateOtherGuestThreads ter... |
| Git for Windows 2.55.0.windows.5 | EXITED | 0x00000001 | - (prints its usage, exit 1) | DumpGuestModules guest module list (guest entry): 18 modules; ntdll=1 kernel32=1 kernelbase=1 / trace:m... |
| GitHub CLI 2.103.1 | CRASH | 0x00000002 | Go runtime: "fatal error: unexpected signal during runtime execution" [signal 0x80000003], panic before mal... | C:\Windows\System32\ucrtbase.dll at 000001CA3FFE0000 vs C:\Window... |

Counts: RUNNING 1, EXITED 3, CRASH 3, FAIL 18 (of 25). Before: RUNNING 1, EXITED 3, CRASH 4, FAIL 17.

The class counts barely move, and that is the honest headline: no further program reached a usable main window. What
moved is how far the programs get. Stopped by the loader before any of their own code ran (c0000135 / c0150014 at start):
13 before, 6 after. Still up at 14 s: 5 before (7-Zip, Firefox, Wireshark, LocalSend, FlutterFlow), 12 after (7-Zip,
WinRAR, Audacity, Brave, Chrome, Firefox, Edge, HxD, Wireshark, WizTree, VirtualBox, LocalSend). Nothing that ran before
regressed (7-Zip paints the same window, now with Wine's version-6 controls through its manifest). Four programs now show
their own error box (Audacity and VirtualBox: Qt's platform plugin; WizTree: WIC; HxD: unreadable): their code runs to
the point where it reports what it lacks. Every row's first error is a different, later one than before except Internet
Explorer, Epic, CMake GUI and Windows Media Player (DLLs this slice did not add).

First-error kinds after:

| Kind | Programs | n |
|---|---|---|
| DLL not available | CMake GUI (icuuc), MediaInfo (msvfw32), YUView (dcomp), iTunes (usp10), FlutterFlow (d3d9), Epic (msi), Audacity, VirtualBox (Qt's qwindows: d3d9, UIAutomationCore), Wireshark (icuuc), LocalSend (opengl32, d3d9, UIAutomationCore), WinRAR (riched20), Windows Media Player (wmp) | 12 |
| Missing export | Internet Explorer (iertutil #798), Node.js (kernelbase FlsGetValue2, then fail-fast) | 2 |
| The program's own refusal | WizTree (WIC check), Chrome, Brave (crashpad registration pipe: Access denied) | 3 |
| No window, nothing traced | Firefox, Edge | 2 |
| Runtime crash | GitHub CLI (Go runtime, breakpoint during runtime init) | 1 |
| .NET / WinRT activation | MuseHub (COMException 0x8007007E) | 1 |
| Message box, text off-screen | HxD | 1 |
| none | 7-Zip File Manager, git, java | 3 |
| Activation context | (all six past it) | 0 |

The most common first error is still a DLL with no builtin: the next candidates by count are d3d9 (5 programs: Qt's
qwindows and Flutter), UIAutomationCore (4), icuuc (2), opengl32 (2). d3d9 and opengl32 need a GPU path Wine's PE side
does not have here (wined3d needs Wine's unix GL/Vulkan); UIAutomationCore is a plain Wine PE DLL (imports oleacc, now
built) and the cheapest next step.

## Not done / next first errors

* host-uwp's staged machine folders (host-uwp/stage.cmake, the console payload) do not get the WinSxS assembly yet.
* Chrome, Brave: crashpad's registration pipe to its handler child answers Access denied (a named pipe across two host
  processes, each a layer instance); Edge, Firefox: up without a window and nothing traced, not looked at. (With only
  patch 0035's effect emulated and no Common-Controls 6 in WinSxS, the three Chromium browsers stopped at a breakpoint
  inside chrome_elf.dll instead; with the whole of Fix 2 they get further.)
* Audacity, VirtualBox: slow to load, close to the 15 s limit. In a check run right after the core fix (old builtins)
  both were still mapping their DLLs at 15 s (Audacity with `+relay` reached vorbisfile/FLAC++ only after 60 s); in the
  after-sweep both reached Qt's platform-plugin error box within 15 s. The layer's per-image mapping cost (Qt6Core is
  10 MB) is worth a look of its own.
* Node.js: `FlsGetValue2` (kernelbase, Windows 11) is not exported by Wine's kernelbase; the CRT's fallback then fails
  fast (0xc0000409). An export with clear semantics, the next candidate for a fix of the "missing export" kind.
* 7-Zip File Manager: `NtUserBeginDeferWindowPos` / `NtUserTranslateAccelerator` unimplemented (fixme only; it paints).
* Local `ctest -j 4` (this laptop): 121/122 with the core fix on the old builtins, 120/122 with the new builtins and
  WinSxS (realgui_putty, whose manifest names Common-Controls 6, passes with Wine's version-6 controls). The failures
  are not this slice's: `integration.guest_regpersist_read` fails under -j 4 and passes alone (another guest test on the
  builtin-only folder ran between write and read and deleted the registry overlay, run_guest.cmake), and
  `integration.ninja_build` builds 1 step instead of 3 on a second run over the same machine folder (its comment says
  so: ninja_out is left from the first run). CI runs ctest without -j on fresh folders.
