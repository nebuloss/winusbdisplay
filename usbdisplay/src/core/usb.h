/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Getting at the dongle from Windows.
 *
 * The single most important fact about this hardware on Windows is that its
 * two planes live on different USB interfaces, and Windows gives different
 * drivers to different interfaces.
 *
 *   control  MI_00, a plain HID collection    hidusb.sys, already in the box
 *   pixels   MI_03, vendor class              WinUSB, via our INF
 *
 * The control transfers are HID class requests addressed to interface 0, so
 * HidD_SetFeature and HidD_GetFeature produce byte for byte the same traffic
 * as the vendor driver, with no INF, no signing and no reboot. WinUSB will
 * not proxy an interface-recipient control request to an interface it does
 * not own, so there is no way to fold the two together: this class holds one
 * handle for each and always will.
 *
 * The practical benefit is large. Everything except pixels, which is to say
 * chip identification, EDID, hotplug, flash and the entire modeset sequence,
 * can be exercised on a machine with no driver installed at all.
 */

#pragma once

#include <windows.h>

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "link.h"

namespace usbdisplay {

/* Somewhere to send control exchanges and pixels. Two implementations: the
 * real dongle, and a directory on disk. */
/* One physical dongle: its HID control interface and its WinUSB data
 * interface. */
class UsbLink : public Link {
 public:
  ~UsbLink() override;

  /* Opens the first adapter found. With `require_panel` false the data
   * interface is optional, which is how the read-only commands work on a
   * machine with no driver installed.
   *
   * When more than one dongle is plugged in, the two interfaces are paired by
   * container id, the property Windows gives every interface of one physical
   * device. Pairing them by enumeration order instead works right up until
   * somebody plugs in a second adapter and then sends one dongle's pixels
   * while reading the other's EDID. */
  static std::unique_ptr<UsbLink> Open(bool require_panel, std::string* error);

  std::string Describe() const override;
  bool HasPanel() const override { return winusb_ != nullptr; }
  bool ControlWrite(const uint8_t* payload) override;
  bool ControlRead(uint8_t* payload) override;
  bool BulkWrite(const uint8_t* data, size_t len) override;
  void Cancel() override;
  bool StillPresent() const override;

  uint16_t vid() const { return vid_; }
  uint16_t pid() const { return pid_; }

  /* One entry per interface Windows can see, for the `list` command. */
  struct Interface {
    std::wstring path;
    uint16_t vid = 0;
    uint16_t pid = 0;
    GUID container = {};
    bool has_container = false;
    bool is_hid = false;
  };
  static std::vector<Interface> Enumerate();

 private:
  UsbLink() = default;
  bool OpenHid(const std::wstring& path, std::string* error);
  bool OpenWinUsb(const std::wstring& path, std::string* error);

  HANDLE hid_ = nullptr;
  size_t feature_report_size_ = 0;
  std::vector<uint8_t> feature_scratch_;

  HANDLE winusb_file_ = nullptr;
  void* winusb_ = nullptr;
  uint8_t bulk_pipe_ = 0;
  unsigned bulk_packet_size_ = 0;

  uint16_t vid_ = 0;
  uint16_t pid_ = 0;
};

/* Writes what would have gone to the chip into a directory instead: one file
 * per bulk transfer, plus a log of the control exchanges.
 *
 * This exists because "the panel is black" has two completely different
 * causes, a broken frame pipeline or a broken transport, and separating them
 * without hardware is otherwise guesswork. Control reads return zeroes, so
 * anything that branches on a register value will take the zero branch. */
class FileLink : public Link {
 public:
  static std::unique_ptr<FileLink> Open(const std::string& directory,
                                        std::string* error);

  std::string Describe() const override;
  bool HasPanel() const override { return false; }
  bool ControlWrite(const uint8_t* payload) override;
  bool ControlRead(uint8_t* payload) override;
  bool BulkWrite(const uint8_t* data, size_t len) override;
  bool StillPresent() const override { return true; }

 private:
  std::string directory_;
  unsigned sequence_ = 0;
};

}  // namespace usbdisplay
