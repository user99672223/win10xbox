status: ready

Run: https://github.com/user99672223/win10xbox/actions/runs/39891347427 (head daa276c; linux-cross, windows (Debug) and
windows (RelWithDebInfo) all green; "Gate - nothing skipped" 55 of 55 ran and passed on both windows jobs, 65 of 65 on
linux-cross; wine-pe.yml and uwp-lite / host-win32-appcontainer green for the same head).

What runs through the layer: `C:\guest\sdl_rects.exe` (builtin-only machine folder), the real SDL2.dll 2.30.9 from the
official release zip loaded from `C:\guest`. Output (ctest-junit.xml of windows-RelWithDebInfo-logs):
```
sdl: init video=windows
sdl: surface 320x240
sdl: pixels 00dc5000 000000ff 0000ff00
sdl: done
```
exit 42. Pixels checked: GetPixel on the window's HWND at (5,5) blue, (30,30) red, (130,50) green, and the screenshot at
screen = client + (100,100): (105,105) ff0050dc, (130,130) ffff0000, (230,150) ff00ff00.

Syscalls added or changed: none. SDL2's windows video driver (software surface, SDL_UpdateWindowSurface) ran entirely on the
NtUser*/NtGdi* calls core/user already implements; no core/user change was needed.

DLLs added to wine-pe/dlls.txt: `setupapi`, `msacm32`.

What still fails: nothing in this slice. Not exercised: SDL's renderer (d3d / opengl), raw input, joystick/haptic, audio.

# Slice: realsdl

A real SDL2 program through the layer: `sdl_rects.exe` (tests/guest/sdl2/sdl_rects.c, about 60 lines) linked against the
official SDL2 2.30.9 x64 prebuilt release (`SDL2-devel-2.30.9-VC.zip`, SHA-256 pinned and verified on every run by
`tools/ci/build_sdl_guest.ps1`, cached with `actions/cache`). `SDL2.dll` is copied unmodified next to the exe in the
machine folder's guest dir, so the guest loads a real third-party DLL from `C:\guest`. Gate: `integration.real_sdl2`
(builtin-only machine folder, windows jobs only): window painted through `SDL_GetWindowSurface` + `SDL_UpdateWindowSurface`,
pixels read back with `GetPixel` on the window's HWND, exit 42, and the screenshot checked at screen = client + (100,100).

## Why the first attempt (3fdc6ad, static SDL2 built from source) failed

windows (Debug): `integration.real_sdl2` died in the loader: `Importing dlls for L"C:\guest\sdl_rects.exe" failed, status
c0000135` (STATUS_DLL_NOT_FOUND). The static SDL2 imports setupapi.dll, which was not in wine-pe/dlls.txt.
(linux-cross failed separately on `integration.guest_window_input2`, a test of main, not of this slice.)

## Changes

* wine-pe/dlls.txt: `setupapi` and `msacm32` (winmm's static import; setupapi was the first c0000135, then WINMM.dll failed on msacm32) (SDL2.dll's static imports: setupapi, winmm, imm32, version, kernel32, user32, gdi32,
  advapi32, shell32, ole32, oleaut32).
