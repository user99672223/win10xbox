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

* `NtUserCallOneParam(CreateCursorIcon)` / `(GetIconParam)`, `NtUserCallTwoParam(SetIconParam)`, `NtUserSetCursorIconData`,
  `NtUserFindExistingCursorIcon`, `NtUserDestroyCursor`, `NtUserGetIconInfo`, `NtUserGetIconSize`, `NtUserGetCursorFrameInfo`,
  `NtUserGetCursorInfo`, `NtUserDrawIconEx` (core/user/win_cursoricon.cpp; unit tests `user_cursor_icon_*`). SDL2 creates a
  cursor and window icons on start-up; all of them were Todo.
* `NtUserQueryDisplayConfig`, `NtUserDisplayConfigGetDeviceInfo` (win_sysparams.cpp; SDL2 reads the display name through
  them; test `user_query_display_config`).
* Not a new syscall but a drawing call that reported success without drawing: `NtGdiStretchDIBitsInternal` and
  `NtGdiSetDIBitsToDeviceInternal` into a destination without a 32 bpp view (the 1 bpp mask of an icon) now draw pixel by
  pixel (test `user_stretch_dibits_into_monochrome_bitmap`); the primitives that still cannot draw there log a FIXME that
  names the call.

## Other fixes found on the way

* `GetUserNameW` (Wine reads `WINEUSERNAME`) now works, which made fastfetch's `LsaGetUserName` path run, and Wine's
  `LsaGetUserName` hands out a Buffer from msvcrt's private heap that fastfetch frees with `RtlFreeUnicodeString`
  (the process heap): `STATUS_HEAP_CORRUPTION` on the Windows host's heap (run 39941218788 and 39944178745, the
  `integration.fastfetch` gate and the module step; Doom itself was green in both). Found by running fastfetch under Wine with
  the host heap's validation on (`GlobalFlag=0xf0` for win10xbox.exe in the prefix: `warn:heap:unsafe_block_from_ptr ... mismatching
  heap`). wine-pe patch `0020-advapi32-lsagetusername-separate-buffer.patch` allocates the Buffers from the process heap.

## Still failing / todo

* Measured under Wine on a Linux host (not the CI numbers): about 12 s from process start to the first frame, the demo
  runs at about 230 fps.
* Doom does not use them, but they are not implemented: `RegisterTouchWindow` is a Wine stub, COM class
  `{529a9e6b-6587-4f23-ab9e-9c7d683e3c50}` is not registered (SDL asks for it at window creation), the other
  `DisplayConfigGetDeviceInfo` packet types and `QDC_VIRTUAL_MODE_AWARE`.
* I_Error's message box (without `-nogui`) is shown and never closed: the run waits for it.
