// frame_scale.h - CPU scaling of a composed frame (top-down BGRA, tightly packed) to another size. The host-uwp
// presenter uses it when the guest's screen (launch.json "guest_display") differs from the console's output mode; host-win32
// uses it for WIN10XBOX_OUTPUT_MODE so the same path is exercised in the integration tests. Linear filter, nearest when
// both axes are an integer ratio (either way round).
#pragma once

#include <cstdint>

namespace win10xbox {

inline bool FrameScaleIsNearest(int sw, int sh, int dw, int dh) {
    const bool x = (dw % sw == 0) || (sw % dw == 0);
    const bool y = (dh % sh == 0) || (sh % dh == 0);
    return x && y;
}

// src: sw x sh pixels, dst: dw x dh pixels (all > 0). Returns true when the nearest filter was used.
inline bool ScaleFrame(const std::uint32_t* src, int sw, int sh, std::uint32_t* dst, int dw, int dh) {
    if (sw == dw && sh == dh) {
        for (long long i = 0; i < static_cast<long long>(sw) * sh; ++i) dst[i] = src[i];
        return true;
    }
    if (FrameScaleIsNearest(sw, sh, dw, dh)) {
        for (int y = 0; y < dh; ++y) {
            const std::uint32_t* row = src + static_cast<long long>(static_cast<long long>(y) * sh / dh) * sw;
            for (int x = 0; x < dw; ++x) dst[static_cast<long long>(y) * dw + x] = row[static_cast<long long>(x) * sw / dw];
        }
        return true;
    }
    for (int y = 0; y < dh; ++y) {
        // pixel centres: sy = (y + 0.5) * sh / dh - 0.5, in 1/256ths
        long long fy = ((2LL * y + 1) * sh * 256) / (2LL * dh) - 128;
        if (fy < 0) fy = 0;
        int y0 = static_cast<int>(fy >> 8);
        const int wy = static_cast<int>(fy & 255);
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        if (y0 >= sh) y0 = sh - 1;
        const std::uint32_t* r0 = src + static_cast<long long>(y0) * sw;
        const std::uint32_t* r1 = src + static_cast<long long>(y1) * sw;
        for (int x = 0; x < dw; ++x) {
            long long fx = ((2LL * x + 1) * sw * 256) / (2LL * dw) - 128;
            if (fx < 0) fx = 0;
            int x0 = static_cast<int>(fx >> 8);
            const int wx = static_cast<int>(fx & 255);
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            if (x0 >= sw) x0 = sw - 1;
            std::uint32_t out = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                const int a = static_cast<int>((r0[x0] >> shift) & 255), b = static_cast<int>((r0[x1] >> shift) & 255);
                const int c = static_cast<int>((r1[x0] >> shift) & 255), d = static_cast<int>((r1[x1] >> shift) & 255);
                const int top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + d * wx;
                out |= static_cast<std::uint32_t>((top * (256 - wy) + bot * wy + 32768) >> 16) << shift;
            }
            dst[static_cast<long long>(y) * dw + x] = out;
        }
    }
    return false;
}

}  // namespace win10xbox
