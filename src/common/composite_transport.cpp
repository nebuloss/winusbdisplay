/* SPDX-License-Identifier: GPL-2.0-only */

#include "composite_transport.h"

#include "hid_transport.h"
#include "winusb_transport.h"

namespace ms912x {

CompositeTransport::CompositeTransport(std::unique_ptr<Transport> control,
                                       std::unique_ptr<Transport> data)
    : control_(std::move(control)), data_(std::move(data)) {}

std::unique_ptr<CompositeTransport> CompositeTransport::Open(
    std::string* error) {
  std::string control_error;
  std::unique_ptr<HidTransport> control = HidTransport::Open(&control_error);
  if (!control) {
    if (error) {
      *error = "control plane: " + control_error;
    }
    return nullptr;
  }

  std::string data_error;
  std::unique_ptr<WinUsbTransport> data = WinUsbTransport::Open(&data_error);
  if (!data) {
    if (error) {
      *error = "data plane: " + data_error;
    }
    return nullptr;
  }

  return std::unique_ptr<CompositeTransport>(
      new CompositeTransport(std::move(control), std::move(data)));
}

std::string CompositeTransport::Describe() const {
  return "control via " + control_->Describe() + "; data via " +
         data_->Describe();
}

bool CompositeTransport::HasDataPlane() const {
  return data_ && data_->HasDataPlane();
}

bool CompositeTransport::ControlSetReport(const uint8_t* data, size_t len) {
  if (!control_->ControlSetReport(data, len)) {
    SetError(control_->last_error());
    return false;
  }
  return true;
}

bool CompositeTransport::ControlGetReport(uint8_t* data, size_t len) {
  if (!control_->ControlGetReport(data, len)) {
    SetError(control_->last_error());
    return false;
  }
  return true;
}

bool CompositeTransport::BulkWrite(const uint8_t* data, size_t len) {
  if (!data_->BulkWrite(data, len)) {
    SetError(data_->last_error());
    return false;
  }
  return true;
}

}  // namespace ms912x
