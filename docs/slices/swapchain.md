status: wip (implemented, never run: the D3D path needs a Windows host; local build clean, compile only)
design: emulated GUI + native dxgi. machine.json "d3dkmt": "host" (or WIN10XBOX_D3DKMT=host) forwards every NtGdiDdDDI* of Wine's win32u thunks to the host's win32u by name (dispatcher.cpp UseHostD3dkmt, Policy::Translate, nargs 20); the host user32/gdi32/win32u are loaded and win32k connected before the PEB take-over (win32k_native.cpp ConfigureHostD3dkmt, same callback trampolines as route B).
host window: core/dispatch/swapchain.cpp. One host popup (off-screen, WS_EX_NOACTIVATE|TOOLWINDOW, shown) per emulated HWND that reaches a window-carrying D3DKMT call, made on a host thread started BEFORE the take-over (a host thread created later initialises against the guest loader lists).
translated: NtGdiDdDDIPresent (D3DKMT_PRESENT.hWindow at +8 -> host window, restored after), NtGdiDdDDIOpenAdapterFromHdc (hDc at +0: emulated window DC -> host window DC, via user::WindowOfDc), NtGdiDdDDICheckOcclusion (visible emulated window = not occluded, answered by core/user; the host window is off-screen). All other NtGdiDdDDI* pass through unchanged (CreateDCFromMemory, SetVidPnSourceOwner, ...), first status per name traced "native d3d: ... returned".
pixels: after a successful Present the host window is captured (PrintWindow PW_RENDERFULLCONTENT, else BitBlt of its DC) into a DIB; core/user overlays it on the client area in ComposeFrame (user::SetClientFrameSource), so host-win32 screenshot and host-uwp's presenter (ComposeFrame) show it. Not done: host-uwp CoreWindow swap chain for the full-screen case; host-uwp job; the CoreWindow copy is the generic ComposeFrame path.
gate: integration.guest_d3d11_swap (tests/guest/d3d11_swap.c, machine-emu-d3d from prepare_machine.cmake D3D_EMU_MACHINE; registered when not cross-compiling and the guest is built, i.e. MSVC): exit 42, readback centre=ffff0000 corner=ff0000ff, screenshot (228,228)=ffff0000 and (101,101)=ff0000ff, trace "swapchain: emulated window .. -> host window" and "swapchain: frame N of .. captured".
CI agent, read first: the trace lines "swapchain: first NtGdiDdDDIPresent: D3DKMT_PRESENT qwords ..." (is hWindow really at +8?), "native d3d: D3DKMT NtGdiDdDDIxxx returned <status>" (what the host refuses), "swapchain: capture of host window failed", and the centre/corner pixel in "frame N captured" (black = PrintWindow got no DWM surface).
Windows-only unknowns: (1) the native dxgi may import D3DKMT* from gdi32 that Wine's gdi32 lacks (load error, then the fix is exporting them like native_imports.cpp); (2) which NtGdiDdDDI* dxgi really calls for a windowed CreateSwapChain (blt model may use CreateDCFromMemory + GDI BitBlt, whose host HDC the emulated gdi32 cannot use); (3) dwmapi calls (DwmFlush, DwmIsCompositionEnabled) go to dwm over ALPC unfiltered/unemulated; (4) whether PrintWindow(PW_RENDERFULLCONTENT) captures an off-screen window's swap chain, or the window must be on-screen; (5) AppContainer: whether the container may create the host window / CreateSwapChainForHwnd on it. If denied, record the measurement here and fall back to CreateSwapChainForComposition with the back buffer copied; nothing is faked.
Known gaps: host windows are freed only when a slot is reused after the emulated window is gone (max 16); a non-window emulated HDC passed to OpenAdapterFromHdc is passed unchanged; Present flags/DWM redirection and ResizeBuffers/SetFullscreenState are untested; SetClientFrameSource overlays on a resized window use the current client size.

# Slice: -swapchain, a swap chain on a window of the emulated user32

Gate and design are in the handoff above. Files: core/dispatch/swapchain.cpp (host windows, translation, capture),
core/dispatch/dispatcher.cpp (UseHostD3dkmt), core/dispatch/win32k_native.cpp (ConfigureHostD3dkmt),
core/user/win_paint.cpp + core/include/win10xbox/user.h (GetWindowGeometry, WindowOfDc, SetClientFrameSource,
NotifyClientFrame), core/vfs (machine.json "d3dkmt"), tests/guest/d3d11_swap.c, tests/integration (D3D_EMU_MACHINE).

## Measurements

(filled in by the CI runs)

## Runs

(filled in)
