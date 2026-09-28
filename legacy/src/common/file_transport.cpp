/* SPDX-License-Identifier: GPL-2.0-only */

#include "file_transport.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "ms912x_proto.h"

namespace ms912x {

std::unique_ptr<FileTransport> FileTransport::Create(
    const std::string& directory, std::string* error) {
  if (!CreateDirectoryA(directory.c_str(), nullptr) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    if (error) {
      *error = "could not create dump directory " + directory;
    }
    return nullptr;
  }
  std::unique_ptr<FileTransport> transport(new FileTransport());
  transport->directory_ = directory;
  return transport;
}

std::string FileTransport::Describe() const {
  return "loopback, frames written to " + directory_;
}

bool FileTransport::ControlSetReport(const uint8_t*, size_t len) {
  return len == kControlPayloadSize;
}

bool FileTransport::ControlGetReport(uint8_t* data, size_t len) {
  if (len != kControlPayloadSize) {
    return false;
  }
  memset(data, 0, len);
  return true;
}

bool FileTransport::BulkWrite(const uint8_t* data, size_t len) {
  if (len == 0) {
    return true; /* end of frame marker, nothing to record */
  }
  if (len < kFrameOverhead) {
    SetError("transfer shorter than the frame overhead");
    return false;
  }

  char path[MAX_PATH];
  _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\frame-%04u.uyvy",
              directory_.c_str(), frame_index_);
  FILE* file = nullptr;
  if (fopen_s(&file, path, "wb") != 0 || !file) {
    SetError(std::string("could not open ") + path);
    return false;
  }
  fwrite(data + kFrameHeaderSize, 1, len - kFrameOverhead, file);
  fclose(file);

  uint32_t position = (static_cast<uint32_t>(data[2]) << 16) |
                      (static_cast<uint32_t>(data[3]) << 8) | data[4];
  uint32_t dimensions = (static_cast<uint32_t>(data[5]) << 16) |
                        (static_cast<uint32_t>(data[6]) << 8) | data[7];

  _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\frame-%04u.txt",
              directory_.c_str(), frame_index_);
  if (fopen_s(&file, path, "w") == 0 && file) {
    fprintf(file,
            "marker     %02X%02X\n"
            "x          %u\n"
            "y          %u\n"
            "width      %u\n"
            "height     %u\n"
            "payload    %zu bytes\n"
            "footer     %02X %02X %02X %02X %02X %02X %02X %02X\n",
            data[0], data[1], (position >> 12) & 0xFFF, position & 0xFFF,
            (dimensions >> 12) & 0xFFF, dimensions & 0xFFF,
            len - kFrameOverhead, data[len - 8], data[len - 7], data[len - 6],
            data[len - 5], data[len - 4], data[len - 3], data[len - 2],
            data[len - 1]);
    fclose(file);
  }

  ++frame_index_;
  return true;
}

}  // namespace ms912x
