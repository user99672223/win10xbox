/* xaudio2_9.dll (guest stub): XAudio2Create, run by the HOST's real xaudio2_9 (core/audio/audio.cpp). The
 * IXAudio2 it returns is the host's object; the HRESULT is the host's. */
#include "bridge.h"

__declspec(dllexport) HRESULT WINAPI XAudio2Create(void **xaudio2, UINT32 flags, UINT32 processor)
{
    struct w10x_audio_xaudio2_create a;
    if (!xaudio2) return E_INVALIDARG;
    *xaudio2 = NULL;
    a.object = NULL;
    a.flags = flags;
    a.processor = processor;
    a.hr = E_FAIL;
    if (w10x_audio_unix_call((const void *)&XAudio2Create, W10X_AUDIO_XAUDIO2_CREATE, &a)) return E_FAIL;
    *xaudio2 = a.object;
    return a.hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
