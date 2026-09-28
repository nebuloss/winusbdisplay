/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Which devices exist on this machine, and how to open one.
 *
 * Separate from the chip implementations because this is the only part that
 * has to know about the operating system it is running on. Keeping it apart
 * means the protocol logic and its tests build anywhere, and it is also the
 * single place to add a probe when a second adapter is supported.
 */

#include "macrosilicon.h"
#include "usb.h"

namespace usbdisplay {

std::unique_ptr<DisplayDevice> OpenDisplayDevice(std::string* error) {
  /* One probe today. A second supported adapter would add another here, and
   * nothing else in the driver would change. */
  std::unique_ptr<UsbLink> link = UsbLink::Open(true, error);
  if (!link) {
    return nullptr;
  }
  return std::unique_ptr<DisplayDevice>(
      new MacroSiliconDevice(std::move(link)));
}

std::unique_ptr<DisplayDevice> OpenLoopbackDevice(const std::string& directory,
                                                  std::string* error) {
  std::unique_ptr<FileLink> link = FileLink::Open(directory, error);
  if (!link) {
    return nullptr;
  }
  return std::unique_ptr<DisplayDevice>(
      new MacroSiliconDevice(std::move(link)));
}

}  // namespace usbdisplay
