// probe_N_winrt.cpp - category N: WinRT activation sweep.
//
// For each Windows.* runtime class in the sweep list, asks combase for the class's
// activation factory (RoGetActivationFactory, queried for IUnknown) and classifies
// the HRESULT. Factory-only and read-only: no instance is created, no method of any
// class is invoked, and every returned pointer is released.
//
// combase.dll is loaded at runtime and the Ro*/Windows*String entry points are
// resolved with GetProcAddress, so no runtimeobject.lib or other import library is
// needed. The C++/WinRT projection headers are deliberately not used; HSTRING comes
// from <hstring.h> and IInspectable** is declared as IUnknown** (same ABI).
// Haiku
#include "probe.h"

#include <unknwn.h>
#include <hstring.h>

#include <cstdio>
#include <string>

namespace probe {
namespace {

// Raw ABI signatures of the combase entry points used by this module.
using RoInitialize_fn           = HRESULT(__stdcall*)(int);
using RoUninitialize_fn         = void(__stdcall*)();
using RoActivateInstance_fn     = HRESULT(__stdcall*)(HSTRING, IUnknown**);
using RoGetActivationFactory_fn = HRESULT(__stdcall*)(HSTRING, const GUID&, void**);
using WindowsCreateString_fn    = HRESULT(__stdcall*)(PCWSTR, UINT32, HSTRING*);
using WindowsDeleteString_fn    = HRESULT(__stdcall*)(HSTRING);

// IID_IUnknown defined locally so that no uuid.lib reference is needed.
const GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000,
                            {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct RoApi {
    RoInitialize_fn           RoInitialize           = nullptr;
    RoUninitialize_fn         RoUninitialize         = nullptr;
    RoActivateInstance_fn     RoActivateInstance     = nullptr;  // resolved, not called
    RoGetActivationFactory_fn RoGetActivationFactory = nullptr;
    WindowsCreateString_fn    WindowsCreateString    = nullptr;
    WindowsDeleteString_fn    WindowsDeleteString    = nullptr;
};

// The sweep list: core, gaming, graphics, UI, storage and networking classes.
const char* const kClasses[] = {
    "Windows.Foundation.Uri",
    "Windows.Foundation.Collections.PropertySet",
    "Windows.System.MemoryManager",
    "Windows.System.Threading.ThreadPool",
    "Windows.System.Launcher",
    "Windows.System.Profile.AnalyticsInfo",
    "Windows.ApplicationModel.Core.CoreApplication",
    "Windows.ApplicationModel.Package",
    "Windows.Storage.ApplicationData",
    "Windows.Storage.StorageFolder",
    "Windows.Storage.Streams.InMemoryRandomAccessStream",
    "Windows.Security.Cryptography.CryptographicBuffer",
    "Windows.Security.Credentials.PasswordVault",
    "Windows.Networking.Sockets.StreamSocket",
    "Windows.Networking.Connectivity.NetworkInformation",
    "Windows.Web.Http.HttpClient",
    "Windows.Devices.Enumeration.DeviceInformation",
    "Windows.Devices.Input.MouseDevice",
    "Windows.Gaming.Input.Gamepad",
    "Windows.Gaming.Input.RawGameController",
    "Windows.Gaming.UI.GameBar",
    "Windows.Gaming.XboxLive.Storage.GameSaveProvider",
    "Windows.Graphics.Display.DisplayInformation",
    "Windows.Graphics.Capture.GraphicsCaptureSession",
    "Windows.UI.Core.CoreWindow",
    "Windows.UI.ViewManagement.ApplicationView",
    "Windows.UI.Composition.Compositor",
    "Windows.UI.Popups.MessageDialog",
    "Windows.Media.Playback.MediaPlayer",
    "Windows.Media.Audio.AudioGraph",
    "Windows.Devices.Power.Battery",
    "Windows.System.Diagnostics.ProcessDiagnosticInfo",
    "Windows.Management.Deployment.PackageManager",
};

// Resolves one export from combase; appends its name to 'missing' on failure.
template <class Fn>
bool resolve_fn(HMODULE mod, const char* sym, Fn& out, std::string& missing) {
    FARPROC p = GetProcAddress(mod, sym);
    if (!p) {
        missing += sym;
        missing += ' ';
        return false;
    }
    out = reinterpret_cast<Fn>(p);
    return true;
}

// Loads combase.dll from the system directory and resolves every entry point this
// module needs. Returns false, with the missing names in 'missing', on any failure.
bool load_ro_api(RoApi& api, std::string& missing) {
    HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!combase) {
        missing = "combase.dll";
        return false;
    }
    bool ok = true;
    ok &= resolve_fn(combase, "RoInitialize", api.RoInitialize, missing);
    ok &= resolve_fn(combase, "RoUninitialize", api.RoUninitialize, missing);
    ok &= resolve_fn(combase, "RoActivateInstance", api.RoActivateInstance, missing);
    ok &= resolve_fn(combase, "RoGetActivationFactory", api.RoGetActivationFactory, missing);
    ok &= resolve_fn(combase, "WindowsCreateString", api.WindowsCreateString, missing);
    ok &= resolve_fn(combase, "WindowsDeleteString", api.WindowsDeleteString, missing);
    return ok;
}

// "hr=0x%08X" formatting for values.
std::string hr_text(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "hr=0x%08X", static_cast<unsigned>(hr));
    return buf;
}

// Probes one class: factory query, classification, and release of everything.
void probe_class(const RoApi& api, const char* cls, Record& rec) {
    const std::wstring wide = widen(cls);
    HSTRING hs = nullptr;
    HRESULT hr = api.WindowsCreateString(wide.c_str(), static_cast<UINT32>(wide.size()), &hs);
    if (FAILED(hr)) {
        rec.status    = Status::INFO;
        rec.errorcode = static_cast<uint32_t>(hr);
        rec.value     = hr_text(hr);
        rec.detail    = "WindowsCreateString failed: " + error_text(static_cast<uint32_t>(hr));
        return;
    }

    IUnknown* factory = nullptr;
    hr = api.RoGetActivationFactory(hs, kIID_IUnknown, reinterpret_cast<void**>(&factory));
    api.WindowsDeleteString(hs);

    if (hr == S_OK) {
        rec.status    = Status::OK;
        rec.errorcode = 0;
        rec.value     = "factory=1";
        rec.detail    = "activation factory obtained (factory-only; no instance created)";
    } else {
        rec.errorcode = static_cast<uint32_t>(hr);
        switch (static_cast<uint32_t>(hr)) {
            case 0x80040154u:  // REGDB_E_CLASSNOTREG
                rec.status = Status::MISSING;
                rec.value  = "classnotreg";
                rec.detail = "class not registered (REGDB_E_CLASSNOTREG)";
                break;
            case 0x80073D54u:  // RO_E_METADATA-class failure
            case 0x80070002u:  // ERROR_FILE_NOT_FOUND
                rec.status = Status::MISSING;
                rec.value  = hr_text(hr);
                rec.detail = "type metadata or activation DLL not found";
                break;
            case 0x80070005u:  // E_ACCESSDENIED
                rec.status = Status::DENIED;
                rec.value  = hr_text(hr);
                rec.detail = "access denied by the sandbox (E_ACCESSDENIED)";
                break;
            case 0x80004001u:  // E_NOTIMPL
                rec.status = Status::NOTIMPL;
                rec.value  = hr_text(hr);
                rec.detail = "factory present but not implemented (E_NOTIMPL)";
                break;
            default:
                rec.status = Status::INFO;
                rec.value  = hr_text(hr);
                rec.detail = "RoGetActivationFactory: " + error_text(static_cast<uint32_t>(hr));
                break;
        }
    }

    if (factory) factory->Release();
}

}  // namespace

void register_N_winrt(Reporter& r) {
    RoApi api;
    std::string missing;
    if (!load_ro_api(api, missing)) {
        r.run("N", "combase:entrypoints", [&](Record& rec) {
            rec.status = Status::MISSING;
            rec.value  = "missing";
            rec.detail = "combase.dll or WinRT entry points not found: " + missing;
        });
        return;
    }

    // Initialize the WinRT apartment once, outside the per-probe bodies. Only a
    // successful call (S_OK or S_FALSE) is paired with RoUninitialize below.
    const HRESULT init_hr = api.RoInitialize(1 /* RO_INIT_MULTITHREADED */);
    const bool uninit_needed = (init_hr == S_OK || init_hr == S_FALSE);
    r.run("N", "ro_initialize", [&](Record& rec) {
        rec.status    = Status::INFO;
        rec.errorcode = static_cast<uint32_t>(init_hr);
        rec.value     = hr_text(init_hr);
        rec.detail    = "RoInitialize(RO_INIT_MULTITHREADED)";
    });

    for (const char* cls : kClasses) {
        r.run("N", std::string("activate:") + cls, [&api, cls](Record& rec) {
            probe_class(api, cls, rec);
        });
    }

    if (uninit_needed) api.RoUninitialize();
}

}  // namespace probe
