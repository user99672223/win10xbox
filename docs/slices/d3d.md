status: wip
handoff (run 39946954565 @ 8237882: linux-cross green, both MSVC jobs red on integration.guest_d3d11):
- the guest reaches the host dxgkrnl: NtGdiDdDDIQueryAdapterInfo returns c000000d (host), QueryStatistics success.
- RelWithDebInfo: exit 1722 (RPC_S_SERVER_UNAVAILABLE) before the "d3d11: adapter=" line; no d3d11: adapter line printed yet.
- cause: Wine sechost device_notify_proc (cfgmgr32 notification) fails its plugplay RPC and returns 1722; RtlExitUserThread
  then took ThreadAmILastThread as true (no ThreadAmILastThread trace line appeared, so the query bypassed the guest-count path).
- this push: ThreadAmILastThread is traced on every query (state, flags, guest count) to settle which branch answers.
- Debug: test hangs 120 s early in guest startup with relay tracing on (not yet diagnosed).
- guest_imgui_d3d11 (not this gate): D3DCOMPILER_47 import api-ms-win-security-cryptoapi-l1-1-0.dll not found (c0000135); same dll blocks D3DCompile in d3d11_tri.
- next: read the ThreadAmILastThread line; map api-ms-win-security-cryptoapi to cryptsp/sechost; Debug hang.

# Slice: -d3d, a guest renders with Direct3D 11 (off-screen)

Gate: `integration.guest_d3d11` (`tests/guest/d3d11_tri.c`, registered for native Windows builds only, so both MSVC
configs of `ci.yml`; not on linux-cross, where the skip gate never counts it) and the `d3d11-plain` / `d3d11-ac` rows of
`.github/workflows/host-win32-appcontainer.yml`.

## What runs

The guest is an ordinary D3D11 client without a window or swap chain. It `LoadLibrary`s the host machine's NATIVE
`d3d11.dll`, creates a device (`D3D_DRIVER_TYPE_HARDWARE`, then `D3D_DRIVER_TYPE_WARP`), compiles a vertex and a pixel
shader at run time with `D3DCompile` from the native `d3dcompiler_47.dll`, clears a 64x64 RGBA8 render target to blue,
draws one red triangle (corners from `SV_VertexID`, no vertex buffer), copies it to a staging texture, `Map`s it and
reads the centre and the (0,0) pixel. Exit 42 = centre is the triangle colour and the corner the clear colour. It prints

    d3d11: adapter=<name> feature_level=0x<hex> driver=<hardware|warp>

plus, after the gate, the guest's module list (where the user-mode driver came from) and the adapters DXGI enumerates.

## How the kernel side works (nothing new was invented)

* The machine folder `machine-native-d3d` (route B, `gui: native`) holds the native user32/gdi32/gdi32full/win32u/imm32
  and the D3D runtime from `machine/guest_dlls.txt` (`tests/integration/prepare_machine.cmake`, `D3D_MACHINE`).
* d3d11/dxgi/d3d10warp talk to the kernel with `D3DKMT*` in gdi32/gdi32full, which are the `NtGdiDdDDI*` syscalls of the
  native win32u. `RenumberSyscallThunks` (core/dispatch/dispatcher.cpp) renumbers every win32u thunk to the dispatcher's
  id for its NAME, and a name Wine's table lacks is added as a pass-through by `GetProcAddress` on the host's
  `win32u.dll`. Every `NtGdiDdDDI*` therefore calls the host's own export of the same name and returns what the host
  returned. The loader prints `win32u.dll: N NtGdiDdDDI* (D3DKMT) thunks forwarded by name to the host's win32u`.
* `core/dispatch/native_d3d.cpp` only watches those calls: the first call of each name with its status
  (`native d3d: first D3DKMT call NtGdiDdDDIxxx -> <status>`), and once per name the first error status
  (`native d3d: D3DKMT NtGdiDdDDIxxx returned <status>`) -- in the AppContainer that line is the measurement of what the
  container's dxgkrnl refuses.
* Native DLLs import kernelbase functions Wine 11.9 lacks; `core/dispatch/native_imports.cpp` extends the guest
  kernelbase's export table with jumps. Its jump block used to lose the execute right to a second `VirtualProtect`
  covering the same page (the native user32 died jumping into a read-only page); the whole block is now one
  `PAGE_EXECUTE_READ` region.

## User-mode driver (UMD) load

d3d11/dxgi ask the host kernel which UMD an adapter uses (`D3DKMTQueryAdapterInfo`, UMDRIVERNAME) and `LoadLibrary` it by
that name: `...\System32\<vendor>umd*.dll`, `...\System32\DriverStore\FileRepository\<package>\<umd>.dll`, or
`...\System32\d3d10warp.dll` for WARP and the Microsoft Basic Render Driver. Guest DLLs normally come from the machine
folder, which never holds a vendor driver. `core/vfs/vfs.cpp` (`IsGpuDriverModule`, `BuildHostPlain`) therefore resolves
two kinds of path in the HOST's `%SystemRoot%\System32` when, and only when, the machine folder does not have the file
and the host does:

* `C:\Windows\System32\<name>.dll` where the name is `d3d10warp`, `nvwgf2*`, `nvd3dum*`, `atidxx*`, `aticfx*`, `amdxx*`,
  `amdxc*`, or contains `umd`;
* `C:\Windows\System32\DriverStore\...\*.dll` (and the other override extensions).

Nothing else of the host's System32 is reachable that way (D17: the console allows opening a System32 file by name,
not listing it). `d3d10warp.dll` is also in the manifest, so on a machine folder staged by `import_windows_dlls.ps1` the
machine copy wins and the fallback is not used for WARP. Measured: see "Measurements".

## DLLs added to `machine/guest_dlls.txt`

`d3d12.dll`, `D3D12Core.dll`, `D3DSCache.dll` (this slice); `d3d11.dll`, `dxgi.dll`, `d3dcompiler_47.dll`,
`d3d10warp.dll`, `dxcore.dll`, `dwmapi.dll` were already in the branch's manifest. `import_windows_dlls.ps1` is unchanged.

## Measurements

(filled in from the runs below)

## Runs

(filled in)

## Still failing / not done

(filled in)
