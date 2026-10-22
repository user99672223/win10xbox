/* exit_probe.dll - a stand-in for "a DLL whose DllMain(DLL_PROCESS_DETACH) misbehaves at process exit".
 *
 * win10xbox.exe loads it when WIN10XBOX_TEST_EXIT_PROBE_DLL names it (host-win32/main.cpp). If the host leaves through
 * the normal process teardown (return from wmain -> exit() -> ExitProcess), ExitProcess terminates every other thread
 * and then calls this DllMain with DLL_PROCESS_DETACH and a non-NULL lpReserved, and the process ends with exit code 99:
 * the guest's exit code is lost, which is what combase.dll's detach did to host-win32-smoke on 532a3bf (there as an
 * unhandled STATUS_INVALID_PARAMETER). If the host ends with TerminateProcess, as it must, this code never runs and the
 * exit code is the guest's.
 *
 * Plain C, no CRT calls: the same source builds with llvm-mingw (Wine) and MSVC.
 */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)module;
    /* lpReserved != NULL: the process is terminating (FreeLibrary passes NULL). */
    if (reason == DLL_PROCESS_DETACH && reserved != NULL)
        TerminateProcess(GetCurrentProcess(), 99);
    return TRUE;
}
