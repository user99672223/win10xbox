/* guest_gamelike: what a game's frame loop does, in one program, on the machine the game runs on (machine-emu-d3d,
 * docs/slices/gamelike.md): a window (CreateWindowExW + message loop), a D3D11 device and swap chain on it
 * (CreateDXGIFactory1 + IDXGIFactory::CreateSwapChain, as d3d11_swap.c), an XAudio2 2.9 engine with a mastering voice
 * and a looping source voice playing a 440 Hz tone, XInputGetState(0) polled every frame (no pad is fine; with a pad the
 * left stick moves the quad), a moving coloured quad drawn with a run-time-compiled HLSL shader (d3dcompiler_47) and
 * Present(1, 0). One status line per 30 frames:
 *   gamelike: frame N, pad=<connected|none>, audio=<playing|no-endpoint>, present ok
 * It runs until the layer's frame limit (WIN10XBOX_TEST_PRESENT_LIMIT) ends it with exit 42; without the limit it
 * stops after 10 s and exits 42 itself. Frames are paced to 60 per second. Any failure prints
 *   gamelike: FAILED: <step> hr=0x...
 * and exits with the code of the step: 1 RegisterClassW, 2 CreateWindowExW, 3 CoInitializeEx, 10 d3d11.dll, 11 device,
 * 12 xinput1_4.dll, 13 DXGI factory/adapter, 14 CreateSwapChain, 15 back buffer/RTV, 16 shader compile, 17 shader
 * objects/states, 18 Present, 19 constant buffer, 20 xaudio2_9.dll, 21 XAudio2Create/mastering voice, 22 source voice,
 * 23 SubmitSourceBuffer/Start. Libraries load at run time. */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 640
#define HEIGHT 360
#define RUN_MS 10000
#define RATE 48000u

static const char kShader[] =
    "cbuffer cb : register(b0) { float4 off; float4 tint; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "  float2 c[6] = { float2(-1,-1), float2(-1,1), float2(1,-1), float2(1,-1), float2(-1,1), float2(1,1) };\n"
    "  VSOut o;\n"
    "  o.pos = float4(c[id] * float2(0.25, 0.4) + off.xy, 0.0, 1.0);\n"
    "  o.col = lerp(tint, float4(1.0, 1.0, 0.2, 1.0), (c[id].y + 1.0) * 0.5);\n"
    "  return o;\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_Target { return i.col; }\n";

typedef HRESULT(WINAPI* PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_CREATE_FACTORY1)(REFIID, void**);

/* XInput, declared by hand (layout of XINPUT_STATE); the DLL is loaded at run time. */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PadGamepad;
typedef struct { DWORD dwPacketNumber; PadGamepad Gamepad; } PadState;
typedef DWORD(WINAPI* PFN_XINPUT_GET_STATE)(DWORD, PadState*);

/* XAudio2 2.8/2.9, declared by hand as in xaudio2_play.c: IXAudio2 is COM, the voices are not. */
typedef long HR;
#define FAILED_(h) ((h) < 0)
struct IUnkVtbl {
    HR(STDMETHODCALLTYPE* QueryInterface)(void*, const GUID*, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(void*);
    ULONG(STDMETHODCALLTYPE* Release)(void*);
};
typedef struct IEnum { const struct IEnumVtbl* v; } IEnum;
struct IEnumVtbl {
    struct IUnkVtbl u;
    HR(STDMETHODCALLTYPE* EnumAudioEndpoints)(IEnum*, int, DWORD, void**);
    HR(STDMETHODCALLTYPE* GetDefaultAudioEndpoint)(IEnum*, int flow, int role, void** device);
};
static const GUID kClsidEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIidEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
typedef struct IXA2 { const struct IXA2Vtbl* v; } IXA2;
struct IXA2Vtbl {
    struct IUnkVtbl u;
    HR(STDMETHODCALLTYPE* RegisterForCallbacks)(IXA2*, void*);
    void(STDMETHODCALLTYPE* UnregisterForCallbacks)(IXA2*, void*);
    HR(STDMETHODCALLTYPE* CreateSourceVoice)(IXA2*, void** voice, const WAVEFORMATEX* fmt, UINT32 flags, float max_ratio,
                                             void* callback, const void* sends, const void* chain);
    HR(STDMETHODCALLTYPE* CreateSubmixVoice)(IXA2*, void**, UINT32, UINT32, UINT32, UINT32, const void*, const void*);
    HR(STDMETHODCALLTYPE* CreateMasteringVoice)(IXA2*, void** voice, UINT32 channels, UINT32 rate, UINT32 flags,
                                                const WCHAR* device, const void* chain, int category);
    HR(STDMETHODCALLTYPE* StartEngine)(IXA2*);
    void(STDMETHODCALLTYPE* StopEngine)(IXA2*);
    HR(STDMETHODCALLTYPE* CommitChanges)(IXA2*, UINT32);
};
#pragma pack(push, 1)
typedef struct { void* pCurrentBufferContext; UINT32 BuffersQueued; UINT64 SamplesPlayed; } XA2_VOICE_STATE;
typedef struct {
    UINT32 Flags, AudioBytes;
    const BYTE* pAudioData;
    UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount;
    void* pContext;
} XA2_BUFFER;
#pragma pack(pop)
typedef struct IVoice { const struct IVoiceVtbl* v; } IVoice;
struct IVoiceVtbl {
    void(STDMETHODCALLTYPE* GetVoiceDetails)(IVoice*, void*);
    void(STDMETHODCALLTYPE* voice_slots[17])(void);
    void(STDMETHODCALLTYPE* DestroyVoice)(IVoice*);
    HR(STDMETHODCALLTYPE* Start)(IVoice*, UINT32 flags, UINT32 set);
    HR(STDMETHODCALLTYPE* Stop)(IVoice*, UINT32 flags, UINT32 set);
    HR(STDMETHODCALLTYPE* SubmitSourceBuffer)(IVoice*, const void* buffer, const void* wma);
    HR(STDMETHODCALLTYPE* FlushSourceBuffers)(IVoice*);
    HR(STDMETHODCALLTYPE* Discontinuity)(IVoice*);
    HR(STDMETHODCALLTYPE* ExitLoop)(IVoice*, UINT32);
    void(STDMETHODCALLTYPE* GetState)(IVoice*, XA2_VOICE_STATE*, UINT32 flags);
};
typedef HR(WINAPI* XAudio2Create_t)(void**, UINT32, UINT32);

static int fail(int code, const char* step, HRESULT hr) {
    printf("gamelike: FAILED: %s hr=0x%08lx\n", step, (unsigned long)hr);
    fflush(stdout);
    return code;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) { ValidateRect(h, NULL); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

/* Returns 1 when WM_QUIT arrived. */
static int pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return 1;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static short g_pcm[RATE * 2]; /* 1 s stereo 16-bit; 440 whole cycles, so the loop is seamless */

int main(void) {
    WNDCLASSW wc;
    HWND hwnd;
    HMODULE d3d11, dxgi, compiler, xi, xa2dll;
    PFN_D3D11_CREATE_DEVICE create;
    PFN_CREATE_FACTORY1 create_factory;
    PFN_COMPILE compile;
    PFN_XINPUT_GET_STATE xinput_get;
    XAudio2Create_t xa2_create;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    D3D_FEATURE_LEVEL level = 0;
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    IDXGIFactory* factory = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGISwapChain* sc = NULL;
    DXGI_SWAP_CHAIN_DESC sd;
    ID3D11Texture2D* back = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    ID3D11VertexShader* vs = NULL;
    ID3D11PixelShader* ps = NULL;
    ID3D11RasterizerState* rs = NULL;
    ID3D11Buffer* cbuf = NULL;
    ID3DBlob *vs_blob = NULL, *ps_blob = NULL, *err = NULL;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BUFFER_DESC bd;
    D3D11_VIEWPORT vp;
    HRESULT hr;
    const char* driver = "hardware";
    const float clear[4] = {0.05f, 0.08f, 0.25f, 1.0f};
    IXA2* xa = NULL;
    IVoice *master = NULL, *src = NULL;
    int audio = 0, pad_connected = 0, quit = 0;
    DWORD t0, pad_err = 0;
    unsigned frame = 0;
    UINT32 i;

    printf("gamelike: start\n");
    fflush(stdout);

    /* --- window --- */
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"w10x_gamelike";
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    if (!RegisterClassW(&wc)) return fail(1, "RegisterClassW", HRESULT_FROM_WIN32(GetLastError()));
    hwnd = CreateWindowExW(0, L"w10x_gamelike", L"gamelike", WS_POPUP | WS_VISIBLE, 100, 100, WIDTH, HEIGHT, NULL, NULL,
                           wc.hInstance, NULL);
    if (!hwnd) return fail(2, "CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
    ShowWindow(hwnd, SW_SHOW);
    pump();
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return fail(3, "CoInitializeEx", hr);

    /* --- D3D11 device + swap chain --- */
    d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) return fail(10, "LoadLibrary(d3d11.dll)", HRESULT_FROM_WIN32(GetLastError()));
    create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11CreateDevice");
    if (!create) return fail(11, "no D3D11CreateDevice", 0x8007007F);
    dxgi = LoadLibraryW(L"dxgi.dll");
    create_factory = dxgi ? (PFN_CREATE_FACTORY1)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    if (!create_factory) return fail(13, "dxgi.dll CreateDXGIFactory1", 0x8007007F);
    hr = create_factory(&IID_IDXGIFactory, (void**)&factory);
    if (FAILED(hr) || !factory) return fail(13, "CreateDXGIFactory1", hr);
    hr = IDXGIFactory_EnumAdapters(factory, 0, &adapter);
    if (FAILED(hr)) return fail(13, "EnumAdapters(0)", hr);
    hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, wanted, 4, D3D11_SDK_VERSION, &dev, &level, &ctx);
    if (FAILED(hr) || !dev) {
        dev = NULL;
        ctx = NULL;
        hr = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, wanted, 4, D3D11_SDK_VERSION, &dev, &level, &ctx);
        driver = "warp";
        if (FAILED(hr) || !dev) return fail(11, "D3D11CreateDevice (adapter 0 and WARP)", hr);
    }
    printf("gamelike: device driver=%s feature_level=0x%04x\n", driver, (unsigned)level);
    memset(&sd, 0, sizeof sd);
    sd.BufferDesc.Width = WIDTH;
    sd.BufferDesc.Height = HEIGHT;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown*)dev, &sd, &sc);
    if (FAILED(hr) || !sc) return fail(14, "IDXGIFactory::CreateSwapChain", hr);
    hr = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&back);
    if (FAILED(hr)) return fail(15, "GetBuffer(0)", hr);
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)back, NULL, &rtv);
    if (FAILED(hr)) return fail(15, "CreateRenderTargetView", hr);

    compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    compile = compiler ? (PFN_COMPILE)GetProcAddress(compiler, "D3DCompile") : NULL;
    if (!compile) return fail(16, "d3dcompiler_47.dll D3DCompile", 0x8007007F);
    hr = compile(kShader, sizeof kShader - 1, "quad.hlsl", NULL, NULL, "vs_main", "vs_4_0", 0, 0, &vs_blob, &err);
    if (FAILED(hr) || !vs_blob) return fail(16, "vertex shader compile", hr);
    hr = compile(kShader, sizeof kShader - 1, "quad.hlsl", NULL, NULL, "ps_main", "ps_4_0", 0, 0, &ps_blob, &err);
    if (FAILED(hr) || !ps_blob) return fail(16, "pixel shader compile", hr);
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL, &vs);
    if (FAILED(hr)) return fail(17, "CreateVertexShader", hr);
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL, &ps);
    if (FAILED(hr)) return fail(17, "CreatePixelShader", hr);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(dev, &rd, &rs);
    if (FAILED(hr)) return fail(17, "CreateRasterizerState", hr);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf);
    if (FAILED(hr)) return fail(19, "CreateBuffer(constant)", hr);
    printf("gamelike: D3D11 ready (%dx%d, quad shader compiled)\n", WIDTH, HEIGHT);
    fflush(stdout);

    /* --- XInput --- */
    xi = LoadLibraryW(L"xinput1_4.dll");
    xinput_get = xi ? (PFN_XINPUT_GET_STATE)GetProcAddress(xi, "XInputGetState") : NULL;
    if (!xinput_get) return fail(12, "xinput1_4.dll XInputGetState", HRESULT_FROM_WIN32(GetLastError()));

    /* --- XAudio2: 440 Hz looping tone, only when the machine has a render endpoint --- */
    xa2dll = LoadLibraryW(L"xaudio2_9.dll");
    xa2_create = xa2dll ? (XAudio2Create_t)GetProcAddress(xa2dll, "XAudio2Create") : NULL;
    if (!xa2_create) return fail(20, "xaudio2_9.dll XAudio2Create", HRESULT_FROM_WIN32(GetLastError()));
    {
        IEnum* en = NULL;
        void* dv = NULL;
        HR h = CoCreateInstance(&kClsidEnumerator, NULL, CLSCTX_INPROC_SERVER, &kIidEnumerator, (void**)&en);
        if (!FAILED_(h)) {
            h = en->v->GetDefaultAudioEndpoint(en, 0 /* eRender */, 0 /* eConsole */, &dv);
            if (dv) ((struct IUnkVtbl**)dv)[0]->Release(dv);
            en->v->u.Release(en);
            audio = !FAILED_(h);
        }
        if (!audio) printf("gamelike: no render endpoint hr=0x%08lx, audio skipped\n", (unsigned long)h);
    }
    if (audio) {
        WAVEFORMATEX f;
        XA2_BUFFER b;
        XA2_VOICE_STATE st;
        for (i = 0; i < RATE; i++) {
            short v = (short)(sin(2.0 * 3.14159265358979 * 440.0 * i / RATE) * 6000.0);
            g_pcm[2 * i] = v;
            g_pcm[2 * i + 1] = v;
        }
        hr = xa2_create((void**)&xa, 0, 0xFFFFFFFFu);
        if (FAILED(hr)) return fail(21, "XAudio2Create", hr);
        hr = xa->v->CreateMasteringVoice(xa, (void**)&master, 0, 0, 0, NULL, NULL, 0);
        if (FAILED(hr)) return fail(21, "CreateMasteringVoice", hr);
        memset(&f, 0, sizeof f);
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 2;
        f.nSamplesPerSec = RATE;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 4;
        f.nAvgBytesPerSec = RATE * 4;
        hr = xa->v->CreateSourceVoice(xa, (void**)&src, &f, 0, 2.0f, NULL, NULL, NULL);
        if (FAILED(hr)) return fail(22, "CreateSourceVoice", hr);
        memset(&b, 0, sizeof b);
        b.AudioBytes = sizeof g_pcm;
        b.pAudioData = (const BYTE*)g_pcm;
        b.LoopCount = 255; /* XAUDIO2_LOOP_INFINITE */
        hr = src->v->SubmitSourceBuffer(src, &b, NULL);
        if (FAILED(hr)) return fail(23, "SubmitSourceBuffer", hr);
        hr = src->v->Start(src, 0, 0);
        if (FAILED(hr)) return fail(23, "IXAudio2SourceVoice::Start", hr);
        src->v->GetState(src, &st, 0);
        printf("gamelike: audio started (440 Hz loop, BuffersQueued=%u)\n", (unsigned)st.BuffersQueued);
        fflush(stdout);
    }

    /* --- frame loop --- */
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)WIDTH;
    vp.Height = (float)HEIGHT;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    t0 = GetTickCount();
    while (!quit && GetTickCount() - t0 < RUN_MS) {
        PadState ps_state;
        float ox, oy, cb[8];
        double t;
        DWORD due;
        quit = pump();
        frame++;
        t = frame / 60.0;
        ox = (float)(0.5 * sin(t * 2.0));
        oy = (float)(0.3 * sin(t * 3.1));
        memset(&ps_state, 0, sizeof ps_state);
        pad_err = xinput_get(0, &ps_state);
        pad_connected = (pad_err == ERROR_SUCCESS);
        if (pad_connected) {
            ox += ps_state.Gamepad.sThumbLX / 32768.0f * 0.5f;
            oy += ps_state.Gamepad.sThumbLY / 32768.0f * 0.5f;
        }
        cb[0] = ox; cb[1] = oy; cb[2] = 0.0f; cb[3] = 0.0f;
        cb[4] = (float)(0.5 + 0.5 * sin(t * 1.3));
        cb[5] = (float)(0.5 + 0.5 * sin(t * 0.9 + 2.0));
        cb[6] = (float)(0.5 + 0.5 * sin(t * 1.7 + 4.0));
        cb[7] = 1.0f;
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)cbuf, 0, NULL, cb, 0, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        ID3D11DeviceContext_RSSetState(ctx, rs);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 6, 0);
        hr = IDXGISwapChain_Present(sc, 1, 0);
        if (FAILED(hr)) {
            printf("gamelike: Present(frame %u) failed\n", frame);
            return fail(18, "Present", hr);
        }
        if (frame % 30 == 0) {
            printf("gamelike: frame %u, pad=%s, audio=%s, present ok\n", frame, pad_connected ? "connected" : "none",
                   audio ? "playing" : "no-endpoint");
            fflush(stdout);
        }
        /* 60 frames per second */
        due = t0 + (DWORD)(frame * 1000u / 60u);
        if ((int)(due - GetTickCount()) > 0) Sleep(due - GetTickCount());
    }

    if (src) { src->v->Stop(src, 0, 0); src->v->DestroyVoice(src); }
    if (master) master->v->DestroyVoice(master);
    if (xa) xa->v->u.Release(xa);
    printf("gamelike: done after %u frames\n", frame);
    fflush(stdout);
    return 42;
}
