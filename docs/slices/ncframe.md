status: ready

# ncframe: the non-client area

Slice of the GUI layer (core/user): caption bar, borders and the three system buttons drawn and hit-tested;
WM_NCHITTEST; WM_NCLBUTTONDOWN on the caption (SC_MOVE) and on a border (SC_SIZE) run DefWindowProc's modal loop;
a click on a system button sends WM_SYSCOMMAND (close / minimize / maximize or restore).

Gate: `integration.guest_window_nc` (tests/guest/window_nc.c) on the builtin-only machine folder, host-injected input
(WIN10XBOX_TEST_INJECT_INPUT), the guest's own GetPixel readback and WIN10XBOX_EXPECT_SCREENSHOT_PIXELS; exit 42.

Changed outside the slice's own files: `integration.guest_window_input2` expected a WM_NCLBUTTONUP after a plain
caption click; with the move loop the button-up is consumed by the loop and the window gets WM_CAPTURECHANGED instead
(Windows and Wine behave the same), so its expected log ends in `4:CAPCH` now.

Geometry (Windows 10 metrics at 96 DPI, as WM_NCCALCSIZE already uses): frame inset 8 (overlapped window), caption
23 rows (22 of caption colour plus the separator line), buttons 36 wide, close at the right edge of the caption.


## Result

Run: https://github.com/user99672223/win10xbox/actions/runs/39877571211 (code head 814e3a9; linux-cross, windows Debug and
windows RelWithDebInfo green, "Gate - nothing skipped" passed, `integration.guest_window_nc` passed on all three, 46 of 46
on the Windows jobs). The final head only adds a merge of main (tools/ci and docs) and this text.

What runs through the layer: `C:\guest\guest_window_nc.exe` on the builtin-only machine folder, driven by
WIN10XBOX_TEST_INJECT_INPUT. The test passes only when every one of these guest lines is printed (CMake regexes) and
the exit code is 42:

    window_nc: frame ok
    window_nc: hit caption=2 close=20 max=9 min=8 client=1 top=12 topleft=13 left=10 bottomright=17
    window_nc: dragged to 150,140 size=400x300
    window_nc: resized to 150,140 size=430x320
    window_nc: pixels ok
    window_nc: WM_SYSCOMMAND SC_CLOSE
    window_nc: WM_CLOSE
    window_nc: WM_DESTROY
    window_nc: done

The host also checks the last frame's pixels (WIN10XBOX_EXPECT_SCREENSHOT_PIXELS): active caption at (420,152), border
at (153,300), close button at (538,150), client at (300,300).

Syscalls: none newly registered. The slice changes the behaviour behind the existing user32 entry points (non-client
paint, hit test, SC_MOVE/SC_SIZE modal loop in DefWindowProc, caption text drawn with the built-in face).

Packaging: guest_window_nc.exe is bundled in the host-uwp package (host-uwp/CMakeLists.txt, host-uwp.yml expected list).

DLLs added to wine-pe/dlls.txt: none.

What still fails / not covered: system-button clicks other than close (minimize, maximize/restore) send
WM_SYSCOMMAND but are not gated; menu bar and scroll bars in the non-client area, the system menu and caption icon
are not drawn; no DPI scaling of the metrics (96 DPI only).
