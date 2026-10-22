/* guest_cxx_dll: the DLL half of cxx_runtime.cpp (C++ throw across a DLL boundary, a global constructor run from
 * DllMain, a thread_local). Built with the same runtime as its executable. */
#define CXXDLL_BUILD
#include <windows.h>

#include "cxx_dll.h"

namespace {
struct Ctor {
    int ran;
    Ctor() : ran(1) {}
};
Ctor g_ctor;
thread_local int t_bump = 100;
} // namespace

extern "C" {
void cxxdll_throw(int code) { throw CxxBoom{code}; }

int cxxdll_call(void (*cb)(void)) {
    struct Guard {
        ~Guard() { ++*hits; }
        int* hits;
    };
    static int destructed = 0;
    Guard g{&destructed};
    cb();
    return 0;
}

int cxxdll_catch_inside(int code) {
    try {
        throw CxxBoom{code};
    } catch (const CxxBoom& b) {
        return b.code;
    }
    return -1;
}

int cxxdll_ctor_ran(void) { return g_ctor.ran; }

int cxxdll_tls_bump(void) { return ++t_bump; }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
