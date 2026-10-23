/* guest_d3d11_tri: a guest renders with Direct3D 11 through the layer and reads the result back.
 *
 * The program is an ordinary D3D11 client with no window and no swap chain (docs/slices/d3d.md): the NATIVE d3d11.dll,
 * dxgi.dll, d3dcompiler_47.dll and the user-mode driver (the hardware's, or d3d10warp.dll) run as guest code, their
 * D3DKMT calls (gdi32 -> win32u NtGdiDdDDI*) are forwarded by name to the host's win32u, i.e. to the host's own
 * dxgkrnl. Every library is loaded at run time, so a missing DLL or export is a printed reason and an exit code and
 * not a loader failure before main().
 *
 *   1. LoadLibrary d3d11.dll, D3D11CreateDevice(HARDWARE); when that fails, D3D11CreateDevice(WARP). The adapter's
 *      name, feature level and which driver type succeeded are printed:
 *        d3d11: adapter=<name> feature_level=<hex> driver=<hardware|warp>
 *   2. compile a vertex and a pixel shader at run time with D3DCompile from the native d3dcompiler_47.dll
 *   3. a 64x64 RGBA8 render-target texture: clear to blue, draw one red triangle (no vertex buffer: the vertex shader
 *      reads the three corners from SV_VertexID), copy to a staging texture, Map it
 *   4. the centre pixel must be the triangle colour (255,0,0) and the corner pixel (0,0) the clear colour (0,0,255)
 * Exit 42 when both match. Other exit codes name the step that failed (see fail()); the cause is the line before.
 *
 * Informational output after the gate (never changes the exit code): the modules this process ended up with (the
 * user-mode driver among them) and the adapters DXGI enumerates.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#define RT_SIZE 64

static const char kShader[] =
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p[3] = { float2(-0.5, -0.5), float2(0.0, 0.7), float2(0.5, -0.5) };\n"
    "  return float4(p[id], 0.0, 1.0);\n"
    "}\n"
    "float4 ps_main(float4 pos : SV_Position) : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";

typedef HRESULT(WINAPI* PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_CREATE_FACTORY1)(REFIID, void**);

static int fail(int code, const char* what) {
    printf("d3d11: FAILED: %s\n", what);
    fflush(stdout);
    return code;
}

static void narrow(const WCHAR* w, char* out, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = 0;
}

static void print_loaded(const char* name) {
    WCHAR path[MAX_PATH];
    HMODULE m = GetModuleHandleA(name);
    char p[MAX_PATH * 3] = "";
    if (m && GetModuleFileNameW(m, path, MAX_PATH)) narrow(path, p, sizeof p);
    printf("d3d11: loaded %s -> %p %s\n", name, (void*)m, p);
}

/* The module list of this process (PEB.Ldr): where the user-mode driver came from is part of the evidence. */
static void print_modules(void) {
    __try {
        PEB* peb = NtCurrentTeb()->ProcessEnvironmentBlock;
        LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
        LIST_ENTRY* e;
        int n = 0;
        for (e = head->Flink; e != head && n < 200; e = e->Flink, ++n) {
            LDR_DATA_TABLE_ENTRY* m = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
            char p[MAX_PATH * 3];
            if (!m->FullDllName.Buffer) continue;
            narrow(m->FullDllName.Buffer, p, sizeof p);
            printf("d3d11: module %p %s\n", m->DllBase, p);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("d3d11: module list unreadable (exception %08lx)\n", (unsigned long)GetExceptionCode());
    }
}

static void print_adapters(void) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    PFN_CREATE_FACTORY1 create = dxgi ? (PFN_CREATE_FACTORY1)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1* factory = NULL;
    UINT i;
    HRESULT hr;
    if (!create) { printf("d3d11: adapters: dxgi.dll CreateDXGIFactory1 unavailable (error %lu)\n", GetLastError()); return; }
    hr = create(&IID_IDXGIFactory1, (void**)&factory);
    if (FAILED(hr) || !factory) { printf("d3d11: adapters: CreateDXGIFactory1 -> %08lx\n", (unsigned long)hr); return; }
    for (i = 0;; ++i) {
        IDXGIAdapter1* a = NULL;
        DXGI_ADAPTER_DESC1 d;
        char name[256];
        if (IDXGIFactory1_EnumAdapters1(factory, i, &a) != S_OK) break;
        memset(&d, 0, sizeof d);
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d))) {
            narrow(d.Description, name, sizeof name);
            printf("d3d11: adapters: #%u \"%s\" vendor=%04x device=%04x flags=%x dedicated_video_memory=%llu\n", i, name,
                   d.VendorId, d.DeviceId, d.Flags, (unsigned long long)d.DedicatedVideoMemory);
        } else {
            printf("d3d11: adapters: #%u GetDesc1 failed\n", i);
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(factory);
}

int main(void) {
    HMODULE d3d11 = NULL, compiler = NULL;
    PFN_D3D11_CREATE_DEVICE create = NULL;
    PFN_COMPILE compile = NULL;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    D3D_FEATURE_LEVEL level = 0;
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0};
    const char* driver = "hardware";
    HRESULT hr, hr_hw;
    IDXGIDevice* dxgi_dev = NULL;
    IDXGIAdapter* adapter = NULL;
    DXGI_ADAPTER_DESC desc;
    char name[256] = "unknown";
    ID3DBlob *vs_blob = NULL, *ps_blob = NULL, *err = NULL;
    ID3D11VertexShader* vs = NULL;
    ID3D11PixelShader* ps = NULL;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *rt = NULL, *staging = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    ID3D11RasterizerState* rs = NULL;
    D3D11_RASTERIZER_DESC rd;
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE map;
    const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const unsigned char *centre, *corner;
    int ok;

    printf("d3d11: start\n");
    fflush(stdout);

    d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) { printf("d3d11: LoadLibrary(d3d11.dll) error %lu\n", GetLastError()); return fail(10, "d3d11.dll did not load"); }
    print_loaded("d3d11.dll");
    create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11CreateDevice");
    if (!create) return fail(11, "d3d11.dll has no D3D11CreateDevice");

    hr = hr_hw = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, wanted, 4, D3D11_SDK_VERSION, &dev, &level, &ctx);
    printf("d3d11: D3D11CreateDevice(hardware) -> %08lx feature_level=0x%04x\n", (unsigned long)hr, (unsigned)level);
    fflush(stdout);
    if (FAILED(hr) || !dev) {
        dev = NULL;
        ctx = NULL;
        level = 0;
        hr = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, wanted, 4, D3D11_SDK_VERSION, &dev, &level, &ctx);
        printf("d3d11: D3D11CreateDevice(warp) -> %08lx feature_level=0x%04x\n", (unsigned long)hr, (unsigned)level);
        fflush(stdout);
        driver = "warp";
        if (FAILED(hr) || !dev) return fail(12, "no hardware and no WARP device");
    }

    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void**)&dxgi_dev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)) && SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
        narrow(desc.Description, name, sizeof name);
        printf("d3d11: adapter detail vendor=%04x device=%04x revision=%u dedicated_video_memory=%llu\n", desc.VendorId,
               desc.DeviceId, desc.Revision, (unsigned long long)desc.DedicatedVideoMemory);
    }
    printf("d3d11: adapter=%s feature_level=0x%04x driver=%s\n", name, (unsigned)ID3D11Device_GetFeatureLevel(dev),
           driver);
    fflush(stdout);
    (void)hr_hw;

    compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) { printf("d3d11: LoadLibrary(d3dcompiler_47.dll) error %lu\n", GetLastError()); return fail(13, "d3dcompiler_47.dll did not load"); }
    compile = (PFN_COMPILE)GetProcAddress(compiler, "D3DCompile");
    if (!compile) return fail(13, "d3dcompiler_47.dll has no D3DCompile");
    hr = compile(kShader, sizeof kShader - 1, "tri.hlsl", NULL, NULL, "vs_main", "vs_4_0", 0, 0, &vs_blob, &err);
    if (FAILED(hr) || !vs_blob) {
        printf("d3d11: D3DCompile(vs) -> %08lx %s\n", (unsigned long)hr,
               err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "");
        return fail(14, "vertex shader did not compile");
    }
    hr = compile(kShader, sizeof kShader - 1, "tri.hlsl", NULL, NULL, "ps_main", "ps_4_0", 0, 0, &ps_blob, &err);
    if (FAILED(hr) || !ps_blob) {
        printf("d3d11: D3DCompile(ps) -> %08lx %s\n", (unsigned long)hr,
               err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "");
        return fail(14, "pixel shader did not compile");
    }
    printf("d3d11: shaders compiled (vs %u bytes, ps %u bytes)\n", (unsigned)ID3D10Blob_GetBufferSize(vs_blob),
           (unsigned)ID3D10Blob_GetBufferSize(ps_blob));
    fflush(stdout);

    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob),
                                         NULL, &vs);
    if (FAILED(hr)) { printf("d3d11: CreateVertexShader -> %08lx\n", (unsigned long)hr); return fail(15, "CreateVertexShader"); }
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob),
                                        NULL, &ps);
    if (FAILED(hr)) { printf("d3d11: CreatePixelShader -> %08lx\n", (unsigned long)hr); return fail(15, "CreatePixelShader"); }

    memset(&td, 0, sizeof td);
    td.Width = td.Height = RT_SIZE;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
    if (FAILED(hr)) { printf("d3d11: CreateTexture2D(render target) -> %08lx\n", (unsigned long)hr); return fail(16, "CreateTexture2D(render target)"); }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging);
    if (FAILED(hr)) { printf("d3d11: CreateTexture2D(staging) -> %08lx\n", (unsigned long)hr); return fail(16, "CreateTexture2D(staging)"); }
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)rt, NULL, &rtv);
    if (FAILED(hr)) { printf("d3d11: CreateRenderTargetView -> %08lx\n", (unsigned long)hr); return fail(16, "CreateRenderTargetView"); }
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(dev, &rd, &rs);
    if (FAILED(hr)) { printf("d3d11: CreateRasterizerState -> %08lx\n", (unsigned long)hr); return fail(16, "CreateRasterizerState"); }
    printf("d3d11: resources created\n");
    fflush(stdout);

    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = vp.Height = (float)RT_SIZE;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, blue);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)rt);
    ID3D11DeviceContext_Flush(ctx);
    printf("d3d11: drawn and copied\n");
    fflush(stdout);

    memset(&map, 0, sizeof map);
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { printf("d3d11: Map -> %08lx\n", (unsigned long)hr); return fail(17, "Map(staging)"); }
    centre = (const unsigned char*)map.pData + (RT_SIZE / 2) * map.RowPitch + (RT_SIZE / 2) * 4;
    corner = (const unsigned char*)map.pData;
    printf("d3d11: centre=%02x%02x%02x%02x corner=%02x%02x%02x%02x (RGBA)\n", centre[0], centre[1], centre[2], centre[3],
           corner[0], corner[1], corner[2], corner[3]);
    ok = centre[0] >= 253 && centre[1] <= 2 && centre[2] <= 2 && corner[0] <= 2 && corner[1] <= 2 && corner[2] >= 253;
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)staging, 0);
    fflush(stdout);

    print_loaded("d3d10warp.dll");
    print_modules();
    print_adapters();
    fflush(stdout);

    if (!ok) return fail(18, "the centre pixel is not the triangle colour or the corner is not the clear colour");
    printf("d3d11: done\n");
    fflush(stdout);
    return 42;
}
