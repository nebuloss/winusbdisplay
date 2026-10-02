/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The Direct3D device the compositor's surfaces live on.
 *
 * Separated from the frame loop because of a bug that only this separation
 * fixes. Creating a Direct3D device for the first time in a process loads
 * the graphics driver's own libraries and takes a few hundred milliseconds.
 * Doing that inside the callback that is handed a swapchain means the
 * swapchain has gone stale by the time it is used, and the attempt fails
 * with DXGI_ERROR_ACCESS_LOST. Measured on this machine: 412 ms for the
 * first attempt, which failed, and 60 ms for the second, which did not.
 *
 * The OS recovers by building another swapchain, so the symptom was only a
 * line in the log and a slower start. It is still the wrong shape: the
 * expensive part has nothing to do with any particular swapchain, so it
 * belongs somewhere that outlives them.
 *
 * One of these is created when the adapter is initialised and lives until
 * the adapter goes away. Swapchains come and go against it.
 */

#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

namespace usbdisplay {

class GraphicsContext {
 public:
  /* Opens the adapter the compositor renders on.
   *
   * Returns false and leaves a reason in error() rather than throwing, so
   * the caller can carry on without pixels: the control plane still works
   * and the log still says why. */
  bool Open(LUID adapter);

  /* True when this context is for that adapter and still usable. The
   * compositor can move rendering to a different adapter, and the device
   * can be lost; both mean this one has to be rebuilt. */
  bool Matches(LUID adapter) const;

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }
  IDXGIDevice* dxgi() const { return dxgi_.Get(); }

  const char* error() const { return error_; }

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_;
  LUID adapter_ = {};
  const char* error_ = "not opened";
};

}  // namespace usbdisplay
