// blit.cpp - see blit.h.
#include "blit.h"

namespace w10x_uwp
{
HRESULT CreateBlitDevice(D3D_DRIVER_TYPE type, Microsoft::WRL::ComPtr<ID3D11Device>* device,
                         Microsoft::WRL::ComPtr<ID3D11DeviceContext>* context, D3D_FEATURE_LEVEL* level)
{
    static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got = static_cast<D3D_FEATURE_LEVEL>(0);
    const HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels,
                                         static_cast<UINT>(sizeof kLevels / sizeof kLevels[0]), D3D11_SDK_VERSION,
                                         device->ReleaseAndGetAddressOf(), &got, context->ReleaseAndGetAddressOf());
    if (level != nullptr) *level = got;
    return hr;
}

HRESULT FrameBlitter::Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height,
                           const std::uint32_t* initial)
{
    texture_.Reset();
    context_ = context;
    width_ = width;
    height_ = height;
    if (device == nullptr || context == nullptr || width <= 0 || height <= 0) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;   // a default texture needs a bind flag; it is only a copy source

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = initial;
    init.SysMemPitch = static_cast<UINT>(width) * 4u;
    return device->CreateTexture2D(&d, initial != nullptr ? &init : nullptr, texture_.ReleaseAndGetAddressOf());
}

void FrameBlitter::Upload(const std::uint32_t* bgra, int stride_bytes)
{
    if (!texture_ || bgra == nullptr) return;
    context_->UpdateSubresource(texture_.Get(), 0, nullptr, bgra, static_cast<UINT>(stride_bytes), 0);
}

void FrameBlitter::CopyTo(ID3D11Texture2D* dst)
{
    if (!texture_ || dst == nullptr) return;
    D3D11_TEXTURE2D_DESC dd = {};
    dst->GetDesc(&dd);
    const UINT w = dd.Width < static_cast<UINT>(width_) ? dd.Width : static_cast<UINT>(width_);
    const UINT h = dd.Height < static_cast<UINT>(height_) ? dd.Height : static_cast<UINT>(height_);
    if (w == 0 || h == 0) return;
    if (w == dd.Width && h == dd.Height && w == static_cast<UINT>(width_) && h == static_cast<UINT>(height_))
    {
        context_->CopyResource(dst, texture_.Get());
        return;
    }
    D3D11_BOX box = {};
    box.right = w;
    box.bottom = h;
    box.back = 1;
    context_->CopySubresourceRegion(dst, 0, 0, 0, 0, texture_.Get(), 0, &box);
}
}  // namespace w10x_uwp
