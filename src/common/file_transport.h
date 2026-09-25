/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Loopback data plane. Records each would-be bulk transfer to disk so that
 * conversion bugs can be told apart from transport bugs without hardware.
 * Control transfers are answered with zeros.
 */

#pragma once

#include <memory>
#include <string>

#include "transport.h"

namespace ms912x {

class FileTransport : public Transport {
 public:
  /* Frames are written to <directory>/frame-NNNN.uyvy alongside a .txt with
   * the decoded header fields. */
  static std::unique_ptr<FileTransport> Create(const std::string& directory,
                                               std::string* error);

  std::string Describe() const override;
  bool HasDataPlane() const override { return true; }
  bool ControlSetReport(const uint8_t* data, size_t len) override;
  bool ControlGetReport(uint8_t* data, size_t len) override;
  bool BulkWrite(const uint8_t* data, size_t len) override;

  unsigned frames_written() const { return frame_index_; }

 private:
  std::string directory_;
  unsigned frame_index_ = 0;
};

}  // namespace ms912x
