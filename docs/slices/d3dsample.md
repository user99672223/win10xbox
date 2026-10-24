status: blocked (ci run https://github.com/user99672223/win10xbox/actions/runs/40122999773 @ 3847a71): integration.guest_d3dsample, first error: FATAL first-chance 0xc0000005 executing d3dcompiler_47 base + 0x8B45F08B (rip = rcx, rax = 0xCC), inside D3DCompileFromFile of Tutorial07.fxh, after CreateSwapChainForHwnd and GetBuffer succeeded
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

3. First CI run (ci run 40121039057 @ 03e0237, RelWithDebInfo): with 1 and 2 in place the sample got as far as `CreateSwapChainForHwnd` and
   `GetBuffer` (so `D3DKMT`, the swap-chain wrapper and the hooked `GetParent` work), then died in `D3DCompileFromFile` with an access
   violation executing `d3dcompiler_47 base + 0x8B45F08B` (code bytes read as a jump-table RVA), `rax = 0xCC`. Cause: the loader's code
   scanner (core/loader/scanner.cpp) found 4 "sites" in d3dcompiler_47.dll; the profile-guided image keeps the sparse-switch index table
   (one byte per case value, read by `movzx eax, byte [rdx+rax+IDX]`) in the code, a pair of index bytes `0F 31` decoded as `rdtsc`,
   and the trap patch wrote `0xCC` over an index byte, so the switch read entry 204 of a smaller jump table. `FindInlineJumpTables`
   now finds the index table from the `movzx` and the `cmp reg, N` in front of it (N+1 bytes), leaves it out of the sweep and takes
   max(index)+1 as the size of the jump table behind it. Unit test `loader_scan_leaves_index_tables_in_code_alone` (fails without the
   change). Unproven that this is the only table form d3dcompiler_47 uses; the integration test runs with `+scan` so the log
   shows `index table of ...` lines.

Further fixes found by CI are listed below.

## Status

Budget: 2 code pushes used (03e0237, 3847a71), blocked.

Everything up to the shader compile works on the Windows runner (RelWithDebInfo, both runs): the sample builds (MSVC, seconds), the 4 other
guests and the other 117 ctest tests pass, the package builds (msix job green), and the sample's window, `D3D11CreateDevice`, the
device -> adapter -> `GetParent` factory (now wrapped), `CreateSwapChainForHwnd` (traced `-> 00000000`) and `GetBuffer` all succeed.
Then the process dies, both runs with the same registers and the same address, in `CompileShaderFromFile(L"Tutorial07.fxh", "VS", "vs_4_0")`:

    FATAL-LOOKING first-chance exception code=0xc0000005 address=base+0x8B45F08B (D3DCOMPILER_47 base 0x2CC46550000 in run 1)
    rax=0xCC rcx=rip rdx=0x11 rbx=rdi=heap object; the stack holds D3DCOMPILER_47 .data pointers (+0x3b71d0..) and return
    addresses +0xff268 +0xc1835 +0xc2f06 +0xcd870 +0x6fffc +0x771e7 +0x3f75f +0x573e0; no ntdll/exception-dispatch frame

imgui's `D3DCompile` (vs_4_0 and ps_4_0 from memory, same DLL, same machine) works, so the difference is the shader or the file path
(`D3DCompileFromFile`, `D3DCOMPILE_ENABLE_STRICTNESS`, `mul(float4, matrix)`, a `Texture2D`/`SamplerState` pair).

What was ruled out: the index-table hypothesis of fix 3 is NOT the cause here (the identical crash after it; the trace
`index table of N bytes` lines appear for the exe and d3d11.dll but none for d3dcompiler_47). The fix itself is still right (it
reproduces in the unit test and makes the scanner leave 50 in-code tables of the sample's own image alone), but it did not change d3dcompiler_47's
"4 candidate sites, patched 3 inline, 1 trapped".

Next (for whoever continues, in this order, each is one CI run):
1. Prove or exclude the scanner: run the d3dsample test with the scan of d3dcompiler_47.dll disabled (an env switch in `ScanAndPatch`, or
   `patch_cpuid`/`patch_rdtsc` off for that image) and log the four site addresses with their bytes (`ScanCore` logs only counts). If the crash
   goes away, a site (or the jump-back of an inline thunk, or a switch whose case target falls in a displaced region that the
   `targets` list missed) is the cause; the stack suggests a `movzx`/`mov` table read with rax = 0xCC (a trap byte) in the function whose return
   address is +0xff268.
2. If the scanner is innocent: a tiny guest that calls `D3DCompile` on the text of Tutorial07.fxh from memory (VS, then PS) separates the file
   read from the compiler; then `D3DCompileFromFile` of the same file.
3. Package: under host-uwp (launcher job of run https://github.com/user99672223/win10xbox/actions/runs/40123022766 @ 3847a71) the sample
   exits 0 (its `InitWindow`/`InitDevice` failure path, the CoreApplication process cannot create HWNDs: the `-uwpswap` limitation that
   `guest_d3d11_swap` and `guest_imgui_d3d11` record as KNOWN-FAIL), so the all-programs assert says `guest_d3dsample.exe is FAIL
   (exit code 0, expected 42)`. The brief asked for `guest_d3dsample.exe 42=frames`; add `KNOWNFAIL=0:uwp-no-hwnd(-uwpswap)` to that row
   until `-uwpswap` lands (tests/guest/expected_exit.txt), as the two siblings have it.

Runs: ci https://github.com/user99672223/win10xbox/actions/runs/40121039057 @ 03e0237 (linux-cross green; RelWithDebInfo red on this one test),
https://github.com/user99672223/win10xbox/actions/runs/40122999773 @ 3847a71 (same); host-uwp https://github.com/user99672223/win10xbox/actions/runs/40121069939
and https://github.com/user99672223/win10xbox/actions/runs/40123022766 (msix + run green, launcher red as above).
