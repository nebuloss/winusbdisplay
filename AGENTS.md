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

## Version control

This is a git repository. **Commit each change separately, as soon as it is
verified.** Several regressions during development were only caught by the
user noticing the display had broken, and with no history there was no way to
bisect or revert cleanly; everything had to be reasoned back by hand.

Specifically:

- One commit per logical change, with the measurement or observation that
  justified it in the message.
- Commit before starting an experiment, so reverting is one command.
- Experiments that did not work are worth committing too, then reverted, so
  the reasoning survives. `docs/troubleshooting.md` lists several ruled-out
  hypotheses that would otherwise be retried.
- `build/` is ignored, as are the vendor archives (`*.zip`, `*.exe`), which
  are large and separately redistributable. Nothing under `build/` is source.
- Check `git status` before assuming the working tree matches HEAD. The frame
  pipeline in `src/driver/device.*` has been rewritten more than once; an
  uncommitted variant can disagree with both the surrounding comments and
  this file.
- The `reconstructed` branch is the project history before git existed,
  replayed out of the Crush session transcript by
  `build/xcript/rebuild_history.py`. It has one commit per change, backdated,
  interleaved with the user feedback that prompted each one
  (`git log --grep feedback reconstructed`). Use it to find which change
  introduced a symptom:

  ```
  git log --oneline -S<symbol> reconstructed -- src/driver/device.cpp
  git show <hash>:src/driver/device.cpp
  ```

  It is a reconstruction, not a recording: 42 of the 51 tracked files match
  the real tree byte for byte and the rest drift by a few lines, so trust it
  for sequence and intent, not for exact bytes.

## Commands

```
scripts\build-tool.bat                    # msdisp console tool -> build\msdisp.exe
scripts\build-driver.bat [Config] [Plat]  # IddCx driver -> build\driver\<plat>\<config>\
scripts\build-tray.bat                    # msbright tray app -> build\msbright.exe

scripts\elev.ps1 -Start                   # elevated worker, ONE UAC prompt per session
scripts\elev.ps1 -Script <abs path>       # run a script elevated, no prompt
scripts\elev.ps1 -Command '<powershell>'  # run a command elevated, no prompt
scripts\elev.ps1 -Stop

scripts\install-all.ps1                   # both packages + root device node
scripts\purge.ps1                         # remove every package and the device node
scripts\diagnose-driver.ps1               # restart device, decode failure status
scripts\visual-check.ps1                  # colour sweep on the panel
scripts\extend-desktop.ps1                # make an arrived monitor an active screen
scripts\stress-test.ps1                   # unpaced update rate, vs the chip's 60/s
scripts\animate-test.ps1                  # paced animation (measures itself, not us)
scripts\ddc-probe.ps1                     # what Twinkle Tray sees; run unelevated
scripts\usb-topology.ps1                  # negotiated USB speed and location
```

`build-driver.bat` defaults to `Release x64`, passes `/p:SignMode=Off`, and
needs the **WDK extension** for Visual Studio, not just the SDK. Driver
sources are listed explicitly in `src/driver/ms912xidd.vcxproj`; a new file
under `src/common` must be added there *and* to `scripts/build-tool.bat`.

Administrative steps go through `elev.ps1`, which starts one hidden elevated
worker and then services commands over a spool directory. Without it every
`pnputil` call is a separate UAC prompt. Read the security note at the top of
`elevated-worker.ps1` before leaving it running.

`build-tool.bat` and `build-tray.bat` compile with `/W4 /WX`; both must stay
warning-clean. The driver project is `/W4` without `/WX` because the WDK
headers are not clean at that level.

## Verification loop

There is no test suite. The tool is the test harness:

```
build\msdisp.exe list                                   # interfaces Windows sees
build\msdisp.exe info                                   # chip, connector, HPD
build\msdisp.exe edid --out monitor.edid
build\msdisp.exe selftest                               # SIMD conversion vs scalar
build\msdisp.exe --transport file testpattern --bars    # offline pipeline check
build\msdisp.exe benchsizes                             # transfer cost vs rect size
build\msdisp.exe bench [--depth N] [--chunk KB]         # sustained throughput
build\msdisp.exe flickertest                            # chip partial-update behaviour
build\msdisp.exe image --mode 1920x1080@60 --bmp x.bmp
```

Global flags come **before** the command: `--transport auto|hid|winusb|
composite|file` and `--dump-dir DIR`. `auto` opens HID for control and WinUSB
for data; `hid` alone is read-only and needs no driver at all. Run
`build\msdisp.exe` with no arguments for the authoritative command list.

`selftest` is the guard for any change to `ms912x_convert.cpp`: it proves the
SIMD path is bit exact against the scalar reference.

## Environment realities

- Toolchain present: VS 2022 **Build Tools** (no IDE), MSVC 14.44, WDK
  10.0.26100. ARM64 driver builds fail — the ARM64 toolset component is not
  installed.
- Driver installation needs **no reboot and no test signing**. Neither package
  loads a third-party kernel binary, so driver signature enforcement never
  engages; PnP only needs the catalog to chain to a cert in Trusted Publisher,
  which the install scripts create. It works with Secure Boot on. It does need
  **elevation**, so anything past "bind WinUSB" goes through `elev.ps1` or is
  a handoff to the user.
- `Inf2Cat.exe` ships **x86 only**, and its OS names are case sensitive with no
  plain `10_ARM64` (use `10_RS3_ARM64`).
- `signtool` needs `/sm` to see a certificate in the machine store.
- `pnputil` silently keeps the old binary if the INF's `DriverVer` has not
  changed. Always install the **build-stamped** INF from `build/driver/...`,
  not the source one in `inf/`.
- The shell here is a Bash-compatible interpreter on Windows with a **minimal
  coreutils set**. `grep`, `sed`, `head`, `tail`, `wc` and `which` are *not*
  available. Use the Grep/View tools, or PowerShell (`Select-String`,
  `Get-Content`).
- When shelling out to PowerShell, the wrapper expands `$_` and `$name` before
  PowerShell sees them, so any pipeline using `$_` fails with a wall of
  errors. Prefer the Grep/View tools, or put the snippet in a `.ps1` file.
- MSVC error output is **localised to French**. Match on the error code
  (`error C2065`) rather than the message text.

## The one non-obvious thing that unblocks everything

The control plane does **not** need a driver. Section 4.1's control transfers
are HID feature reports (`wValue 0x0300`) aimed at **interface 0** (`wIndex 0`),
which is a plain HID collection Windows already owns. `HidD_SetFeature` /
`HidD_GetFeature` produce identical traffic.

So chip id, connector type, EDID, flash reads, power and the entire modeset
sequence are all testable right now, with no INF, no signing and no reboot.
Only pixels need WinUSB. `msdisp --transport hid` is where almost all
iteration should happen.

## Architecture

```
src/common/     transport-agnostic protocol + frame pipeline (shared by both binaries)
  ms912x_proto.*    wire constants and packed structs. No logic.
  transport.h       Transport interface: 8-byte control in/out, bulk out,
                    CancelTransfers, plus container-id matching for multi-dongle
  hid_transport.*   control plane over hidusb. No data plane.
  winusb_transport.*control + bulk over WinUSB, for the standalone tool
  composite_transport.* control from one transport, data from another
  file_transport.*  loopback: writes would-be transfers to disk
  ms912x_device.*   all device logic. Owns the control lock.
  ms912x_convert.*  XRGB8888 -> UYVY, damage rect alignment, frame framing
src/tools/msdisp/   console tool
src/tools/msbright/ tray brightness control (writes the registry value the
                    driver polls; pure Win32 plus GDI+, no dependencies)
src/driver/       IddCx UMDF2 driver (root-enumerated software device)
  driver.*          DriverEntry and the IddCx callbacks
  device.*          IndirectDevice (adapter, monitor, mode list),
                    SwapChainProcessor (one thread per swapchain),
                    FrameSender (2 buffers + worker thread)
  ddcci.*           DDC/CI slave and the registry brightness fallback
  usb_backend.*     WDF USB target, data plane only; control deliberately
                    always fails. Not on the live path, kept because its
                    header documents why WDF cannot carry the control plane
  log.*             file logger, see below
```

**The two planes live on different USB interfaces.** Control transfers are
class requests aimed at interface 0 (the HID collection); the bulk pixel pipe
is on MI_03. Windows gives those interfaces to different drivers and WinUSB
will not proxy a control request to an interface it does not own. So both the
tool and the driver use a `CompositeTransport`: `HidTransport` for control,
`WinUsbTransport` for data. That pipe is exclusive, so the tool and the driver
cannot run at the same time. To use the tool, disable the root device first:

```
scripts\elev.ps1 -Command "Disable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:$false"
```

The driver is **root-enumerated**, not bound to the USB interface, because
IddCx's `IndirectKmd` upper filter is incompatible with
`UmdfDispatcher = WinUsb`. It reaches the dongle through user-mode handles,
which is fine because UMDF hosts are user-mode processes. Both INF packages
must be installed. See `docs/troubleshooting.md`.

Data flow, driver side:

```
compositor -> IddCx swapchain -> SwapChainProcessor thread
   acquire buffer, read dirty rects and move regions, merge into the damage
   owed per chip buffer, align to even x, CopySubresourceRegion into a staging
   texture, Map, convert to UYVY (brightness and contrast applied here)
   straight into a transfer buffer
     -> FrameSender (2 buffers, worker thread, drops after ~10 ms if none free)
       -> Device::SendFrame -> Transport::BulkWrite -> endpoint 4
```

If nothing has been sent for ~2.5 s the processor repaints from the last
acquired surface, which is what stops the panel deciding there is no signal.

The `Transport` split is the most important structural decision: it is what
lets the control plane run over HID during bring-up and the frame pipeline be
validated against disk dumps. Keep new device logic in `Device`, not in a
transport.

## Runtime configuration

Everything tunable lives in `HKLM\SOFTWARE\winusbdisplay`, read by
`ReadPolicyDword` in `device.cpp` and by `DdcCiSlave::RefreshFromRegistry`:

| Value | Default | Effect |
|---|---|---|
| `Brightness` | 100 | 0..100, applied during conversion |
| `Contrast` | 50 | 0..100, applied during conversion |
| `IdleRefresh` | 1 | periodic whole-screen repaint |
| `SyncDivider` | 0 | `vSyncFreqDivider` above 1; breaks the topology, off |
| `ReportAsInternal` | 0 | report an INTERNAL connector (see brightness below) |

HKLM, not HKCU, because the driver runs as LOCAL SERVICE and cannot read a
user hive. `install-all.ps1` widens the ACL on that one key so `msbright` can
write it without elevation.

## Debugging the driver

WDF collapses initialisation failures into one generic status, so the driver
writes its own log to `C:\Windows\Temp\ms912xidd.log` recording the exact
`NTSTATUS` of every IddCx call. Always read that first; the event log will
only say "problem code 10". `Log()` is in `src/driver/log.h`.

Returning a **distinct** `NTSTATUS` per failure point is also worth keeping:
`PrepareHardware`'s return value is one of the few things that surfaces
verbatim in the UMDF event log. `WdfUsbBackend::Create` documents the
convention even though it is not on the live path.

## Settled questions, do not re-litigate

Each of these cost real investigation; the evidence is in
`docs/protocol-notes.md`.

- **The chip is vsync locked at 60 Hz.** Transfer cost is quantised into
  16.67 ms periods: up to ~520 KB costs one period, the next byte costs a
  whole extra one. So a small update and a 490 KB update cost the same, and
  normal desktop damage runs at the full 60 updates/s. Only full screen
  repaints are slow (8 periods, 7.5 fps). Re-measure with
  `msdisp benchsizes`.
- **Every transfer must carry this frame's damage and the previous frame's.**
  The chip alternates between two frame buffers on every transfer, so a region
  sent once lands in one and leaves the other stale, and the two alternate
  visibly on screen. `pending_damage_[0]` accumulates what has changed since
  the last transfer, `pending_damage_[1]` is what the last transfer covered,
  and each transfer sends the union of the two.

  Tracking which chip buffer is next and writing them one at a time was tried
  twice and failed both times. It is only correct while the driver's idea of
  the next buffer stays in step with the chip's, which nothing enforces; once
  they drift, every update goes to the wrong buffer. The union needs no such
  agreement. A refresh meant to resynchronise must still send the whole
  screen. See `git log --grep flicker` and the `reconstructed` branch.
- **~30 MB/s is a hardware ceiling.** The chip is USB 2 silicon (MS912C, no
  BOS descriptor) and saturates at 29.6 MB/s. Pipelined overlapped transfers
  at depth 2, 4 and 8 all measure within 1% of synchronous. Do not go looking
  for host-side throughput wins; there are none. `msdisp bench` re-measures.
- **There is no cheaper pixel format.** The chip offers RGB565, RGB888,
  YUV422 and YUV444. We already send the cheapest at 16 bpp.
- **`vSyncFreqDivider` above 1 breaks the topology**, even with correct
  timings. Windows returns `ERROR_GEN_FAILURE`; behind the `SyncDivider`
  switch, default off. Pacing is done by dropping frames instead.
- **Mode timings must be self consistent**: `pixelRate` equals
  `totalSize.cx * totalSize.cy * vSyncFreq` and `hSyncFreq` equals
  `pixelRate / totalSize.cx`, with `totalSize` including blanking. Setting
  `totalSize` equal to `activeSize` breaks those identities. Monitor modes
  additionally require `vSyncFreqDivider == 0`; only target modes may set it.
- **There is one conversion path, threaded SIMD on the CPU.** A GPU compute
  shader was implemented and removed: slower than the CPU path for ordinary
  damage, equal at full screen, and one least significant bit different, which
  makes mixing the two a correctness hazard. Do not reintroduce a second path
  without making it bit exact first. Check with `msdisp selftest`.
- **Windows never calls the IddCx I2C callbacks**, so DDC/CI cannot work for
  an indirect display. Microsoft documents this, and it was measured: the
  callbacks never fire and `dxva2` returns `ERROR_NOT_SUPPORTED`. The
  `DdcCiSlave` is kept complete and correct in case that changes; its checksum
  rules are written out at the top of `ddcci.h`.
- **Twinkle Tray cannot reach this monitor.** Its two paths are DDC/CI (above)
  and WMI, and WMI needs a `WmiMonitorBrightness` instance that only
  `monitor.sys` creates after finding a brightness interface on the kernel
  miniport. `IndirectKmd` has none. Reporting an INTERNAL connector gets the
  monitor classified into the WMI path but there is still nothing behind it;
  the switch is kept as `ReportAsInternal` for anyone who adds a kernel
  filter later. Brightness is delivered by `msbright` instead.

## Known unresolved

Small text shimmers on the panel. Five plausible causes were implemented and
measured away; `docs/troubleshooting.md` lists them so they are not retried.
The remaining suspect is that the chip displays progressively during a
transfer, which would make the occasional whole-screen update the OS asks for
visible as a sweep. Settling it needs a USB capture of the vendor driver, per
`AGENT_PROMPT.md` section 9.

## Rules that are easy to violate

- **Serialize the control plane.** `SET_REPORT` then `GET_REPORT` is one
  stateful sequence. Everything goes through `Device`, which holds
  `ctrl_mutex_`. A path that reaches the transport directly will return
  garbage under concurrency.
- **Never block the IddCx processing thread on USB.** Convert on that thread
  if you must, hand the transfer to `FrameSender`, and if no buffer is free
  after ~10 ms, **drop the frame** — the damage stays pending, so a later
  frame still sends it. Queueing builds unbounded latency and Windows
  eventually declares the monitor hung.
- **Cancel in-flight transfers on stop or surprise removal.** A full frame
  occupies the bus for over 100 ms and PnP will not wait; that is what
  `Transport::CancelTransfers` exists for, and skipping it produces "device
  offline due to a user-mode driver hang".
- **Even x, even width** on every damage rectangle. UYVY encodes pixel pairs.
- **Zero length bulk packet after every frame.** The vendor driver does this
  and the chip can hang waiting without it.
- **Do not enable output in the modeset.** `SetResolution` leaves video off;
  `SendFrame` enables it once a frame has actually landed, so the panel never
  shows leftover memory.
- **Probe the chip id, never infer it from the USB product id.** The test unit
  reports a USB 3 product id and contains a 912x die. The chip id selects the
  flash base address for custom timings.
- **Move regions are a tradeoff, not a free win.** Damage is reduced to a
  single bounding rectangle, so merging a move's source with its destination
  can swell a 75 KB update into 3.2 MB while a window is dragged; sending only
  the destination relies on the OS reporting the uncovered area separately.
  Whichever choice is in the tree, the comment beside it explains why. Change
  it only with a measurement.
- **IddCx object attributes must carry a context type.** `IddCxMonitorCreate`
  with null attributes succeeds and then `IddCxMonitorArrival` fails with the
  thoroughly misleading `STATUS_DEVICE_NOT_READY`.
- Source rows from a mapped D3D staging texture use `RowPitch`, which is not
  `width * 4`.

## Style

C++17, Google-ish style: two space indent, `CamelCase` functions and types,
`snake_case_` members, `kConstant`. Every file opens with an
`SPDX-License-Identifier: GPL-2.0-only` line, and the non-obvious ones carry a
header comment explaining their reason to exist. Comments explain *why*, and
the ones present are load-bearing — most encode a protocol gotcha that cost
real time. Do not strip them. No em dashes in source.

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
