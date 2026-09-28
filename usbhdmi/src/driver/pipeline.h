/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The frame loop: one per swapchain, on its own thread.
 *
 * What happens to a frame, in order:
 *
 *   acquire the surface the compositor just finished
 *   fold its dirty rectangles and move regions into the damage tracker
 *   ask the planner for the transfers to send, by cost
 *   for each, in order:
 *     convert it, on the GPU if it is large and on the processor if not
 *     compare the result with what is already on screen, send only the
 *       part that differs
 *     hand the bytes to the sender and move on
 *
 * The ordering constraints are spelled out in render/gpu_convert.h and are
 * not negotiable, since two updates in one frame can take different paths.
 *
 * Move regions deserve a note. When a window is dragged the compositor
 * reports the area it came from and the area it went to. Sending only the
 * destination is much cheaper and relies on the compositor separately
 * reporting the area the window uncovered, which it does. Merging the two
 * into one region does not: it turns a 75 KB update into a multi-megabyte
 * one for the whole duration of the drag. Since damage is now tracked as a
 * set rather than a single box, both regions can simply be added and the
 * planner decides whether keeping them apart is worth it.
 */

#pragma once

#include <windows.h>

#include <wdf.h>

#include <iddcx.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <thread>
#include <vector>

#include "../core/chip.h"
#include "../render/convert.h"
#include "../render/damage.h"
#include "../render/gpu_convert.h"
#include "sender.h"
#include "settings.h"

namespace usbhdmi {

class Pipeline {
 public:
  Pipeline(IDDCX_SWAPCHAIN swapchain, LUID render_adapter,
           HANDLE new_frame_event, Chip* chip, FrameSender* sender,
           const Mode& mode);
  ~Pipeline();

  /* Direct3D is set up on the calling thread so a failure can be reported
   * before any worker exists. Doing it on the worker instead leaves the
   * worker running when the OS reclaims the swapchain, and destroying it
   * from there takes the whole driver host down with it. */
  bool Start();
  void Stop();

 private:
  void Run();
  bool CreateDevice();
  bool EnsureStaging(int width, int height);

  /* Converts `rect` of `source` into scratch_, choosing the path by size.
   * Returns false if neither path could do it. */
  bool ConvertRegion(ID3D11Texture2D* source, const Rect& rect);
  bool ConvertOnCpu(ID3D11Texture2D* source, const Rect& rect);

  /* Converts, refines and submits one region. Returns false when the update
   * had to be abandoned, which obliges the caller to abandon the rest of the
   * frame too so nothing arrives out of order. */
  bool SendRegion(ID3D11Texture2D* source, const Rect& rect);

  void ProcessFrame(const IDARG_OUT_RELEASEANDACQUIREBUFFER& buffer);

  /* Repaints from the last surface the compositor gave us, which IddCx
   * guarantees stays valid until the next acquire. Without this the panel
   * blanks whenever the desktop is still, because the compositor stops
   * presenting and nothing would arrive to keep the signal alive. */
  void RefreshIdle();

  void RefreshSettings();

  IDDCX_SWAPCHAIN swapchain_;
  LUID render_adapter_;
  HANDLE new_frame_event_;
  Chip* chip_;
  FrameSender* sender_;
  Mode mode_;

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
  int staging_width_ = 0;
  int staging_height_ = 0;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> last_surface_;

  GpuConverter gpu_;
  bool gpu_usable_ = false;

  DamageTracker damage_;

  /* Converted pixels for the region being worked on, packed. */
  std::vector<uint8_t> scratch_;

  /* The whole screen as last sent, in the same format, so a region can be
   * compared against what the panel is actually showing. */
  std::vector<uint8_t> onscreen_;
  size_t onscreen_stride_ = 0;
  bool onscreen_valid_ = false;

  Settings settings_;
  unsigned long long last_settings_poll_ms_ = 0;
  unsigned long long last_send_ms_ = 0;

  std::thread thread_;
  HANDLE terminate_event_ = nullptr;

  uint64_t regions_sent_ = 0;
  uint64_t regions_skipped_ = 0;
  uint64_t gpu_conversions_ = 0;
  uint64_t cpu_conversions_ = 0;
};

}  // namespace usbhdmi
