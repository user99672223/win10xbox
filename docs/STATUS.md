# Status


## Current state of main (2026-10-22)

**What works, measured.** On the Xbox Series S (build 26100) one unmodified MSVC x64 guest is mapped, bound,
run and exits 42 (the hardware run described in HANDOFF.md). Under Wine and on the Windows CI runners, the
following run and are gated (`ci.yml`, `host-win32-smoke.yml`, `host-win32-appcontainer.yml`, `host-uwp.yml`):

* **Loader** (`core/loader`): PE mapping, import binding, Wine builtin stub rewrite, scanner.
* **Dispatcher** (`core/dispatch`): by-name syscall table generated from Wine's headers; per-syscall policy
  (`Emulate` / `PassThrough` / `Todo`); privileged NT calls answered as an unprivileged caller would.
* **VFS** (`core/vfs`): virtual `C:\` over the machine folder, case-folded path comparison, NT path translation.
* **Registry** (`core/registry`): virtual `\Registry` from read-only REGF hives plus an in-memory write overlay.
* **NLS** (`core/dispatch/nt_nls.cpp`): `NtGetNlsSectionPtr` / `NtInitializeNlsFiles` map the `.nls` files staged
  in the machine folder; a missing file fails with `STATUS_OBJECT_NAME_NOT_FOUND`, nothing is faked.
* **Console guests**: `tests/guest/{hello,cpuid,sleep,dll}` and the smoke guest, through the host, with exit codes.
* **core/user** (`core/user/`, Wine 11.9 win32u contract): 234 `NtUser*`/`NtGdi*` syscalls emulated. Windows
  (class, create, show, paint, timers, PostMessage/SendMessage, DestroyWindow), GDI (DCs, DIB sections, memory DCs,
  BitBlt, FillRect, GetPixel), and input delivery in `win_message.cpp` (mouse and key messages queued to the
  window). Gated: `integration.guest_window`, `integration.guest_window2`, and input delivery
  (`integration.guest_window_input`, `guest_window_input2`: scripted mouse and keys reach the guest's window,
  merged from `-input` at `d9f7639`). Not on main: glyph text, menus,
  clipboard, hooks (see in-flight list).
* **D3D11 presenter** (`host-uwp/presenter.cpp`, `blit.cpp`): `user::ComposeFrame()` into a flip-sequential swap
  chain on the CoreWindow, presented from the UI thread's 60 Hz pump. Device on its own thread; hardware, then WARP,
  then headless with a `display:` line. `LocalState\screen.bmp` is written every 5 s and at exit.
* **Nine bundled guests** in the host-uwp package (`host-uwp/CMakeLists.txt`, `HOST_UWP_GUESTS`): `window.exe`
  (the default), `hello.exe`, `hello_cui.exe`, `guest_hello`, `guest_cpuid`, `guest_sleep`, `guest_dll`,
  `smoke_cui.exe`, `drv_client.exe`.
* **Native user32/gdi32 in an AppContainer (measured, non-gating)**: a genuine AppContainer process can load the
  native user32/gdi32, create, show and paint a window and read pixels back on the Windows runner's session
  (run 39827778745, `tests/appcontainer/ac_gui_probe.c`). Not measured on the console. This is the evidence for
  route B (forward to the host's win32u); route A (core/user) remains the gated path.

**What does not work yet.** Glyph text (`ExtTextOut`/`DrawText` fill rectangles only), menus, clipboard, hooks,
window-frame dragging, networking, audio, guest child processes, D3D for guest apps (M6), and the 21 `todo`
syscalls. `integration.guest_window_text` is `DISABLED` on main until the `-text` slice lands. The package's `window.exe` is a 30-second blue window that exits 42 only when painted.

**Packaged `.msix`.** Artifact `host-uwp-msix` from the latest completed green `host-uwp.yml` run on main:
[39837028686](https://github.com/user99672223/win10xbox/actions/runs/39837028686) (head `fb86712`; main has since
moved to `1a6fb74`, whose run [39846504150](https://github.com/user99672223/win10xbox/actions/runs/39846504150) was
still in progress when this was written).

## In-flight slices (branches `claude/youthful-cerf-l7xxsb-<slice>`)

Found on origin at this writing (each is created from main; the one-line scope is from its latest commit):

| Slice | Branch | Scope |
|---|---|---|
| native | `-native` | route B: native user32/gdi32 in the guest, `gui` knob in machine.json |
| text | `-text` | built-in 8x16 bitmap font for `TextOutW`/`DrawTextW` (`font8x16.h`) |
| texttest | `-texttest` | `guest_window_text` (disabled until `-text` lands) |
| input | `-input` | input delivery into core/user: merged into main at `d9f7639` |
| inputtest | `-inputtest` | `WIN10XBOX_TEST_INJECT_INPUT` host hook, `guest_window_input` (merged with input) |
| realprog | `-realprog` | Lua 5.4.7 as the first real guest |
| sqlite | `-sqlite` | SQLite shell as a real guest (pinned amalgamation build) |
| sys1 / sys2 | `-sys1`, `-sys2` | privileged NT syscalls; five Todo syscalls via existing modules |
| syscalls | `-syscalls` | syscall work (its own commits are on the branch; main was merged into it) |
| present | `-present` | `window.exe` exits 42 only when painted and quit by its timer |
| ci | `-ci` | CI uploads crash.txt, crash.dmp, entry-reached.txt, GUI screenshots |
| gui-probe | `-gui-probe` | the AppContainer native user32/gdi32 measurement (above) |
| user | `-user` | core/user emulation and its STATUS/TESTING entries |

Named for the next slices but **no remote branch found at this writing**: `-ncframe` (non-client frame),
`-menu`, `-dialog`, `-clipboard`, `-hooks` (timers and hooks), `-guiapp` (a real GUI program), `-net` (ws2_32/AFD),
`-audio`, `-uwpchild` (child processes in UWP), `-d3d` (ImGui on d3d11 via route B). Check `git ls-remote` before
relying on them.

## Real programs: the SQLite shell (2026-10-22)

`sqlite3.exe` (SQLite 3.54.1, the official amalgamation, `cl /O2 /MT shell.c sqlite3.c`; `tools/ci/build_sqlite.ps1`,
pinned by URL and SHA-256 and verified on every run) runs unmodified through the layer on both Windows configs:
`integration.sqlite_memory` (`-batch :memory: "select 6*7;"` prints `42`), `integration.sqlite_script`
(`tests/guest/sqlite/smoke.sql` on stdin against `C:\Users\User\smoke.db`: table, inserts with one rollback journal
each, ordered and aggregate selects, an index, `.dump`, `date('now')`, `pragma integrity_check`, `.exit 42`; every
output line gated, the `.db` file must exist in the machine folder) and the smoke rows. `ci.yml` run
[39868114103](https://github.com/user99672223/win10xbox/actions/runs/39868114103) on `8d9e2b0` (later commits only change docs): windows (Debug) and windows (RelWithDebInfo) green, `43 tests, 0 failures, 0 skipped` in each
job's `ctest-junit.xml`; `host-win32-smoke` (console conditions), `host-win32-appcontainer`, `uwp-lite`, `host-uwp`
green on the same commit. The script's stdout (identical in both configs and under Wine):

```
plum|7        pear|5        apple|3       fig|2
4|17|2.0      pear|3.75     plum|1.75     apple|1.5      indexed|1
PRAGMA foreign_keys=OFF; BEGIN TRANSACTION; CREATE TABLE fruit(...); INSERT INTO fruit VALUES(1,'apple',3,0.5); ...
CREATE INDEX fruit_name on fruit(name); COMMIT;
date|10|1|1      ok
```

Three defects, all found by running it, none visible to our own test programs; each has a regression test that runs
without SQLite:

1. **The command line ran into the next string.** `RtlCreateProcessParametersEx` packs the block's strings by
   `MaximumLength`, which was `Length`, so `GetCommandLineW()` returned `select 6*7;C:\guest\sqlite3.exeWinSta0\Default`
   (a "near C: syntax error"). Fixed in `core/dispatch/process.cpp` (the Lua bring-up found the same defect on its
   branch and made the same change; the merge keeps one). `integration.guest_args`.
2. **Every file lock failed on real Windows.** Wine's kernelbase calls `NtLockFile` with `IoStatusBlock == NULL`; Windows
   answers `STATUS_ACCESS_VIOLATION` (10 microseconds, in the trace), SQLite reported "database is locked" on its first
   statement. `NtLockFile`/`NtUnlockFile` are now `Translate` (`core/dispatch/nt_file.cpp`, `docs/DISPATCHER.md`): a
   Windows host gets a block of the layer's, a Wine host (which refuses any block in `NtLockFile`, and where `NtUnlockFile`
   wants NULL on Wine 9 and a block on Wine 11) gets what it accepts. `integration.guest_lockfile`, which passes on
   linux-cross, Debug and RelWithDebInfo.
3. **The scanner patched a jump table.** MSVC without optimisation (the Debug configuration) puts a switch's jump table
   inside `.text`; the sweep decoded its bytes, found a `syscall` in them and wrote an INT3 over the low byte of an entry
   (`0x5c1c5` became `0x5c1cc`), so `printf("%lu")` jumped into the middle of a `call` and died with an access violation. Only
   `guest_lockfile` in Debug showed it (sqlite3.exe is Release); the crash record's `rax = rip - 2`, `rcx` = image
   base and the `7c fa` at the target gave it away, and the CI artifact carrying the guest's exe and pdb confirmed the table.
   `core/loader/scanner.cpp` now finds such tables from the `mov eax,[base+idx*4+TABLE]` that reads them, keeps them out of
   the sweep and takes their entries as branch targets. `unit.core` `loader_scan_leaves_jump_tables_in_code_alone`.

Not defects of the layer, worth knowing: the Linux reproduction's host is Wine 9.0 while the guest's DLLs are Wine 11.9,
which disagree about `NtUnlockFile` (handled above); `sqlite_*` are registered on MSVC builds only (the Linux job has no
`sqlite3.exe`), so they cannot skip there. What `sqlite3.exe` did not need, and the next program might: memory-mapped
files (`PRAGMA mmap_size`), `LockFileEx` with a pending wait on an overlapped handle (returns `STATUS_PENDING` into a
block the layer keeps alive, never completed), WAL's shared memory (`CreateFileMapping` over the `-shm` file), the
console (`.mode` with a terminal, `isatty`), and `GetFileAttributesEx` on directories other than the machine folder.

## Install the package on the console

1. Download artifact `host-uwp-msix` from the green run above (or the newest green `host-uwp.yml` run on main).
2. Device Portal: install the `.cer` certificate first, then the `.msix`.
3. The default `LocalState\launch.json` runs `window.exe` (30 s, expects exit 42) and keeps the last frame on the TV.
   Result: `RESULT: PASS` or `RESULT: FAIL` at the top of `LocalState\status.txt`.
4. `LocalState\screen.bmp` is the last composed frame; `LocalState\entry-reached.txt` and `crash.txt` are written
   on the run. A `launch.json` placed in `LocalState` overrides the default (`launch.example.json` lists the keys).

## core/user: text rendering (2026-10-22)

`TextOutW` / `ExtTextOutW` / `DrawTextW` now put glyphs on the screen: `core/user/font8x16.h` is a public-domain
8x16 VGA-style bitmap table (ASCII 0x20-0x7E, box glyph for everything else; cell 8x16, baseline at row 12) and
`NtGdiExtTextOutW` draws it with ETO_OPAQUE / ETO_CLIPPED / ETO_PDY, the `dx` array, OPAQUE or TRANSPARENT
background, text colour, every `TA_*` alignment (including `TA_UPDATECP`) and the DC clip, alpha 0xFF. The metrics
syscalls (`GetTextMetricsW`, `GetTextExtentExW`, `GetCharWidthW`, `GetCharABCWidthsW`, the new `GetCharWidthInfo`)
report the same cell for every font, so user32's `DrawText` layout (centring, `DT_VCENTER`, multi-line) comes out
right; LOGFONT heights >= 32 scale the glyphs by whole factors. Design and limits: docs/GUI.md ("Text").
Local (llvm-mingw + Wine): `ctest --test-dir build-mingw --label-exclude wine_conformance` 100% green with nothing
skipped; new `user_font8x16_contract`, `user_ext_text_out_*` and `user_text_metrics_and_extents` unit cases. A
scratch guest through the real gdi32 / user32 showed `TextOutW(10,10,"HI")` with 'H' pixels (1,3) set and (3,3)
clear, `DrawTextW("Centered", DT_CENTER|DT_VCENTER|DT_SINGLELINE)` in a 400x50 rectangle starting at x=168 (centred
64 px of text) and three `DrawTextW` lines 16 px apart.

## core/user: the first pixels, under Wine and on real Windows (2026-10-22)

`core/user` emulates Wine 11.9's win32u contract (235 `NtUser*`/`NtGdi*` syscalls registered, listed as Emulate in
docs/SYSCALLS.md, plus `NtCallbackReturn`; design and the not-implemented list in docs/GUI.md).
`integration.guest_window` (tests/guest/window.c) and `integration.guest_window2` (tests/guest/window2.c) pass, gated:

* Local (llvm-mingw + Wine): `100% tests passed, 0 tests failed out of 28` (`ctest --test-dir build-mingw
  --label-exclude wine_conformance`; no skips); `unit.core`: `121 tests, 0 failures` (the `user_*` cases included).
* `ci.yml` run [39839907345](https://github.com/user99672223/win10xbox/actions/runs/39839907345) on `249d386`
  (first green on `bc82f9a`, run [39834605986](https://github.com/user99672223/win10xbox/actions/runs/39834605986)):
  windows (Debug) and windows (RelWithDebInfo) green with the full suite gated (`100% tests passed out of 32`,
  `ctest gate: 32 registered, 32 ran and passed, 0 DISABLED`). The tests' output in both configs (from the jobs'
  `ctest-junit.xml`; the first line is run_guest.cmake's `+modules` check, the rest the guest's stdout):
  ```
  guest system DLLs from Windows\builtin: kernel32.dll kernelbase.dll user32.dll gdi32.dll advapi32.dll sechost.dll ucrtbase.dll msvcrt.dll win32u.dll imm32.dll
  window: class registered
  window: created hwnd=0000000000010021 client=400x300
  window: painted
  window: pixel(10,10)=00dc5000 pixel(30,30)=000000ff
  window: done
  window2: client=320x200
  window2: window=(50,60)-(386,299) created=1
  window2: text=win10xbox two
  window2: pixel(5,5)=0000ff00 pixel(20,20)=0000ffff
  window2: screen=1920x1080 window colour=ffffff
  window2: timer=2 destroy=1 ncdestroy=1 iswindow=0 paints=2
  window2: done
  ```
  The same programs run natively on the runner (`guest.window`, `guest.window2`, real win32k) print the same
  lines except the HWND and `screen=1024x768` (the runner's desktop; ours is the headless 1920x1080 monitor).
  Screenshots (`guest_window.bmp`, `guest_window2.bmp` in `windows-<config>-logs`, 1920x1080): window (110,110)
  `ff0050dc`, (130,130) `ffff0000`; window2 (63,96) `ff00ff00`, (78,111) `ffffff00`, caption (100,75) `ff99b4d1`,
  identical in both configs and under Wine.

Two Windows-only failures were found on the way, neither in core/user:

1. **0xC0000374 (heap corruption) in the host.** The guest thread entered `LdrInitializeThunk` with the host's TLS
   vector in `TEB.ThreadLocalStoragePointer`. While resolving static imports Wine's loader runs `alloc_tls_slot`
   for each DLL with a TLS directory (msvcrt, reached through user32 -> advapi32) *before* `alloc_thread_tls`, and
   frees and replaces slot *i* of whatever vector the TEB holds: the host's own TLS blocks. Fixed in
   `EnterGuest` (`core/dispatch/process.cpp`): the guest starts with no vector (the host's is already in
   `ThreadState::host_tls`); `NtGetNextThread` also enumerates guest threads only (docs/DISPATCHER.md).
2. **0x80000100 (`EXCEPTION_WINE_STUB`).** The shared machine folder of the Windows jobs holds the runner's
   build-26100 DLLs with `native,builtin`, so Wine's builtin user32/gdi32 pulled in native advapi32, sechost and
   rpcrt4. Those import ntdll exports Wine 11.9 lacks (`RtlCreateBoundaryDescriptor`,
   `TpSetDefaultPoolStackInformation`, ...), and the host's apiset schema (the guest shares the host's PEB, so
   `PEB.ApiSetMap` is the host's) resolves their `api-ms-win-core-crt-l2-1-0` to `kernelbase.dll`, which in the
   guest is always Wine's (traced: `found L"kernelbase.dll" for L"api-ms-win-core-crt-l2-1-0.dll"`); sechost's
   DllMain called the `_initterm_e` stub. The GUI tests now run on `bin/machine-builtin`, the console's DLL set
   (Wine builtins + NLS, `"*": "builtin,native"`), and `run_guest.cmake` (`REQUIRE_BUILTIN_DLLS`) fails them if
   the `+modules` trace shows any system DLL mapped from outside `Windows\builtin` (docs/TESTING.md).

## CI: the whole suite gates on real Windows; task #12 closed (2026-10-22)

**Measured first.** `ci.yml` run 39818491081 (`88dc49d`) was green, but its windows (Debug) job had found no wine-pe
run ("no successful wine-pe run yet") and SKIPPED all 14 end-to-end tests, while the RelWithDebInfo job of the same
run did get the builtins. In that one job the "non-blocking" end-to-end step showed that **the guest's stdout is
captured on real Windows**: `integration.guest_hello` and 10 more `guest_e2e` tests passed on their stdout assertions.
The recorded "stdio is not yet captured" gap (task #12) no longer existed; what kept the tests from gating were two
real bugs, both hidden by the non-blocking step:

* `integration.driver_ioctl`: `FAIL kernel timer: ok=1 driver_ms=127 wall_ms=8`. Wine's `GetTickCount` /
  `GetTickCount64` / `NtGetTickCount` / win32u's message clock ignore `KUSER_SHARED_DATA.TickCountMultiplier`
  (Wine's server keeps `TickCount` in ms with multiplier `1 << 24`); on a Windows host the guest reads the host's page,
  where `TickCount` counts 15.625 ms ticks (127 / 15.625 = 8). Every guest tick count ran 15.625x slow on Windows and
  on the console. Fixed by `wine-pe/patches/0002-honour-tick-count-multiplier.patch`.
* `integration.guest_view_violation_detected`: c0000005 reading 0x104 inside `win10xbox.exe`. The test hook touched a
  function-local static inside the guest-view window, i.e. it performed on MSVC the very fault its detector exists
  to report. On MSVC the hook now records the violation without the access (`core/dispatch/dispatcher.cpp`).

**Changed.** Both MSVC configs run the full ctest suite (no label excluded, no `continue-on-error`); linux-cross also
runs `wine_conformance` gated (Wine sources cloned in seconds); a gate after every ctest fails on any skipped test or
a label below its floor, DISABLED tests excepted (`tools/ci/check_ctest_run.py`); the wine-pe download is mandatory
and waits for a wine-pe run of the same commit (`tools/ci/fetch_wine_pe.py`); every bundled guest (now including the
host-uwp-lite smoke guest in both subsystems and the driver clients) runs through the host with its exit code on the
Windows job and, under console conditions, in host-win32-smoke; host-uwp, uwp-lite, host-win32-smoke and
host-win32-appcontainer run on every push. `drivers/` now runs on Windows in full: `drivers.unit`, `drivers.layout`
(against the WDK, from the NuGet package; the image has no WDK), `integration.driver_ioctl` and
`integration.driver_kmdf_ioctl`, plus `drv_client` / `drv_kmdf_client` in both smoke runs. The WDK layout check found
one real difference: `DEVICE_OBJECT` is `DECLSPEC_ALIGN(16)` (size 0x150; `ntk.h` had 0x148 because mingw-w64's
header omits the alignment) - `ntk.h` now matches. Details: `docs/TESTING.md`, "What gates where".

**Evidence (run ids).** Before: 39818491081. Iterations: 39823615327 (linux-cross 54/54; windows: every guest and
driver_ioctl green on both configs, gates red on the missing WDK and on wine tests registered under MSVC),
39824978636 (layout_ddk.c vs the WDK's bit field), 39826229792 (everything green on Windows except drivers.layout:
`sizeof(DEVICE_OBJECT)` 328 vs 336). All green on `e23e82f`: ci 39827533313 (linux-cross 54 of 55 ran and passed, 1
DISABLED; each windows config 29 of 30 ran and passed, 1 DISABLED, 15 `guest_e2e`, 7 guests smoke-run), wine-pe
39827533416 (consumed by every job as "built from this commit"), host-win32-smoke 39827533375 (7 guests under console
conditions), host-win32-appcontainer 39827533182, host-uwp 39827533101, uwp-lite 39827533216.

**Not gating anywhere:** `wine_conformance` on the Windows jobs (MSVC cannot build Wine's tests; not registered) and
`integration.guest_window` (DISABLED by its author until core/user lands; the gate lists it as not run on purpose).

## Native user32/gdi32 inside an AppContainer (measured) (2026-10-22)

Question: can route B (forward NtUser*/NtGdi* to the host's own `win32u.dll`, so that the real win32k
manages windows) work inside an AppContainer, i.e. may a container process create, show and paint windows
with the native user32/gdi32, and can the pixels be read back? Measured by `tests/appcontainer/ac_gui_probe.c`
in `host-win32-appcontainer`, run [39827778745](https://github.com/user99672223/win10xbox/actions/runs/39827778745)
on `612c6fa` (green; the smoke verdict of that workflow is unchanged, the GUI runs are non-gating). The probe
imports only kernel32 at load time; user32 and gdi32 are delay-loaded and bound with `LoadLibraryW`
(`GUI.load.*`), so a context that cannot connect to win32k would report an error code instead of dying in the
loader. It calls user32/gdi32 directly; the layer's dispatcher was not involved.

Runs: `gui-plain` = `ac_launch --plain` (ordinary process, detached like a UWP app); `gui-ac` = genuine
AppContainer (launch log: `PROVENANCE OK`, same capabilities and grants as `probe-ac`); `gui-lpac` = LPAC
(identified by the token attribute `WIN://NOALLAPPPKG=present`: `TokenIsLessPrivilegedAppContainer` cannot be
queried, error 87, in every context, which is also why ac_launch prints `lessPrivilegedAppContainer=0` for the
LPAC child). `gui-direct` = the probe started straight from the runner's shell, outside ac_launch, as the
control for what the runner session allows at all (a GitHub runner session may have no interactive desktop):
its output is identical to `gui-plain` apart from pids, handles, addresses, the number of messages peeked and cursor coordinates (36 OK, 0 FAIL).
All four exited with code 0. What the runner session is, as measured: Windows build 26100, product type 3,
session 2, window station `WinSta0` with `WSF_VISIBLE`, thread desktop `Default` with `UOI_IO=1`, a 1024x768
32 bpp display; the container children inherit that desktop from the launcher.

plain (`gui-plain.log`):

```
PROBE GUI.probe | INFO | ac_gui_probe started
PROBE GUI.os | INFO | Windows 10.0 build 26100, product type 3
PROBE GUI.context | INFO | pid 2956, session 2, token: AppContainer=0 integrity=0x3000 TokenIsLessPrivilegedAppContainer=not queryable (error 87 (0x00000057) The parameter is incorrect); LPAC attribute WIN://NOALLAPPPKG=absent; security attributes: TSA://ProcUnique
PROBE GUI.dlls.at_start | INFO | user32.dll=no gdi32.dll=no gdi32full.dll=no win32u.dll=no uxtheme.dll=no comctl32.dll=no imm32.dll=no
PROBE GUI.load.user32 | OK | user32.dll at 00007FFD41CF0000
PROBE GUI.load.gdi32 | OK | gdi32.dll at 00007FFD425D0000
PROBE GUI.winsta.station | OK | window station "WinSta0", UOI_FLAGS dwFlags=0x1 -> interactive (WSF_VISIBLE)
PROBE GUI.winsta.desktop | OK | thread desktop "Default", UOI_IO=1 (this desktop receives user input)
PROBE GUI.winsta.input_desktop | OK | OpenInputDesktop: "Default"
PROBE GUI.register | OK | RegisterClassExW atom 0xC146
PROBE GUI.create.overlapped | OK | hwnd 0000000000030190 (style 0x00CF0000; WM_NCCREATE 1, WM_CREATE 1)
PROBE GUI.create.popup | OK | hwnd 0000000000060096 (style 0x80000000; WM_NCCREATE 1, WM_CREATE 1)
PROBE GUI.create.message_only | OK | hwnd 00000000000A007A (style 0x00000000; WM_NCCREATE 1, WM_CREATE 1)
PROBE GUI.show.showwindow | OK | ShowWindow(SW_SHOW) previously-visible=0, UpdateWindow=1, IsWindowVisible=1, IsIconic=0, GetForegroundWindow=0000000000030190 (this window), WM_PAINT so far 1
PROBE GUI.show.rects | OK | GetWindowRect=1 (50,50)-(450,350) GetClientRect=1 (0,0)-(384,261)
PROBE GUI.paint.getdc_fill_getpixel | OK | GetDC 0000000024010798, FillRect=1, GetPixel(10,10)=0x332211, painted 0x332211 -> match
PROBE GUI.paint.messages | OK | after 2000 ms: WM_PAINT arrived (2, rcPaint 0,0-384,261), WM_TIMER arrived (1), posted WM_APP+1 arrived (1, via PeekMessage 1); SetTimer(2000 ms) ok, PostMessage ok, 14 message(s) peeked, WM_ERASEBKGND 1
PROBE GUI.paint.post_getmessage | OK | PostMessageW -> GetMessageW returned it (wParam 0xBEEF, lParam 0xCAFE)
PROBE GUI.paint.sendmessage | OK | SendMessageW(WM_APP+2) returned 0x5A5A (wndproc saw it 1 time(s)) 
PROBE GUI.paint.after_wm_paint | OK | GetPixel(10,10) after the loop = 0x50A030 (WM_PAINT color 0x50A030, GetDC color 0x332211)
PROBE GUI.memdc.dib_fill | OK | CreateCompatibleDC(NULL) + CreateDIBSection 64x64x32 + FillRect=1: 4096/4096 pixels = 0x204080, pixel[0]=0x00204080
PROBE GUI.memdc.font | OK | DEFAULT_GUI_FONT 00000000018A06C3, face "MS Shell Dlg", GetTextMetricsW=1 height 13 ave width 5
PROBE GUI.memdc.textout | OK | TextOutW=1, 208 pixel(s) changed
PROBE GUI.memdc.drawtext | OK | DrawTextW returned height 13, 208 pixel(s) changed
PROBE GUI.memdc.font_families | OK | EnumFontFamiliesExW(DEFAULT_CHARSET): 974 face(s)
PROBE GUI.readback.printwindow_client | OK | PrintWindow(PW_CLIENTONLY) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.readback.printwindow_full | OK | PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.readback.bitblt_window | OK | BitBlt(window DC -> DIB) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.readback.bitblt_screen | OK | BitBlt(screen DC at client origin 58,81 -> DIB) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.sysmetrics.screen | OK | SM_CXSCREEN=1024 SM_CYSCREEN=768 SM_CMONITORS=1 SM_REMOTESESSION=0
PROBE GUI.sysmetrics.display_dc | OK | CreateDCW(DISPLAY): HORZRES=1024 VERTRES=768 BITSPIXEL=32 PLANES=1 LOGPIXELSX=96 VREFRESH=64 TECHNOLOGY=1
PROBE GUI.sysmetrics.monitors | OK | EnumDisplayMonitors returned 1, 1 monitor(s)
PROBE GUI.sysmetrics.dpi | OK | GetDpiForSystem=96
PROBE GUI.input.cursor | OK | SetCursorPos(200,200)=1; GetCursorPos=1 -> (200,200)
PROBE GUI.input.keystate | INFO | GetKeyState: VK_SHIFT=0x0000 VK_CAPITAL=0x0000 VK_LBUTTON=0x0000; GetAsyncKeyState(VK_LBUTTON)=0x0000
PROBE GUI.input.sendinput | OK | SendInput(mouse move +7,+7) inserted 1 event(s); GetCursorPos before (200,200) after=1 (208,208)
PROBE GUI.dlls.loaded | INFO | user32.dll=loaded(C:\Windows\System32\user32.dll) gdi32.dll=loaded(C:\Windows\System32\GDI32.dll) gdi32full.dll=loaded(C:\Windows\System32\gdi32full.dll) win32u.dll=loaded(C:\Windows\System32\win32u.dll) uxtheme.dll=loaded comctl32.dll=no imm32.dll=loaded
PROBE GUI.dlls.win32u_exports | OK | win32u.dll (already loaded): NtUserCreateWindowEx=exported NtGdiPatBlt=exported NtUserGetMessage=exported NtGdiCreateSolidBrush=exported
PROBE GUI.syscallstub.NtUserCreateWindowEx | OK | 4C 8B D1 B8 6F 10 00 00 F6 04 25 08 03 FE 7F 01 75 03 0F 05 C3 CD 2E C3; shape mov r10,rcx; mov eax,imm32; ... syscall at +18; syscall id 0x106F
PROBE GUI.syscallstub.NtGdiCreateSolidBrush | OK | 4C 8B D1 B8 AA 10 00 00 F6 04 25 08 03 FE 7F 01 75 03 0F 05 C3 CD 2E C3; shape mov r10,rcx; mov eax,imm32; ... syscall at +18; syscall id 0x10AA
PROBE GUI.destroy.windows | OK | DestroyWindow: overlapped=1, popup=1, message_only=1
PROBE GUI.destroy.class | OK | UnregisterClassW=1
PROBE GUI.summary | INFO | 36 ok, 0 fail (FAIL is a measurement, not an error)
```

AppContainer (`gui-ac.log`):

```
PROBE GUI.probe | INFO | ac_gui_probe started
PROBE GUI.os | INFO | Windows 10.0 build 26100, product type 3
PROBE GUI.context | INFO | pid 8068, session 2, token: AppContainer=1 integrity=0x1000 TokenIsLessPrivilegedAppContainer=not queryable (error 87 (0x00000057) The parameter is incorrect); LPAC attribute WIN://NOALLAPPPKG=absent; security attributes: TSA://ProcUnique
PROBE GUI.dlls.at_start | INFO | user32.dll=no gdi32.dll=no gdi32full.dll=no win32u.dll=no uxtheme.dll=no comctl32.dll=no imm32.dll=no
PROBE GUI.load.user32 | OK | user32.dll at 00007FFD41CF0000
PROBE GUI.load.gdi32 | OK | gdi32.dll at 00007FFD425D0000
PROBE GUI.winsta.station | OK | window station "WinSta0", UOI_FLAGS dwFlags=0x1 -> interactive (WSF_VISIBLE)
PROBE GUI.winsta.desktop | OK | thread desktop "Default", UOI_IO=1 (this desktop receives user input)
PROBE GUI.winsta.input_desktop | OK | OpenInputDesktop: "Default"
PROBE GUI.register | OK | RegisterClassExW atom 0xC146
PROBE GUI.create.overlapped | OK | hwnd 0000000000050196 (style 0x00CF0000; WM_NCCREATE 1, WM_CREATE 1)
PROBE GUI.create.popup | OK | hwnd 0000000000070096 (style 0x80000000; WM_NCCREATE 1, WM_CREATE 1)
PROBE GUI.create.message_only | OK | hwnd 0000000000050188 (style 0x00000000; WM_NCCREATE 1, WM_CREATE 1)
PROBE GUI.show.showwindow | OK | ShowWindow(SW_SHOW) previously-visible=0, UpdateWindow=1, IsWindowVisible=1, IsIconic=0, GetForegroundWindow=0000000000050196 (this window), WM_PAINT so far 1
PROBE GUI.show.rects | OK | GetWindowRect=1 (50,50)-(450,350) GetClientRect=1 (0,0)-(384,261)
PROBE GUI.paint.getdc_fill_getpixel | OK | GetDC 0000000024010798, FillRect=1, GetPixel(10,10)=0x332211, painted 0x332211 -> match
PROBE GUI.paint.messages | OK | after 2015 ms: WM_PAINT arrived (2, rcPaint 0,0-384,261), WM_TIMER arrived (1), posted WM_APP+1 arrived (1, via PeekMessage 1); SetTimer(2000 ms) ok, PostMessage ok, 13 message(s) peeked, WM_ERASEBKGND 1
PROBE GUI.paint.post_getmessage | OK | PostMessageW -> GetMessageW returned it (wParam 0xBEEF, lParam 0xCAFE)
PROBE GUI.paint.sendmessage | OK | SendMessageW(WM_APP+2) returned 0x5A5A (wndproc saw it 1 time(s)) 
PROBE GUI.paint.after_wm_paint | OK | GetPixel(10,10) after the loop = 0x50A030 (WM_PAINT color 0x50A030, GetDC color 0x332211)
PROBE GUI.memdc.dib_fill | OK | CreateCompatibleDC(NULL) + CreateDIBSection 64x64x32 + FillRect=1: 4096/4096 pixels = 0x204080, pixel[0]=0x00204080
PROBE GUI.memdc.font | OK | DEFAULT_GUI_FONT 00000000018A06C3, face "MS Shell Dlg", GetTextMetricsW=1 height 13 ave width 5
PROBE GUI.memdc.textout | OK | TextOutW=1, 208 pixel(s) changed
PROBE GUI.memdc.drawtext | OK | DrawTextW returned height 13, 208 pixel(s) changed
PROBE GUI.memdc.font_families | OK | EnumFontFamiliesExW(DEFAULT_CHARSET): 974 face(s)
PROBE GUI.readback.printwindow_client | OK | PrintWindow(PW_CLIENTONLY) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.readback.printwindow_full | OK | PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.readback.bitblt_window | OK | BitBlt(window DC -> DIB) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.readback.bitblt_screen | OK | BitBlt(screen DC at client origin 58,81 -> DIB) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.sysmetrics.screen | OK | SM_CXSCREEN=1024 SM_CYSCREEN=768 SM_CMONITORS=1 SM_REMOTESESSION=0
PROBE GUI.sysmetrics.display_dc | OK | CreateDCW(DISPLAY): HORZRES=1024 VERTRES=768 BITSPIXEL=32 PLANES=1 LOGPIXELSX=96 VREFRESH=64 TECHNOLOGY=1
PROBE GUI.sysmetrics.monitors | OK | EnumDisplayMonitors returned 1, 1 monitor(s)
PROBE GUI.sysmetrics.dpi | OK | GetDpiForSystem=96
PROBE GUI.input.cursor | FAIL | SetCursorPos(200,200)=0 error 5 (0x00000005) Access is denied; GetCursorPos=1 -> (208,208)
PROBE GUI.input.keystate | INFO | GetKeyState: VK_SHIFT=0x0000 VK_CAPITAL=0x0000 VK_LBUTTON=0x0000; GetAsyncKeyState(VK_LBUTTON)=0x0000
PROBE GUI.input.sendinput | OK | SendInput(mouse move +7,+7) inserted 1 event(s); GetCursorPos before (208,208) after=1 (215,215)
PROBE GUI.dlls.loaded | INFO | user32.dll=loaded(C:\Windows\System32\user32.dll) gdi32.dll=loaded(C:\Windows\System32\GDI32.dll) gdi32full.dll=loaded(C:\Windows\System32\gdi32full.dll) win32u.dll=loaded(C:\Windows\System32\win32u.dll) uxtheme.dll=loaded comctl32.dll=no imm32.dll=loaded
PROBE GUI.dlls.win32u_exports | OK | win32u.dll (already loaded): NtUserCreateWindowEx=exported NtGdiPatBlt=exported NtUserGetMessage=exported NtGdiCreateSolidBrush=exported
PROBE GUI.syscallstub.NtUserCreateWindowEx | OK | 4C 8B D1 B8 6F 10 00 00 F6 04 25 08 03 FE 7F 01 75 03 0F 05 C3 CD 2E C3; shape mov r10,rcx; mov eax,imm32; ... syscall at +18; syscall id 0x106F
PROBE GUI.syscallstub.NtGdiCreateSolidBrush | OK | 4C 8B D1 B8 AA 10 00 00 F6 04 25 08 03 FE 7F 01 75 03 0F 05 C3 CD 2E C3; shape mov r10,rcx; mov eax,imm32; ... syscall at +18; syscall id 0x10AA
PROBE GUI.destroy.windows | OK | DestroyWindow: overlapped=1, popup=1, message_only=1
PROBE GUI.destroy.class | OK | UnregisterClassW=1
PROBE GUI.summary | INFO | 35 ok, 1 fail (FAIL is a measurement, not an error)
```

LPAC (`gui-lpac.log`):

```
PROBE GUI.probe | INFO | ac_gui_probe started
PROBE GUI.os | INFO | Windows 10.0 build 26100, product type 3
PROBE GUI.context | INFO | pid 5444, session 2, token: AppContainer=1 integrity=0x1000 TokenIsLessPrivilegedAppContainer=not queryable (error 87 (0x00000057) The parameter is incorrect); LPAC attribute WIN://NOALLAPPPKG=present; security attributes: TSA://ProcUnique WIN://NOALLAPPPKG
PROBE GUI.dlls.at_start | INFO | user32.dll=no gdi32.dll=no gdi32full.dll=no win32u.dll=no uxtheme.dll=no comctl32.dll=no imm32.dll=no
PROBE GUI.load.user32 | OK | user32.dll at 00007FFD41CF0000
PROBE GUI.load.gdi32 | OK | gdi32.dll at 00007FFD425D0000
PROBE GUI.winsta.station | OK | window station "WinSta0", UOI_FLAGS dwFlags=0x1 -> interactive (WSF_VISIBLE)
PROBE GUI.winsta.desktop | OK | thread desktop "Default", UOI_IO=1 (this desktop receives user input)
PROBE GUI.winsta.input_desktop | OK | OpenInputDesktop: "Default"
PROBE GUI.register | OK | RegisterClassExW atom 0xC146
PROBE GUI.create.overlapped | OK | hwnd 0000000000160184 (style 0x00CF0000; WM_NCCREATE 1, WM_CREATE 1)
PROBE GUI.create.popup | OK | hwnd 0000000000080096 (style 0x80000000; WM_NCCREATE 1, WM_CREATE 1)
PROBE GUI.create.message_only | OK | hwnd 00000000000C007A (style 0x00000000; WM_NCCREATE 1, WM_CREATE 1)
PROBE GUI.show.showwindow | OK | ShowWindow(SW_SHOW) previously-visible=0, UpdateWindow=1, IsWindowVisible=1, IsIconic=0, GetForegroundWindow=0000000000160184 (this window), WM_PAINT so far 1
PROBE GUI.show.rects | OK | GetWindowRect=1 (50,50)-(450,350) GetClientRect=1 (0,0)-(384,261)
PROBE GUI.paint.getdc_fill_getpixel | OK | GetDC 0000000024010798, FillRect=1, GetPixel(10,10)=0x332211, painted 0x332211 -> match
PROBE GUI.paint.messages | OK | after 2000 ms: WM_PAINT arrived (2, rcPaint 0,0-384,261), WM_TIMER arrived (1), posted WM_APP+1 arrived (1, via PeekMessage 1); SetTimer(2000 ms) ok, PostMessage ok, 9 message(s) peeked, WM_ERASEBKGND 3
PROBE GUI.paint.post_getmessage | OK | PostMessageW -> GetMessageW returned it (wParam 0xBEEF, lParam 0xCAFE)
PROBE GUI.paint.sendmessage | OK | SendMessageW(WM_APP+2) returned 0x5A5A (wndproc saw it 1 time(s)) 
PROBE GUI.paint.after_wm_paint | OK | GetPixel(10,10) after the loop = 0x50A030 (WM_PAINT color 0x50A030, GetDC color 0x332211)
PROBE GUI.memdc.dib_fill | OK | CreateCompatibleDC(NULL) + CreateDIBSection 64x64x32 + FillRect=1: 4096/4096 pixels = 0x204080, pixel[0]=0x00204080
PROBE GUI.memdc.font | OK | DEFAULT_GUI_FONT 00000000018A06C3, face "MS Shell Dlg", GetTextMetricsW=1 height 13 ave width 5
PROBE GUI.memdc.textout | OK | TextOutW=1, 208 pixel(s) changed
PROBE GUI.memdc.drawtext | OK | DrawTextW returned height 13, 208 pixel(s) changed
PROBE GUI.memdc.font_families | OK | EnumFontFamiliesExW(DEFAULT_CHARSET): 974 face(s)
PROBE GUI.readback.printwindow_client | OK | PrintWindow(PW_CLIENTONLY) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.readback.printwindow_full | OK | PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.readback.bitblt_window | OK | BitBlt(window DC -> DIB) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.readback.bitblt_screen | OK | BitBlt(screen DC at client origin 58,81 -> DIB) returned 1; of 4096 pixels: 4096 = WM_PAINT color, 0 = GetDC color, 0 untouched (sentinel), 0 other; pixel[10,10]=0x30A050; WM_PRINTCLIENT +0
PROBE GUI.sysmetrics.screen | OK | SM_CXSCREEN=1024 SM_CYSCREEN=768 SM_CMONITORS=1 SM_REMOTESESSION=0
PROBE GUI.sysmetrics.display_dc | OK | CreateDCW(DISPLAY): HORZRES=1024 VERTRES=768 BITSPIXEL=32 PLANES=1 LOGPIXELSX=96 VREFRESH=64 TECHNOLOGY=1
PROBE GUI.sysmetrics.monitors | OK | EnumDisplayMonitors returned 1, 1 monitor(s)
PROBE GUI.sysmetrics.dpi | OK | GetDpiForSystem=96
PROBE GUI.input.cursor | FAIL | SetCursorPos(200,200)=0 error 5 (0x00000005) Access is denied; GetCursorPos=1 -> (215,215)
PROBE GUI.input.keystate | INFO | GetKeyState: VK_SHIFT=0x0000 VK_CAPITAL=0x0000 VK_LBUTTON=0x0000; GetAsyncKeyState(VK_LBUTTON)=0x0000
PROBE GUI.input.sendinput | OK | SendInput(mouse move +7,+7) inserted 1 event(s); GetCursorPos before (215,215) after=1 (223,223)
PROBE GUI.dlls.loaded | INFO | user32.dll=loaded(C:\Windows\System32\user32.dll) gdi32.dll=loaded(C:\Windows\System32\GDI32.dll) gdi32full.dll=loaded(C:\Windows\System32\gdi32full.dll) win32u.dll=loaded(C:\Windows\System32\win32u.dll) uxtheme.dll=loaded comctl32.dll=no imm32.dll=loaded
PROBE GUI.dlls.win32u_exports | OK | win32u.dll (already loaded): NtUserCreateWindowEx=exported NtGdiPatBlt=exported NtUserGetMessage=exported NtGdiCreateSolidBrush=exported
PROBE GUI.syscallstub.NtUserCreateWindowEx | OK | 4C 8B D1 B8 6F 10 00 00 F6 04 25 08 03 FE 7F 01 75 03 0F 05 C3 CD 2E C3; shape mov r10,rcx; mov eax,imm32; ... syscall at +18; syscall id 0x106F
PROBE GUI.syscallstub.NtGdiCreateSolidBrush | OK | 4C 8B D1 B8 AA 10 00 00 F6 04 25 08 03 FE 7F 01 75 03 0F 05 C3 CD 2E C3; shape mov r10,rcx; mov eax,imm32; ... syscall at +18; syscall id 0x10AA
PROBE GUI.destroy.windows | OK | DestroyWindow: overlapped=1, popup=1, message_only=1
PROBE GUI.destroy.class | OK | UnregisterClassW=1
PROBE GUI.summary | INFO | 35 ok, 1 fail (FAIL is a measurement, not an error)
```

**Verdict (measured, build 26100, runner session with an interactive desktop): YES.** Inside a genuine
AppContainer and inside an LPAC, the host's own user32/gdi32 (`C:\Windows\System32\user32.dll`, `GDI32.dll`,
`gdi32full.dll`, `win32u.dll`) loaded, `RegisterClassExW` succeeded, `CreateWindowExW` created an overlapped, a
popup and a message-only window, `ShowWindow`/`UpdateWindow` made the window visible and foreground, painting
through `GetDC` and through `WM_PAINT`/`BeginPaint` worked and `GetPixel` returned the painted colours, `WM_TIMER`,
a posted message (`PostMessageW` -> `GetMessageW`) and `SendMessageW` round-tripped, a DIB in a memory DC took
`FillRect` and text in `DEFAULT_GUI_FONT` ("MS Shell Dlg", 974 font faces enumerated), and the window's pixels
were read back by `PrintWindow(PW_CLIENTONLY)`, `PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT)`, `BitBlt` from
the window DC and `BitBlt` from the screen DC, 4096/4096 pixels equal to the `WM_PAINT` colour in each case. None
of these calls failed, so there is no error code to report for them; results are identical to the plain
control. The only difference from the control: `SetCursorPos` fails with error 5 (`ERROR_ACCESS_DENIED`) in the
AppContainer and in the LPAC, while `SendInput` of a mouse move succeeded there and moved the cursor. The host's
`win32u.dll` exports `NtUserCreateWindowEx`, `NtGdiPatBlt`, `NtUserGetMessage` and `NtGdiCreateSolidBrush`, and
its thunks have the `4C 8B D1 B8 <id> ... 0F 05` shape the loader rewrites (`NtUserCreateWindowEx` id `0x106F`,
`NtGdiCreateSolidBrush` id `0x10AA`, `syscall` at +18). Not measured here: the console itself, a UWP app's
process (package identity, CoreWindow, the Xbox's OS policy), and a session without an interactive desktop.

## host-uwp presents the composed frame; every guest is bundled (2026-10-22)

`host-uwp` now has a presenter (`host-uwp/presenter.cpp`, `blit.cpp`). It copies
`win10xbox::user::ComposeFrame` (core/user, `core/include/win10xbox/user.h`) into a D3D11 flip-sequential
swap chain on the CoreWindow and presents it from the UI thread's 60 Hz pump. The D3D11 device is created on a
thread of its own, because `D3D11CreateDevice` hangs on the UI thread of a Game-bucket app (DECISIONS.md). It tries
hardware, then WARP, and falls back to headless with a `display:` line. All of this is brought up before the Host's
worker starts. The Host hands its headless winexbox backend to core/user (`user::SetDisplayDriver`). The CoreWindow
size is converted to physical pixels for the swap chain, the virtual monitor and the pointer positions.
`LocalState\screen.bmp` is written every 5 s while the guest runs and when it has exited. The package bundles
`window.exe` (a placeholder GUI guest until `tests/guest/window.c` lands; CMake switches to that file automatically),
the `tests/guest` programs, `smoke_cui.exe` and `drv_client.exe`. The packaged `launch.json` runs `window.exe` and
keeps the app open on the last frame.

What CI proves (`host-uwp` workflow): it builds and packages. The new `host_uwp.blit` test (gating) runs the
presenter's upload + copy on a WARP device and checks the pixels it reads back. The package assertion checks every
bundled guest. The non-gating `run` job (run `39822419428`) installed the package on windows-latest and launched it,
and status.txt said `display.presenter=D3D11 hardware swap chain; screen 800x600, window 800x600 px; presents=2`, so
`CreateSwapChainForCoreWindow` + `Present` worked on a real CoreWindow there.

That run also exposed a **false PASS**, now fixed. Under the stub core/user, `CreateWindowExW` returned an NTSTATUS
as the hwnd (`0xC0000002`), `GetMessageW` failed at once, and the first placeholder `window.exe` fell through to
`return 42`. It now returns 42 only if it was painted and its own timer ended the loop (otherwise 3 or 4).

**What only the console can prove:** the hardware device in the Game bucket and the UI-thread hang rule, the TV
output, and, with core/user's `ComposeFrame` still a stub in this tree, that a window actually appears. Until then the
TV shows the presenter's dark-grey background.

## The GUI path, settled from the Wine source (2026-10-22)

Two successive statements about `win32u` were made in `docs/HANDOFF.md` and both are now replaced
by what `wine-pe/wine-src` (Wine 11.9, the pinned tree) actually contains:

1. *"GUI needs a second syscall table of ~1000 entries, a second dispatch surface plus a window
   manager"* (85769ba). The **dispatch surface** part was never true: the dispatcher already
   owns ids `0x1000..0x1fff` (`kMaxId`, `WIN32U_SYSCALL`), the loader already rewrites every
   `Nt*` thunk of every mapped builtin (`win32u.dll` included), and dispatch is by name. The
   "window manager" part was right (see 2).
2. *"win32u is a userspace PE that contains the window manager, message queues and dibdrv; there
   is nothing to build but `winexbox.drv` and a presenter"* (88dc49d). **Wrong, withdrawn.**
   Counter-evidence: `dlls/win32u/Makefile.in` declares `UNIXLIB = win32u.so` and lists
   `window.c`, `message.c`, `class.c`, `dce.c`, `dibdrv/*.c`, `gdiobj.c` ... as the sources of that
   **unix** library; `dlls/win32u/win32u.spec` has 1541 `@ stdcall -syscall` / `@ stub -syscall`
   lines and nothing else, i.e. the PE `win32u.dll` we ship is syscall thunks only
   (`core/loader/stubs.cpp` rewrites them; a run of `tests/guest/window.c` through the layer lists
   `unimplemented syscall NtUserInitializeClientPfnArrays, NtUserRegisterClassExWOW,
   NtUserCreateWindowEx, ...` — the thunks reach our dispatcher and nothing answers). The window
   manager, the queues and the GDI engine live in `win32u.so` plus wineserver, neither of which
   is built or shipped (`wine-pe/README.md`: "We never build or ship the unix side"). This is also
   what `docs/ARCHITECTURE.md` decision **E5** has said since M3: "`win32u.dll` in Wine is thunks
   only; the implementation lives in `win32u.so` + wineserver".

What follows from it: the win32k surface is the **EMULATE** class of the dispatcher, exactly as
ARCHITECTURE.md's dispatcher table lists it ("EMULATE -> user-mode implementation (processes,
registry, win32k, ...)"), implemented in the new `core/user` module against Wine's own PE-side
contract (`include/ntuser.h`, `include/ntgdi.h`). `winexbox.drv` is not needed: there is no Wine
`user_driver_funcs` caller to adapt, `core/user` calls `IWinexboxDriver` directly. Design:
`docs/GUI.md`. The syscall table now carries the 1540 win32u ids with argument counts
(`tools/gen_syscalls.py`, from `dlls/win32u/win32syscalls.h`); an entry is Emulate when
`core/user` registers it and Todo otherwise, never pass-through.

The lesson is the one HANDOFF.md already records as trap 4, made expensive twice: both
statements were written from plausibility, and the second one from the headers of our own
`winexbox/README.md` rather than from `wine-pe/wine-src`, which was one `fetch-wine.sh` away.

## Probe results folded in (2026-10-21)

The capability probe ran on an Xbox Series S in Dev Mode (build 26100, Game
bucket). All four load-bearing capabilities are confirmed: RWX + execute,
`NtCreateSection(SEC_IMAGE)` mapping, VEH trap-and-emulate (`rdmsr` `#GP` →
RIP adjust → continue), and canonical ntdll syscall stubs. `docs/DECISIONS.md`
is updated with evidence links; the probe-settled defaults are now in
`core/include/win10xbox/config.h` (real child processes, SEC_IMAGE mapping,
dispatch-by-name mandatory on build 26100, inbound sockets allowed, 3.5 GiB
guest ceiling, the D18 single CPU-identity story). Full report:
`tools/probe/results/REPORT.md`. Open items routed to probe-v2 and M6
(child-process resume, feature-level 11_0 vs 12_1, GLOn12/Dozen/Vulkan
bundling, WoW64 far jump).

## Console run of host-uwp-lite v1.0.0.4 (source `e7f25de`)

The throwaway validator `host-uwp-lite` ran on an Xbox Series S in SystemOS Dev Mode, OS build 26100.9445. Its build
`v1.0.0.4` is `uwp-lite` run #4 on `e7f25de`. The kernel32/kernelbase forwarder fix (`eb52c50`) is confirmed on hardware:
0 of 92 `KERNEL32.dll` imports are unresolved (the first run had all 92 dangling), `kernelbase.dll` loads as its own
module, the guest entry is reached, and the guest CRT runs through the full NLS/codepage initialisation (`l_intl`,
`c_1252`, `c_437`, `sortdefault`, `normnfc` all resolve) and writes its locale registry values.

The run then stopped at a separate defect whose cause is not yet established: an `EXCEPTION_ACCESS_VIOLATION` reading
address `0x4` at RIP `0x7ff6cb75a36c`. The faulting instruction is `mov eax,[rdx+rax]` with `rdx=4` and `rax=0`: a 32-bit
load off a near-null base, the shape a corrupted thread-local-storage pointer produces. It is not in any guest module.

**The faulting image is NOT identified, and two attributions have already been made and withdrawn.** The console report
gave the surrounding image as "base ~`0x7ff6cb758000`, size ~`0x14b000`", but `crash.txt`'s `AnnotateAddress` falls back
to `VirtualQuery` and prints a *protection region* rather than a module base whenever the address is not in the guest's
loader list — and while the guest runs, that list holds only guest modules. So that pair is a region inside some image and
any RVA derived from it is meaningless. Checked and ruled out: it is not a Wine builtin (none of the 23 in
`wine-pe/out/builtin` carries the faulting bytes `8b 04 02` at the implied offset, and Wine's `ntdll.dll` is
`SizeOfImage=0x302000`, far larger than `0x14b000`); and it is not the host exe at that offset either
(`win10xbox-uwp-lite.exe` from the `.msix` that actually crashed has `SizeOfImage=0x24f000`, `.text` VirtualSize
`0x1736c8`, and `cc cc cc` padding at that offset). The console and its crash files are no longer accessible, so this
report cannot be resolved further; the real-Windows CI reproduction below is the route to an unambiguous answer. The
lesson for the tooling: a crash record must print every loaded module's base **and** `SizeOfImage`, or an absolute RIP
cannot be attributed at all.

### A real bug, found and fixed — but NOT the cause of the real-Windows exit-139: recursion in `core/clock/clock.cpp`

> **Correction.** Commit `b97f7f0` is titled as though this fixed the real-Windows exit-139 crash. It did not.
> `host-win32-smoke` run #3 on `b97f7f0` still fails with exit 139. The bug below is genuine, reproduced, and now
> covered by a gating test, but it is a *Wine-host* defect: on real Windows the guest never reaches a `Sleep` at all
> (see the next section). The commit title stands in history; this is the correction.

`Clk_NtDelayExecution` resolved its "host" `NtDelayExecution` through a **lazy** `GetModuleHandleW(L"ntdll.dll")` inside
a function-local static (`clock.cpp:243-245`, called from `:502`). The first call therefore happened on the guest's
first `Sleep` — i.e. *after* the PEB takeover, when `PEB->Ldr` is Wine's — so it resolved the **guest** ntdll's
`NtDelayExecution`, which is a stub we rewrote to jump into the dispatcher. `Clk_NtDelayExecution` then called itself
until the stack ran out. Reproduced under Wine with the fix reverted: exit **139** plus
`err:virtual:virtual_setup_exception stack overflow 1792 bytes`.

**The real differential is which guest program runs, not which DLL set is staged.** `host-uwp-lite/smoke_guest.c:233`
calls `Sleep(20)`; no `guest_e2e` guest sleeps at all. So `host-win32-smoke` (which runs `smoke_cui.exe`) dies, while
linux-cross and the `ci.yml` Windows end-to-end step (which run `guest_hello`/`guest_cpuid`) pass. `integration.guest_sleep`
plus `tests/guest/sleep.c` now cover this as a **gating** linux-cross test, and it fails with exit 139 if the fix is
reverted. Fixed by latching the host ntdll in `clock::Initialize`, before the takeover; the same hazard in
`core/loader/internal.cpp` was latched too, and `dispatcher.cpp`'s `HostNtdll()` now errors if first resolved after
`Initialize`.

### Three claims about `TEB.TlsSlots` made here earlier, now WITHDRAWN

They were investigated because they are a genuine hazard class, but none of them caused this crash:

1. *"Wine's `loader_init` re-initialises the TLS bitmap, destroying the record that our index is in use."* **False.**
   `RtlInitializeBitMap` sets only `SizeOfBitMap` and `Buffer` and does not clear the buffer
   (`wine-src/dlls/ntdll/rtlbitmap.c:73-78`), and the buffer it is given is `peb->TlsBitmapBits` — storage *inside* the
   PEB that Microsoft's ntdll backs `PEB.TlsBitmap` with too (`loader.c:4450`). The allocators therefore share the bits.
   Proven on the real takeover path, not argued: `guest TLS bitmap 0x17009D3E8 (bits 0x7FFD0080, PEB.TlsBitmapBits
   0x7FFD0080, 64 slots): slot 19 already in use, allocators share the PEB bits`. This is already recorded as
   intentional in `docs/DECISIONS.md:97` (E2).
2. *"Wine claims and writes `TlsSlots[NTDLL_TLS_ERRNO]` = 16, so it clobbers our `ThreadState*`."* The three source
   lines are real (`loader.c:4455`, `ntdll_misc.h:36`, `thread.c:445` returning
   `(int *)&NtCurrentTeb()->TlsSlots[16]`), **but nothing in our configuration reaches that write**: no shipped builtin
   imports `ntdll._errno` (all 23 import tables scanned — `msvcrt`/`ucrtbase` export their own), and Wine's ntdll has no
   internal caller. Forcing index 16 under Wine (`WIN10XBOX_TEST_TLS_INDEX=!16`) runs to completion, exit 42, slot intact.
3. *"With the runner's genuine System32 DLLs, Wine's `loader_init` never runs."* **False.**
   `core/dispatch/process.cpp:288-299` always loads the guest ntdll from `<machine>\Windows\builtin` and fails if it is
   absent, while `import_windows_dlls.ps1` stages into `Windows\System32`, which the guest ntdll is never taken from.
   `loader_init` runs identically in both configurations.

The `TEB.TlsSlots` work was nevertheless kept, as **hardening and diagnostics rather than a fix**: `TlsSlots` really is
an inline TEB array that cannot be swapped per universe the way `ThreadLocalStoragePointer` and `FlsData` are
(`dispatcher.cpp:99-110`), so the slot is now taken at an index ≥ 19 (never 0 or 16), reserved in the guest's bitmap,
stored **tagged** so the pointer is validated arithmetically with no memory access before any dereference, backed by a
`ThreadState::magic` cookie, and repaired-and-reported if ever clobbered. The chosen index is now always traced on the
`dispatch` channel. `integration.guest_tls_slot_clobber` simulates the 4-byte store through `&TEB.TlsSlots[i]` and fails
if the tag check is disabled, confirming the clobber leaves a **non-NULL** survivor
(`FE7D7FFFFE7DF380` → `FE7D7FFF00000000`) that the old null check would have waved through.

**Process note, worth keeping.** The `clock.cpp` recursion had already been reported by an earlier subagent, and was
wrongly demoted here on the grounds that the CI log showed no `NtDelayExecution` relay lines after guest entry — absence
of evidence in a log truncated by the crash itself. Two successive misattributions of the console fault followed. The
corrective that worked was reproducing the failure locally under Wine rather than reasoning further from the report.

### Root cause: MSVC's thread-safe static guard reads TLS under the guest's TLS view (REPRODUCED)

This is the live bug, and it is now reproducible on CI without the console. `host-win32-smoke` run #3 on `b97f7f0`
(i.e. *with* the clock fix) still fails. From its `smoke-console.log` artifact, the trace ends:

```
NtContinue(000000b2732ff310,00000001) (may not return)
NoteGuestEntry guest entry reached (RtlUserThreadStart at 00000001700568B4)
DumpGuestModules [0] 0000000140000000 C:\guest\smoke_cui.exe
DumpGuestModules [1] 0000000170000000 C:\windows\system32\ntdll.dll
DumpGuestModules [2] 0000000178000000 C:\windows\system32\kernel32.dll
DumpGuestModules [3] 0000000174000000 C:\windows\system32\kernelbase.dll
NoteGuestEntry guest entry: TEB.TlsSlots[19] still holds our ThreadState
```

…and stops. What that establishes:

* **Zero syscalls after `NtContinue`.** The guest faults within its first instructions of real execution, before
  issuing a single syscall. So it is neither the `Sleep` path nor dispatcher recursion.
* Imports and module loading are **fine**: 0 unresolved, all four modules present at their expected bases.
* `TEB.TlsSlots[19]` is **intact**, so the TlsSlots collision theory is dead on real Windows too, not just under Wine.
* It fails **identically** with and without `WIN10XBOX_TEST_NO_HOST_PATHS=1`, so it is not AppContainer-specific.
* Under Wine the same path is **green** (`guest_e2e` 8/8). Real-Windows-only.

**This is very probably the same defect as the Xbox access violation**, which also showed no syscalls after guest entry
and a load off a near-null base (`mov eax,[rdx+rax]`, `rdx=4`, `rax=0`). Treat the console report and the smoke failure
as one bug until evidence separates them — which consolidates two unreproducible reports into one CI-reproducible one.

**The cause, reproduced** by building the real tree MSVC-ABI with clang-cl and running the smoke guest under Wine,
which reproduces the CI trace exactly:

* **Fault site:** `win10xbox::dispatch::detail::S()` — the function-local `static State s;` at
  `core/dispatch/dispatcher.cpp:69` — reached via `Host()` (`dispatcher.cpp:218`) from `Xl_NtContinue`
  (`core/dispatch/nt_process.cpp:289`).
* **Faulting bytes:** `48 8b 0c ca` then `3b 81 04 00 00 00`, with `rcx=0`: `rcx = TlsArray[tls_index]` is NULL, then
  `cmp eax,[rcx+4]`.
* **Mechanism:** MSVC's thread-safe function-local-static guard reads `_Init_thread_epoch`, which lives at **offset 4
  of the module's TLS block**, through `gs:[0x58]` (`TEB.ThreadLocalStoragePointer`). `Xl_NtContinue` calls
  `LeaveToGuestView()` *first*, installing the **guest's** TLS array — which has no block for the host exe. The guard
  read therefore hits `NULL+4`.
* **Why Wine/mingw is green:** mingw uses `__cxa_guard`, which touches no TLS at all. That one difference accounts for
  the entire real-Windows-only divergence, and for why our gating job could never see it.
* **The console AV is the same bug:** `mov eax,[rdx+rax]` with `rdx=4`, `rax=0` is the same guard sequence in MSVC
  codegen. Two unreproducible reports collapse into one CI-reproducible defect.
* **Confirmed by construction:** the same tree rebuilt with only `/Zc:threadSafeInit-` runs the guest to exit 42.
* The same ordering exists at `Xl_NtContinueEx`, `Xl_NtRaiseException` and `Xl_NtSetContextThread`, which also call
  `Host()` right after `LeaveToGuestView()`.

**The hazard is general, so the fix must close the class, not the instances.** Any host code reached while the guest's
TLS view is installed that touches a function-local static — or any other TLS — faults under MSVC, with a near-null
load that is very hard to attribute. Four call sites today; an unbounded number otherwise. The deeper problem is that
mingw *cannot* reproduce it, so the gating linux-cross job is structurally blind to this class; an invariant check that
fires under Wine (host state touched while the guest view is active is a bug by construction) is worth more than the
fix itself.

**Confirmed on the real `cl`/`link` toolchain**, `host-win32-smoke` on `a24b57a` (windows-latest). The crash record now
attributes the fault instead of leaving it unattributable:

```
at 0x00007ff674e8976c  = host module win10xbox.exe+0x3976c
access violation: reading 0x0000000000000104  = <unattributed; null pointer + 0x104>
bytes at rip-8 .. rip+16: 04 01 00 00 48 8b 04 c8 | 8b 04 02 39 05 bf c7 28 00 7f 0c ...
```

`48 8b 04 c8` is `mov rax,[rax+rcx*8]` — the TLS-array index — immediately followed by `8b 04 02`
(`mov eax,[rdx+rax]`), which is **byte-for-byte the instruction the console reported**. Symbolized from the uploaded
MSVC map: `S()+0x1c` (fault) ← `Host()+0x9` ← `CallWithArgs()+0x15d` ← `win10xbox_dispatch+0x345`, under
`Xl_NtContinue()+0x26`. The faulting read is `0x104` here and `0x4` on the console — the same mechanism at a different
TLS-block offset, so the exact offset is not significant.

The precise ordering, `core/dispatch/nt_process.cpp:289-295`:

```c
static NTSTATUS NTAPI Xl_NtContinue(PCONTEXT ctx, BOOLEAN alert) {
    NoteGuestEntry(ctx);                          // 290 — completes here, still the HOST view
    std::uint32_t d = LeaveToGuestView();         // 291 — installs the GUEST's TLS array
    NTSTATUS s = Host().NtContinue(ctx, alert);   // 292 — Host() → S() → guard → FAULT
```

`NoteGuestEntry` also appears in the stack scan, and it is instrumentation added in `eb52c50`, so it was checked as a
possible self-inflicted cause: it is **not**. It returns before the swap, so its return address is merely left on the
stack — the raw-x64-stack-scan caveat. The faulting ordering is 291 then 292 and predates all of this work. The same
`LeaveToGuestView()` → `Host()` ordering is at `Xl_NtContinueEx` (`:297-303`), `Xl_NtRaiseException` (`:305-309`) and
`Xl_NtSetContextThread`.

### FIXED, and the guest now runs to completion on real Windows

Fixed in `532a3bf` by closing the class rather than the four call sites: the dispatcher's process state is
`constinit State g_state;` at namespace scope (no guard, no TLS, and `constinit` makes the compiler reject any future
member that would reintroduce dynamic initialisation), and `CallInGuestView(Host().NtX, ...)` makes the host-view
resolution happen *by language rule* — arguments are sequenced before the call — rather than by the author remembering
an ordering. `/Zc:threadSafeInit-` was rejected on the merits: `Threads()`, `Objects()`, `ExportCache()` and the three
`trace` singletons are first touched from guest threads created at arbitrary times, so dropping the guard there is a
data race, and being process-wide it would mask the next instance instead of revealing it.

**Result on real Windows** (`host-win32-smoke` on `532a3bf`, windows-latest), in order from `smoke-console.log`:

```
hello from guest
guest: command line 'C:\guest\smoke_cui.exe'
trace:process:GuestExit guest exit, code 42
trace:process:RestorePebFields host PEB fields restored
trace:relay:Shutdown statistics: 188 calls to 34 syscalls
trace:core:Shutdown shutdown
```

98 relay lines after guest entry, where previously there were **zero**. An unmodified Windows x64 PE is loaded, its
imports bound, run to completion returning its own exit code, and the layer torn down cleanly — on real Windows, not
under Wine. That is the project's core claim, demonstrated.

**The hazard class is now gated where it was previously invisible.** mingw uses `__cxa_guard` and touches no TLS, so
linux-cross can never reproduce the fault itself — that structural blindness, not the single bug, is what let this
reach the console. `LeaveToGuestView` now sets `kThreadGuestView`, and `GuestViewActive()` reports the window open only
while that flag is set **and** `ThreadState::depth == 0` (that second condition is load-bearing: keying on the flag
alone left it set for the thread's life, because `NtContinue` never returns, and silently suppressed 645 trace lines).
`NoteHostStateUnderGuestView()` covers `dispatch::S`, `ExportCache`, `Objects`, `Threads`, `process::G`,
`dispatch::Views`, `dispatch::CpuPolicy` and a probe `trace::Log` consults, so even a stray `TRACE` in the window is
dropped and counted rather than faulting. Verified gating by reinstating the original ordering: `integration.guest_view_clean`
then **fails** on linux-cross with `err:dispatch: host state reached under the guest TLS view 1 times (last:
dispatch::S (Host, Lookup, ...))`. Suite is 21/21, `guest_e2e` 10/10 (21 tests, from 14).

### The AppContainer surface is now MEASURED on real Windows, not simulated

`.github/workflows/host-win32-appcontainer.yml` runs the layer inside a genuine AppContainer on a Windows runner
(`CreateAppContainerProfile` + `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES`), with the child's token read back while it
is suspended and the run aborted unless it really is in the requested container. Validity is established by **negative
controls**: reading an ungranted file and writing `C:\Windows\Temp` both succeed outside and are **denied inside**
(`error 5`), so the sandbox is genuinely enforced rather than nominally requested. Run `39786498188` on `4e1131f`:
**`PASS (genuine AppContainer, zero unresolved KERNEL32 imports, guest exit code 42)`** — now **gating**.

Three measurements, two of which settle long-standing guesses in opposite directions:

**1. `GetFinalPathNameByHandleW(VOLUME_NAME_DOS)` is REFUSED inside a stock AppContainer** — `error 5`, both AC and
LPAC, while it succeeds outside. So the console's refusal, which was the entire root cause of the kernel32/kernelbase
forwarder bug, is **generic AppContainer behaviour, not Xbox-specific**. The layer hits it for real:
`smoke-ac` (container, **no** test hook) logs 3 `host path of image file ... unavailable (Win32 error 5)` warnings
against 0 in `smoke-plain`. That means the real condition is now exercised in CI and can replace the
`WIN10XBOX_TEST_NO_HOST_PATHS=1` simulation as the gate.

The mechanism is identified too, and it is actionable. Opening `\\.\MountPointManager` is denied in the container, which
is why *DOS*-path resolution fails — and the variants show what still works:

| Call | In AppContainer |
|---|---|
| `GetFinalPathNameByHandleW` DOS (normalized / opened) | **FAIL** (error 5) |
| `GetFinalPathNameByHandleW` GUID volume | **FAIL** (error 5) |
| `GetFinalPathNameByHandleW` **NT** path | **OK** — `\Device\HarddiskVolume5\…` |
| `GetFinalPathNameByHandleW` none/normalized | **OK** — `\a\win10xbox\…` |
| `FileNameInformation`, `NtQueryInformationFile` | **OK** |
| `NtQueryObject(ObjectNameInformation)` | **OK** — `\Device\HarddiskVolume5\…` |

So a real host path *is* obtainable inside an AppContainer by asking for an NT path instead of a DOS one. That would let
`HostPathOfHandle` work there rather than falling back to the size+contents comparison in `SameImageFile`.

**2. `dbghelp.dll` BINDS inside a genuine AppContainer** — `LoadLibrary` OK and a real self-dump written, in AC and
LPAC, and the host's own `crash.cpp` reports `dbghelp.dll bound: crash.dmp enabled`. So the console's failure to bind it
is **not** generic AppContainer behaviour. **Nor is it Xbox behaviour** — corrected by the hardware run of `host-uwp` 1.0.0.8, where `crash.cpp` reported `dbghelp.dll bound: crash.dmp enabled` on the console itself. So the earlier `0x800736b2` from host-uwp-lite 1.0.0.4 was *not* a general inability to bind dbghelp on Xbox, and the prediction made here that the console could not bind it was **wrong**. Console crash dumps are available; Device Portal per-app dump collection is also enabled now.

**3. The smoke guest reaches exit 42 inside the container**, with 0 unresolved imports, matching `smoke-plain` and
`smoke-plain-console` (the two controls that separate a console effect from a container effect, since the container
child is launched detached as a UWP app is).

Runner was Windows 10.0 build **26100** (server SKU); the console is 26100.9445 `xb_flt`, i.e. the same base build.
What this still cannot cover: package identity, WinRT activation, and Xbox OS policy — a failure here is conclusive that
behaviour is generic, a pass is not proof the Xbox agrees.

### FIXED: the host's exit code is the guest's, and `host-win32-smoke` is GREEN

Fixed in `c1e4466`. `host-win32-smoke` on that commit: **`PASS (zero unresolved KERNEL32 imports, guest exit code 42)`**
in both the console-conditions and desktop runs. An unmodified Windows x64 PE is loaded, run to completion, and the host
returns the guest's own exit code — on real Windows.

The real cause was a **leaked `PEB.ActivationContextData`**: the guest ntdll overwrites it and nothing restored it, so
Microsoft's SxS code found Wine's own static `system_actctx` there during `ExitProcess`'s DLL detach. That had been
incomplete since the PEB takeover was written; it was never a regression. The faulting chain, symbolized from the run's
own map and pdb, is `__scrt_common_main_seh` → `common_exit` → `exit_or_terminate_process` → `ExitProcess` →
`LdrShutdownProcess` → `combase` → ntdll, and the new record confirms the phase directly: *"EXITING — ntdll reports DLL
shutdown in progress (`RtlDllShutdownInProgress`)"*.

A five-variant matrix in the job settles fix-versus-mask rather than leaving it to judgement:

| Variant | Exit path | ActCtx restored | Native status |
|---|---|---|---|
| `version-default` | new | yes | `0x00000000` |
| `version-normal-exit` | old | yes | `0x00000000` |
| `guest-default` | new | yes | `0x0000002A` (42) |
| `guest-normal-exit` | old | **yes** | **`0x0000002A` (42)** |
| `guest-normal-leak` | old | **no** | **`0xC000000D`** |

`guest-normal-exit` clean and `guest-normal-leak` faulting is the controlled comparison: the activation-context restore
is the actual fix, and the `TerminateProcess` hard exit (`main.cpp`'s `EndProcess`) is defence in depth rather than a
mask. `version-normal-exit` being clean also establishes that the fault requires a guest at all. The matrix stays in the
job permanently, so a future regression in the *normal* exit path is still visible even though the shipping path no
longer uses it.

Two incidental corrections, both of earlier guesses recorded here: the 180-second stall was **not**
`MiniDumpWriteDump` — `ExitProcess` terminates every other thread before DLL detach, so the crash thread was already
dead and `PostJob` sat on its 180s timeout (the two records are 180.25s apart and no `crash.dmp` was ever written);
`PostJob` now waits on the thread handle too. And Git Bash's `127` was its own rendering of an NT status: the raw status
is `0xC000000D`.

### (historical) OPEN: the host's own exit code is 127, not the guest's 42

`host-win32-smoke` is still red, but for a different and much narrower reason: after the clean shutdown above, the main
thread raises an **unhandled `0xc000000d` (`STATUS_INVALID_PARAMETER`)** at `ntdll.dll+0x7e55f`, with
`combase`/`KERNELBASE`/`RPCRT4` on the stack and `runs guest code: no`, and `win10xbox.exe` exits **127** instead of
42. The bytes at RIP are `eb 00` right after an `e8` call, and `0x00000080c000000d` sits on the stack, which looks like
`RtlRaiseStatus` machinery rather than a memory fault. The step also took ~3 minutes, consistent with
`MiniDumpWriteDump` running now that dbghelp is available (it was refused inside the console AppContainer).

This is a **host exit-path** defect, not a layer defect, and it is newly *reachable* rather than newly broken — before
`532a3bf` the guest never got far enough to reach teardown. Note that 127 needs its own explanation: Git Bash reports a
Windows status modulo 256, and `0xC000000D mod 256` is 13, not 127.

No `crash.dmp` was written. The dump path reported HRESULT `0x800736b2`, the form `crash.txt` uses when `dbghelp.dll`
cannot be bound inside the AppContainer (to be confirmed against `crash.txt`). `dbghelp.dll` is delay-loaded, so the
package still ran, and `crash.txt` and `entry-reached.txt` were produced: the first on-console run to leave crash
evidence (crash capture was added in `e7f25de`, see `host-uwp-lite/README.md`).

A similar failure shows up on real Windows. `host-win32-smoke` run `39667248899` on `e7f25de` fails its gate: its
console-conditions run and its desktop sanity run both report `KERNEL32 unresolved: 0` and exit `139` (128+11, a SIGSEGV
as Git Bash reports it; the gate expects 42). It is believed to be the same defect, but the fault address on real Windows
is not recorded, so that link is not confirmed.

## Milestone commits

Annotated tags `m0`/`m1`/`m2` are created in the repo; if a tag is not visible
on the remote it is because this session's git credentials pushed branches but
were refused tag refs (HTTP 403) — re-push the tags with full credentials.
Milestone commits, so they stay pinpointable regardless:

| Tag | Commit | Milestone |
|-----|--------|-----------|
| m0  | `cbcfbab` | repo skeleton, submodules, core + host-win32 hello, CI |
| m1  | `fc18910` | loader (PE map, stub rewrite, scanner) — green CI |
| m2  | `fc18910` | dispatcher + subsystems (VFS/registry/clock/token/NLS) — green CI |
| m3  | `9e22180` | Wine PE set, winexbox interface + headless backend, Wine conformance baseline — green CI |
| m4  | `692037b` | driver host (user-mode ntoskrnl, object/IRP engine, KMDF shim) + privileged-instruction trap-and-emulate — green CI on both toolchains. The M4 content landed in `a8c4074`, whose `ci` run #19 was red; `692037b` is the first commit where CI went green with it, so that is the milestone point. |
| m5  | `7aac27a` | real UWP host (`host-uwp/`): app shell, Host, standalone build, signed `.msix` built by CI — green CI, not yet run on hardware |


## M5 - real UWP host (`host-uwp/`)

`host-uwp/` is the real win10xbox UWP host: a headless C++/WinRT `CoreApplication` shell (`main.cpp`, `App.cpp`, `App.idl`)
around the Host of `host.h`. The Host reads `LocalState\launch.json`, stages the machine folder in `LocalState\machine`,
calls `Initialize`, runs the configured guest through `process::Launch` on a worker thread, and writes its verdict to
`LocalState\status.txt`. Its evidence trail is plain files in `LocalState` (`trace.log`, `stdout.txt`, `crash.txt`,
`crash.dmp`, `entry-reached.txt`). It is a standalone CMake project: it `add_subdirectory()`s `third_party/`, `core/` and
`winexbox/` itself, and the top-level tree refuses it (`host-uwp/CMakeLists.txt:41` raises `FATAL_ERROR` if it is added
there), so `-DWIN10XBOX_BUILD_UWP=ON` is not a way to build it.

Commit `4db0789` added the app shell and build scaffolding as a WIP checkpoint marked unverified: `App.idl`, `App.cpp`,
`App.h`, `main.cpp`, `host_shell.cpp`/`.h`, `CMakeLists.txt`, `Package.appxmanifest`, `pch.h`, `sdk.cmake`, `stage.cmake`,
`package.cmd`, `crash.cpp`/`.h` (carried over from host-uwp-lite), `guest/hello.c`, `payload/launch.json`,
`payload/launch.example.json` and `.gitattributes`. The Host sources (`host.cpp`, `host_diag.cpp`, `host_stage.cpp`,
`host_util.cpp`, `launch_config.cpp` and `selftest/`, with their headers) came in the earlier WIP checkpoints `d223773` and
`295fae4`. Commit `7aac27a` added `.github/workflows/host-uwp.yml`. The package identity is `win10xbox.uwp` (exe
`win10xbox-uwp.exe`, WinRT namespace `Win10XboxUwp`), so it installs side by side with host-uwp-lite. See
`host-uwp/README.md`.

CI: workflow `host-uwp` run #1 (run `39681539555`) on `7aac27a` completed `success` on its first attempt. Configure, build,
package, the portable launch.json selftest and the artifact upload all passed. Artifact `host-uwp-msix` (21,794,185 bytes,
about 20.8 MiB) holds the signed `.msix` and the public `.cer` needed to trust it for side-loading. It is not yet run on
hardware. No tag is made for m5 here (tags cannot be pushed from this session); the SHA in the table is the record.

## M4 - driver host (`drivers/`)

`drivers/` loads kernel-mode drivers in user mode and routes guest device I/O to them. A WDM driver
(`OurTestDevice.sys`, freestanding C against `drivers/include/ntk/ntk.h`) is mapped by `core/loader`, its imports are
bound to a ~400-function user-mode ntoskrnl, `DriverEntry` builds `\Device\OurTestDevice` + `\??\OurTestDevice`, and a
guest `CreateFile`/`DeviceIoControl` (buffered, direct, neither, pending-from-DPC, timer, overlapped) travels
Wine ntdll -> dispatcher -> `Drv_NtDeviceIoControlFile` -> IRP -> `IoCallDriver` -> driver -> `IoCompleteRequest` -> output
copy-back, verified end to end under Wine (`integration.driver_ioctl`). KMDF drivers built with Microsoft's public
headers run on a minimal WDF function-table shim (`integration.driver_kmdf_ioctl`). `drivers.unit` has 27 cases.
Green on both CI jobs (commit `52d39be`): the whole `drivers/` tree + the freestanding test drivers build under
**MSVC** (windows-latest, Debug and RelWithDebInfo) and **llvm-mingw**; `drivers.unit` + `drivers.layout` gate on
both, and the IOCTL round-trip (`integration.driver_ioctl`, `integration.driver_kmdf_ioctl`, label `driver_e2e`)
is a hard gate on the Wine job and, since 2026-10-22, on both Windows jobs too (see the CI section at the top; the
"capture gap" this line used to cite turned out not to exist).
Privileged-instruction trap-and-emulate landed (`drivers/host/privileged.cpp`, commit `a8c4074`): a VEH armed via
`drivers::SetImageLoadHook` that, for a fault inside a loaded driver image, decodes with Zydis and emulates the
instructions privileged at CPL 3 — `mov cr8` <-> virtual IRQL (the inline `KeGetCurrentIrql`/`KeRaiseIrql`/
`KeLowerIrql`), `rdmsr`/`wrmsr`, `in`/`out`, `cli`/`sti`, `rdpmc`, `hlt`/`wbinvd`/`invd`/`invlpg` — then resumes.
`OurTestDevice`'s `IOCTL_OTD_PRIV` exercises it from kernel mode; verified under Wine (`cr8=1 msr=1 portio=1
clisti=1`), the same `#GP`->VEH->resume path the probe proved on the console. Gates on linux-cross and (since
2026-10-22) on the Windows jobs. Follow-ups (`drivers/README.md`): the kernel-VA `KUSER_SHARED_DATA` mirror read and gs-relative
KPCR fields — memory accesses needing the virtual KUSER/KPCR served, not a one-instruction emulation. Also not
done: PnP/power, real KMDF framework.

## M0 - repo skeleton (tag `m0`)

Layout created per `docs/ARCHITECTURE.md`; the capability probe moved to
`tools/probe/` untouched. Eleven third-party projects pinned as shallow
submodules, Wine and ReactOS as pinned-tag fetches. CMake builds `core/`
(static lib with config + tracing), `host-win32` ("hello"), and the unit/
integration test harness with both MSVC and llvm-mingw. CI: `linux-cross`
(llvm-mingw + Wine smoke) and `windows` (MSVC Debug/RelWithDebInfo, sccache,
machine folder populated from the runner's System32 at test time).

## M1 - loader (tag `m1`)

`core/loader` manually maps x64 PE images (sections, relocations, protections,
host `.pdata` registration), resolves exports/forwarders, rewrites the guest
ntdll's syscall thunks to enter our dispatcher, and scans executable code with
Zydis to redirect `cpuid`/`rdtsc`/`rdtscp`/`syscall` to trampolines (inline
near-jump where the displaced bytes relocate safely, `int3`+VEH otherwise).
Validated under Wine on the real Wine-built `ntdll.dll`: all 258 syscall
thunks matched and were rewritten. Guest test programs (`tests/guest`) build
with the host toolchain; `test_loader` has 21 cases. PE/loader corner cases
and the two scanner strategies are documented in `docs/LOADER.md`. SEH-based
tests are MSVC-only (clang-mingw has no `__try`), so the mingw smoke build
stays green.

## M2 - dispatcher + subsystems (tag `m2`)

`core/dispatch` routes every guest syscall through a table generated from
Wine's syscall numbers (`tools/gen_syscalls.py`, 258 entries) plus a
hand-maintained policy (`core/dispatch/syscall_policy.inc`): 163 pass-through
to the host ntdll by name, 29 translate (paths/handles/timeouts), 45 emulate,
21 todo (loud `STATUS_NOT_IMPLEMENTED`). Per-channel relay tracing logs every
call with args and NTSTATUS. `SEC_IMAGE` sections are emulated via the loader;
63 process-wide `Rtl*` routines are forwarded to the host ntdll (heap, locks,
exception/unwind - see `DECISIONS.md` E2); the guest's eight Wine unix calls
are answered. Subsystems: VFS (machine-folder path translation + native/
builtin DLL routing), registry (own bounds-checked REGF reader/writer + write
overlay over supplied hives), virtual clock (QPC/TSC/system time +
KUSER_SHARED_DATA page), token (interactive medium-integrity user identity).
`process::Launch` boots Wine's PE ntdll inside the host, loads kernelbase/
kernel32/ucrtbase from the machine folder, and runs the program.

End-to-end under Wine: `win10xbox C:\guest\guest_hello.exe` prints
`hello from guest` and exits 42; `guest_cpuid` sees `AuthenticAMD` with the
hypervisor-present bit cleared (natively the box is Intel with the bit set),
proving the scanner patches are live. `SYSCALLS.md` is generated; the
per-syscall status table is the fidelity ledger going forward.

### CI gating and real-Windows status

- The whole tree builds and links under **MSVC** (windows-latest, Debug and
  RelWithDebInfo) and under **llvm-mingw**; `unit.core` (109 cases across
  trace/vfs/registry/clock/token/loader/dispatch) is green on both.
- The guest **end-to-end** tests (`integration.guest_hello`, `guest_cpuid`:
  boot a real PE through the full layer, check output and exit code) are a
  **hard gate on the Wine job** (`linux-cross`), which downloads the Wine PE
  builtins and stages the NLS data the emulated `NtGetNlsSectionPtr` serves.
- On a **real-Windows** host the guest boots through the entire Wine-ntdll
  init and its stdio IS captured; the end-to-end tests gate on both Windows
  jobs since 2026-10-22 (the CI section at the top of this file). The earlier
  claim here that stdio was not captured on real Windows is withdrawn: run
  39818491081 shows guest stdout matched on the windows RelWithDebInfo job.
