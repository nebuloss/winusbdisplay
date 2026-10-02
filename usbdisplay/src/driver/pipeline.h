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
#include <mutex>
#include <thread>
#include <vector>

#include "../core/display_device.h"
#include "../render/convert.h"
#include "../render/damage.h"
#include "../render/converter.h"
#include "sender.h"
#include "settings.h"

namespace usbdisplay {

class Pipeline {
 public:
  Pipeline(IDDCX_SWAPCHAIN swapchain, LUID render_adapter,
           HANDLE new_frame_event, DisplayDevice* device, FrameSender* sender,
           const Mode& mode);
  ~Pipeline();

  /* Direct3D is set up on the calling thread so a failure can be reported
   * before any worker exists. Doing it on the worker instead leaves the
   * worker running when the OS reclaims the swapchain, and destroying it
   * from there takes the whole driver host down with it. */
  bool Start();
  void Stop();

  /* Installs a gamma table from the operating system. Safe from another
   * thread: the frame loop picks it up on its next pass. */
  void SetGammaRamp(const GammaRamp& gamma);

 private:
  void Run();
  bool CreateDevice();

  /* Converts `rect` of `source` into scratch_, choosing the path by size.
   * Returns false if neither path could do it. */
  bool ConvertForSending(ID3D11Texture2D* source, const Rect& rect);

  /* Converts, refines and submits one region, twice, because the adapter
   * keeps two copies of the picture and alternates between them.
   *
   * `force` sends even when the region turns out to be identical to what is
   * already on screen; the idle repaint needs that, since its purpose is to
   * keep the signal alive rather than to change anything.
   *
   * Returns false when the update had to be abandoned, which obliges the
   * caller to abandon the rest of the frame too so nothing arrives out of
   * order. */
  bool SendRegion(ID3D11Texture2D* source, const Rect& rect, bool force);

  /* Sends `sub` from the converted pixels already in scratch_, which cover
   * `region`. Twice and all or nothing, because the adapter keeps two copies
   * of the picture. */
  bool SubmitConverted(const Rect& region, const Rect& sub);

  void ProcessFrame(const IDARG_OUT_RELEASEANDACQUIREBUFFER& buffer);

  /* Puts traffic on the wire when the desktop is still, repainting a band
   * from the last surface the compositor gave us, which stays valid until
   * the next acquire. Without this the panel blanks whenever nothing is
   * moving, because the compositor stops presenting and nothing else would
   * arrive to keep the signal alive. */
  void RefreshIdle();

  void RefreshSettings();

  /* Notices that the adapter has been reprogrammed behind this thread's
   * back, which voids everything known about what the panel is showing. */
  void CheckAdapterReprogrammed();
  void CheckGammaRamp();

  IDDCX_SWAPCHAIN swapchain_;
  LUID render_adapter_;
  HANDLE new_frame_event_;
  DisplayDevice* device_;
  FrameSender* sender_;
  Mode mode_;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  /* The last image the compositor handed over.
   *
   * Held so the idle repaint has something real to convert from. It stays
   * valid until the next acquire, and while the desktop is still there is
   * no next acquire, which is exactly when this is needed. */
  Microsoft::WRL::ComPtr<ID3D11Texture2D> last_surface_;

  ConverterSet converters_;

  /* The gamma table in force, and the one the frame loop is using. Kept
   * apart so the operating system can install one at any moment without
   * tearing a conversion that is already under way. */
  std::mutex gamma_lock_;
  GammaRamp pending_gamma_;
  bool gamma_changed_ = false;
  unsigned long long gamma_changed_at_ms_ = 0;
  GammaRamp gamma_;

  DamageTracker damage_;

  /* Reused every frame. The compositor reports how many regions changed and
   * then copies them into buffers we hand it, so these exist to avoid an
   * allocation on the frame path. */
  std::vector<RECT> dirty_rects_;
  std::vector<IDDCX_MOVEREGION> move_regions_;

  /* Converted pixels for the region being worked on, packed. */
  std::vector<uint8_t> scratch_;

  /* The whole screen as last sent, in the same format, so a region can be
   * compared against what the panel is actually showing. */
  std::vector<uint8_t> onscreen_;
  size_t onscreen_stride_ = 0;
  bool onscreen_valid_ = false;

  uint64_t adapter_generation_ = 0;
  Settings settings_;
  unsigned long long last_settings_poll_ms_ = 0;
  int idle_band_row_ = 0;
  /* Rows per keepalive update, worked out from what the device charges for
   * a transfer rather than assumed. A whole screen on parts where a
   * transfer costs the same whatever its size. */
  int idle_band_rows_ = 128;
  unsigned long long last_send_ms_ = 0;

  std::thread thread_;
  HANDLE terminate_event_ = nullptr;

  uint64_t regions_sent_ = 0;
  uint64_t regions_skipped_ = 0;
  uint64_t bytes_sent_ = 0;
  /* How many transfers have had their geometry logged. The first few are
   * worth seeing; a running log at sixty a second is not. */
  unsigned regions_logged_ = 0;
  /* Transmissions the device requires per region, cached so the frame path
   * does not make a virtual call per update. */
  int transmissions_ = 2;
};

}  // namespace usbdisplay
