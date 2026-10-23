status: ready
slice: -displaymode, branch claude/youthful-cerf-l7xxsb-displaymode, code head 83b0381
CI: ci 40033692831 (3 jobs green), host-uwp 40033692670 (msix/run/launcher green); status.txt and report header carry display=, guest_display=, output_scaling=.
Fix after the first runs: HdmiDisplayInformation::GetForCurrentView() returns null off the console (runner crashed in the launcher); now falls back to DisplayInformation.
Runner showed: display=1024x768@60 hdr=0 supported=0 requested=auto result=kept; guest_display=1024x768@60 requested=auto; output_scaling=system; tearing supported.
Only the console proves: the 2560x1440@120 request (RequestSetCurrentDisplayModeAsync result: set/refused/timeout, and the mode afterwards).
Only the console proves: the supported-modes list and supported=N count in status.txt.
Only the console proves: the system-scaling path (back buffers at guest size, DXGI_SCALING_STRETCH) on the real compositor; check for an automatic fallback to layer and the recorded hr.
Only the console proves: ALLOW_TEARING support and that sync-interval-0 presents tear/unlock the rate at 120 Hz.
Only the console proves: the hdr= flag (BT2020 colour space of the current HDMI mode).
Console check: set "display_mode": "auto" and "guest_display": "1920x1080@120" in launch.json, read display=/guest_display=/scale= in status.txt; run guest_display_mode.exe (expects exit 42).
Runs on the runner: integration.guest_display_mode (both cases) passes in ci; the guest is in HOST_UWP_GUESTS, the console foreach, $expected, expected_exit.txt.
