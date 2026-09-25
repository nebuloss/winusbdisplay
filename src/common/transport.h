/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Transport abstraction. The control plane (8 byte HID feature reports) and
 * the data plane (bulk OUT on endpoint 4) are separated so that either half
 * can be stubbed out during bring-up: the HID transport gives a control plane
 * with no driver installation at all, and the file transport records frames
 * to disk so conversion bugs can be told apart from transport bugs.
 */

#pragma once

#include <windows.h>

#include <stdint.h>

#include <string>
#include <vector>

namespace ms912x {

class Transport {
 public:
  virtual ~Transport() = default;

  virtual std::string Describe() const = 0;

  /* True if BulkWrite is actually connected to hardware. */
  virtual bool HasDataPlane() const = 0;

  /* Both take and return exactly kControlPayloadSize bytes. Callers must
   * already hold the control lock; see Device, which owns it. */
  virtual bool ControlSetReport(const uint8_t* data, size_t len) = 0;
  virtual bool ControlGetReport(uint8_t* data, size_t len) = 0;

  virtual bool BulkWrite(const uint8_t* data, size_t len) = 0;

  /* Human readable reason for the last failure. */
  const std::string& last_error() const { return last_error_; }

 protected:
  void SetError(const std::string& message) { last_error_ = message; }
  void SetWin32Error(const char* what, unsigned long code);

  std::string last_error_;
};

struct DeviceLocation {
  uint16_t vid = 0;
  uint16_t pid = 0;
  std::wstring path;      /* device interface path */
  std::wstring instance;  /* human readable instance id, best effort */
  /* Same value for every interface of one physical dongle, which is how the
   * HID control interface is matched to the right USB data interface when
   * more than one adapter is plugged in. */
  GUID container_id = {};
  bool has_container_id = false;
};

/* Reads DEVPKEY_Device_ContainerId for a device interface path. */
bool GetContainerIdForInterface(const std::wstring& interface_path,
                                const GUID& interface_class, GUID* container);

}  // namespace ms912x
