/* guest_wasapi: plays sound through WASAPI. CoInitializeEx, the MMDeviceEnumerator class, the default render endpoint,
 * IAudioClient::Initialize in shared mode at 48 kHz (float, stereo; the engine converts), 200 ms of a 440 Hz sine
 * through IAudioRenderClient, then IAudioClient::GetCurrentPadding and IAudioClock::GetPosition must show the device
 * consuming it: the position advances and the padding drains to zero.
 *
 * The COM interfaces it needs are declared below, vtable slot for slot, so that no SDK audio header or GUID library
 * is required and the same source builds with MSVC and with llvm-mingw.
 *
 * Exit codes:
 *   42  the whole sequence worked (a device played the sine)
 *   43  the host has no render endpoint: GetDefaultAudioEndpoint failed (prints "wasapi: RESULT no-endpoint hr=0x...",
 *       E_NOTFOUND 0x80070490 on a machine without a device); nothing is faked, the sequence stops there
 *   1   anything else went wrong (the failing step is printed as "wasapi: FAILED <step> hr=0x...")
 * tests/integration/audio_test.cmake runs the program on the host and through the layer and requires the same verdict
 * (and the same HRESULT in the no-endpoint case) from both. */
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

typedef long HR;
#define FAILED_(h) ((h) < 0)

static const GUID kClsidEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIidEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIidAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID kIidRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID kIidAudioClock = {0xCD63314F, 0x3FBA, 0x4A1B, {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};

struct IUnkVtbl {
    HR (STDMETHODCALLTYPE *QueryInterface)(void *, const GUID *, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
};

typedef struct IEnum { const struct IEnumVtbl *v; } IEnum;
struct IEnumVtbl {
    struct IUnkVtbl u;
    HR (STDMETHODCALLTYPE *EnumAudioEndpoints)(IEnum *, int, DWORD, void **);
    HR (STDMETHODCALLTYPE *GetDefaultAudioEndpoint)(IEnum *, int flow, int role, void **device);
    HR (STDMETHODCALLTYPE *GetDevice)(IEnum *, const WCHAR *, void **);
    HR (STDMETHODCALLTYPE *RegisterEndpointNotificationCallback)(IEnum *, void *);
    HR (STDMETHODCALLTYPE *UnregisterEndpointNotificationCallback)(IEnum *, void *);
};

typedef struct IDev { const struct IDevVtbl *v; } IDev;
struct IDevVtbl {
    struct IUnkVtbl u;
    HR (STDMETHODCALLTYPE *Activate)(IDev *, const GUID *iid, DWORD ctx, void *params, void **out);
    HR (STDMETHODCALLTYPE *OpenPropertyStore)(IDev *, DWORD, void **);
    HR (STDMETHODCALLTYPE *GetId)(IDev *, WCHAR **);
    HR (STDMETHODCALLTYPE *GetState)(IDev *, DWORD *);
};

typedef struct IClient { const struct IClientVtbl *v; } IClient;
struct IClientVtbl {
    struct IUnkVtbl u;
    HR (STDMETHODCALLTYPE *Initialize)(IClient *, int mode, DWORD flags, LONGLONG duration, LONGLONG period,
                                       const WAVEFORMATEX *fmt, const GUID *session);
    HR (STDMETHODCALLTYPE *GetBufferSize)(IClient *, UINT32 *);
    HR (STDMETHODCALLTYPE *GetStreamLatency)(IClient *, LONGLONG *);
    HR (STDMETHODCALLTYPE *GetCurrentPadding)(IClient *, UINT32 *);
    HR (STDMETHODCALLTYPE *IsFormatSupported)(IClient *, int, const WAVEFORMATEX *, WAVEFORMATEX **);
    HR (STDMETHODCALLTYPE *GetMixFormat)(IClient *, WAVEFORMATEX **);
    HR (STDMETHODCALLTYPE *GetDevicePeriod)(IClient *, LONGLONG *, LONGLONG *);
    HR (STDMETHODCALLTYPE *Start)(IClient *);
    HR (STDMETHODCALLTYPE *Stop)(IClient *);
    HR (STDMETHODCALLTYPE *Reset)(IClient *);
    HR (STDMETHODCALLTYPE *SetEventHandle)(IClient *, HANDLE);
    HR (STDMETHODCALLTYPE *GetService)(IClient *, const GUID *, void **);
};

typedef struct IRender { const struct IRenderVtbl *v; } IRender;
struct IRenderVtbl {
    struct IUnkVtbl u;
    HR (STDMETHODCALLTYPE *GetBuffer)(IRender *, UINT32 frames, BYTE **data);
    HR (STDMETHODCALLTYPE *ReleaseBuffer)(IRender *, UINT32 frames, DWORD flags);
};

typedef struct IClock { const struct IClockVtbl *v; } IClock;
struct IClockVtbl {
    struct IUnkVtbl u;
    HR (STDMETHODCALLTYPE *GetFrequency)(IClock *, UINT64 *);
    HR (STDMETHODCALLTYPE *GetPosition)(IClock *, UINT64 *pos, UINT64 *qpc);
    HR (STDMETHODCALLTYPE *GetCharacteristics)(IClock *, DWORD *);
};

#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM_ 0x80000000u
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY_ 0x08000000u
#define RATE 48000u
#define CHANNELS 2u
#define MS 200u
#define FRAMES (RATE * MS / 1000u)

static int fail(const char *step, HR hr)
{
    printf("wasapi: FAILED %s hr=0x%08lx\n", step, (unsigned long)hr);
    fflush(stdout);
    return 1;
}

int main(void)
{
    IEnum *en = NULL;
    IDev *dev = NULL;
    IClient *cl = NULL;
    IRender *rc = NULL;
    IClock *clock = NULL;
    WAVEFORMATEX *mix = NULL;
    WAVEFORMATEX fmt;
    UINT32 bufsize = 0, pad = 0, pad0 = 0;
    UINT64 freq = 0, pos = 0, pos_first = 0, pos_last = 0;
    LONGLONG latency = 0, defper = 0, minper = 0;
    BYTE *data = NULL;
    HR hr;
    int i, advanced = 0, drained = 0, monotonic = 1;
    double phase = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED_(hr)) return fail("CoInitializeEx", hr);
    printf("wasapi: CoInitializeEx ok\n");

    hr = CoCreateInstance(&kClsidEnumerator, NULL, CLSCTX_INPROC_SERVER, &kIidEnumerator, (void **)&en);
    if (FAILED_(hr)) return fail("CoCreateInstance(MMDeviceEnumerator)", hr);
    printf("wasapi: MMDeviceEnumerator created\n");

    hr = en->v->GetDefaultAudioEndpoint(en, 0 /* eRender */, 0 /* eConsole */, (void **)&dev);
    if (FAILED_(hr)) {
        /* No render endpoint on this machine: the exact answer, not a pretence of success. */
        printf("wasapi: RESULT no-endpoint hr=0x%08lx\n", (unsigned long)hr);
        fflush(stdout);
        en->v->u.Release(en);
        return 43;
    }
    printf("wasapi: default render endpoint found\n");

    hr = dev->v->Activate(dev, &kIidAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&cl);
    if (FAILED_(hr)) return fail("IMMDevice::Activate(IAudioClient)", hr);
    hr = cl->v->GetMixFormat(cl, &mix);
    if (FAILED_(hr)) return fail("GetMixFormat", hr);
    printf("wasapi: mix format tag=0x%x channels=%u rate=%lu bits=%u\n", mix->wFormatTag, mix->nChannels,
           (unsigned long)mix->nSamplesPerSec, mix->wBitsPerSample);
    cl->v->GetDevicePeriod(cl, &defper, &minper);
    printf("wasapi: device period default=%lld min=%lld (100 ns)\n", defper, minper);

    /* 48 kHz stereo float, shared mode; the engine converts to the device format (AUTOCONVERTPCM). */
    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = CHANNELS;
    fmt.nSamplesPerSec = RATE;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = (WORD)(CHANNELS * 4);
    fmt.nAvgBytesPerSec = RATE * fmt.nBlockAlign;
    hr = cl->v->Initialize(cl, 0 /* AUDCLNT_SHAREMODE_SHARED */,
                           AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM_ | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY_,
                           10000000 /* 1 s */, 0, &fmt, NULL);
    if (FAILED_(hr)) return fail("IAudioClient::Initialize(shared, 48000 Hz float stereo)", hr);
    hr = cl->v->GetBufferSize(cl, &bufsize);
    if (FAILED_(hr)) return fail("GetBufferSize", hr);
    if (bufsize < FRAMES) { printf("wasapi: buffer of %u frames is smaller than the %u to render\n", (unsigned)bufsize, FRAMES); return fail("GetBufferSize value", (HR)bufsize); }
    hr = cl->v->GetStreamLatency(cl, &latency);
    if (FAILED_(hr)) return fail("GetStreamLatency", hr);
    printf("wasapi: initialized: buffer=%u frames, stream latency=%lld (100 ns)\n", (unsigned)bufsize, latency);

    hr = cl->v->GetService(cl, &kIidRenderClient, (void **)&rc);
    if (FAILED_(hr)) return fail("GetService(IAudioRenderClient)", hr);
    hr = cl->v->GetService(cl, &kIidAudioClock, (void **)&clock);
    if (FAILED_(hr)) return fail("GetService(IAudioClock)", hr);
    hr = clock->v->GetFrequency(clock, &freq);
    if (FAILED_(hr) || !freq) return fail("IAudioClock::GetFrequency", hr);

    hr = rc->v->GetBuffer(rc, FRAMES, &data);
    if (FAILED_(hr)) return fail("IAudioRenderClient::GetBuffer", hr);
    for (i = 0; i < (int)FRAMES; i++) {
        float s = (float)(sin(phase) * 0.2);
        phase += 2 * 3.14159265358979 * 440.0 / RATE;
        ((float *)data)[i * 2] = s;
        ((float *)data)[i * 2 + 1] = s;
    }
    hr = rc->v->ReleaseBuffer(rc, FRAMES, 0);
    if (FAILED_(hr)) return fail("IAudioRenderClient::ReleaseBuffer", hr);
    hr = cl->v->GetCurrentPadding(cl, &pad0);
    if (FAILED_(hr)) return fail("GetCurrentPadding (before Start)", hr);
    printf("wasapi: rendered %u frames (200 ms of 440 Hz); padding before Start=%u\n", FRAMES, (unsigned)pad0);
    if (pad0 != FRAMES) { printf("wasapi: padding before Start is %u, expected %u\n", (unsigned)pad0, FRAMES); return fail("padding before Start", (HR)pad0); }

    hr = clock->v->GetPosition(clock, &pos_first, NULL);
    if (FAILED_(hr)) return fail("IAudioClock::GetPosition (before Start)", hr);
    hr = cl->v->Start(cl);
    if (FAILED_(hr)) return fail("IAudioClient::Start", hr);
    printf("wasapi: started; clock frequency=%llu\n", (unsigned long long)freq);

    pos_last = pos_first;
    for (i = 0; i < 600; i++) {   /* up to 6 s */
        Sleep(10);
        hr = cl->v->GetCurrentPadding(cl, &pad);
        if (FAILED_(hr)) return fail("GetCurrentPadding", hr);
        hr = clock->v->GetPosition(clock, &pos, NULL);
        if (FAILED_(hr)) return fail("IAudioClock::GetPosition", hr);
        if (pos < pos_last) monotonic = 0;
        pos_last = pos;
        if (pos > pos_first) advanced = 1;
        if (pad == 0 && advanced) { drained = 1; break; }
    }
    printf("wasapi: after %d ms: padding=%u position=%llu (%.3f s of the clock) advanced=%d drained=%d\n", (i + 1) * 10,
           (unsigned)pad, (unsigned long long)pos, (double)pos / (double)freq, advanced, drained);
    if (!advanced) return fail("the position did not advance", 0);
    if (!monotonic) return fail("the position went backwards", 0);
    if (!drained) return fail("the padding did not drain to zero", (HR)pad);
    {
        double secs = (double)pos / (double)freq;
        if (secs < 0.15 || secs > 0.6) {   /* 200 ms were played: the clock must say about that */
            printf("wasapi: the clock says %.3f s for 200 ms of audio\n", secs);
            return fail("the position does not match the audio played", 0);
        }
    }
    hr = cl->v->Stop(cl);
    if (FAILED_(hr)) return fail("IAudioClient::Stop", hr);
    hr = cl->v->Reset(cl);
    if (FAILED_(hr)) return fail("IAudioClient::Reset", hr);

    clock->v->u.Release(clock);
    rc->v->u.Release(rc);
    CoTaskMemFree(mix);
    cl->v->u.Release(cl);
    dev->v->u.Release(dev);
    en->v->u.Release(en);
    CoUninitialize();
    printf("wasapi: RESULT played\n");
    printf("wasapi: done\n");
    fflush(stdout);
    return 42;
}
