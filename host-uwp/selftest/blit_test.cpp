// blit_test.cpp - off-device proof of the presenter's frame path (blit.h): the exact upload +
// copy routine the console runs on its swap chain back buffer, here on a WARP device and a
// render-target texture, read back through a staging texture. A console program run by ctest
// on the CI runner (no CoreWindow there: only CreateSwapChainForCoreWindow is left untested).
// Exit code 0 = all checks passed.
#include "../blit.h"

#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace w10x_uwp;

namespace
{
    int g_checks = 0;
    int g_failed = 0;

    void Check(bool ok, const char* what, int line)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::printf("FAIL  blit_test.cpp:%d  %s\n", line, what);
        }
    }
#define CHECK(cond) Check((cond), #cond, __LINE__)

    constexpr std::uint32_t kSentinel = 0xFF123456u;

    // A frame whose every pixel encodes its position: B = x, G = y, R = 0xA5 ^ (x + y), A = 0xFF.
    std::uint32_t Pattern(int x, int y)
    {
        return 0xFF000000u | (static_cast<std::uint32_t>((0xA5 ^ (x + y)) & 0xFF) << 16) |
               (static_cast<std::uint32_t>(y & 0xFF) << 8) | static_cast<std::uint32_t>(x & 0xFF);
    }

    ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* dev, int w, int h, D3D11_USAGE usage, UINT bind, UINT cpu,
                                        const std::uint32_t* init)
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = static_cast<UINT>(w);
        d.Height = static_cast<UINT>(h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = usage;
        d.BindFlags = bind;
        d.CPUAccessFlags = cpu;
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = init;
        sd.SysMemPitch = static_cast<UINT>(w) * 4u;
        ComPtr<ID3D11Texture2D> t;
        const HRESULT hr = dev->CreateTexture2D(&d, init ? &sd : nullptr, &t);
        if (FAILED(hr)) std::printf("CreateTexture2D(%dx%d) failed: 0x%08lx\n", w, h, static_cast<unsigned long>(hr));
        return t;
    }

    // Reads `tex` back through a staging texture: w*h pixels, top-down.
    std::vector<std::uint32_t> ReadBack(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int w, int h)
    {
        std::vector<std::uint32_t> out;
        ComPtr<ID3D11Texture2D> staging = MakeTexture(dev, w, h, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, nullptr);
        if (!staging) return out;
        ctx->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE m = {};
        const HRESULT hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr))
        {
            std::printf("Map failed: 0x%08lx\n", static_cast<unsigned long>(hr));
            return out;
        }
        out.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (int y = 0; y < h; ++y)
        {
            const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(m.pData) +
                                                                    static_cast<std::size_t>(y) * m.RowPitch);
            for (int x = 0; x < w; ++x) out[static_cast<std::size_t>(y) * w + x] = row[x];
        }
        ctx->Unmap(staging.Get(), 0);
        return out;
    }

    // One case: a frame of fw x fh (rows `pad` pixels wider than the frame, to exercise the
    // stride) uploaded and copied into a dw x dh render target that starts as kSentinel.
    void RunCase(ID3D11Device* dev, ID3D11DeviceContext* ctx, int fw, int fh, int pad, int dw, int dh)
    {
        std::printf("case: frame %dx%d (stride %d px) -> target %dx%d\n", fw, fh, fw + pad, dw, dh);
        const int stride_px = fw + pad;
        std::vector<std::uint32_t> frame(static_cast<std::size_t>(stride_px) * fh, 0xDEADBEEFu);
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) frame[static_cast<std::size_t>(y) * stride_px + x] = Pattern(x, y);

        FrameBlitter blit;
        std::vector<std::uint32_t> grey(static_cast<std::size_t>(fw) * fh, 0xFF202020u);
        const HRESULT hr = blit.Init(dev, ctx, fw, fh, grey.data());
        CHECK(SUCCEEDED(hr));
        CHECK(blit.Ready() && blit.Width() == fw && blit.Height() == fh);
        if (FAILED(hr)) return;

        std::vector<std::uint32_t> sentinel(static_cast<std::size_t>(dw) * dh, kSentinel);
        ComPtr<ID3D11Texture2D> target =
            MakeTexture(dev, dw, dh, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, sentinel.data());
        CHECK(target != nullptr);
        if (!target) return;

        // Before any upload the texture holds its initial contents.
        blit.CopyTo(target.Get());
        std::vector<std::uint32_t> px = ReadBack(dev, ctx, target.Get(), dw, dh);
        CHECK(!px.empty());
        if (px.empty()) return;
        CHECK(px[0] == 0xFF202020u);

        blit.Upload(frame.data(), stride_px * 4);
        blit.CopyTo(target.Get());
        px = ReadBack(dev, ctx, target.Get(), dw, dh);
        CHECK(!px.empty());
        if (px.empty()) return;

        int bad_in = 0, bad_out = 0;
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
            {
                const std::uint32_t got = px[static_cast<std::size_t>(y) * dw + x];
                const bool inside = x < fw && y < fh;
                const std::uint32_t want = inside ? Pattern(x, y) : kSentinel;
                if (got != want)
                {
                    if ((inside ? bad_in : bad_out) < 3)
                        std::printf("  pixel (%d,%d): got %08x want %08x (%s the overlap)\n", x, y, got, want,
                                    inside ? "inside" : "outside");
                    ++(inside ? bad_in : bad_out);
                }
            }
        Check(bad_in == 0, "every pixel of the overlapping region is the frame's", __LINE__);
        Check(bad_out == 0, "pixels outside the overlap are untouched", __LINE__);
    }
}  // namespace

int main()
{
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = static_cast<D3D_FEATURE_LEVEL>(0);
    const HRESULT hr = CreateBlitDevice(D3D_DRIVER_TYPE_WARP, &dev, &ctx, &fl);
    std::printf("WARP device: hr=0x%08lx feature level 0x%x\n", static_cast<unsigned long>(hr), static_cast<unsigned>(fl));
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr))
    {
        std::printf("blit_test: no WARP device, %d/%d checks failed\n", g_failed, g_checks);
        return 1;
    }

    RunCase(dev.Get(), ctx.Get(), 64, 64, 0, 64, 64);    // same size: CopyResource
    RunCase(dev.Get(), ctx.Get(), 64, 64, 7, 64, 64);    // padded source rows
    RunCase(dev.Get(), ctx.Get(), 80, 40, 3, 64, 64);    // wider and shorter than the target: cropped / partial
    RunCase(dev.Get(), ctx.Get(), 32, 48, 0, 64, 64);    // smaller than the target

    std::printf("blit_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
