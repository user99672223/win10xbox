// mmdevapi's "unix side" (dlls/mmdevapi/unixlib.h, enum unix_funcs): the audio driver Wine's PE mmdevapi talks to.
//
// Wine's mmdevapi.dll is the whole WASAPI stack in PE code (IMMDeviceEnumerator, IAudioClient, IAudioRenderClient,
// IAudioClock, sessions, volumes); what it lacks is a driver, which on Wine is a unix library (winepulse.so,
// winealsa.so...) loaded with __wine_load_unix_lib("wine<name>.drv") and driven through __wine_unix_call. This file is
// that driver, on the HOST's WASAPI: on a Windows host the real IMMDeviceEnumerator / IAudioClient, on a Wine host
// Wine's own. The unix library is registered as "winewasapi"; the machine registry selects it
// (HKCU\Software\Wine\Drivers: Audio=wasapi, core/registry).
//
// Shape (the contract of Wine's own drivers, winealsa.drv/alsa.c is the closest reference):
//   * One default render endpoint and one default capture endpoint of the host, named by their host endpoint ID.
//     get_endpoint_ids reports what the host has (no endpoint: zero devices, which makes GetDefaultAudioEndpoint
//     answer E_NOTFOUND exactly like a host without one); get_mix_format is the host's own mix format.
//   * Shared mode only (exclusive: AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED, as winepulse). A stream owns a ring buffer
//     in this process that the guest writes (render) or reads (capture) through get/release_*_buffer; the host
//     stream is fed from it. Positions are the host's: frames played = frames pushed into the host stream minus the
//     host's own GetCurrentPadding, interpolated with the system timer between pump steps, never past what was pushed.
//     Latency is the host stream's GetStreamLatency plus the pump interval.
//   * All host COM work (the enumerator, IAudioClient, the render/capture client) happens on ONE host thread that
//     owns the COM apartment (AudioThread): guest threads run with the guest's view of the TEB, which host COM must
//     not see. The thread is started, and every DLL the host stack needs is loaded by a complete throw-away stream
//     cycle, in PrimeAudio() BEFORE the PEB take-over: afterwards PEB.Ldr is the guest's and a host LoadLibrary binds
//     to the wrong loader (docs/slices/net.md has the same story for Winsock). Guest threads only call into this
//     file; control-plane calls (create/release stream, start/stop/reset, format queries) are requests the audio thread
//     executes; the data plane (buffers, padding, positions) touches only the stream's own state under its lock.
//   * The guest-side `timer_loop` thread Wine creates at IAudioClient::Start just waits for the stream's quit event
//     (the pump is the audio thread; Wine's drivers pump in that thread, which would be host COM on a guest thread).
//   * Event-callback streams: the guest's event is signalled once per period by the pump, as the Wine drivers do.
//
// There are no function-local statics anywhere here (docs/DISPATCHER.md: guest-reachable host code); the state is
// one namespace-scope object. Everything is reached from guest threads in the host view (UnixCallDispatcher).
#include "internal.h"
#include "win10xbox/config.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propsys.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

WIN10XBOX_DECLARE_CHANNEL(audio);

namespace win10xbox::dispatch::detail {
namespace {

// ---- constants (spelled out: no SDK GUID libraries are linked) ------------------------------------------------
constexpr GUID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr GUID kIidMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr GUID kIidAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
constexpr GUID kIidRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
constexpr GUID kIidCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
constexpr GUID kPkeyFriendlyNameFmt = {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}};
constexpr GUID kPkeyPhysicalSpeakersFmt = {0x1DA5D803, 0xD492, 0x4EDD, {0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E}};
constexpr GUID kPkeyDevicePathFmt = {0xB3F8FA53, 0x0004, 0x438E, {0x90, 0x03, 0x51, 0xA4, 0x6E, 0x13, 0x9B, 0xFC}};
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

constexpr HRESULT kEFail = static_cast<HRESULT>(0x80004005);
constexpr HRESULT kENotImpl = static_cast<HRESULT>(0x80004001);
constexpr HRESULT kEOutOfMemory = static_cast<HRESULT>(0x8007000E);
constexpr HRESULT kEInsufficientBuffer = static_cast<HRESULT>(0x8007007A);   // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)
constexpr HRESULT kENotSufficientBuffer = kEInsufficientBuffer;               // E_NOT_SUFFICIENT_BUFFER is the same code
constexpr HRESULT kEInvalidated = AUDCLNT_E_DEVICE_INVALIDATED;
constexpr HRESULT kEOutOfOrder = AUDCLNT_E_OUT_OF_ORDER;
constexpr HRESULT kEBufferTooLarge = AUDCLNT_E_BUFFER_TOO_LARGE;
constexpr HRESULT kEInvalidSize = AUDCLNT_E_INVALID_SIZE;
constexpr HRESULT kENotStopped = AUDCLNT_E_NOT_STOPPED;
constexpr HRESULT kEBufferOpPending = AUDCLNT_E_BUFFER_OPERATION_PENDING;
constexpr HRESULT kEEventHandleNotSet = AUDCLNT_E_EVENTHANDLE_NOT_SET;
constexpr HRESULT kEEventHandleNotExpected = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
constexpr HRESULT kSBufferEmpty = AUDCLNT_S_BUFFER_EMPTY;
constexpr DWORD kStreamEventCallback = AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kStreamAutoConvertPcm = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                kStreamSrcDefaultQuality = AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
constexpr DWORD kBufferFlagSilent = AUDCLNT_BUFFERFLAGS_SILENT, kBufferFlagDiscontinuity = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
constexpr REFERENCE_TIME kPumpRt = 50000;   // 5 ms: the pump interval
constexpr REFERENCE_TIME kMinHostBufferRt = 1000000;   // 100 ms
constexpr unsigned kMaxStreams = 64;
constexpr std::uint32_t kStreamMagic = 0x57415341;   // "WASA"

// ---- Wine's unixlib.h argument blocks (dlls/mmdevapi/unixlib.h), field for field -------------------------------
enum WineFuncs : unsigned {
    kProcessAttach, kProcessDetach, kMainLoop, kGetEndpointIds, kCreateStream, kReleaseStream, kStart, kStop, kReset,
    kTimerLoop, kGetRenderBuffer, kReleaseRenderBuffer, kGetCaptureBuffer, kReleaseCaptureBuffer, kIsFormatSupported,
    kGetLoopbackCaptureDevice, kGetMixFormat, kGetDevicePeriod, kGetBufferSize, kGetLatency, kGetCurrentPadding,
    kGetNextPacketSize, kGetFrequency, kGetPosition, kSetVolumes, kSetEventHandle, kSetSampleRate, kTestConnect,
    kIsStarted, kGetPropValue, kMidiGetDriver, kMidiInit, kMidiRelease, kMidiOutMessage, kMidiInMessage,
    kMidiNotifyWait, kAuxMessage, kFuncsCount
};
struct WEndpoint { unsigned int name; unsigned int device; };
struct WMainLoopParams { HANDLE event; };
struct WGetEndpointIdsParams { int flow; WEndpoint* endpoints; unsigned int size; HRESULT result; unsigned int num; unsigned int default_idx; };
struct WCreateStreamParams {
    const WCHAR* name; const char* device; int flow; int share; DWORD flags; REFERENCE_TIME duration;
    REFERENCE_TIME period; const WAVEFORMATEX* fmt; HRESULT result; UINT32* channel_count; UINT64* stream;
};
struct WReleaseStreamParams { UINT64 stream; HANDLE timer_thread; HRESULT result; };
struct WStreamResultParams { UINT64 stream; HRESULT result; };   // start, stop, reset, is_started
struct WTimerLoopParams { UINT64 stream; };
struct WGetRenderBufferParams { UINT64 stream; UINT32 frames; HRESULT result; BYTE** data; };
struct WReleaseRenderBufferParams { UINT64 stream; UINT32 written_frames; UINT flags; HRESULT result; };
struct WGetCaptureBufferParams { UINT64 stream; HRESULT result; BYTE** data; UINT32* frames; UINT* flags; UINT64* devpos; UINT64* qpcpos; };
struct WReleaseCaptureBufferParams { UINT64 stream; UINT32 done; HRESULT result; };
struct WIsFormatSupportedParams { const char* device; int flow; int share; const WAVEFORMATEX* fmt_in; HRESULT result; };
struct WGetLoopbackParams { const WCHAR* name; const char* device; char* ret_device; UINT32 ret_device_len; HRESULT result; };
struct WGetMixFormatParams { const char* device; int flow; WAVEFORMATEXTENSIBLE* fmt; HRESULT result; };
struct WGetDevicePeriodParams { const char* device; int flow; HRESULT result; REFERENCE_TIME* def_period; REFERENCE_TIME* min_period; };
struct WGetBufferSizeParams { UINT64 stream; HRESULT result; UINT32* frames; };
struct WGetLatencyParams { UINT64 stream; HRESULT result; REFERENCE_TIME* latency; };
struct WGetCurrentPaddingParams { UINT64 stream; HRESULT result; UINT32* padding; };
struct WGetNextPacketSizeParams { UINT64 stream; HRESULT result; UINT32* frames; };
struct WGetFrequencyParams { UINT64 stream; HRESULT result; UINT64* freq; };
struct WGetPositionParams { UINT64 stream; BOOL device; HRESULT result; UINT64* pos; UINT64* qpctime; };
struct WSetVolumesParams { UINT64 stream; float master_volume; const float* volumes; const float* session_volumes; };
struct WSetEventHandleParams { UINT64 stream; HANDLE event; HRESULT result; };
struct WTestConnectParams { const WCHAR* name; int priority; };
struct WGetPropValueParams {
    const char* device; int flow; const GUID* guid; const PROPERTYKEY* prop; HRESULT result; PROPVARIANT* value;
    void* buffer; unsigned int* buffer_size;
};
struct WMidiInitParams { UINT* err; };
struct WNotifyContext {
    BOOL send_notify; WORD dev_id; WORD msg; UINT_PTR param_1; UINT_PTR param_2; UINT_PTR callback; UINT flags;
    HANDLE device; UINT_PTR instance;
};
struct WMidiNotifyWaitParams { BOOL* quit; WNotifyContext* notify; };
constexpr int kFlowRender = 0, kFlowCapture = 1;   // EDataFlow
constexpr int kPriorityUnavailable = 0, kPriorityNeutral = 2;

// ---- state --------------------------------------------------------------------------------------------------------
struct Endpoint {
    bool present = false;
    WCHAR name[256] = {};
    WCHAR id[160] = {};
    char device[480] = {};
    WAVEFORMATEXTENSIBLE mix = {};
    REFERENCE_TIME def_period = 0, min_period = 0;
    IAudioClient* query = nullptr;   // never initialised: IsFormatSupported
};

struct Request {
    Request* next;
    void (*fn)(void*);
    void* arg;
    HANDLE done;
};

struct Stream {
    std::uint32_t magic = kStreamMagic;
    SRWLOCK lock = SRWLOCK_INIT;
    int flow = kFlowRender;
    DWORD flags = 0;
    // format (the guest's): host conversion does the rest
    BYTE fmt_raw[64] = {};
    UINT32 channels = 0, rate = 0, block = 0, bits = 0;
    bool is_float = false;
    // host objects: audio thread only
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    IAudioCaptureClient* capture = nullptr;
    UINT32 host_frames = 0;
    REFERENCE_TIME host_latency = 0;
    // the guest-visible ring
    BYTE* ring = nullptr;
    UINT32 ring_frames = 0;
    BYTE* tmp = nullptr;
    UINT32 tmp_frames = 0;
    UINT32 period_frames = 0;
    REFERENCE_TIME period_rt = 0;
    float* vols = nullptr;
    bool vols_unity = true;
    // state, under `lock`
    bool started = false;
    HANDLE event = nullptr;
    HANDLE quit = nullptr;       // manual-reset: ends the guest timer_loop thread
    HRESULT error = S_OK;        // sticky host failure (device invalidated...)
    int getbuf_last = 0;         // +frames: ring chunk, -frames: tmp buffer (render); frames (capture)
    UINT32 wri_offs = 0;         // next write (render) / next host fill (capture)
    UINT32 rd_offs = 0;          // next host push (render) / next guest read (capture)
    UINT32 held = 0;             // capture: frames in the ring
    UINT64 written = 0;          // render: frames the guest released; capture: frames the guest consumed
    UINT64 pushed = 0;           // render: frames given to the host stream; capture: frames taken from it
    UINT64 played = 0;           // render: frames the host has played, as of played_qpc
    LONGLONG played_qpc = 0;
    UINT64 last_pos = 0;
    LONGLONG next_event_qpc = 0;
    bool discontinuity = false;
};

struct Audio {
    bool enabled = false;        // PrimeAudio ran the thread
    bool stack_ok = false;       // the host's enumerator could be created
    HANDLE thread = nullptr, wake = nullptr, ready = nullptr, timer = nullptr;
    SRWLOCK queue_lock = SRWLOCK_INIT;
    Request* head = nullptr;
    Request* tail = nullptr;
    SRWLOCK streams_lock = SRWLOCK_INIT;
    Stream* streams[kMaxStreams] = {};
    unsigned stream_count = 0;
    Endpoint ep[2];
    IMMDeviceEnumerator* enumerator = nullptr;
    LONGLONG qpc_freq = 1;
    // ole32 and winmm, resolved dynamically (nothing here links them)
    HRESULT(WINAPI* co_initialize_ex)(void*, DWORD) = nullptr;
    HRESULT(WINAPI* co_create_instance)(REFCLSID, IUnknown*, DWORD, REFIID, void**) = nullptr;
    void(WINAPI* co_task_mem_free)(void*) = nullptr;
    HRESULT(WINAPI* prop_variant_clear)(PROPVARIANT*) = nullptr;
};
Audio g_audio;

// ---- small helpers ------------------------------------------------------------------------------------------------
UINT64 MulDiv64(UINT64 a, UINT64 b, UINT64 c) {
    if (!c) return 0;
    return (a / c) * b + ((a % c) * b) / c;
}

LONGLONG Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

UINT64 QpcTo100ns(LONGLONG qpc) { return MulDiv64(static_cast<UINT64>(qpc), 10000000, static_cast<UINT64>(g_audio.qpc_freq)); }

bool IsFloatFormat(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
        return IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat, kSubtypeFloat) != 0;
    return false;
}

// The host's mix format as the extensible form every Windows mix format has.
void MixToExtensible(const WAVEFORMATEX* in, WAVEFORMATEXTENSIBLE* out) {
    std::memset(out, 0, sizeof *out);
    if (in->wFormatTag == WAVE_FORMAT_EXTENSIBLE && in->cbSize >= 22) {
        std::memcpy(out, in, sizeof *out);
        return;
    }
    out->Format = *in;
    out->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    out->Format.cbSize = 22;
    out->Samples.wValidBitsPerSample = in->wBitsPerSample;
    out->dwChannelMask = in->nChannels == 1 ? 0x4 : in->nChannels == 2 ? 0x3 : (1u << in->nChannels) - 1;
    out->SubFormat = in->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? kSubtypeFloat : kSubtypePcm;
}

void SilenceFrames(const Stream* s, BYTE* data, UINT32 frames) {
    std::memset(data, s->bits == 8 && !s->is_float ? 0x80 : 0, static_cast<std::size_t>(frames) * s->block);
}

void ApplyVolume(const Stream* s, BYTE* data, UINT32 frames) {
    if (s->vols_unity || !s->vols) return;
    const UINT32 ch = s->channels;
    const UINT32 bytes = s->bits / 8;
    for (UINT32 f = 0; f < frames; ++f) {
        for (UINT32 c = 0; c < ch; ++c) {
            const float v = s->vols[c];
            BYTE* p = data + (static_cast<std::size_t>(f) * ch + c) * bytes;
            if (s->is_float && bytes == 4) {
                float x;
                std::memcpy(&x, p, 4);
                x *= v;
                std::memcpy(p, &x, 4);
            } else if (bytes == 2) {
                short x;
                std::memcpy(&x, p, 2);
                x = static_cast<short>(x * v);
                std::memcpy(p, &x, 2);
            } else if (bytes == 4) {
                int x;
                std::memcpy(&x, p, 4);
                x = static_cast<int>(static_cast<double>(x) * v);
                std::memcpy(p, &x, 4);
            } else if (bytes == 3) {
                int x = (static_cast<int>(p[2]) << 24 | static_cast<int>(p[1]) << 16 | static_cast<int>(p[0]) << 8) >> 8;
                x = static_cast<int>(static_cast<double>(x) * v);
                p[0] = static_cast<BYTE>(x); p[1] = static_cast<BYTE>(x >> 8); p[2] = static_cast<BYTE>(x >> 16);
            } else if (bytes == 1) {
                p[0] = static_cast<BYTE>((static_cast<int>(p[0]) - 128) * v + 128);
            }
        }
    }
}

Stream* FindStream(UINT64 handle) {
    Stream* found = nullptr;
    AcquireSRWLockShared(&g_audio.streams_lock);
    for (unsigned i = 0; i < g_audio.stream_count; ++i)
        if (reinterpret_cast<UINT64>(g_audio.streams[i]) == handle) { found = g_audio.streams[i]; break; }
    ReleaseSRWLockShared(&g_audio.streams_lock);
    return found;
}

Endpoint* FindEndpoint(const char* device, int flow) {
    if (!device || flow < 0 || flow > 1) return nullptr;
    Endpoint& e = g_audio.ep[flow];
    return e.present && std::strcmp(e.device, device) == 0 ? &e : nullptr;
}

// Runs fn(arg) on the audio thread and waits for it.
void CallAudioThread(void (*fn)(void*), void* arg) {
    Request r{nullptr, fn, arg, CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!r.done) { ERR(audio, "cannot create a request event (%lu)", GetLastError()); return; }
    AcquireSRWLockExclusive(&g_audio.queue_lock);
    if (g_audio.tail) g_audio.tail->next = &r; else g_audio.head = &r;
    g_audio.tail = &r;
    ReleaseSRWLockExclusive(&g_audio.queue_lock);
    SetEvent(g_audio.wake);
    WaitForSingleObject(r.done, INFINITE);
    CloseHandle(r.done);
}

// ---- audio thread: host COM ---------------------------------------------------------------------------------------
void Utf8(const WCHAR* in, char* out, int cap) {
    out[0] = 0;
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, cap, nullptr, nullptr);
}

// Opens the host's default endpoint of `flow`, records what the guest will be told, and runs a throw-away stream
// through it so every DLL the host stack loads lazily is loaded now (see the file comment).
void ProbeEndpoint(int flow) {
    Endpoint& ep = g_audio.ep[flow];
    IMMDevice* dev = nullptr;
    HRESULT hr = g_audio.enumerator->GetDefaultAudioEndpoint(static_cast<EDataFlow>(flow), eConsole, &dev);
    if (FAILED(hr)) {
        TRACE(audio, "no default %s endpoint on the host: %08lx", flow ? "capture" : "render", static_cast<unsigned long>(hr));
        return;
    }
    LPWSTR id = nullptr;
    if (SUCCEEDED(dev->GetId(&id)) && id) {
        wcsncpy_s(ep.id, id, _TRUNCATE);
        g_audio.co_task_mem_free(id);
    }
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT v;
        PropVariantInit(&v);
        PROPERTYKEY key;
        key.fmtid = kPkeyFriendlyNameFmt;
        key.pid = 14;
        if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) wcsncpy_s(ep.name, v.pwszVal, _TRUNCATE);
        g_audio.prop_variant_clear(&v);
        props->Release();
    }
    if (!ep.name[0]) wcsncpy_s(ep.name, flow ? L"Default capture device" : L"Default render device", _TRUNCATE);
    Utf8(ep.id, ep.device, sizeof ep.device);
    IAudioClient* client = nullptr;
    hr = dev->Activate(kIidAudioClient, CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) {
        WAVEFORMATEX* mix = nullptr;
        hr = client->GetMixFormat(&mix);
        if (SUCCEEDED(hr) && mix) {
            MixToExtensible(mix, &ep.mix);
            client->GetDevicePeriod(&ep.def_period, &ep.min_period);
            // The throw-away cycle: Initialize in the host's own mix format, the service interfaces, one silent
            // buffer, Start/Stop (render only: starting a capture stream is not needed to load anything).
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kMinHostBufferRt, 0, mix, nullptr);
            if (SUCCEEDED(hr)) {
                UINT32 frames = 0;
                client->GetBufferSize(&frames);
                if (flow == kFlowRender) {
                    IAudioRenderClient* rc = nullptr;
                    if (SUCCEEDED(client->GetService(kIidRenderClient, reinterpret_cast<void**>(&rc))) && rc) {
                        BYTE* data = nullptr;
                        if (frames > 4 && SUCCEEDED(rc->GetBuffer(4, &data))) rc->ReleaseBuffer(4, AUDCLNT_BUFFERFLAGS_SILENT);
                        client->Start();
                        client->Stop();
                        client->Reset();
                        rc->Release();
                    }
                } else {
                    IAudioCaptureClient* cc = nullptr;
                    if (SUCCEEDED(client->GetService(kIidCaptureClient, reinterpret_cast<void**>(&cc))) && cc) cc->Release();
                }
            } else {
                WARN(audio, "host %s endpoint: priming stream failed %08lx", flow ? "capture" : "render", static_cast<unsigned long>(hr));
            }
            g_audio.co_task_mem_free(mix);
        }
        client->Release();
    }
    if (FAILED(hr) && !ep.mix.Format.nChannels) { WARN(audio, "host endpoint unusable: %08lx", static_cast<unsigned long>(hr)); dev->Release(); return; }
    dev->Activate(kIidAudioClient, CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&ep.query));
    dev->Release();
    ep.present = ep.mix.Format.nChannels != 0;
    TRACE(audio, "host %s endpoint '%ls': %u Hz, %u ch, %u bit, period %lld/%lld", flow ? "capture" : "render", ep.name,
          static_cast<unsigned>(ep.mix.Format.nSamplesPerSec), static_cast<unsigned>(ep.mix.Format.nChannels),
          static_cast<unsigned>(ep.mix.Format.wBitsPerSample), static_cast<long long>(ep.def_period),
          static_cast<long long>(ep.min_period));
}

// ---- pump: moves data between a stream's ring and its host stream (audio thread, stream lock held) ---------------
void SignalEventIfDue(Stream* s, LONGLONG now) {
    if (!(s->flags & kStreamEventCallback) || !s->event || now < s->next_event_qpc) return;
    SetEvent(s->event);
    const LONGLONG step = static_cast<LONGLONG>(MulDiv64(static_cast<UINT64>(s->period_rt), static_cast<UINT64>(g_audio.qpc_freq), 10000000));
    s->next_event_qpc += step > 0 ? step : 1;
    if (s->next_event_qpc < now) s->next_event_qpc = now + step;
}

void PumpRender(Stream* s) {
    const LONGLONG now = Qpc();
    UINT32 padding = 0;
    HRESULT hr = s->client->GetCurrentPadding(&padding);
    if (FAILED(hr)) { s->error = hr; return; }
    s->played = s->pushed > padding ? s->pushed - padding : 0;
    s->played_qpc = now;
    const UINT32 pending = static_cast<UINT32>(s->written - s->pushed);
    UINT32 n = pending;
    const UINT32 room = s->host_frames > padding ? s->host_frames - padding : 0;
    if (n > room) n = room;
    if (n) {
        BYTE* dst = nullptr;
        hr = s->render->GetBuffer(n, &dst);
        if (FAILED(hr)) { s->error = hr; return; }
        UINT32 left = n, off = s->rd_offs;
        BYTE* out = dst;
        while (left) {
            const UINT32 chunk = std::min(left, s->ring_frames - off);
            std::memcpy(out, s->ring + static_cast<std::size_t>(off) * s->block, static_cast<std::size_t>(chunk) * s->block);
            ApplyVolume(s, out, chunk);
            out += static_cast<std::size_t>(chunk) * s->block;
            off = (off + chunk) % s->ring_frames;
            left -= chunk;
        }
        s->render->ReleaseBuffer(n, 0);
        s->rd_offs = off;
        s->pushed += n;
    }
    SignalEventIfDue(s, now);
}

void PumpCapture(Stream* s) {
    const LONGLONG now = Qpc();
    bool got = false;
    for (;;) {
        UINT32 packet = 0;
        HRESULT hr = s->capture->GetNextPacketSize(&packet);
        if (FAILED(hr)) { s->error = hr; return; }
        if (!packet) break;
        BYTE* src = nullptr;
        UINT32 frames = 0;
        DWORD hflags = 0;
        hr = s->capture->GetBuffer(&src, &frames, &hflags, nullptr, nullptr);
        if (FAILED(hr)) { s->error = hr; return; }
        if (hr == AUDCLNT_S_BUFFER_EMPTY || !frames) { s->capture->ReleaseBuffer(frames); break; }
        const UINT32 room = s->ring_frames - s->held;
        const UINT32 take = std::min(frames, room);
        if (take < frames) s->discontinuity = true;   // the guest is not reading fast enough: frames are dropped
        if (hflags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) s->discontinuity = true;
        UINT32 left = take, off = s->wri_offs;
        const BYTE* in = src;
        while (left) {
            const UINT32 chunk = std::min(left, s->ring_frames - off);
            BYTE* dst = s->ring + static_cast<std::size_t>(off) * s->block;
            if (hflags & AUDCLNT_BUFFERFLAGS_SILENT) SilenceFrames(s, dst, chunk);
            else std::memcpy(dst, in, static_cast<std::size_t>(chunk) * s->block);
            in += static_cast<std::size_t>(chunk) * s->block;
            off = (off + chunk) % s->ring_frames;
            left -= chunk;
        }
        s->wri_offs = off;
        s->held += take;
        s->pushed += take;
        s->capture->ReleaseBuffer(frames);
        got = true;
    }
    if (got) SignalEventIfDue(s, now);
}

void PumpAll() {
    for (unsigned i = 0; i < g_audio.stream_count; ++i) {
        Stream* s = g_audio.streams[i];
        AcquireSRWLockExclusive(&s->lock);
        if (s->started && SUCCEEDED(s->error)) {
            if (s->flow == kFlowRender) PumpRender(s); else PumpCapture(s);
        }
        ReleaseSRWLockExclusive(&s->lock);
    }
}

bool AnyStarted() {
    for (unsigned i = 0; i < g_audio.stream_count; ++i) if (g_audio.streams[i]->started) return true;
    return false;
}

DWORD WINAPI AudioThreadMain(void*) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_audio.qpc_freq = f.QuadPart;
    g_audio.co_initialize_ex(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = g_audio.co_create_instance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER, kIidMMDeviceEnumerator,
                                            reinterpret_cast<void**>(&g_audio.enumerator));
    if (SUCCEEDED(hr) && g_audio.enumerator) {
        g_audio.stack_ok = true;
        ProbeEndpoint(kFlowRender);
        ProbeEndpoint(kFlowCapture);
    } else {
        WARN(audio, "the host's MMDeviceEnumerator cannot be created: %08lx", static_cast<unsigned long>(hr));
    }
    // A 5 ms timer when the system gives one (a high-resolution waitable timer; else a plain one at its granularity).
    g_audio.timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!g_audio.timer) g_audio.timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    SetEvent(g_audio.ready);
    bool timer_armed = false;
    for (;;) {
        HANDLE waits[2] = {g_audio.wake, g_audio.timer};
        const bool need_timer = AnyStarted();
        if (need_timer && !timer_armed && g_audio.timer) {
            LARGE_INTEGER due;
            due.QuadPart = -kPumpRt;
            SetWaitableTimer(g_audio.timer, &due, static_cast<LONG>(kPumpRt / 10000), nullptr, nullptr, FALSE);
            timer_armed = true;
        } else if (!need_timer && timer_armed) {
            CancelWaitableTimer(g_audio.timer);
            timer_armed = false;
        }
        const DWORD w = WaitForMultipleObjects(timer_armed ? 2 : 1, waits, FALSE, g_audio.timer || !need_timer ? INFINITE : 5);
        (void)w;
        for (;;) {
            AcquireSRWLockExclusive(&g_audio.queue_lock);
            Request* r = g_audio.head;
            if (r) { g_audio.head = r->next; if (!g_audio.head) g_audio.tail = nullptr; }
            ReleaseSRWLockExclusive(&g_audio.queue_lock);
            if (!r) break;
            r->fn(r->arg);
            SetEvent(r->done);
        }
        if (AnyStarted()) PumpAll();
    }
}

// ---- requests (run on the audio thread) ---------------------------------------------------------------------------
void FreeStreamMemory(Stream* s) {
    if (s->ring) VirtualFree(s->ring, 0, MEM_RELEASE);
    if (s->tmp) VirtualFree(s->tmp, 0, MEM_RELEASE);
    if (s->quit) CloseHandle(s->quit);
    delete[] s->vols;
    delete s;
}

struct CreateReq { WCreateStreamParams* p; };

void DoCreateStream(void* arg) {
    WCreateStreamParams* p = static_cast<CreateReq*>(arg)->p;
    p->result = kEFail;
    *p->stream = 0;
    Endpoint* ep = FindEndpoint(p->device, p->flow);
    if (!ep) { p->result = kEInvalidated; return; }
    if (p->share != AUDCLNT_SHAREMODE_SHARED) { p->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED; return; }
    const WAVEFORMATEX* fmt = p->fmt;
    const std::size_t fmt_size = sizeof(WAVEFORMATEX) + fmt->cbSize;
    if (fmt_size > sizeof(Stream::fmt_raw) || !fmt->nSamplesPerSec || !fmt->nChannels) { p->result = E_INVALIDARG; return; }
    if (g_audio.stream_count >= kMaxStreams) { p->result = kEOutOfMemory; return; }

    Stream* s = new (std::nothrow) Stream();
    if (!s) { p->result = kEOutOfMemory; return; }
    std::memcpy(s->fmt_raw, fmt, fmt_size);
    s->flow = p->flow;
    s->flags = p->flags;
    s->channels = fmt->nChannels;
    s->rate = fmt->nSamplesPerSec;
    s->block = fmt->nBlockAlign;
    s->bits = fmt->wBitsPerSample;
    s->is_float = IsFloatFormat(fmt);
    s->period_rt = p->period;
    s->period_frames = static_cast<UINT32>(MulDiv64(static_cast<UINT64>(p->period), s->rate, 10000000));
    if (!s->period_frames) s->period_frames = s->rate / 100;
    UINT64 ring = (MulDiv64(static_cast<UINT64>(p->duration), s->rate, 10000000) + 0);
    if (ring * 10000000 < static_cast<UINT64>(p->duration) * s->rate) ++ring;   // ceil
    if (ring < s->period_frames * 2ull) ring = s->period_frames * 2ull;
    s->ring_frames = static_cast<UINT32>(ring);
    s->vols = new (std::nothrow) float[s->channels];
    s->quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s->ring = static_cast<BYTE*>(VirtualAlloc(nullptr, static_cast<std::size_t>(s->ring_frames) * s->block, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!s->vols || !s->quit || !s->ring) { FreeStreamMemory(s); p->result = kEOutOfMemory; return; }
    for (UINT32 i = 0; i < s->channels; ++i) s->vols[i] = 1.0f;

    IMMDevice* dev = nullptr;
    HRESULT hr = g_audio.enumerator->GetDevice(ep->id, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(kIidAudioClient, CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&s->client));
    if (dev) dev->Release();
    if (SUCCEEDED(hr)) {
        REFERENCE_TIME dur = p->duration < kMinHostBufferRt ? kMinHostBufferRt : p->duration;
        hr = s->client->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamAutoConvertPcm | kStreamSrcDefaultQuality, dur, 0, fmt, nullptr);
        TRACE(audio, "host Initialize(%s, %u Hz, %u ch, %u bit%s, buffer %lld): %08lx", p->flow ? "capture" : "render",
              s->rate, s->channels, s->bits, s->is_float ? " float" : "", static_cast<long long>(dur), static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) hr = s->client->GetBufferSize(&s->host_frames);
    if (SUCCEEDED(hr)) {
        s->client->GetStreamLatency(&s->host_latency);
        if (p->flow == kFlowRender) hr = s->client->GetService(kIidRenderClient, reinterpret_cast<void**>(&s->render));
        else hr = s->client->GetService(kIidCaptureClient, reinterpret_cast<void**>(&s->capture));
    }
    if (FAILED(hr)) {
        if (s->client) s->client->Release();
        FreeStreamMemory(s);
        p->result = hr;
        return;
    }
    AcquireSRWLockExclusive(&g_audio.streams_lock);
    g_audio.streams[g_audio.stream_count++] = s;
    ReleaseSRWLockExclusive(&g_audio.streams_lock);
    *p->channel_count = s->channels;
    *p->stream = reinterpret_cast<UINT64>(s);
    p->result = S_OK;
}

struct StreamReq { Stream* s; HRESULT result; };

void DoStart(void* arg) {
    auto* r = static_cast<StreamReq*>(arg);
    Stream* s = r->s;
    AcquireSRWLockExclusive(&s->lock);
    if (FAILED(s->error)) r->result = s->error;
    else if ((s->flags & kStreamEventCallback) && !s->event) r->result = kEEventHandleNotSet;
    else if (s->started) r->result = kENotStopped;
    else {
        r->result = s->client->Start();
        if (SUCCEEDED(r->result)) {
            s->started = true;
            s->played_qpc = Qpc();
            s->next_event_qpc = s->played_qpc;
            if (s->flow == kFlowRender) PumpRender(s); else PumpCapture(s);
        }
    }
    ReleaseSRWLockExclusive(&s->lock);
}

void DoStop(void* arg) {
    auto* r = static_cast<StreamReq*>(arg);
    Stream* s = r->s;
    AcquireSRWLockExclusive(&s->lock);
    if (!s->started) r->result = S_FALSE;
    else {
        r->result = s->client->Stop();
        s->started = false;
        if (s->flow == kFlowRender) {   // freeze the position where the host stopped
            UINT32 padding = 0;
            if (SUCCEEDED(s->client->GetCurrentPadding(&padding))) s->played = s->pushed > padding ? s->pushed - padding : 0;
            s->played_qpc = Qpc();
        }
    }
    ReleaseSRWLockExclusive(&s->lock);
}

void DoReset(void* arg) {
    auto* r = static_cast<StreamReq*>(arg);
    Stream* s = r->s;
    AcquireSRWLockExclusive(&s->lock);
    if (s->started) r->result = kENotStopped;
    else if (s->getbuf_last) r->result = kEBufferOpPending;
    else {
        r->result = s->client->Reset();
        if (SUCCEEDED(r->result)) {
            if (s->flow == kFlowRender) { s->written = s->pushed = s->played = 0; s->last_pos = 0; }
            else s->written += s->held;
            s->held = 0;
            s->wri_offs = s->rd_offs = 0;
        }
    }
    ReleaseSRWLockExclusive(&s->lock);
}

void DoRelease(void* arg) {
    auto* r = static_cast<StreamReq*>(arg);
    Stream* s = r->s;
    AcquireSRWLockExclusive(&g_audio.streams_lock);
    for (unsigned i = 0; i < g_audio.stream_count; ++i)
        if (g_audio.streams[i] == s) {
            g_audio.streams[i] = g_audio.streams[--g_audio.stream_count];
            g_audio.streams[g_audio.stream_count] = nullptr;
            break;
        }
    ReleaseSRWLockExclusive(&g_audio.streams_lock);
    AcquireSRWLockExclusive(&s->lock);
    if (s->started) { s->client->Stop(); s->started = false; }
    ReleaseSRWLockExclusive(&s->lock);
    if (s->render) s->render->Release();
    if (s->capture) s->capture->Release();
    if (s->client) s->client->Release();
    s->magic = 0;
    FreeStreamMemory(s);
    r->result = S_OK;
}

struct FormatReq { Endpoint* ep; const WAVEFORMATEX* fmt; HRESULT result; };

void DoIsFormatSupported(void* arg) {
    auto* r = static_cast<FormatReq*>(arg);
    if (!r->ep->query) { r->result = kEInvalidated; return; }
    WAVEFORMATEX* closest = nullptr;
    r->result = r->ep->query->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, r->fmt, &closest);
    if (closest) g_audio.co_task_mem_free(closest);
}

// ---- the unix functions (WINAPI NTSTATUS fn(void* args)) ----------------------------------------------------------
NTSTATUS WINAPI U_not_implemented(void*) { return st::Success; }

NTSTATUS WINAPI U_main_loop(void* args) {
    SetEvent(static_cast<WMainLoopParams*>(args)->event);   // the host has no main loop of its own to run here
    return st::Success;
}

NTSTATUS WINAPI U_get_endpoint_ids(void* args) {
    auto* p = static_cast<WGetEndpointIdsParams*>(args);
    p->num = 0;
    p->default_idx = 0;
    const Endpoint* e = p->flow >= 0 && p->flow <= 1 && g_audio.ep[p->flow].present ? &g_audio.ep[p->flow] : nullptr;
    unsigned int needed = 0;
    if (e) {
        const unsigned name_bytes = static_cast<unsigned>((wcslen(e->name) + 1) * sizeof(WCHAR));
        const unsigned dev_bytes = static_cast<unsigned>(std::strlen(e->device) + 1);
        needed = sizeof(WEndpoint) + name_bytes + ((dev_bytes + 1) & ~1u);
        p->num = 1;
        if (needed <= p->size) {
            p->endpoints[0].name = sizeof(WEndpoint);
            std::memcpy(reinterpret_cast<char*>(p->endpoints) + sizeof(WEndpoint), e->name, name_bytes);
            p->endpoints[0].device = sizeof(WEndpoint) + name_bytes;
            std::memcpy(reinterpret_cast<char*>(p->endpoints) + sizeof(WEndpoint) + name_bytes, e->device, dev_bytes);
        }
    }
    if (needed > p->size) { p->size = needed; p->result = kEInsufficientBuffer; }
    else p->result = S_OK;
    return st::Success;
}

NTSTATUS WINAPI U_create_stream(void* args) {
    auto* p = static_cast<WCreateStreamParams*>(args);
    CreateReq req{p};
    CallAudioThread(&DoCreateStream, &req);
    return st::Success;
}

NTSTATUS WINAPI U_release_stream(void* args) {
    auto* p = static_cast<WReleaseStreamParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    SetEvent(s->quit);
    if (p->timer_thread) {   // Wine's timer thread for this stream ends once the quit event is set
        WaitForSingleObject(p->timer_thread, INFINITE);
        CloseHandle(p->timer_thread);
    }
    StreamReq req{s, S_OK};
    CallAudioThread(&DoRelease, &req);
    p->result = req.result;
    return st::Success;
}

template <class P, void (*Do)(void*)> NTSTATUS WINAPI U_stream_op(void* args) {
    auto* p = static_cast<P*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    StreamReq req{s, S_OK};
    CallAudioThread(Do, &req);
    p->result = req.result;
    return st::Success;
}

NTSTATUS WINAPI U_timer_loop(void* args) {
    Stream* s = FindStream(static_cast<WTimerLoopParams*>(args)->stream);
    if (s) WaitForSingleObject(s->quit, INFINITE);
    return st::Success;
}

NTSTATUS WINAPI U_is_started(void* args) {
    auto* p = static_cast<WStreamResultParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    AcquireSRWLockShared(&s->lock);
    p->result = s->started ? S_OK : S_FALSE;
    ReleaseSRWLockShared(&s->lock);
    return st::Success;
}

// A buffer for `frames` frames at ring position `pos`, contiguous: the ring itself or the stream's temporary buffer.
BYTE* EnsureTmp(Stream* s, UINT32 frames) {
    if (s->tmp_frames < frames) {
        if (s->tmp) { VirtualFree(s->tmp, 0, MEM_RELEASE); s->tmp = nullptr; s->tmp_frames = 0; }
        s->tmp = static_cast<BYTE*>(VirtualAlloc(nullptr, static_cast<std::size_t>(frames) * s->block, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!s->tmp) return nullptr;
        s->tmp_frames = frames;
    }
    return s->tmp;
}

UINT32 RenderPadding(const Stream* s) {   // frames the guest wrote that the device has not played
    const UINT64 padding = s->written > s->played ? s->written - s->played : 0;
    return static_cast<UINT32>(padding > s->ring_frames ? s->ring_frames : padding);
}

NTSTATUS WINAPI U_get_render_buffer(void* args) {
    auto* p = static_cast<WGetRenderBufferParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    AcquireSRWLockExclusive(&s->lock);
    HRESULT hr = S_OK;
    const UINT32 frames = p->frames;
    if (FAILED(s->error)) hr = s->error;
    else if (s->getbuf_last) hr = kEOutOfOrder;
    else if (frames) {
        if (RenderPadding(s) + static_cast<UINT64>(frames) > s->ring_frames) hr = kEBufferTooLarge;
        else if (s->wri_offs + static_cast<UINT64>(frames) > s->ring_frames) {
            BYTE* tmp = EnsureTmp(s, frames);
            if (!tmp) hr = kEOutOfMemory;
            else { *p->data = tmp; s->getbuf_last = -static_cast<int>(frames); }
        } else {
            *p->data = s->ring + static_cast<std::size_t>(s->wri_offs) * s->block;
            s->getbuf_last = static_cast<int>(frames);
        }
        if (SUCCEEDED(hr)) SilenceFrames(s, *p->data, frames);
    }
    ReleaseSRWLockExclusive(&s->lock);
    p->result = hr;
    return st::Success;
}

NTSTATUS WINAPI U_release_render_buffer(void* args) {
    auto* p = static_cast<WReleaseRenderBufferParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    AcquireSRWLockExclusive(&s->lock);
    HRESULT hr = S_OK;
    const UINT32 n = p->written_frames;
    if (!n) s->getbuf_last = 0;
    else if (!s->getbuf_last) hr = kEOutOfOrder;
    else if (n > static_cast<UINT32>(s->getbuf_last >= 0 ? s->getbuf_last : -s->getbuf_last)) hr = kEInvalidSize;
    else {
        BYTE* buffer = s->getbuf_last >= 0 ? s->ring + static_cast<std::size_t>(s->wri_offs) * s->block : s->tmp;
        if (p->flags & kBufferFlagSilent) SilenceFrames(s, buffer, n);
        if (s->getbuf_last < 0) {   // wrapped: copy the temporary buffer into the ring in two parts
            const UINT32 first = std::min(n, s->ring_frames - s->wri_offs);
            std::memcpy(s->ring + static_cast<std::size_t>(s->wri_offs) * s->block, buffer, static_cast<std::size_t>(first) * s->block);
            if (n > first) std::memcpy(s->ring, buffer + static_cast<std::size_t>(first) * s->block, static_cast<std::size_t>(n - first) * s->block);
        }
        s->wri_offs = (s->wri_offs + n) % s->ring_frames;
        s->written += n;
        s->getbuf_last = 0;
    }
    ReleaseSRWLockExclusive(&s->lock);
    p->result = hr;
    return st::Success;
}

NTSTATUS WINAPI U_get_capture_buffer(void* args) {
    auto* p = static_cast<WGetCaptureBufferParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    AcquireSRWLockExclusive(&s->lock);
    HRESULT hr = S_OK;
    if (FAILED(s->error)) hr = s->error;
    else if (s->getbuf_last) hr = kEOutOfOrder;
    else if (s->held < s->period_frames) { *p->frames = 0; hr = kSBufferEmpty; }
    else {
        const UINT32 frames = s->period_frames;
        *p->frames = frames;
        if (s->rd_offs + static_cast<UINT64>(frames) > s->ring_frames) {
            BYTE* tmp = EnsureTmp(s, frames);
            if (!tmp) hr = kEOutOfMemory;
            else {
                const UINT32 first = s->ring_frames - s->rd_offs;
                std::memcpy(tmp, s->ring + static_cast<std::size_t>(s->rd_offs) * s->block, static_cast<std::size_t>(first) * s->block);
                std::memcpy(tmp + static_cast<std::size_t>(first) * s->block, s->ring, static_cast<std::size_t>(frames - first) * s->block);
                *p->data = tmp;
            }
        } else *p->data = s->ring + static_cast<std::size_t>(s->rd_offs) * s->block;
        if (SUCCEEDED(hr)) {
            s->getbuf_last = static_cast<int>(frames);
            *p->flags = s->discontinuity ? kBufferFlagDiscontinuity : 0;
            s->discontinuity = false;
            if (p->devpos) *p->devpos = s->written;
            if (p->qpcpos) *p->qpcpos = QpcTo100ns(Qpc());
        }
    }
    ReleaseSRWLockExclusive(&s->lock);
    p->result = hr;
    return st::Success;
}

NTSTATUS WINAPI U_release_capture_buffer(void* args) {
    auto* p = static_cast<WReleaseCaptureBufferParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    AcquireSRWLockExclusive(&s->lock);
    HRESULT hr = S_OK;
    if (!p->done) s->getbuf_last = 0;
    else if (!s->getbuf_last) hr = kEOutOfOrder;
    else if (static_cast<UINT32>(s->getbuf_last) != p->done) hr = kEInvalidSize;
    else {
        s->written += p->done;
        s->held -= p->done;
        s->rd_offs = (s->rd_offs + p->done) % s->ring_frames;
        s->getbuf_last = 0;
    }
    ReleaseSRWLockExclusive(&s->lock);
    p->result = hr;
    return st::Success;
}

NTSTATUS WINAPI U_is_format_supported(void* args) {
    auto* p = static_cast<WIsFormatSupportedParams*>(args);
    Endpoint* ep = FindEndpoint(p->device, p->flow);
    if (!ep) { p->result = kEInvalidated; return st::Success; }
    if (p->share != AUDCLNT_SHAREMODE_SHARED) { p->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED; return st::Success; }
    FormatReq req{ep, p->fmt_in, kEFail};
    CallAudioThread(&DoIsFormatSupported, &req);
    p->result = req.result;
    return st::Success;
}

NTSTATUS WINAPI U_get_loopback_capture_device(void* args) {
    static_cast<WGetLoopbackParams*>(args)->result = kENotImpl;
    return st::Success;
}

NTSTATUS WINAPI U_get_mix_format(void* args) {
    auto* p = static_cast<WGetMixFormatParams*>(args);
    Endpoint* ep = FindEndpoint(p->device, p->flow);
    if (!ep) { p->result = kEInvalidated; return st::Success; }
    *p->fmt = ep->mix;
    p->result = S_OK;
    return st::Success;
}

NTSTATUS WINAPI U_get_device_period(void* args) {
    auto* p = static_cast<WGetDevicePeriodParams*>(args);
    Endpoint* ep = FindEndpoint(p->device, p->flow);
    if (!ep) { p->result = kEInvalidated; return st::Success; }
    if (p->def_period) *p->def_period = ep->def_period;
    if (p->min_period) *p->min_period = ep->min_period;
    p->result = S_OK;
    return st::Success;
}

NTSTATUS WINAPI U_get_buffer_size(void* args) {
    auto* p = static_cast<WGetBufferSizeParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    *p->frames = s->ring_frames;
    p->result = S_OK;
    return st::Success;
}

NTSTATUS WINAPI U_get_latency(void* args) {
    auto* p = static_cast<WGetLatencyParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    *p->latency = s->host_latency + (s->flow == kFlowRender ? kPumpRt : s->period_rt);
    p->result = S_OK;
    return st::Success;
}

NTSTATUS WINAPI U_get_current_padding(void* args) {
    auto* p = static_cast<WGetCurrentPaddingParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    AcquireSRWLockShared(&s->lock);
    if (FAILED(s->error)) p->result = s->error;
    else {
        *p->padding = s->flow == kFlowRender ? RenderPadding(s) : s->held;
        p->result = S_OK;
    }
    ReleaseSRWLockShared(&s->lock);
    return st::Success;
}

NTSTATUS WINAPI U_get_next_packet_size(void* args) {
    auto* p = static_cast<WGetNextPacketSizeParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    AcquireSRWLockShared(&s->lock);
    *p->frames = s->held < s->period_frames ? 0 : s->period_frames;
    p->result = FAILED(s->error) ? s->error : S_OK;
    ReleaseSRWLockShared(&s->lock);
    return st::Success;
}

NTSTATUS WINAPI U_get_frequency(void* args) {
    auto* p = static_cast<WGetFrequencyParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    *p->freq = static_cast<UINT64>(s->rate) * s->block;   // shared mode: positions are in bytes
    p->result = S_OK;
    return st::Success;
}

NTSTATUS WINAPI U_get_position(void* args) {
    auto* p = static_cast<WGetPositionParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    AcquireSRWLockExclusive(&s->lock);
    UINT64 pos;
    const LONGLONG now = Qpc();
    if (s->flow == kFlowRender) {
        pos = s->played;
        if (!p->device && s->started && s->written > s->played) {
            // between pump steps: the system timer advances the host's last sample, never past what was pushed
            const UINT64 elapsed = MulDiv64(static_cast<UINT64>(now - s->played_qpc), s->rate, static_cast<UINT64>(g_audio.qpc_freq));
            pos = std::min(pos + elapsed, s->pushed);
        }
    } else {
        pos = s->written + s->held;
    }
    if (pos < s->last_pos) pos = s->last_pos; else s->last_pos = pos;
    *p->pos = pos * s->block;
    if (p->qpctime) *p->qpctime = QpcTo100ns(now);
    p->result = FAILED(s->error) ? s->error : S_OK;
    ReleaseSRWLockExclusive(&s->lock);
    return st::Success;
}

NTSTATUS WINAPI U_set_volumes(void* args) {
    auto* p = static_cast<WSetVolumesParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) return st::Success;
    AcquireSRWLockExclusive(&s->lock);
    bool unity = true;
    for (UINT32 i = 0; i < s->channels; ++i) {
        s->vols[i] = p->volumes[i] * p->session_volumes[i] * p->master_volume;
        if (s->vols[i] != 1.0f) unity = false;
    }
    s->vols_unity = unity;
    ReleaseSRWLockExclusive(&s->lock);
    return st::Success;
}

NTSTATUS WINAPI U_set_event_handle(void* args) {
    auto* p = static_cast<WSetEventHandleParams*>(args);
    Stream* s = FindStream(p->stream);
    if (!s) { p->result = kEInvalidated; return st::Success; }
    AcquireSRWLockExclusive(&s->lock);
    if (!(s->flags & kStreamEventCallback)) p->result = kEEventHandleNotExpected;
    else if (s->event) p->result = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);   // Wine's drivers: "called twice"
    else { s->event = p->event; p->result = S_OK; }
    ReleaseSRWLockExclusive(&s->lock);
    return st::Success;
}

NTSTATUS WINAPI U_test_connect(void* args) {
    // The driver works when the host's enumerator does; whether it has devices is what the endpoint lists say.
    static_cast<WTestConnectParams*>(args)->priority = g_audio.stack_ok ? kPriorityNeutral : kPriorityUnavailable;
    return st::Success;
}

NTSTATUS WINAPI U_get_prop_value(void* args) {
    auto* p = static_cast<WGetPropValueParams*>(args);
    Endpoint* ep = FindEndpoint(p->device, p->flow);
    if (!ep) { p->result = kEInvalidated; return st::Success; }
    if (IsEqualGUID(p->prop->fmtid, kPkeyDevicePathFmt) && p->prop->pid == 2) {
        // Windows derives a stable serial from the endpoint's GUID; so does Wine's driver.
        const BYTE* d4 = p->guid->Data4;
        const unsigned serial = static_cast<unsigned>(d4[4] << 24 | d4[5] << 16 | d4[6] << 8 | d4[7]);
        char buf[64];
        std::snprintf(buf, sizeof buf, "{1}.ROOT\\MEDIA\\%04u", serial & 0x1FF);
        const unsigned len = static_cast<unsigned>(std::strlen(buf)) + 1;
        if (*p->buffer_size < len * sizeof(WCHAR)) { *p->buffer_size = len * sizeof(WCHAR); p->result = kENotSufficientBuffer; return st::Success; }
        WCHAR* w = static_cast<WCHAR*>(p->buffer);
        for (unsigned i = 0; i < len; ++i) w[i] = static_cast<unsigned char>(buf[i]);
        p->value->vt = VT_LPWSTR;
        p->value->pwszVal = w;
        p->result = S_OK;
        return st::Success;
    }
    if (p->flow == kFlowRender && IsEqualGUID(p->prop->fmtid, kPkeyPhysicalSpeakersFmt) && p->prop->pid == 3) {
        p->value->vt = VT_UI4;
        p->value->ulVal = ep->mix.dwChannelMask;   // the host's own speaker layout
        p->result = S_OK;
        return st::Success;
    }
    p->result = kENotImpl;
    return st::Success;
}

NTSTATUS WINAPI U_midi_get_driver(void*) { return st::Success; }   // the name stays empty: this driver is no MIDI driver

NTSTATUS WINAPI U_midi_init(void* args) {
    *static_cast<WMidiInitParams*>(args)->err = 0;   // DRV_FAILURE: no MIDI devices, so winmm starts no notify thread
    return st::Success;
}

NTSTATUS WINAPI U_midi_notify_wait(void* args) {
    auto* p = static_cast<WMidiNotifyWaitParams*>(args);
    *p->quit = TRUE;
    p->notify->send_notify = FALSE;
    return st::Success;
}

const UnixCallFn kFuncs[kFuncsCount] = {
    &U_not_implemented,                 // process_attach
    &U_not_implemented,                 // process_detach
    &U_main_loop,
    &U_get_endpoint_ids,
    &U_create_stream,
    &U_release_stream,
    &U_stream_op<WStreamResultParams, &DoStart>,
    &U_stream_op<WStreamResultParams, &DoStop>,
    &U_stream_op<WStreamResultParams, &DoReset>,
    &U_timer_loop,
    &U_get_render_buffer,
    &U_release_render_buffer,
    &U_get_capture_buffer,
    &U_release_capture_buffer,
    &U_is_format_supported,
    &U_get_loopback_capture_device,
    &U_get_mix_format,
    &U_get_device_period,
    &U_get_buffer_size,
    &U_get_latency,
    &U_get_current_padding,
    &U_get_next_packet_size,
    &U_get_frequency,
    &U_get_position,
    &U_set_volumes,
    &U_set_event_handle,
    &U_not_implemented,                 // set_sample_rate
    &U_test_connect,
    &U_is_started,
    &U_get_prop_value,
    &U_midi_get_driver,
    &U_midi_init,
    &U_not_implemented,                 // midi_release
    &U_not_implemented,                 // midi_out_message
    &U_not_implemented,                 // midi_in_message
    &U_midi_notify_wait,
    &U_not_implemented,                 // aux_message
};

}  // namespace

void PrimeAudio(const std::wstring& machine_root) {
    if (g_audio.enabled) return;
    wchar_t off[8];
    const DWORD n = GetEnvironmentVariableW(L"WIN10XBOX_AUDIO", off, 8);
    if (n && n < 8 && off[0] == L'0') { TRACE(audio, "WIN10XBOX_AUDIO=0: no audio driver"); return; }
    if (GetFileAttributesW((machine_root + L"\\Windows\\builtin\\mmdevapi.dll").c_str()) == INVALID_FILE_ATTRIBUTES) return;   // no mmdevapi to serve
    HMODULE ole32 = LoadLibraryW(L"ole32.dll");
    if (!ole32) { WARN(audio, "host ole32.dll: LoadLibrary failed, error %lu", GetLastError()); return; }
    g_audio.co_initialize_ex = reinterpret_cast<decltype(g_audio.co_initialize_ex)>(GetProcAddress(ole32, "CoInitializeEx"));
    g_audio.co_create_instance = reinterpret_cast<decltype(g_audio.co_create_instance)>(GetProcAddress(ole32, "CoCreateInstance"));
    g_audio.co_task_mem_free = reinterpret_cast<decltype(g_audio.co_task_mem_free)>(GetProcAddress(ole32, "CoTaskMemFree"));
    g_audio.prop_variant_clear = reinterpret_cast<decltype(g_audio.prop_variant_clear)>(GetProcAddress(ole32, "PropVariantClear"));
    if (!g_audio.co_initialize_ex || !g_audio.co_create_instance || !g_audio.co_task_mem_free || !g_audio.prop_variant_clear) return;
    g_audio.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_audio.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_audio.wake || !g_audio.ready) return;
    g_audio.thread = CreateThread(nullptr, 0, &AudioThreadMain, nullptr, 0, nullptr);
    if (!g_audio.thread) { ERR(audio, "cannot start the audio thread (%lu)", GetLastError()); return; }
    // The host stack, fully exercised, before the guest takes the PEB. A host stack that does not answer in time (an
    // audio service that hangs) leaves the guest without a driver instead of without a start.
    if (WaitForSingleObject(g_audio.ready, 20000) != WAIT_OBJECT_0) {
        WARN(audio, "the host audio stack did not answer within 20 s: no audio driver for the guest");
        return;
    }
    g_audio.enabled = true;
    TRACE(audio, "audio thread up: host stack %s, render endpoint %s, capture endpoint %s", g_audio.stack_ok ? "ok" : "unusable",
          g_audio.ep[kFlowRender].present ? "yes" : "no", g_audio.ep[kFlowCapture].present ? "yes" : "no");
}

void RegisterMmdevapiUnixLib() {
    if (g_audio.enabled) RegisterUnixLib(L"winewasapi", kFuncs, kFuncsCount);
}

}  // namespace win10xbox::dispatch::detail
