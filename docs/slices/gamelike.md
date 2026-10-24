status: ready
owner: A

# -gamelike: one game-like frame loop as the gate for the console package

Branch `claude/youthful-cerf-l7xxsb-gamelike`, from main 1dc0410.

## What it is

`tests/guest/gamelike.c` (MSVC only, like d3d11_swap.c): window + message loop, D3D11 device and swap chain
(CreateDXGIFactory1 / CreateSwapChain), XAudio2 2.9 mastering voice + looping 440 Hz source voice (only when the machine has
a render endpoint), `XInputGetState(0)` per frame, a moving coloured quad with an HLSL shader compiled at run time,
`Present(1, 0)`, 60 fps pacing, one status line per 30 frames. Exit 42 from the layer's frame limit, or itself after 10 s.
Failure codes are listed at the top of the source.

## Wiring

* tests/guest/CMakeLists.txt (MSVC block), host-uwp/CMakeLists.txt (`guest_gamelike`, `HOST_UWP_GUESTS`).
* `integration.guest_gamelike_emu` (machine-emu-d3d) and `integration.guest_gamelike_native` (machine-native-d3d):
  `WIN10XBOX_TEST_PRESENT_LIMIT=90`, `WIN10XBOX_AUDIO_NULL_ENDPOINT=1`, exit 42, `present-limit: 90 frames presented`,
  status line at frame 60, varied-pixels gate over the window (100,100 + 640x360).
* Package: `host-uwp/payload/guest/guest_gamelike.{machine,env,title}` (machine-emu-d3d, `WIN10XBOX_TEST_PRESENT_LIMIT=300`,
  no null endpoint), host-uwp.yml `$expected`, tests/guest/expected_exit.txt (`guest_gamelike.exe 42=frames`).

## Layer fixes

1. core/dispatch/dxgi_interpose.cpp, `VirtSwap::DoPresent`: the virtual swap chain (machine-emu-d3d) did not count presents for
   `WIN10XBOX_TEST_PRESENT_LIMIT` (only `WrapSwap` did), so the guest on machine-emu-d3d ran to its own 10 s stop (462 frames,
   exit 42 by itself, no `present-limit:` line). Now `VirtSwap::CountPresent()` runs after the composed frame is handed over,
   with the same `present-limit:` line and `GuestExit(42)`. First push: `integration.guest_gamelike_native` passed,
   `integration.guest_gamelike_emu` failed on exactly that (ci run 40104300918); host-uwp run 40104304141 was green with
   guest_gamelike.exe PASS (via the guest's own 10 s stop).

## Untested locally

No MSVC or llvm-mingw in the authoring container: the guest and the CMake changes are unbuilt until CI.
Unproven on the console: real pad, audible tone, real GPU.

## Result

* ci.yml 3 jobs green on 4434050: https://github.com/user99672223/win10xbox/actions/runs/40105880500
  (`integration.guest_gamelike_emu` and `_native` pass, present-limit line at 90 frames).
* host-uwp msix/run/launcher green: https://github.com/user99672223/win10xbox/actions/runs/40105880423 (launcher job rerun once:
  the first attempt's report was read after only 2 programs, a poll race unrelated to this slice; the rerun shows
  guest_gamelike.exe PASS, exit 42 = frames). First push runs: ci 40104300918 (emu red, see Layer fixes), host-uwp 40104304141.
