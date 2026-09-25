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

---

# Indirect display driver (IddCx)

## The driver and the msdisp tool fight over the USB pipe

The WinUSB pixel pipe is exclusive. If the driver is running, `msdisp` fails
to open it with `0x00000005` (access denied), and vice versa. Disable the
driver's device node while using the tool:

```
Disable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:$false
```

## Reading what the driver actually did

WDF collapses most initialisation failures into a single generic status, so
the driver keeps its own log:

```
C:\Windows\Temp\ms912xidd.log
```

It records every step of `DriverEntry`, `PrepareHardware`, `D0Entry`, adapter
init and monitor creation with the exact `NTSTATUS` of each IddCx call. Start
here; the event log will usually only tell you "problem code 10".

For the PnP-level view, the UMDF operational log is disabled by default:

```
wevtutil sl Microsoft-Windows-DriverFrameworks-UserMode/Operational /e:true
```

## Device fails to start, problem code 10

The event log reports only `STATUS_DEVICE_POWER_FAILURE` (0xC000009E) on the
start IRP, which is WDF's generic mapping for `EvtDeviceD0Entry` failing. The
driver log has the real status. Causes found so far:

| Symptom in the log | Cause |
|---|---|
| `IddCxAdapterInitAsync -> 0xC000000D` | `pHardwareVersion` / `pFirmwareVersion` left null in the adapter caps. Both are mandatory. |
| `IddCxMonitorCreate -> 0xC000000D` | `MonitorContainerId` left as an all-zero GUID. Generate one with `CoCreateGuid`. |
| `PrepareHardware -> 0xC0000182` | The WDF USB target could not be created. See below. |

## Why the driver is root-enumerated rather than bound to the USB interface

An earlier revision bound the driver directly to the dongle's display
interface and used the WDF USB target with `UmdfDispatcher = WinUsb`. That
never starts: `WdfUsbTargetDeviceCreateWithParameters` fails, even with the
WinUSB service installed and `LowerFilters = WinUsb` correctly applied.

IddCx requires the `IndirectKmd` upper filter, and that topology appears to be
incompatible with the WinUsb dispatcher. Neither in-box indirect display
driver (`rdpidd.inf`, `miradisp.inf`) declares a `UmdfDispatcher` at all.

So the driver is a root-enumerated software device and reaches the dongle
through user-mode handles instead, which works because UMDF hosts are user
mode processes. This also means both INF packages must be installed.

## Known unresolved issue: IddCxMonitorArrival

Current state: the device starts cleanly (problem 0), both transports open
from `WUDFHost` running as LOCAL SERVICE, EDID reads correctly, the mode list
builds, `IddCxAdapterInitAsync` and `IddCxMonitorCreate` both succeed, and
`EvtIddCxParseMonitorDescription` is called and answers both passes.

`IddCxMonitorArrival` then returns `STATUS_DEVICE_NOT_READY` (0xC00000A3), so
no monitor appears in Settings.

Ruled out:

- timing: deferring creation off the `AdapterInitFinished` callback does not help
- retrying: arrival may only be called once, a second call returns `STATUS_INVALID_PARAMETER`
- `IndirectKmd` not loading: the filter is attached and the service is running
- connector type: `INDIRECT_WIRED` behaves the same as `HDMI`
- `MaxDisplayPipelineRate` being too low to admit any mode

Still to try:

- returning a mode list derived from the EDID's own detailed timings rather
  than the chip's 26-entry table, in case the OS cross-checks them
- running the host with a different `UmdfImpersonationLevel`, or as a
  different account
- comparing against a stock build of Microsoft's `IndirectDisplay` sample on
  this same machine to isolate whether the problem is our code or the
  environment

## "Device offline due to a user-mode driver hang" on disable

Event 10111 appears in the system log every time the display device is
disabled or restarted while a monitor is attached to the desktop. It is worth
knowing that this is not our driver hanging.

Measured with timers in `ReleaseHardware`, the whole teardown, including
stopping the swapchain thread and the USB sender, completes in **0 ms**, and
90 seconds of steady operation produces no events at all. The report comes
from the OS removing a live display from the desktop topology on a forced
disable, which is a development operation rather than something normal use
hits.

Teardown does cancel any USB transfer in flight rather than waiting for it, via
`WinUsb_AbortPipe`. Without that, a full frame occupies the bus for over a
hundred milliseconds and a stop request would genuinely block for that long.

## Text shimmer: unresolved

Small text, particularly in File Explorer, shimmers on the USB panel. The
following were each implemented, measured and ruled out, so anyone picking
this up can skip them:

| Hypothesis | Test | Result |
|---|---|---|
| GPU and CPU conversion paths disagree | disabled the GPU path entirely | still shimmers |
| SIMD and scalar disagree by rounding | unified all three paths to identical 15 bit arithmetic, verified byte-identical | still shimmers |
| One transfer reaches only one of the chip's two frame buffers | sent every update twice | still shimmers |
| The two buffers sit one update apart | restored the reference driver's union of current and previous damage | still shimmers |
| The idle refresh paints a recycled surface | driver now keeps its own desktop copy | still shimmers |

What was learned along the way, and is worth knowing:

- `IddCxSwapChainGetDirtyRects` sometimes reports a single rectangle covering
  the whole screen even when little has changed. Those updates cost 125 ms on
  the wire, and the chip appears to display progressively as data arrives
  rather than swapping a completed frame, so a full screen transfer is visible
  as a sweep. This is the most likely remaining explanation and it is not
  something the driver can avoid by tracking damage better.
- The vendor protocol has a `TRIGGER_FRAME` sub-operation, `0x00`, taking a
  buffer index and a delay. It is present in the vendor's own Linux driver but
  commented out. If the chip can be told to hold display until a transfer
  completes, that command is where to look.

The tool for settling it is the one `AGENT_PROMPT.md` section 9 recommends and
which has not been used here: capture the vendor driver's USB traffic while
text redraws, and diff it against ours. That will show directly whether the
vendor sends something we do not, most likely around `TRIGGER_FRAME`.

## Recovering a wedged chip

If a bulk transfer times out, the WinUSB pipe stays stalled and every later
write fails with the same error, leaving the panel dark permanently. The
driver now aborts and resets the pipe on any failed write, and reprograms the
chip after three consecutive failures. Before that fix the only recovery was
to replug the dongle.
