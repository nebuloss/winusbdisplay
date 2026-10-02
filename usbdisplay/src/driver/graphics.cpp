/* SPDX-License-Identifier: GPL-2.0-only */

#include "graphics.h"

#include "log.h"

namespace usbdisplay {

bool GraphicsContext::Open(LUID adapter) {
  device_.Reset();
  context_.Reset();
  dxgi_.Reset();
  adapter_ = {};

  Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    error_ = "no DXGI factory";
    Log("graphics: CreateDXGIFactory2 -> 0x%08X", hr);
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter1> target;
  hr = factory->EnumAdapterByLuid(adapter, IID_PPV_ARGS(&target));
  if (FAILED(hr)) {
    error_ = "the render adapter could not be opened";
    Log("graphics: EnumAdapterByLuid(%08X:%08X) -> 0x%08X", adapter.HighPart,
        adapter.LowPart, hr);
    return false;
  }

  /* BGRA support because the compositor hands over surfaces in that order
   * and the conversion path samples them directly. Without the flag the
   * device is created and binding one of those textures fails later, which
   * is a far harder failure to read than this one. */
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  hr = D3D11CreateDevice(target.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                         D3D11_SDK_VERSION, &device_, &level, &context_);
  if (FAILED(hr)) {
    error_ = "the graphics device could not be created";
    Log("graphics: D3D11CreateDevice -> 0x%08X", hr);
    return false;
  }

  hr = device_.As(&dxgi_);
  if (FAILED(hr)) {
    error_ = "the graphics device is not a DXGI device";
    Log("graphics: no IDXGIDevice -> 0x%08X", hr);
    return false;
  }

  /* The acquire loop is latency sensitive and loses frames if it is
   * scheduled behind ordinary graphics work. */
  dxgi_->SetGPUThreadPriority(7);

  adapter_ = adapter;
  error_ = nullptr;
  Log("graphics: ready on adapter %08X:%08X, feature level %X",
      adapter.HighPart, adapter.LowPart, level);
  return true;
}

bool GraphicsContext::Matches(LUID adapter) const {
  if (!device_ || adapter_.LowPart != adapter.LowPart ||
      adapter_.HighPart != adapter.HighPart) {
    return false;
  }
  /* A device can be lost without anything else saying so, and every later
   * call against it fails in its own way. Asking here turns that into one
   * decision made in one place. */
  return SUCCEEDED(device_->GetDeviceRemovedReason());
}

}  // namespace usbdisplay
