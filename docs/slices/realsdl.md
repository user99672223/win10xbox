status: wip

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

* wine-pe/dlls.txt: `setupapi` (SDL2.dll's static imports: setupapi, winmm, imm32, version, kernel32, user32, gdi32,
  advapi32, shell32, ole32, oleaut32).
