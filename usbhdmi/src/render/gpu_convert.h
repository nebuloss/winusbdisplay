/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The GPU half of the conversion, and the rule for choosing between the two.
 *
 * Both paths start by copying the damaged region out of the surface the
 * compositor handed us, because that surface cannot be read directly. What
 * happens next differs:
 *
 *   CPU   copy into a staging texture, map it, convert while reading it
 *   GPU   copy into a shader readable texture, convert in a compute shader,
 *         read back the finished UYVY
 *
 * The GPU path moves half as many bytes across the bus, 2 bytes per pixel
 * instead of 4, and does no work on the processor. Against that it pays a
 * fixed cost: a dispatch, a buffer copy and a readback that blocks until the
 * GPU has finished. For a small update that fixed cost dominates and the CPU
 * path wins comfortably. For a large one it is amortised and the halved
 * readback wins.
 *
 * So the driver runs both and picks by area. `kGpuThresholdPixels` is where
 * the crossover sits; it is a starting point, not a law of nature, and it is
 * overridable at runtime because it depends on the machine. On a discrete GPU
 * the crossover should move down, since the CPU path has to drag four bytes
 * per pixel back across PCIe while the GPU path drags two.
 *
 * ## Mixing the two paths safely
 *
 * Two updates in one frame can take different paths, which makes ordering a
 * correctness problem rather than a preference. Three rules, all enforced in
 * driver/pipeline.cpp, and none of them optional:
 *
 * 1. **Transfers reach the chip in plan order.** The chip has no notion of a
 *    frame; it applies each transfer to the region it names, as it arrives.
 *    If an update overtakes an earlier one that touches the same pixels, the
 *    older content wins and stays on screen. The pipeline therefore converts
 *    and submits strictly in order, and the sender is a queue rather than a
 *    race for a free buffer.
 *
 * 2. **Every update in a frame comes from the same image.** Both paths copy
 *    out of the one surface the compositor handed over, before it is
 *    released. Converting one region from the live surface and another from
 *    a staging copy taken at a different moment would tear across regions in
 *    a way no amount of damage tracking can explain afterwards.
 *
 * 3. **A dropped update drops everything after it.** When no transfer buffer
 *    is free the update is abandoned and its damage stays owed, which is
 *    fine on its own. Continuing to the next region of the same frame is not
 *    fine, because that region would arrive without the one that should have
 *    preceded it. The rest of the frame is abandoned too, and the next frame
 *    resends the lot.
 *
 * Finally, this class talks to the D3D11 immediate context, which is not
 * thread safe. It belongs to the swapchain thread and must not be touched
 * from the sender thread.
 */

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <stdint.h>

#include "convert.h"
#include "rect.h"

namespace usbhdmi {

class GpuConverter {
 public:
  /* Compiles nothing at runtime: the shader is built into the binary by the
   * build script, so there is no dependency on a shader compiler being
   * present on the target machine. Returns false if the device cannot
   * support the path, in which case the caller uses the CPU one and keeps
   * using it. */
  bool Initialise(ID3D11Device* device, ID3D11DeviceContext* context);

  bool Available() const { return output_.Get() != nullptr; }

  /* Converts `rect` of `source` and writes UYVY into `dst`, which must have
   * room for width * 2 * height bytes. `source` is copied internally, so it
   * needs no particular bind flags.
   *
   * `rect` must be aligned: even x and a width that is a multiple of two. */
  bool Convert(ID3D11Texture2D* source, const Rect& rect,
               const PictureAdjust& adjust, uint8_t* dst, size_t capacity);

  const char* error() const { return error_; }

 private:
  bool EnsureRegionTexture(int width, int height, DXGI_FORMAT format);
  bool EnsureBuffers(size_t bytes);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;

  /* A copy of just the damaged region, which is what the shader reads. */
  Microsoft::WRL::ComPtr<ID3D11Texture2D> region_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> region_view_;
  int region_width_ = 0;
  int region_height_ = 0;
  DXGI_FORMAT region_format_ = DXGI_FORMAT_UNKNOWN;

  Microsoft::WRL::ComPtr<ID3D11Buffer> output_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> output_view_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> readback_;
  size_t buffer_bytes_ = 0;

  const char* error_ = "";
};

/* Which path an update of this size should take. Kept next to the converter
 * so the rule and the reasoning stay together. */
inline bool ShouldConvertOnGpu(const Rect& rect, int64_t threshold_pixels) {
  return rect.area() >= threshold_pixels;
}

}  // namespace usbhdmi
