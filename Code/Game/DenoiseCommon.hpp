#pragma once
#include <cstdint>
#include <cstring>

inline float HalfToFloat(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp  = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t bits;
    if (exp == 0)
    {
        if (mant == 0) bits = sign;
        else
        {
            // subnormal: renormalize
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) { mant <<= 1; --exp; }
            bits = sign | (exp << 23) | ((mant & 0x3ff) << 13);
        }
    }
    else if (exp == 31) bits = sign | 0x7f800000 | (mant << 13);
    else                bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Same as the end of raygen: remodulate by albedo (sky keeps the raw light),
// then luminance-preserving Reinhard.
inline void ComposeLDR(float r, float g, float b, const uint8_t albedo[4], uint8_t out[4])
{
    float rgb[3] = { r > 0.f ? r : 0.f, g > 0.f ? g : 0.f, b > 0.f ? b : 0.f };
    if (albedo[3] >= 128)
        for (int c = 0; c < 3; ++c) rgb[c] *= albedo[c] / 255.f;
    float Y = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    if (Y < 1e-4f) Y = 1e-4f;
    float sc = 1.f / (Y + 1.f);
    for (int c = 0; c < 3; ++c)
    {
        float v = rgb[c] * sc;
        out[c] = (uint8_t)((v < 1.f ? v : 1.f) * 255.f + 0.5f);
    }
    out[3] = 255;
}
