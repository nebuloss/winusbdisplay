/* SPDX-License-Identifier: GPL-2.0-only */

#include "composite_transport.h"

namespace ms912x {

CompositeTransport::CompositeTransport(std::unique_ptr<Transport> control,
                                       std::unique_ptr<Transport> data)
    : control_(std::move(control)), data_(std::move(data)) {}

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

void CompositeTransport::CancelTransfers() {
  if (data_) {
    data_->CancelTransfers();
  }
}

bool CompositeTransport::BulkWrite(const uint8_t* data, size_t len) {
  if (!data_->BulkWrite(data, len)) {
    SetError(data_->last_error());
    return false;
  }
  return true;
}

}  // namespace ms912x
