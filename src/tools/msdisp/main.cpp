/* SPDX-License-Identifier: GPL-2.0-only
 *
 * msdisp: bring-up and diagnostic tool for MacroSilicon USB display dongles.
 */

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hid_transport.h"
#include "ms912x_device.h"
#include "ms912x_proto.h"

using namespace ms912x;

namespace {

void PrintUsage() {
  printf(
      "msdisp - MacroSilicon USB display bring-up tool\n"
      "\n"
      "usage: msdisp <command> [options]\n"
      "\n"
      "commands:\n"
      "  list              list MacroSilicon interfaces visible to Windows\n"
      "  info              chip id, connector type, display status\n"
      "  edid [--blocks N] [--out FILE]\n"
      "                    read and verify EDID (default 1 block)\n"
      "  timings           read custom timings from flash\n"
      "  modes             print the built-in mode table\n"
      "\n"
      "The control plane runs over the in-box HID stack, so no driver\n"
      "installation is required for any of the commands above.\n");
}

void HexDump(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i += 16) {
    printf("  %04zX  ", i);
    for (size_t j = 0; j < 16; ++j) {
      if (i + j < len) {
        printf("%02X ", data[i + j]);
      } else {
        printf("   ");
      }
      if (j == 7) {
        printf(" ");
      }
    }
    printf("\n");
  }
}

std::unique_ptr<Device> OpenDevice() {
  std::string error;
  std::unique_ptr<HidTransport> transport = HidTransport::Open(&error);
  if (!transport) {
    fprintf(stderr, "error: %s\n", error.c_str());
    return nullptr;
  }
  printf("transport: %s\n", transport->Describe().c_str());
  return std::unique_ptr<Device>(new Device(std::move(transport)));
}

int CmdList() {
  std::vector<DeviceLocation> devices = HidTransport::Enumerate();
  if (devices.empty()) {
    printf("no MacroSilicon HID interfaces found\n");
    return 1;
  }
  for (const DeviceLocation& loc : devices) {
    printf("%04X:%04X  %ws\n", loc.vid, loc.pid, loc.path.c_str());
  }
  return 0;
}

const char* ChipFamilyName(ChipFamily family) {
  switch (family) {
    case ChipFamily::kMs912x:
      return "MS912x (USB 2)";
    case ChipFamily::kMs913x:
      return "MS913x (USB 3)";
    default:
      return "unrecognised";
  }
}

int CmdInfo() {
  std::unique_ptr<Device> device = OpenDevice();
  if (!device) {
    return 1;
  }

  ChipInfo chip;
  if (!device->ReadChipInfo(&chip)) {
    fprintf(stderr, "error: %s\n", device->last_error().c_str());
    return 1;
  }
  printf("chip:      %s  signature %02X %02X %02X\n",
         ChipFamilyName(chip.family), chip.signature[0], chip.signature[1],
         chip.signature[2]);
  if (chip.family != ChipFamily::kUnknown) {
    printf("timing base: 0x%04X\n", chip.custom_timing_base);
  }

  VideoPort port = VideoPort::kUnknown;
  if (device->ReadVideoPort(&port)) {
    printf("connector: %s (0x%02X)\n", VideoPortName(port),
           static_cast<unsigned>(port));
  } else {
    fprintf(stderr, "connector: read failed: %s\n",
            device->last_error().c_str());
  }

  uint8_t status = 0;
  if (device->ReadDisplayStatus(&status)) {
    printf("display:   %s (0x%02X)\n",
           status == 1 ? "connected" : "disconnected", status);
  } else {
    fprintf(stderr, "display:   read failed: %s\n",
            device->last_error().c_str());
  }
  return 0;
}

void PrintEdidSummary(const std::vector<uint8_t>& edid) {
  static const uint8_t kMagic[8] = {0x00, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0x00};
  if (edid.size() < 128 || memcmp(edid.data(), kMagic, sizeof(kMagic)) != 0) {
    printf("edid:      header magic missing, block is not valid EDID\n");
    return;
  }
  uint16_t manufacturer = static_cast<uint16_t>((edid[8] << 8) | edid[9]);
  char vendor[4];
  vendor[0] = static_cast<char>('A' - 1 + ((manufacturer >> 10) & 0x1F));
  vendor[1] = static_cast<char>('A' - 1 + ((manufacturer >> 5) & 0x1F));
  vendor[2] = static_cast<char>('A' - 1 + (manufacturer & 0x1F));
  vendor[3] = '\0';
  printf("edid:      vendor %s product 0x%04X, EDID %u.%u, %u extension(s)\n",
         vendor, static_cast<unsigned>(edid[10] | (edid[11] << 8)), edid[18],
         edid[19], edid[126]);

  /* First detailed timing descriptor at offset 54. */
  const uint8_t* dtd = edid.data() + 54;
  unsigned pixel_clock = (dtd[0] | (dtd[1] << 8)) * 10;
  if (pixel_clock) {
    unsigned hactive = dtd[2] | ((dtd[4] & 0xF0) << 4);
    unsigned vactive = dtd[5] | ((dtd[7] & 0xF0) << 4);
    printf("preferred: %ux%u, pixel clock %u kHz\n", hactive, vactive,
           pixel_clock);
  }
}

int CmdEdid(int argc, char** argv) {
  int blocks = 1;
  const char* out_path = nullptr;
  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
      blocks = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out_path = argv[++i];
    } else {
      fprintf(stderr, "error: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (blocks < 1 || blocks > 2) {
    fprintf(stderr, "error: --blocks must be 1 or 2\n");
    return 2;
  }

  std::unique_ptr<Device> device = OpenDevice();
  if (!device) {
    return 1;
  }

  printf("reading %d block(s), %d control round trips, please wait\n", blocks,
         blocks * 128);
  std::vector<uint8_t> edid;
  bool checksum_ok = false;
  if (!device->ReadEdid(&edid, blocks, &checksum_ok)) {
    fprintf(stderr, "error: %s\n", device->last_error().c_str());
    return 1;
  }

  HexDump(edid.data(), edid.size());
  printf("checksum:  %s\n", checksum_ok ? "ok" : "BAD");
  PrintEdidSummary(edid);

  if (out_path) {
    FILE* file = nullptr;
    if (fopen_s(&file, out_path, "wb") == 0 && file) {
      fwrite(edid.data(), 1, edid.size(), file);
      fclose(file);
      printf("written:   %s\n", out_path);
    } else {
      fprintf(stderr, "error: could not write %s\n", out_path);
      return 1;
    }
  }
  return checksum_ok ? 0 : 1;
}

int CmdTimings() {
  std::unique_ptr<Device> device = OpenDevice();
  if (!device) {
    return 1;
  }
  std::vector<CustomMode> modes;
  if (!device->ReadCustomTimings(&modes)) {
    fprintf(stderr, "error: %s\n", device->last_error().c_str());
    return 1;
  }
  if (modes.empty()) {
    printf("no custom timings programmed in flash\n");
    return 0;
  }
  for (const CustomMode& mode : modes) {
    printf(
        "mode 0x%02X  %ux%u@%u  htotal %u vtotal %u pixclk %u kHz  %s %cH %cV\n",
        mode.mode.mode_id, mode.mode.width, mode.mode.height, mode.mode.hz,
        mode.htotal, mode.vtotal, mode.pixclk_10khz * 10u,
        mode.progressive ? "progressive" : "interlaced",
        mode.positive_hsync ? '+' : '-', mode.positive_vsync ? '+' : '-');
  }
  return 0;
}

int CmdModes() {
  printf("  mode  resolution   refresh\n");
  for (size_t i = 0; i < kModeListLen; ++i) {
    printf("  0x%02X  %4ux%-4u    %u Hz\n", kModeList[i].mode_id,
           kModeList[i].width, kModeList[i].height, kModeList[i].hz);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }
  const char* command = argv[1];
  if (strcmp(command, "list") == 0) {
    return CmdList();
  }
  if (strcmp(command, "info") == 0) {
    return CmdInfo();
  }
  if (strcmp(command, "edid") == 0) {
    return CmdEdid(argc - 2, argv + 2);
  }
  if (strcmp(command, "timings") == 0) {
    return CmdTimings();
  }
  if (strcmp(command, "modes") == 0) {
    return CmdModes();
  }
  PrintUsage();
  return 2;
}
