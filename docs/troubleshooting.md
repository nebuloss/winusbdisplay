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
| The idle refresh paints a recycled surface | driver keeps its own desktop copy | still shimmers |
| GPU and CPU paths differ by one bit and are mixed by damage size | required exact agreement, which disabled the GPU path | still shimmers |
| The chip's `BYPASS_MANUAL_BLOCK` transfer mode avoids double buffering | selected mode 5 instead of 3 | chip rejects every transfer, panel dark |

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

One further observation worth recording: the panel drops its signal after
roughly one to two seconds without traffic, which is why the driver repaints
periodically at all. Shortening that interval does not remove the shimmer.

The tool for settling it is the one `AGENT_PROMPT.md` section 9 recommends and
which has not been used here, because neither USBPcap nor Wireshark is
installed on this machine: capture the vendor driver's USB traffic while text
redraws and diff it against ours. That shows directly whether the vendor sends
something we do not, most likely around `TRIGGER_FRAME`, which exists in the
vendor's own source but is commented out.

Each of the fixes above was reverted once it was shown not to help, so the
driver is not carrying speculative complexity. The GPU conversion path in
particular was removed entirely: it was measurably slower than the threaded
SIMD path for ordinary damage, only equal at full screen, and mixing two paths
that disagree by one bit is a correctness hazard for no real gain.

## Recovering a wedged chip

If a bulk transfer times out, the WinUSB pipe stays stalled and every later
write fails with the same error, leaving the panel dark permanently. The
driver now aborts and resets the pipe on any failed write, and reprograms the
chip after three consecutive failures. Before that fix the only recovery was
to replug the dongle.


## The machine blue screens while installing or removing

`IRQL_NOT_LESS_OR_EQUAL`, bug check 0xA, with no dump written.

**Cause: a display device node was removed while its monitor was live.**

This happened during development and is worth stating plainly because the
instinct on seeing it is to suspect this driver, and that instinct is wrong
in a way that wastes time. A user mode driver cannot raise the interrupt
level, cannot touch kernel memory, and cannot produce this bug check. The
faulting code is the kernel side of the display stack, which this project
does not contain.

What provoked it was an install script calling a removal on
`ROOT\DISPLAY\...` nodes, several in quick succession, while a monitor was
still attached to the desktop. Those nodes are not inert: while one exists
and is started, its monitor is part of the desktop topology and kernel
components hold state for it.

**The rule: disable, wait, then remove. Never in a tight loop.**

Disabling makes Windows take the monitor out of the desktop through the path
designed for exactly that, and lets this driver's stop path run to
completion. `install.ps1` and `purge.ps1` both do this now, with a pause
between each step. Without the removal tool available they leave the device
disabled instead, which is harmless and visible, rather than forcing it.

If it has already happened: nothing is damaged, and the leftover nodes can be
cleaned up by running `purge.ps1`, which now takes the safe path. Check what
is present with:

```
Get-PnpDevice -InstanceId 'ROOT\DISPLAY\*' | ForEach-Object {
  $_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_HardwareIds' }
```

Several entries with a hardware id of `root\usbdisplaydd` means duplicates,
which earlier versions of the installer could create; one identifying itself
as `root\usbhdmidd` is from before this project was renamed.

## Ruled out: sending each region once, alternating between the two images

Tempting, because sending everything twice costs exactly half the link, and
at 1080p60 that is the difference between needing 249 MB/s and needing 498
on a part that carries 237. The advertised refresh rate is unreachable while
every region goes out twice, so this looks like the one optimisation worth
having.

The vendor's own driver appears to do it: it keeps two accumulated damage
rectangles, merges every change into both, and each send drains one and
flips. The appeal is that nothing has to agree with the adapter about which
image is which, because both accumulators receive the same changes and are
merely drained at different times.

Implemented, including the part that is easy to miss: the record used to
decide whether a region's pixels actually changed has to be **per image**.
With one shared record the second image's turn looks like a no-op, the
region is skipped, and that image keeps its old content forever. That is
the two-pointers artifact, and it appeared immediately when the record was
shared.

With per-image records the artifact changed rather than went away: the
pointer became invisible and the top of the screen flickered. Which is the
real objection, and it is not an implementation detail. The adapter
alternates which image it displays every frame. If the two images are ever
different, and with one-at-a-time updates they differ for a frame every
time anything changes, the panel shows frame N and frame N-1 alternately at
60 Hz. That is flicker by construction, not a bug to be fixed.

So the double send is not redundancy that can be optimised away. It is what
keeps the two images identical, which is what stops the alternation being
visible. The bandwidth cost is the price of that, and 1080p60 with
full-screen damage is therefore out of reach on this hardware.

None of this is wasted: ordinary desktop damage is nowhere near full screen,
and the measured idle and typical-use figures are good. It is specifically
full-screen animation that cannot reach 60.

What was not tried, and is the only remaining idea: `trigger_frame`, the
vendor command that asks the chip to show a chosen image after a delay. It
is present in the vendor's source and commented out there. If it controls
which image is scanned out, it would allow updating one while the other is
displayed, which is the thing that would make single-send correct.

## Telling a dark panel from a working one without looking at it

The single most expensive thing about this hardware is that "the transfers
succeeded" and "there is a picture" are unrelated statements, so the only
way to tell them apart was to ask somebody what the screen looked like.
That is slow, and worse, it is unreliable: once the adapter is in the dark
state it stays there, so every later experiment in the same session reports
the fault of the one before it. Several correct changes were reverted that
way.

**`usbdisplayctl peek 0xFB1A 4`.** The third byte reads `44` whenever a
picture is on the glass and `01` when it is dark. Verified three times in a
row against a frame known to display, and across many dark states. The
control plane is HID, so this works while the driver holds the pixel pipe.

That is a detector, not an explanation. It is enough to iterate without a
person in the loop, which is what matters.

## What differs between a freshly plugged adapter and a dark one

Dumped `0x0000`, `0xD000` and `0xF000`-`0xFFFF` in both states and compared.
Fifty seven lines differ, most of them noise, but two stand out.

**`0xF900` through `0xFAFF` reads `9A` everywhere when healthy and `9E`
everywhere when dark.** Five hundred identical bytes is not memory, it is
one register that does not decode its low address bits, mirrored across the
range. So a single register differs by exactly one bit, `0x04`, and that bit
is set when the adapter has stopped displaying.

That makes it the best candidate for both a cause and a cure. If the bit can
be cleared by writing `9A` back, the adapter may recover without being
unplugged, which would remove the worst obstacle to working on this thing.
**Untested**: the adapter was healthy again by the time this was written
down, and testing it needs a dark one.

Also differing, and worth trying in the same experiment:

| register | healthy | dark |
|---|---|---|
| `0xF050+4` | `00` | `01` |
| `0xF3D0` | `00` | `28` |
| `0xF530+8` | `03 00 03` | `00 00 04` |
| `0xF750+14` | `00` | `01` |
| `0xFB1B..D` | `14 44 12` | `6D 01 00` |

## Many small transfers in quick succession are what breaks it

Measured with the tool, so no driver is involved:

| what | result |
|---|---|
| one full frame | works, 265 MB/s |
| the same picture as 34 bands | works |
| as 68 bands | pipe times out |
| as 135 bands | pipe times out |

Per transfer time climbs towards one vsync as the count rises: 10.4 ms at
four transfers, 15.8 ms at thirty four. The limit is somewhere between
thirty four and sixty eight back to back, and a pipe timeout leaves the
adapter in the dark state described above.

This is the practical constraint the driver has to respect, and it is the
reverse of the intuition that smaller updates are cheaper. On this part they
are not cheaper and they are considerably more dangerous.

## Correction: a pipe timeout does not cause the dark state

Worth writing down because the opposite was believed for most of a day and
several correct changes were reverted on the strength of it.

Overrunning the pipe deliberately, by sending a picture as 135 small bands
back to back, produces the timeout reliably. It does **not** produce the
dark state. Checked with `usbdisplayctl health` immediately afterwards: the
chip still reports a picture, and the next full frame is accepted at
237 MB/s.

So the timeout and the dark panel are two separate faults that happened to
appear together, and "the pipe timed out" is not evidence about the
display. Use the detector rather than inferring.

## How to work on this without wasting a day

The honest lesson from the session that found all of the above.

1. **Check `usbdisplayctl health` before and after every experiment.** It
   reads a register that is 0x44 when a picture is on the glass and 0x01
   when it is not, and it agrees with what a person sees. The control
   plane is a separate USB interface, so it works while the driver holds
   the pixel pipe.
2. **If it says dark, replug before measuring anything else.** The state
   persists, so every later experiment in the same session reports the
   fault of the one before it. This is how two correct changes came to be
   reverted and one wrong one kept.
3. **The driver's own counters do not answer this question.** `sent`,
   `dropped` and `failed` all look perfect while the panel is dark, because
   the transfers genuinely do succeed.
4. **A silent log is not a stopped driver.** The pipeline reports when it
   is idle as well as when it is drawing, but it did not always, and a log
   that stopped at "running" was read as a crash when it was a still
   desktop.
