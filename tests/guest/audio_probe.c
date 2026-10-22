/* guest_audio_probe: what does the audio stack answer? A plain Win32 program (no SDK audio headers: the few
 * COM interfaces it needs are declared below, vtable slot for slot) that walks WASAPI and XAudio2 and prints
 * one line per step:
 *
 *   audio: <step> PASS hr=0x00000000 [detail]
 *   audio: <step> FAIL hr=0x80070490 [detail]
 *
 * A step whose prerequisite failed is not attempted; it prints FAIL with the prerequisite's hr and
 * "(blocked by <step>)". The program is a measurement: it exits 0 whatever the answers are. On a machine
 * without an audio device the expected answers are the host's "no device" codes (E_NOTFOUND from
 * GetDefaultAudioEndpoint, AUDCLNT_E_DEVICE_INVALIDATED / ERROR_NOT_FOUND from XAudio2), and the integration
 * test (tests/integration/audio_probe_test.cmake) requires the layer run to print the same hr for every step
 * as the same program run directly on the host.
 *
 * Steps: co_init; wasapi.{enumerator,default_endpoint,activate,mix_format,initialize,render_client,write}
 * (shared-mode IAudioClient, 200 ms of a 440 Hz sine through IAudioRenderClient); xaudio2.{load,create,
 * mastering,source,play} (XAudio2Create from xaudio2_9.dll, a mastering voice, a source voice playing 100 ms). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef long HR;
#define HR_FAIL(h) ((h) < 0)

static const GUID CLSID_Enumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_Enumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_AudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID IID_RenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

/* ---- the interfaces, declared as vtables ---------------------------------------------------------------- */
typedef struct IUnk { const struct IUnkVtbl *v; } IUnk;
struct IUnkVtbl { HR (STDMETHODCALLTYPE *QueryInterface)(void *, const GUID *, void **);
                  ULONG (STDMETHODCALLTYPE *AddRef)(void *); ULONG (STDMETHODCALLTYPE *Release)(void *); };

typedef struct IEnum { const struct IEnumVtbl *v; } IEnum;
struct IEnumVtbl {
    struct IUnkVtbl u;
    HR (STDMETHODCALLTYPE *EnumAudioEndpoints)(IEnum *, int, DWORD, void **);
    HR (STDMETHODCALLTYPE *GetDefaultAudioEndpoint)(IEnum *, int dataflow, int role, void **device);
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

/* XAudio2 2.8/2.9 (xaudio2_9.dll): IXAudio2 is a COM interface, the voices are not. */
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
typedef struct IVoice { const struct IVoiceVtbl *v; } IVoice;
struct IVoiceVtbl {   /* IXAudio2Voice (19 methods, no IUnknown) then IXAudio2SourceVoice */
    void (STDMETHODCALLTYPE *voice_slots[18])(void);
    void (STDMETHODCALLTYPE *DestroyVoice)(IVoice *);
    HR (STDMETHODCALLTYPE *Start)(IVoice *, UINT32 flags, UINT32 set);
    HR (STDMETHODCALLTYPE *Stop)(IVoice *, UINT32 flags, UINT32 set);
    HR (STDMETHODCALLTYPE *SubmitSourceBuffer)(IVoice *, const void *buffer, const void *wma);
};
typedef struct { UINT32 Flags, AudioBytes; const BYTE *pAudioData; UINT32 PlayBegin, PlayLength, LoopBegin,
                 LoopLength, LoopCount; void *pContext; } XA2_BUFFER;

/* ---- reporting -------------------------------------------------------------------------------------------- */
static void step(const char *name, HR hr, const char *detail)
{
    printf("audio: %s %s hr=0x%08lx%s%s\n", name, HR_FAIL(hr) ? "FAIL" : "PASS", (unsigned long)hr,
           detail && *detail ? " " : "", detail ? detail : "");
    fflush(stdout);
}
static void blocked(const char *name, const char *by, HR hr)
{
    char d[96];
    sprintf(d, "(blocked by %s)", by);
    step(name, HR_FAIL(hr) ? hr : (HR)0x80004005, d);
}

typedef HR (WINAPI *CoInitializeEx_t)(void *, DWORD);
typedef HR (WINAPI *CoCreateInstance_t)(const GUID *, void *, DWORD, const GUID *, void **);
typedef void (WINAPI *CoTaskMemFree_t)(void *);
typedef void (WINAPI *CoUninitialize_t)(void);
typedef HR (WINAPI *XAudio2Create_t)(void **, UINT32, UINT32);

static void wasapi(CoCreateInstance_t cocreate, CoTaskMemFree_t cofree)
{
    IEnum *en = NULL;
    IDev *dev = NULL;
    IClient *cl = NULL;
    IRender *rc = NULL;
    WAVEFORMATEX *fmt = NULL;
    UINT32 bufsize = 0;
    char d[160];
    HR hr;

    hr = cocreate(&CLSID_Enumerator, NULL, CLSCTX_INPROC_SERVER, &IID_Enumerator, (void **)&en);
    step("wasapi.enumerator", hr, "");
    if (HR_FAIL(hr)) { const char *n[] = {"default_endpoint", "activate", "mix_format", "initialize", "render_client", "write"};
                       int i; for (i = 0; i < 6; i++) { char s[48]; sprintf(s, "wasapi.%s", n[i]); blocked(s, "wasapi.enumerator", hr); } return; }

    hr = en->v->GetDefaultAudioEndpoint(en, 0 /* eRender */, 0 /* eConsole */, (void **)&dev);
    step("wasapi.default_endpoint", hr, "");
    if (HR_FAIL(hr)) { const char *n[] = {"activate", "mix_format", "initialize", "render_client", "write"};
                       int i; for (i = 0; i < 5; i++) { char s[48]; sprintf(s, "wasapi.%s", n[i]); blocked(s, "wasapi.default_endpoint", hr); }
                       en->v->u.Release(en); return; }

    hr = dev->v->Activate(dev, &IID_AudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&cl);
    step("wasapi.activate", hr, "");
    if (HR_FAIL(hr)) { const char *n[] = {"mix_format", "initialize", "render_client", "write"};
                       int i; for (i = 0; i < 4; i++) { char s[48]; sprintf(s, "wasapi.%s", n[i]); blocked(s, "wasapi.activate", hr); }
                       goto out; }

    hr = cl->v->GetMixFormat(cl, &fmt);
    if (!HR_FAIL(hr))
        sprintf(d, "tag=0x%x channels=%u rate=%lu bits=%u", fmt->wFormatTag, fmt->nChannels,
                (unsigned long)fmt->nSamplesPerSec, fmt->wBitsPerSample);
    else d[0] = 0;
    step("wasapi.mix_format", hr, d);
    if (HR_FAIL(hr)) { const char *n[] = {"initialize", "render_client", "write"};
                       int i; for (i = 0; i < 3; i++) { char s[48]; sprintf(s, "wasapi.%s", n[i]); blocked(s, "wasapi.mix_format", hr); }
                       goto out; }

    hr = cl->v->Initialize(cl, 0 /* AUDCLNT_SHAREMODE_SHARED */, 0, 10000000 /* 1 s */, 0, fmt, NULL);
    if (!HR_FAIL(hr)) { cl->v->GetBufferSize(cl, &bufsize); sprintf(d, "buffer=%u frames", (unsigned)bufsize); } else d[0] = 0;
    step("wasapi.initialize", hr, d);
    if (HR_FAIL(hr)) { blocked("wasapi.render_client", "wasapi.initialize", hr); blocked("wasapi.write", "wasapi.initialize", hr); goto out; }

    hr = cl->v->GetService(cl, &IID_RenderClient, (void **)&rc);
    step("wasapi.render_client", hr, "");
    if (HR_FAIL(hr)) { blocked("wasapi.write", "wasapi.render_client", hr); goto out; }

    {   /* 200 ms of 440 Hz */
        const UINT32 total = (UINT32)((unsigned long long)fmt->nSamplesPerSec * 200 / 1000);
        UINT32 done = 0, pad = 0;
        int is_float = fmt->wFormatTag == 3 || (fmt->wFormatTag == 0xFFFE && ((const DWORD *)((const BYTE *)fmt + 24))[0] == 3);
        double phase = 0;
        int spins = 0;
        hr = cl->v->Start(cl);
        while (!HR_FAIL(hr) && done < total && spins++ < 400) {
            UINT32 n;
            BYTE *p = NULL;
            hr = cl->v->GetCurrentPadding(cl, &pad);
            if (HR_FAIL(hr)) break;
            n = bufsize - pad;
            if (n > total - done) n = total - done;
            if (!n) { Sleep(5); continue; }
            hr = rc->v->GetBuffer(rc, n, &p);
            if (HR_FAIL(hr)) break;
            {
                UINT32 i, c;
                for (i = 0; i < n; i++) {
                    double s = sin(phase) * 0.2;
                    phase += 2 * 3.14159265358979 * 440.0 / fmt->nSamplesPerSec;
                    for (c = 0; c < fmt->nChannels; c++) {
                        if (is_float && fmt->wBitsPerSample == 32) ((float *)p)[i * fmt->nChannels + c] = (float)s;
                        else if (fmt->wBitsPerSample == 16) ((short *)p)[i * fmt->nChannels + c] = (short)(s * 32767);
                        else memset(p + ((size_t)i * fmt->nChannels + c) * (fmt->wBitsPerSample / 8), 0, fmt->wBitsPerSample / 8);
                    }
                }
            }
            hr = rc->v->ReleaseBuffer(rc, n, 0);
            done += n;
        }
        sprintf(d, "frames=%u/%u", (unsigned)done, (unsigned)total);
        step("wasapi.write", hr, d);
        cl->v->Stop(cl);
    }
out:
    if (rc) rc->v->u.Release(rc);
    if (fmt) cofree(fmt);
    if (cl) cl->v->u.Release(cl);
    if (dev) dev->v->u.Release(dev);
    en->v->u.Release(en);
}

static void xaudio2(void)
{
    static const char *rest[] = {"create", "mastering", "source", "play"};
    HMODULE m = LoadLibraryW(L"xaudio2_9.dll");
    const HR load_hr = m ? 0 : (HR)(0x80070000 | (GetLastError() & 0xFFFF));
    XAudio2Create_t create;
    IXA2 *xa = NULL;
    IVoice *master = NULL, *src = NULL;
    HR hr;
    char d[96];
    int i;

    step("xaudio2.load", load_hr, "");
    if (!m) { HR e = load_hr; for (i = 0; i < 4; i++) { char s[48]; sprintf(s, "xaudio2.%s", rest[i]); blocked(s, "xaudio2.load", e); } return; }
    create = (XAudio2Create_t)GetProcAddress(m, "XAudio2Create");
    if (!create) { HR e = (HR)0x8007007F; for (i = 0; i < 4; i++) { char s[48]; sprintf(s, "xaudio2.%s", rest[i]); blocked(s, "xaudio2.load", e); } return; }

    hr = create((void **)&xa, 0, 0xFFFFFFFFu /* XAUDIO2_DEFAULT_PROCESSOR */);
    step("xaudio2.create", hr, "");
    if (HR_FAIL(hr)) { for (i = 1; i < 4; i++) { char s[48]; sprintf(s, "xaudio2.%s", rest[i]); blocked(s, "xaudio2.create", hr); } return; }

    hr = xa->v->CreateMasteringVoice(xa, (void **)&master, 0 /* XAUDIO2_DEFAULT_CHANNELS */, 0, 0, NULL, NULL, 0);
    step("xaudio2.mastering", hr, "");
    if (HR_FAIL(hr)) { for (i = 2; i < 4; i++) { char s[48]; sprintf(s, "xaudio2.%s", rest[i]); blocked(s, "xaudio2.mastering", hr); } goto out; }

    {
        WAVEFORMATEX f;
        short pcm[4410];   /* 100 ms of 440 Hz, mono, 44.1 kHz */
        XA2_BUFFER b;
        memset(&f, 0, sizeof f);
        f.wFormatTag = 1; f.nChannels = 1; f.nSamplesPerSec = 44100; f.wBitsPerSample = 16;
        f.nBlockAlign = 2; f.nAvgBytesPerSec = 88200;
        for (i = 0; i < 4410; i++) pcm[i] = (short)(sin(2 * 3.14159265358979 * 440.0 * i / 44100.0) * 6000);
        hr = xa->v->CreateSourceVoice(xa, (void **)&src, &f, 0, 2.0f, NULL, NULL, NULL);
        step("xaudio2.source", hr, "");
        if (HR_FAIL(hr)) { blocked("xaudio2.play", "xaudio2.source", hr); goto out; }
        memset(&b, 0, sizeof b);
        b.Flags = 0x40; /* XAUDIO2_END_OF_STREAM */
        b.AudioBytes = sizeof pcm;
        b.pAudioData = (const BYTE *)pcm;
        hr = src->v->SubmitSourceBuffer(src, &b, NULL);
        if (!HR_FAIL(hr)) hr = src->v->Start(src, 0, 0);
        if (!HR_FAIL(hr)) Sleep(150);
        sprintf(d, "100 ms buffer");
        step("xaudio2.play", hr, d);
    }
out:
    if (src) src->v->DestroyVoice(src);
    if (master) master->v->DestroyVoice(master);
    xa->v->u.Release(xa);
}

int main(void)
{
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    CoInitializeEx_t coinit = ole ? (CoInitializeEx_t)GetProcAddress(ole, "CoInitializeEx") : NULL;
    CoCreateInstance_t cocreate = ole ? (CoCreateInstance_t)GetProcAddress(ole, "CoCreateInstance") : NULL;
    CoTaskMemFree_t cofree = ole ? (CoTaskMemFree_t)GetProcAddress(ole, "CoTaskMemFree") : NULL;
    CoUninitialize_t counit = ole ? (CoUninitialize_t)GetProcAddress(ole, "CoUninitialize") : NULL;
    HR hr;

    if (!coinit || !cocreate || !cofree || !counit) { step("co_init", (HR)0x8007007F, "(ole32 exports missing)"); return 0; }
    hr = coinit(NULL, 0 /* COINIT_MULTITHREADED */);
    step("co_init", hr, "");
    if (HR_FAIL(hr)) { printf("audio: done\n"); return 0; }
    wasapi(cocreate, cofree);
    xaudio2();
    counit();
    printf("audio: done\n");
    fflush(stdout);
    return 0;
}
