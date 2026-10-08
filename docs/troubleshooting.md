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

## Ruled out: sending the whole screen on the USB 3 parts

The cost model says a full frame and a small region both cost one slot on
these parts, so sending the whole screen every frame should be free and
would avoid damage tracking entirely. It is not free: the panel goes dark
within seconds.

This was tried twice. The first attempt was abandoned without a verdict,
because the adapter was already dark when it ran and every observation was
therefore worthless. The second was run properly, with `usbdisplayctl
health` before and after, and the answer was unambiguous: `DARK (10 40 01
00)` on all five samples, where the same driver with damage tracking left
in reports `showing a picture (10 14 44 12)` throughout. Reverting restored
it immediately.

So "a transfer costs one slot regardless of size" is true about timing and
false about everything else. Something about a stream of full frames is not
acceptable to this adapter even when each one individually is. The
measurement that says a full frame takes 16 ms is still correct; it simply
does not license sending one every frame.

Worth knowing before anyone reads the cost model and reaches the same
conclusion a third time.

## Solved: the dark panel, and why the console tool never suffered from it

The adapter stops displaying while still accepting everything sent to it.
Every transfer succeeds, the output reports itself enabled, the driver's
counters are perfect, and the screen is black. For most of a day the only
known cure was to unplug it.

**The console tool has been curing it by accident the whole time.**
`SendImage` calls `PowerOn` and `SetMode` before every single frame. The
driver programmed the mode once at startup and never again. That is the
entire difference: a dark adapter is revived by reprogramming it, so every
tool invocation silently fixed whatever the previous experiment had broken.

This is also why so many measurements in that session were wrong. Running
the tool to check whether the panel was dark *repaired* it, so the tool
always appeared to work and the driver always appeared broken, whatever
either of them was actually doing.

The driver now asks `DisplayingPicture` every three seconds and calls
`Revive` when the answer is no. Verified by darkening the adapter
deliberately and starting the driver against it: noticed in three seconds,
picture back within twelve, no replug.

Two things worth keeping from this:

- **A register read is the only way to ask.** Nothing on the frame path can
  tell the difference, because from its point of view nothing is wrong.
- **An experiment that uses the tool to observe has already changed what it
  is observing.** Use `usbdisplayctl health`, which only reads.

## Correction: 0xF900 is not a display indicator

Recorded earlier as the one clean difference between a working adapter and
a dark one, 0x9A against 0x9E, and suggested as a possible cure. It is
neither.

Watched across a recovery: the panel went from dark to showing a picture
while 0xF900 stayed at 0x9E throughout. Whatever that bit tracks, it is not
whether anything is on the glass, and writing it back does nothing. The
register that does answer the question is the one `usbdisplayctl health`
reads.

## The live register is about transmission, not about the glass

An important limit on the detector, found by trusting it too far.

With the driver stopped and nothing sent to the adapter at all, an MS9132
appeared to hold a test pattern for two minutes, and `usbdisplayctl health`
agreed throughout. On the strength of that the keepalive repaint was
removed for the USB 3 parts, since it exists only to stop the panel
blanking during silence and the panel evidently was not blanking.

The screen went black within seconds, while the register still reported a
picture.

So `kRegDisplayLive` describes what the chip is **transmitting**, not what
is **on the panel**, and the two come apart precisely in the case that
matters here. The detector is still the right tool for spotting the dark
state, because that is what it was validated against, and it is not
evidence that silence is safe.

**Both families need the keepalive.** The cost, roughly 15 MB/s on the
USB 3 parts to show a picture that is not changing, is the price of a panel
that stays lit.

The wider lesson, and the second time this session has taught it: a
register that correlates with a symptom is not a model of the hardware.
Check a change against the thing you actually care about, which here means
a person looking at the screen.

## Ruled out: TRIGGER_FRAME as a cure for the shimmer

The last untried explanation, and it is now tried.

The notes above record `TRIGGER_FRAME`, sub-operation `0x00`, as the place
to look: it takes an image index and a delay, it is present in the vendor's
own Linux driver, and it is commented out there. If it chose which of the
chip's two images is scanned out, a frame could be written to the hidden
one and swapped in complete, every update would be atomic, and a
progressive sweep could not happen.

**The chip accepts it and ignores it.** Forty commands out of forty
returned success, and the adapter went on displaying, so it is not
rejected the way `BYPASS_MANUAL_BLOCK` was.

The test that distinguishes accepting from obeying: fill one image red and
the other green, then ask for each in turn, three seconds apart, with no
pixels sent in between. A command that selects the displayed image makes
the panel alternate on its own.

It never showed red. The panel held green, which is simply the last frame
written, and went black in between, which is the signal dropping for want
of traffic. So the command does something, or nothing, but it does not
select what is on screen.

`usbdisplayctl trigger --select` reruns this, and is worth keeping for
anyone who wants to check the same thing on a different part.

That exhausts the hypotheses that can be tested from this side. What
remains is what `AGENT_PROMPT.md` recommended at the start: capture the
vendor driver's USB traffic while small text redraws, and diff it against
ours. Everything reachable by reasoning about the protocol has now been
reached.

## Withdrawn: reprogramming the adapter automatically when it looks dark

Added, shipped in v0.2.0, and withdrawn after a reboot. Worth recording in
full, because the feature is tempting and the reasoning behind it was
sound as far as it went.

The fault it addressed is real. The adapter can stop displaying while
accepting everything sent to it, and reprogramming genuinely cures it: a
dark adapter was revived by a single frame from the console tool, which
differs from the driver only in setting the mode before every frame.

What is not reliable is the trigger. After a cold boot the driver blinked
continuously, and the log shows why:

```
09:58:41 .. 09:59:11   failed=0 throughout, thirty three seconds clean
09:59:14   the panel has stopped displaying, reprogramming
09:59:19   transfer of 4147216 bytes failed
09:59:24   transfer of 4147216 bytes failed
09:59:29   transfer of 496 bytes failed
```

The adapter was healthy. The register claimed otherwise, the driver
reprogrammed a working chip, and because reprogramming drops the signal
and interrupts whatever is in flight, every transfer after it failed. The
cure was causing the disease, and each attempt was a visible blink.

An earlier version was worse still: the check fired twenty one
milliseconds after the pipeline started, before any frame could have
reached the glass, and then every three seconds for ever.

**That register has now misled four times.** It reported a picture while
the panel was black, which is how the keepalive came to be removed; it
reads zero on the USB 2 parts whatever is happening; it reported darkness
on an adapter running cleanly; and it reports darkness now, while the
panel shows a steady picture.

So the driver logs the state and does nothing. A dark panel that needs one
replug is a better outcome than a working panel that blinks whenever a
status register is read at an unlucky moment.

The real defence against the dark state is elsewhere and does not depend
on guessing: the pixel pipe is reset when opened and when a transfer is
cancelled, so an interrupted frame cannot leave the chip waiting for the
rest of a block. That addresses the cause. `usbdisplayctl health` remains
for asking the question by hand.

**The wider lesson, for the third time in this project.** A register that
correlates with a symptom is not a model of the hardware, and the more
convenient it is the more carefully it has to be checked. Each time this
one was trusted a little further it held, until it did not.

## The dark panel, properly understood

Three sessions of confusion and several wrong fixes went into this, so
here is what the measurements actually support.

**The fault.** Under sustained transfer the pixel pipe occasionally times
out. After that the adapter accepts every transfer, reports its output
unmuted and the panel attached, and puts out no signal at all. It is
intermittent: the same test can fail once and then run twenty seconds at
59 frames a second.

**The cure.** Power on, set the mode, then send a **full frame**. That is
what `usbdisplayctl testpattern` does, which is why the tool never
appeared to suffer from this and why using the tool to check the panel
repaired it first. Enabling the video output alone does nothing, measured.

**The thing that is easy to get wrong.** After a mode set the chip needs a
full frame before it will display anything. Send a partial band instead and
the pipe times out and the panel stays dark:

| first transfer after a mode set | result |
|---|---|
| one full frame | works |
| 128 row bands | pipe timeout, dark |

The driver gets this right because the repaint after reprogramming is
`MarkAll`, which makes the next update the whole screen. Anything that
changes that ordering will reintroduce this.

**The keepalive must not send whole screens.** Sizing it from the cost
model gives the whole screen on the USB 3 parts, because there a transfer
costs one slot whatever it carries. That is true about timing and false
about everything else: a stream of full frames is the pattern the adapter
will not tolerate. Driving the keepalive from the cost model therefore
built the known failure into the one path that runs when nothing else is
happening, which is why a still desktop went dark while a moving mouse
kept it alive. It is capped at 128 rows, which is what both families used
before anything was measured.

**Revival has to be rationed, not merely correct.** Reprogramming drops
the signal, so an attempt that does not take is itself a visible blink. At
three second intervals, which shipped in v0.2.0, a panel that kept falling
dark flickered permanently. Removing revival altogether, which shipped in
v0.2.1, left it dark instead. Neither is acceptable. What works: two
consecutive dark readings, nothing in flight, at most one attempt every
thirty seconds, three attempts in total, then stop and say so.

**What the register is worth.** It has been accused of lying four times in
these notes and it was telling the truth every time. Each apparent lie was
a measurement taken from an unknown starting state, or taken with the tool,
which repairs the thing it is measuring. Given a known baseline it has
agreed with the panel in every test. The lesson is about the measurements,
not the register.

## No display after a cold boot, working after a reinstall

**Settled, with a measurement. This is a Windows driver load failure, not
anything the driver does, and the driver's own log cannot show it because
the driver never runs.**

The symptom is the one a user actually reports: power the laptop on in the
morning, no second monitor. Reinstalling or re-enumerating the device fixes
it until the next boot.

Look in the Windows event log, not in `usbdisplaydd.log`:

```powershell
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 219 } |
    Where-Object { $_.Message -match 'ROOT\\DISPLAY' } |
    Select-Object TimeCreated, Message
```

What comes back, one to two seconds after every boot:

```
Driver \Driver\WUDFRd failed to load for the device ROOT\DISPLAY\0000.
Status: 0xC0000365
```

`0xC0000365` is `STATUS_FAILED_DRIVER_ENTRY`. `WUDFRd` is Microsoft's
user-mode driver reflector, the kernel side of the host process this driver
runs in. It failed, so the host never started, so `DriverEntry` never ran.

It is not intermittent. Every boot since the device node was created, with
no exceptions:

| boot | failure |
|---|---|
| 28/09 16:17:34 | 16:17:36 |
| 29/09 09:39:15 | 09:39:16 |
| 01/10 09:14:20 | 09:14:21 |
| 02/10 09:34:22 | 09:34:23 |
| 05/10 09:48:15 | 09:48:16 |
| 06/10 09:21:04 | 09:21:05 |

Why: this is a **root enumerated** device, so Windows starts it during boot
device enumeration, which is before the user-mode driver framework is
usable. A user-mode driver cannot run that early. Windows does not retry a
failed driver load, so the device sits in error until something
re-enumerates it, which is exactly why every reinstall appears to fix it.

Being root enumerated is not a choice that can simply be reversed; see
`AGENTS.md`, where binding to the USB interface instead is ruled out by the
display stack's required upper filter.

**Do not go looking for this in the driver log.** Two traps, both of which
have already wasted a session:

- The driver truncated its log on every load, so the first reinstall after
  a bad boot destroyed the evidence. It now keeps one generation as
  `usbdisplaydd.log.prev`. **Read that file when diagnosing a boot fault.**
- Log lines carry a time but no date, so a boot session and a hand started
  one look alike. `DriverEntry` now records the date and the machine's
  uptime, so "2 s after this machine started" is unambiguous.

**This is fixed.** `install.ps1` and `driversetup.exe` both register a
scheduled task, `usbdisplay repair after startup`, which re-enumerates the
device as SYSTEM once the framework is up. Two triggers: at logon with a
15 second delay for the normal case, and at boot with a one minute delay so
a machine left sitting at the logon screen still drives the panel.

The repair itself is `usbdisplay/scripts/repair.ps1` for a source build and
`driversetup /repair` for a release. Either is safe to run by hand at any
time; restarting a working display costs about four seconds of black
screen. It appends to `%TEMP%\usbdisplay-repair.log`.

Verified end to end on the machine rather than reasoned about:

```
state    : Ready
runas    : SYSTEM  level=Highest
trigger  : MSFT_TaskLogonTrigger  delay=PT15S
trigger  : MSFT_TaskBootTrigger   delay=PT1M
result   : 0x0

2026-10-06 10:51:59  repair starting, 5456 s after this machine booted
2026-10-06 10:52:00  found ROOT\DISPLAY\0000, status OK
2026-10-06 10:52:07  restarted ROOT\DISPLAY\0000, status now OK
```

and the driver's own log confirms it reloaded and lit the panel:

```
10:52:03  DriverEntry on 2026-10-06, 5460 s after this machine started
10:52:04  pipeline: full frame 1: 1920x1080, 4147216 bytes
```

It restarts the device rather than removing and recreating it, because a
live display device removed with its monitor still in the desktop bug
checked a machine during development. Disable, settle, enable.

The alternative considered and not taken was to create the device node at
logon instead of installing it permanently, so that it is never present
during boot enumeration. Tidier in principle and a great deal more code:
the node must be owned by a running process for its whole lifetime, which
means a service, and the failure mode when that service is late is the very
one being fixed here. A restart after the fact is a few lines and testable.

**The remaining cost is that the monitor appears a few seconds into the
session rather than at the logon screen.** Windows will not load a user
mode driver earlier than that, so it is a floor rather than an oversight.

## The startup repair must decide whether it is needed, and the device status cannot tell it

A follow-up to the entry above, reported as "the driver still does not work
after reboot". It did work, and the complaint was fair anyway.

With both triggers registered, the task runs **twice** in an ordinary
session, and the repair restarted the device unconditionally both times. One
boot, measured:

```
09:00:44  machine booted
09:00:46  WUDFRd failed to load for ROOT\DISPLAY\0000, status 0xC0000365
09:01:30  repair ran (logon trigger, +46 s), restarted the device
09:01:34  DriverEntry, 50 s after this machine started   <- display works
09:02:18  repair ran (boot trigger, +94 s), restarted it again
09:02:22  DriverEntry, 98 s after this machine started
```

So the panel went black, came up, and went black again, the second time to
cure a display that was already working. From the user's side that is a
display that does not work after a reboot.

**The device's own status cannot be used to decide.** This is the trap, and
it is the same one that hid the original fault. From the repair log of that
boot, at 09:01:30, 44 seconds after the load had definitively failed and
before anything had restarted it:

```
2026-10-08 09:01:30  repair starting, 45 s after this machine booted
2026-10-08 09:01:31  found ROOT\DISPLAY\0000, status OK
```

`Get-PnpDevice` reports **OK, `CM_PROB_NONE`, "this device is working
properly"** for a device whose user mode driver never loaded. The kernel
side started fine; it is the host process that did not. So the status reads
identically in the broken case and the working one, and a check built on it
would either restart always or never.

**What does answer the question is the driver's own log**, because nothing
else writes that file. If `C:\Windows\Temp\usbdisplaydd.log` was last
written before this machine booted, the driver has not run this session,
which is exactly the fault. Both the script and `driversetup /repair` now
check that, and the second trigger is a no-op when the first has already
done the work. `/force` and `-Force` override it.

Two details worth keeping:

- **The post-restart check compares against the log's position before the
  restart, not against the boot time.** Under `/force` the driver has
  usually been running, so its log already postdates the boot and a check
  against that would report success without the driver having restarted at
  all. The restart is only confirmed when the log moves.
- **This deliberately does not try to detect a driver that started and
  later stopped showing a picture.** That is a different fault, automatic
  recovery for it was implemented and withdrawn, and the reasoning is under
  "Withdrawn: reprogramming the adapter automatically when it looks dark".
  Confining the check to "has the driver run at all this session" is what
  keeps it from blinking healthy displays.

Verified both ways on the machine. Declining when the driver is up:

```
The driver has already run this session, so nothing is being
restarted: its log has been written since this machine booted.
Use /force to restart it anyway.
```

and restarting when told to, with the driver confirmed back:

```
restarting ROOT\DISPLAY\0000
Done: the driver is running. The monitor should appear within a few seconds.

09:35:20.078  DriverEntry on 2026-10-08, 2075 s after this machine started
09:35:20.560  pipeline: full frame 1: 1920x1080, 4147216 bytes
```

## A monitor arriving is not a screen appearing

**This is what "I still don't see a second screen" actually was**, after the
driver load failure above had been worked around and the driver was
demonstrably running. The two entries above are about getting the driver to
load. Neither of them produces a screen.

The state, measured while the user had no second display:

```
driver     : running, DriverEntry 98 s after boot
device     : OK, CM_PROB_NONE
monitors   : 2 active (WmiMonitorID), both Acer KA240HQ
adapter    : USB Display (indirect display driver), 1920x1080
pipeline   : sent=23694 skipped=1316 dropped=0 failed=0, 489 MB total
desktop    : ONE screen, \\.\DISPLAY1, virtual desktop 1920x1080
```

Everything inside the driver reported success because everything inside the
driver *was* succeeding. The monitor had arrived, a swapchain was assigned,
frames were going out and none were dropped. Windows had simply never
extended the desktop onto it, so there was no second screen for the user and
nothing whatever in the driver's log to find.

**This is the third distinct way this display appears broken, and the only
one with no symptom on the driver side at all.** The others are a dark panel
while frames flow, and a driver that never loaded. Check in this order: the
driver log, then whether the driver ran this session, then the topology.

Diagnose it with the topology, not with the driver:

```powershell
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.Screen]::AllScreens |
    ForEach-Object { "$($_.DeviceName) $($_.Bounds) primary=$($_.Primary)" }
```

One screen listed while `Win32_VideoController` shows the USB adapter at a
real resolution is this fault exactly.

The cure is one call:

```
SetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_EXTEND)
```

which took the desktop from 1920x1080 to 3840x1080 and produced
`\\.\DISPLAY7` at (1920,0). `scripts/extend-desktop.ps1` is that call;
`driversetup /extend` is the same with a condition on it.

**It cannot be done from the startup repair task**, and that shapes the fix.
The desktop layout belongs to an interactive session, so SYSTEM in session 0
has no say over it. Nor can a task run `driversetup /extend` as the
logged-on user, because that program's manifest requires elevation and a
standard user's task could not start it.

So it lives in **the tray program**, which already starts at sign-in from
`HKCU\...\Run` as the signed-in user, unelevated: exactly the identity
needed, where the alternative was a second program that did nothing else.
See `EnsureDisplayIsOnTheDesktop` in `src/tools/usbdisplaytray/main.cpp`.

Two limits on it, both deliberate:

- **Conditional.** If the display is attached in any arrangement it does
  nothing, so somebody who has put the panel above the built-in screen, or
  made it their only one, does not find it rearranged behind their back.
- **Only for the first two minutes of a session.** The fault is a startup
  one, and the driver is often not up when the tray starts, because the
  repair may need two attempts. After that it stops looking, so a user
  detaching this screen on purpose is not fought over it every few seconds.

What is verified and what is not:

- Verified: the fault exists, in the state measured above.
- Verified: `SDC_TOPOLOGY_EXTEND` cures it. That is how the screen was
  brought back.
- Verified: the condition reads the attached case correctly, and the tray
  leaves an attached display alone across 24 seconds of polling without
  disturbing its position.
- **Not verified: the branch that fires.** Reproducing "present but not
  attached" on demand defeated two attempts. `SDC_TOPOLOGY_INTERNAL`
  returns success and changes nothing, and detaching a single display with
  `ChangeDisplaySettingsEx` at 0x0 returns `DISP_CHANGE_BADFLAGS`. The state
  appears to need a monitor arriving mid-session for which Windows has no
  remembered topology, which is what a cold boot plus a late driver load
  produces. **A reboot is the test.**

### The reboot happened, and the session came up working

The test above has now been run, by rebooting the machine. Reported by the
user as the first time the display has worked after a reboot, and the
measurements agree:

```
11:13:39  booted
11:13:41  WUDFRd failed to load, 0xC0000365   <- still happens, as expected
11:13:55  DriverEntry, 16 s after this machine started
11:14:20  startup repair ran: declined, the driver has already run
11:15:01  startup repair ran again: declined, the same
11:16:39  two screens, 3840x1080, health says showing a picture
```

What this confirms, and what it does not:

- **Confirmed: the repair no longer blanks a working display.** Both
  triggers fired and both declined, where before this fix each one
  restarted the device and cost four seconds of black screen. The decision
  came from the driver's log being newer than the boot, exactly as
  intended.
- **Confirmed: the detector fix matters in practice.** The register read
  `0x43` on the working panel, which is the value the old comparison called
  dark. Without that fix this session would have logged a dark panel while
  the user looked at a working one.
- **Confirmed: no dark panel this session.** Not one "not transmitting"
  line, where every session before the replug produced one within seconds.
  Consistent with the wedge being cleared by the power cycle and not
  returning on its own.
- **Not explained: what started the driver at 16 s.** The load failed at
  2 s and `DriverEntry` ran at 16 s, before either repair trigger. So
  something re-enumerated the device in between, and it was not this
  project's task. Windows may retry once for a root enumerated device when
  the framework comes up, which would mean the repair task is a safety net
  rather than the mechanism. **Do not conclude the task is unnecessary from
  one boot**: six earlier boots on record failed with no recovery at all,
  which is why it exists.
- **Still confounded: the desktop extend.** Two screens were present, but
  Windows may have remembered the arrangement from the previous session
  rather than the tray having applied it. A boot with the arrangement
  deliberately cleared first would separate those.


One encouraging measurement: restarting the device with
`driversetup /repair /force` **keeps** the extended desktop, even though the
display's name changes (`\\.\DISPLAY7` became `\\.\DISPLAY8`). Windows
matches it by monitor rather than by path, so the arrangement has to be
established once per session, not after every repair.

## The live detector had an off-by-one, and it explains some of its "lies"

**The detector was calibrated on two readings and written to require one of
them exactly**, which is the bug. `kRegDisplayLive + 2` was compared with
`== 0x44`, so any other value counted as dark.

Measured on an MS9132 while a person was looking at an ordinary, correct
desktop on the panel:

```
display:  DARK (10 F7 43 12)      <- 0x43, and the panel was fine
```

Observed values of that byte since, with the panel state confirmed by eye
each time:

| byte | panel | occurrences |
|---|---|---|
| 0x44 | showing | the original calibration |
| 0x43 | showing | confirmed by eye |
| 0x9D | showing | immediately after a full frame |
| 0x01 | dark | confirmed by eye, twice |

So the low bits vary while displaying, and the high nibble is what
separates the states. The test is now for the **dark signature**, not for a
showing value: dark only when the high nibble is zero, anything else
displaying. `0x9D` is why that direction matters, because it would have
failed any whitelist of known-good values too.

**This partly retracts "that register has now misled four times."** At
least one of those four was this off-by-one rather than the hardware: a
reading of 0x43 on a working panel is exactly the false alarm that made
automatic revival look untrustworthy and got it withdrawn. The register was
answering correctly and the comparison was wrong.

It does not retract the rest. The limit recorded above still stands, that
the register describes what the chip is **transmitting** and not what is on
the glass, and that remains the reason silence is not safe.

The asymmetry in the new test is deliberate. A false "dark" used to drive
reprogramming that blinked healthy displays; a false "displaying" costs a
dark panel that needs one replug. Unknown readings therefore count as
displaying. `usbdisplayctl health` now agrees with the panel in both
directions, where before it reported DARK over a working desktop.

The `pipe:` line it printed is also fixed. It was labelled as "the bit that
is set only when the adapter has stopped displaying" while this document
already recorded, two sections above, that 0xF900 is **not** a display
indicator. It asserted a dark panel next to a working one. The value is
still printed, because it is occasionally useful when comparing two
adapters, and it now claims nothing.

## Unsolved: this adapter's driver cannot light a panel the tool can

**Status: reproducible, cause unknown, several explanations eliminated.**
Recorded because it is the first time this fault has been reproducible on
demand; every previous session described it as intermittent.

The reproduction, on the MS9132 adapter, with nothing else running:

```
driver stopped, one full frame from the tool   ->  showing a picture   6/6
start the driver                               ->  DARK within 8 s     5/6
```

The driver's log is clean through this: mode programmed, `full frame 1:
1920x1080, 4147216 bytes`, further frames, `dropped=0 failed=0`, no
cancellations, no timeouts. Every transfer the driver makes succeeds and
the panel is dark anyway. The single exception stayed lit for 30 seconds
and could not be reproduced.

**What this is not.** Each of these was measured, not reasoned about, and
each is now ruled out:

- **Not the brightness tray or the gamma repaint.** The log shows the tray
  forcing a second full repaint milliseconds after the first, which looked
  promising. Killing the tray entirely changes nothing: the panel still
  goes dark.
- **Not the keepalive band walk.** Turning it off with `IdleRefresh=0` once
  produced a panel that stayed lit for 30 seconds, which looked like the
  answer. It did not hold: with the keepalive off, both a plain driver
  restart and a tool-lit panel handed to the driver went dark anyway. One
  promising measurement, disproved by the next two. The registry override
  was removed again rather than shipped.
- **Not the transfer pattern.** `testpattern --bands` reproduces the
  driver's exact shape, 128 row bands each transmitted twice, with no
  driver involved. The panel stays lit. So neither banding, nor the double
  transmission, nor 9 bands in 272 ms is what does it:

  ```
  one full frame, once          ->  showing
  128 row bands, each twice     ->  showing
  one full frame again          ->  showing
  128 row bands, each once      ->  showing
  ```
- **Not a failing or cancelled transfer.** `failed=0` and `dropped=0`
  throughout, and the dark reading precedes nothing.
- **Not the mode programming being skipped.** The driver logs `adapter was
  reprogrammed, repainting everything` and does `PowerOn` then `SetMode`,
  the same pair the tool calls, through the same `SendTransfer` that sends
  the zero length packet and enables the output.

**What is left.** The two paths call the same functions in the same order
and differ in how the pixel pipe is owned: the driver's handle is opened
inside WUDFHost as LOCAL SERVICE and written from `FrameSender`'s thread,
the tool's from an ordinary elevated process on its main thread. That is
the next thing to examine, and it is not something the tool can be made to
imitate from outside.

Until then the honest statement is that **this adapter needs a physical
replug**, and that the driver may darken it again on the next start.

### The replug worked, and it says what kind of fault this is

Confirmed on the machine: a physical unplug and replug restored the panel,
and it then stayed lit under the driver with no complaint at all.

```
10:42:07  attach: 345F:9133, after the driver had been waiting 9 s for it
10:42:08  full frame 1, then the ordinary band walk
10:43:58  sent=436 skipped=1 dropped=0 failed=0, 212 MB
          health: showing a picture (10 F7 43 12), held over 60 s
```

Not one "not transmitting" line in the whole session, where every previous
session today produced one within seconds.

**Every session today splits perfectly on one line of the log:**

| how the driver attached | sessions | outcome |
|---|---|---|
| `PrepareHardware: adapter present` | 09:01, 09:02, 09:45, 10:02, 10:17, 10:23, 10:33 | dark, every time |
| `PrepareHardware: no adapter yet, watching for one` | 10:41 | works |

That looks like the answer and it is probably not, which is worth stating
plainly so the next person does not chase it. **The two cases are
confounded.** The second one is both a different code path *and* a
freshly power-cycled adapter, because the only way to reach it is to
unplug the thing.

The code path is the weaker explanation of the two. `TryAttach` is one
function called from two places, synchronously from `PrepareHardware` when
the adapter is already there and from `WatcherLoop` when it arrives later.
It does the same work to the chip either way, so there is little for the
ordering alone to break.

**So the better reading is that the dark state is a wedge in the adapter
that only a power cycle clears**, which is what this document said at the
outset and what today's measurements support from a new direction:

- A full frame from the tool lights the panel **temporarily**, and the
  driver then darkens it again within seconds. The wedge survives the frame
  that appears to cure it.
- Reprogramming, output enable, and a device restart all leave it dark.
- A power cycle clears it permanently.

That is why no software remedy has ever held: every one of them was applied
to a chip that was still wedged. It also means the earlier conclusion, that
a frame revives a dark adapter, is **too strong**. It revives the picture,
not the chip.

To separate the confound properly, restart the device now, while the
adapter is freshly replugged and working, so that the driver takes the
`adapter present` path on a chip that is not wedged. If it stays lit the
code path is innocent and the wedge is the whole story. If it goes dark the
path matters after all. **That test costs a working display and a physical
replug to undo, so it needs the user's agreement rather than being run on
its own.**




