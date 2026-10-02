/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The protocol layer, against a fake adapter.
 *
 * These are the tests that would have caught the historical failures. Every
 * one of the following cost real debugging time, and every one is silent:
 * the transfers all report success and the panel simply stays dark or shows
 * the wrong thing.
 *
 *   - forgetting the zero length packet that ends a transfer
 *   - enabling the output before a frame has landed, so the panel shows
 *     whatever was left in the adapter's memory
 *   - reordering the mode programming sequence
 *   - not selecting manual block mode, after which partial updates are
 *     refused and every update has to be a whole frame
 *   - reading registers one byte at a time, making an EDID read four times
 *     slower than it needs to be
 */

#include <map>
#include <set>
#include <cstring>
#include <vector>

#include "../src/core/macrosilicon.h"
#include "testing.h"

using namespace usbdisplay;

namespace {

/* Records everything sent and answers reads from a register map, so a
 * sequence can be asserted on without any hardware. */
class FakeLink : public Link {
 public:
  struct Control {
    uint8_t bytes[kControlSize];

    uint8_t op() const { return bytes[0]; }
    uint8_t sub_op() const { return bytes[1]; }
    uint16_t address() const {
      return static_cast<uint16_t>((bytes[1] << 8) | bytes[2]);
    }
  };

  struct Bulk {
    size_t length;
    std::vector<uint8_t> head;  /* first bytes, enough to check the header */
  };

  std::string Describe() const override { return "fake adapter"; }
  bool HasPanel() const override { return has_panel; }
  bool StillPresent() const override { return present; }

  bool ControlWrite(const uint8_t* payload) override {
    Control record;
    memcpy(record.bytes, payload, kControlSize);
    controls.push_back(record);

    if (record.op() == kOpReadXdata) {
      pending_read_address_ = record.address();
      pending_read_is_flash_ = false;
    } else if (record.op() == kOpReadFlash) {
      pending_read_address_ = static_cast<uint16_t>(
          (payload[1] << 16) | (payload[2] << 8) | payload[3]);
      pending_read_is_flash_ = true;
    }
    return !fail_control;
  }

  bool ControlRead(uint8_t* payload) override {
    memset(payload, 0, kControlSize);
    if (fail_control) {
      return false;
    }
    if (pending_read_is_flash_) {
      return true; /* unprogrammed flash reads back as zeroes */
    }
    if (unreadable_.count(static_cast<uint16_t>(pending_read_address_))) {
      return false;
    }
    /* The response carries four consecutive register bytes at offset 3. */
    for (size_t i = 0; i < kMaxReadBytes; ++i) {
      const uint16_t address =
          static_cast<uint16_t>(pending_read_address_ + i);
      auto it = registers.find(address);
      payload[3 + i] = it == registers.end() ? 0 : it->second;
    }
    reads_served++;
    return true;
  }

  bool BulkWrite(const uint8_t* data, size_t len) override {
    Bulk record;
    record.length = len;
    if (data && len) {
      record.head.assign(data, data + (len < 16 ? len : 16));
    }
    bulks.push_back(record);
    return !fail_bulk;
  }

  /* Finds the first video command with this sub-operation, or -1. */
  int IndexOfCommand(uint8_t sub_op) const {
    for (size_t i = 0; i < controls.size(); ++i) {
      if (controls[i].op() == kOpVideo && controls[i].sub_op() == sub_op) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  /* Makes one register refuse to be read, for the paths that have to cope
   * with a chip that will not answer a particular question. */
  void FailReadsAt(uint16_t address) { unreadable_.insert(address); }

  std::vector<Control> controls;
  std::vector<Bulk> bulks;
  std::map<uint16_t, uint8_t> registers;
  bool has_panel = true;
  bool present = true;
  bool fail_control = false;
  bool fail_bulk = false;
  int reads_served = 0;

 private:
  uint32_t pending_read_address_ = 0;
  bool pending_read_is_flash_ = false;
  std::set<uint16_t> unreadable_;
};

/* Keeps a borrowed pointer to the fake while Chip owns it. */
struct Harness {
  FakeLink* link;
  std::unique_ptr<MacroSiliconDevice> chip;

  Harness() {
    auto owned = std::unique_ptr<FakeLink>(new FakeLink());
    link = owned.get();
    chip.reset(new MacroSiliconDevice(std::move(owned)));
  }
};

const Mode& Mode1080p60() { return *FindMode(1920, 1080, 60); }

}  // namespace

TEST(protocol, register_read_is_a_write_then_a_read) {
  Harness harness;
  harness.link->registers[0x0031] = 0x05;

  uint8_t value = 0;
  CHECK(harness.chip->ReadByte(0x0031, &value));
  CHECK_EQ(static_cast<int>(value), 5);

  CHECK_EQ_BECAUSE(harness.link->controls.size(), static_cast<size_t>(1),
                   "a read is one request followed by fetching the answer; "
                   "interleaving two of these returns each other's data");
  CHECK_EQ(static_cast<int>(harness.link->controls[0].op()), kOpReadXdata);
  CHECK_EQ(static_cast<int>(harness.link->controls[0].address()), 0x0031);
}

TEST(protocol, connector_type_is_decoded) {
  Harness harness;
  harness.link->registers[0x0031] = 0x05;

  VideoPort port = VideoPort::kUnknown;
  CHECK(harness.chip->ReadConnector(&port));
  CHECK(port == VideoPort::kHdmi);
  CHECK_EQ(std::string(VideoPortName(port)), std::string("HDMI"));
}

TEST(protocol, an_out_of_range_connector_is_reported_as_unknown) {
  Harness harness;
  harness.link->registers[0x0031] = 0x42;

  VideoPort port = VideoPort::kHdmi;
  CHECK(harness.chip->ReadConnector(&port));
  CHECK_BECAUSE(port == VideoPort::kUnknown,
                "an unrecognised value must not be cast into the enum and "
                "used to pick a mode list");
}

TEST(protocol, edid_is_read_four_bytes_at_a_time) {
  Harness harness;
  /* A minimal valid EDID: the magic, then a correcting checksum. */
  uint8_t block[128] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  uint8_t sum = 0;
  for (int i = 0; i < 127; ++i) {
    sum = static_cast<uint8_t>(sum + block[i]);
  }
  block[127] = static_cast<uint8_t>(256 - sum);
  for (int i = 0; i < 128; ++i) {
    harness.link->registers[static_cast<uint16_t>(kRegEdidBase + i)] =
        block[i];
  }

  std::vector<uint8_t> edid;
  bool checksum_ok = false;
  CHECK(harness.chip->ReadEdid(&edid, 1, &checksum_ok));
  CHECK_EQ(edid.size(), static_cast<size_t>(128));
  CHECK(checksum_ok);
  CHECK_EQ(memcmp(edid.data(), block, 128), 0);

  CHECK_EQ_BECAUSE(harness.link->reads_served, 32,
                   "one request returns four consecutive bytes, so a block "
                   "costs 32 round trips rather than 128; the reverse "
                   "engineered driver read one at a time and the extra "
                   "cost showed as a stall on every hotplug");
}

TEST(protocol, a_corrupt_edid_is_reported_rather_than_hidden) {
  Harness harness;
  /* A block of zeroes would pass, since its bytes sum to zero. One stray
   * value is what a real truncated or noisy read looks like. */
  harness.link->registers[kRegEdidBase] = 0x12;

  std::vector<uint8_t> edid;
  bool checksum_ok = true;
  CHECK_BECAUSE(harness.chip->ReadEdid(&edid, 1, &checksum_ok),
                "a bad checksum is not an I/O failure; the caller decides "
                "whether to fall back rather than being told nothing");
  CHECK(!checksum_ok);
}

TEST(protocol, chip_id_falls_back_to_the_usb2_address) {
  Harness harness;
  /* Nothing at the USB 3 address, an MS912C signature at the other. */
  harness.link->registers[kRegChipId912x] = kSignaturePart912C;
  harness.link->registers[kRegChipId912x + 1] = kSignatureFamily912x;
  harness.link->registers[kRegChipId912x + 2] = kSignatureTail;

  ChipId id;
  CHECK(harness.chip->ReadChipId(&id));
  CHECK_BECAUSE(id.model == ChipModel::kMs912C,
                "the test adapter reports a USB 3 product id and contains a "
                "USB 2 die, so the family must be probed, never inferred");
  CHECK_EQ(id.flash_timing_base, kFlashTimingBase912x);
}

TEST(protocol, an_unknown_chip_id_is_not_an_error) {
  Harness harness;
  ChipId id;
  CHECK_BECAUSE(harness.chip->ReadChipId(&id),
                "an unrecognised signature still leaves the raw bytes for "
                "the caller to report, which is more useful than a failure");
  CHECK(id.model == ChipModel::kUnknown);
}

/* ---- noticing that the panel has gone dark ---------------------------- */

TEST(health, a_chip_that_reports_showing_is_left_alone) {
  Harness harness;
  /* The detector is a USB 3 fact, so the chip has to say it is one. */
  harness.link->registers[kRegChipId913x] = kSignaturePart912A;
  harness.link->registers[kRegChipId913x + 1] = kSignatureFamily913x;
  harness.link->registers[kRegChipId913x + 2] = kSignatureTail;
  ChipId id;
  CHECK(harness.chip->ReadChipId(&id));
  harness.link->registers[kRegDisplayLive + 2] = kDisplayLiveShowing;
  CHECK(harness.chip->DisplayingPicture());
}

TEST(health, a_chip_that_reports_dark_is_detected) {
  Harness harness;
  /* The detector is a USB 3 fact, so the chip has to say it is one. */
  harness.link->registers[kRegChipId913x] = kSignaturePart912A;
  harness.link->registers[kRegChipId913x + 1] = kSignatureFamily913x;
  harness.link->registers[kRegChipId913x + 2] = kSignatureTail;
  ChipId id;
  CHECK(harness.chip->ReadChipId(&id));
  harness.link->registers[kRegDisplayLive + 2] = 0x01;
  CHECK_BECAUSE(!harness.chip->DisplayingPicture(),
                "every transfer succeeds whether or not a picture is on "
                "the glass, so asking the adapter is the only way to know");
}

TEST(health, a_chip_that_will_not_answer_is_given_the_benefit_of_the_doubt) {
  Harness harness;
  harness.link->FailReadsAt(kRegDisplayLive);
  CHECK_BECAUSE(harness.chip->DisplayingPicture(),
                "a failed read is not evidence of a dark panel, and "
                "reprogramming the adapter over one would turn a glitch "
                "into a visible interruption");
}

TEST(health, a_chip_without_the_register_is_never_called_dark) {
  Harness harness;
  /* A USB 2 part, identified the way the chip identifies itself. */
  harness.link->registers[kRegChipId912x] = kSignaturePart912C;
  harness.link->registers[kRegChipId912x + 1] = kSignatureFamily912x;
  harness.link->registers[kRegChipId912x + 2] = kSignatureTail;
  ChipId id;
  CHECK(harness.chip->ReadChipId(&id));
  CHECK(id.model == ChipModel::kMs912C);

  /* The register the detector reads exists only on the USB 3 parts.
   * On an MS912C the same address reads zero whether or not a picture
   * is on the glass, measured against a frame that visibly lit the
   * panel. Reading that as dark would have the driver reprogram the
   * adapter every few seconds for ever. */
  harness.link->registers[kRegDisplayLive + 2] = 0x00;
  CHECK_BECAUSE(harness.chip->DisplayingPicture(),
                "a part that cannot answer must not be treated as "
                "answering no, or the cure becomes the fault");
}

TEST(health, every_part_asks_for_a_keepalive) {
  Harness harness;
  /* Removing it on the USB 3 parts was tried, on the strength of a
   * measurement that said they hold a picture through two minutes of
   * silence. The panel went black within seconds. Whatever that
   * measurement was reading, it was not what reaches the glass. */
  CHECK_BECAUSE(harness.chip->KeepaliveMs() > 0,
                "both families blank without traffic, however much the "
                "registers suggest otherwise");
}
TEST(health, reviving_reprograms_the_mode) {
  Harness harness;
  CHECK(harness.chip->SetMode(Mode1080p60()));
  harness.link->controls.clear();

  CHECK(harness.chip->Revive());
  CHECK_BECAUSE(harness.link->IndexOfCommand(kVideoOutputInfo) >= 0,
                "a dark adapter comes back when the mode is programmed "
                "again, which is the only thing the console tool does "
                "that the driver did not");
}

/* ---- what each family costs -------------------------------------------- */

namespace {

/* A full 1080p frame, the update whose cost separates the two families. */
Rect FullFrame() { return Rect{0, 0, 1920, 1080}; }

void IdentifyAs912C(Harness& harness) {
  harness.link->registers[kRegChipId912x] = kSignaturePart912C;
  harness.link->registers[kRegChipId912x + 1] = kSignatureFamily912x;
  harness.link->registers[kRegChipId912x + 2] = kSignatureTail;
  ChipId id;
  CHECK(harness.chip->ReadChipId(&id));
  CHECK(id.model == ChipModel::kMs912C);
}

void IdentifyAs9132(Harness& harness) {
  harness.link->registers[kRegChipId913x] = kSignaturePart912A;
  harness.link->registers[kRegChipId913x + 1] = kSignatureFamily913x;
  harness.link->registers[kRegChipId913x + 2] = kSignatureTail;
  ChipId id;
  CHECK(harness.chip->ReadChipId(&id));
  CHECK(id.model == ChipModel::kMs9132);
}

}  // namespace

TEST(cost, a_usb2_chip_charges_eight_periods_for_a_full_frame) {
  Harness harness;
  IdentifyAs912C(harness);
  /* Measured: 491 KB took 16.7 ms and 553 KB took 33.2 ms, so roughly
   * 520 KB fits in a period and a 4.1 MB frame spans eight of them. This is
   * what makes merging distant regions ruinous on these parts. */
  CHECK_EQ(harness.chip->TransferCost(FullFrame()), 8);
}

TEST(cost, a_usb3_chip_charges_one_period_for_a_full_frame) {
  Harness harness;
  IdentifyAs9132(harness);
  /* Measured on the MS9132: a 30 KB update took 14.5 ms and a 4.1 MB full
   * frame took 17.1 ms. Both are one 60 Hz period, so size is free and only
   * the number of transfers costs anything. */
  CHECK_EQ(harness.chip->TransferCost(FullFrame()), 1);
}

TEST(cost, size_is_free_on_a_usb3_chip) {
  Harness harness;
  IdentifyAs9132(harness);
  CHECK_BECAUSE(
      harness.chip->TransferCost(Rect{0, 0, 64, 8}) ==
          harness.chip->TransferCost(FullFrame()),
      "when every transfer costs one period the planner should merge "
      "freely, which only holds if a tiny update and a whole frame price "
      "the same");
}

TEST(cost, an_unidentified_chip_is_charged_as_the_slower_family) {
  Harness harness;
  /* Deliberately without identifying it. Guessing the faster model on a
   * slower chip turns ordinary damage into full-screen repaints; guessing
   * the slower one on a faster chip merely splits updates that could have
   * been merged. Only one of those is a bug a user would notice. */
  CHECK_EQ(harness.chip->TransferCost(FullFrame()), 8);
}

/* ---- unmuting the output ----------------------------------------------- */

namespace {

/* The value written to a register, or -1 if it was never written. */
int WroteTo(const FakeLink& link, uint16_t address) {
  for (const FakeLink::Control& control : link.controls) {
    if (control.op() == kOpWriteXdataByte && control.address() == address) {
      return control.bytes[3];
    }
  }
  return -1;
}

void BringUpHdmi(Harness& harness, uint16_t chip_register, uint8_t part,
                 uint8_t family, uint16_t mute_register, uint8_t mute_value) {
  harness.link->registers[chip_register] = part;
  harness.link->registers[chip_register + 1] = family;
  harness.link->registers[chip_register + 2] = kSignatureTail;
  harness.link->registers[kRegVideoPort] = 0x05; /* HDMI */
  harness.link->registers[mute_register] = mute_value;

  CHECK(harness.chip->SetMode(Mode1080p60()));
  CHECK(harness.chip->EnableOutput(true));
}

}  // namespace

TEST(mute, a_usb3_chip_is_unmuted_at_its_own_register) {
  Harness harness;
  BringUpHdmi(harness, kRegChipId913x, kSignaturePart912A, kSignatureFamily913x,
              kRegHdmiMute913x, 0xFF);
  CHECK_BECAUSE(
      WroteTo(*harness.link, kRegHdmiMute913x) == (0xFF & ~kHdmiMuteBit),
      "the mute bit has to be cleared for a picture to leave the chip, and "
      "the USB 3 parts keep it at a different address from the USB 2 ones");
  CHECK_BECAUSE(WroteTo(*harness.link, kRegHdmiMute912x) == -1,
                "writing the other family's address would touch a register "
                "that means something else entirely");
}

TEST(mute, a_usb2_chip_is_unmuted_at_its_own_register) {
  Harness harness;
  BringUpHdmi(harness, kRegChipId912x, kSignaturePart912C, kSignatureFamily912x,
              kRegHdmiMute912x, 0xFF);
  CHECK_EQ(WroteTo(*harness.link, kRegHdmiMute912x), 0xFF & ~kHdmiMuteBit);
  CHECK(WroteTo(*harness.link, kRegHdmiMute913x) == -1);
}

TEST(mute, an_already_unmuted_chip_is_left_alone) {
  Harness harness;
  BringUpHdmi(harness, kRegChipId913x, kSignaturePart912A, kSignatureFamily913x,
              kRegHdmiMute913x, 0x00);
  CHECK_BECAUSE(WroteTo(*harness.link, kRegHdmiMute913x) == -1,
                "an adapter that comes up showing a picture should not be "
                "written to at all, so this costs a read and nothing more");
}

TEST(mute, turning_the_output_off_mutes_it) {
  Harness harness;
  BringUpHdmi(harness, kRegChipId913x, kSignaturePart912A, kSignatureFamily913x,
              kRegHdmiMute913x, 0x00);
  harness.link->controls.clear();
  CHECK(harness.chip->EnableOutput(false));
  CHECK_EQ(WroteTo(*harness.link, kRegHdmiMute913x), kHdmiMuteBit);
}

TEST(mute, a_connector_we_have_no_register_for_is_not_guessed_at) {
  Harness harness;
  harness.link->registers[kRegChipId913x] = kSignaturePart912A;
  harness.link->registers[kRegChipId913x + 1] = kSignatureFamily913x;
  harness.link->registers[kRegChipId913x + 2] = kSignatureTail;
  harness.link->registers[kRegVideoPort] = 0x02; /* VGA */

  CHECK(harness.chip->SetMode(Mode1080p60()));
  CHECK(harness.chip->EnableOutput(true));
  for (const FakeLink::Control& control : harness.link->controls) {
    CHECK_BECAUSE(control.op() != kOpWriteXdataByte,
                  "the other connectors each have their own register and "
                  "none of them has been tried on hardware, so writing a "
                  "guessed address is not worth the risk to the dongle");
  }
}

namespace {

bool Offers(const std::vector<Mode>& modes, int w, int h) {
  for (const Mode& mode : modes) {
    if (mode.width == w && mode.height == h) {
      return true;
    }
  }
  return false;
}

std::vector<Mode> ModesWithMemory(Harness& harness, uint8_t sdram) {
  harness.link->registers[kRegSdramType] = sdram;
  return harness.chip->SupportedModes(VideoPort::kHdmi);
}

}  // namespace

TEST(memory, an_eight_megabyte_board_can_do_1080p) {
  Harness harness;
  /* Two frames at two bytes a pixel is 7.9 MB, which fits in 8 and is what
   * the adapter this was written against reports. */
  CHECK(Offers(ModesWithMemory(harness, kSdram8M), 1920, 1080));
}

TEST(memory, a_four_megabyte_board_is_not_offered_1080p) {
  Harness harness;
  const std::vector<Mode> modes = ModesWithMemory(harness, kSdram4M);
  CHECK_BECAUSE(!Offers(modes, 1920, 1080),
                "1080p needs 7.9 MB for its two frames and will not fit, "
                "and the hardware answers a mode it cannot hold with a "
                "corrupt picture rather than an error");
  CHECK_BECAUSE(Offers(modes, 1280, 720),
                "720p needs 3.5 MB, so a smaller board is still useful and "
                "must not be left with an empty mode list");
}

TEST(memory, a_board_that_will_not_say_keeps_every_mode) {
  Harness harness;
  harness.link->FailReadsAt(kRegSdramType);
  CHECK_BECAUSE(
      Offers(harness.chip->SupportedModes(VideoPort::kHdmi), 1920, 1080),
      "a failed read is not evidence the memory is small, so the benefit of "
      "the doubt keeps the behaviour that shipped");
}

/* ---- which mode a user gets by default --------------------------------- */

namespace {

/* The refresh rate of the first 1080p mode offered, which is the one
 * Windows settles on unless the user goes looking. */
int FirstFullHdRate(const std::vector<Mode>& modes) {
  for (const Mode& mode : modes) {
    if (mode.width == 1920 && mode.height == 1080) {
      return mode.hz;
    }
  }
  return 0;
}

}  // namespace

TEST(defaultmode, a_usb3_chip_is_offered_the_full_rate_first) {
  Harness harness;
  IdentifyAs9132(harness);
  harness.link->registers[kRegSdramType] = kSdram8M;
  CHECK_BECAUSE(
      FirstFullHdRate(harness.chip->SupportedModes(VideoPort::kHdmi)) == 60,
      "a whole frame costs one slot on these parts, so 60 is comfortable "
      "and defaulting to 30 would halve the frame rate for no reason");
}

TEST(defaultmode, a_usb2_chip_is_offered_the_sustainable_rate_first) {
  Harness harness;
  IdentifyAs912C(harness);
  harness.link->registers[kRegSdramType] = kSdram8M;
  CHECK_BECAUSE(
      FirstFullHdRate(harness.chip->SupportedModes(VideoPort::kHdmi)) == 30,
      "a whole frame costs eight slots on these parts, so 60 cannot be "
      "sustained and 30 is the honest default");
}

TEST(modeset, programs_the_captured_sequence_in_order) {
  Harness harness;
  CHECK(harness.chip->SetMode(Mode1080p60()));

  const int transfer_mode = harness.link->IndexOfCommand(kVideoTransferMode);
  const int input = harness.link->IndexOfCommand(kVideoInputInfo);
  const int output = harness.link->IndexOfCommand(kVideoOutputInfo);

  CHECK(transfer_mode >= 0 && input >= 0 && output >= 0);
  CHECK_BECAUSE(transfer_mode < input && input < output,
                "this order comes from a capture of the vendor driver; "
                "rearranging it produces a dark panel with every transfer "
                "still reporting success");
}

TEST(modeset, selects_manual_block_mode) {
  Harness harness;
  CHECK(harness.chip->SetMode(Mode1080p60()));

  const int index = harness.link->IndexOfCommand(kVideoTransferMode);
  CHECK(index >= 0);
  CHECK_EQ_BECAUSE(
      static_cast<int>(harness.link->controls[index].bytes[2]),
      static_cast<int>(kTransferModeManualBlock),
      "manual block is what makes partial updates possible at all; in any "
      "other mode every update has to be a whole frame, which at 1080p is "
      "eight refresh periods instead of one");
}

TEST(modeset, announces_uyvy_and_the_right_geometry) {
  Harness harness;
  CHECK(harness.chip->SetMode(Mode1080p60()));

  const int index = harness.link->IndexOfCommand(kVideoInputInfo);
  CHECK(index >= 0);
  const FakeLink::Control& command = harness.link->controls[index];
  const int width = (command.bytes[2] << 8) | command.bytes[3];
  const int height = (command.bytes[4] << 8) | command.bytes[5];

  CHECK_EQ(width, 1920);
  CHECK_EQ(height, 1080);
  CHECK_EQ_BECAUSE(static_cast<int>(command.bytes[6]),
                   static_cast<int>(kPixelFormatUyvy),
                   "4:2:2 at 16 bits a pixel is the cheapest format the "
                   "adapter offers, so there is no fallback if this is wrong");
}

TEST(modeset, sends_the_timing_table_index_not_the_resolution) {
  Harness harness;
  CHECK(harness.chip->SetMode(Mode1080p60()));

  const int index = harness.link->IndexOfCommand(kVideoOutputInfo);
  CHECK(index >= 0);
  CHECK_EQ(static_cast<int>(harness.link->controls[index].bytes[2]), 0x81);
}

TEST(modeset, leaves_the_output_disabled) {
  Harness harness;
  CHECK(harness.chip->SetMode(Mode1080p60()));

  CHECK_EQ_BECAUSE(
      harness.link->IndexOfCommand(kVideoEnable), -1,
      "the adapter's memory still holds the previous session's picture, so "
      "lighting the panel before a frame has landed shows that instead of "
      "the desktop");
}

TEST(modeset, brackets_the_reprogramming_with_transfer_enable) {
  Harness harness;
  CHECK(harness.chip->SetMode(Mode1080p60()));

  int first = -1, last = -1;
  for (size_t i = 0; i < harness.link->controls.size(); ++i) {
    const FakeLink::Control& command = harness.link->controls[i];
    if (command.op() == kOpVideo && command.sub_op() == kVideoTransferEnable) {
      if (first < 0) {
        first = static_cast<int>(i);
      }
      last = static_cast<int>(i);
    }
  }
  CHECK(first >= 0 && last > first);
  CHECK_EQ_BECAUSE(static_cast<int>(harness.link->controls[first].bytes[2]), 0,
                   "transfers are stopped before anything is reprogrammed");
  CHECK_EQ(static_cast<int>(harness.link->controls[last].bytes[2]), 1);
}

TEST(transfer, every_frame_is_followed_by_a_zero_length_packet) {
  Harness harness;
  const uint8_t frame[64] = {0xFF, 0x00};
  CHECK(harness.chip->SendTransfer(frame, sizeof(frame)));

  CHECK_EQ(harness.link->bulks.size(), static_cast<size_t>(2));
  CHECK_EQ(harness.link->bulks[0].length, sizeof(frame));
  CHECK_EQ_BECAUSE(
      harness.link->bulks[1].length, static_cast<size_t>(0),
      "the zero length packet is how the adapter is told the transfer is "
      "complete; without it the adapter waits for data that never arrives "
      "and the panel stays dark while every write reports success");
}

TEST(transfer, the_output_is_enabled_only_after_the_first_frame_lands) {
  Harness harness;
  CHECK(harness.chip->SetMode(Mode1080p60()));
  const size_t before = harness.link->controls.size();

  const uint8_t frame[64] = {0xFF, 0x00};
  CHECK(harness.chip->SendTransfer(frame, sizeof(frame)));

  bool enabled_after = false;
  for (size_t i = before; i < harness.link->controls.size(); ++i) {
    const FakeLink::Control& command = harness.link->controls[i];
    if (command.op() == kOpVideo && command.sub_op() == kVideoEnable &&
        command.bytes[2] == 1) {
      enabled_after = true;
    }
  }
  CHECK_BECAUSE(enabled_after,
                "the panel is lit once there is something real to show, and "
                "not before");
}

TEST(transfer, the_output_is_not_re_enabled_on_every_frame) {
  Harness harness;
  const uint8_t frame[64] = {0xFF, 0x00};
  CHECK(harness.chip->SendTransfer(frame, sizeof(frame)));
  const size_t after_first = harness.link->controls.size();
  CHECK(harness.chip->SendTransfer(frame, sizeof(frame)));

  CHECK_EQ_BECAUSE(harness.link->controls.size(), after_first,
                   "a control exchange between every pair of transfers would "
                   "cost round trips on the critical path for no reason");
}

TEST(transfer, refuses_to_send_when_there_is_no_data_plane) {
  Harness harness;
  harness.link->has_panel = false;

  const uint8_t frame[64] = {0xFF, 0x00};
  CHECK_BECAUSE(!harness.chip->SendTransfer(frame, sizeof(frame)),
                "reporting success into a void makes a missing driver "
                "package look like a hardware fault");
  CHECK(!harness.chip->error().empty());
}

TEST(transfer, repeated_failures_reprogram_the_adapter) {
  Harness harness;
  CHECK(harness.chip->SetMode(Mode1080p60()));
  harness.link->fail_bulk = true;

  const uint8_t frame[64] = {0xFF, 0x00};
  for (int i = 0; i < 3; ++i) {
    harness.chip->SendTransfer(frame, sizeof(frame));
  }

  /* A reprogram is visible as a second run of the mode sequence. */
  int transfer_mode_commands = 0;
  for (const FakeLink::Control& command : harness.link->controls) {
    if (command.op() == kOpVideo && command.sub_op() == kVideoTransferMode) {
      ++transfer_mode_commands;
    }
  }
  CHECK_BECAUSE(transfer_mode_commands >= 2,
                "once the adapter stops accepting transfers it stays that "
                "way, and before this the only recovery was unplugging it");
}

TEST(errors, a_control_failure_is_reported_with_context) {
  Harness harness;
  harness.link->fail_control = true;

  uint8_t value = 0;
  CHECK(!harness.chip->ReadByte(0x0031, &value));
  CHECK_BECAUSE(!harness.chip->error().empty(),
                "a bare false tells whoever is reading the log nothing about "
                "which step failed");
}

TEST(errors, resetting_before_a_mode_is_set_fails_cleanly) {
  Harness harness;
  CHECK(!harness.chip->Reset());
  CHECK(!harness.chip->error().empty());
}
