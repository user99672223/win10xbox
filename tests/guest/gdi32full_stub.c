/* gdi32full_stub: stands in for gdi32full.dll in machine-emu-d3d (docs/slices/uwpswap.md). The native dxgi/d3d11/d3d10warp
 * import D3DKMT* and a few GDI functions from gdi32full.dll (through the ext-ms-win-gdi-* / api-ms-win-dx-d3dkmt-* API
 * sets); the machine's GUI is Wine's builtin gdi32 over core/user, which has no gdi32full. This DLL has one export; when
 * the loader maps it core/dispatch/native_imports.cpp (ExtendExports) adds the rest of the host's gdi32full exports to
 * its export table, as jumps to Wine's gdi32 where it has the function and to the host's gdi32full (D3DKMT* go to the
 * host's dxgkrnl) where it does not. It is never a guest program. */
#include <windows.h>

__declspec(dllexport) int W10xGdi32FullStub(void) { return 0; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
