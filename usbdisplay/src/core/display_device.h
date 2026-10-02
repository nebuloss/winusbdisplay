/* SPDX-License-Identifier: GPL-2.0-only
 *
 * What the rest of the driver is allowed to know about a display adapter.
 *
 * Everything above this interface, which is roughly four fifths of the code,
 * is about Windows and about deciding what to send. Everything below it is
 * about one particular chip. This file is the seam between the two, and it
 * exists so that supporting a second adapter means writing a new
 * implementation of this interface rather than editing the frame loop.
 *
 * ## Why these particular methods
 *
 * Each one is here because the frame pipeline genuinely cannot be written
 * without it, and because the answer differs between plausible devices. They
 * were not guessed at: every one was a MacroSilicon-specific fact hardcoded
 * somewhere it did not belong.
 *
 *   TransferCost          the MS912x completes transfers on its own 60 Hz
 *                         boundary, so cost is quantised into slots rather
 *                         than proportional to size. A device that simply
 *                         streams would return bytes. The damage planner
 *                         merges regions by comparing these numbers, so
 *                         getting it from the device is what makes the
 *                         planner device independent.
 *
 *   TransmissionsPerRegion
 *                         the MS912x keeps two copies of the picture and
 *                         alternates between them, so every region has to be
 *                         sent twice or the two alternate visibly on screen.
 *                         A single buffered device returns 1.
 *
 *   AlignRegion           UYVY encodes pixel pairs and this chip wants four
 *                         pixel horizontal granularity. A device taking RGB
 *                         would not care.
 *
 *   BytesPerRow, ConvertRow, Frame
 *                         the wire format: how wide a converted row is, how
 *                         to produce it, and what wraps it. All three differ
 *                         per device and the first two are also what the
 *                         comparison against the on-screen record works in.
 *
 * ## What is deliberately *not* here
 *
 * No notion of USB. How an implementation reaches its hardware is its own
 * business; `Link` is the abstraction for that and only MacroSilicon's
 * implementation currently uses it. A device on some other bus would simply
 * not have one.
 */

#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "../render/rect.h"
#include "mode.h"

namespace usbdisplay {

struct PictureAdjust;

class DisplayDevice : public TransferCostModel {
 public:
  virtual ~DisplayDevice() = default;

  /* ---- identity and capability ---------------------------------------- */

  /* One line for the log, naming the part and how it is reached. */
  virtual std::string Describe() const = 0;

  /* What is plugged into the adapter's output. Drives which modes are
   * offered: a composite output cannot do 1080p. */
  virtual bool ReadConnector(VideoPort* port) = 0;

  /* The attached screen's capabilities. `checksum_ok` reports corruption
   * separately, because a bad checksum is not an I/O failure and the caller
   * may reasonably choose to carry on with a synthesised description. */
  virtual bool ReadEdid(std::vector<uint8_t>* edid, int blocks,
                        bool* checksum_ok) = 0;

  /* Modes this adapter can drive into that connector, best first. */
  virtual std::vector<Mode> SupportedModes(VideoPort port) = 0;

  /* ---- the cost model -------------------------------------------------- */

  /* What sending this region costs, in whatever unit the device finds
   * natural, and comparable only against itself. The damage planner merges
   * two regions when the merged cost does not exceed the separate costs, so
   * the unit cancels out and never leaves this interface.
   *
   * Must be zero for an empty region and non-zero otherwise, and must never
   * decrease as a region grows. */
  int TransferCost(const Rect& region) const override = 0;

  /* How many times the same region must be transmitted for it to be fully
   * on screen. See the note above. */
  virtual int TransmissionsPerRegion() const = 0;

  /* ---- geometry and wire format ---------------------------------------- */

  /* Grows a region to whatever boundary the device requires, and clips it to
   * the screen. Must only ever grow: cropping here silently drops pixels
   * that were genuinely damaged. */
  virtual Rect AlignRegion(const Rect& region, int width,
                           int height) const = 0;

  /* Bytes one converted row of `width` pixels occupies. This is the unit the
   * driver's record of the on-screen image is kept in, so it has to be
   * exact. */
  virtual size_t BytesPerRow(int width) const = 0;

  /* Converts one row from the format the compositor produces, which is
   * always 32 bit BGRA, into the device's own. */
  virtual void ConvertRow(uint8_t* dst, const uint8_t* source,
                          int width) const = 0;

  /* Largest single transfer, so buffers can be sized once up front. */
  virtual size_t MaxTransferBytes() const = 0;

  /* Wraps already-converted pixels for the wire. `pixels` holds `region`
   * packed row by row; `sub` selects the part to send and must lie inside
   * it. Returns bytes written, or zero if the arguments do not agree. */
  virtual size_t Frame(uint8_t* destination, size_t capacity,
                       const uint8_t* pixels, const Rect& region,
                       const Rect& sub) const = 0;

  /* ---- driving it ------------------------------------------------------ */

  virtual bool PowerOn() = 0;
  virtual bool PowerOff() = 0;

  /* Programs a mode. Implementations must leave the output dark: the panel
   * still holds the previous session's picture, and lighting it before a
   * frame has landed shows that instead of the desktop. The first successful
   * transfer is what turns it on. */
  virtual bool SetMode(const Mode& mode) = 0;

  /* Sends one framed transfer. */
  virtual bool SendTransfer(const uint8_t* data, size_t length) = 0;

  /* Abandons the transfer in flight rather than waiting for it. Teardown
   * will not wait, and a driver that blocks is reported as hung. Called from
   * another thread by definition. */
  virtual void Cancel() = 0;

  /* Counts how often the adapter has been programmed.
   *
   * Programming clears its picture memory, and it can happen without the
   * frame loop asking: a run of failed transfers makes an implementation
   * reset its hardware. Watching this is how the pipeline learns that its
   * record of what is on screen is void and it must repaint rather than
   * send differences against it. */
  virtual uint64_t generation() const = 0;

  /* Whether the hardware is still attached.
   *
   * Asked once a second by the display driver, because nothing notifies it
   * when a USB device is pulled out. Implementations should answer from
   * whatever the operating system already knows rather than by talking to
   * the hardware: a request to a device that has just been unplugged can
   * block until it times out, and this is asked on the thread that most
   * needs to stay responsive at exactly that moment. */
  virtual bool StillPresent() const = 0;

  /* Whether a picture is actually reaching the panel.
   *
   * Separate from StillPresent, and from whether transfers succeed,
   * because on this hardware those are three different questions. An
   * adapter can be plugged in, accept every transfer, report its output
   * enabled, and display nothing.
   *
   * There is no way to infer this from the frame path, which is why it is
   * asked rather than deduced: the counters all look perfect while the
   * screen is dark. A device that cannot answer should return true, so
   * that nothing is done about a condition it cannot detect.
   *
   * Called occasionally rather than per frame, so an implementation may
   * talk to the hardware. */
  virtual bool DisplayingPicture() = 0;

  /* Reprograms the adapter from scratch, as though it had just been
   * opened: power, mode, and whatever else it takes to get a picture out.
   *
   * The way back from a device that has stopped displaying while still
   * accepting everything sent to it. Measured on the MacroSilicon parts:
   * a dark adapter is revived by exactly this, with no replug, which is
   * why it is on the interface rather than being a private detail. */
  virtual bool Revive() = 0;

  virtual const std::string& error() const = 0;
};

/* Finds and opens whatever is plugged in.
 *
 * The only place in the driver that names a concrete device. Adding support
 * for another adapter means adding a probe here and an implementation of the
 * interface above; nothing else in the tree needs to change.
 *
 * Returns null when no supported adapter is present, which is an ordinary
 * state rather than an error: the display driver starts anyway and waits for
 * one to be plugged in. */
std::unique_ptr<DisplayDevice> OpenDisplayDevice(std::string* error);

/* Opens a device that writes what it would have sent into a directory. Used
 * by the console tool to exercise the whole pipeline with no hardware. */
std::unique_ptr<DisplayDevice> OpenLoopbackDevice(const std::string& directory,
                                                  std::string* error);

}  // namespace usbdisplay
