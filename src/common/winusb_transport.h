/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Full transport over WinUSB: control transfers for the register and command
 * plane, bulk OUT on endpoint 4 for pixels. Requires the WinUSB INF in inf/
 * to be bound to the vendor specific display interface (MI_03 on USB 3 parts).
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "transport.h"

namespace ms912x {

class WinUsbTransport : public Transport {
 public:
  ~WinUsbTransport() override;

  /* Enumerates devices exposing our driver's device interface GUID. */
  static std::vector<DeviceLocation> Enumerate();

  static std::unique_ptr<WinUsbTransport> Open(std::string* error);
  static std::unique_ptr<WinUsbTransport> OpenPath(const std::wstring& path,
                                                   std::string* error);

  std::string Describe() const override;
  bool HasDataPlane() const override { return true; }
  bool ControlSetReport(const uint8_t* data, size_t len) override;
  bool ControlGetReport(uint8_t* data, size_t len) override;
  bool BulkWrite(const uint8_t* data, size_t len) override;

  /* Writes a human readable dump of every interface setting and pipe. Used by
   * the enumerate phase to record what the hardware actually exposes. */
  bool DumpDescriptors(std::string* out);

 private:
  bool FindBulkOutPipe(std::string* error);

  void* file_handle_ = nullptr;      /* HANDLE */
  void* winusb_handle_ = nullptr;    /* WINUSB_INTERFACE_HANDLE */
  uint8_t bulk_out_pipe_id_ = 0;
  uint16_t bulk_max_packet_ = 0;
  DeviceLocation location_;
};

}  // namespace ms912x
