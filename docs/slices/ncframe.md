status: wip

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

Result sections (run URL, guest output, syscalls, DLLs, what still fails) follow when the run is green.
