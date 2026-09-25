# AGENTS.md

Orientation for working in this repository. Read `docs/protocol-notes.md`
before changing anything under `src/common`.

## What this is

A Windows indirect display driver (IddCx / UMDF2) plus a bring-up tool for
MacroSilicon MS912x / MS913x USB display dongles. GPL-2.0, deliberately: it is
derived from the GPL-2.0 `ms912x` Linux driver and MacroSilicon's own GPL-2.0
Linux sources. **Do not relicense or accept permissively licensed
reimplementations without revisiting that decision** — it was made explicitly
and it is expensive to reverse.

`AGENT_PROMPT.md` is the original task specification. It is largely accurate
but wrong in several specific places; `docs/protocol-notes.md` records every
correction with evidence. When they conflict, the protocol notes win.

## Commands

```
scripts\build-tool.bat                      # msdisp console tool -> build\msdisp.exe
scripts\build-driver.bat [Config] [Plat]    # IddCx driver -> build\driver\<plat>\<config>\
scripts\install-winusb.ps1                  # bind WinUSB for the tool   (elevated)
scripts\install-driver.ps1                  # install the display driver (elevated)
scripts\uninstall-winusb.ps1 / uninstall-driver.ps1
```

`build-tool.bat` compiles with `/W4 /WX`; the tool build must stay
warning-clean. The driver project is `/W4` without `/WX` because the WDK
headers are not clean at that level.

There is no test suite. The verification loop is the tool itself:

```
build\msdisp.exe list
build\msdisp.exe info                                   # chip, connector, HPD
build\msdisp.exe edid --out monitor.edid
build\msdisp.exe --transport file testpattern --bars    # offline pipeline check
```

## Environment realities

- Toolchain present: VS 2022 **Build Tools** (no IDE), MSVC 14.44, WDK
  10.0.26100. ARM64 driver builds fail — the ARM64 toolset component is not
  installed.
- The shell here is a Bash-compatible interpreter on Windows with a **minimal
  coreutils set**. `grep`, `sed`, `head`, `tail` and `wc` are *not* available.
  Use the Grep/View tools, or PowerShell (`Select-String`, `Get-Content`).
- MSVC error output is **localised to French**. Match on the error code
  (`error C2065`) rather than the message text.
- Driver installation needs elevation plus test signing plus a reboot. Agent
  sessions here are not elevated, so anything past "bind WinUSB" is a handoff
  to the user.

## The one non-obvious thing that unblocks everything

The control plane does **not** need a driver. Section 4.1's control transfers
are HID feature reports (`wValue 0x0300`) aimed at **interface 0** (`wIndex 0`),
which is a plain HID collection Windows already owns. `HidD_SetFeature` /
`HidD_GetFeature` produce identical traffic.

So chip id, connector type, EDID, flash reads, power and the entire modeset
sequence are all testable right now, with no INF, no signing and no reboot.
Only pixels need WinUSB. `msdisp --transport hid` is the default when WinUSB
is not bound, and it is where almost all iteration should happen.

## Architecture

```
src/common/     transport-agnostic protocol + frame pipeline (shared by both binaries)
  ms912x_proto.*    wire constants and packed structs. No logic.
  transport.h       Transport interface: 8-byte control in/out, bulk out
  hid_transport.*   control plane over hidusb. No data plane.
  winusb_transport.*control + bulk over WinUSB, for the standalone tool
  file_transport.*  loopback: writes would-be transfers to disk
  ms912x_device.*   all device logic. Owns the control lock.
  ms912x_convert.*  XRGB8888 -> UYVY, damage rect alignment, frame framing
src/tools/msdisp/ console tool
src/driver/       IddCx UMDF2 driver
  usb_backend.*     Transport implemented over the WDF USB target
  device.*          IddCx adapter/monitor, swapchain thread, frame sender
  driver.*          DriverEntry and the IddCx callbacks
```

Data flow, driver side:

```
compositor -> IddCx swapchain -> SwapChainProcessor thread
   acquire buffer, read dirty rects, union with previous frame's damage,
   align to even x, CopySubresourceRegion into a staging texture, Map,
   convert to UYVY straight into a transfer buffer
     -> FrameSender (2 buffers, worker thread, drops if both busy)
       -> Device::SendFrame -> Transport::BulkWrite -> endpoint 4
```

The `Transport` split is the most important structural decision: it is what
lets the control plane run over HID during bring-up and the frame pipeline be
validated against disk dumps. Keep new device logic in `Device`, not in a
transport.

## Rules that are easy to violate

- **Serialize the control plane.** `SET_REPORT` then `GET_REPORT` is one
  stateful sequence. Everything goes through `Device`, which holds
  `ctrl_mutex_`. A path that reaches the transport directly will return
  garbage under concurrency.
- **Never block the IddCx processing thread on USB.** Convert on that thread
  if you must, hand the transfer to `FrameSender`, and if both buffers are
  busy after ~10 ms, **drop the frame**. Queueing builds unbounded latency and
  Windows eventually declares the monitor hung.
- **Send the union of this frame's and last frame's damage.** The panel double
  buffers.
- **Even x, even width** on every damage rectangle. UYVY encodes pixel pairs.
- **Zero length bulk packet after every frame.** The vendor driver does this
  and the chip can hang waiting without it.
- **Do not enable output in the modeset.** `SetResolution` leaves video off;
  `SendFrame` enables it once a frame has actually landed, so the panel never
  shows leftover memory.
- **Probe the chip id, never infer it from the USB product id.** The test unit
  reports a USB 3 product id and contains a 912x die. The chip id selects the
  flash base address for custom timings.
- Source rows from a mapped D3D staging texture use `RowPitch`, which is not
  `width * 4`.

## Style

C++17, Google-ish style: two space indent, `CamelCase` functions and types,
`snake_case_` members, `kConstant`. Comments explain *why*, and the ones
present are load-bearing — most encode a protocol gotcha that cost real time.
Do not strip them. No em dashes in source.

## Reference material

The vendor's GPL Linux sources and the Windows installer are extracted under
`build/vendorsrc/` and `build/vendor/` by:

```
7z x -y -obuild/vendorsrc MS91xx_Linux_Drm_SourceCode_V3.0.3.13.zip
```

`build/vendorsrc/DRM_SourceCode_V3.0.3.12/usb_hal/` is the authoritative
reference: `usb_device_hid.h` names every opcode, `usb_device.c` has the
register and command plumbing, `usb_hal_thread.c` has the framing, colour
conversion and the zero length packet. It answers most questions that
`rhgndf/ms912x` leaves as "unknown".

`build/` is disposable and regenerated; nothing in it is source.
