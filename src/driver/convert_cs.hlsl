// SPDX-License-Identifier: GPL-2.0-only
//
// RGB to UYVY 4:2:2 conversion on the GPU.
//
// This exists because the CPU path spends most of its time *reading* the
// acquired frame: mapped GPU memory is far slower than ordinary RAM, and at
// 1080p the source is 8.3 MB. Converting on the GPU means the only thing that
// crosses the bus is the 4.1 MB of UYVY we actually transmit, which halves the
// readback and removes the conversion from the CPU entirely.
//
// Each thread produces one UYVY quad, that is two horizontally adjacent
// pixels, so a 8x8 group covers 16x8 pixels.
//
// The integer coefficients match the scalar reference in ms912x_convert.cpp
// exactly, so the two paths agree bit for bit.

Texture2D<float4> Source : register(t0);
RWByteAddressBuffer Output : register(u0);

cbuffer Params : register(b0)
{
    uint2 Origin;      // top left of the damage rect, in pixels
    uint2 Size;        // damage rect size, in pixels
    uint  RowBytes;    // bytes per output row, Size.x * 2
    uint  LumaGain;    // 8.8 fixed point, 256 means unchanged
    uint  ChromaGain;  // 8.8 fixed point, 256 means unchanged
    uint  Padding;
};

// UNORM8 round trips exactly through a float multiply by 255.
uint3 LoadPixel(uint x, uint y)
{
    float4 texel = Source.Load(int3(int(x), int(y), 0));
    return uint3(round(texel.b * 255.0f),
                 round(texel.g * 255.0f),
                 round(texel.r * 255.0f));  // b, g, r
}

// 15 bit fixed point, matching ms912x_convert.cpp exactly. The CPU SIMD path
// cannot use the usual 16 bit constants because 32904 does not fit in a signed
// 16 bit multiply lane, and every path has to agree bit for bit: a region
// converted by different paths at different times would otherwise alternate
// between two values, which shows up as shimmering text.
static const int kYr = 8382, kYg = 16452, kYb = 3196;
static const int kUr = -4838, kUg = -9498, kUb = 14336;
static const int kVr = 14336, kVg = -12005, kVb = -2332;

int RgbToY(uint3 bgr)
{
    return 16 + ((kYr * int(bgr.z) + kYg * int(bgr.y) + kYb * int(bgr.x)) >> 15);
}

int RgbToU(uint3 bgr)
{
    return 128 + ((kUr * int(bgr.z) + kUg * int(bgr.y) + kUb * int(bgr.x)) >> 15);
}

int RgbToV(uint3 bgr)
{
    return 128 + ((kVr * int(bgr.z) + kVg * int(bgr.y) + kVb * int(bgr.x)) >> 15);
}

// Brightness scales luma above its black point, contrast scales chroma about
// neutral. Both are skipped when the gains are unchanged.
int AdjustLuma(int y)
{
    if (LumaGain == 256)
    {
        return clamp(y, 0, 255);
    }
    int scaled = (((y - 16) * int(LumaGain)) >> 8) + 16;
    return clamp(scaled, 16, 235);
}

int AdjustChroma(int c)
{
    if (ChromaGain == 256)
    {
        return clamp(c, 0, 255);
    }
    int scaled = (((c - 128) * int(ChromaGain)) >> 8) + 128;
    return clamp(scaled, 16, 240);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint pair = id.x;          // which pixel pair in the row
    uint row = id.y;
    if (pair * 2 >= Size.x || row >= Size.y)
    {
        return;
    }

    uint sx = Origin.x + pair * 2;
    uint sy = Origin.y + row;

    uint3 first = LoadPixel(sx, sy);
    uint3 second = LoadPixel(sx + 1, sy);

    int y0 = AdjustLuma(RgbToY(first));
    int y1 = AdjustLuma(RgbToY(second));
    // U and V are averaged across the pair, matching the CPU path.
    // Unsigned halving, matching the CPU path and avoiding a signed divide.
    int u = AdjustChroma(int((uint(RgbToU(first)) + uint(RgbToU(second))) >> 1));
    int v = AdjustChroma(int((uint(RgbToV(first)) + uint(RgbToV(second))) >> 1));

    // Little endian byte order in the dword gives U Y0 V Y1 in memory.
    uint packed = uint(u) | (uint(y0) << 8) | (uint(v) << 16) | (uint(y1) << 24);
    Output.Store(row * RowBytes + pair * 4, packed);
}
