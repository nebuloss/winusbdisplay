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

| Phase | State |
|---|---|
| Enumerate the device | done |
| Control plane: chip id, connector, display status | **verified on hardware** |
| EDID read and checksum | **verified on hardware** |
| Flash read / custom timings | works (none programmed on the test unit) |
| Modeset sequence | **verified on hardware** |
| Colour conversion and frame framing | **verified byte for byte** |
| Pixels on the panel | **working: colour bars and solid colours at 1080p and 720p** |
| IddCx driver: installs, starts, reads EDID, builds mode list | working |
| IddCx driver: monitor appears in Windows | **not yet** (see below) |

The protocol half of this project is finished and proven. `msdisp` drives the
panel end to end.

The indirect display driver installs and starts cleanly, opens both transports,
reads the monitor's EDID and builds its mode list, but `IddCxMonitorArrival`
returns `STATUS_DEVICE_NOT_READY`, so no extra monitor appears in Settings yet.
Investigation so far is written up in `docs/troubleshooting.md`.

The test hardware is a dongle with USB id `345F:9133` that turns out to carry
an **MS912C** die running at USB 2.0 high speed. See `docs/protocol-notes.md`.

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
