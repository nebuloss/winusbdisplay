/* SPDX-License-Identifier: GPL-2.0-only */

#include "ddcci.h"

#include <windows.h>

#include <cstring>

#include "log.h"

namespace ms912x {
namespace {

constexpr uint8_t kHostAddress = 0x51;
constexpr uint8_t kDisplayAddress = 0x6E;
constexpr uint8_t kVirtualHostAddress = 0x50;

constexpr uint8_t kOpGetVcp = 0x01;
constexpr uint8_t kOpGetVcpReply = 0x02;
constexpr uint8_t kOpSetVcp = 0x03;
constexpr uint8_t kOpCapabilitiesRequest = 0xF3;
constexpr uint8_t kOpCapabilitiesReply = 0xE3;

/* One window of a capabilities reply. The reader walks the string 32 bytes at
 * a time and stops when a reply comes back short. */
constexpr size_t kCapabilitiesChunk = 32;

uint8_t Checksum(uint8_t seed, const uint8_t* data, size_t len) {
  uint8_t sum = seed;
  for (size_t i = 0; i < len; ++i) {
    sum ^= data[i];
  }
  return sum;
}

}  // namespace

DdcCiSlave::DdcCiSlave() {
  /* Advertise only what is actually implemented. Claiming more makes
   * utilities offer controls that then silently do nothing. */
  capabilities_ =
      "(prot(monitor)type(lcd)model(MS912x)cmds(01 02 03 F3)"
      "vcp(10 12)mswhql(1))";
}

int DdcCiSlave::brightness() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return brightness_;
}

int DdcCiSlave::contrast() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return contrast_;
}

void DdcCiSlave::QueueReply(const uint8_t* message, size_t len) {
  pending_.clear();
  pending_.push_back(kDisplayAddress);
  pending_.push_back(static_cast<uint8_t>(0x80 | len));
  pending_.insert(pending_.end(), message, message + len);
  pending_.push_back(
      Checksum(kVirtualHostAddress, pending_.data(), pending_.size()));
}

void DdcCiSlave::HandleMessage(const uint8_t* message, size_t len) {
  if (len == 0) {
    return;
  }

  switch (message[0]) {
    case kOpGetVcp: {
      if (len < 2) {
        return;
      }
      const uint8_t vcp = message[1];
      uint16_t current = 0;
      uint16_t maximum = 100;
      uint8_t result = 0; /* 0 = supported */

      if (vcp == kVcpBrightness) {
        current = static_cast<uint16_t>(brightness_);
      } else if (vcp == kVcpContrast) {
        current = static_cast<uint16_t>(contrast_);
      } else {
        result = 1; /* unsupported VCP code */
      }

      const uint8_t reply[] = {
          kOpGetVcpReply,
          result,
          vcp,
          0x00, /* set parameter, not momentary */
          static_cast<uint8_t>(maximum >> 8),
          static_cast<uint8_t>(maximum & 0xFF),
          static_cast<uint8_t>(current >> 8),
          static_cast<uint8_t>(current & 0xFF),
      };
      QueueReply(reply, sizeof(reply));
      break;
    }

    case kOpSetVcp: {
      if (len < 4) {
        return;
      }
      const uint8_t vcp = message[1];
      int value = (message[2] << 8) | message[3];
      if (value < 0) {
        value = 0;
      }
      if (value > 100) {
        value = 100;
      }
      if (vcp == kVcpBrightness) {
        brightness_ = value;
        Log("ddcci: brightness -> %d", value);
      } else if (vcp == kVcpContrast) {
        contrast_ = value;
        Log("ddcci: contrast -> %d", value);
      }
      /* Set has no reply. */
      pending_.clear();
      break;
    }

    case kOpCapabilitiesRequest: {
      if (len < 3) {
        return;
      }
      const size_t offset = (message[1] << 8) | message[2];
      std::vector<uint8_t> reply;
      reply.push_back(kOpCapabilitiesReply);
      reply.push_back(static_cast<uint8_t>(offset >> 8));
      reply.push_back(static_cast<uint8_t>(offset & 0xFF));
      if (offset < capabilities_.size()) {
        const size_t remaining = capabilities_.size() - offset;
        const size_t chunk =
            remaining < kCapabilitiesChunk ? remaining : kCapabilitiesChunk;
        reply.insert(reply.end(), capabilities_.begin() + offset,
                     capabilities_.begin() + offset + chunk);
      }
      QueueReply(reply.data(), reply.size());
      break;
    }

    default:
      /* Unknown opcode: stay silent rather than replying with nonsense. */
      pending_.clear();
      break;
  }
}

namespace {

/* Reads a DWORD, clamped to 0..100. Returns the fallback when absent. */
int ReadRegistryValue(const wchar_t* name, int fallback) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\winusbdisplay", 0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return fallback;
  }
  DWORD value = 0;
  DWORD size = sizeof(value);
  DWORD type = 0;
  int result = fallback;
  if (RegQueryValueExW(key, name, nullptr, &type,
                       reinterpret_cast<LPBYTE>(&value),
                       &size) == ERROR_SUCCESS &&
      type == REG_DWORD) {
    result = static_cast<int>(value);
    if (result < 0) {
      result = 0;
    }
    if (result > 100) {
      result = 100;
    }
  }
  RegCloseKey(key);
  return result;
}

}  // namespace

bool DdcCiSlave::RefreshFromRegistry() {
  std::lock_guard<std::mutex> lock(mutex_);
  const int brightness = ReadRegistryValue(L"Brightness", brightness_);
  const int contrast = ReadRegistryValue(L"Contrast", contrast_);
  if (brightness == brightness_ && contrast == contrast_) {
    return false;
  }
  Log("ddcci: registry brightness %d -> %d, contrast %d -> %d", brightness_,
      brightness, contrast_, contrast);
  brightness_ = brightness;
  contrast_ = contrast;
  return true;
}

bool DdcCiSlave::Transmit(uint32_t address, const uint8_t* data, size_t len) {
  if (address != kDdcI2cAddress7Bit || len < 3) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);

  if (data[0] != kHostAddress) {
    return false;
  }
  const size_t message_len = data[1] & 0x7F;
  if (message_len + 3 > len) {
    return false;
  }

  /* The checksum covers the 8-bit destination address as sent on the wire. */
  const uint8_t expected =
      Checksum(kDisplayAddress, data, message_len + 2);
  if (expected != data[message_len + 2]) {
    Log("ddcci: bad checksum, got 0x%02X want 0x%02X", data[message_len + 2],
        expected);
    return false;
  }

  HandleMessage(data + 2, message_len);
  return true;
}

bool DdcCiSlave::Receive(uint32_t address, uint8_t* data, size_t len) {
  if (address != kDdcI2cAddress7Bit) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);

  if (pending_.empty()) {
    return false;
  }
  memset(data, 0, len);
  const size_t copy = pending_.size() < len ? pending_.size() : len;
  memcpy(data, pending_.data(), copy);
  return true;
}

}  // namespace ms912x
