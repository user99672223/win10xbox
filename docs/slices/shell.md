status: wip

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
