// probe_I_graphics.cpp - category "I": graphics capability probe.
//
// Read-only / non-destructive: creates (and immediately releases) DXGI / D3D11 /
// D3D12 objects, and reports which "on-12" translation bridges (d3d11on12,
// d3d9on12, GLOn12, Dozen/Vulkan) the eventual compat layer could ride on.
// No window, no swapchain, no present.
//
// LINKING: this static lib is linked into both a desktop host and an AppContainer
// host that cannot mix import libs, so NOTHING here uses d3d/dxgi import libs.
// Every device-creation entry point is resolved with LoadLibraryExW +
// GetProcAddress. The D3D headers are included for types/enums only, and
// __uuidof() supplies the IIDs (no dxguid.lib). Only kernel32 APIs that are legal
// in an AppContainer are used (LoadLibraryExW, GetProcAddress, FreeLibrary).
#include "probe.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <initializer_list>

namespace probe {
namespace {

// D3D_FEATURE_LEVEL_12_2 only exists in newer SDKs; use the raw value so this
// builds against any SDK.
constexpr D3D_FEATURE_LEVEL kFL_12_2 = static_cast<D3D_FEATURE_LEVEL>(0xc200);

// ---- small helpers ----------------------------------------------------------

template <class T>
void rel(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

std::string hexmin(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%x", v);
    return b;
}

std::string hex4(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%04X", v);
    return b;
}

std::string num(unsigned long long v) { return std::to_string(v); }

// "12_1" style name for a D3D_FEATURE_LEVEL value.
const char* fl_name(uint32_t fl) {
    switch (fl) {
        case 0x9100: return "9_1";
        case 0x9200: return "9_2";
        case 0x9300: return "9_3";
        case 0xa000: return "10_0";
        case 0xa100: return "10_1";
        case 0xb000: return "11_0";
        case 0xb100: return "11_1";
        case 0xc000: return "12_0";
        case 0xc100: return "12_1";
        case 0xc200: return "12_2";
        default:     return "?";
    }
}

// Map a failed HRESULT onto a probe Status.
Status classify_hr(HRESULT hr) {
    if (SUCCEEDED(hr)) return Status::OK;
    // E_ACCESSDENIED == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) == 0x80070005
    if (hr == E_ACCESSDENIED) return Status::DENIED;
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

// LoadLibraryExW from System32 only (no CWD / app-dir planting). Returns nullptr
// and fills *err on failure. Falls back to a default-search load if the flag is
// unsupported on this OS. Libraries are NOT freed by the device probes (graphics
// drivers may leave worker threads behind); bridge probes free their own refs.
HMODULE load_sys(const wchar_t* dll, DWORD* err) {
    HMODULE h = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    DWORD e = h ? 0 : GetLastError();
    if (!h && e == ERROR_INVALID_PARAMETER) {
        h = LoadLibraryExW(dll, nullptr, 0);
        e = h ? 0 : GetLastError();
    }
    if (err) *err = e;
    return h;
}

// Map a LoadLibrary failure onto Status (missing vs denied vs other).
Status classify_load_err(DWORD e) {
    switch (e) {
        case ERROR_ACCESS_DENIED:    return Status::DENIED;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_MOD_NOT_FOUND:    return Status::MISSING;
        default:                     return Status::INFO;
    }
}

// Mark `rec` as "module could not be loaded".
void fail_load(Record& rec, const char* dll, DWORD e) {
    rec.status = classify_load_err(e);
    rec.errorcode = e;
    rec.detail = std::string("LoadLibraryExW(") + dll + ") failed: " + error_text(e);
}

// ---- dxgi-factory -----------------------------------------------------------

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2_t)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1_t)(REFIID, void**);

void probe_dxgi_factory(Record& rec) {
    DWORD le = 0;
    HMODULE h = load_sys(L"dxgi.dll", &le);
    if (!h) { fail_load(rec, "dxgi.dll", le); return; }

    auto p2 = reinterpret_cast<PFN_CreateDXGIFactory2_t>(GetProcAddress(h, "CreateDXGIFactory2"));
    auto p1 = reinterpret_cast<PFN_CreateDXGIFactory1_t>(GetProcAddress(h, "CreateDXGIFactory1"));
    if (!p2 && !p1) {
        rec.status = Status::MISSING;
        rec.errorcode = GetLastError();
        rec.detail = "dxgi.dll loaded but CreateDXGIFactory2/CreateDXGIFactory1 not exported";
        return;
    }

    IDXGIFactory1* fac = nullptr;
    HRESULT hr = p2 ? p2(0, __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac))
                    : p1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac));
    const char* via = p2 ? "CreateDXGIFactory2" : "CreateDXGIFactory1";
    if (FAILED(hr) && p2 && p1) {  // Factory2 refused; try the older entry point
        rel(fac);
        hr = p1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac));
        via = "CreateDXGIFactory1(fallback)";
    }
    if (FAILED(hr) || !fac) {
        rel(fac);
        fail_hr(rec, hr, via);
        return;
    }

    // Enumerate; report adapter 0, or the first hardware adapter if adapter 0 is
    // the software (Basic Render Driver / WARP) adapter.
    UINT count = 0, soft = 0;
    int idx0 = -1, first_hw = -1;
    DXGI_ADAPTER_DESC1 d0 = {}, dhw = {};
    for (UINT i = 0; i < 64; ++i) {  // bounded sanity loop
        IDXGIAdapter1* ad = nullptr;
        HRESULT ehr = fac->EnumAdapters1(i, &ad);
        if (FAILED(ehr) || !ad) { rel(ad); break; }  // DXGI_ERROR_NOT_FOUND ends the list
        DXGI_ADAPTER_DESC1 d = {};
        if (SUCCEEDED(ad->GetDesc1(&d))) {
            ++count;
            const bool sw = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            if (sw) ++soft;
            if (i == 0) { idx0 = 0; d0 = d; }
            if (!sw && first_hw < 0) { first_hw = static_cast<int>(i); dhw = d; }
        }
        ad->Release();
    }
    rel(fac);

    int pick = -1;
    DXGI_ADAPTER_DESC1 pd = {};
    if (idx0 >= 0 && !(d0.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { pick = 0; pd = d0; }
    else if (first_hw >= 0) { pick = first_hw; pd = dhw; }
    else if (idx0 >= 0) { pick = 0; pd = d0; }

    if (pick < 0) {
        rec.status = Status::WARN;
        rec.value = "adapter_count=0";
        rec.detail = std::string(via) + " ok but EnumAdapters1 returned no adapters";
        return;
    }
    const bool sw = (pd.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    rec.status = Status::OK;
    rec.value = "adapter=" + narrow(pd.Description) +
                ";vendor=0x" + hex4(pd.VendorId) +
                ";device=0x" + hex4(pd.DeviceId) +
                ";dedicated_mb=" + num(pd.DedicatedVideoMemory / (1024ull * 1024ull)) +
                ";shared_mb=" + num(pd.SharedSystemMemory / (1024ull * 1024ull)) +
                ";flags=" + hexmin(pd.Flags) +
                ";index=" + num(static_cast<unsigned>(pick)) +
                ";adapter_count=" + num(count) +
                ";software_adapters=" + num(soft);
    rec.detail = std::string("created via ") + via + "; reporting adapter " + num(static_cast<unsigned>(pick));
    if (sw) rec.detail += " (SOFTWARE adapter - no hardware adapter present)";
    else if (pick != 0) rec.detail += " (adapter 0 is a software adapter, skipped)";
}

// ---- d3d11-create -----------------------------------------------------------

const D3D_FEATURE_LEVEL kFL11[] = {
    D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1,
};

void probe_d3d11_create(Record& rec) {
    DWORD le = 0;
    HMODULE h = load_sys(L"d3d11.dll", &le);
    if (!h) { fail_load(rec, "d3d11.dll", le); return; }
    auto pfn = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(h, "D3D11CreateDevice"));
    if (!pfn) {
        rec.status = Status::MISSING;
        rec.errorcode = GetLastError();
        rec.detail = "d3d11.dll loaded but D3D11CreateDevice not exported";
        return;
    }

    const UINT nfl = static_cast<UINT>(sizeof kFL11 / sizeof kFL11[0]);
    auto create = [&](D3D_DRIVER_TYPE type, D3D_FEATURE_LEVEL* got) -> HRESULT {
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL fl = static_cast<D3D_FEATURE_LEVEL>(0);
        HRESULT hr = pfn(nullptr, type, nullptr, 0, kFL11, nfl, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        if (SUCCEEDED(hr)) *got = fl;
        rel(ctx);
        rel(dev);
        return hr;
    };

    D3D_FEATURE_LEVEL fl = static_cast<D3D_FEATURE_LEVEL>(0);
    HRESULT hr = create(D3D_DRIVER_TYPE_HARDWARE, &fl);
    if (SUCCEEDED(hr)) {
        rec.status = Status::OK;
        rec.value = "featurelevel=" + hexmin(static_cast<uint32_t>(fl)) + ";driver=hardware;fl=" + fl_name(static_cast<uint32_t>(fl));
        rec.detail = std::string("D3D11CreateDevice(HARDWARE) ok, feature level ") + fl_name(static_cast<uint32_t>(fl));
        return;
    }

    // Hardware failed: remember why, then see whether WARP still works so the
    // report distinguishes "no GPU access" from "no D3D11 at all".
    const HRESULT hw_hr = hr;
    D3D_FEATURE_LEVEL wfl = static_cast<D3D_FEATURE_LEVEL>(0);
    HRESULT whr = create(D3D_DRIVER_TYPE_WARP, &wfl);
    rec.errorcode = static_cast<uint32_t>(hw_hr);
    if (SUCCEEDED(whr)) {
        rec.status = (hw_hr == E_ACCESSDENIED) ? Status::DENIED : Status::WARN;
        rec.value = "featurelevel=" + hexmin(static_cast<uint32_t>(wfl)) + ";driver=warp;fl=" + fl_name(static_cast<uint32_t>(wfl)) +
                    ";hw_hr=" + hex32(static_cast<uint32_t>(hw_hr));
        rec.detail = "hardware device failed (hr=" + hex32(static_cast<uint32_t>(hw_hr)) + " " +
                     error_text(static_cast<uint32_t>(hw_hr)) + "); WARP software device works, feature level " +
                     fl_name(static_cast<uint32_t>(wfl));
    } else {
        rec.status = classify_hr(hw_hr);
        rec.value = "driver=none;hw_hr=" + hex32(static_cast<uint32_t>(hw_hr)) + ";warp_hr=" + hex32(static_cast<uint32_t>(whr));
        rec.detail = "D3D11CreateDevice failed for HARDWARE (hr=" + hex32(static_cast<uint32_t>(hw_hr)) + " " +
                     error_text(static_cast<uint32_t>(hw_hr)) + ") and WARP (hr=" + hex32(static_cast<uint32_t>(whr)) + ")";
    }
}

// ---- d3d12-create / d3d12-debug-layer ---------------------------------------

void probe_d3d12_create(Record& rec) {
    DWORD le = 0;
    HMODULE h = load_sys(L"d3d12.dll", &le);
    if (!h) { fail_load(rec, "d3d12.dll", le); return; }
    auto pfn = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(h, "D3D12CreateDevice"));
    if (!pfn) {
        rec.status = Status::MISSING;
        rec.errorcode = GetLastError();
        rec.detail = "d3d12.dll loaded but D3D12CreateDevice not exported";
        return;
    }

    ID3D12Device* dev = nullptr;
    HRESULT hr = pfn(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev));
    if (FAILED(hr) || !dev) {
        rel(dev);
        rec.value = "created=0";
        fail_hr(rec, hr, "D3D12CreateDevice(min FL 11_0)");
        return;
    }

    std::string value = "created=1";
    std::string detail = "D3D12CreateDevice(default adapter, min FL 11_0) ok";
    Status st = Status::OK;

    const D3D_FEATURE_LEVEL req[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
                                     D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1, kFL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fls = {};
    fls.NumFeatureLevels = static_cast<UINT>(sizeof req / sizeof req[0]);
    fls.pFeatureLevelsRequested = req;
    HRESULT fhr = dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fls, sizeof fls);
    if (SUCCEEDED(fhr)) {
        const uint32_t mx = static_cast<uint32_t>(fls.MaxSupportedFeatureLevel);
        value += ";maxfeaturelevel=" + hexmin(mx) + ";fl=" + fl_name(mx);
        detail += std::string("; max feature level ") + fl_name(mx);
    } else {
        value += ";maxfeaturelevel=unknown";
        detail += "; CheckFeatureSupport(FEATURE_LEVELS) failed hr=" + hex32(static_cast<uint32_t>(fhr));
        st = Status::WARN;
        rec.errorcode = static_cast<uint32_t>(fhr);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS opt = {};
    HRESULT ohr = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof opt);
    if (SUCCEEDED(ohr)) {
        value += ";resource_binding_tier=" + num(static_cast<unsigned>(opt.ResourceBindingTier)) +
                 ";resource_heap_tier=" + num(static_cast<unsigned>(opt.ResourceHeapTier));
    } else {
        value += ";resource_binding_tier=unknown;resource_heap_tier=unknown";
        detail += "; CheckFeatureSupport(D3D12_OPTIONS) failed hr=" + hex32(static_cast<uint32_t>(ohr));
        st = Status::WARN;
        if (!rec.errorcode) rec.errorcode = static_cast<uint32_t>(ohr);
    }
    dev->Release();

    rec.status = st;
    rec.value = value;
    rec.detail = detail;
}

void probe_d3d12_debug_layer(Record& rec) {
    DWORD le = 0;
    HMODULE h = load_sys(L"d3d12.dll", &le);
    if (!h) { fail_load(rec, "d3d12.dll", le); rec.value = "available=0"; return; }
    auto pfn = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(h, "D3D12GetDebugInterface"));
    if (!pfn) {
        rec.status = Status::MISSING;
        rec.value = "available=0";
        rec.errorcode = GetLastError();
        rec.detail = "d3d12.dll loaded but D3D12GetDebugInterface not exported";
        return;
    }

    // Only *query* the interface; never call EnableDebugLayer (that would change
    // device-creation behaviour for every later probe in this process).
    ID3D12Debug* dbg = nullptr;
    HRESULT hr = pfn(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&dbg));
    if (SUCCEEDED(hr) && dbg) {
        dbg->Release();
        rec.status = Status::OK;
        rec.value = "available=1";
        rec.detail = "ID3D12Debug obtainable (debug layer / Graphics Tools present)";
        return;
    }
    rel(dbg);
    rec.value = "available=0";
    fail_hr(rec, hr, "D3D12GetDebugInterface(ID3D12Debug)");
    // 0x887A002D DXGI_ERROR_SDK_COMPONENT_MISSING / E_NOINTERFACE: debug layer not installed.
    if (hr == static_cast<HRESULT>(0x887A002D) || hr == E_NOINTERFACE) rec.status = Status::MISSING;
}

// ---- bridge detection -------------------------------------------------------

// Load `dll`, then GetProcAddress each named export. Fills `value` with
// "<keyprefix>=<0|1>;<expkey>=<0|1>..." (caller supplies the keys) and sets the
// record status: OK if the DLL loads and the FIRST (key) export resolves,
// NOTIMPL if the DLL loads but that export is absent, else MISSING/DENIED/INFO
// per the LoadLibrary error. All module refs taken here are released.
struct Export { const char* key; const char* symbol; };

void bridge_probe(Record& rec, const wchar_t* dll, const char* dll_key,
                  std::initializer_list<Export> exports, const std::string& note) {
    const std::string dll_narrow = narrow(dll);
    DWORD le = 0;
    HMODULE h = load_sys(dll, &le);
    std::string value = std::string(dll_key) + "=" + (h ? "1" : "0");
    if (!h) {
        for (const Export& e : exports) value += std::string(";") + e.key + "=0";
        rec.value = value;
        fail_load(rec, dll_narrow.c_str(), le);
        rec.detail += note.empty() ? "" : " | " + note;
        return;
    }
    bool primary = false, first = true;
    for (const Export& e : exports) {
        const bool ok = GetProcAddress(h, e.symbol) != nullptr;
        value += std::string(";") + e.key + "=" + (ok ? "1" : "0");
        if (first) { primary = ok; first = false; }
    }
    FreeLibrary(h);
    rec.value = value;
    rec.status = primary ? Status::OK : Status::NOTIMPL;
    rec.detail = dll_narrow + (primary ? " present, key export resolved" : " present but key export NOT found");
    if (!note.empty()) rec.detail += " | " + note;
}

void probe_bridge_d3d11on12(Record& rec) {
    // D3D11On12CreateDevice lives in d3d11.dll itself (no separate DLL).
    bridge_probe(rec, L"d3d11.dll", "dll", {{"export", "D3D11On12CreateDevice"}},
                 "d3d11on12 = D3D11 API layered over a D3D12 device; export is in d3d11.dll");
}

void probe_bridge_d3d9on12(Record& rec) {
    // The spec'd export is looked up in d3d9on12.dll; d3d9.dll!Direct3DCreate9On12
    // is the app-facing entry point, so report that too as extra context.
    bridge_probe(rec, L"d3d9on12.dll", "dll", {{"export", "Direct3DCreate9On12"}, {"umd_OpenAdapter", "OpenAdapter"}},
                 "d3d9on12 = D3D9 UMD layered over D3D12");
    // Append d3d9.dll!Direct3DCreate9On12 (the documented public entry point).
    DWORD le = 0;
    HMODULE h9 = load_sys(L"d3d9.dll", &le);
    bool pub = h9 && GetProcAddress(h9, "Direct3DCreate9On12") != nullptr;
    if (h9) FreeLibrary(h9);
    rec.value += std::string(";d3d9_Direct3DCreate9On12=") + (pub ? "1" : "0");
    // If the spec'd export is absent from d3d9on12.dll but the DLL and the public
    // d3d9.dll entry point both exist, the bridge is present: upgrade NOTIMPL->OK.
    if (rec.status == Status::NOTIMPL && pub) {
        rec.status = Status::OK;
        rec.detail += " | Direct3DCreate9On12 is exported by d3d9.dll, not d3d9on12.dll";
    }
}

void probe_bridge_glon12(Record& rec) {
    bridge_probe(rec, L"opengl32.dll", "opengl32",
                 {{"wglCreateContext", "wglCreateContext"}, {"glGetString", "glGetString"}},
                 "GLOn12 is the Mesa-based GL-over-D3D12 mapping layer; presence here only indicates the inbox "
                 "opengl32.dll front-end, NOT that a GLOn12 ICD is installed");
}

void probe_bridge_dxcore(Record& rec) {
    bridge_probe(rec, L"dxcore.dll", "dll", {{"export", "DXCoreCreateAdapterFactory"}},
                 "DXCore adapter enumeration (works for D3D12-only / compute adapters)");
}

void probe_bridge_vulkan(Record& rec) {
    bridge_probe(rec, L"vulkan-1.dll", "dll", {{"export", "vkCreateInstance"}},
                 "Dozen is Vulkan-on-D3D12; a vulkan-1 loader is the signal (loader only - says nothing about "
                 "which ICD sits behind it)");
}

}  // namespace

void register_I_graphics(Reporter& r) {
    r.run("I", "dxgi-factory", probe_dxgi_factory);
    r.run("I", "d3d11-create", probe_d3d11_create);
    r.run("I", "d3d12-create", probe_d3d12_create);
    r.run("I", "d3d12-debug-layer", probe_d3d12_debug_layer);
    r.run("I", "bridge:d3d11on12", probe_bridge_d3d11on12);
    r.run("I", "bridge:d3d9on12", probe_bridge_d3d9on12);
    r.run("I", "bridge:glon12", probe_bridge_glon12);
    r.run("I", "bridge:dxcore", probe_bridge_dxcore);
    r.run("I", "bridge:vulkan", probe_bridge_vulkan);
}

}  // namespace probe
