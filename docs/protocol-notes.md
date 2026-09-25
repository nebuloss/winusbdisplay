# Protocol notes and corrections

`AGENT_PROMPT.md` documents the MacroSilicon wire protocol as reverse
engineered in the `ms912x` Linux driver. This file records where that
description is wrong, incomplete, or where the hardware disagreed with it.

Three sources were reconciled:

1. **`rhgndf/ms912x`** (GPL-2.0) — reverse engineered from Windows captures of
   the USB 2 parts.
2. **MacroSilicon's own GPL Linux DRM driver**, `MS91xx_Linux_Drm_SourceCode`
   — vendor source with a `usb_hal/` layer, covers 912x and 913x. This names
   everything `ms912x` had to guess at.
3. **Real hardware**: one dongle reporting USB id `345F:9133`.

Where they disagree, the hardware wins, then the vendor source, then `ms912x`.

---

## Verified on hardware

| What | Result |
|---|---|
| USB id | `345F:9133`, composite: MI_00 HID, MI_01 audio, MI_03 display (class FF) |
| Chip signature at `0xF000` | `B7 16 0A` |
| Chip | **MS912C**, *not* a 913x |
| Connector register `0x0031` | `0x05` = HDMI |
| Display status `0x0032` | `0x01` = connected |
| EDID | 128 bytes, checksum valid, Acer KA240HQ, preferred 1920x1080 |
| Custom timings in flash | none programmed (`modify1`/`modify2` marker absent) |
| Modeset sequence | completes without error over the control plane |
| Colour conversion | matches the reference coefficients exactly (see below) |

### Correction: product id does not imply chip family

`AGENT_PROMPT.md` §3 maps `345F:*` to "MS913x". That is wrong. This dongle
carries a **912x die behind a USB 3 product id**. The chip id register at
`0xFF00` did not match; the one at `0xF000` did.

Always probe the chip id. Never infer it from the USB product id, because the
chip id also selects the custom timing base address in flash (`0x1C00` for
912x, `0xFC50` for 913x) and reading the wrong one yields garbage.

The vendor source decodes the 912x signature further, by its first byte:

| Signature byte 0 | Chip |
|---|---|
| `0xB7` | MS912C |
| `0xA7` | MS912A |
| anything else | MS9120 |

---

## The control plane is reachable without any driver

§4.1 says control traffic is HID `SET_REPORT`/`GET_REPORT` with `wValue`
`0x0300` and `wIndex` `0`. Decoding that:

- `wIndex = 0` means **interface 0**, which on these dongles is a plain vendor
  HID collection that Windows already binds `hidusb.sys` to. It is *not* the
  display interface.
- `wValue = 0x0300` means **feature report, report id 0**.

So `HidD_SetFeature` / `HidD_GetFeature` on the MI_00 HID device emit byte for
byte the same control transfers. The entire control plane — chip id, connector
type, EDID, flash, power, and the whole modeset sequence — works with **no INF,
no signing, and no need to displace the vendor driver**. Only pixels need
WinUSB.

This is what `msdisp --transport hid` uses, and it is by far the fastest way to
make progress on a machine where driver installation is awkward.

Windows prepends a report id byte to the `HidD_*` buffer, so the buffer is 9
bytes for an 8 byte wire payload. `HIDP_CAPS.FeatureReportByteLength` is 9.

---

## Correction: register reads return four bytes, not one

`ms912x` reads registers one byte at a time. The vendor HAL shows the response
payload is `{ op, addr_hi, addr_lo, data[4], reserved }` and defines
`MS9132_HID_OP_XDATA_READ_MAX_CNT = 4`.

Reading four consecutive bytes per round trip makes a 128 byte EDID block cost
**32 transfers instead of 128**. Verified on hardware: the 4-byte read produces
a byte-identical EDID to the 1-byte read.

This matters because EDID is re-read on hotplug and 128 round trips is a
visible stall.

---

## Correction: the "pointless" register reads are not handshakes

§4.3 says the three register reads in the modeset sequence "appear to be
required handshakes". They are not. From the vendor source:

| Register | Meaning |
|---|---|
| `0x0030` | SDRAM type (2M / 4M / 8M / 16M / none) |
| `0x0031` | video port / connector type |
| `0x0032` | HPD, display status |
| `0x0033` | read during modeset, purpose still unknown |
| `0xC620` | read during modeset, purpose still unknown |

The vendor driver reads the SDRAM type for real and passes it to its screen
enable path, which picks different mute registers per chip and port. `ms912x`
discards the value because it does not need it.

Keeping the reads costs nothing and preserves the captured ordering, so this
code keeps them. But do not go looking for a handshake protocol that is not
there.

---

## Correction: `vic` in the mode command is not a CEA-861 VIC

§4.3 step 7 calls the first byte of the `cmd=0x02` payload a "CEA-861 Video
Identification Code". For the low values it coincides with one, but the table
runs up to `0x81` and those are not VICs. The vendor calls this command
`UPDATE_OUT_INFO` and the field `index`: it is an index into the chip's own
output timing table.

The full table, from `ms912x` and confirmed by the vendor's structure:

| index | mode | index | mode |
|---|---|---|---|
| `0x02` | 720x480@60 | `0x54` | 1280x768@60 |
| `0x11` | 720x576@50 | `0x56` | 1280x768@75 |
| `0x13` | 1280x720@50 | `0x57` | 1280x800@60 |
| `0x1F` | 1920x1080@50 | `0x5B` | 1280x960@60 |
| `0x22` | 1920x1080@30 | `0x60` | 1280x1024@60 |
| `0x40` | 640x480@60 | `0x61` | 1280x1024@75 |
| `0x42` | 800x600@60 | `0x64` | 1360x768@60 |
| `0x44` | 800x600@75 | `0x66` | 1366x768@60 |
| `0x47` | 1024x768@60 | `0x67` | 1400x1050@60 |
| `0x49` | 1024x768@75 | `0x6B` | 1440x900@60 |
| `0x4C` | 1152x864@60 | `0x73` | 1600x1200@60 |
| `0x4E` | 1280x600@60 | `0x78` | 1680x1050@60 |
| `0x4F` | 1280x720@60 | `0x81` | 1920x1080@60 |

---

## The command sub-opcodes, named

§4.2 lists `cmd` values with two marked "undocumented". The vendor names all of
them. The opcode byte `0xA6` is `HID_OP_VIDEO`; the second byte is a sub-op:

| sub-op | vendor name | payload |
|---|---|---|
| `0x00` | `TRIGGER_FRAME` | `{ index, delay }` |
| `0x01` | `UPDATE_IN_INFO` | `{ width_be, height_be, colour, byte_sel }` |
| `0x02` | `UPDATE_OUT_INFO` | `{ index, colour, width_be, height_be }` |
| `0x03` | `SET_TRANS_MODE` | `{ mode, param0..3 }` |
| `0x04` | `TRANSFER` (enable) | `{ enable }` |
| `0x05` | `VIDEO_ENABLE` | `{ enable }` |
| `0x07` | `POWER` | `{ on, data }` |

So `ms912x`'s "undocumented" `0x03` with payload `{0x03, ...}` is
`SET_TRANS_MODE(MANUAL_BLOCK)`. **That is the setting that makes partial
updates possible at all.** Transfer modes:

| value | meaning |
|---|---|
| 0 | whole frame |
| 1 | fixed block, M x N |
| 2 | fixed block, W x H |
| 3 | manual block (what we use — damage rectangles) |
| 4 | bypass, whole frame |
| 5 | bypass, manual block |

And `0x04` is simply transfer enable/disable, bracketing the reprogramming.

---

## Correction: send a zero-length bulk packet after every frame

Not mentioned in `AGENT_PROMPT.md` at all. The vendor driver does this after
every single frame:

```c
usb_bulk_msg(udev, usb_sndbulkpipe(udev, ep), zero_msg, 0, &snd_len, 2000);
```

A zero length bulk OUT packet terminates the transfer. Without it the chip can
sit waiting for more data and the panel stays dark **even though every transfer
reported success** — which is exactly the symptom §10 attributes to a reordered
modeset sequence. Check this first.

`ms912x` does not send one and reportedly works on USB 2 parts, so this may be
913x specific, but sending it is harmless and matches the vendor.

---

## Correction: do not enable output until the first frame has landed

`ms912x` finishes its modeset with `VIDEO_ENABLE(1)`. The vendor driver
explicitly does the opposite:

```c
// disable video, until first frame sends successfully
```

It leaves video and screen disabled at the end of the modeset and only enables
them after a frame has been transferred. Otherwise the panel shows whatever was
left in the chip's memory. This code does the same: `SetResolution()` leaves
output off and `SendFrame()` enables it on first success.

---

## Frame framing: confirmed exactly

The vendor's `usb_hal_package_block` packs the header byte for byte the same
way `ms912x` does, so this is solid:

```
offset 0   FF 00                      marker
offset 2   3 bytes  be24: (x & 0xFFF) << 12 | (y & 0xFFF)
offset 5   3 bytes  be24: (w & 0xFFF) << 12 | (h & 0xFFF)
offset 8   UYVY pixel data, w*2 bytes per row, no row padding
end - 8    FF C0 00 00 00 00 00 00    footer
```

Total length is `w * 2 * h + 16`. There is **no padding** between the header
and the pixel data: §4.4's "`u8 padding[8]`" is wrong. The header is 8 bytes,
the footer is 8 bytes, and `FRAME_OVERHEAD = 16` is the sum of the two.

Damage rectangles must have an even `x` and an even width, because UYVY encodes
pixels in pairs.

## Colour conversion

Two different coefficient sets exist for the same BT.601 limited range matrix:

```
ms912x (16 bit, more precise — what this code uses):
  Y = 16  + (16763*R + 32904*G +  6391*B) >> 16
  U = 128 + (-9676*R - 18996*G + 28672*B) >> 16
  V = 128 + (28672*R - 24009*G -  4663*B) >> 16

vendor (10 bit):
  Y = 16  + (263*R + 516*G +  97*B) >> 10
  U = 128 + (-152*R - 298*G + 450*B) >> 10
  V = 128 + (450*R - 377*G -  73*B) >> 10
```

U and V are averaged across each horizontal pixel pair. The vendor clamps to
0..255; `ms912x` does not, and with 8 bit RGB inputs neither set overflows, so
clamping is belt and braces.

Spot checked against the scalar implementation in this repo via the loopback
transport:

| colour | expected UYVY | produced |
|---|---|---|
| white | `80 EA 80 EA` | `80 EA 80 EA` |
| cyan | `A5 A8 10 A8` | `A5 A8 10 A8` |
| blue | `EF 28 6D 28` | `EF 28 6D 28` |
| black | `80 10 80 10` | `80 10 80 10` |

## The panel double buffers

`ms912x` sends the union of the current and previous frame's damage
rectangles, and the vendor source alternates a `frame_index` between 0 and 1
with a `TRIGGER_FRAME` command available to select which buffer is shown. Send
the union, or trailing damage is left stale on screen every other frame.

---

## Still unverified

Everything below needs the bulk pipe, which needs driver installation:

- whether any frame actually appears on the panel
- whether the zero length packet is required on this specific chip
- bandwidth in practice, and therefore how aggressive damage coalescing has
  to be
- hotplug, suspend and resume
- custom timing decoding (this dongle's flash has none programmed)

---

## Windows-specific constraint: the two planes live on different interfaces

Not a protocol issue, but it shapes the whole driver design and is invisible
from the Linux sources.

The control transfers are class requests with recipient = Interface and
`wIndex = 0`, so they target **interface 0**, the HID collection. The bulk
pixel endpoint is on a **different interface** (MI_03 on the USB 3 parts).

On Linux one driver holds the whole `usb_device` and can address either
interface. On Windows each interface is owned by a different driver, and
WinUSB refuses to forward an interface-recipient control request to an
interface it does not own: it fails with `ERROR_GEN_FAILURE` (0x1F).

So the working arrangement is a **hybrid transport**:

| plane | interface | owner | mechanism |
|---|---|---|---|
| control | MI_00 | hidusb (in-box) | `HidD_SetFeature` / `HidD_GetFeature` |
| data | MI_03 | WinUSB (our INF) | `WinUsb_WritePipe` on endpoint 4 |

This is what `CompositeTransport` exists for, and it is verified working: the
panel displays our frames.

Note the WinUSB pipe is **exclusive**. The `msdisp` tool and the indirect
display driver cannot both hold it; whichever opens it first wins and the
other gets `ERROR_ACCESS_DENIED` (0x5). Disable the driver's device node when
using the tool.

## Verified on hardware, second pass

| What | Result |
|---|---|
| Bulk OUT pipe | endpoint 0x04, 512 byte max packet, USB 2.0 high speed |
| Full 1080p frame transfer | ~160 ms wall clock, consistent with ~35 MB/s |
| Modeset then full frame | **panel displays the image** |
| Solid colours and colour bars at 1080p | correct, no fringing, no tearing |
| 720p60 (chip mode 0x4F) | works |

So sections 4.1 through 4.5 of `AGENT_PROMPT.md`, with the corrections above,
are confirmed end to end against real hardware.

---

## Measured performance ceiling

Benchmarked with `msdisp bench` on the MS912C test unit, sending full 1080p
frames back to back:

| Configuration | Throughput | Full-frame rate |
|---|---|---|
| Synchronous writes | 29.5 MB/s | 7.4 fps |
| Pipelined, depth 2 | 29.4 MB/s | 7.4 fps |
| Pipelined, depth 4 | 29.5 MB/s | 7.5 fps |
| Pipelined, depth 8 | 29.7 MB/s | 7.5 fps |

Pipelining with RAW_IO and up to eight overlapped transfers in flight makes no
measurable difference, which means **the device, not the host or the bus, is
the limit**. USB 2.0 high speed can carry about 53 MB/s of bulk traffic and a
well driven host reaches 40 to 45; this chip tops out at 29.6.

Three independent confirmations that the silicon is USB 2:

1. Chip id `B7 16 0A` at `0xF000` decodes to MS912C, which the vendor source
   places in the 912x (USB 2) family.
2. The device descriptor reports `bcdUSB 2.00` with 512 byte bulk packets.
3. There is **no BOS descriptor**. A USB 3 capable device carries one even
   when it negotiates high speed, so this rules out the port, cable or hub
   being the constraint.

Note the product id `345F:9133` appears in the USB 3 list in
`AGENT_PROMPT.md` §3, but this unit is USB 2 silicon behind that id. Another
reason never to infer capability from the product id.

Per-mode consequences, all measured:

| Mode | Bytes/frame | Full-frame rate |
|---|---|---|
| 1920x1080 | 4.1 MB | 7.5 fps |
| 1280x720 | 1.8 MB | 15.0 fps |
| 1024x768 | 1.6 MB | 19.2 fps |

So the only ways to go faster are to send fewer pixels (damage tracking) or
smaller frames (lower resolution). There is no encoding to fall back on: the
chip's formats are RGB565, RGB888, YUV422 and YUV444, and the 16 bpp ones we
already use are the cheapest available.

### vSyncFreqDivider does not work here

`DISPLAYCONFIG_VIDEO_SIGNAL_INFO.AdditionalSignalInfo.vSyncFreqDivider` looks
like the designed answer: let the panel run at 60 Hz while the OS composes
less often. Setting it to anything above 1 makes Windows reject the whole
topology, with `SetDisplayConfig` failing with `ERROR_GEN_FAILURE` and the
path disappearing. Pacing is therefore done by dropping frames in
`FrameSender`, and by offering 1920x1080@30 (chip mode `0x22`) as a real mode
so a user can pick a rate the link can sustain.

## DDC/CI is not routed to indirect displays

The driver implements a complete DDC/CI slave (`src/driver/ddcci.cpp`): VCP
`0x10` brightness, `0x12` contrast, the capabilities string, and correct
checksums on both directions. It is wired to `EvtIddCxMonitorI2CTransmit` and
`EvtIddCxMonitorI2CReceive`.

Windows never calls it. Probing with the same API Twinkle Tray uses:

```
monitor: Generic PnP Monitor        <- real Acer on the Intel GPU
  capabilities: (prot(monitor)type(lcd)model(ACER)...vcp(...10 12...))
  VCP 0x10 brightness: current=35 max=100

monitor: Generic PnP Monitor        <- ours
  capabilities length FAILED err=50     (ERROR_NOT_SUPPORTED)
  VCP 0x10 read FAILED err=50
```

The driver log shows no `ddcci:` transmit entries at all, so the callbacks are
never invoked. Microsoft's documentation for the newer
`EvtIddCxMonitorI2CTransmitAndReceive` says outright that it is "safe for the
driver to expose the new function, but the OS doesn't use it". DDC/CI goes
from `dxva2.dll` straight to the graphics adapter's hardware I2C master, which
an indirect display has no place in.

The slave is kept because it is correct and costs nothing if this ever
changes. Brightness is delivered instead through a registry value the driver
polls, applied during colour conversion so it genuinely dims the picture. See
`scripts/brightness.ps1`.
