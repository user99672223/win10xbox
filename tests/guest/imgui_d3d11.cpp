// guest_imgui_d3d11: Dear ImGui's own example_win32_directx11 (examples/example_win32_directx11/main.cpp of a
// Dear ImGui release, fetched by CI, NOT vendored) as a guest of route B + Direct3D 11 (docs/GUI.md, docs/DECISIONS.md
// "G1"). The example is compiled UNMODIFIED: this file only wraps it, so it can end by itself after a fixed number
// of frames and say on stdout what it did:
//
//   * every IDXGISwapChain::Present(sync, flags) the example makes becomes `Present(sync, flags) + Tick(flags)`
//     (a macro defined after <d3d11.h>, so the interface declaration is untouched; '+' keeps the HRESULT and binds
//     tighter than the '==' the example compares it with). Tick counts the real presents (flags without
//     DXGI_PRESENT_TEST), prints the first and the last, and after the last one posts WM_QUIT, which the example's
//     own PeekMessage loop turns into its normal shutdown (CleanupDeviceD3D, DestroyWindow, return 0).
//   * the frame count is WIN10XBOX_IMGUI_FRAMES from the environment (default 60, one second at vsync).
//   * D3D11CreateDeviceAndSwapChain is routed through a logging forwarder, so a failure (after which the example
//     just returns 1) says which driver type failed with which HRESULT.
//
// Output (the integration test matches it):
//   imgui: D3D11CreateDeviceAndSwapChain(driver type 1) -> 00000000, feature level 0xb000
//   imgui: adapter "<DXGI adapter description>" vendor=xxxx device=xxxx feature level=0xb000
//   imgui: frame 1 presented
//   imgui: frame 60 presented
//   imgui: done frames=60
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>

namespace w10x {
HRESULT Tick(UINT flags);
HRESULT WINAPI CreateDeviceAndSwapChain(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                        UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
                                        D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
}
#define Present(sync, flags) Present(sync, flags) + w10x::Tick(flags)
#define D3D11CreateDeviceAndSwapChain w10x::CreateDeviceAndSwapChain

#include "examples/example_win32_directx11/main.cpp"

#undef Present
#undef D3D11CreateDeviceAndSwapChain

namespace w10x {
namespace {
int g_frames;
int g_target;

void DescribeAdapter() {
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    if (g_pd3dDevice && SUCCEEDED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
        printf("imgui: adapter \"%ls\" vendor=%04x device=%04x feature level=0x%x\n", desc.Description, desc.VendorId,
               desc.DeviceId, g_pd3dDevice->GetFeatureLevel());
    else
        printf("imgui: adapter unknown (device %p)\n", static_cast<void*>(g_pd3dDevice));
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
}
}  // namespace

HRESULT WINAPI CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                        const D3D_FEATURE_LEVEL* levels, UINT nlevels, UINT sdk,
                                        const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** swap, ID3D11Device** dev,
                                        D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    printf("imgui: D3D11CreateDeviceAndSwapChain(driver type %d) ...\n", static_cast<int>(type));
    fflush(stdout);
    const HRESULT hr = ::D3D11CreateDeviceAndSwapChain(adapter, type, sw, flags, levels, nlevels, sdk, sd, swap, dev,
                                                       level, ctx);
    printf("imgui: D3D11CreateDeviceAndSwapChain(driver type %d) -> %08lx, feature level 0x%x\n",
           static_cast<int>(type), static_cast<unsigned long>(hr), level ? static_cast<unsigned>(*level) : 0u);
    fflush(stdout);
    return hr;
}

// Runs right next to the Present call; which of the two runs first is unspecified and does not matter.
HRESULT Tick(UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return S_OK;
    if (!g_target) {
        char v[16] = {};
        g_target = GetEnvironmentVariableA("WIN10XBOX_IMGUI_FRAMES", v, sizeof v) ? atoi(v) : 0;
        if (g_target <= 0) g_target = 60;
    }
    ++g_frames;
    if (g_frames == 1) DescribeAdapter();
    if (g_frames == 1 || g_frames == g_target) {
        printf("imgui: frame %d presented\n", g_frames);
        fflush(stdout);
    }
    if (g_frames == g_target) {
        printf("imgui: done frames=%d\n", g_frames);
        fflush(stdout);
        ::PostQuitMessage(0);
    }
    return S_OK;
}
}  // namespace w10x
