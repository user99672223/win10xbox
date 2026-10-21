// probe_payload.cpp - built as a tiny DLL (probe_payload.dll) shipped next to
// each host and copied into a writable location by category A to test
// out-of-package LoadLibrary and SEC_IMAGE mapping.
//
// Intentionally minimal: one exported function, a DllMain that does nothing,
// no CRT dependencies beyond default. Not static-CRT linked (we want a tiny
// relocation footprint that still forces the loader to apply base relocations
// so the SEC_IMAGE test is meaningful).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" __declspec(dllexport) int probe_payload_answer() {
    return 0xABCD;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    (void)reason;
    return TRUE;
}
