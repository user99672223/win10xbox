status: wip

# Slice: doom (Chocolate Doom through the layer)

Goal: Chocolate Doom 3.1.1 (official win64 zip, pinned SHA-256) with the shareware `doom1.wad` (pinned SHA-256) runs
`-timedemo demo1` on the Wine PE builtins over the dispatcher and core/user, SDL2 on its software renderer (blits
through GDI onto the window), and the composed frame is verified (`integration.doom_timedemo`, MSVC / real-program builds only).

Run: (not run on CI yet)
Screenshot artifact: `windows-<config>-logs` -> `build/bin/doom_timedemo.bmp`
Gate pixels: see below (taken from a real frame).

## What the gate is, and where it differs from the first draft of the task

* `chocolate-doom.exe` reports a finished timedemo through `I_Error`: `timed N gametics in M realtics (F fps)` goes to
  **stderr** and the process exits with **-1** (0xFFFFFFFF), not 0 (`src/doom/g_game.c` `G_CheckDemoStatus`, `src/i_system.c`).
  Without `-nogui` `I_Error` also opens an SDL message box (the layer shows it; nobody closes it, the run hangs), so the test
  passes `-nogui`. The test therefore expects exit -1 (`run_guest.cmake` accepts -1 / 255 / 4294967295 for it) and matches the
  stats line on the guest's stderr (the test's trace regexes read stderr; the layer's own trace is cut down to err/fixme
  with `WIN10XBOX_DEBUG=-all,err+all,fixme+all` so the line is not interleaved).
* The host's inject script gained `waitflush F N` (host-win32/main.cpp): wait until the guest's windows have flushed F
  surfaces (a game draws a frame per tick). The script is `waitflush 300 250000;shot`: `shot` logs the distinct colour count
  of the frame 300 frames into the demo. The BMP that is uploaded and pixel-checked is the one the host rewrites when the
  process ends: the last frame of the demo, which is deterministic (tic 5026).
* Run flags: `-iwad C:\guest\doom1.wad -nosound -nomusic -nomouse -window -geometry 640x400 -nogui -timedemo demo1`,
  `SDL_RENDER_DRIVER=software`. SDL keeps the 4:3 aspect, so the 640x400 request gives a 533x400 client (window 549x439).

## What had to change

* `tools/ci/fetch_doom.ps1` + ci.yml steps (cache, fetch): `chocolate-doom.exe`, every DLL of the zip (the exe hard-imports
  `libfluidsynth-3.dll`, which hard-imports `SDL3.dll`, glib, sndfile, portaudio, ...) and `doom1.wad` into
  `build/bin/machine/guest`; `prepare_machine.cmake` mirrors them into the builtin-only folder the test runs on.
* Wine builtins added to `wine-pe/dlls.txt`: `msacm32` (winmm's hard import), `dsound` (libfluidsynth), `setupapi` (SDL2,
  libportaudio). (`winmm`, `imm32`, `version`, `ole32`, `oleaut32`, `hid`, `shell32`, `shcore` were already there.)
* `core/dispatch/process.cpp`: the guest environment gets `WINEUSERNAME` (Wine's advapi32 `GetUserNameW` reads it; Wine's
  server sets it in every process). Without it `GetUserNameW` failed and shell32's `%USERPROFILE%` expansion built
  `C:\users\<garbage>` so `SHGetFolderPath(CSIDL_APPDATA | CSIDL_FLAG_CREATE)` (SDL_GetPrefPath, Doom's config directory) failed.
* host-win32/main.cpp: inject verb `waitflush`.
* tests/integration: `integration.doom_timedemo`, `run_guest.cmake` (`RUN_TIMEOUT`, exit -1).

## Syscalls added

(none yet)

## Still failing / todo

* Measured under Wine on a Linux host (not the CI numbers): about 12 s from process start to the first frame, the demo
  runs at about 230 fps.
* `NtUserFindExistingCursorIcon`, `NtUserDestroyCursor` (SDL's cursor setup), `NtUserQueryDisplayConfig`,
  `NtUserDisplayConfigGetDeviceInfo` (SDL's display names), a 1 bpp bitmap draw target and `RegisterTouchWindow` are hit
  and fail or are ignored; Doom does not need them.
