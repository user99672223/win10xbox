// blit.h - the D3D11 half of the presenter: CPU BGRA frame -> GPU texture -> any
// BGRA texture (a swap chain back buffer on the console, a render target in the CI test).
//
// No WinRT and no CoreWindow here, so the same code runs in host_uwp_blittest
// (selftest/blit_test.cpp) on a CI runner with a WARP device: that test proves the
// upload + copy path; only the CoreWindow swap chain (presenter.cpp) is left to the console.
//
// Threading: Init/Upload/CopyTo use the device's immediate context, which is not
// thread-safe; the presenter calls them only from the UI thread.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace w10x_uwp
{
    // D3D11CreateDevice with BGRA support (what a DXGI swap chain of B8G8R8A8_UNORM needs),
    // feature levels 11_1 down to 10_0. `type` is D3D_DRIVER_TYPE_HARDWARE or _WARP.
    HRESULT CreateBlitDevice(D3D_DRIVER_TYPE type, Microsoft::WRL::ComPtr<ID3D11Device>* device,
                             Microsoft::WRL::ComPtr<ID3D11DeviceContext>* context, D3D_FEATURE_LEVEL* level);

    class FrameBlitter
    {
    public:
        // Creates the screen texture: width x height, B8G8R8A8_UNORM, default usage, filled
        // from `initial` (width * 4 bytes per row; nullptr = black).
        HRESULT Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height,
                     const std::uint32_t* initial);

        // Uploads a whole top-down BGRA frame of Width() x Height() pixels (UpdateSubresource).
        void Upload(const std::uint32_t* bgra, int stride_bytes);

        // Copies the screen texture into `dst` (a BGRA texture of any size) at (0, 0): the
        // overlapping region only, so a size mismatch crops or leaves the rest untouched.
        void CopyTo(ID3D11Texture2D* dst);

        int Width() const { return width_; }
        int Height() const { return height_; }
        bool Ready() const { return texture_ != nullptr; }

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
        int width_ = 0;
        int height_ = 0;
    };
}  // namespace w10x_uwp
