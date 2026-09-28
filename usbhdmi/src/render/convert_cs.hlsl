// SPDX-License-Identifier: GPL-2.0-only
//
// XRGB8888 to UYVY 4:2:2 on the GPU. One thread produces one UYVY pair,
// which is four output bytes from two source pixels.
//
// The arithmetic here is not "equivalent" to the CPU path, it is identical,
// down to the shift and the order of operations. That is a hard requirement
// rather than tidiness: the driver picks between this path and the CPU one
// based on how big the update is, so the same pixel converted twice through
// different paths must produce the same byte. A one bit disagreement shows up
// on small text as a shimmer, because a region redrawn at slightly different
// sizes flips between the two paths.
//
// Source pixels are read as floats because a UNORM texture cannot be viewed
// as integers without changing its format family. Multiplying by 255 and
// rounding recovers the exact original byte: the representation error of
// v/255 in single precision is around 1e-7, nowhere near half a unit.

cbuffer Params : register(b0) {
  uint2 gSize;        // width and height of the region, in pixels
  int   gLumaGain;    // brightness, fixed point over 256
  int   gChromaGain;  // contrast, fixed point over 256
};

Texture2D<float4> gSource : register(t0);
RWStructuredBuffer<uint> gOutput : register(u0);

// Must match kCoeff* in render/convert.h.
static const int kYr = 8382, kYg = 16452, kYb = 3196;
static const int kUr = -4838, kUg = -9498, kUb = 14336;
static const int kVr = 14336, kVg = -12005, kVb = -2332;

int3 LoadRgb(uint x, uint y) {
  float4 texel = gSource.Load(int3(int(x), int(y), 0));
  return int3(round(texel.r * 255.0f), round(texel.g * 255.0f),
              round(texel.b * 255.0f));
}

int Luma(int3 c) { return 16 + ((kYr * c.r + kYg * c.g + kYb * c.b) >> 15); }
int ChromaU(int3 c) {
  return 128 + ((kUr * c.r + kUg * c.g + kUb * c.b) >> 15);
}
int ChromaV(int3 c) {
  return 128 + ((kVr * c.r + kVg * c.g + kVb * c.b) >> 15);
}

int AdjustLuma(int y) {
  int scaled = (((y - 16) * gLumaGain) >> 8) + 16;
  return clamp(scaled, 16, 235);
}

int AdjustChroma(int c) {
  int scaled = (((c - 128) * gChromaGain) >> 8) + 128;
  return clamp(scaled, 16, 240);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint pairs = gSize.x / 2;
  if (id.x >= pairs || id.y >= gSize.y) {
    return;
  }

  uint x = id.x * 2;
  int3 left = LoadRgb(x, id.y);
  int3 right = LoadRgb(x + 1, id.y);

  int y0 = Luma(left);
  int y1 = Luma(right);
  int u = (ChromaU(left) + ChromaU(right)) >> 1;
  int v = (ChromaV(left) + ChromaV(right)) >> 1;

  // Identity gains are the common case and must leave the bytes untouched,
  // so the branch is on the values rather than always running the scaling.
  if (gLumaGain != 256 || gChromaGain != 256) {
    y0 = AdjustLuma(y0);
    y1 = AdjustLuma(y1);
    u = AdjustChroma(u);
    v = AdjustChroma(v);
  }

  // Little endian packing, so byte 0 of the word is U, then Y0, V, Y1.
  uint packed = (uint(u) & 0xFF) | ((uint(y0) & 0xFF) << 8) |
                ((uint(v) & 0xFF) << 16) | ((uint(y1) & 0xFF) << 24);
  gOutput[id.y * pairs + id.x] = packed;
}
