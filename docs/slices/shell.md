status: wip

## Handoff (implement agent 2 of 2: virtual processes under the desktop; for the CI-loop agent)
Half 1 kept explorer.exe as the desktop (the table row); half 2 implements Session A's option (b) under it.
1. `CreateProcess` while `user::ShellActive()` (explorer adopted the desktop window or set the shell window) starts a virtual process: a second guest view in the same host (`core/dispatch/vprocess.h`, code at the end of `process.cpp`; D1-b in docs/DECISIONS.md, "Child processes" in docs/DISPATCHER.md). Otherwise the host child (`integration.guest_spawn`) is unchanged.
2. Per process: second guest ntdll mapping (relocated), private PEB (copy of the host's pre-take-over PEB), parameters, pid 1000+4n, exit event. Threads carry `ThreadState::vp/guest_peb/vpid`; `SwapIn`/`SwapOut` (and the `win10xbox_dispatch` wrapper) switch TEB.ProcessEnvironmentBlock and ClientId.UniqueProcess. The asm entry stubs are untouched.
3. Handles: process = duplicate of a host event (waitable), thread = the real host thread; tracked in a table through NtClose/NtDuplicateObject; NtQueryInformationProcess(ProcessBasicInformation, image), NtTerminateProcess, ThreadAmILastThread are per virtual process. core/user: `WindowObj::pid`, `user::ProcessExited`, `user::ShellActive`.
4. Gate: `integration.shell_launch` (explorer /desktop=shell,1920x1080 C:\guest\guest_shell_launch.exe; tests/guest/shell_launch.c opens a 300x200 window at 600,100 and CreateProcess-es guest_window.exe 400x300 at 100,100). Pixels: blue 0050dc (300,250), red (130,130), green 00a000 (750,200), black desktop (550,150), taskbar f0f0f0 (1000,1070), active button 99b4d1 (400,1075). Added to host-uwp CMake/yml (guest_shell_launch).
5. Passes locally under Wine; printed `shell_launch: own pid 1000 window ...`, `shell_launch: child pid 1004 window ok`; screenshot 1920x1080 shows both windows and a taskbar button per process ("shell_launch", "win10xbox" active).
6. Untested on Windows: integration.shell_launch on both MSVC configs (a relocated second ntdll and EXE mapping on the real loader; the dispatch wrapper's TEB writes with MSVC TLS; button pixel x=400 comes from the builtin font metrics), host-uwp build of guest_shell_launch.
7. If shell_launch fails on Windows, first suspects: the second ntdll mapping/relocation (`MapGuestNtdll` log line `guest ntdll at`), `CreateVirtualProcess` trace lines (`trace:process`), and whether `GuestViewViolations` is nonzero (new namespace-scope table `vprocess::table` is noted with NoteHostStateUnderGuestView).
8. Known gaps: no memory/crash isolation, one shared host heap, `NtOpenProcess(pid)`/`GetProcessTimes` of a virtual pid fail, a process killed from outside runs no DLL detach, two processes registering one class name with one hInstance clash, no Start menu or tray icons tested.
9. Debug: `WIN10XBOX_DEBUG=-all,+process,+user,+dispatch`; run `win10xbox.exe --machine build-mingw/bin/machine-builtin C:\Windows\System32\explorer.exe /desktop=shell,1920x1080 C:\guest\guest_shell_launch.exe` with WIN10XBOX_TEST_INJECT_INPUT="waitwin 60000;sleep 12000;shot;quit 0" and WIN10XBOX_SCREENSHOT=out.bmp.
10. Commands: `cmake --build build-mingw -j4; ctest --test-dir build-mingw -R 'integration.shell'` (shell_desktop, shell_desktop_hook, shell_launch).

Half 1 verified: ci.yml https://github.com/user99672223/win10xbox/actions/runs/40004553209 (linux-cross, windows Debug, windows RelWithDebInfo all green) and wine-pe.yml https://github.com/user99672223/win10xbox/actions/runs/40004553359 (user32 rebuilt with patch 0021) at 4bee0d4. integration.shell_desktop and integration.shell_desktop_hook pass on Windows. Printed lines: `shellhook: registered, message=49152`, `shellhook: message code=1/2/4/6 window=0000000000010022`, `shellhook: created ok`, `shellhook: activated ok`, `shellhook: destroyed ok`; The first push failed seven later GUI guests because the added tests' `unset(_guest_machine)` ended the builtin-only block that continues to guest_gdi_bitmap; fixed in 4bee0d4. Half 2 (child processes) pending; status stays wip.

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
