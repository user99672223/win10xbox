/* guest_xaudio2: XAudio2 on top of WASAPI. XAudio2Create (from xaudio2_9.dll), a mastering voice, a source voice with
 * a callback, one buffer of 200 ms of a 440 Hz sine; the program waits for IXAudio2VoiceCallback::OnBufferEnd and
 * for the voice's SamplesPlayed to show the buffer was consumed.
 *
 * Declared by hand (vtable slot for slot), like wasapi.c, so no SDK header is needed.
 *
 * Exit codes:
 *   42  the buffer was played and OnBufferEnd arrived
 *   43  the host has no audio endpoint (GetDefaultAudioEndpoint failed, "xaudio2: RESULT no-endpoint hr=0x...");
 *       nothing is faked and the engine is not driven
 *   1   anything else (printed as "xaudio2: FAILED <step> hr=0x...")
 * tests/integration/audio_test.cmake runs the program on the host and through the layer and requires the same verdict. */
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

typedef long HR;
#define FAILED_(h) ((h) < 0)

struct IUnkVtbl {
    HR (STDMETHODCALLTYPE *QueryInterface)(void *, const GUID *, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
};

/* The MMDevice API, only to ask whether the machine has a render endpoint (see main). */
typedef struct IEnum { const struct IEnumVtbl *v; } IEnum;
struct IEnumVtbl {
    struct IUnkVtbl u;
    HR (STDMETHODCALLTYPE *EnumAudioEndpoints)(IEnum *, int, DWORD, void **);
    HR (STDMETHODCALLTYPE *GetDefaultAudioEndpoint)(IEnum *, int flow, int role, void **device);
};
static const GUID kClsidEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIidEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};

/* XAudio2 2.8/2.9: IXAudio2 is a COM interface, the voices are not. */
typedef struct IXA2 { const struct IXA2Vtbl *v; } IXA2;
struct IXA2Vtbl {
    struct IUnkVtbl u;
    HR (STDMETHODCALLTYPE *RegisterForCallbacks)(IXA2 *, void *);
    void (STDMETHODCALLTYPE *UnregisterForCallbacks)(IXA2 *, void *);
    HR (STDMETHODCALLTYPE *CreateSourceVoice)(IXA2 *, void **voice, const WAVEFORMATEX *fmt, UINT32 flags,
                                              float max_ratio, void *callback, const void *sends, const void *chain);
    HR (STDMETHODCALLTYPE *CreateSubmixVoice)(IXA2 *, void **, UINT32, UINT32, UINT32, UINT32, const void *, const void *);
    HR (STDMETHODCALLTYPE *CreateMasteringVoice)(IXA2 *, void **voice, UINT32 channels, UINT32 rate, UINT32 flags,
                                                 const WCHAR *device, const void *chain, int category);
    HR (STDMETHODCALLTYPE *StartEngine)(IXA2 *);
    void (STDMETHODCALLTYPE *StopEngine)(IXA2 *);
    HR (STDMETHODCALLTYPE *CommitChanges)(IXA2 *, UINT32);
};

/* xaudio2.h declares its structures under #pragma pack(push, 1) */
#pragma pack(push, 1)
typedef struct { void *pCurrentBufferContext; UINT32 BuffersQueued; UINT64 SamplesPlayed; } XA2_VOICE_STATE;
#pragma pack(pop)

typedef struct IVoice { const struct IVoiceVtbl *v; } IVoice;
struct IVoiceVtbl {   /* IXAudio2Voice (19 methods, no IUnknown) then IXAudio2SourceVoice */
    void (STDMETHODCALLTYPE *voice_slots[18])(void);
    void (STDMETHODCALLTYPE *DestroyVoice)(IVoice *);
    HR (STDMETHODCALLTYPE *Start)(IVoice *, UINT32 flags, UINT32 set);
    HR (STDMETHODCALLTYPE *Stop)(IVoice *, UINT32 flags, UINT32 set);
    HR (STDMETHODCALLTYPE *SubmitSourceBuffer)(IVoice *, const void *buffer, const void *wma);
    HR (STDMETHODCALLTYPE *FlushSourceBuffers)(IVoice *);
    HR (STDMETHODCALLTYPE *Discontinuity)(IVoice *);
    HR (STDMETHODCALLTYPE *ExitLoop)(IVoice *, UINT32);
    void (STDMETHODCALLTYPE *GetState)(IVoice *, XA2_VOICE_STATE *, UINT32 flags);
};

#pragma pack(push, 1)
typedef struct {
    UINT32 Flags, AudioBytes;
    const BYTE *pAudioData;
    UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount;
    void *pContext;
} XA2_BUFFER;
#pragma pack(pop)

/* IXAudio2VoiceCallback: seven methods, called from XAudio2's own thread. */
typedef struct Callback Callback;
struct CallbackVtbl {
    void (STDMETHODCALLTYPE *OnVoiceProcessingPassStart)(Callback *, UINT32);
    void (STDMETHODCALLTYPE *OnVoiceProcessingPassEnd)(Callback *);
    void (STDMETHODCALLTYPE *OnStreamEnd)(Callback *);
    void (STDMETHODCALLTYPE *OnBufferStart)(Callback *, void *);
    void (STDMETHODCALLTYPE *OnBufferEnd)(Callback *, void *);
    void (STDMETHODCALLTYPE *OnLoopEnd)(Callback *, void *);
    void (STDMETHODCALLTYPE *OnVoiceError)(Callback *, void *, HR);
};
struct Callback { const struct CallbackVtbl *v; HANDLE buffer_end; HANDLE stream_end; volatile LONG buffer_starts; void *context_seen; };

static void STDMETHODCALLTYPE cb_pass_start(Callback *c, UINT32 n) { (void)c; (void)n; }
static void STDMETHODCALLTYPE cb_pass_end(Callback *c) { (void)c; }
static void STDMETHODCALLTYPE cb_stream_end(Callback *c) { SetEvent(c->stream_end); }
static void STDMETHODCALLTYPE cb_buffer_start(Callback *c, void *ctx) { (void)ctx; InterlockedIncrement(&c->buffer_starts); }
static void STDMETHODCALLTYPE cb_buffer_end(Callback *c, void *ctx) { c->context_seen = ctx; SetEvent(c->buffer_end); }
static void STDMETHODCALLTYPE cb_loop_end(Callback *c, void *ctx) { (void)c; (void)ctx; }
static void STDMETHODCALLTYPE cb_error(Callback *c, void *ctx, HR hr) { (void)c; (void)ctx; printf("xaudio2: OnVoiceError hr=0x%08lx\n", (unsigned long)hr); }
static const struct CallbackVtbl kCallbackVtbl = {cb_pass_start, cb_pass_end, cb_stream_end, cb_buffer_start, cb_buffer_end, cb_loop_end, cb_error};

typedef HR (WINAPI *XAudio2Create_t)(void **, UINT32, UINT32);

#define RATE 48000u
#define FRAMES (RATE / 5u)   /* 200 ms */

static int fail(const char *step, HR hr)
{
    printf("xaudio2: FAILED %s hr=0x%08lx\n", step, (unsigned long)hr);
    fflush(stdout);
    return 1;
}

int main(void)
{
    HMODULE m;
    XAudio2Create_t create;
    IXA2 *xa = NULL;
    IVoice *master = NULL, *src = NULL;
    Callback cb;
    WAVEFORMATEX f;
    static short pcm[FRAMES];
    XA2_BUFFER b;
    XA2_VOICE_STATE state;
    HR hr;
    DWORD w;
    int i;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED_(hr)) return fail("CoInitializeEx", hr);

    m = LoadLibraryW(L"xaudio2_9.dll");
    if (!m) return fail("LoadLibrary(xaudio2_9.dll)", (HR)(0x80070000 | (GetLastError() & 0xFFFF)));
    create = (XAudio2Create_t)GetProcAddress(m, "XAudio2Create");
    if (!create) return fail("GetProcAddress(XAudio2Create)", (HR)0x8007007F);
    hr = create((void **)&xa, 0, 0xFFFFFFFFu /* XAUDIO2_DEFAULT_PROCESSOR */);
    if (FAILED_(hr)) return fail("XAudio2Create", hr);
    printf("xaudio2: XAudio2Create ok\n");

    /* Whether there is anything to play on is WASAPI's answer. Native XAudio2 fails CreateMasteringVoice without an
     * endpoint; Wine's xaudio2 (FAudio underneath) ignores that failure, returns S_OK with a dead voice and crashes in
     * the first CreateSourceVoice, so the engine is only driven when an endpoint exists: nothing is faked, the exact
     * HRESULT of the endpoint lookup is the verdict. */
    {
        IEnum *en = NULL;
        void *dev = NULL;
        hr = CoCreateInstance(&kClsidEnumerator, NULL, CLSCTX_INPROC_SERVER, &kIidEnumerator, (void **)&en);
        if (FAILED_(hr)) return fail("CoCreateInstance(MMDeviceEnumerator)", hr);
        hr = en->v->GetDefaultAudioEndpoint(en, 0 /* eRender */, 0 /* eConsole */, &dev);
        if (dev) ((struct IUnkVtbl **)dev)[0]->Release(dev);
        en->v->u.Release(en);
        if (FAILED_(hr)) {
            printf("xaudio2: RESULT no-endpoint hr=0x%08lx\n", (unsigned long)hr);
            fflush(stdout);
            xa->v->u.Release(xa);
            return 43;
        }
    }
    hr = xa->v->CreateMasteringVoice(xa, (void **)&master, 0 /* default channels */, 0 /* default rate */, 0, NULL, NULL, 0);
    if (FAILED_(hr)) return fail("CreateMasteringVoice", hr);
    printf("xaudio2: mastering voice created\n");

    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = RATE * 2;
    for (i = 0; i < (int)FRAMES; i++) pcm[i] = (short)(sin(2 * 3.14159265358979 * 440.0 * i / RATE) * 6000);

    memset(&cb, 0, sizeof cb);
    cb.v = &kCallbackVtbl;
    cb.buffer_end = CreateEventW(NULL, FALSE, FALSE, NULL);
    cb.stream_end = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = xa->v->CreateSourceVoice(xa, (void **)&src, &f, 0, 2.0f, &cb, NULL, NULL);
    if (FAILED_(hr)) return fail("CreateSourceVoice", hr);
    printf("xaudio2: source voice created\n");

    memset(&b, 0, sizeof b);
    b.Flags = 0x40; /* XAUDIO2_END_OF_STREAM */
    b.AudioBytes = sizeof pcm;
    b.pAudioData = (const BYTE *)pcm;
    b.pContext = &b;
    hr = src->v->SubmitSourceBuffer(src, &b, NULL);
    if (FAILED_(hr)) return fail("SubmitSourceBuffer", hr);
    hr = src->v->Start(src, 0, 0);
    if (FAILED_(hr)) return fail("IXAudio2SourceVoice::Start", hr);
    printf("xaudio2: buffer of %u frames (200 ms of 440 Hz) submitted, voice started\n", FRAMES);

    /* While it plays: the voice reports the buffer queued and the samples played advancing; then OnBufferEnd. */
    {
        UINT64 max_played = 0;
        UINT32 max_queued = 0;
        for (i = 0; i < 800; i++) {   /* up to 8 s */
            w = WaitForSingleObject(cb.buffer_end, 10);
            if (w == WAIT_OBJECT_0) break;
            src->v->GetState(src, &state, 0);
            if (state.SamplesPlayed > max_played) max_played = state.SamplesPlayed;
            if (state.BuffersQueued > max_queued) max_queued = state.BuffersQueued;
        }
        if (w != WAIT_OBJECT_0) return fail("OnBufferEnd never arrived", (HR)w);
        if (cb.context_seen != &b) {
            printf("xaudio2: pBufferContext %p, expected %p\n", cb.context_seen, (void *)&b);
            return fail("OnBufferEnd got the wrong pBufferContext", 0);
        }
        w = WaitForSingleObject(cb.stream_end, 2000);
        printf("xaudio2: OnBufferEnd received after about %d ms; OnStreamEnd %s; while playing: BuffersQueued max %u, "
               "SamplesPlayed max %llu; OnBufferStart x%ld\n", i * 10, w == WAIT_OBJECT_0 ? "received" : "not seen",
               (unsigned)max_queued, (unsigned long long)max_played, (long)cb.buffer_starts);
        if (cb.buffer_starts < 1) return fail("OnBufferStart never arrived", 0);
        if (max_queued < 1) return fail("the voice never reported the buffer queued", 0);
        if (max_played < 1) return fail("SamplesPlayed never advanced while the buffer played", 0);
        if (i < 5) return fail("the 200 ms buffer ended at once: nothing consumed it in real time", (HR)i);
    }

    src->v->DestroyVoice(src);
    master->v->DestroyVoice(master);
    xa->v->u.Release(xa);
    CoUninitialize();
    printf("xaudio2: RESULT played\n");
    printf("xaudio2: done\n");
    fflush(stdout);
    return 42;
}
