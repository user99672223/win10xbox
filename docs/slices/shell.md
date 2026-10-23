status: wip

## Handoff (implement agent 1 of 2: explorer.exe as the desktop of one process; child processes are agent 2's)
1. core/user/win_shell.cpp (new): one window station "WinSta0" + one desktop (reported as "Default" so explorer sets the shell window): NtUserCreateDesktopEx/Open*/GetThreadDesktop/SetThreadDesktop/GetProcessWindowStation/GetObjectInformation, NtUserSetShellWindowEx/SetProgmanWindow/SetTaskmanWindow (+ NtUserCallNoParam getters), NtUserDrawCaptionTemp (taskbar buttons, Start label), NtUserChangeDisplaySettings, SPI_SETWORKAREA stored, NtUserMessageCall ClipboardWindowProc / SystemTrayCall (-1: explorer's own tray), GetDeskPattern.
2. explorer's `CreateWindowEx(#32769)` / `Message` adopt the layer's desktop / message-parent window for the creating thread (AdoptSpecialWindow; IsPassiveWindow = flagged and tid 0): its window procedure now gets the desktop's messages; the desktop still paints as the flat COLOR_BACKGROUND in the composer.
3. Top-level create/destroy post WM_PARENTNOTIFY to the adopted desktop (what Wine's taskbar listens to) and WM_SHELLHOOK to hook windows (HSHELL_WINDOWCREATED before the first show, ACTIVATED, DESTROYED, REDRAW on a title change).
4. Wine 11.9's user32 RegisterShellHookWindow is a FIXME stub: wine-pe/patches/0021-user32-shell-hook-window.patch makes both calls NtUserCallOneParam codes 0x10001/0x10002 (kOneParamRegisterShellHook, user_internal.h). Needs the wine-pe.yml rebuild of user32.dll; locally rebuilt with `wine-pe/build.sh --only user32`.
5. Tests: integration.shell_desktop (explorer /desktop=shell,1920x1080, builtin-only folder, screenshot pixels: black desktop, 20 px taskbar f0f0f0 at y 1060.., Start button frame) and integration.shell_desktop_hook (tests/guest/shellhook.c, exit 42; also in host-uwp HOST_UWP_GUESTS and host-uwp.yml). Both pass under Wine locally; the rest of the local suite is unchanged (local-only failures: guest_identity WMI, guest_nowine/wasapi/xaudio2/regpersist_read from the stale local builtins).
6. Untested on Windows: both new tests on the MSVC and mingw jobs (the taskbar height 20 and the Start button x=67 come from the builtin font metrics, same on both); host-uwp build of guest_shellhook.
7. Known gap for agent 2: explorer's add_taskbar_button skips windows whose GetWindowThreadProcessId pid equals its own, so top-level windows of this process get no taskbar button; the buttons appear once programs are separate (virtual) processes with their own pids. No taskbar button is tested yet; the Start menu and tray icons (WM_COPYDATA to Shell_TrayWnd) are untouched and untested.
8. Debug: `WIN10XBOX_DEBUG=-all,+user,+dispatch` shows `unimplemented syscall NtUser*`; run explorer with `win10xbox.exe --machine build-mingw/bin/machine-builtin C:\Windows\System32\explorer.exe /desktop=shell,1920x1080` and WIN10XBOX_TEST_INJECT_INPUT="waitwin 30000;sleep 3000;shot;quit 0".
9. Commands: `cmake --build build-mingw -j4; ctest --test-dir build-mingw -R 'integration.shell_desktop'`.

# Slice: shell (desktop) — design question for Session A before any code

Session B, 2026-10-23. Not started; waiting for Session A's answer here (edit this file on this branch or
answer in the COORDINATION.md log).

## The fact that blocks a straightforward design

core/user (the window manager, message queues, GDI, `ComposeFrame`) lives inside each host process. A program
started with `CreateProcess` is a separate host process (`NtCreateUserProcess`, docs/DISPATCHER.md "Child
processes") with its own private screen; host-uwp refuses children altogether. So a shell (Wine's `explorer
/desktop`, or our launcher grown into one) can never see, place or switch the windows of programs it launches,
and the launcher can only run one program at a time. Today's "desktop" is: launcher list -> one program with
its windows and the drawn cursor -> result screen.

## Questions

1. Which architecture do you want for `-shell`?
   a. A shared window-manager server: one process (or shared memory + a lock) that every guest process attaches
      to for windows, z-order, input routing and composition, wineserver-style. The real desktop; a large slice
      (new IPC, every NtUser*/NtGdi* path that touches shared state); it also answers the UWP child-process
      question only if the console lets children run (probe-v2 item 1, still open).
   b. In-process virtual processes (DECISIONS.md D1's fallback): the shell launches programs into its own host
      process (separate PEB view, loader lists, heaps as far as Wine's ntdll allows, one core/user). Smaller;
      works on the console today; a crash in one program takes the desktop down, and process isolation
      (handles, address space) is by convention.
   c. Wait: ship the launcher + single program + cursor as the desktop for the first on-console test of the
      current package, and decide (a) or (b) from what the hardware shows.
2. My queue after `-net2` (implement agent running): is it `-shell` (once answered), then anything else you want
   ahead of it? `-notells` (Wine-only exports/strings/keys hidden from guests, gated guest) is running from the
   owner's request; keep or drop?
3. The ntdll fidelity rows are all ready on their branches (`-wt-ntdll-reg`, `-wt-ntdll-file`, `-wt-ntdll-rest`
   merged at 654dabd/7bd4278). The `reg` unit sits at 1 failure (a win_skip under the Wine platform) and cannot be
   gated by parity either (6 RtlQueryRegistryValues lines native passes): do you want a per-unit failure budget in
   the allowlist, or leave it recorded?
