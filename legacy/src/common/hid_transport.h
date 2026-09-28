/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Control plane over the in-box HID stack.
 *
 * Section 4.1 of the protocol notes says the control payloads are HID class
 * SET_REPORT / GET_REPORT with wValue 0x0300 and wIndex 0. wIndex 0 means
 * interface 0, which on these dongles is a plain vendor HID collection that
 * Windows already binds hidusb.sys to. wValue 0x0300 is "feature report,
 * report id 0". So HidD_SetFeature / HidD_GetFeature emit byte for byte the
 * same control transfers the Linux driver does, with no INF, no signing and
 * no need to displace the vendor driver.
 *
 * This is the bring-up path for phases 2 and 3. It cannot carry pixels.
 */

#pragma once

#include <memory>
#include <vector>

#include "transport.h"

namespace ms912x {

class HidTransport : public Transport {
 public:
  ~HidTransport() override;

  /* Enumerates HID interfaces belonging to a known MacroSilicon VID/PID. */
  static std::vector<DeviceLocation> Enumerate();

  /* Opens the first usable device, or a specific one by path. */
  static std::unique_ptr<HidTransport> Open(std::string* error);
  static std::unique_ptr<HidTransport> OpenPath(const std::wstring& path,
                                                std::string* error);

  /* Opens the HID interface belonging to a specific physical dongle. Used by
   * the driver, which already knows which device it is bound to and must not
   * grab the control interface of a different adapter. */
  static std::unique_ptr<HidTransport> OpenForContainer(const GUID& container,
                                                        std::string* error);

  std::string Describe() const override;
  bool HasDataPlane() const override { return false; }
  bool ControlSetReport(const uint8_t* data, size_t len) override;
  bool ControlGetReport(uint8_t* data, size_t len) override;
  bool BulkWrite(const uint8_t* data, size_t len) override;

 private:
  void* handle_ = nullptr; /* HANDLE */
  size_t feature_report_len_ = 0;
  DeviceLocation location_;
  std::vector<uint8_t> scratch_;
};

}  // namespace ms912x
