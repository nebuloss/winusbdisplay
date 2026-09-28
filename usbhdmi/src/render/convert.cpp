/* SPDX-License-Identifier: GPL-2.0-only */

#include "convert.h"

#include <emmintrin.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <cstring>

namespace usbhdmi {
namespace {

inline unsigned RgbToY(unsigned r, unsigned g, unsigned b) {
  return static_cast<unsigned>(
      16 + ((kCoeffYr * static_cast<int>(r) + kCoeffYg * static_cast<int>(g) +
             kCoeffYb * static_cast<int>(b)) >> 15));
}

inline unsigned RgbToU(unsigned r, unsigned g, unsigned b) {
  return static_cast<unsigned>(
      128 + ((kCoeffUr * static_cast<int>(r) + kCoeffUg * static_cast<int>(g) +
              kCoeffUb * static_cast<int>(b)) >> 15));
}

inline unsigned RgbToV(unsigned r, unsigned g, unsigned b) {
  return static_cast<unsigned>(
      128 + ((kCoeffVr * static_cast<int>(r) + kCoeffVg * static_cast<int>(g) +
              kCoeffVb * static_cast<int>(b)) >> 15));
}

/* Horizontally adds the two dot products in a madd result, leaving them in
 * lanes 0 and 1.
 *
 * Staying in registers is the whole point. An earlier version of this kernel
 * wrote the lanes to a stack array and read them back, six times per four
 * pixels, and the store forwarding stall that produced dominated the entire
 * conversion: fixing it alone roughly doubled throughput. */
inline __m128i PairDots(__m128i madd_result) {
  const __m128i summed = _mm_add_epi32(
      madd_result, _mm_shuffle_epi32(madd_result, _MM_SHUFFLE(3, 3, 1, 1)));
  return _mm_shuffle_epi32(summed, _MM_SHUFFLE(3, 1, 2, 0));
}

inline __m128i QuadDots(__m128i lo, __m128i hi, __m128i coeff) {
  return _mm_unpacklo_epi64(PairDots(_mm_madd_epi16(lo, coeff)),
                            PairDots(_mm_madd_epi16(hi, coeff)));
}

inline __m128i ScaleAndBias(__m128i value, int bias) {
  return _mm_add_epi32(_mm_srai_epi32(value, 15), _mm_set1_epi32(bias));
}

/* A persistent worker pool. Threads are created once; spawning them per frame
 * would cost more than the work being spread. */
class RowPool {
 public:
  static RowPool& Instance() {
    static RowPool pool;
    return pool;
  }

  unsigned workers() const {
    return static_cast<unsigned>(threads_.size()) + 1;
  }

  void Resize(unsigned total) {
    if (total < 1) {
      total = 1;
    }
    Shutdown();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
    for (unsigned i = 0; i + 1 < total; ++i) {
      threads_.emplace_back(&RowPool::Worker, this);
    }
  }

  /* Runs body(first, last) over [0, rows) split across the pool, including
   * the calling thread, and returns once every chunk is finished. */
  void Run(int rows, const std::function<void(int, int)>& body) {
    const unsigned total = workers();
    if (total <= 1 || rows < 64) {
      body(0, rows);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      body_ = &body;
      rows_ = rows;
      chunks_ = total;
      next_chunk_ = 1; /* chunk 0 belongs to the caller */
      outstanding_ = total - 1;
      ++generation_;
    }
    work_cv_.notify_all();

    RunChunk(0, rows, total, body);

    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock, [this] { return outstanding_ == 0; });
    body_ = nullptr;
  }

 private:
  RowPool() {
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
      hardware = 2;
    }
    /* Leave headroom. This runs on the compositor's processing thread while
     * the USB sender and the rest of the machine are also busy. */
    Resize(hardware > 4 ? 4 : hardware);
  }

  ~RowPool() { Shutdown(); }

  static void RunChunk(unsigned index, int rows, unsigned chunks,
                       const std::function<void(int, int)>& body) {
    const int first =
        static_cast<int>((static_cast<long long>(rows) * index) / chunks);
    const int last =
        static_cast<int>((static_cast<long long>(rows) * (index + 1)) / chunks);
    if (last > first) {
      body(first, last);
    }
  }

  void Worker() {
    unsigned seen = 0;
    for (;;) {
      unsigned index = 0;
      int rows = 0;
      unsigned chunks = 0;
      const std::function<void(int, int)>* body = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        work_cv_.wait(lock, [&] { return !running_ || generation_ != seen; });
        if (!running_) {
          return;
        }
        seen = generation_;
        if (next_chunk_ >= chunks_) {
          continue;
        }
        index = next_chunk_++;
        rows = rows_;
        chunks = chunks_;
        body = body_;
      }

      if (body) {
        RunChunk(index, rows, chunks, *body);
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        --outstanding_;
      }
      done_cv_.notify_one();
    }
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ && threads_.empty()) {
        return;
      }
      running_ = false;
      ++generation_;
    }
    work_cv_.notify_all();
    for (std::thread& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  bool running_ = false;
  unsigned generation_ = 0;
  unsigned chunks_ = 0;
  unsigned next_chunk_ = 0;
  unsigned outstanding_ = 0;
  int rows_ = 0;
  const std::function<void(int, int)>* body_ = nullptr;
};

}  // namespace

void SetConversionThreads(unsigned threads) {
  RowPool::Instance().Resize(threads);
}

unsigned ConversionThreads() { return RowPool::Instance().workers(); }

void ConvertRowScalar(uint8_t* dst, const uint8_t* src, int width) {
  for (int i = 0; i + 1 < width; i += 2) {
    const uint8_t* p1 = src + static_cast<size_t>(i) * 4;
    const uint8_t* p2 = p1 + 4;

    /* XRGB8888 is B, G, R, X in memory. */
    const unsigned b1 = p1[0], g1 = p1[1], r1 = p1[2];
    const unsigned b2 = p2[0], g2 = p2[1], r2 = p2[2];

    const unsigned y1 = RgbToY(r1, g1, b1);
    const unsigned y2 = RgbToY(r2, g2, b2);
    const unsigned u = (RgbToU(r1, g1, b1) + RgbToU(r2, g2, b2)) / 2;
    const unsigned v = (RgbToV(r1, g1, b1) + RgbToV(r2, g2, b2)) / 2;

    *dst++ = static_cast<uint8_t>(u);
    *dst++ = static_cast<uint8_t>(y1);
    *dst++ = static_cast<uint8_t>(v);
    *dst++ = static_cast<uint8_t>(y2);
  }
}

void ConvertRowSimd(uint8_t* dst, const uint8_t* src, int width) {
  /* Coefficient lanes follow memory order: B, G, R, X. */
  const __m128i y_coeff = _mm_setr_epi16(
      static_cast<int16_t>(kCoeffYb), static_cast<int16_t>(kCoeffYg),
      static_cast<int16_t>(kCoeffYr), 0, static_cast<int16_t>(kCoeffYb),
      static_cast<int16_t>(kCoeffYg), static_cast<int16_t>(kCoeffYr), 0);
  const __m128i u_coeff = _mm_setr_epi16(
      static_cast<int16_t>(kCoeffUb), static_cast<int16_t>(kCoeffUg),
      static_cast<int16_t>(kCoeffUr), 0, static_cast<int16_t>(kCoeffUb),
      static_cast<int16_t>(kCoeffUg), static_cast<int16_t>(kCoeffUr), 0);
  const __m128i v_coeff = _mm_setr_epi16(
      static_cast<int16_t>(kCoeffVb), static_cast<int16_t>(kCoeffVg),
      static_cast<int16_t>(kCoeffVr), 0, static_cast<int16_t>(kCoeffVb),
      static_cast<int16_t>(kCoeffVg), static_cast<int16_t>(kCoeffVr), 0);
  const __m128i zero = _mm_setzero_si128();
  const __m128i ones = _mm_set1_epi16(1);

  int i = 0;
  /* Eight pixels per iteration is exactly one 16 byte UYVY store. */
  for (; i + 7 < width; i += 8) {
    const uint8_t* p = src + static_cast<size_t>(i) * 4;
    const __m128i raw0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m128i raw1 =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));

    const __m128i lo0 = _mm_unpacklo_epi8(raw0, zero);
    const __m128i hi0 = _mm_unpackhi_epi8(raw0, zero);
    const __m128i lo1 = _mm_unpacklo_epi8(raw1, zero);
    const __m128i hi1 = _mm_unpackhi_epi8(raw1, zero);

    const __m128i y_lo = ScaleAndBias(QuadDots(lo0, hi0, y_coeff), 16);
    const __m128i y_hi = ScaleAndBias(QuadDots(lo1, hi1, y_coeff), 16);
    const __m128i y_bytes =
        _mm_packus_epi16(_mm_packs_epi32(y_lo, y_hi), zero);

    const __m128i u_all = _mm_packs_epi32(
        ScaleAndBias(QuadDots(lo0, hi0, u_coeff), 128),
        ScaleAndBias(QuadDots(lo1, hi1, u_coeff), 128));
    const __m128i v_all = _mm_packs_epi32(
        ScaleAndBias(QuadDots(lo0, hi0, v_coeff), 128),
        ScaleAndBias(QuadDots(lo1, hi1, v_coeff), 128));

    /* madd against all ones is a pairwise horizontal add, which is the
     * average across each pixel pair once shifted. */
    const __m128i u_pairs = _mm_srai_epi32(_mm_madd_epi16(u_all, ones), 1);
    const __m128i v_pairs = _mm_srai_epi32(_mm_madd_epi16(v_all, ones), 1);

    const __m128i uv = _mm_unpacklo_epi16(_mm_packs_epi32(u_pairs, zero),
                                          _mm_packs_epi32(v_pairs, zero));
    const __m128i uv_bytes = _mm_packus_epi16(uv, zero);

    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst),
                     _mm_unpacklo_epi8(uv_bytes, y_bytes));
    dst += 16;
  }

  if (i < width) {
    ConvertRowScalar(dst, src + static_cast<size_t>(i) * 4, width - i);
  }
}

void ConvertRow(uint8_t* dst, const uint8_t* src, int width) {
  ConvertRowSimd(dst, src, width);
}

void ApplyPictureAdjust(uint8_t* row, int width, const PictureAdjust& adjust) {
  if (adjust.IsIdentity()) {
    return;
  }
  const int luma_gain = (adjust.brightness * 256) / 100;
  const int chroma_gain = (adjust.contrast * 256) / 50;

  for (int i = 0; i < width * 2; i += 4) {
    for (int luma : {1, 3}) {
      int y = row[i + luma] - 16;
      y = ((y * luma_gain) >> 8) + 16;
      row[i + luma] =
          static_cast<uint8_t>(y < 16 ? 16 : (y > 235 ? 235 : y));
    }
    for (int chroma : {0, 2}) {
      int c = row[i + chroma] - 128;
      c = ((c * chroma_gain) >> 8) + 128;
      row[i + chroma] =
          static_cast<uint8_t>(c < 16 ? 16 : (c > 240 ? 240 : c));
    }
  }
}

size_t FrameExisting(uint8_t* dst, size_t dst_capacity, const Rect& rect) {
  const size_t needed = TransferLength(rect);
  if (rect.empty() || dst_capacity < needed) {
    return 0;
  }

  FrameHeader header;
  PutBe16(header.marker_be, kFrameMarker);
  PutBe24(header.position_be24,
          ((static_cast<uint32_t>(rect.x1) & 0xFFF) << 12) |
              (static_cast<uint32_t>(rect.y1) & 0xFFF));
  PutBe24(header.dimensions_be24,
          ((static_cast<uint32_t>(rect.width()) & 0xFFF) << 12) |
              (static_cast<uint32_t>(rect.height()) & 0xFFF));
  memcpy(dst, &header, sizeof(header));

  memcpy(dst + needed - kFrameFooterSize, kFrameFooter, kFrameFooterSize);
  return needed;
}

void ConvertRegion(uint8_t* dst, const uint8_t* src, size_t stride,
                   const Rect& rect, const PictureAdjust& adjust) {
  const size_t row_bytes = static_cast<size_t>(rect.width()) * 2;
  const int width = rect.width();

  RowPool::Instance().Run(rect.height(), [&](int first, int last) {
    for (int i = first; i < last; ++i) {
      const uint8_t* row = src +
                           static_cast<size_t>(rect.y1 + i) * stride +
                           static_cast<size_t>(rect.x1) * 4;
      uint8_t* target = dst + static_cast<size_t>(i) * row_bytes;
      ConvertRow(target, row, width);
      ApplyPictureAdjust(target, width, adjust);
    }
  });
}

size_t FrameSubRegion(uint8_t* dst, size_t dst_capacity,
                      const uint8_t* converted, const Rect& region,
                      const Rect& sub) {
  if (sub.empty() || sub.x1 < region.x1 || sub.y1 < region.y1 ||
      sub.x2 > region.x2 || sub.y2 > region.y2 || (sub.x1 & 1) ||
      (sub.width() & 1)) {
    return 0;
  }

  const size_t needed = FrameExisting(dst, dst_capacity, sub);
  if (needed == 0) {
    return 0;
  }

  const size_t region_row = static_cast<size_t>(region.width()) * 2;
  const size_t sub_row = static_cast<size_t>(sub.width()) * 2;
  uint8_t* out = dst + kFrameHeaderSize;

  for (int y = sub.y1; y < sub.y2; ++y) {
    memcpy(out, converted + static_cast<size_t>(y - region.y1) * region_row +
                    static_cast<size_t>(sub.x1 - region.x1) * 2,
           sub_row);
    out += sub_row;
  }
  return needed;
}

size_t FrameRect(uint8_t* dst, size_t dst_capacity, const uint8_t* src,
                 size_t stride, int image_width, int image_height,
                 const Rect& rect, const PictureAdjust& adjust) {
  if (rect.empty() || rect.x1 < 0 || rect.y1 < 0 || rect.x2 > image_width ||
      rect.y2 > image_height || (rect.x1 & 1) || (rect.width() & 1)) {
    return 0;
  }

  const size_t needed = FrameExisting(dst, dst_capacity, rect);
  if (needed == 0) {
    return 0;
  }

  ConvertRegion(dst + kFrameHeaderSize, src, stride, rect, adjust);
  return needed;
}

int SelfTest(int width, int iterations) {
  std::vector<uint8_t> source(static_cast<size_t>(width) * 4);
  std::vector<uint8_t> scalar(static_cast<size_t>(width) * 2);
  std::vector<uint8_t> simd(static_cast<size_t>(width) * 2);

  uint32_t seed = 12345;
  int worst = 0;
  for (int iteration = 0; iteration < iterations; ++iteration) {
    for (size_t i = 0; i < source.size(); ++i) {
      seed = seed * 1664525u + 1013904223u;
      source[i] = static_cast<uint8_t>(seed >> 24);
    }
    ConvertRowScalar(scalar.data(), source.data(), width);
    ConvertRowSimd(simd.data(), source.data(), width);
    for (size_t i = 0; i < scalar.size(); ++i) {
      int diff = static_cast<int>(scalar[i]) - static_cast<int>(simd[i]);
      if (diff < 0) {
        diff = -diff;
      }
      if (diff > worst) {
        worst = diff;
      }
    }
  }
  return worst;
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

}  // namespace usbhdmi
