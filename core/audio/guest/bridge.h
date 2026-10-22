/* Guest-side half of the audio bridge: the call into core/audio/audio.cpp through Wine's unix-call mechanism
 * (the same one Wine's own builtins use to reach their unix libraries: NtQueryVirtualMemory(MemoryWineLoadUnixLib)
 * for the handle of the unix lib named like this DLL, then ntdll's __wine_unix_call_dispatcher). Plain C, kernel32
 * imports only, so the stubs load whatever else the machine folder holds. */
#pragma once
#include <windows.h>
#include "../audio_abi.h"

typedef LONG (WINAPI *w10x_nqvm_t)(HANDLE, const void*, int, void*, SIZE_T, SIZE_T*);
typedef LONG (WINAPI *w10x_unix_call_t)(unsigned long long, unsigned int, void*);

/* Returns an NTSTATUS: 0 when the host function ran (its HRESULT is in the argument block). */
static LONG w10x_audio_unix_call(const void* self_address, unsigned int code, void* args)
{
    static unsigned long long handle;
    static w10x_unix_call_t *dispatcher;
    if (!handle) {
        HMODULE self = NULL, ntdll = GetModuleHandleW(L"ntdll.dll");
        w10x_nqvm_t nqvm;
        unsigned long long h = 0;
        LONG s;
        if (!ntdll || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                          (LPCWSTR)self_address, &self))
            return (LONG)0xC0000135;   /* STATUS_DLL_NOT_FOUND */
        nqvm = (w10x_nqvm_t)GetProcAddress(ntdll, "NtQueryVirtualMemory");
        dispatcher = (w10x_unix_call_t *)GetProcAddress(ntdll, "__wine_unix_call_dispatcher");
        if (!nqvm || !dispatcher || !*dispatcher) return (LONG)0xC0000139;   /* STATUS_ENTRYPOINT_NOT_FOUND */
        /* MemoryWineLoadUnixLib = 1000 */
        s = nqvm(GetCurrentProcess(), self, 1000, &h, sizeof(h), NULL);
        if (s) return s;
        handle = h;
    }
    return (*dispatcher)(handle, code, args);
}
