// probe_J_audio.cpp - category "J": audio capability probe (WASAPI + XAudio2).
//
// Non-destructive: no stream is Initialize()d or Start()ed, nothing is played.
// WASAPI is reached through CoCreateInstance (ole32, linked by both hosts).
// XAudio2 is resolved with LoadLibraryExW + GetProcAddress so no xaudio2 import
// lib is needed (the desktop and AppContainer hosts cannot mix import libs).
// The only optional "active" step is xaudio2-mastering, which opens a mastering
// voice (silent, no buffers submitted) and destroys it immediately.
#include "probe.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <xaudio2.h>

#include <cstdio>

namespace probe {
namespace {

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

std::string num(unsigned long long v) { return std::to_string(v); }

template <class T>
void rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// S_OK=>OK; E_ACCESSDENIED=>DENIED; class-not-registered / module missing=>MISSING;
// anything else=>INFO (caller stores the raw HRESULT in errorcode).
Status classify_hr(HRESULT hr) {
    if (SUCCEEDED(hr)) return Status::OK;
    if (hr == E_ACCESSDENIED) return Status::DENIED;  // == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)
    if (hr == REGDB_E_CLASSNOTREG || hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND))
        return Status::MISSING;
    if (hr == E_NOTIMPL) return Status::NOTIMPL;
    return Status::INFO;
}

void fail_hr(Record& rec, HRESULT hr, const std::string& what) {
    rec.status = classify_hr(hr);
    rec.errorcode = static_cast<uint32_t>(hr);
    rec.detail = what + " failed: hr=" + hex32(static_cast<uint32_t>(hr)) + " (" +
                 error_text(static_cast<uint32_t>(hr)) + ")";
}

// HRESULT_FROM_WIN32(ERROR_NOT_FOUND) - what GetDefaultAudioEndpoint returns when
// there is no endpoint of that role.
const HRESULT kNoEndpoint = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

// COM apartment bookkeeping for the whole module. Set in register_J_audio; the
// probe lambdas run synchronously inside Reporter::run so plain statics are safe.
HRESULT g_co_hr = S_OK;

HRESULT make_enumerator(IMMDeviceEnumerator** out) {
    return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(out));
}

// ---- WASAPI -----------------------------------------------------------------

void probe_wasapi_enumerator(Record& rec) {
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = make_enumerator(&en);
    if (FAILED(hr) || !en) {
        rel(en);
        fail_hr(rec, hr, "CoCreateInstance(MMDeviceEnumerator)");
        rec.detail += "; CoInitializeEx hr=" + hex32(static_cast<uint32_t>(g_co_hr));
        return;
    }
    rec.status = Status::OK;
    rec.value = "created=1";
    rec.detail = "MMDeviceEnumerator created; CoInitializeEx hr=" + hex32(static_cast<uint32_t>(g_co_hr));

    // Extra context only (never changes the status): active render endpoint count.
    IMMDeviceCollection* col = nullptr;
    HRESULT chr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (SUCCEEDED(chr) && col) {
        UINT n = 0;
        if (SUCCEEDED(col->GetCount(&n))) rec.value += ";render_endpoints=" + num(n);
        col->Release();
    } else {
        rec.value += ";render_endpoints=err:" + hex32(static_cast<uint32_t>(chr));
    }
    en->Release();
}

void probe_wasapi_default_device(Record& rec) {
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = make_enumerator(&en);
    if (FAILED(hr) || !en) {
        rel(en);
        fail_hr(rec, hr, "CoCreateInstance(MMDeviceEnumerator)");
        return;
    }
    IMMDevice* dev = nullptr;
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (SUCCEEDED(hr) && dev) {
        dev->Release();
        rec.status = Status::OK;
        rec.value = "have_device=1";
        rec.detail = "default render endpoint (eRender/eConsole) present";
        return;
    }
    rel(dev);
    rec.value = "have_device=0";
    if (hr == kNoEndpoint) {
        // Not a failure: e.g. a console / VM with nothing attached.
        rec.status = Status::WARN;
        rec.errorcode = static_cast<uint32_t>(hr);
        rec.detail = "no default render endpoint (ERROR_NOT_FOUND)";
        return;
    }
    fail_hr(rec, hr, "GetDefaultAudioEndpoint(eRender,eConsole)");
}

void probe_wasapi_activate_client(Record& rec) {
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = make_enumerator(&en);
    if (FAILED(hr) || !en) {
        rel(en);
        fail_hr(rec, hr, "CoCreateInstance(MMDeviceEnumerator)");
        return;
    }
    IMMDevice* dev = nullptr;
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (FAILED(hr) || !dev) {
        rel(dev);
        if (hr == kNoEndpoint) {
            rec.status = Status::SKIPPED;
            rec.errorcode = static_cast<uint32_t>(hr);
            rec.detail = "no default render endpoint to activate";
        } else {
            fail_hr(rec, hr, "GetDefaultAudioEndpoint(eRender,eConsole)");
        }
        return;
    }

    IAudioClient* ac = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac));
    dev->Release();
    if (FAILED(hr) || !ac) {
        rel(ac);
        fail_hr(rec, hr, "IMMDevice::Activate(IAudioClient)");
        return;
    }

    // Query only. No Initialize()/Start(): nothing is opened for playback.
    WAVEFORMATEX* wfx = nullptr;
    hr = ac->GetMixFormat(&wfx);
    if (FAILED(hr) || !wfx) {
        ac->Release();
        fail_hr(rec, hr, "IAudioClient::GetMixFormat");
        return;
    }
    rec.status = Status::OK;
    rec.value = "rate=" + num(wfx->nSamplesPerSec) + ";channels=" + num(wfx->nChannels) +
                ";bits=" + num(wfx->wBitsPerSample);
    rec.detail = "IAudioClient activated; mix format tag=0x" + [&] {
        char b[16]; std::snprintf(b, sizeof b, "%X", wfx->wFormatTag); return std::string(b); }() +
        " (not initialized, no stream started)";
    CoTaskMemFree(wfx);
    ac->Release();
}

// ---- XAudio2 ----------------------------------------------------------------

typedef HRESULT(WINAPI* PFN_XAudio2Create_t)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

struct XA2Result {
    IXAudio2*   engine = nullptr;
    const char* dll = "none";   // which DLL satisfied the create
    HRESULT     hr = E_FAIL;    // HRESULT of the last attempted create
    DWORD       load_err = 0;   // LoadLibrary error when nothing loaded
    bool        any_loaded = false;
    bool        any_export = false;
};

// Try xaudio2_9, xaudio2_9redist, xaudio2_8 in order. Stops at the first DLL that
// loads, exports XAudio2Create and creates an engine. Modules stay loaded (the
// engine's worker threads can outlive Release by a moment; unloading is not worth
// the risk for a one-shot probe).
XA2Result create_xaudio2() {
    static const char* const names[] = {"xaudio2_9.dll", "xaudio2_9redist.dll", "xaudio2_8.dll"};
    XA2Result res;
    for (const char* n : names) {
        const std::wstring wn = widen(n);
        HMODULE h = LoadLibraryExW(wn.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        DWORD e = h ? 0 : GetLastError();
        if (!h && e == ERROR_INVALID_PARAMETER) {  // flag unsupported: default search
            h = LoadLibraryExW(wn.c_str(), nullptr, 0);
            e = h ? 0 : GetLastError();
        }
        if (!h) { if (!res.any_loaded) res.load_err = e; continue; }
        res.any_loaded = true;
        auto pfn = reinterpret_cast<PFN_XAudio2Create_t>(GetProcAddress(h, "XAudio2Create"));
        if (!pfn) continue;
        res.any_export = true;
        IXAudio2* x = nullptr;
        HRESULT hr = pfn(&x, 0, XAUDIO2_DEFAULT_PROCESSOR);
        res.hr = hr;
        res.dll = n;
        if (SUCCEEDED(hr) && x) { res.engine = x; return res; }
        rel(x);
    }
    return res;
}

void xaudio2_fail(Record& rec, const XA2Result& x) {
    rec.value = std::string("dll=") + (x.any_loaded ? x.dll : "none") + ";created=0";
    if (!x.any_loaded) {
        rec.status = (x.load_err == ERROR_ACCESS_DENIED) ? Status::DENIED
                   : (x.load_err == ERROR_MOD_NOT_FOUND || x.load_err == ERROR_FILE_NOT_FOUND ||
                      x.load_err == ERROR_PATH_NOT_FOUND) ? Status::MISSING : Status::INFO;
        rec.errorcode = x.load_err;
        rec.detail = "no XAudio2 DLL could be loaded (xaudio2_9 / xaudio2_9redist / xaudio2_8): " +
                     error_text(x.load_err);
    } else if (!x.any_export) {
        rec.status = Status::MISSING;
        rec.detail = "XAudio2 DLL loaded but XAudio2Create not exported";
    } else {
        fail_hr(rec, x.hr, std::string("XAudio2Create (") + x.dll + ")");
    }
}

void probe_xaudio2_create(Record& rec) {
    XA2Result x = create_xaudio2();
    if (!x.engine) { xaudio2_fail(rec, x); return; }
    rec.status = Status::OK;
    rec.value = std::string("dll=") + x.dll + ";created=1";
    rec.detail = std::string("XAudio2Create ok via ") + x.dll + " (engine only; no mastering voice created)";
    x.engine->Release();
}

// Opens the default endpoint through a mastering voice and tears it down at once.
// Silent: no source voice, no buffers submitted. Skipped when XAudio2 is absent.
void probe_xaudio2_mastering(Record& rec) {
    XA2Result x = create_xaudio2();
    if (!x.engine) {
        xaudio2_fail(rec, x);
        rec.status = Status::SKIPPED;
        rec.detail = "XAudio2 engine unavailable, mastering voice not attempted";
        return;
    }
    IXAudio2MasteringVoice* mv = nullptr;
    HRESULT hr = x.engine->CreateMasteringVoice(&mv);
    if (SUCCEEDED(hr) && mv) {
        XAUDIO2_VOICE_DETAILS vd = {};
        mv->GetVoiceDetails(&vd);
        rec.status = Status::OK;
        rec.value = std::string("dll=") + x.dll + ";created=1;channels=" + num(vd.InputChannels) +
                    ";rate=" + num(vd.InputSampleRate);
        rec.detail = "mastering voice created and destroyed immediately (silent, no buffers submitted)";
        mv->DestroyVoice();
    } else {
        rec.value = std::string("dll=") + x.dll + ";created=0";
        fail_hr(rec, hr, "IXAudio2::CreateMasteringVoice");
    }
    x.engine->Release();
}

}  // namespace

void register_J_audio(Reporter& r) {
    // Balance CoUninitialize once at the end, not per probe. If the thread is
    // already in a different apartment mode (RPC_E_CHANGED_MODE) we simply run in
    // that apartment and must NOT call CoUninitialize.
    g_co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool need_uninit = SUCCEEDED(g_co_hr);  // S_OK or S_FALSE both need a matching call

    r.run("J", "wasapi-enumerator", probe_wasapi_enumerator);
    r.run("J", "wasapi-default-device", probe_wasapi_default_device);
    r.run("J", "wasapi-activate-client", probe_wasapi_activate_client);
    r.run("J", "xaudio2-create", probe_xaudio2_create);
    r.run("J", "xaudio2-mastering", probe_xaudio2_mastering);

    if (need_uninit) CoUninitialize();
}

}  // namespace probe
