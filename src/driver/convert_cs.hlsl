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

int RgbToY(uint3 bgr)
{
    return int((16 << 16) + 16763 * bgr.z + 32904 * bgr.y + 6391 * bgr.x) >> 16;
}

int RgbToU(uint3 bgr)
{
    return int((128 << 16) - 9676 * bgr.z - 18996 * bgr.y + 28672 * bgr.x) >> 16;
}

int RgbToV(uint3 bgr)
{
    return int((128 << 16) + 28672 * bgr.z - 24009 * bgr.y - 4663 * bgr.x) >> 16;
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
    int u = AdjustChroma((RgbToU(first) + RgbToU(second)) / 2);
    int v = AdjustChroma((RgbToV(first) + RgbToV(second)) / 2);

    // Little endian byte order in the dword gives U Y0 V Y1 in memory.
    uint packed = uint(u) | (uint(y0) << 8) | (uint(v) << 16) | (uint(y1) << 24);
    Output.Store(row * RowBytes + pair * 4, packed);
}
