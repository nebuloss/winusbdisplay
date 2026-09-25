# Troubleshooting

Symptoms and their usual causes, ordered roughly by how often they bite.

## Nothing detected at all

```
build\msdisp.exe list
```

If no HID control interface is listed, the dongle is not enumerating. Check
`pnputil /enum-devices /connected` for `VID_345F` or `VID_534D`. Note that the
dongle may enumerate with a different product id in different modes: one unit
appears as both `9132` and `9133` at different times, and one of the
interfaces is a mass storage "driver CD" partition.

## Black screen, but every transfer reports success

In order of likelihood:

1. **No zero length packet after the frame.** The chip waits for more data.
   See `docs/protocol-notes.md`; `Device::SendFrame` sends one.
2. **Output never enabled.** Output is deliberately left off after a modeset
   and only enabled once a frame lands. If `SendFrame` short-circuits, the
   panel stays dark.
3. **Modeset sequence reordered.** Keep the order in `Device::SetResolution`.
4. **Wrong transfer mode.** `SET_TRANS_MODE` must be manual block (3) or the
   chip will not accept partial updates.

Use `--transport file` to dump what would have been sent and confirm the
pipeline is producing sane frames before blaming the chip.

## Colour fringing on the left edge of updated regions

The damage rectangle's `x` is odd. UYVY encodes pixels in pairs.
`AlignDamageRect` handles this; if you bypass it, you get fringing.

## Garbage on screen, image sheared

Row stride. Rows are `width * 2` bytes with no padding between them. Note the
source side is different: a mapped D3D staging texture has its own
`RowPitch` which is usually larger than `width * 4`.

## Control reads return garbage

Two control sequences interleaved. The `SET_REPORT` then `GET_REPORT` pair is
stateful. Everything in `Device` serializes on `ctrl_mutex_`; do not add a path
that talks to the transport directly.

## Stale content left on screen every other frame

The panel double buffers. Each transfer must cover the union of this frame's
damage and the previous frame's damage.

## Display goes blank under load, Windows says the monitor is hung

Frames are being queued instead of dropped. `FrameSender::AcquireBuffer`
returns `nullptr` after a short wait and the caller must drop the frame, not
retry. Never block the IddCx processing thread on USB.

## Driver will not install

- Test signing off: `bcdedit /set testsigning on`, reboot, Secure Boot off.
- The vendor package outranks ours. Windows prefers a WHQL signed package over
  a test signed one at the same hardware id specificity, so the install
  scripts delete the vendor package. Run without `-KeepVendorDriver`.
- `pnputil /enum-drivers` shows which `oemNN.inf` is actually bound.

## Build failures

- `Unknown or unsupported property value '.' for UmdfVersion` — the WDK reads
  the `UmdfVersion` property, not the `UmdfVersionMajor`/`UmdfVersionMinor`
  pair that older templates use.
- `IDDCX_VERSION_MAJOR is not defined` — the IddCx headers need
  `IDDCX_VERSION_MAJOR` and `IDDCX_VERSION_MINOR` as preprocessor defines;
  they are not derived from the include path.
- A wall of syntax errors inside `wdfusb.h` — include `<usb.h>` and
  `<usbspec.h>` before `<wdf.h>` and `<wdfusb.h>`.
- `Cannot load DLL 'x86\InfVerif.dll'` — the standalone Build Tools install
  has no x86 verifier. The project sets `SkipPackageVerification`.
- `inf2cat ... Signability test failed, 22.9.1` — the INF and every file it
  copies must sit in one directory. The build does not stage them, so catalog
  generation is done by `scripts/install-driver.ps1` instead.
- ARM64: needs the ARM64 build tools component installed alongside the WDK.

## Useful captures

USBPcap plus Wireshark will capture your own traffic. If you can install the
vendor driver on a spare machine and capture *its* traffic doing the same
operation, diffing the two is by far the most effective debugging tool
available for this project.
