status: wip
owner: A

# -d3dsample: an unmodified real-world Direct3D 11 sample as a gated guest

Sample chosen: `walbourn/directx-sdk-samples`, `Direct3D11Tutorials/Tutorial07`, pinned at commit
`1ad8f0f6a3e4d9be7e54ca52640ac12b6565ab0c` (main at the time of the slice).
Why: it is the one tutorial that covers the whole path the brief asks for with no DXUT and no extra dependency: its own window
(`CreateWindow`, 800x600), `D3D11CreateDevice` with the HARDWARE -> WARP -> REFERENCE loop, the device -> `IDXGIDevice` ->
`GetAdapter` -> `GetParent` factory route (the way most engines and every SDK sample get their factory), `CreateSwapChainForHwnd`,
run-time HLSL from `Tutorial07.fxh` through `D3DCompileFromFile` (d3dcompiler_47), `seafloor.dds` through its own
`DDSTextureLoader.cpp`, constant buffers, a textured and lit rotating cube, `Present(0, 0)` in a `PeekMessage` loop. Three source
files (`Tutorial07.cpp`, `DDSTextureLoader.cpp`, `Tutorial07.rc`); compiles in seconds, far under the 5-minute budget. DirectXTK's sample
projects need the whole library built first; the DXUT samples need DXUT, Effects11 and the SDK media. Not modified: the sources are
compiled from the fetched tree, never copied into the repository.

## What was added

* `tools/ci/fetch_d3dsample.ps1`: shallow fetch of the pinned SHA, verifies HEAD, exports `WIN10XBOX_D3DSAMPLE_DIR`. ci.yml and
  host-uwp.yml cache the directory (`actions/cache`, key = the SHA) and run the script before Configure.
* `tests/guest/CMakeLists.txt` and `host-uwp/CMakeLists.txt`: `guest_d3dsample` (MSVC only, `WIN32` subsystem,
  `d3d11 d3dcompiler dxguid comctl32`), built when `WIN10XBOX_D3DSAMPLE_DIR` is set; `Tutorial07.fxh` and `seafloor.dds` are staged next to it
  (`WIN10XBOX_GUEST_DIR` via POST_BUILD; `prepare_machine.cmake` now copies `*.fxh` and `*.dds` into the machine folders;
  `stage.cmake` `-DD3DSAMPLE_DATA` for the package).
* `integration.guest_d3dsample` (tests/integration/CMakeLists.txt): `machine-native-d3d`, cwd `C:\guest`,
  `WIN10XBOX_TEST_PRESENT_LIMIT=60`, exit 42, stdout `present-limit: 60 frames presented`, trace `CreateSwapChainForHwnd` ... `Present(0, 0x0)
  ... (back buffer copied)`, and the varied-pixels gate `WIN10XBOX_EXPECT_SCREENSHOT_VARIED=150,150,650,450,40,3`. Registered on the
  Windows jobs only (needs the fetched source, MSVC), like the imgui test.
* Package: `guest_d3dsample.machine` (machine-native-d3d), `.env` (`WIN10XBOX_TEST_PRESENT_LIMIT=60`), `.title`, and a new `.cwd` sidecar
  (host-uwp/host.cpp, documented in guest_sidecar.h, checked in selftest.cpp): the working directory relative to `C:\guest`; the sample opens
  its shader and texture by relative name and the console default would be `C:\Users\<user>`. In `HOST_UWP_GUESTS`, the host-uwp.yml
  `$expected` list and the `.msix` entry check; `tests/guest/expected_exit.txt`: `guest_d3dsample.exe 42=frames`.

## Layer fixes the sample needed (core/dispatch/dxgi_interpose.cpp)

1. A factory reached through device -> adapter -> `GetParent` was the real factory (the known gap of the swapchain2 slice): its swap chain
   was never wrapped, so no `Present` was counted (no present limit) and no frame reached the compositor. `IDXGIObject::GetParent` is now
   hooked in the vtable of the real adapter every created device hands out (`HookDeviceAdapter` / `W10x_AdapterGetParent`, applied after
   `D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain`); a factory asked for through it by guest code comes back as the wrapper
   factory (callers inside the interposed dxgi/d3d11 images still get the real one).
2. `D3D11_CREATE_DEVICE_DEBUG` is dropped from `D3D11CreateDevice*` flags: Tutorial07 sets it under `_DEBUG` (the ci.yml Debug job) and the SDK
   layers (d3d11sdklayers.dll) are in neither the machine folder nor a retail console, so every device type would fail.

Further fixes found by CI are listed below.

## Status

Not yet run on Windows. Local: llvm-mingw build of the layer and host-uwp selftest (262 checks) green; the sample itself only builds with MSVC.
