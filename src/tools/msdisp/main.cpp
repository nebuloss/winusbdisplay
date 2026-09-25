/* SPDX-License-Identifier: GPL-2.0-only
 *
 * msdisp: bring-up and diagnostic tool for MacroSilicon USB display dongles.
 *
 * The control plane can run either over the in-box HID stack (no driver
 * installation needed, but no pixels) or over WinUSB (full access). Frames can
 * also be dumped to disk instead of being sent, which is how you tell a
 * conversion bug apart from a transport bug.
 */

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "composite_transport.h"
#include "file_transport.h"
#include "hid_transport.h"
#include "ms912x_convert.h"
#include "ms912x_device.h"
#include "ms912x_proto.h"
#include "winusb_transport.h"

using namespace ms912x;

namespace {

enum class TransportKind { kAuto, kHid, kWinUsb, kFile, kComposite };

struct GlobalOptions {
  TransportKind transport = TransportKind::kAuto;
  std::string dump_dir = "build\\frames";
};

GlobalOptions g_options;

void PrintUsage() {
  printf(
      "msdisp - MacroSilicon USB display bring-up tool\n"
      "\n"
      "usage: msdisp [--transport auto|hid|winusb|composite|file]"
      " [--dump-dir DIR] <command> [options]\n"
      "\n"
      "commands:\n"
      "  list                    interfaces visible to Windows\n"
      "  dump                    USB descriptors and pipes (WinUSB only)\n"
      "  info                    chip id, connector type, display status\n"
      "  edid [--blocks N] [--out FILE]\n"
      "  timings                 custom timings stored in flash\n"
      "  modes                   built-in mode table\n"
      "  poweron | poweroff\n"
      "  modeset --mode WxH@Hz   run the section 4.3 modeset sequence\n"
      "  testpattern --mode WxH@Hz [--bars | --solid R,G,B] [--no-modeset]\n"
      "                          modeset then push one full frame\n"
      "  image --mode WxH@Hz --bmp FILE [--no-modeset]\n"
      "                          push a 24 or 32 bit BMP, letterboxed\n"
      "\n"
      "The control plane lives on the HID interface and the bulk pixel pipe\n"
      "on the WinUSB one, so the default 'auto' transport opens both. Pass\n"
      "--transport hid to stay read-only with no driver installed at all.\n");
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

std::unique_ptr<Transport> MakeTransport(bool need_data_plane) {
  std::string error;

  if (g_options.transport == TransportKind::kFile) {
    std::unique_ptr<FileTransport> file =
        FileTransport::Create(g_options.dump_dir, &error);
    if (!file) {
      fprintf(stderr, "error: %s\n", error.c_str());
    }
    return file;
  }

  if (g_options.transport == TransportKind::kWinUsb) {
    std::unique_ptr<WinUsbTransport> winusb = WinUsbTransport::Open(&error);
    if (!winusb) {
      fprintf(stderr, "error: %s\n", error.c_str());
    }
    return winusb;
  }

  /* Composite is the only combination that can do both planes, because the
   * control transfers target the HID interface and the bulk pipe is on the
   * WinUSB one. */
  if (g_options.transport == TransportKind::kComposite ||
      (g_options.transport == TransportKind::kAuto && need_data_plane)) {
    std::unique_ptr<CompositeTransport> composite =
        CompositeTransport::Open(&error);
    if (composite) {
      return composite;
    }
    if (g_options.transport == TransportKind::kComposite ||
        need_data_plane) {
      fprintf(stderr, "error: %s\n", error.c_str());
      fprintf(stderr,
              "hint: sending pixels needs the WinUSB data plane; run\n"
              "      scripts\\install-winusb.ps1, or use --transport file\n");
      return nullptr;
    }
  }

  std::unique_ptr<HidTransport> hid = HidTransport::Open(&error);
  if (!hid) {
    fprintf(stderr, "error: %s\n", error.c_str());
  }
  return hid;
}

std::unique_ptr<Device> OpenDevice(bool need_data_plane) {
  std::unique_ptr<Transport> transport = MakeTransport(need_data_plane);
  if (!transport) {
    return nullptr;
  }
  printf("transport: %s\n", transport->Describe().c_str());
  return std::unique_ptr<Device>(new Device(std::move(transport)));
}

bool ParseModeSpec(const char* text, uint16_t* width, uint16_t* height,
                   uint16_t* hz) {
  unsigned w = 0, h = 0, r = 60;
  if (sscanf_s(text, "%ux%u@%u", &w, &h, &r) < 2) {
    return false;
  }
  *width = static_cast<uint16_t>(w);
  *height = static_cast<uint16_t>(h);
  *hz = static_cast<uint16_t>(r);
  return true;
}

const Mode* ResolveMode(const char* spec) {
  uint16_t width = 0, height = 0, hz = 60;
  if (!ParseModeSpec(spec, &width, &height, &hz)) {
    fprintf(stderr, "error: bad mode '%s', expected WxH@Hz\n", spec);
    return nullptr;
  }
  const Mode* mode = FindMode(width, height, hz);
  if (!mode) {
    fprintf(stderr,
            "error: %ux%u@%u is not in the mode table; run 'msdisp modes'\n",
            width, height, hz);
    return nullptr;
  }
  return mode;
}

int CmdList() {
  std::vector<DeviceLocation> hid = HidTransport::Enumerate();
  printf("HID control interfaces:\n");
  if (hid.empty()) {
    printf("  none\n");
  }
  for (const DeviceLocation& loc : hid) {
    printf("  %04X:%04X  %ws\n", loc.vid, loc.pid, loc.path.c_str());
  }

  std::vector<DeviceLocation> winusb = WinUsbTransport::Enumerate();
  printf("WinUSB display interfaces:\n");
  if (winusb.empty()) {
    printf("  none (run scripts\\install-winusb.ps1)\n");
  }
  for (const DeviceLocation& loc : winusb) {
    printf("  %04X:%04X  %ws\n", loc.vid, loc.pid, loc.path.c_str());
  }
  return hid.empty() && winusb.empty() ? 1 : 0;
}

int CmdDump() {
  std::string error;
  std::unique_ptr<WinUsbTransport> transport = WinUsbTransport::Open(&error);
  if (!transport) {
    fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  printf("transport: %s\n", transport->Describe().c_str());
  std::string text;
  if (!transport->DumpDescriptors(&text)) {
    fprintf(stderr, "error: descriptor query failed\n");
    return 1;
  }
  fputs(text.c_str(), stdout);
  return 0;
}

int CmdInfo() {
  std::unique_ptr<Device> device = OpenDevice(false);
  if (!device) {
    return 1;
  }

  ChipInfo chip;
  if (!device->ReadChipInfo(&chip)) {
    fprintf(stderr, "error: %s\n", device->last_error().c_str());
    return 1;
  }
  printf("chip:      %s, signature %02X %02X %02X\n",
         ChipFamilyName(chip.family), chip.signature[0], chip.signature[1],
         chip.signature[2]);
  if (chip.family != ChipFamily::kUnknown) {
    printf("timings:   flash base 0x%04X\n", chip.custom_timing_base);
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
    printf("edid:      header magic missing, this is not valid EDID\n");
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

  std::unique_ptr<Device> device = OpenDevice(false);
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
  std::unique_ptr<Device> device = OpenDevice(false);
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

int CmdPower(bool on) {
  std::unique_ptr<Device> device = OpenDevice(false);
  if (!device) {
    return 1;
  }
  bool ok = on ? device->PowerOn() : device->PowerOff();
  if (!ok) {
    fprintf(stderr, "error: %s\n", device->last_error().c_str());
    return 1;
  }
  printf("power %s\n", on ? "on" : "off");
  return 0;
}

int CmdModeset(int argc, char** argv) {
  const char* spec = nullptr;
  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      spec = argv[++i];
    } else {
      fprintf(stderr, "error: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (!spec) {
    fprintf(stderr, "error: --mode is required\n");
    return 2;
  }
  const Mode* mode = ResolveMode(spec);
  if (!mode) {
    return 2;
  }

  std::unique_ptr<Device> device = OpenDevice(false);
  if (!device) {
    return 1;
  }
  if (!device->PowerOn()) {
    fprintf(stderr, "error: power on: %s\n", device->last_error().c_str());
    return 1;
  }
  if (!device->SetResolution(*mode)) {
    fprintf(stderr, "error: modeset: %s\n", device->last_error().c_str());
    return 1;
  }
  printf("modeset %ux%u@%u using chip mode 0x%02X\n", mode->width, mode->height,
         mode->hz, mode->mode_id);
  return 0;
}

/* Minimal 24/32 bit uncompressed BMP loader. Returns BGRA top-down. */
bool LoadBmp(const char* path, std::vector<uint8_t>* pixels, int* width,
             int* height) {
  FILE* file = nullptr;
  if (fopen_s(&file, path, "rb") != 0 || !file) {
    fprintf(stderr, "error: could not open %s\n", path);
    return false;
  }
  BITMAPFILEHEADER file_header = {};
  BITMAPINFOHEADER info_header = {};
  bool ok = fread(&file_header, sizeof(file_header), 1, file) == 1 &&
            fread(&info_header, sizeof(info_header), 1, file) == 1;
  if (!ok || file_header.bfType != 0x4D42 ||
      info_header.biCompression != BI_RGB ||
      (info_header.biBitCount != 24 && info_header.biBitCount != 32)) {
    fclose(file);
    fprintf(stderr, "error: %s is not an uncompressed 24 or 32 bit BMP\n",
            path);
    return false;
  }

  const int w = info_header.biWidth;
  const bool bottom_up = info_header.biHeight > 0;
  const int h = bottom_up ? info_header.biHeight : -info_header.biHeight;
  const int bpp = info_header.biBitCount / 8;
  const size_t src_stride = (static_cast<size_t>(w) * bpp + 3) & ~size_t(3);

  std::vector<uint8_t> row(src_stride);
  pixels->assign(static_cast<size_t>(w) * h * 4, 0);
  fseek(file, static_cast<long>(file_header.bfOffBits), SEEK_SET);

  for (int y = 0; y < h; ++y) {
    if (fread(row.data(), 1, src_stride, file) != src_stride) {
      fclose(file);
      fprintf(stderr, "error: %s is truncated\n", path);
      return false;
    }
    int dst_y = bottom_up ? (h - 1 - y) : y;
    uint8_t* dst = pixels->data() + static_cast<size_t>(dst_y) * w * 4;
    for (int x = 0; x < w; ++x) {
      dst[x * 4 + 0] = row[x * bpp + 0];
      dst[x * 4 + 1] = row[x * bpp + 1];
      dst[x * 4 + 2] = row[x * bpp + 2];
      dst[x * 4 + 3] = 0xFF;
    }
  }
  fclose(file);
  *width = w;
  *height = h;
  return true;
}

int PushFullFrame(Device* device, const Mode& mode,
                  const std::vector<uint8_t>& framebuffer) {
  Rect rect;
  rect.x1 = 0;
  rect.y1 = 0;
  rect.x2 = mode.width;
  rect.y2 = mode.height;
  rect = AlignDamageRect(rect, mode.width, mode.height);

  std::vector<uint8_t> transfer(TransferLength(rect));
  size_t len = FrameRect(transfer.data(), transfer.size(), framebuffer.data(),
                         static_cast<size_t>(mode.width) * 4, mode.width,
                         mode.height, rect);
  if (len == 0) {
    fprintf(stderr, "error: framing failed\n");
    return 1;
  }
  printf("sending %zu bytes (%ux%u UYVY + %zu overhead)\n", len, mode.width,
         mode.height, kFrameOverhead);
  if (!device->SendFrame(transfer.data(), len)) {
    fprintf(stderr, "error: %s\n", device->last_error().c_str());
    return 1;
  }
  printf("frame sent\n");
  return 0;
}

int CmdTestPattern(int argc, char** argv) {
  const char* spec = "1920x1080@60";
  bool bars = true;
  bool do_modeset = true;
  uint8_t solid[3] = {0, 0, 0};

  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      spec = argv[++i];
    } else if (strcmp(argv[i], "--bars") == 0) {
      bars = true;
    } else if (strcmp(argv[i], "--no-modeset") == 0) {
      do_modeset = false;
    } else if (strcmp(argv[i], "--solid") == 0 && i + 1 < argc) {
      unsigned r = 0, g = 0, b = 0;
      if (sscanf_s(argv[++i], "%u,%u,%u", &r, &g, &b) != 3) {
        fprintf(stderr, "error: --solid wants R,G,B\n");
        return 2;
      }
      solid[0] = static_cast<uint8_t>(r);
      solid[1] = static_cast<uint8_t>(g);
      solid[2] = static_cast<uint8_t>(b);
      bars = false;
    } else {
      fprintf(stderr, "error: unknown option %s\n", argv[i]);
      return 2;
    }
  }

  const Mode* mode = ResolveMode(spec);
  if (!mode) {
    return 2;
  }

  std::unique_ptr<Device> device = OpenDevice(true);
  if (!device) {
    return 1;
  }

  if (do_modeset) {
    if (!device->PowerOn() || !device->SetResolution(*mode)) {
      fprintf(stderr, "error: %s\n", device->last_error().c_str());
      return 1;
    }
    printf("modeset %ux%u@%u using chip mode 0x%02X\n", mode->width,
           mode->height, mode->hz, mode->mode_id);
  }

  const size_t stride = static_cast<size_t>(mode->width) * 4;
  std::vector<uint8_t> framebuffer(stride * mode->height);
  if (bars) {
    FillColourBars(framebuffer.data(), stride, mode->width, mode->height);
  } else {
    FillSolid(framebuffer.data(), stride, mode->width, mode->height, solid[0],
              solid[1], solid[2]);
  }
  return PushFullFrame(device.get(), *mode, framebuffer);
}

int CmdImage(int argc, char** argv) {
  const char* spec = "1920x1080@60";
  const char* bmp_path = nullptr;
  bool do_modeset = true;

  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      spec = argv[++i];
    } else if (strcmp(argv[i], "--bmp") == 0 && i + 1 < argc) {
      bmp_path = argv[++i];
    } else if (strcmp(argv[i], "--no-modeset") == 0) {
      do_modeset = false;
    } else {
      fprintf(stderr, "error: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (!bmp_path) {
    fprintf(stderr, "error: --bmp is required\n");
    return 2;
  }
  const Mode* mode = ResolveMode(spec);
  if (!mode) {
    return 2;
  }

  std::vector<uint8_t> image;
  int image_width = 0, image_height = 0;
  if (!LoadBmp(bmp_path, &image, &image_width, &image_height)) {
    return 1;
  }

  const size_t stride = static_cast<size_t>(mode->width) * 4;
  std::vector<uint8_t> framebuffer(stride * mode->height, 0);
  const int copy_w = image_width < mode->width ? image_width : mode->width;
  const int copy_h = image_height < mode->height ? image_height : mode->height;
  for (int y = 0; y < copy_h; ++y) {
    memcpy(framebuffer.data() + static_cast<size_t>(y) * stride,
           image.data() + static_cast<size_t>(y) * image_width * 4,
           static_cast<size_t>(copy_w) * 4);
  }
  printf("loaded %dx%d from %s\n", image_width, image_height, bmp_path);

  std::unique_ptr<Device> device = OpenDevice(true);
  if (!device) {
    return 1;
  }
  if (do_modeset) {
    if (!device->PowerOn() || !device->SetResolution(*mode)) {
      fprintf(stderr, "error: %s\n", device->last_error().c_str());
      return 1;
    }
  }
  return PushFullFrame(device.get(), *mode, framebuffer);
}

}  // namespace

int main(int argc, char** argv) {
  int index = 1;
  while (index < argc && argv[index][0] == '-') {
    if (strcmp(argv[index], "--transport") == 0 && index + 1 < argc) {
      const char* kind = argv[++index];
      if (strcmp(kind, "hid") == 0) {
        g_options.transport = TransportKind::kHid;
      } else if (strcmp(kind, "winusb") == 0) {
        g_options.transport = TransportKind::kWinUsb;
      } else if (strcmp(kind, "file") == 0) {
        g_options.transport = TransportKind::kFile;
      } else if (strcmp(kind, "composite") == 0) {
        g_options.transport = TransportKind::kComposite;
      } else if (strcmp(kind, "auto") == 0) {
        g_options.transport = TransportKind::kAuto;
      } else {
        fprintf(stderr, "error: unknown transport '%s'\n", kind);
        return 2;
      }
      ++index;
    } else if (strcmp(argv[index], "--dump-dir") == 0 && index + 1 < argc) {
      g_options.dump_dir = argv[++index];
      ++index;
    } else {
      break;
    }
  }

  if (index >= argc) {
    PrintUsage();
    return 2;
  }

  const char* command = argv[index];
  int rest_argc = argc - index - 1;
  char** rest_argv = argv + index + 1;

  if (strcmp(command, "list") == 0) {
    return CmdList();
  }
  if (strcmp(command, "dump") == 0) {
    return CmdDump();
  }
  if (strcmp(command, "info") == 0) {
    return CmdInfo();
  }
  if (strcmp(command, "edid") == 0) {
    return CmdEdid(rest_argc, rest_argv);
  }
  if (strcmp(command, "timings") == 0) {
    return CmdTimings();
  }
  if (strcmp(command, "modes") == 0) {
    return CmdModes();
  }
  if (strcmp(command, "poweron") == 0) {
    return CmdPower(true);
  }
  if (strcmp(command, "poweroff") == 0) {
    return CmdPower(false);
  }
  if (strcmp(command, "modeset") == 0) {
    return CmdModeset(rest_argc, rest_argv);
  }
  if (strcmp(command, "testpattern") == 0) {
    return CmdTestPattern(rest_argc, rest_argv);
  }
  if (strcmp(command, "image") == 0) {
    return CmdImage(rest_argc, rest_argv);
  }

  PrintUsage();
  return 2;
}
