status: ready (item 1 only; NOT built or run locally, no mingw/MSVC here: first signal is ci.yml)
# Slice: -d3dheavy, item 1: WIN10XBOX_TEST_PRESENT_LIMIT
Files: core/dispatch/dxgi_interpose.cpp (CountPresent in the swap wrapper, SetPresentLimit), core/dispatch/process.cpp (latched in Launch), core/include/win10xbox/dispatch.h, docs/TESTING.md, tests/integration/CMakeLists.txt.
Behaviour: after the N-th successful non-TEST Present/Present1 (all swap chains) and after the frame went to the compositor, prints "present-limit: N frames presented, WxH, adapter" (stdout + dispatch trace) and process::GuestExit(42).
Test: integration.guest_d3d11_swap_limit = guest_d3d11_swap.exe, limit 10, exit 42, regex + trace regex, screenshot VARIED 100,100,356,356,8,2 (not exact pixels: frame 10's triangle is off-centre).
Untested: compile (HostModeScope around a noreturn call; __uuidof(IDXGIDevice)), whether the host writes the screenshot on the GuestExit path (d3d11_swap's normal exit 42 uses the same path).
Not done: the `.env` sidecar form (brief truncated, descoped), items 2 (heavy samples).
Commands: ctest -R guest_d3d11_swap.
