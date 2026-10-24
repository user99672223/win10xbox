/* actctx_stack: an activation context frame of the guest's while the layer's host code calls the host's loader
 * (docs/slices/laptop-programs.md). The program imports neither user32 nor gdi32; it activates the context of its own
 * manifest (resource 1, actctx_stack.manifest) and loads gdi32.dll inside it. On machine-emu-d3d mapping Wine's gdi32
 * makes the layer resolve host gdi32 exports with GetProcAddress (core/dispatch/native_imports.cpp), and the host's
 * loader walks the thread's activation context stack: with the guest's frame on it (a Wine ACTIVATION_CONTEXT) it raised
 * STATUS_SXS_CORRUPT_ACTIVATION_STACK (0xc0150014), the first error of Audacity, YUView and VirtualBox in the sweep.
 * Prints `actctx: gdi32 loaded`, `actctx: done`, exit 42. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    ACTCTXW ac;
    ZeroMemory(&ac, sizeof ac);
    ac.cbSize = sizeof ac;
    ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    ac.hModule = GetModuleHandleW(NULL);
    ac.lpResourceName = MAKEINTRESOURCEW(1);   /* CREATEPROCESS_MANIFEST_RESOURCE_ID */
    HANDLE ctx = CreateActCtxW(&ac);
    if (ctx == INVALID_HANDLE_VALUE) {
        printf("actctx: CreateActCtxW failed, error %lu\n", GetLastError());
        return 1;
    }
    ULONG_PTR cookie = 0;
    if (!ActivateActCtx(ctx, &cookie)) {
        printf("actctx: ActivateActCtx failed, error %lu\n", GetLastError());
        return 2;
    }
    printf("actctx: activated\n");
    fflush(stdout);
    HMODULE gdi32 = LoadLibraryW(L"gdi32.dll");
    printf("actctx: gdi32 %s\n", gdi32 ? "loaded" : "NOT loaded");
    fflush(stdout);
    if (!DeactivateActCtx(0, cookie)) {
        printf("actctx: DeactivateActCtx failed, error %lu\n", GetLastError());
        return 3;
    }
    ReleaseActCtx(ctx);
    if (!gdi32) return 4;
    printf("actctx: done\n");
    return 42;
}
