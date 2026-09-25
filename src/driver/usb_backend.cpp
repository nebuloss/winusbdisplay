/* SPDX-License-Identifier: GPL-2.0-only */

#include "usb_backend.h"

#include <cstdio>
#include <cstring>

#include "ms912x_proto.h"

namespace ms912x {
namespace {

constexpr ULONG kBulkTimeoutMs = 5000;

}  // namespace

std::unique_ptr<WdfUsbBackend> WdfUsbBackend::Create(WDFDEVICE device,
                                                     std::string* error) {
  std::unique_ptr<WdfUsbBackend> backend(new WdfUsbBackend());

  WDF_USB_DEVICE_CREATE_CONFIG create_config;
  WDF_USB_DEVICE_CREATE_CONFIG_INIT(&create_config,
                                    USBD_CLIENT_CONTRACT_VERSION_602);
  NTSTATUS status = WdfUsbTargetDeviceCreateWithParameters(
      device, &create_config, WDF_NO_OBJECT_ATTRIBUTES,
      &backend->usb_device_);
  if (!NT_SUCCESS(status)) {
    if (error) {
      char buf[96];
      _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                  "WdfUsbTargetDeviceCreateWithParameters: 0x%08X", status);
      *error = buf;
    }
    return nullptr;
  }

  WDF_USB_DEVICE_SELECT_CONFIG_PARAMS config_params;
  WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&config_params);
  status = WdfUsbTargetDeviceSelectConfig(
      backend->usb_device_, WDF_NO_OBJECT_ATTRIBUTES, &config_params);
  if (!NT_SUCCESS(status)) {
    if (error) {
      char buf[96];
      _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                  "WdfUsbTargetDeviceSelectConfig: 0x%08X", status);
      *error = buf;
    }
    return nullptr;
  }
  backend->usb_interface_ =
      config_params.Types.SingleInterface.ConfiguredUsbInterface;

  const BYTE pipe_count =
      config_params.Types.SingleInterface.NumberConfiguredPipes;
  for (BYTE i = 0; i < pipe_count; ++i) {
    WDF_USB_PIPE_INFORMATION info;
    WDF_USB_PIPE_INFORMATION_INIT(&info);
    WDFUSBPIPE pipe =
        WdfUsbInterfaceGetConfiguredPipe(backend->usb_interface_, i, &info);
    if (!pipe) {
      continue;
    }
    if (info.PipeType != WdfUsbPipeTypeBulk ||
        !WdfUsbTargetPipeIsOutEndpoint(pipe)) {
      continue;
    }
    if ((info.EndpointAddress & 0x0F) != kBulkOutEndpoint) {
      continue;
    }
    backend->bulk_pipe_ = pipe;
    backend->bulk_endpoint_ = info.EndpointAddress;
    /* Frames are far larger than the maximum packet size and the framework
     * splits them; do not demand a short packet at the end of each one. */
    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);
    break;
  }

  if (!backend->bulk_pipe_) {
    if (error) {
      char buf[96];
      _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                  "no bulk OUT pipe on endpoint %u among %u pipes",
                  kBulkOutEndpoint, pipe_count);
      *error = buf;
    }
    return nullptr;
  }
  return backend;
}

std::string WdfUsbBackend::Describe() const {
  char buf[96];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE,
              "WDF USB target, bulk OUT endpoint 0x%02X", bulk_endpoint_);
  return buf;
}

bool WdfUsbBackend::ControlSetReport(const uint8_t*, size_t) {
  SetError("this backend has no control plane; pair it with a HidTransport");
  return false;
}

bool WdfUsbBackend::ControlGetReport(uint8_t*, size_t) {
  SetError("this backend has no control plane; pair it with a HidTransport");
  return false;
}

bool WdfUsbBackend::BulkWrite(const uint8_t* data, size_t len) {
  if (!bulk_pipe_) {
    SetError("bulk pipe not configured");
    return false;
  }

  WDF_REQUEST_SEND_OPTIONS options;
  WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
      &options, WDF_REL_TIMEOUT_IN_MS(kBulkTimeoutMs));

  ULONG transferred = 0;
  NTSTATUS status;

  if (len == 0) {
    /* Zero length terminating packet. A null memory descriptor is how WDF
     * expresses that. */
    status = WdfUsbTargetPipeWriteSynchronously(
        bulk_pipe_, WDF_NO_HANDLE, &options, nullptr, &transferred);
  } else {
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, const_cast<uint8_t*>(data),
                                      static_cast<ULONG>(len));
    status = WdfUsbTargetPipeWriteSynchronously(
        bulk_pipe_, WDF_NO_HANDLE, &options, &descriptor, &transferred);
  }

  if (!NT_SUCCESS(status)) {
    char buf[96];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "bulk write: 0x%08X", status);
    SetError(buf);
    WdfUsbTargetPipeResetSynchronously(bulk_pipe_, WDF_NO_HANDLE, nullptr);
    return false;
  }
  if (transferred != len) {
    SetError("short bulk write");
    return false;
  }
  return true;
}

}  // namespace ms912x
