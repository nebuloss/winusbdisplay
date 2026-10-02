/* SPDX-License-Identifier: GPL-2.0-only
 *
 * usbdisplayctl: the bring-up and diagnosis tool, and the project's test
 * harness. There is no unit test suite; this is it.
 *
 * Most of what matters can be exercised here with no driver installed at
 * all, because the control plane runs over the in-box HID stack. Only the
 * commands that push pixels need WinUSB, and `--dump` replaces even that
 * with a directory on disk.
 */

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../core/display_device.h"
#include "../../core/macrosilicon.h"
#include "../../core/proto.h"
#include "../../core/usb.h"
#include "../../render/convert.h"
#include "../../render/damage.h"
#include "../../render/rect.h"

using namespace usbdisplay;

namespace {

std::string g_dump_directory;

void PrintUsage() {
  printf(
      "usbdisplayctl - MacroSilicon USB display bring-up tool\n"
      "\n"
      "usage: usbdisplayctl [--dump DIR] <command> [options]\n"
      "\n"
      "  --dump DIR      write transfers to DIR instead of the dongle, so\n"
      "                  the frame pipeline can be checked with no hardware\n"
      "\n"
      "reads, no driver installation needed:\n"
      "  list            USB interfaces Windows can see\n"
      "  info            chip model, connector, hotplug state\n"
      "  edid [--blocks N] [--out FILE]\n"
      "  modes           the chip's built-in timing table\n"
      "  timings         custom timings programmed into flash\n"
      "\n"
      "offline checks, no hardware at all:\n"
      "  selftest        quick check that this binary is sane here\n"
      "  plan            what the planner does with realistic damage\n"
      "                  the thorough version is scripts\\test.bat\n"
      "\n"
      "needs the WinUSB data interface:\n"
      "  testpattern [--mode WxH@Hz] [--bars] [--solid R,G,B]\n"
      "  bench [--mode WxH@Hz] [--seconds N]\n"
      "  benchsizes [--mode WxH@Hz]\n");
}

/* quiet is for callers that work perfectly well with no adapter and are only
 * asking in case there is one. Without it they print an error for a
 * condition that is not one. */
std::unique_ptr<MacroSiliconDevice> OpenChip(bool require_panel,
                                             bool quiet = false) {
  if (!g_dump_directory.empty()) {
    std::string error;
    std::unique_ptr<FileLink> link = FileLink::Open(g_dump_directory, &error);
    if (!link) {
      if (!quiet) {
        fprintf(stderr, "error: %s\n", error.c_str());
      }
      return nullptr;
    }
    printf("link: %s\n", link->Describe().c_str());
    return std::unique_ptr<MacroSiliconDevice>(
        new MacroSiliconDevice(std::move(link)));
  }

  std::string error;
  std::unique_ptr<UsbLink> link = UsbLink::Open(require_panel, &error);
  if (!link) {
    if (!quiet) {
      fprintf(stderr, "error: %s\n", error.c_str());
    }
    return nullptr;
  }
  printf("link: %s\n", link->Describe().c_str());
  return std::unique_ptr<MacroSiliconDevice>(
        new MacroSiliconDevice(std::move(link)));
}

bool ParseMode(const char* text, Mode* out) {
  int width = 0, height = 0, hz = 60;
  if (sscanf_s(text, "%dx%d@%d", &width, &height, &hz) < 2) {
    return false;
  }
  const Mode* found = FindMode(width, height, hz);
  if (!found) {
    return false;
  }
  *out = *found;
  return true;
}

Mode ModeFromArgs(int argc, char** argv) {
  Mode mode = *FindMode(1920, 1080, 60);
  for (int i = 0; i + 1 < argc; ++i) {
    if (strcmp(argv[i], "--mode") == 0 && !ParseMode(argv[i + 1], &mode)) {
      fprintf(stderr,
              "warning: %s is not in the chip's table, using 1920x1080@60\n",
              argv[i + 1]);
    }
  }
  return mode;
}

int IntArg(int argc, char** argv, const char* name, int fallback) {
  for (int i = 0; i + 1 < argc; ++i) {
    if (strcmp(argv[i], name) == 0) {
      return atoi(argv[i + 1]);
    }
  }
  return fallback;
}

const char* StringArg(int argc, char** argv, const char* name,
                      const char* fallback) {
  for (int i = 0; i + 1 < argc; ++i) {
    if (strcmp(argv[i], name) == 0) {
      return argv[i + 1];
    }
  }
  return fallback;
}

/* An option with no value of its own. */
bool HasFlag(int argc, char** argv, const char* name) {
  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], name) == 0) {
      return true;
    }
  }
  return false;
}

double NowSeconds() {
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) /
         static_cast<double>(frequency.QuadPart);
}

int CmdList() {
  const std::vector<UsbLink::Interface> interfaces = UsbLink::Enumerate();
  if (interfaces.empty()) {
    printf(
        "nothing found. The dongle may not be plugged in, or may be\n"
        "enumerating under a product id this tool does not recognise. Check\n"
        "with: pnputil /enum-devices /connected\n");
    return 1;
  }

  for (const UsbLink::Interface& entry : interfaces) {
    printf("%-8s %04X:%04X  %S\n", entry.is_hid ? "control" : "pixels",
           entry.vid, entry.pid, entry.path.c_str());
  }

  bool has_data = false;
  for (const UsbLink::Interface& entry : interfaces) {
    has_data = has_data || !entry.is_hid;
  }
  if (!has_data) {
    printf(
        "\nNo WinUSB display interface. Everything except pixels still\n"
        "works; install inf/usbdisplay_winusb.inf to send frames.\n");
  }
  return 0;
}

int CmdInfo() {
  std::unique_ptr<MacroSiliconDevice> chip = OpenChip(false);
  if (!chip) {
    return 1;
  }

  ChipId id;
  if (!chip->ReadChipId(&id)) {
    fprintf(stderr, "error: %s\n", chip->error().c_str());
    return 1;
  }
  printf("chip:      %s (signature %02X %02X %02X)\n",
         ChipModelName(id.model), id.signature[0], id.signature[1],
         id.signature[2]);
  if (id.model == ChipModel::kUnknown) {
    printf("           unrecognised, so flash timings cannot be located\n");
  }

  VideoPort port = VideoPort::kUnknown;
  if (chip->ReadConnector(&port)) {
    printf("connector: %s\n", VideoPortName(port));
  }

  /* The board's memory, and what it rules out. Worth printing even when
   * nothing is wrong: it is the first thing to look at when a resolution a
   * panel clearly supports is missing from the list. */
  uint8_t sdram = 0;
  if (chip->ReadSdramType(&sdram)) {
    const size_t memory = SdramBytes(sdram);
    if (sdram == kSdramNone) {
      printf("memory:    none reported, so no mode is ruled out here\n");
    } else {
      printf("memory:    %zu MB, enough for two frames up to %s\n",
             memory / (1024u * 1024u),
             SdramBytesNeeded(1920, 1080) <= memory ? "1080p" : "720p");
    }
  }

  uint8_t status = 0;
  if (chip->ReadDisplayStatus(&status)) {
    printf("display:   %s (0x%02X)\n",
           (status & 1) ? "connected" : "nothing attached", status);
  }
  return 0;
}

int CmdEdid(int argc, char** argv) {
  std::unique_ptr<MacroSiliconDevice> chip = OpenChip(false);
  if (!chip) {
    return 1;
  }

  const int blocks = IntArg(argc, argv, "--blocks", 1);
  std::vector<uint8_t> edid;
  bool checksum_ok = false;
  if (!chip->ReadEdid(&edid, blocks, &checksum_ok)) {
    fprintf(stderr, "error: %s\n", chip->error().c_str());
    return 1;
  }

  printf("%zu bytes, checksum %s\n", edid.size(), checksum_ok ? "ok" : "BAD");
  for (size_t i = 0; i < edid.size(); i += 16) {
    printf("%04zX  ", i);
    for (size_t j = 0; j < 16 && i + j < edid.size(); ++j) {
      printf("%02X ", edid[i + j]);
    }
    printf("\n");
  }

  const char* out = StringArg(argc, argv, "--out", nullptr);
  if (out) {
    FILE* file = nullptr;
    if (fopen_s(&file, out, "wb") == 0 && file) {
      fwrite(edid.data(), 1, edid.size(), file);
      fclose(file);
      printf("written to %s\n", out);
    } else {
      fprintf(stderr, "error: could not write %s\n", out);
      return 1;
    }
  }
  return 0;
}

int CmdModes() {
  /* The mode table is static, so this works with nothing plugged in. The
   * cost column is not: what a full frame costs depends on which family the
   * chip belongs to, by a factor of eight. So the adapter is consulted when
   * there is one, and the answer says which it is rather than printing a
   * number whose meaning the reader has to guess. */
  size_t per_period = kBytesPerPeriod912x;
  const char* measured_for = "USB 2 parts, no adapter attached to ask";

  std::unique_ptr<MacroSiliconDevice> chip = OpenChip(false, true);
  if (chip) {
    ChipId id;
    if (chip->ReadChipId(&id) && id.model != ChipModel::kUnknown) {
      per_period = (id.model == ChipModel::kMs9132) ? kBytesPerPeriod913x
                                                    : kBytesPerPeriod912x;
      measured_for = ChipModelName(id.model);
    }
  }

  printf("cost periods are for: %s\n\n", measured_for);
  printf("index  mode\n");
  for (size_t i = 0; i < kModeCount; ++i) {
    const Mode& mode = kModes[i];
    const size_t bytes =
        static_cast<size_t>(mode.width) * mode.height * 2 + kFrameOverhead;
    printf(" 0x%02X  %4dx%-4d @%3d   full frame %6.2f MB, %d periods\n",
           mode.index, mode.width, mode.height, mode.hz,
           bytes / (1024.0 * 1024.0),
           static_cast<int>((bytes + per_period - 1) / per_period));
  }
  return 0;
}

int CmdTimings() {
  std::unique_ptr<MacroSiliconDevice> chip = OpenChip(false);
  if (!chip) {
    return 1;
  }
  std::vector<CustomTiming> timings;
  if (!chip->ReadCustomTimings(&timings)) {
    fprintf(stderr, "error: %s\n", chip->error().c_str());
    return 1;
  }
  if (timings.empty()) {
    printf("no custom timings programmed, the built-in table applies\n");
    return 0;
  }
  for (const CustomTiming& timing : timings) {
    printf("index 0x%02X  %dx%d @%d  htotal %u vtotal %u  pixclk %u0 kHz\n",
           timing.mode.index, timing.mode.width, timing.mode.height,
           timing.mode.hz, timing.htotal, timing.vtotal,
           timing.pixel_clock_10khz);
  }
  return 0;
}

/* One planner scenario, described so a failure says what broke. */
struct PlannerCase {
  const char* name;
  const char* expectation;
  std::vector<Rect> damage;
  int max_periods;
};

Rect MakeRect(int x, int y, int w, int h) {
  Rect rect;
  rect.x1 = x;
  rect.y1 = y;
  rect.x2 = x + w;
  rect.y2 = y + h;
  return rect;
}

int RunPlannerCases(bool verbose) {
  std::vector<PlannerCase> cases;

  /* The case the whole planner exists for. Merging these costs a full screen
   * transfer, eight periods; sent separately they are two. */
  cases.push_back({"two far apart small updates",
                   "stays split rather than merging into a full screen box",
                   {MakeRect(0, 0, 200, 200), MakeRect(1700, 880, 200, 200)},
                   2});

  /* Adjacent updates should merge: two transfers cost two periods, one
   * merged transfer costs one. */
  cases.push_back({"two adjacent small updates",
                   "merges, because one transfer is cheaper than two",
                   {MakeRect(100, 100, 200, 200), MakeRect(300, 100, 200, 200)},
                   1});

  /* A typing caret and its line. Trivial either way, but must not blow up. */
  cases.push_back({"caret and line",
                   "one cheap transfer",
                   {MakeRect(520, 400, 4, 20), MakeRect(180, 398, 604, 52)},
                   1});

  /* A window drag: source and destination of a move. Merging these is the
   * expensive mistake the old driver made routinely. */
  cases.push_back({"window drag, source and destination",
                   "kept apart, so a drag does not cost a full repaint",
                   {MakeRect(100, 100, 600, 400), MakeRect(1200, 600, 600, 400)},
                   4});

  cases.push_back({"full screen",
                   "one transfer, eight periods, nothing to be done about it",
                   {MakeRect(0, 0, 1920, 1080)},
                   8});

  int failures = 0;
  for (const PlannerCase& test : cases) {
    RectSet set;
    for (const Rect& rect : test.damage) {
      set.Add(rect, DefaultCostModel());
    }

    Rect planned[kMaxTransfersPerFrame];
    const size_t count = PlanTransfers(set, planned, kMaxTransfersPerFrame, DefaultCostModel());
    const int periods = DefaultCostModel().TransferCost(planned, count);

    const bool ok = periods <= test.max_periods;
    if (!ok) {
      ++failures;
    }
    if (verbose || !ok) {
      printf("%-4s %-38s %zu transfer%s, %d period%s (%s)\n",
             ok ? "ok" : "FAIL", test.name, count, count == 1 ? "" : "s",
             periods, periods == 1 ? "" : "s", test.expectation);
      if (verbose) {
        for (size_t i = 0; i < count; ++i) {
          printf("       %4d,%-4d %4dx%-4d  %8zu bytes, %d period%s\n",
                 planned[i].x1, planned[i].y1, planned[i].width(),
                 planned[i].height(), TransferLength(planned[i]),
                 DefaultCostModel().TransferCost(planned[i]),
                 DefaultCostModel().TransferCost(planned[i]) == 1 ? "" : "s");
        }
      }
    }
  }
  return failures;
}

int CmdPlan() { return RunPlannerCases(true) == 0 ? 0 : 1; }

/* Reads registers by address.
 *
 * The control plane needs no driver, so this works whenever the adapter is
 * plugged in, and it is the only way to see what the chip actually holds
 * rather than what the code believes it holds. Added while chasing a dark
 * panel, where the question "what is really in that register" had no way to
 * be answered.
 *
 *   usbdisplayctl peek 0xFB07        one byte
 *   usbdisplayctl peek 0xC000 16     a range
 */
int CmdPeek(int argc, char** argv) {
  if (argc < 1) {
    fprintf(stderr, "usage: peek <address> [count]\n");
    return 1;
  }
  const unsigned long address = strtoul(argv[0], nullptr, 0);
  const unsigned long count = argc > 1 ? strtoul(argv[1], nullptr, 0) : 1;
  if (address > 0xFFFF || count == 0 || count > 256 ||
      address + count > 0x10000) {
    fprintf(stderr, "error: address must be 16 bit and the range must fit\n");
    return 1;
  }

  std::unique_ptr<MacroSiliconDevice> chip = OpenChip(false);
  if (!chip) {
    return 1;
  }

  std::vector<uint8_t> data(count);
  /* One exchange returns at most four consecutive bytes, so a range is
   * several of them. Doing this here rather than in the device keeps the
   * chunking where the convenience is wanted. */
  for (size_t done = 0; done < data.size();) {
    const size_t chunk =
        data.size() - done < kMaxReadBytes ? data.size() - done : kMaxReadBytes;
    if (!chip->Read(static_cast<uint16_t>(address + done), data.data() + done,
                    chunk)) {
      fprintf(stderr, "error: %s\n", chip->error().c_str());
      return 1;
    }
    done += chunk;
  }

  for (size_t i = 0; i < data.size(); ++i) {
    if (i % 16 == 0) {
      printf("%s%04lX  ", i ? "\n" : "",
             static_cast<unsigned long>(address + i));
    }
    printf("%02X ", data[i]);
  }
  printf("\n");
  return 0;
}

/* A short sanity check for use in the field, on a machine that has the tool
 * but not the source. The thorough version is the test suite, built and run
 * by scripts\test.bat; this deliberately does not duplicate it, it only
 * answers "is this binary sane on this processor". */
int CmdSelfTest() {
  int failures = 0;

  printf("conversion, SIMD against the scalar reference\n");
  for (int width : {2, 6, 8, 14, 16, 64, 130, 512, 1920}) {
    const int worst = SelfTest(width, 32);
    const bool ok = worst == 0;
    failures += ok ? 0 : 1;
    printf("  %-5s width %4d  largest difference %d\n", ok ? "ok" : "FAIL",
           width, worst);
  }
  printf(
      "  the two paths must agree exactly, not closely: a region redrawn\n"
      "  at different widths would otherwise alternate between two values\n"
      "  and shimmer\n");

  printf("\n%s\n", failures == 0
                       ? "ok. For the full suite run scripts\\test.bat"
                       : "FAILED, this binary does not match its reference");
  return failures == 0 ? 0 : 1;
}

/* Sends one full image built on the CPU. Shared by testpattern. */
bool SendImage(MacroSiliconDevice* chip, const Mode& mode, const uint8_t* rgb,
               size_t stride) {
  if (!chip->PowerOn() || !chip->SetMode(mode)) {
    fprintf(stderr, "error: %s\n", chip->error().c_str());
    return false;
  }

  Rect full;
  full.x2 = mode.width;
  full.y2 = mode.height;

  std::vector<uint8_t> transfer(TransferLength(full));
  const size_t length =
      FrameRect(transfer.data(), transfer.size(), rgb, stride, mode.width,
                mode.height, full);
  if (length == 0) {
    fprintf(stderr, "error: framing failed\n");
    return false;
  }

  const double start = NowSeconds();
  if (!chip->SendTransfer(transfer.data(), length)) {
    fprintf(stderr, "error: %s\n", chip->error().c_str());
    return false;
  }
  const double elapsed = NowSeconds() - start;
  printf("sent %zu bytes in %.1f ms (%.1f MB/s)\n", length, elapsed * 1000.0,
         length / elapsed / (1024.0 * 1024.0));
  return true;
}

/* Sends an image the way the driver does: in horizontal bands, each one
 * transmitted twice back to back.
 *
 * The plain path above sends one full-frame transfer, once, and that has
 * always worked. The driver does neither of those things, so when a panel
 * looks wrong under the driver and right under the tool, there is no way to
 * tell which difference is responsible. This reproduces the driver's
 * behaviour with content that is known and static, which the driver's never
 * is.
 *
 *   --bands      send in bands rather than one frame
 *   --band N     rows per band, default 128 to match the driver
 *   --single     send each band once, to test the double transmission
 */
bool SendImageBanded(MacroSiliconDevice* chip, const Mode& mode,
                     const uint8_t* rgb, size_t stride, int band_rows,
                     int transmissions) {
  if (!chip->PowerOn() || !chip->SetMode(mode)) {
    fprintf(stderr, "error: %s\n", chip->error().c_str());
    return false;
  }

  Rect full;
  full.x2 = mode.width;
  full.y2 = mode.height;
  std::vector<uint8_t> transfer(TransferLength(full));

  const double start = NowSeconds();
  size_t total = 0;
  int bands = 0;

  for (int y = 0; y < mode.height; y += band_rows) {
    Rect band;
    band.x1 = 0;
    band.x2 = mode.width;
    band.y1 = y;
    band.y2 = y + band_rows > mode.height ? mode.height : y + band_rows;
    band = AlignDamageRect(band, mode.width, mode.height);
    if (band.empty()) {
      continue;
    }

    const size_t length =
        FrameRect(transfer.data(), transfer.size(), rgb, stride, mode.width,
                  mode.height, band);
    if (length == 0) {
      fprintf(stderr, "error: framing %d,%d %dx%d failed\n", band.x1, band.y1,
              band.width(), band.height());
      return false;
    }
    /* Twice, back to back, exactly as the pipeline does, because the chip
     * keeps two copies and alternates between them. */
    for (int i = 0; i < transmissions; ++i) {
      if (!chip->SendTransfer(transfer.data(), length)) {
        fprintf(stderr, "error: %s\n", chip->error().c_str());
        return false;
      }
      total += length;
    }
    ++bands;
  }

  const double elapsed = NowSeconds() - start;
  printf("sent %d bands x%d, %zu bytes in %.1f ms (%.1f MB/s)\n", bands,
         transmissions, total, elapsed * 1000.0,
         total / elapsed / (1024.0 * 1024.0));
  return true;
}

int CmdTestPattern(int argc, char** argv) {
  const Mode mode = ModeFromArgs(argc, argv);
  std::unique_ptr<MacroSiliconDevice> chip = OpenChip(g_dump_directory.empty());
  if (!chip) {
    return 1;
  }

  const size_t stride = static_cast<size_t>(mode.width) * 4;
  std::vector<uint8_t> rgb(stride * mode.height);

  const char* solid = StringArg(argc, argv, "--solid", nullptr);
  if (solid) {
    int r = 0, g = 0, b = 0;
    sscanf_s(solid, "%d,%d,%d", &r, &g, &b);
    FillSolid(rgb.data(), stride, mode.width, mode.height,
              static_cast<uint8_t>(r), static_cast<uint8_t>(g),
              static_cast<uint8_t>(b));
  } else {
    FillColourBars(rgb.data(), stride, mode.width, mode.height);
  }

  printf("mode %dx%d@%d, chip index 0x%02X\n", mode.width, mode.height,
         mode.hz, mode.index);

  if (HasFlag(argc, argv, "--bands")) {
    const int rows = IntArg(argc, argv, "--band", 128);
    const int transmissions = HasFlag(argc, argv, "--single") ? 1 : 2;
    return SendImageBanded(chip.get(), mode, rgb.data(), stride, rows,
                           transmissions)
               ? 0
               : 1;
  }
  return SendImage(chip.get(), mode, rgb.data(), stride) ? 0 : 1;
}

int CmdBench(int argc, char** argv) {
  const Mode mode = ModeFromArgs(argc, argv);
  const int seconds = IntArg(argc, argv, "--seconds", 3);

  std::unique_ptr<MacroSiliconDevice> chip = OpenChip(true);
  if (!chip) {
    return 1;
  }
  if (!chip->PowerOn() || !chip->SetMode(mode)) {
    fprintf(stderr, "error: %s\n", chip->error().c_str());
    return 1;
  }

  Rect full;
  full.x2 = mode.width;
  full.y2 = mode.height;

  const size_t stride = static_cast<size_t>(mode.width) * 4;
  std::vector<uint8_t> rgb(stride * mode.height);
  FillColourBars(rgb.data(), stride, mode.width, mode.height);

  std::vector<uint8_t> transfer(TransferLength(full));
  const size_t length =
      FrameRect(transfer.data(), transfer.size(), rgb.data(), stride,
                mode.width, mode.height, full);

  printf("sending %zu byte frames for %d seconds\n", length, seconds);
  const double start = NowSeconds();
  unsigned frames = 0;
  while (NowSeconds() - start < seconds) {
    if (!chip->SendTransfer(transfer.data(), length)) {
      fprintf(stderr, "error: %s\n", chip->error().c_str());
      return 1;
    }
    ++frames;
  }
  const double elapsed = NowSeconds() - start;
  const double bytes = static_cast<double>(length) * frames;

  printf("%u frames in %.2f s: %.1f fps, %.1f MB/s\n", frames, elapsed,
         frames / elapsed, bytes / elapsed / (1024.0 * 1024.0));
  printf(
      "\nAbout 29.6 MB/s is the ceiling and it is the chip, not the bus or\n"
      "the host: the silicon is USB 2 and overlapped transfers at depth 2,\n"
      "4 and 8 all measure within one percent of this. There are no\n"
      "host-side throughput wins to find here.\n");
  return 0;
}

int CmdBenchSizes(int argc, char** argv) {
  const Mode mode = ModeFromArgs(argc, argv);

  std::unique_ptr<MacroSiliconDevice> chip = OpenChip(true);
  if (!chip) {
    return 1;
  }
  if (!chip->PowerOn() || !chip->SetMode(mode)) {
    fprintf(stderr, "error: %s\n", chip->error().c_str());
    return 1;
  }

  const size_t stride = static_cast<size_t>(mode.width) * 4;
  std::vector<uint8_t> rgb(stride * mode.height);
  FillColourBars(rgb.data(), stride, mode.width, mode.height);
  std::vector<uint8_t> transfer(kMaxTransferBytes);

  printf("%-14s %10s %9s %9s %8s\n", "rect", "bytes", "time ms", "updates/s",
         "periods");
  for (int height : {8, 32, 64, 128, 136, 144, 256, 320, 540, mode.height}) {
    if (height > mode.height) {
      continue;
    }
    Rect rect;
    rect.x2 = mode.width;
    rect.y2 = height;
    rect = AlignDamageRect(rect, mode.width, mode.height);

    const size_t length =
        FrameRect(transfer.data(), transfer.size(), rgb.data(), stride,
                  mode.width, mode.height, rect);

    /* Several repetitions, because a single transfer can land anywhere
     * within a period and the quantisation is the thing being measured. */
    const int repeats = 10;
    const double start = NowSeconds();
    for (int i = 0; i < repeats; ++i) {
      if (!chip->SendTransfer(transfer.data(), length)) {
        fprintf(stderr, "error: %s\n", chip->error().c_str());
        return 1;
      }
    }
    const double each = (NowSeconds() - start) / repeats;

    char label[32];
    _snprintf_s(label, sizeof(label), _TRUNCATE, "%dx%d", rect.width(),
                rect.height());
    printf("%-14s %10zu %9.1f %9.1f %8d\n", label, length, each * 1000.0,
           1.0 / each, DefaultCostModel().TransferCost(rect));
  }

  printf(
      "\nEvery time should be close to a multiple of 16.7 ms. The chip\n"
      "completes transfers on its own 60 Hz boundary, so below roughly\n"
      "520 KB size is free and above it cost jumps a whole period at a\n"
      "time. That is why the damage planner merges by cost, not by area.\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  int index = 1;
  while (index < argc && strncmp(argv[index], "--", 2) == 0) {
    if (strcmp(argv[index], "--dump") == 0 && index + 1 < argc) {
      g_dump_directory = argv[index + 1];
      index += 2;
      continue;
    }
    break;
  }

  if (index >= argc) {
    PrintUsage();
    return 1;
  }

  const char* command = argv[index];
  char** rest = argv + index + 1;
  const int rest_count = argc - index - 1;

  if (strcmp(command, "list") == 0) {
    return CmdList();
  }
  if (strcmp(command, "info") == 0) {
    return CmdInfo();
  }
  if (strcmp(command, "edid") == 0) {
    return CmdEdid(rest_count, rest);
  }
  if (strcmp(command, "modes") == 0) {
    return CmdModes();
  }
  if (strcmp(command, "timings") == 0) {
    return CmdTimings();
  }
  if (strcmp(command, "selftest") == 0) {
    return CmdSelfTest();
  }
  if (strcmp(command, "plan") == 0) {
    return CmdPlan();
  }
  if (strcmp(command, "peek") == 0) {
    return CmdPeek(rest_count, rest);
  }
  if (strcmp(command, "testpattern") == 0) {
    return CmdTestPattern(rest_count, rest);
  }
  if (strcmp(command, "bench") == 0) {
    return CmdBench(rest_count, rest);
  }
  if (strcmp(command, "benchsizes") == 0) {
    return CmdBenchSizes(rest_count, rest);
  }

  fprintf(stderr, "unknown command: %s\n\n", command);
  PrintUsage();
  return 1;
}
