status: wip
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

None so far (none known without a Windows run).

## Untested locally

No MSVC or llvm-mingw in the authoring container: the guest and the CMake changes are unbuilt until CI.
Unproven on the console: real pad, audible tone, real GPU.
