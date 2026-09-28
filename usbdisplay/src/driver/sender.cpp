/* SPDX-License-Identifier: GPL-2.0-only */

#include "sender.h"

#include <windows.h>

#include "log.h"

namespace usbdisplay {

FrameSender::FrameSender(DisplayDevice* device) : device_(device) {
  for (Slot& slot : slots_) {
    slot.data.resize(device_->MaxTransferBytes());
  }
}

FrameSender::~FrameSender() { Stop(); }

void FrameSender::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_) {
    return;
  }
  running_ = true;
  worker_ = std::thread(&FrameSender::Worker, this);
}

void FrameSender::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return;
    }
    running_ = false;
  }
  work_cv_.notify_all();
  free_cv_.notify_all();

  /* Kill the transfer in flight rather than waiting for it. PnP stop will not
   * wait a hundred milliseconds for the bus, and a driver that makes it
   * reports as hung. */
  device_->Cancel();

  if (worker_.joinable()) {
    worker_.join();
  }
}

std::vector<uint8_t>* FrameSender::Acquire(unsigned wait_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto free_slot = [this]() -> Slot* {
    for (Slot& slot : slots_) {
      if (!slot.owned_by_producer && !slot.queued) {
        return &slot;
      }
    }
    return nullptr;
  };

  Slot* slot = free_slot();
  if (!slot && running_) {
    free_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                      [&] { return !running_ || free_slot() != nullptr; });
    slot = free_slot();
  }
  if (!slot) {
    return nullptr;
  }
  slot->owned_by_producer = true;
  return &slot->data;
}

void FrameSender::Submit(std::vector<uint8_t>* buffer, size_t length) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Slot& slot : slots_) {
      if (&slot.data == buffer) {
        slot.owned_by_producer = false;
        slot.queued = true;
        slot.length = length;
        slot.sequence = next_sequence_++;
        break;
      }
    }
  }
  work_cv_.notify_one();
}

void FrameSender::Release(std::vector<uint8_t>* buffer) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Slot& slot : slots_) {
      if (&slot.data == buffer) {
        slot.owned_by_producer = false;
        slot.queued = false;
        break;
      }
    }
  }
  free_cv_.notify_one();
}

bool FrameSender::Idle() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Slot& slot : slots_) {
    if (slot.queued || slot.owned_by_producer) {
      return false;
    }
  }
  return !busy_;
}

void FrameSender::Worker() {
  for (;;) {
    Slot* slot = nullptr;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_cv_.wait(lock, [this] {
        if (!running_) {
          return true;
        }
        for (const Slot& candidate : slots_) {
          if (candidate.queued) {
            return true;
          }
        }
        return false;
      });

      /* Stopping discards whatever is still queued rather than draining it.
       *
       * Draining looks tidier and is a trap: the pipe has just been aborted,
       * so every queued transfer fails, and three failures in a row make the
       * adapter be reprogrammed, which is a dozen more round trips. All of
       * it happens on the thread that teardown is waiting to join, and the
       * framework reports the delay as a hung driver and takes the device
       * offline. */
      if (!running_) {
        return;
      }

      /* Oldest queued transfer first. Sending them out of order would let a
       * later update be overwritten by an earlier one covering the same
       * pixels, because the adapter applies each transfer as it arrives. */
      for (Slot& candidate : slots_) {
        if (candidate.queued &&
            (!slot || candidate.sequence < slot->sequence)) {
          slot = &candidate;
        }
      }
      if (!slot) {
        continue;
      }
      busy_ = true;
    }

    if (!device_->SendTransfer(slot->data.data(), slot->length)) {
      ++failed_;
      Log("sender: transfer of %zu bytes failed: %s", slot->length,
          device_->error().c_str());
    } else {
      ++sent_;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      slot->queued = false;
      busy_ = false;
    }
    free_cv_.notify_one();
  }
}

}  // namespace usbdisplay
