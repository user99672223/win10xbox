/* ABI between the guest stubs (core/audio/guest/*.c) and the host half (core/audio/audio.cpp): the argument
 * blocks of the unix calls of the "mmdevapi" and "xaudio2_9" unix libs (both share one table). */
#pragma once
#include <stdint.h>

enum {
    W10X_AUDIO_CREATE_INSTANCE = 0,   /* host mmdevapi: DllGetClassObject(clsid) -> IClassFactory::CreateInstance(riid) */
    W10X_AUDIO_XAUDIO2_CREATE  = 1,   /* host xaudio2_9: XAudio2Create(&obj, flags, processor) */
};

struct w10x_audio_create_instance {
    uint32_t clsid[4];     /* GUID, in memory order */
    uint32_t riid[4];
    void*    object;       /* out */
    int32_t  hr;           /* out: the HOST's HRESULT */
};

struct w10x_audio_xaudio2_create {
    void*    object;       /* out: IXAudio2* */
    uint32_t flags;
    uint32_t processor;
    int32_t  hr;           /* out: the HOST's HRESULT */
};
