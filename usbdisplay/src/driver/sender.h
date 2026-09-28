/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The thread that owns the USB write, and the queue in front of it.
 *
 * Two rules shape this class, and both were learned the hard way.
 *
 * **Never block the compositor's thread on USB.** A full screen transfer owns
 * the bus for over a hundred milliseconds. Doing that on the thread that
 * acquires frames makes the desktop stutter, and if it goes on long enough
 * Windows decides the monitor has hung and removes it. So conversion happens
 * on the frame thread and the finished bytes are handed over here.
 *
 * **Drop, do not queue.** When no buffer is free the update is abandoned.
 * That is safe because the damage it covered stays owed and a later update
 * will carry it; the alternative, waiting for a buffer, builds latency
 * without bound and ends in the same hang. What the caller must not do is
 * skip one update and send the next: see the ordering note below.
 *
 * **Order is preserved.** This is a FIFO, not a pool of interchangeable
 * buffers. The adapter has no concept of a frame and simply applies each
 * transfer to the region it names as it arrives, so an update that overtakes
 * an earlier one covering the same pixels leaves the older content on screen.
 * Submitted transfers therefore go out in submission order, always.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "../core/display_device.h"

namespace usbdisplay {

/* Every region goes out twice, once for each of the adapter's two internal
 * copies of the picture, so two buffers is exactly one region and the
 * producer stalls on every single update. Four lets one region be queued
 * while another is on the wire. More than that would only add latency: the
 * adapter, not the queue, is the bottleneck. */
constexpr size_t kTransferSlots = 4;

class FrameSender {
 public:
  explicit FrameSender(DisplayDevice* device);
  ~FrameSender();

  void Start();
  void Stop();

  /* Returns a buffer to convert into, or nullptr if none is free within
   * `wait_ms` and the caller should abandon this update. */
  std::vector<uint8_t>* Acquire(unsigned wait_ms);

  /* Queues a buffer returned by Acquire. Transfers leave in the order they
   * were submitted. */
  void Submit(std::vector<uint8_t>* buffer, size_t length);

  /* Hands a buffer back without sending it. */
  void Release(std::vector<uint8_t>* buffer);

  /* True when nothing is queued and nothing is on the wire.
   *
   * Used to decide whether the link has spare capacity. Deliberately "the
   * queue is completely empty" rather than "a buffer is free": filling the
   * queue with speculative work would keep the adapter busy at the cost of
   * making every real update wait behind it, which is the opposite of what
   * spare capacity is for. */
  bool Idle();

  uint64_t sent() const { return sent_; }
  uint64_t dropped() const { return dropped_; }
  uint64_t failed() const { return failed_; }

  void CountDropped() { ++dropped_; }

 private:
  void Worker();

  struct Slot {
    std::vector<uint8_t> data;
    size_t length = 0;
    bool owned_by_producer = false;  /* handed out by Acquire */
    bool queued = false;
    uint64_t sequence = 0;
  };

  DisplayDevice* device_;
  Slot slots_[kTransferSlots];
  uint64_t next_sequence_ = 0;

  std::mutex mutex_;
  std::condition_variable free_cv_;
  std::condition_variable work_cv_;
  std::thread worker_;
  bool running_ = false;
  bool busy_ = false;  /* a transfer is on the wire right now */

  std::atomic<uint64_t> sent_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> failed_{0};
};

}  // namespace usbdisplay
