/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Turning a region of the desktop into the adapter's pixel format.
 *
 * There are two ways to do this and the driver uses both, choosing per
 * update. That is not indecision: each wins decisively in its own range.
 *
 *   processor   reads the region back from the graphics card and converts it
 *               with vector code. Pays four bytes per pixel crossing the bus
 *               and some processor time, but starts instantly.
 *
 *   graphics    converts on the card with a compute shader and reads back
 *               the finished result. Pays two bytes per pixel and no
 *               processor time, but pays a fixed setup and readback cost
 *               that a small update cannot repay.
 *
 * So they are expressed as one interface with two implementations, and the
 * pipeline asks each in turn whether it wants a given region. Before this
 * they were an if, a fallback flag and two sets of members tangled into the
 * frame loop, which made it easy to lose track of which one had actually run.
 *
 * ## The rule that binds them
 *
 * **Both must produce byte-identical output.** A region redrawn at slightly
 * different sizes crosses the threshold and takes different paths on
 * consecutive frames; if they disagreed by even one least significant bit,
 * that region would flicker between two values, which on small text reads as
 * a shimmer. Any third implementation added here inherits that obligation,
 * and the test suite enforces it.
 */

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "convert.h"
#include "rect.h"

namespace usbdisplay {

class DisplayDevice;

class RegionConverter {
 public:
  virtual ~RegionConverter() = default;

  /* For the log, so which path ran is never a matter of inference. */
  virtual const char* Name() const = 0;

  /* False when this implementation cannot run here at all, for instance
   * because the graphics device refused the shader. */
  virtual bool Usable() const = 0;

  /* Whether this implementation is the right one for a region this size.
   * `threshold` is the pixel count at which the graphics path starts to pay
   * for itself, and is adjustable because it depends on the machine. */
  virtual bool Suits(const Rect& region, int64_t threshold) const = 0;

  /* Converts `region` of `source` into `destination`, packed row by row in
   * the device's format. Returns false on failure, after which the caller
   * should stop offering it work. */
  virtual bool Convert(ID3D11Texture2D* source, const Rect& region,
                       const PictureAdjust& adjust, const GammaRamp& gamma,
                       uint8_t* destination, size_t capacity) = 0;

  virtual const char* error() const = 0;
};

/* Reads the region back and converts it with vector code on a small thread
 * pool. Always usable, and therefore always the fallback. */
class CpuRegionConverter : public RegionConverter {
 public:
  CpuRegionConverter(ID3D11Device* device, ID3D11DeviceContext* context);

  const char* Name() const override { return "processor"; }
  bool Usable() const override { return true; }
  bool Suits(const Rect& region, int64_t threshold) const override;
  bool Convert(ID3D11Texture2D* source, const Rect& region,
               const PictureAdjust& adjust, const GammaRamp& gamma,
               uint8_t* destination, size_t capacity) override;
  const char* error() const override { return error_; }

 private:
  bool EnsureStaging(int width, int height);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
  int staging_width_ = 0;
  int staging_height_ = 0;
  const char* error_ = "";
};

/* Converts on the graphics card with a compute shader. */
class GpuRegionConverter : public RegionConverter {
 public:
  bool Initialise(ID3D11Device* device, ID3D11DeviceContext* context);

  const char* Name() const override { return "graphics card"; }
  bool Usable() const override { return usable_; }
  bool Suits(const Rect& region, int64_t threshold) const override;
  bool Convert(ID3D11Texture2D* source, const Rect& region,
               const PictureAdjust& adjust, const GammaRamp& gamma,
               uint8_t* destination, size_t capacity) override;
  const char* error() const override { return error_; }

 private:
  bool EnsureRegionTexture(int width, int height, DXGI_FORMAT format);
  bool EnsureBuffers(size_t bytes);
  bool UploadGamma(const GammaRamp& gamma);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> region_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> region_view_;
  int region_width_ = 0;
  int region_height_ = 0;
  DXGI_FORMAT region_format_ = DXGI_FORMAT_UNKNOWN;

  Microsoft::WRL::ComPtr<ID3D11Buffer> output_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> output_view_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> readback_;
  size_t buffer_bytes_ = 0;

  /* The gamma table, uploaded only when it changes. */
  Microsoft::WRL::ComPtr<ID3D11Buffer> gamma_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gamma_view_;
  GammaRamp uploaded_gamma_;
  bool gamma_uploaded_ = false;

  bool usable_ = false;
  const char* error_ = "";
};

/* Holds the available implementations in preference order and picks one.
 *
 * Failure is permanent by design: an implementation that has failed once is
 * dropped rather than retried. The two produce identical bytes, so falling
 * back costs nothing but processor time, whereas retrying a broken shader
 * every frame costs a stall every frame. */
class ConverterSet {
 public:
  void Add(std::unique_ptr<RegionConverter> converter);

  /* Converts using the most suitable implementation, falling back through
   * the rest on failure. Names the one that ran in `used`. */
  bool Convert(ID3D11Texture2D* source, const Rect& region,
               const PictureAdjust& adjust, const GammaRamp& gamma,
               uint8_t* destination, size_t capacity, int64_t threshold,
               const char** used);

  std::string Describe() const;

 private:
  std::vector<std::unique_ptr<RegionConverter>> converters_;
};

}  // namespace usbdisplay
