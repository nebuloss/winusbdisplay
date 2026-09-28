/* SPDX-License-Identifier: GPL-2.0-only */

#include "converter.h"

#include <cstring>

#include "../core/display_device.h"

/* Generated from convert_cs.hlsl by the build. */
#include "convert_cs.h"

using Microsoft::WRL::ComPtr;

namespace usbdisplay {

namespace {

struct ShaderParams {
  uint32_t width;
  uint32_t height;
  int32_t luma_gain;
  int32_t chroma_gain;
  int32_t apply_gamma;
  int32_t padding[3];
};

/* B8G8R8A8_UNORM and its typeless sibling are what the compositor hands out.
 * Anything else is rejected rather than guessed at: a wrong channel order
 * would produce a plausible but colour swapped picture, which is much harder
 * to spot than a refusal. */
bool FormatIsSupported(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
         format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

DXGI_FORMAT ViewFormatFor(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
      return format;
  }
}

}  // namespace

bool GpuRegionConverter::Initialise(ID3D11Device* device,
                                    ID3D11DeviceContext* context) {
  device_ = device;
  context_ = context;

  if (FAILED(device->CreateComputeShader(kConvertComputeShader,
                                         sizeof(kConvertComputeShader),
                                         nullptr, &shader_))) {
    error_ = "the device would not accept the conversion shader";
    return false;
  }

  D3D11_BUFFER_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.ByteWidth = sizeof(ShaderParams);
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&desc, nullptr, &constants_))) {
    error_ = "could not create the shader constant buffer";
    return false;
  }

  /* Allocate for the largest update there can be, once, so no frame ever
   * pays for an allocation. Four megabytes of video memory is not worth
   * economising on here. */
  if (!EnsureBuffers(static_cast<size_t>(kMaxFrameWidth) * kMaxFrameHeight *
                     2)) {
    return false;
  }
  usable_ = true;
  return true;
}

bool GpuRegionConverter::Suits(const Rect& region, int64_t threshold) const {
  return usable_ && threshold > 0 && region.area() >= threshold;
}

bool GpuRegionConverter::EnsureBuffers(size_t bytes) {
  if (buffer_bytes_ >= bytes && output_) {
    return true;
  }

  output_.Reset();
  output_view_.Reset();
  readback_.Reset();

  /* Rounded to whole UYVY pairs, which is what the shader indexes. */
  const size_t elements = (bytes + 3) / 4;
  const size_t rounded = elements * 4;

  D3D11_BUFFER_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.ByteWidth = static_cast<UINT>(rounded);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  desc.StructureByteStride = 4;
  if (FAILED(device_->CreateBuffer(&desc, nullptr, &output_))) {
    error_ = "could not create the conversion output buffer";
    return false;
  }

  D3D11_UNORDERED_ACCESS_VIEW_DESC view;
  memset(&view, 0, sizeof(view));
  view.Format = DXGI_FORMAT_UNKNOWN;
  view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  view.Buffer.NumElements = static_cast<UINT>(elements);
  if (FAILED(device_->CreateUnorderedAccessView(output_.Get(), &view,
                                                &output_view_))) {
    error_ = "could not create the conversion output view";
    return false;
  }

  memset(&desc, 0, sizeof(desc));
  desc.ByteWidth = static_cast<UINT>(rounded);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(device_->CreateBuffer(&desc, nullptr, &readback_))) {
    error_ = "could not create the conversion readback buffer";
    return false;
  }

  buffer_bytes_ = rounded;
  return true;
}

bool GpuRegionConverter::EnsureRegionTexture(int width, int height,
                                       DXGI_FORMAT format) {
  if (region_ && region_width_ >= width && region_height_ >= height &&
      region_format_ == format) {
    return true;
  }

  region_.Reset();
  region_view_.Reset();

  D3D11_TEXTURE2D_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, &region_))) {
    error_ = "could not create the region texture";
    return false;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC view;
  memset(&view, 0, sizeof(view));
  view.Format = ViewFormatFor(format);
  view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  view.Texture2D.MipLevels = 1;
  if (FAILED(device_->CreateShaderResourceView(region_.Get(), &view,
                                               &region_view_))) {
    error_ = "could not create the region view";
    return false;
  }

  region_width_ = width;
  region_height_ = height;
  region_format_ = format;
  return true;
}

bool GpuRegionConverter::UploadGamma(const GammaRamp& gamma) {
  if (gamma_ && gamma_uploaded_ && uploaded_gamma_ == gamma) {
    return true;
  }

  /* 768 entries, one per channel per level, held as 32 bit values because a
   * structured buffer of 16 bit elements is awkward to index in a shader and
   * this costs three kilobytes. */
  uint32_t table[768];
  for (int i = 0; i < 256; ++i) {
    table[i] = gamma.red[i];
    table[256 + i] = gamma.green[i];
    table[512 + i] = gamma.blue[i];
  }

  if (!gamma_) {
    D3D11_BUFFER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = sizeof(table);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(uint32_t);
    if (FAILED(device_->CreateBuffer(&desc, nullptr, &gamma_))) {
      error_ = "could not create the gamma table buffer";
      return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC view;
    memset(&view, 0, sizeof(view));
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = 768;
    if (FAILED(device_->CreateShaderResourceView(gamma_.Get(), &view,
                                                 &gamma_view_))) {
      error_ = "could not create the gamma table view";
      return false;
    }
  }

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context_->Map(gamma_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                           &mapped))) {
    error_ = "could not upload the gamma table";
    return false;
  }
  memcpy(mapped.pData, table, sizeof(table));
  context_->Unmap(gamma_.Get(), 0);

  uploaded_gamma_ = gamma;
  gamma_uploaded_ = true;
  return true;
}


bool GpuRegionConverter::Convert(ID3D11Texture2D* source, const Rect& region,
                                 const PictureAdjust& adjust,
                                 const GammaRamp& gamma, uint8_t* dst,
                                 size_t capacity) {
  if (!usable_ || region.empty() || (region.width() & 1)) {
    error_ = "the GPU path was asked for an update it cannot handle";
    return false;
  }

  const size_t needed =
      static_cast<size_t>(region.width()) * 2 * region.height();
  if (capacity < needed) {
    error_ = "the destination buffer is too small";
    return false;
  }

  D3D11_TEXTURE2D_DESC source_desc;
  source->GetDesc(&source_desc);
  if (!FormatIsSupported(source_desc.Format)) {
    error_ = "the compositor surface is in an unexpected pixel format";
    return false;
  }

  if (!EnsureRegionTexture(region.width(), region.height(),
                           source_desc.Format)) {
    return false;
  }
  if (!UploadGamma(gamma)) {
    return false;
  }

  /* Copy just the damaged region. This is the cheap part, measured at
   * hundredths of a millisecond even for a full screen, because it never
   * leaves the GPU. */
  D3D11_BOX box;
  box.left = static_cast<UINT>(region.x1);
  box.top = static_cast<UINT>(region.y1);
  box.front = 0;
  box.right = static_cast<UINT>(region.x2);
  box.bottom = static_cast<UINT>(region.y2);
  box.back = 1;
  context_->CopySubresourceRegion(region_.Get(), 0, 0, 0, 0, source, 0, &box);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                           &mapped))) {
    error_ = "could not update the shader constants";
    return false;
  }
  ShaderParams params;
  params.width = static_cast<uint32_t>(region.width());
  params.height = static_cast<uint32_t>(region.height());
  params.luma_gain = adjust.LumaGain();
  params.chroma_gain = adjust.ChromaGain();
  params.apply_gamma = gamma.identity ? 0 : 1;
  memset(params.padding, 0, sizeof(params.padding));
  memcpy(mapped.pData, &params, sizeof(params));
  context_->Unmap(constants_.Get(), 0);

  ID3D11ShaderResourceView* views[] = {region_view_.Get(), gamma_view_.Get()};
  ID3D11UnorderedAccessView* targets[] = {output_view_.Get()};
  ID3D11Buffer* buffers[] = {constants_.Get()};
  context_->CSSetShader(shader_.Get(), nullptr, 0);
  context_->CSSetShaderResources(0, 2, views);
  context_->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
  context_->CSSetConstantBuffers(0, 1, buffers);

  const UINT groups_x = static_cast<UINT>((region.width() / 2 + 7) / 8);
  const UINT groups_y = static_cast<UINT>((region.height() + 7) / 8);
  context_->Dispatch(groups_x, groups_y, 1);

  /* Unbind before the copy. Leaving the output bound as a UAV while it is
   * also the source of a copy makes the debug layer complain and the runtime
   * silently drop one or the other. */
  ID3D11ShaderResourceView* no_views[] = {nullptr, nullptr};
  ID3D11UnorderedAccessView* no_targets[] = {nullptr};
  context_->CSSetShaderResources(0, 2, no_views);
  context_->CSSetUnorderedAccessViews(0, 1, no_targets, nullptr);

  D3D11_BOX finished;
  finished.left = 0;
  finished.right = static_cast<UINT>((needed + 3) & ~size_t(3));
  finished.top = 0;
  finished.bottom = 1;
  finished.front = 0;
  finished.back = 1;
  context_->CopySubresourceRegion(readback_.Get(), 0, 0, 0, 0, output_.Get(),
                                  0, &finished);

  /* This blocks until the GPU is done. Double buffering the readback would
   * remove the stall at the cost of a frame of latency; it is not worth it
   * while the transfer that follows takes twenty times longer. */
  if (FAILED(context_->Map(readback_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    error_ = "could not read back the converted pixels";
    return false;
  }
  memcpy(dst, mapped.pData, needed);
  context_->Unmap(readback_.Get(), 0);
  return true;
}

/* ---- processor ---------------------------------------------------------- */

CpuRegionConverter::CpuRegionConverter(ID3D11Device* device,
                                       ID3D11DeviceContext* context,
                                       const DisplayDevice* format)
    : device_(device), context_(context), format_(format) {}

bool CpuRegionConverter::Suits(const Rect& region, int64_t threshold) const {
  /* Everything the graphics path does not want, and everything at all when
   * that path is switched off or unavailable. */
  return threshold <= 0 || region.area() < threshold;
}

bool CpuRegionConverter::EnsureStaging(int width, int height) {
  if (staging_ && staging_width_ >= width && staging_height_ >= height) {
    return true;
  }
  staging_.Reset();

  D3D11_TEXTURE2D_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) {
    error_ = "could not create the readback texture";
    return false;
  }
  staging_width_ = width;
  staging_height_ = height;
  return true;
}

bool CpuRegionConverter::Convert(ID3D11Texture2D* source, const Rect& region,
                                 const PictureAdjust& adjust,
                                 const GammaRamp& gamma, uint8_t* destination,
                                 size_t capacity) {
  if (region.empty()) {
    error_ = "asked to convert nothing";
    return false;
  }
  if (!EnsureStaging(region.width(), region.height())) {
    return false;
  }

  /* Copy only the damaged region, into the corner of the readback texture,
   * so the map that follows touches as little memory as possible. Reading
   * mapped graphics memory is far slower than ordinary memory and is most of
   * the cost of this path. */
  D3D11_BOX box;
  box.left = static_cast<UINT>(region.x1);
  box.top = static_cast<UINT>(region.y1);
  box.front = 0;
  box.right = static_cast<UINT>(region.x2);
  box.bottom = static_cast<UINT>(region.y2);
  box.back = 1;
  context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, source, 0, &box);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    error_ = "could not read back the desktop image";
    return false;
  }

  /* The region now sits at the origin of the readback texture, so convert it
   * as if it were a whole image of that size. RowPitch is not width * 4. */
  Rect local;
  local.x1 = 0;
  local.y1 = 0;
  local.x2 = region.width();
  local.y2 = region.height();
  const bool ok =
      ConvertRegion(destination, capacity,
                    static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                    local, adjust, &gamma);
  context_->Unmap(staging_.Get(), 0);

  if (!ok) {
    error_ = "the destination is too small for this region";
  }
  return ok;
}

/* ---- choosing between them ---------------------------------------------- */

void ConverterSet::Add(std::unique_ptr<RegionConverter> converter) {
  converters_.push_back(std::move(converter));
}

bool ConverterSet::Convert(ID3D11Texture2D* source, const Rect& region,
                           const PictureAdjust& adjust, const GammaRamp& gamma,
                           uint8_t* destination, size_t capacity,
                           int64_t threshold, const char** used) {
  /* Preferred implementation first, then anything else still standing. */
  for (int pass = 0; pass < 2; ++pass) {
    for (auto it = converters_.begin(); it != converters_.end();) {
      RegionConverter* converter = it->get();
      if (!converter->Usable() ||
          (pass == 0 && !converter->Suits(region, threshold))) {
        ++it;
        continue;
      }
      if (converter->Convert(source, region, adjust, gamma, destination,
                             capacity)) {
        if (used) {
          *used = converter->Name();
        }
        return true;
      }
      /* Dropped rather than retried: see the note in the header. */
      if (converters_.size() > 1) {
        it = converters_.erase(it);
      } else {
        ++it;
      }
    }
  }
  return false;
}

std::string ConverterSet::Describe() const {
  std::string out;
  for (const auto& converter : converters_) {
    if (!out.empty()) {
      out += ", ";
    }
    out += converter->Name();
    if (!converter->Usable()) {
      out += " (unavailable)";
    }
  }
  return out.empty() ? "none" : out;
}

}  // namespace usbdisplay
