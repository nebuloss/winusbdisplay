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
  void CancelTransfers() override;

  /* Writes a human readable dump of every interface setting and pipe. Used by
   * the enumerate phase to record what the hardware actually exposes. */
  bool DumpDescriptors(std::string* out);

  /* Reads the Binary Object Store descriptor, which only USB 3 capable
   * devices have. Returns false when the device has none. */
  bool DumpBosDescriptor(std::string* out);

  /* Turns on pipelined transfers: the frame is split into chunks and several
   * are kept in flight at once, so the host controller always has data queued
   * and the bus does not idle between transfers. A single synchronous write
   * per frame leaves roughly a third of the bus unused.
   *
   * RAW_IO is enabled alongside, which removes WinUSB's own buffering. It
   * requires every chunk except the last to be a multiple of the maximum
   * packet size, which ChunkSize() guarantees. */
  bool EnablePipelining(unsigned depth, size_t chunk_bytes);

  uint16_t max_packet_size() const { return bulk_max_packet_; }
  bool pipelined() const { return pipeline_depth_ > 1; }

 private:
  bool FindBulkOutPipe(std::string* error);

  void* file_handle_ = nullptr;      /* HANDLE */
  void* winusb_handle_ = nullptr;    /* WINUSB_INTERFACE_HANDLE */
  bool BulkWritePipelined(const uint8_t* data, size_t len);

  uint8_t bulk_out_pipe_id_ = 0;
  unsigned pipeline_depth_ = 1;
  size_t chunk_bytes_ = 0;

  /* One reusable event per in-flight chunk. Creating these per frame would
   * cost more than the pipelining saves. */
  struct PipelineSlot {
    OVERLAPPED overlapped = {};
    bool busy = false;
  };
  std::vector<PipelineSlot> slots_;
  uint16_t bulk_max_packet_ = 0;
  DeviceLocation location_;
};

}  // namespace ms912x
