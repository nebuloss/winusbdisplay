# Task: Windows Indirect Display Driver for MacroSilicon USB display adapters

You are working on a Windows development machine. Your job is to build a
working Windows display driver for a USB-to-VGA/HDMI dongle based on a
MacroSilicon MS912x / MS913x chip, so the dongle can be used as a normal
secondary monitor without the vendor's closed-source driver.

Read this whole document before writing code. It contains the wire protocol,
which you will not be able to rediscover on your own without a logic analyser.

---

## 1. Why this exists

The vendor (MacroSilicon, resold under many brands) ships a signed but closed
Windows driver. It works, but it is unauditable, lags behind Windows releases,
is frequently flagged by antivirus, and has no ARM64 story worth relying on.
The protocol itself is simple and already fully documented by the Linux
community. So a clean Windows implementation is very achievable.

The goal is a driver that a user installs and then sees an extra monitor in
Settings > Display, which they can extend or mirror onto, at 1080p, with a
usable cursor and no visible tearing on typical desktop use.

Non-goals, at least initially: gaming framerates, HDR, portrait/rotated modes,
audio (the chip exposes a separate UAC interface that Windows already handles),
multiple dongles at once.

---

## 2. Architecture you must use

Windows has not allowed third-party kernel display miniports for USB devices
for a long time. The supported mechanism is **IddCx — the Indirect Display
Driver class extension**, a UMDF2 user-mode driver. Do not attempt a WDDM
miniport; it is the wrong tool and will not be signable.

The driver has two halves, and it is important to keep them separate in your
mind and in your source tree:

```
  Windows compositor
        |
        v
  [ IddCx frontend ]     <- virtual monitor, EDID, mode list, cursor,
        |                   swapchain acquire/release loop
        v
  [ frame pipeline ]     <- D3D texture -> staging -> XRGB8888 -> UYVY 4:2:2,
        |                   damage rect tracking
        v
  [ USB backend ]        <- WinUSB: control transfers for registers/commands,
        |                   bulk OUT for pixel data
        v
     the dongle
```

The frontend is generic IddCx work. The backend is entirely device-specific and
is where the protocol below applies. Write them so the backend could be
swapped for a file-dump or loopback stub during testing — you will need exactly
that when the hardware misbehaves.

**Use WinUSB for the transport.** Write an INF that binds `WinUSB.sys` to the
vendor-specific interface, and do all device I/O from user mode via
`WinUsb_ControlTransfer` and `WinUsb_WritePipe`. There is no reason to write
any kernel code in this project. The IddCx driver and the WinUSB access live in
the same UMDF2 driver binary.

---

## 3. Identify the hardware first

Before anything else, plug the dongle in and record what it actually is. Run:

```
pnputil /enum-devices /connected
```

or check Device Manager > Details > Hardware Ids. You are looking for one of:

| VID:PID | Chip family | USB | Notes |
|---|---|---|---|
| `534D:6021` | MS912x | USB 2.0 | most common |
| `534D:0821` | MS912x | USB 2.0 | |
| `345F:9132` | MS913x | USB 3 | |
| `345F:9133` | MS913x | USB 3 | |
| `345F:9135` | MS913x | USB 3 | |

**Report which one you found before proceeding.** The two families differ in
chip-ID register location and custom-timing base address (see §4.6), and the
USB 3 parts have far more bandwidth headroom, which changes how aggressive you
must be about damage tracking.

Also enumerate the interfaces and endpoints (`WinUsb_QueryInterfaceSettings`,
`WinUsb_QueryPipe`) and confirm you can see a bulk OUT pipe at endpoint 4. Log
the full descriptor set to a file on first run; you will refer back to it.

---

## 4. The wire protocol

This is reverse-engineered from USB captures of the Windows driver and
corroborated against the vendor's own GPLv2 Linux driver. Treat it as accurate
but verify each step against real hardware as you implement it.

### 4.1 Register and command access: HID class control transfers

All control-plane traffic is HID `SET_REPORT` / `GET_REPORT` on the control
pipe, even though you are not writing a HID driver:

- Request: `SET_REPORT` = `0x09`, `GET_REPORT` = `0x01`
- Request type: `Class | Interface`, direction as appropriate
- `wValue` = `0x0300`, `wIndex` = `0`
- Payload is always **8 bytes**

Three payload shapes exist, distinguished by the first byte:

**Read a byte from a register** (`type = 0xB5`):
```
struct { u8 type; u16 addr_be; u8 data[5]; }   // 8 bytes, addr big-endian
```
Send it with `SET_REPORT`, then immediately issue a `GET_REPORT` for 8 bytes;
the register value comes back in `data[0]`.

**Write a 6-byte command** (`type = 0xA6`):
```
struct { u8 type; u8 cmd; u8 data[6]; }        // 8 bytes
```
Send with `SET_REPORT`. No response.

**Read flash** (`type = 0xF5`):
```
struct { u8 type; u8 addr[3]; u8 reserved[4]; } // 8 bytes, addr big-endian 24-bit
```
`SET_REPORT` then `GET_REPORT`; you get 8 bytes of flash content back. Loop,
advancing the address by 8, to read longer runs.

**Serialize all of this behind one lock.** The SET/GET pair is a stateful
sequence; interleaving two of them from different threads returns garbage.

### 4.2 Commands

Written via the `0xA6` form. `cmd` values:

| cmd | Meaning |
|---|---|
| `0x01` | Set resolution |
| `0x02` | Set mode |
| `0x03` | Undocumented, part of modeset sequence |
| `0x04` | Undocumented, part of modeset sequence |
| `0x05` | Output enable |
| `0x07` | Power |

Power on is `cmd=0x07`, data `{0x01, 0x02, 0, 0, 0, 0}`.
Power off is `cmd=0x07`, data all zero.

### 4.3 The modeset sequence

This exact order was taken from Windows captures. Do not reorder it or drop the
seemingly pointless register reads — they appear to be required handshakes, and
the chip will silently produce no output if you skip them.

1. write `cmd=0x04`, data `{0, 0, 0, 0, 0, 0}`
2. read byte at register `0x0030`  (discard value)
3. read byte at register `0x0033`  (discard value)
4. read byte at register `0xC620`  (discard value)
5. write `cmd=0x03`, data `{0x03, 0, 0, 0, 0, 0}`
6. write `cmd=0x01` with the resolution payload:
   ```
   struct { u16 width_be; u16 height_be; u8 pixel_format; u8 byte_select; }
   ```
   `pixel_format = 0x22` (UYVY), `byte_select = 0x00`.
   (`0x11` is RGB888; UYVY is what the vendor driver uses and what is known to
   work. Try RGB888 later as an experiment, not now.)
7. write `cmd=0x02` with the mode payload:
   ```
   struct { u8 vic; u8 pixel_format; u16 width_be; u16 height_be; }
   ```
   `vic` is the CEA-861 Video Identification Code for the mode.
   `pixel_format = 0x01` here — note this is a *different* field from step 6.
8. write `cmd=0x04`, data `{0x01, 0, 0, 0, 0, 0}`
9. write `cmd=0x05`, data `{0x01, 0, 0, 0, 0, 0}`   (output enable)

### 4.4 Framebuffer transfers

Pixel data goes out the **bulk OUT pipe on endpoint 4**, as a single transfer
per damage rectangle:

```
[ 16-byte header ] [ UYVY pixel data ] [ 8-byte footer ]
```

Header:
```
u16 marker_be    = 0xFF00
u8  position[3]  = big-endian 24-bit: (x & 0xFFF) << 12 | (y & 0xFFF)
u8  dimensions[3]= big-endian 24-bit: (w & 0xFFF) << 12 | (h & 0xFFF)
u8  padding[8]   = 0        // header struct is 8 bytes; total overhead is 16
```
Note the constant is named `FRAME_OVERHEAD = 16` in the reference code: 8 bytes
of header struct plus the 8-byte footer. Compute transfer length as
`width * 2 * height + 16`.

Footer, appended immediately after the pixel data:
```
FF C0 00 00 00 00 00 00
```

Pixel data is **UYVY 4:2:2**, two pixels per four bytes, written as
`U Y1 V Y2`, row by row, `width * 2` bytes per row, no padding between rows.

**Damage rectangles must be x-aligned to an even pixel**, because UYVY encodes
pixels in pairs. Round `x1` down to even and `x2` up to even, clamped to the
framebuffer width. Failing to do this produces colour fringing on the left edge
of updated regions, which is a confusing symptom if you have not anticipated it.

### 4.5 Colour conversion

XRGB8888 to UYVY, with Y1/Y2 computed per pixel and U/V averaged across the
pair. The reference implementation uses fixed-point coefficients:

```
Y = 16  + (16763*R + 32904*G +  6391*B) >> 16
U = 128 + (-9676*R - 18996*G + 28672*B) >> 16
V = 128 + (28672*R - 24009*G -  4663*B) >> 16
```

Do this with SIMD or a compute shader. A scalar loop will not keep up at 1080p
and will eat a core. Since IddCx hands you a D3D texture, a compute shader that
outputs directly into a staging buffer is the natural choice and avoids a
CPU-side copy of the RGB data entirely. Start scalar to get correctness, then
optimise, and keep the scalar version as a reference to diff against.

### 4.6 EDID and mode enumeration

**EDID** is read one byte at a time from registers starting at `0xC000`:
block 0 is `0xC000`..`0xC07F`, block 1 continues from `0xC080`. That is 128
control-transfer round-trips per block, so cache it and only re-read on hotplug.

Feed the EDID to IddCx as the monitor descriptor. If the EDID is absent or
fails its checksum, fall back to a synthesized one — do not fail to create the
monitor, or the user sees nothing at all and has no way to diagnose it.

**Connector type** is register `0x0031`:
`0`=CVBS, `1`=S-Video, `2`=VGA, `3`=YPbPr, `4`=CVBS+S-Video, `5`=HDMI,
`6`=Digital. Use it to decide the default mode list: HDMI/VGA get the full
list, YPbPr gets {720p60, 1080p60, 480p60, 576p50}, CVBS/S-Video only get
{480p60, 576p50}.

Register `0x0032` is display status — poll it for hotplug detection.

**Custom timings** live in flash. Read the chip ID first to know where:
- MS913x: chip ID at `0xFF00`, signature MSB `0x13`, timing base `0xFC50`
- MS912x: chip ID at `0xF000`, signature MSB `0x16`, timing base `0x1C00`
- signature LSB is `0x0A` for both

At the timing base there is a 7-byte ASCII marker: `"modify1"` means one custom
record follows, `"modify2"` means two. Records start at base+`0x10`, stride
`0x20`, and are packed little-endian:

```
u8  vic; u8 polarity;
u16 htotal, vtotal, hactive, vactive, pixclk, vfreq, hoffset, voffset,
    hsyncwidth, vsyncwidth;
```
`polarity` bit 0 = progressive, bit 1 = positive HSync, bit 2 = positive VSync.
`vfreq` is in centihertz (divide by 100 for Hz).

This is optional polish — implement it after the fixed mode list works.

---

## 5. Bandwidth: the constraint that shapes everything

On USB 2.0 you get roughly **35 MB/s** in practice. A full 1920x1080 UYVY frame
is 4.1 MB. That is about **8 full frames per second**. There is no clever
encoding to rescue you; the chip takes raw UYVY.

Consequences you must design around from the start, not retrofit:

- **Damage tracking is mandatory**, not an optimisation. IddCx gives you dirty
  rects in `IDARG_OUT_RELEASEANDACQUIREBUFFER`. Use them. Typical desktop use
  (text cursor blinking, a window moving) touches a tiny fraction of the screen
  and will feel fine.
- **Drop frames deliberately.** Use two transfer buffers so conversion and USB
  transfer overlap, and if the next buffer is still in flight when a new frame
  arrives, *discard the new frame* rather than queueing it. The reference
  implementation waits 10 ms for the buffer and gives up. Queueing will build
  unbounded latency and eventually stall the compositor's acquire loop, which
  Windows interprets as a hung display.
- **Never block the IddCx processing thread on USB.** Convert on that thread if
  you must, but hand the transfer to a worker.
- Consider coalescing multiple small dirty rects into one bounding-box transfer
  when they are close together; each transfer has fixed overhead.

On the USB 3 parts (`345F:*`) you have roughly 10x the budget and can be far
less careful, but write the code as though you are on USB 2 anyway.

---

## 6. Reference material

You will find these invaluable. Read them; do not guess at IddCx semantics.

**`github.com/rhgndf/ms912x`** — GPL-2.0 Linux DRM driver, reverse-engineered
from Windows captures of *this exact chip*. It is the authoritative source for
the protocol above. Key files: `ms912x.h` (all constants and struct layouts),
`ms912x_registers.c` (control transfers, modeset sequence), `ms912x_transfer.c`
(colour conversion, framing), `ms912x_connector.c` (EDID, mode lists).

**MacroSilicon's own GPLv2 Linux driver** —
`http://www.macrosilicon.com:9080/download/USBDisplay/Linux/SourceCode/`
Vendor source, ~8700 lines, with a clean `usb_hal/` abstraction layer. Covers
the USB 3 MS913x parts. Useful for cross-checking and for anything `ms912x`
marks as unknown. Note it does *not* cover the USB 2 parts.

**`github.com/gnif/LookingGlass`, `idd/` directory** — GPL-2.0, and by a wide
margin the most complete open IddCx implementation in existence. Its *purpose*
is different (it ships frames to a VM host over shared memory, not over USB),
but the entire frontend half is the same problem you have: the acquire/release
loop with correct timeouts and teardown, D3D11/D3D12 interop to get at the
acquired texture without stalling the compositor, hardware cursor setup,
frame pacing so Windows does not declare the monitor hung, monitor
arrival/departure and mode enumeration. Study it for the *shape* of a working
IddCx driver.

Two things you cannot borrow from Looking Glass: it is root-enumerated (a
software device with no hardware behind it) whereas you must bind to a real USB
interface, and it assumes effectively infinite bandwidth to its sink, so its
"send every frame" model is wrong for you and will deadlock your acquire loop.

**Microsoft's `IndirectDisplay` sample** in `microsoft/Windows-driver-samples`
— MIT licensed, but essentially a stub that discards frames. Good for the
skeleton, INF structure and build setup; useless for anything real.

---

## 7. Licensing — decide this before you write code

Every open reference above is GPL-2.0: `ms912x`, the vendor Linux driver, and
Looking Glass's IDD. If you copy or transliterate code from any of them, the
result is GPL-2.0, which is fine for a personal or open project but makes WHQL
attestation signing and redistribution awkward.

If a permissive or commercially distributable result is required, treat all
three as **specification only**: build from Microsoft's MIT-licensed sample and
implement against the protocol documented in §4, which is a description of
hardware behaviour rather than copyrightable expression. Keep a note of which
approach was chosen and stick to it consistently.

**Ask the user which they want if it is not already clear.** This decision is
expensive to reverse.

---

## 8. Suggested order of work

Do not try to build the whole thing before testing any of it. Each phase should
end with something observable.

1. **Enumerate.** WinUSB INF, bind the device, dump descriptors to a log.
   Observable: your INF loads and you can open the device handle.
2. **Talk to it.** Implement the `0xB5` read path. Read the chip ID and the
   video-port register. Observable: you print a plausible chip signature and
   connector type that matches the physical port.
3. **Read EDID.** Dump all 128 (or 256) bytes, verify the checksum, parse it
   with any EDID tool. Observable: the dumped EDID matches the monitor you
   have plugged in.
4. **Light it up.** Implement the modeset sequence and push one hardcoded
   full-screen frame — a solid colour, then colour bars. Observable: the
   monitor displays it. This is the milestone that proves the protocol; get
   here before touching IddCx at all, using a standalone console program.
5. **Colour conversion.** Feed a real image through the RGB->UYVY path and
   compare on screen. Observable: an image displays with correct colours.
   Scalar first.
6. **IddCx skeleton.** Monitor arrives in Windows, correct EDID, correct mode
   list, frames discarded. Observable: an extra display appears in Settings.
7. **Wire the halves together.** Full frames, no damage tracking. Observable:
   the desktop appears on the dongle, slowly.
8. **Damage tracking and double buffering.** Observable: dragging a window is
   responsive; a static desktop generates almost no USB traffic.
9. **Cursor, hotplug, suspend/resume, custom timings.** Polish.
10. **SIMD or compute-shader conversion.** Optimise last, with the scalar path
    retained for correctness diffing.

---

## 9. Testing and debugging

- Keep a **loopback backend** that writes each would-be USB transfer to a file
  as a PPM or raw UYVY dump. When the screen is black you need to know whether
  the bug is in conversion or in transport, and this separates them in seconds.
- **USBPcap + Wireshark** to capture your own traffic. If you can also install
  the vendor driver on a spare machine or VM and capture *its* traffic doing
  the same operation, a diff of the two is the single most effective debugging
  tool available for this project.
- Enable the **WDF verifier** and IddCx tracing early, not after things break.
- Test the ugly cases explicitly: unplug during active display, sleep/resume,
  changing resolution while frames are in flight, and a monitor whose EDID is
  missing or corrupt.

## 10. Known symptoms and their causes

Note these now; each will otherwise cost you an afternoon.

| Symptom | Likely cause |
|---|---|
| Black screen, transfers succeed | modeset sequence reordered or a register read skipped (§4.3) |
| Colour fringing on left edge of updates | damage rect `x` not aligned to even pixel (§4.4) |
| Garbage on screen, wrong stride | forgot rows are `width*2` bytes with no padding |
| Control reads return garbage | SET/GET pair not serialized under a lock (§4.1) |
| Display goes blank under load, Windows says monitor hung | queueing frames instead of dropping them (§5) |
| Nothing at all after a clean install | EDID read failed and you refused to create the monitor (§4.6) |

---

## 11. Reporting back

At the end of each phase, state plainly: what works, what you verified it
against, and what you had to guess. Where hardware behaviour contradicts this
document, **the hardware is right** — say so explicitly and record the
correction, because this document will be wrong in small ways.
