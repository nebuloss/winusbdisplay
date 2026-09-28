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

---

## Performance audit

Measured with per-phase timers inside the driver, not estimated. One full
1080p frame, before any optimisation:

| Phase | Cost | What it is |
|---|---|---|
| `CopySubresourceRegion` | 0.02 ms | GPU side, negligible |
| `Map` | 4.6 ms | GPU to CPU synchronisation |
| convert | 10.6 ms | RGB to UYVY on the CPU |
| USB transfer | 133.6 ms | hardware ceiling |

The conversion number was the surprise. The same conversion over ordinary RAM
measures 3.98 ms, so roughly 6.6 ms of the 10.6 was the cost of *reading* the
mapped staging texture. Mapped GPU memory is far slower to read than normal
memory, and at 1080p the source is 8.3 MB.

Two fixes, both verified against the scalar reference with `msdisp selftest`:

1. **The SIMD inner loop had a store forwarding stall.** It computed each dot
   product, wrote the vector lanes to a stack array and read them back, six
   times per four pixels. Rewritten to stay in registers and emit one 16 byte
   UYVY store per eight pixels. About 2x on its own.
2. **Rows are now converted on a small persistent thread pool.** This helps
   more than the arithmetic saving suggests, because several threads keep more
   cache misses outstanding against the slow mapped memory at once.

Result:

| | Before | After |
|---|---|---|
| Convert, full 1080p frame | 10.6 ms | 2.5 ms |
| Convert, typical 532x402 damage | 0.75 ms | 0.29 ms |
| CPU per full frame | 15.2 ms | 7.0 ms |

End to end this is a few percent, because USB still dominates at 133 ms. What
it buys is a much more responsive acquire loop and less CPU burnt per frame.

The remaining CPU cost is the 4.5 ms `Map`. Removing it means converting on
the GPU with a compute shader and reading back UYVY, which would also halve
the readback from 8.3 MB to 4.1 MB. That is the optimisation `AGENT_PROMPT`
§4.5 suggests and it is the obvious next step, worth perhaps another 4 ms.

## The ERROR_GEN_FAILURE was real, and partly our fault

`SetDisplayConfig` was failing with `ERROR_GEN_FAILURE` (31) whenever
`vSyncFreqDivider` was set above 1. Two separate problems were tangled here.

**The mode timings were wrong.** `MakeSignalInfo` reported `totalSize` equal
to `activeSize`, so there was no blanking interval, and computed `pixelRate`
and `hSyncFreq` from the active area. Those fields have to satisfy:

```
pixelRate = totalSize.cx * totalSize.cy * vSyncFreq
hSyncFreq = pixelRate / totalSize.cx
```

With `totalSize == activeSize` they did not. This is now fixed with roughly
CVT reduced blanking (160 pixels horizontal, 45 lines vertical) and the
identities hold. This was a genuine correctness bug worth fixing on its own.

**The divider is still rejected.** With the timings corrected and the divider
left at 1, `SetDisplayConfig` returns 0 and the display comes up. Set the
divider to 2 or more and it returns 31 again, so Windows is refusing the
divider itself rather than reacting to malformed timings. It is left behind
the `SyncDivider` registry switch, default off.

## GPU conversion

The remaining CPU cost after the SIMD and threading work was the readback:
about 4.5 ms of the 7 ms per full frame went on `Map`, waiting for and then
reading the acquired surface. A compute shader removes both halves of that
problem. It converts on the GPU and writes UYVY straight into a buffer, so the
only thing crossing the bus is the 4.1 MB actually transmitted rather than the
8.3 MB of source RGBA, and the CPU does no conversion at all.

`src/driver/convert_cs.hlsl`, compiled offline by `scripts/build-driver.bat`
into `convert_cs.h`, so the driver needs no runtime shader compiler. Each
thread produces one UYVY quad, and the integer coefficients are identical to
the scalar reference.

Correctness is checked at runtime, not assumed: the first frame after the
swapchain is assigned is converted both ways and compared. On the test machine
the largest difference over all 4,147,200 bytes is **0**, exactly matching the
CPU path. If it ever exceeded two the driver logs it and falls back
permanently.

Measured, driving the same animation on the USB display:

| Path | Full frame | Typical damage | CPU used |
|---|---|---|---|
| CPU, threaded SIMD | ~7.0 ms | ~1.6 ms | 3.3% of a core |
| GPU compute | ~6.8 ms | ~1.0 ms | **1.5% of a core** |

Wall clock is close to break even, which is worth being clear about. The win is
that the processor is left alone: half the CPU for the same work. On this
machine the GPU is an integrated Intel part sharing system memory, so reading
back from it was never especially slow. On a discrete GPU the CPU path would
have to drag 8.3 MB per frame across PCIe while the GPU path drags 4.1 MB, and
the gap should be considerably wider.

The remaining cost is that `Map` on the readback buffer still blocks until the
GPU finishes. Removing that needs either a second readback buffer, which costs
a frame of latency, or D3D12 async copy queues with fences. Looking Glass does
the latter, but it streams at full refresh rate to a VM; here USB caps the
frame rate at 7.5 fps and the conversion is already about 5% of the transfer
time, so neither is currently worth the complexity.

Set `UseComputeShader` to 0 under `HKLM\SOFTWARE\winusbdisplay` to force the
CPU path, which is also used automatically if the GPU path cannot initialise.

### Two bugs the GPU path exposed

Both were found by making the verification honest rather than by inspection,
and are worth recording because neither is obvious.

**The first verification was meaningless.** It ran once, on the first frame
after the swapchain was assigned. That frame is a full-screen update of a
desktop that has not been drawn yet, so it is uniformly black. Comparing black
to black reports a perfect match no matter how broken the conversion is. The
check now runs over the first several frames, skips any frame whose pixels are
all identical, and only declares the GPU path trustworthy after eight frames
with real content at differing sizes and offsets have matched.

**The idle refresh resent stale pixels.** `SendRefresh` re-read the staging
texture, which on the CPU path always held the most recent frame. On the GPU
path nothing writes to that texture during normal operation, so after two and
a half seconds of an unchanged desktop the panel was sent whatever happened to
be left in it. It now keeps the last acquired surface, which IddCx guarantees
stays valid until the next acquire, and re-converts from that through whichever
path is active.

## The chip is vsync locked, and that changes the whole performance picture

Measured with `msdisp benchsizes`, which times transfers of increasing size:

| Rect | Bytes | Time | Updates/s |
|---|---|---|---|
| 1920x128 | 491 KB | 16.7 ms | **60** |
| 1920x136 | 522 KB | 17.9 ms | 56 |
| 1920x144 | 553 KB | 33.2 ms | 30 |
| 1920x256 | 983 KB | 33.4 ms | 30 |
| 1920x320 | 1.2 MB | 49.5 ms | 20 |
| 1920x540 | 2.1 MB | 67.0 ms | 15 |
| 1920x1080 | 4.1 MB | 133.1 ms | 7.5 |

Every figure is a multiple of 16.67 ms. The chip does not simply stream at
some byte rate: it completes a bulk transfer on its own 60 Hz vsync boundary.
The cost of an update is therefore quantised, and the only thing that matters
is how many periods it spans:

```
  bytes <= ~520 KB   1 period    60 updates/s
  bytes <= ~1.0 MB   2 periods   30 updates/s
  bytes <= ~4.1 MB   8 periods   7.5 updates/s
```

Note the shape of it: a 8 KB update and a 491 KB update both cost exactly one
period. Below the threshold, size is free.

This reframes the earlier "7.5 fps" conclusion. That number is correct, but it
only describes a full screen repaint. Ordinary desktop damage is a few
kilobytes to a few hundred, comfortably inside one period, so interactive use
runs at the full 60 updates per second. It also means coalescing several small
dirty rectangles into one transfer is close to free, while splitting one update
into two transfers doubles its cost.

### What this rules out

Sending less data per update stops helping once an update already fits in one
period, which for normal desktop work it always does. That is why the SIMD,
threading and GPU work changed CPU use but not frame rate: the transfer was
never waiting on us.

### Idle keepalive

The panel blanks without traffic, so a static desktop still needs periodic
updates. A full repaint costs eight periods, so sending only a band looks
attractive. It does not work: the chip alternates between two frame buffers on
every transfer, so a partial update lands in one and leaves the other holding
older content, and the two alternate visibly on screen. Partial updates have to
be tracked per buffer, which is what `pending_damage_` does; a refresh whose
whole purpose is to resynchronise sends the whole screen.

The same reasoning caught a real bug: the refresh was clearing the pending
damage for *both* buffers while writing only one, so the unwritten buffer kept
stale content and it reappeared on the next flip. It now clears only the buffer
it wrote and alternates, so consecutive refreshes bring both current.

---

# Second implementation, `usbdisplay/`

Findings from the rewrite. Everything above still holds; this is what was
learned or corrected afterwards.

## The chroma coefficients had to be adjusted by one

The 16 bit constants published with the reference driver cannot be used
directly by the processor's vector instructions, which need coefficients that
fit a signed 16 bit lane. Halving them and rounding each independently left
the red-blue difference row summing to **-1** instead of 0, so every neutral
grey pixel acquired a faint tint. `kCoeffVb` is rounded the other way on
purpose to restore the sum, and a static assertion now enforces it for both
chroma rows.

The luma row deliberately still undershoots: full white converts to 234, not
the nominal 235. That is what the reference produces and what was confirmed
against the panel, so it is what we match.

## A zero dirty rectangle count means nothing changed

The metadata documents this explicitly, and it is easy to read the other way.
Treating "no regions reported" as "assume the whole screen changed" repaints
everything on exactly the frames where there is nothing to draw. A full
repaint costs eight slots, sixteen once it has reached both of the adapter's
internal copies, and the wire is blocked throughout. The symptom is a display
that lags by hundreds of milliseconds while apparently idle.

## Each region has to reach both copies, and the pair must be atomic

Confirmed again, with a new failure mode. Sending the two halves of the pair
independently means the first can go out and the second find no free buffer,
leaving the region in one copy only. The two copies then alternate on screen.
With a moving mouse pointer this is unmistakable: **the pointer appears in
two places at once**, the old position refusing to erase.

Both buffers are now taken before either is sent, so a region either reaches
both copies or is not sent at all. An unsent region stays owed.

Note also that sending the pair back to back costs exactly what sending it a
frame apart costs, two transfers either way, and delivers the region a frame
sooner.

## Refinement has to compare converted pixels, not source pixels

The compositor sometimes reports a single region covering the whole screen
when almost nothing has changed. Comparing against what was last sent and
transmitting only the part that genuinely differs turns that back into a
one-slot update.

The comparison runs **after** conversion, which sounds backwards. It is not:
conversion costs a small fraction of a transfer, so converting a region and
then discovering most of it was unnecessary is still a large win. More
importantly it is the only arrangement that works for both conversion paths.
Comparing source pixels would need a retained copy of the desktop, and the
GPU path never reads the source into memory the processor can see, so that
copy could never be kept current.

Two rules make it correct:

- The record is updated only once a region has reached **both** copies.
  Updating it after the first transfer makes the second look unnecessary,
  which is how the two-pointer symptom appears even with an atomic pair.
- Abandoning a region must **not** discard the record. Nothing was sent, so
  it is still accurate. Discarding it turns a moment of congestion into a
  lasting one: with nothing to compare against, every later region goes out
  in full, which causes more congestion.

## Spare capacity is worth spending

The link is idle most of the time, because ordinary desktop damage is far
below the one-slot threshold. Rather than waiting out a fixed interval before
the repaint that keeps the panel's signal alive, the driver repaints a band
whenever the link has gone **completely** quiet, walking it down the screen.
This keeps the signal alive and repairs anything that somehow went stale.

The condition is "nothing queued and nothing on the wire", not "a buffer is
free". Queueing speculative work keeps the adapter busy at the cost of making
the next real update wait behind it, which is the opposite of what spare
capacity is for.

A band rather than the whole screen: one slot against eight, doubled for the
pair. A partial repaint is only safe because it is sent twice like everything
else; sent once it would land in one copy and reintroduce the alternation it
exists to prevent.

## Windows drops the pixel interface when its package is replaced

Reinstalling the WinUSB package detaches the adapter's display interface, and
it does not come back on its own: the device has to be disabled and re-enabled
before Windows republishes the interface the driver looks for. Until then the
driver reports, accurately but confusingly, that the package is not installed.

Related: Windows keeps every copy of a package ever added under a fresh
`oemNN` name, and stale copies go on competing to claim the hardware. The
first implementation had accumulated **59** copies of its two packages this
way. The installer now removes its own previous copies, and skips the WinUSB
package entirely when the installed one is already current.

## Brightness: the path that does work

Earlier work concluded brightness was impossible for this display and
recorded two dead ends. Both were correctly diagnosed and both are still
dead:

- **DDC/CI**, the protocol that travels along a display cable, needs the
  graphics card's I2C master. An indirect display is not attached to one.
  Probing returns `ERROR_NOT_SUPPORTED` and the driver's I2C callbacks are
  never invoked.
- **WMI**, the path laptop panels use, needs `monitor.sys` to find a
  brightness interface on a kernel display driver. There is no kernel driver
  here to expose one.

The investigation stopped there, and that was the mistake: there is a third
path, and it was available the whole time.

**The gamma ramp reaches us.** The display stack will hand a driver the
gamma table that `SetDeviceGammaRamp` sets, through
`EvtIddCxMonitorSetGammaRamp`, which exists as far back as IddCx 1.2. But it
only does so if the driver declares it applies one. The driver was setting
`GammaSupport` to `IDDCX_FEATURE_IMPLEMENTATION_NONE`, which is a promise not
to, so the callback was never called and the mechanism was invisible to
exactly the sort of probing that was done.

Declaring `IDDCX_FEATURE_IMPLEMENTATION_SOFTWARE` and applying the table
during colour conversion makes the panel dim. Verified on hardware.

Two things about applying it:

- **On the source colour, not the encoded result.** That is where a gamma
  table is defined to act. Applying it afterwards would shift the chroma and
  colours would drift as brightness changed.
- **Identically in both conversion paths.** The same rule as the conversion
  itself, for the same reason: an update that crosses the size threshold
  takes a different path on consecutive frames, so any disagreement flickers.

A table that only alters bits below the eighth is treated as neutral and
skipped, since conversion discards them anyway and the neutral case is the
common one.

`usbdisplayctl`'s companion `brightnessprobe` reports which of the three
paths each attached monitor answers, so this question never has to be
settled by argument again.

### What this does not fix

Whether a given application offers a slider is that application's decision.
Twinkle Tray, the usual choice, only gained a gamma-based mode in
**1.18.0-beta2**; its current stable release, 1.17.2, contains no such code
at all and will report this monitor as unsupported no matter what the driver
does. There is a setting, `useSoftwareBrightnessFallback`, and it is off by
default even in the versions that have it.

So the driver side is done and any tool that sets a gamma ramp now works.
Getting a slider in one specific application depends on that application.

### The limit of what brightness can do here

At full brightness the driver does nothing to the picture: the adjustment is
a multiply by one and is skipped entirely. So if a screen on this adapter
looks dimmer than one beside it with both controls at 100, the difference is
not in software and no amount of it will help.

It is the panel's own backlight, and this adapter cannot reach it. The chip
exposes exactly twenty-four operations, listed in the vendor's own header:
register, flash, EEPROM, SDRAM, USB3 and HDMI PHY access, plus the video
command. **None of them carries I2C to the attached screen.** The chip reads
the screen's capabilities into its own registers itself and offers no way to
send anything back out, so the monitor's brightness controls are reachable
only from its own buttons.

What the driver can do is dim the picture below that, which is the whole
range a normal monitor's control covers minus the top end. Matching two
screens therefore means turning the brighter one down, not the dimmer one
up.

### Dimming has to follow light, not stored values

Worth stating because the obvious implementation is wrong and looks almost
right. A monitor's own control dims its backlight, and emitted light falls
in proportion to the setting. Pixels are not stored in proportion to light:
they are stored on roughly a square-and-a-bit curve. Scaling the stored
value by the setting therefore produces far less light than the setting
suggests, and at half it lands near a fifth.

The correction is to raise the fraction to the reciprocal of that exponent
before scaling, so half asks for a stored value of 0.73. Both conversion
paths take the resulting integer from one place, so they cannot drift apart.
