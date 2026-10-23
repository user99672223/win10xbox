/* guest_d3d11_swap: a swap chain on a window of OUR user32 (docs/slices/swapchain.md).
 *
 * The window is an ordinary one made with the layer's emulated user32/win32k (core/user HWND). The NATIVE d3d11/dxgi
 * create a swap chain for it (IDXGIFactory::CreateSwapChain, windowed, 1 buffer) and present 60 frames of a moving
 * red triangle on a blue clear with Present(1,0). The triangle's viewport slides horizontally with sin(2*pi*n/60), so
 * frames 30 and 60 are centred. At frame 30 the back buffer is copied to a staging texture and read back before the
 * Present. Exit 42 = the centre pixel is the triangle colour and the corner the clear colour. The host test then checks
 * the screenshot at the window's rectangle (window at 100,100, 256x256: centre (228,228) red, (101,101) blue).
 *
 *   swap: window hwnd=<h> client=256x256
 *   swap: device driver=<hardware|warp> feature_level=0x<hex>
 *   swap: CreateSwapChain -> <hr>
 *   swap: readback centre=<aarrggbb> corner=<aarrggbb>
 *   swap: presented 60 frames
 *   swap: done
 * Other exit codes name the step that failed (the line before says why). Libraries load at run time. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SIZE 256
#define FRAMES 60

static const char kShader[] =
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p[3] = { float2(-0.5, -0.5), float2(0.0, 0.7), float2(0.5, -0.5) };\n"
    "  return float4(p[id], 0.0, 1.0);\n"
    "}\n"
    "float4 ps_main(float4 pos : SV_Position) : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";

typedef HRESULT(WINAPI* PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

static int fail(int code, const char* what) {
    printf("swap: FAILED: %s\n", what);
    fflush(stdout);
    return code;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) { ValidateRect(h, NULL); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

static void pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

int main(void) {
    WNDCLASSW wc;
    HWND hwnd;
    RECT rc;
    HMODULE d3d11 = NULL, compiler = NULL;
    PFN_D3D11_CREATE_DEVICE create;
    PFN_COMPILE compile;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    D3D_FEATURE_LEVEL level = 0;
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const char* driver = "hardware";
    HRESULT hr;
    IDXGIDevice* dxgi_dev = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIFactory* factory = NULL;
    IDXGISwapChain* sc = NULL;
    DXGI_SWAP_CHAIN_DESC sd;
    ID3D11Texture2D *back = NULL, *staging = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    ID3D11VertexShader* vs = NULL;
    ID3D11PixelShader* ps = NULL;
    ID3DBlob *vs_blob = NULL, *ps_blob = NULL, *err = NULL;
    ID3D11RasterizerState* rs = NULL;
    D3D11_RASTERIZER_DESC rd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    unsigned centre = 0, corner = 0;
    int frame, readback_ok = 0;

    printf("swap: start\n");
    fflush(stdout);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"w10x_swap";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassW(&wc)) return fail(1, "RegisterClassW");
    hwnd = CreateWindowExW(0, L"w10x_swap", L"swap", WS_POPUP | WS_VISIBLE, 100, 100, SIZE, SIZE, NULL, NULL,
                           wc.hInstance, NULL);
    if (!hwnd) return fail(2, "CreateWindowExW");
    ShowWindow(hwnd, SW_SHOW);
    pump();
    GetClientRect(hwnd, &rc);
    printf("swap: window hwnd=%p client=%ldx%ld\n", (void*)hwnd, rc.right - rc.left, rc.bottom - rc.top);
    fflush(stdout);

    d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) { printf("swap: LoadLibrary(d3d11.dll) error %lu\n", GetLastError()); return fail(10, "d3d11.dll did not load"); }
    create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11CreateDevice");
    if (!create) return fail(11, "no D3D11CreateDevice");
    hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, wanted, 4, D3D11_SDK_VERSION, &dev, &level, &ctx);
    printf("swap: D3D11CreateDevice(hardware) -> %08lx\n", (unsigned long)hr);
    if (FAILED(hr) || !dev) {
        dev = NULL; ctx = NULL;
        hr = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, wanted, 4, D3D11_SDK_VERSION, &dev, &level, &ctx);
        printf("swap: D3D11CreateDevice(warp) -> %08lx\n", (unsigned long)hr);
        driver = "warp";
        if (FAILED(hr) || !dev) return fail(12, "no hardware and no WARP device");
    }
    printf("swap: device driver=%s feature_level=0x%04x\n", driver, (unsigned)level);
    fflush(stdout);

    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void**)&dxgi_dev)) ||
        FAILED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)) ||
        FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void**)&factory)))
        return fail(13, "IDXGIFactory from the device");

    memset(&sd, 0, sizeof sd);
    sd.BufferDesc.Width = SIZE;
    sd.BufferDesc.Height = SIZE;
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
    printf("swap: CreateSwapChain -> %08lx\n", (unsigned long)hr);
    fflush(stdout);
    if (FAILED(hr) || !sc) return fail(14, "IDXGIFactory::CreateSwapChain on an emulated window");

    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&back))) return fail(15, "GetBuffer(0)");
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)back, NULL, &rtv))) return fail(15, "CreateRenderTargetView");
    memset(&td, 0, sizeof td);
    ID3D11Texture2D_GetDesc(back, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging))) return fail(15, "CreateTexture2D(staging)");

    compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    compile = compiler ? (PFN_COMPILE)GetProcAddress(compiler, "D3DCompile") : NULL;
    if (!compile) return fail(16, "d3dcompiler_47.dll D3DCompile");
    hr = compile(kShader, sizeof kShader - 1, "tri.hlsl", NULL, NULL, "vs_main", "vs_4_0", 0, 0, &vs_blob, &err);
    if (FAILED(hr) || !vs_blob) return fail(17, "vertex shader did not compile");
    hr = compile(kShader, sizeof kShader - 1, "tri.hlsl", NULL, NULL, "ps_main", "ps_4_0", 0, 0, &ps_blob, &err);
    if (FAILED(hr) || !ps_blob) return fail(17, "pixel shader did not compile");
    if (FAILED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL, &vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL, &ps)))
        return fail(17, "CreateVertexShader/CreatePixelShader");
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &rs))) return fail(17, "CreateRasterizerState");

    for (frame = 1; frame <= FRAMES; ++frame) {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = (float)(40.0 * sin(2.0 * 3.14159265358979 * frame / FRAMES));
        vp.TopLeftY = 0.0f;
        vp.Width = (float)SIZE;
        vp.Height = (float)SIZE;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        pump();
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, blue);
        ID3D11DeviceContext_RSSetState(ctx, rs);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        if (frame == FRAMES / 2) {
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)back);
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &map))) {
                const unsigned char* base = (const unsigned char*)map.pData;
                memcpy(&centre, base + (SIZE / 2) * map.RowPitch + (SIZE / 2) * 4, 4);
                memcpy(&corner, base + 1 * map.RowPitch + 1 * 4, 4);
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)staging, 0);
                readback_ok = 1;
                printf("swap: readback centre=%08x corner=%08x\n", centre, corner);
                fflush(stdout);
            }
        }
        hr = IDXGISwapChain_Present(sc, 1, 0);
        if (FAILED(hr)) { printf("swap: Present(frame %d) -> %08lx\n", frame, (unsigned long)hr); return fail(18, "Present"); }
        if (frame == 1) { printf("swap: first Present -> %08lx\n", (unsigned long)hr); fflush(stdout); }
    }
    printf("swap: presented %d frames\n", FRAMES);
    fflush(stdout);
    if (!readback_ok) return fail(19, "Map(staging)");
    /* B8G8R8A8: 0xAARRGGBB as a little-endian word. */
    if (centre != 0xffff0000u || corner != 0xff0000ffu) return fail(20, "readback colours");
    printf("swap: done\n");
    fflush(stdout);
    return 42;
}
