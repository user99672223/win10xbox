status: ready

# Slice: GDI bitmaps (CreateDIBSection, BitBlt/StretchBlt, LoadImage of a BITMAP resource, AlphaBlend, TransparentBlt)

Run: https://github.com/user99672223/win10xbox/actions/runs/39885961936 (ci.yml, head b26d46b: linux-cross, windows Debug and
RelWithDebInfo green, `ctest gate: PASS (nothing skipped, nothing missing)`). The commit after it only adds
`guest_gdi_bitmap` to host-uwp's package (host-uwp/CMakeLists.txt, host-uwp.yml); the run of the final head is linked from the branch.

Gate: `integration.guest_gdi_bitmap` (tests/guest/gdi_bitmap.c, tests/guest/gdi_bitmap.rc.in, tests/guest/gdi_bitmap_res/*.bmp)
on the builtin-only machine folder. The guest checks every pixel itself (GetPixel / GetDIBits / the DIB bits pointers)
and exits 42 only if all checks passed; the ctest also compares 17 screen pixels of the window with
`WIN10XBOX_EXPECT_SCREENSHOT_PIXELS`.

## What runs through the layer

`C:\guest\guest_gdi_bitmap.exe` (exit 42), first lines of the job log:

```
bmp: setup ok
bmp: dibsection-bits-bgr ok
bmp: srccopy ok
...
bmp: dib32-getobject ok / dib24-getobject ok / dib8-getobject ok
bmp: window created
bmp: win-dib32-srccopy ok ... win-notsrccopy ok, win-srcinvert ok, win-stretch2x-all ok, win-readback-bits ok
bmp: win-alphablend-128 ok, win-transparent-px ok
bmp: loadimage24-getobject ok, loadimage24-pixels ok, loadimage8-pixels ok
bmp: failures=0
bmp: done
```

Covered: `CreateDIBSection` at 32 (top-down), 24 (bottom-up, padded rows) and 8 bpp (palette, `GetDIBColorTable` /
`SetDIBColorTable`), the bits pointer written by the guest seen by GetPixel / BitBlt and GDI drawing seen in the bits;
`BitBlt` memory DC to window DC (SRCCOPY, NOTSRCCOPY, SRCINVERT, SRCPAINT, SRCAND), window DC to memory DC readback (bits,
GetPixel, GetDIBits), 2x `StretchBlt`; `AlphaBlend` / `TransparentBlt` through the real `msimg32.dll` import; `LoadImageW`
of two BITMAP resources (24 bpp 6x4, 8 bpp 4x4 with a palette) with `LR_CREATEDIBSECTION`: `GetObject` size / depth, pixels
after a blit.

The same program was run against Wine's own gdi32 (a real X display under Xvfb) while writing it: every guest-side
check passes there, so the expected values are what Windows GDI produces (AlphaBlend with constant alpha 128 is
accepted within one unit per channel in the guest; the screenshot pins our exact rounding).

## Fixes found by the gate

* `core/user/gdi_bitmap.cpp` defined the `gdi` trace channel a second time (LNK2005 in every job).
* user32's `LoadImage(LR_CREATEDIBSECTION)` creates the DIB section and fills it with `StretchDIBits` on a memory DC;
  `NtGdiStretchDIBitsInternal` and `NtGdiSetDIBitsToDeviceInternal` did nothing for a 24 bpp / 8 bpp / mono target
  ("drawing into a 24 bpp bitmap is not supported"), so the loaded bitmaps were black. Both now draw pixel by pixel into such bitmaps.

## Syscalls added or changed

Added: `NtGdiAlphaBlend`, `NtGdiTransparentBlt` (core/user/gdi_bitmap.cpp, registered in user_syscalls.h; docs/SYSCALLS.md rows
updated by hand to what the generator prints, there is no Wine tree in the container to run it).
Changed: `NtGdiStretchDIBitsInternal`, `NtGdiSetDIBitsToDeviceInternal` (non-32 bpp targets).

## DLLs added to wine-pe/dlls.txt

`msimg32` (AlphaBlend / TransparentBlt / GradientFill forward to gdi32). wine-pe.yml green for the head.

## Build changes

`enable_language(RC)` in the root CMakeLists.txt and host-uwp/CMakeLists.txt; the guest's .rc is generated from
`gdi_bitmap.rc.in` with absolute .bmp paths (windres and rc.exe resolve relative names differently); `guest_gdi_bitmap` is
bundled in the host-uwp package (and checked by host-uwp.yml).

## What still fails / not done

* AlphaBlend / TransparentBlt: nearest-neighbour sampling, the DC clip region is not applied; `GradientFill` is still Todo.
* StretchBlt shrinking does not implement STRETCH_ANDSCANS combining (plain nearest sampling).
* Line / polygon / text primitives into 8 / 24 bpp DIB sections stay unsupported (FIXME in `ResolveDrawTarget`).
* The test is not registered as a native `guest.*` smoke on the Windows runners (its pixel values were validated against Wine's gdi32 instead).
