/* Shared by seh.cpp and seh_dll.cpp: a C++ exception type that crosses the DLL boundary. */
#pragma once
#ifdef SEHDLL_BUILD
#define SEHDLL_API __declspec(dllexport)
#else
#define SEHDLL_API __declspec(dllimport)
#endif

struct SehBoom {
    int code;
};

extern "C" {
/* Throws SehBoom{code} from inside the DLL. */
SEHDLL_API void sehdll_throw(int code);
/* Calls cb() from a DLL frame that owns an object with a destructor; whatever cb throws unwinds through this frame. */
SEHDLL_API int sehdll_call(void (*cb)(void));
/* How many times that frame's destructor ran (unwinding must run it). */
SEHDLL_API int sehdll_dtors(void);
/* Throws and catches inside the DLL; returns the caught code. */
SEHDLL_API int sehdll_catch_inside(int code);
/* Set by a namespace-scope constructor in the DLL (runs from DllMain, before main). */
SEHDLL_API int sehdll_ctor_ran(void);
/* Raises an access violation (write to address 0) from a DLL frame. */
SEHDLL_API void sehdll_fault(void);
}
