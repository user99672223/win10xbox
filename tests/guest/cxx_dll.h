/* Shared by cxx_runtime.cpp and cxx_dll.cpp: a C++ exception type that crosses the DLL boundary. */
#pragma once
#ifdef CXXDLL_BUILD
#define CXXDLL_API __declspec(dllexport)
#else
#define CXXDLL_API __declspec(dllimport)
#endif

struct CxxBoom {
    int code;
};

extern "C" {
/* Throws CxxBoom{code} from inside the DLL. */
CXXDLL_API void cxxdll_throw(int code);
/* Calls cb() from a DLL frame; whatever cb throws unwinds through this frame. */
CXXDLL_API int cxxdll_call(void (*cb)(void));
/* Throws and catches inside the DLL; returns the caught code. */
CXXDLL_API int cxxdll_catch_inside(int code);
/* Set by a namespace-scope constructor in the DLL (runs from DllMain, before main). */
CXXDLL_API int cxxdll_ctor_ran(void);
/* The DLL's own thread_local counter, bumped per call on the calling thread. */
CXXDLL_API int cxxdll_tls_bump(void);
}
