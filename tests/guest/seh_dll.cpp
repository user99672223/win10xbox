/* guest_seh_dll: the DLL half of seh.cpp (C++ throw across a DLL boundary, unwinding through a DLL frame, a global
 * constructor run from DllMain, an access violation raised from a DLL frame). Built with the same runtime as its
 * executable. */
#define SEHDLL_BUILD
#include <windows.h>

#include "seh_dll.h"

namespace {
struct Ctor {
    int ran;
    Ctor() : ran(1) {}
};
Ctor g_ctor;
int g_dtors = 0;
struct Guard {
    ~Guard() { ++g_dtors; }
};
} // namespace

void sehdll_throw(int code) { throw SehBoom{code}; }

int sehdll_call(void (*cb)(void)) {
    Guard g;
    cb();
    return 0;
}

int sehdll_dtors(void) { return g_dtors; }

int sehdll_catch_inside(int code) {
    try {
        throw SehBoom{code};
    } catch (const SehBoom& b) {
        return b.code;
    }
    return -1;
}

int sehdll_ctor_ran(void) { return g_ctor.ran; }

void sehdll_fault(void) {
    int* volatile p = nullptr;
    *p = 1;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
