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
| Modeset sequence | runs clean; visual result unverified |
| Colour conversion and frame framing | **verified byte for byte** offline |
| Pixels on the panel | blocked on driver installation |
| IddCx driver | builds clean; not yet installed |

The test hardware is a dongle with USB id `345F:9133` that turns out to carry
an **MS912C** die. See `docs/protocol-notes.md`.

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

The data plane needs the dongle's display interface bound to WinUSB, which
means test signing:

```
bcdedit /set testsigning on      :: then reboot, Secure Boot must be off
```

Then, from an elevated prompt:

```
powershell -ExecutionPolicy Bypass -File scripts\install-winusb.ps1
build\msdisp.exe testpattern --mode 1920x1080@60 --bars
```

## Installing the display driver

```
powershell -ExecutionPolicy Bypass -File scripts\install-driver.ps1
```

An extra monitor should appear in Settings > System > Display. Reverse with
`scripts\uninstall-driver.ps1`.

`install-winusb.ps1` and `install-driver.ps1` claim the same interface, so
install one or the other, not both.

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
