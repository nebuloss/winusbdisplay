# winusbdisplay

An open Windows display driver for MacroSilicon MS912x / MS913x USB-to-HDMI
and USB-to-VGA dongles, so they work as a normal secondary monitor without the
vendor's closed-source driver.

Two pieces:

- **`msdisp`** — a standalone console tool for bring-up and diagnosis. Reads
  the chip id, connector type and EDID over the in-box HID stack with **no
  driver installation at all**, and can drive modesets and push test patterns
  once WinUSB is bound.
- **`ms912xidd.dll`** — a UMDF2 indirect display driver (IddCx) that presents
  the dongle to Windows as a real monitor.

Licensed GPL-2.0, because it is derived from the GPL-2.0 `ms912x` Linux driver
and MacroSilicon's own GPL-2.0 Linux sources.

## Status

Working: the dongle appears as a second monitor in Windows and shows the
desktop.

| Phase | State |
|---|---|
| Control plane: chip id, connector, hotplug | verified on hardware |
| EDID read and checksum | verified on hardware |
| Modeset sequence | verified on hardware |
| Colour conversion and framing | verified byte for byte |
| Pixels on the panel | working |
| IddCx driver: monitor appears and shows the desktop | **working** |
| Damage tracking, move regions, idle refresh | working |
| Brightness control | working, but not via DDC/CI (see below) |

The test unit reports USB id `345F:9133` but carries an **MS912C** die running
at USB 2.0 high speed. See `docs/protocol-notes.md`.

## Performance

Measured, not estimated:

| Mode | Full-frame rate |
|---|---|
| 1920x1080 | 7.5 fps |
| 1280x720 | 15.0 fps |
| 1024x768 | 19.2 fps |

This is a hardware ceiling, confirmed three ways: the chip id says MS912C,
there is no BOS descriptor (so the silicon is not USB 3 capable), and
pipelining up to eight overlapped transfers changes throughput by less than
1%. The device saturates at 29.6 MB/s while USB 2.0 itself can carry 40 to 45.

Full-frame rate only matters for full-screen video. Ordinary desktop use is
driven by damage tracking, which sends what actually changed:

```
damage: 20x22   at (268,332)  ->    896 bytes
damage: 24x22   at (1172,624) ->  1,072 bytes
damage: 604x52  at (180,398)  -> 62,832 bytes
```

Roughly 1 KB per update rather than 4 MB. For smoother full-screen content,
pick a lower resolution: 1280x720 doubles the frame rate, and 1920x1080@30 is
offered as a native chip mode.

## Brightness

Windows does not route DDC/CI to indirect displays, so Twinkle Tray and
similar tools report this monitor as unsupported. That is an OS limitation,
not a gap in this driver: Microsoft's own DDI documentation states the OS does
not call an indirect display driver's I2C callbacks, and probing confirms they
are never invoked. The driver implements a full DDC/CI slave regardless, in
case that changes.

Brightness works through a registry value instead, applied during colour
conversion so it genuinely dims the panel:

```
powershell -File scripts\brightness.ps1 -Brightness 60
powershell -File scripts\brightness.ps1              :: show current
```

## Quick start

No installation needed for the read-only commands:

```
scripts\build-tool.bat
build\msdisp.exe list
build\msdisp.exe info
build\msdisp.exe edid --out monitor.edid
```

To check the frame pipeline without hardware, dump frames to disk instead of
sending them:

```
build\msdisp.exe --transport file testpattern --mode 1920x1080@60 --bars
```

## Sending pixels

No reboot and no test signing are needed: neither package loads a third-party
kernel binary, so all that is required is a trusted code signing certificate,
which the install scripts create for you. It works with Secure Boot enabled.

```
powershell -ExecutionPolicy Bypass -File scripts\elev.ps1 -Start
powershell -ExecutionPolicy Bypass -File scripts\elev.ps1 ^
    -Script %CD%\scripts\install-all.ps1

build\msdisp.exe testpattern --mode 1920x1080@60 --bars
powershell -ExecutionPolicy Bypass -File scripts\visual-check.ps1
```

`elev.ps1` starts one hidden elevated worker, so you get a single UAC prompt
for the whole session instead of one per step.

The WinUSB pixel pipe is exclusive, so the tool and the driver cannot both use
it. To use the tool while the driver is installed, disable the driver first:

```
powershell -File scripts\elev.ps1 -Command ^
    "Disable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:$false"
```

Remove everything with `scripts\purge.ps1`.

## Layout

```
src/common/     protocol, transports and frame pipeline, shared by both binaries
src/tools/      the msdisp console tool
src/driver/     the IddCx UMDF2 driver
inf/            driver packages
scripts/        build and install
docs/           protocol corrections and troubleshooting
```

## Documentation

- `docs/protocol-notes.md` — what the hardware actually does, and where the
  published descriptions are wrong. Read this before changing anything in
  `src/common`.
- `docs/troubleshooting.md` — symptoms and their usual causes.
- `AGENTS.md` — orientation for anyone, human or otherwise, working in this
  repository.
