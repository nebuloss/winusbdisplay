/* SPDX-License-Identifier: GPL-2.0-only */

#include "ms912x_convert.h"

#include <climits>
#include <cstring>

namespace ms912x {
namespace {

inline unsigned RgbToY(unsigned r, unsigned g, unsigned b) {
  return ((16u << 16) + 16763u * r + 32904u * g + 6391u * b) >> 16;
}

inline unsigned RgbToU(unsigned r, unsigned g, unsigned b) {
  return ((128u << 16) - 9676u * r - 18996u * g + 28672u * b) >> 16;
}

inline unsigned RgbToV(unsigned r, unsigned g, unsigned b) {
  return ((128u << 16) + 28672u * r - 24009u * g - 4663u * b) >> 16;
}

inline int AlignDown2(int value) { return value & ~1; }
inline int AlignUp2(int value) { return (value + 1) & ~1; }

}  // namespace

Rect EmptyRect() {
  Rect rect;
  rect.x1 = INT_MAX;
  rect.y1 = INT_MAX;
  rect.x2 = 0;
  rect.y2 = 0;
  return rect;
}

Rect MergeRects(const Rect& a, const Rect& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  Rect out;
  out.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
  out.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
  out.x2 = a.x2 > b.x2 ? a.x2 : b.x2;
  out.y2 = a.y2 > b.y2 ? a.y2 : b.y2;
  return out;
}

Rect AlignDamageRect(const Rect& rect, int fb_width, int fb_height) {
  Rect out = rect;
  if (out.x1 < 0) {
    out.x1 = 0;
  }
  if (out.y1 < 0) {
    out.y1 = 0;
  }
  if (out.x2 > fb_width) {
    out.x2 = fb_width;
  }
  if (out.y2 > fb_height) {
    out.y2 = fb_height;
  }
  if (out.empty()) {
    out.x1 = out.y1 = out.x2 = out.y2 = 0;
    return out;
  }
  out.x1 = AlignDown2(out.x1);
  out.x2 = AlignUp2(out.x2);
  if (out.x2 > fb_width) {
    out.x2 = AlignDown2(fb_width);
  }
  if (out.x2 <= out.x1) {
    out.x1 = out.y1 = out.x2 = out.y2 = 0;
  }
  return out;
}

void ConvertRowXrgbToUyvy(uint8_t* dst, const uint8_t* src, int width) {
  for (int i = 0; i + 1 < width; i += 2) {
    const uint8_t* p1 = src + static_cast<size_t>(i) * 4;
    const uint8_t* p2 = p1 + 4;

    /* XRGB8888 is little endian in memory: B, G, R, X. */
    unsigned b1 = p1[0], g1 = p1[1], r1 = p1[2];
    unsigned b2 = p2[0], g2 = p2[1], r2 = p2[2];

    unsigned y1 = RgbToY(r1, g1, b1);
    unsigned y2 = RgbToY(r2, g2, b2);
    unsigned u = (RgbToU(r1, g1, b1) + RgbToU(r2, g2, b2)) / 2;
    unsigned v = (RgbToV(r1, g1, b1) + RgbToV(r2, g2, b2)) / 2;

    *dst++ = static_cast<uint8_t>(u);
    *dst++ = static_cast<uint8_t>(y1);
    *dst++ = static_cast<uint8_t>(v);
    *dst++ = static_cast<uint8_t>(y2);
  }
}

size_t FrameRect(uint8_t* dst, size_t dst_capacity, const uint8_t* src,
                 size_t src_stride, int fb_width, int fb_height,
                 const Rect& rect) {
  if (rect.empty() || rect.x1 < 0 || rect.y1 < 0 || rect.x2 > fb_width ||
      rect.y2 > fb_height || (rect.x1 & 1) || (rect.width() & 1)) {
    return 0;
  }
  const size_t needed = TransferLength(rect);
  if (dst_capacity < needed) {
    return 0;
  }

  FrameUpdateHeader header;
  PutBe16(header.marker_be, kFrameMarker);
  PutBe24(header.position,
          ((static_cast<uint32_t>(rect.x1) & 0xFFF) << 12) |
              (static_cast<uint32_t>(rect.y1) & 0xFFF));
  PutBe24(header.dimensions,
          ((static_cast<uint32_t>(rect.width()) & 0xFFF) << 12) |
              (static_cast<uint32_t>(rect.height()) & 0xFFF));
  memcpy(dst, &header, sizeof(header));

  uint8_t* out = dst + sizeof(header);
  const size_t row_bytes = static_cast<size_t>(rect.width()) * 2;
  for (int y = rect.y1; y < rect.y2; ++y) {
    const uint8_t* row =
        src + static_cast<size_t>(y) * src_stride +
        static_cast<size_t>(rect.x1) * 4;
    ConvertRowXrgbToUyvy(out, row, rect.width());
    out += row_bytes;
  }

  memcpy(out, kFrameFooter, kFrameFooterSize);
  return needed;
}

void FillSolid(uint8_t* dst, size_t stride, int width, int height, uint8_t r,
               uint8_t g, uint8_t b) {
  for (int y = 0; y < height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      row[x * 4 + 0] = b;
      row[x * 4 + 1] = g;
      row[x * 4 + 2] = r;
      row[x * 4 + 3] = 0xFF;
    }
  }
}

void FillColourBars(uint8_t* dst, size_t stride, int width, int height) {
  static const uint8_t kBars[8][3] = {
      {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
      {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
  };
  for (int y = 0; y < height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      int bar = (x * 8) / (width > 0 ? width : 1);
      if (bar > 7) {
        bar = 7;
      }
      row[x * 4 + 0] = kBars[bar][2];
      row[x * 4 + 1] = kBars[bar][1];
      row[x * 4 + 2] = kBars[bar][0];
      row[x * 4 + 3] = 0xFF;
    }
  }
}

}  // namespace ms912x
