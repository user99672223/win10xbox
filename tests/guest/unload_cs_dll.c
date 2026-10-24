/* unload_cs_dll: DllMain initialises critical sections and never deletes them (docs/slices/unload-slist.md).
 *  - cs_dyn:    InitializeCriticalSection (the ntdll allocates the debug record).
 *  - cs_ex:     InitializeCriticalSectionEx with a spin count and flags.
 *  - cs_static: statically initialised the way Wine's DEFINE_CRITICAL_SECTION does it: the RTL_CRITICAL_SECTION_DEBUG
 *               record lives in this image's data. It is deleted at DLL_PROCESS_DETACH, so the host ntdll's
 *               RtlDeleteCriticalSection sees a debug record that is not one of its own.
 * The host calls the first two leaks; the third is the one that used to put an address inside the image on the host
 * ntdll's free list of debug records. */
#include <windows.h>

static CRITICAL_SECTION cs_dyn, cs_ex;
static CRITICAL_SECTION cs_static;
static CRITICAL_SECTION_DEBUG cs_static_debug = {
    0, 0, &cs_static, { &cs_static_debug.ProcessLocksList, &cs_static_debug.ProcessLocksList }, 0, 0, { 0 }
};
static CRITICAL_SECTION cs_static = { &cs_static_debug, -1, 0, 0, 0, 0 };

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&cs_dyn);
        InitializeCriticalSectionEx(&cs_ex, 4000, 0x08000000 /* RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN */);
        EnterCriticalSection(&cs_static);
        LeaveCriticalSection(&cs_static);
        EnterCriticalSection(&cs_dyn);
        LeaveCriticalSection(&cs_dyn);
    } else if (reason == DLL_PROCESS_DETACH) {
        DeleteCriticalSection(&cs_static);
    }
    return TRUE;
}

__declspec(dllexport) int unload_cs_touch(void) { return 1; }
