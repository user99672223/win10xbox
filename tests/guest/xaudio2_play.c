/* guest_xaudio2_play: XAudio2 2.9 engine + mastering voice + one stereo 16-bit 48 kHz source voice. Submits a 500 ms
 * 440 Hz buffer, queues a second one while the first plays, waits for OnStreamEnd, prints the voice state transitions
 * and GetVoiceDetails of the mastering voice. Declared by hand like xaudio2.c.
 *
 * Exit codes:
 *   42  both buffers played (OnStreamEnd arrived after about 1 s)
 *   43  no render endpoint: "xaudio2_play: no-endpoint hr=0x..." (the engine is not driven)
 *   2..17  any other failure, printed as "xaudio2_play: FAILED <step> hr=0x..."
 * With WIN10XBOX_AUDIO_NULL_ENDPOINT=1 the layer provides an endpoint (docs/TESTING.md). */
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



#pragma pack(push, 1)
typedef struct { void *pCurrentBufferContext; UINT32 BuffersQueued; UINT64 SamplesPlayed; } XA2_VOICE_STATE;
typedef struct { UINT32 CreationFlags, ActiveFlags, InputChannels, InputSampleRate; } XA2_VOICE_DETAILS;
typedef struct {
    UINT32 Flags, AudioBytes;
    const BYTE *pAudioData;
    UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount;
    void *pContext;
} XA2_BUFFER;
#pragma pack(pop)

typedef struct IVoice { const struct IVoiceVtbl *v; } IVoice;
struct IVoiceVtbl {   /* IXAudio2Voice (19 methods, no IUnknown) then IXAudio2SourceVoice */
    void (STDMETHODCALLTYPE *GetVoiceDetails)(IVoice *, XA2_VOICE_DETAILS *);
    void (STDMETHODCALLTYPE *voice_slots[17])(void);
    void (STDMETHODCALLTYPE *DestroyVoice)(IVoice *);
    HR (STDMETHODCALLTYPE *Start)(IVoice *, UINT32 flags, UINT32 set);
    HR (STDMETHODCALLTYPE *Stop)(IVoice *, UINT32 flags, UINT32 set);
    HR (STDMETHODCALLTYPE *SubmitSourceBuffer)(IVoice *, const void *buffer, const void *wma);
    HR (STDMETHODCALLTYPE *FlushSourceBuffers)(IVoice *);
    HR (STDMETHODCALLTYPE *Discontinuity)(IVoice *);
    HR (STDMETHODCALLTYPE *ExitLoop)(IVoice *, UINT32);
    void (STDMETHODCALLTYPE *GetState)(IVoice *, XA2_VOICE_STATE *, UINT32 flags);
};

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
struct Callback { const struct CallbackVtbl *v; HANDLE stream_end; volatile LONG buffer_starts, buffer_ends; };

static void STDMETHODCALLTYPE cb_pass_start(Callback *c, UINT32 n) { (void)c; (void)n; }
static void STDMETHODCALLTYPE cb_pass_end(Callback *c) { (void)c; }
static void STDMETHODCALLTYPE cb_stream_end(Callback *c) { SetEvent(c->stream_end); }
static void STDMETHODCALLTYPE cb_buffer_start(Callback *c, void *ctx) { (void)ctx; InterlockedIncrement(&c->buffer_starts); }
static void STDMETHODCALLTYPE cb_buffer_end(Callback *c, void *ctx) { (void)ctx; InterlockedIncrement(&c->buffer_ends); }
static void STDMETHODCALLTYPE cb_loop_end(Callback *c, void *ctx) { (void)c; (void)ctx; }
static void STDMETHODCALLTYPE cb_error(Callback *c, void *ctx, HR hr) { (void)c; (void)ctx; printf("xaudio2_play: OnVoiceError hr=0x%08lx\n", (unsigned long)hr); }
static const struct CallbackVtbl kCallbackVtbl = {cb_pass_start, cb_pass_end, cb_stream_end, cb_buffer_start, cb_buffer_end, cb_loop_end, cb_error};

typedef HR (WINAPI *XAudio2Create_t)(void **, UINT32, UINT32);

#define RATE 48000u
#define FRAMES (RATE / 2u)   /* 500 ms */

static int fail(const char *step, HR hr, int code)
{
    printf("xaudio2_play: FAILED %s hr=0x%08lx\n", step, (unsigned long)hr);
    fflush(stdout);
    return code;
}

static void fill(short *pcm, double phase0)
{
    UINT32 i;
    for (i = 0; i < FRAMES; i++) {
        short v = (short)(sin(2 * 3.14159265358979 * 440.0 * (i + phase0) / RATE) * 8000);
        pcm[2 * i] = v;
        pcm[2 * i + 1] = v;
    }
}

int main(void)
{
    HMODULE m;
    XAudio2Create_t create;
    IXA2 *xa = NULL;
    IVoice *master = NULL, *src = NULL;
    Callback cb;
    WAVEFORMATEX f;
    static short pcm1[FRAMES * 2], pcm2[FRAMES * 2];
    XA2_BUFFER b1, b2;
    XA2_VOICE_STATE state;
    XA2_VOICE_DETAILS det;
    HR hr;
    DWORD w, t0;
    UINT32 last_queued = 0xFFFFFFFFu, polls;
    int rc_step = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED_(hr)) return fail("CoInitializeEx", hr, 2);

    m = LoadLibraryW(L"xaudio2_9.dll");
    if (!m) return fail("LoadLibrary(xaudio2_9.dll)", (HR)(0x80070000 | (GetLastError() & 0xFFFF)), 3);
    create = (XAudio2Create_t)GetProcAddress(m, "XAudio2Create");
    if (!create) return fail("GetProcAddress(XAudio2Create)", (HR)0x8007007F, 4);

    /* Endpoint lookup first (Wine's FAudio crashes in CreateSourceVoice when there is no device, see xaudio2.c). */
    {
        IEnum *en = NULL;
        void *dev = NULL;
        hr = CoCreateInstance(&kClsidEnumerator, NULL, CLSCTX_INPROC_SERVER, &kIidEnumerator, (void **)&en);
        if (FAILED_(hr)) return fail("CoCreateInstance(MMDeviceEnumerator)", hr, 5);
        hr = en->v->GetDefaultAudioEndpoint(en, 0 /* eRender */, 0 /* eConsole */, &dev);
        if (dev) ((struct IUnkVtbl **)dev)[0]->Release(dev);
        en->v->u.Release(en);
        if (FAILED_(hr)) {
            printf("xaudio2_play: no-endpoint hr=0x%08lx\n", (unsigned long)hr);
            printf("xaudio2_play: RESULT no-endpoint hr=0x%08lx\n", (unsigned long)hr);
            fflush(stdout);
            return 43;
        }
    }

    hr = create((void **)&xa, 0, 0xFFFFFFFFu /* XAUDIO2_DEFAULT_PROCESSOR */);
    if (FAILED_(hr)) return fail("XAudio2Create", hr, 6);
    printf("xaudio2_play: XAudio2Create ok\n");
    hr = xa->v->CreateMasteringVoice(xa, (void **)&master, 0, 0, 0, NULL, NULL, 0);
    if (FAILED_(hr)) return fail("CreateMasteringVoice", hr, 7);
    memset(&det, 0, sizeof det);
    master->v->GetVoiceDetails(master, &det);
    printf("xaudio2_play: mastering voice: GetVoiceDetails CreationFlags=0x%x ActiveFlags=0x%x InputChannels=%u InputSampleRate=%u\n",
           (unsigned)det.CreationFlags, (unsigned)det.ActiveFlags, (unsigned)det.InputChannels, (unsigned)det.InputSampleRate);
    if (det.InputChannels == 0 || det.InputSampleRate == 0) return fail("mastering voice reports no channels/rate", 0, 8);

    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = RATE * 4;
    fill(pcm1, 0);
    fill(pcm2, FRAMES);

    memset(&cb, 0, sizeof cb);
    cb.v = &kCallbackVtbl;
    cb.stream_end = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = xa->v->CreateSourceVoice(xa, (void **)&src, &f, 0, 2.0f, &cb, NULL, NULL);
    if (FAILED_(hr)) return fail("CreateSourceVoice", hr, 9);
    printf("xaudio2_play: state: source voice created (stopped)\n");

    memset(&b1, 0, sizeof b1);
    b1.AudioBytes = sizeof pcm1;
    b1.pAudioData = (const BYTE *)pcm1;
    b1.pContext = &b1;
    b2 = b1;
    b2.Flags = 0x40; /* XAUDIO2_END_OF_STREAM */
    b2.pAudioData = (const BYTE *)pcm2;
    b2.pContext = &b2;
    hr = src->v->SubmitSourceBuffer(src, &b1, NULL);
    if (FAILED_(hr)) return fail("SubmitSourceBuffer(1)", hr, 10);
    hr = src->v->Start(src, 0, 0);
    if (FAILED_(hr)) return fail("IXAudio2SourceVoice::Start", hr, 11);
    t0 = GetTickCount();
    printf("xaudio2_play: state: started, buffer 1 (500 ms, 440 Hz, 48000 Hz stereo 16-bit) playing\n");
    /* the second buffer is queued while the first one plays */
    Sleep(100);
    src->v->GetState(src, &state, 0);
    printf("xaudio2_play: state: before queueing buffer 2: BuffersQueued=%u SamplesPlayed=%llu\n", (unsigned)state.BuffersQueued,
           (unsigned long long)state.SamplesPlayed);
    if (state.BuffersQueued < 1) rc_step = 1;
    hr = src->v->SubmitSourceBuffer(src, &b2, NULL);
    if (FAILED_(hr)) return fail("SubmitSourceBuffer(2)", hr, 12);
    src->v->GetState(src, &state, 0);
    printf("xaudio2_play: state: buffer 2 queued: BuffersQueued=%u\n", (unsigned)state.BuffersQueued);
    if (state.BuffersQueued < 2) rc_step = 2;

    /* OnStreamEnd (buffer 2 carries END_OF_STREAM); GetState polled in between, every change of BuffersQueued printed */
    polls = 0;
    for (;;) {
        w = WaitForSingleObject(cb.stream_end, 10);
        src->v->GetState(src, &state, 0);
        polls++;
        if (state.BuffersQueued != last_queued) {
            printf("xaudio2_play: state: t=%lu ms BuffersQueued=%u SamplesPlayed=%llu\n", (unsigned long)(GetTickCount() - t0),
                   (unsigned)state.BuffersQueued, (unsigned long long)state.SamplesPlayed);
            last_queued = state.BuffersQueued;
        }
        if (w == WAIT_OBJECT_0) break;
        if (GetTickCount() - t0 > 10000) return fail("OnStreamEnd never arrived", (HR)w, 13);
    }
    src->v->GetState(src, &state, 0);
    printf("xaudio2_play: OnStreamEnd after %lu ms; OnBufferStart x%ld OnBufferEnd x%ld; final BuffersQueued=%u SamplesPlayed=%llu; polls=%u\n",
           (unsigned long)(GetTickCount() - t0), (long)cb.buffer_starts, (long)cb.buffer_ends, (unsigned)state.BuffersQueued,
           (unsigned long long)state.SamplesPlayed, (unsigned)polls);
    if (rc_step == 1) return fail("buffer 1 was not queued while playing", 0, 14);
    if (rc_step == 2) return fail("buffer 2 was not queued behind buffer 1", 0, 15);
    if (cb.buffer_starts < 2 || cb.buffer_ends < 2) return fail("expected two OnBufferStart/OnBufferEnd", 0, 16);
    if (GetTickCount() - t0 < 800) return fail("1 s of audio ended early: nothing consumed it in real time", 0, 17);

    src->v->Stop(src, 0, 0);
    printf("xaudio2_play: state: stopped\n");
    src->v->DestroyVoice(src);
    master->v->DestroyVoice(master);
    xa->v->u.Release(xa);   /* the engine thread ends here; the host side counts what its render client consumed */
    CoUninitialize();
    printf("xaudio2_play: RESULT played\n");
    printf("xaudio2_play: done\n");
    fflush(stdout);
    return 42;
}
