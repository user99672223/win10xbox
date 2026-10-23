status: wip

# Slice: GDI bitmaps (CreateDIBSection, BitBlt/StretchBlt, LoadImage of a BITMAP resource, AlphaBlend, TransparentBlt)

Gate: `integration.guest_gdi_bitmap` (tests/guest/gdi_bitmap.c, tests/guest/gdi_bitmap.rc.in, tests/guest/gdi_bitmap_res/*.bmp)
on the builtin-only machine folder. The guest checks every pixel itself (GetPixel / GetDIBits / the DIB bits pointers)
and exits 42 only if all checks passed; the ctest also compares 17 screen pixels of the window with
`WIN10XBOX_EXPECT_SCREENSHOT_PIXELS`.

The same program was run against Wine's own gdi32 (a real X display under Xvfb) while writing it: every guest-side
check passes there, so the expected values are what Windows GDI produces (AlphaBlend with constant alpha 128 is
accepted within one unit per channel in the guest; the screenshot pins our exact rounding).

(Results of the CI run, syscalls and DLLs: to be filled in when ci.yml is green.)
