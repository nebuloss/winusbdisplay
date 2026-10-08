# AGENTS.md

Orientation for working in this repository. Read `docs/protocol-notes.md`
before changing anything that talks to the hardware.

## What this is

Windows drivers for MacroSilicon MS912x / MS913x USB display adapters. GPL-2.0
deliberately: derived from the GPL-2.0 `ms912x` Linux driver and
MacroSilicon's own GPL-2.0 Linux sources. **Do not relicense or accept
permissively licensed reimplementations without revisiting that decision.**

```
usbdisplay/  the project
docs/        protocol notes, troubleshooting, the original specification
reference/   vendor archives, not in git
scripts/     shared elevation helper
```

An earlier implementation is frozen at the `legacy-final` tag. **It has been
mined out; do not go looking there.** Every protocol constant it contains is
now in this tree, checked mechanically rather than assumed: of ninety, the
only sixteen not present here are a device interface identifier deliberately
changed so the two packages cannot claim each other's hardware, two standard
USB request types, and the DDC/CI addresses, and DDC/CI is settled as
impossible for an indirect display. There is no protocol fact left in it that
`docs/protocol-notes.md` does not state better.

It stays for provenance: this is a derived work under GPL-2.0 and that tag is
where the derivation is visible. Reading it to answer a question about the
hardware is wasted effort.

`docs/AGENT_PROMPT.md` is the original specification. It is largely accurate
and wrong in several specific places; `docs/protocol-notes.md` records every
correction with evidence. **When they conflict, the protocol notes win.**

## Version control

**Commit each change separately, as soon as it is verified.** Several
regressions in the first implementation were only caught by the user noticing
the display had broken, and with no history there was no way to bisect.

- One commit per logical change, with the measurement that justified it.
- Commit before starting an experiment, so reverting is one command.
- Experiments that failed are worth committing and then reverting, so the
  reasoning survives. `docs/troubleshooting.md` lists ruled-out hypotheses
  that would otherwise be retried.
- `build/` is ignored, as are the vendor archives, which are large and
  separately redistributable.
- The `reconstructed` branch is the earlier implementation's history from
  before git existed, replayed from a session transcript. It documents how
  that implementation was arrived at, which is of historical interest only
  now that the implementation itself is superseded. It is a reconstruction,
  not a recording: trust it for sequence and intent, not exact bytes.

## Commands

```
usbdisplay\scripts\build-tool.bat         # usbdisplayctl -> usbdisplay\build\
usbdisplay\scripts\test.bat [filter]      # build and run the tests
usbdisplay\scripts\smoke.ps1 [-Install]   # check a build against real hardware
usbdisplay\scripts\build-driver.bat       # the driver
usbdisplay\scripts\build-setup.bat        # driversetup.exe, the install step
usbdisplay\scripts\build-tray.bat         # usbdisplaytray.exe, brightness
usbdisplay\scripts\build-probe.bat        # brightnessprobe.exe, diagnostic
usbdisplay\scripts\build-shader.bat       # regenerate the committed shader
usbdisplay\scripts\install.ps1            # both packages, elevated
usbdisplay\scripts\reattach.ps1           # after the USB package is replaced
usbdisplay\scripts\repair.ps1             # re-enumerate a device that failed
                                          # to start; this is the cold boot fix.
                                          # Does nothing unless the driver has
                                          # not run this session; -Force to
                                          # restart regardless
usbdisplay\scripts\purge.ps1              # remove everything

scripts\elev.ps1 -Start                # one UAC prompt per session
scripts\elev.ps1 -Script <abs path>
scripts\elev.ps1 -Command '<powershell>'
scripts\elev.ps1 -Stop
scripts\extend-desktop.ps1             # force the panel into the desktop
```

On Linux, where the whole release is built:

```
usbdisplay/scripts/cross-build.sh         # driver, driver step, both tools
usbdisplay/scripts/package.sh <version>   # catalogs, signatures, installer
```

`build-tool.bat` and `test.bat` compile with `/W4 /WX` and must stay
warning-clean. The driver is `/W4` without `/WX`, because the driver kit
headers are not clean at that level.

## Verification loop

The tests run with no hardware, including the protocol sequences, which talk
to a stand-in that records what was sent:

```
usbdisplay\scripts\test.bat
usbdisplay\scripts\test.bat planner      # one group
cd usbdisplay && make test                # same tests, no Windows needed
cd usbdisplay && make build               # compile only
```

`make` refuses to run on anything but 64-bit x86, and says why rather than
failing later at a missing header: the conversion kernels are written against
SSE2 directly, which is baseline there and would need a second
implementation anywhere else.

Everything the tests cover is free of any operating system, which is why
they also build with an ordinary compiler on an ordinary Linux box. Keep it
that way: if a change to `src/core` or `src/render` starts needing
`windows.h`, it has been put in the wrong layer. Platform-specific code goes
in `src/core/usb.*`, `src/core/open_device.cpp` or `src/driver`.

`test_portable.cpp` enforces this by reading the sources rather than
compiling them, which is unusual for a test and is the point: the fault is
in what was written, and only visible before a compiler has had a chance to
forgive it. It checks for Microsoft's own spellings of standard functions,
for platform headers creeping into the portable layer, and for files using
things they never included. That last one matters because the Microsoft
compiler supplies a great deal transitively that others do not, so such a
file builds cleanly here and fails elsewhere. Every one of these has
actually happened.

The tool is the hardware harness. Nothing below needs a driver installed
except the last line:

```
usbdisplay\build\usbdisplayctl.exe list | info | edid | modes | timings
usbdisplay\build\usbdisplayctl.exe plan            # planner decisions, explained
usbdisplay\build\usbdisplayctl.exe selftest        # is this binary sane here
usbdisplay\build\usbdisplayctl.exe --dump out testpattern   # offline pipeline
usbdisplay\build\usbdisplayctl.exe benchsizes      # re-measure the cost model
usbdisplay\build\usbdisplayctl.exe testpattern --bars
```

Five commands are deliberately missing from its own usage text because they
are for investigation rather than use: `health`, `enable`, `trigger`, `peek`
and `poke`. They work; `peek` and `poke` talk straight to chip registers.

The driver writes `C:\Windows\Temp\usbdisplaydd.log`. **Read it first.** The
Windows event log will generally only say "problem code 10".

Three things about that log, each of which has already cost time:

- It is truncated on every load, so a reinstall destroys the record of the
  session that went wrong. One generation is kept as
  `usbdisplaydd.log.prev`, and **that is the file to read for anything about
  startup.**
- Lines carry a time but no date, so a boot session and a hand started one
  look alike. `DriverEntry` records the date and the machine's uptime; find
  that line before trusting any timestamp.
- **An empty log is itself a finding.** It means the driver never ran, which
  is a Windows load failure invisible from inside the driver. See the cold
  boot entry under settled questions.

`scripts\smoke.ps1` checks a built driver against whatever is plugged in,
in about two minutes. Everything in `tests\` runs without hardware, which
is what makes it fast and also means none of it could have caught any of
the faults that actually reached the panel: a register read that is valid
on one chip and meaningless on the other, an adapter left dark while every
counter reported success, a pointer drawn as a solid block. Run it after
installing.

`usbdisplayctl health` asks the adapter whether it is transmitting a
picture. **It reports what the chip is sending, not what is on the glass**,
and those two came apart once already: a change that removed the keepalive
left the register insisting all was well while the panel was black. It is
the right tool for spotting the dark state, and it is not evidence that a
change is safe. Look at the screen.

**The detector tests for a dark signature, not for a known-good value**, and
that distinction was a real bug. It used to require `0x44` exactly and so
called a working panel dark whenever the register read `0x43`, which happens
often; `0x9D` has also been seen on a working panel. Dark is a zero high
nibble and everything else counts as displaying, because a false "dark" used
to drive reprogramming that blinked healthy displays while a false
"displaying" only costs a replug. This also partly explains the register's
reputation for lying: at least one of its four recorded "lies" was this
comparison, not the hardware.

## The one non-obvious thing that unblocks everything

The control side of the adapter **needs no driver**. Its control transfers are
HID feature reports aimed at interface 0, which is a plain HID collection
Windows already owns, so `HidD_SetFeature` and `HidD_GetFeature` produce
identical traffic.

Chip identification, connector type, EDID, flash, power and the entire mode
programming sequence are therefore testable right now, with no package, no
signing and no reboot. Only pixels need WinUSB.

## Architecture

```
usbdisplay/src/core/  display_device.h  the seam: what the rest of the driver
                                        may assume about any adapter
                      link.h            where control exchanges and pixels go,
                                        with no operating system in sight; it
                                        is why the tests run on Linux
                      macrosilicon.*    the MS912x/MS913x implementation
                      proto.*           its wire constants, no logic
                      mode.h            device independent mode and connector
                      usb.*             HID control plus WinUSB bulk handles
                      open_device.cpp   enumeration and probing, Windows
usbdisplay/src/render/ rect.*        rectangles and the cost model interface
                      damage.*       the planner, and refinement
                      convert.*      conversion kernels and framing
                      converter.*    RegionConverter, processor and graphics
                      overlay.*      blending the pointer into already
                                     converted pixels; arithmetic only, so
                                     it can be tested without a compositor
usbdisplay/src/driver/   driver.cpp     the display callbacks
                      device.*       adapter, monitor, mode list
                      pipeline.*     the frame loop
                      sender.*       the thread that owns the USB write
                      graphics.*     the long lived Direct3D device
                      cursor.*       pointer shape and position, from the OS
                      settings.*     runtime knobs, read from the registry
                      log.*          where every investigation starts
usbdisplay/src/tools/ usbdisplayctl/   the hardware harness
                      setup/           driversetup.exe: packages, device
                                       nodes, certificate, permissions
                      usbdisplaytray/  brightness, notification area
                      brightnessprobe/ which brightness paths a monitor
                                       actually answers
usbdisplay/tests/        runs without hardware
```

**The two halves of the adapter live on different USB interfaces**, and
Windows gives different drivers to different interfaces. Control is HID on
MI_00; pixels are bulk on MI_03 through WinUSB. WinUSB will not forward an
interface-recipient control request to an interface it does not own, so both
handles are always held. The bulk pipe is exclusive: the tool and the driver
cannot both run.

The driver is **root enumerated**, not bound to the USB interface, because the
display stack's required upper filter is incompatible with the framework's
WinUSB dispatcher. It reaches the hardware through user mode handles, which
works because these drivers run in a user mode host process. Both packages
must be installed.

## Continuous integration

- `.github/workflows/linux.yml` runs the test suite under two compilers and
  again under the address and undefined behaviour checkers, **and cross
  compiles the driver**. A few minutes, on every push.
- `.github/workflows/windows.yml` builds everything natively and publishes
  a package when a tag starting with `v` is pushed.

### The driver really does cross compile

Verified, not theorised: the Linux workflow produces a 292 KB Windows
library exporting the framework entry point, within a few hundred bytes of
what the Windows build produces from the same sources.

This is usually assumed impossible, so the reasoning is worth keeping.

A user-mode driver of this kind is an ordinary Windows DLL. Check what the
built one actually depends on and there is no driver runtime in the list at
all: the framework and the display extension are both bound at load time
through function tables, so the only things the link needs from the driver
kit are two small static stubs, `iddcxstub.lib` and `WdfDriverStubUm.lib`,
together about 600 KB.

Everything else is the ordinary Microsoft toolchain, and
`scripts/cross-build.sh` fetches it from two places:

- **xwin** for the compiler's own runtime and the Windows SDK. Those do not
  come from a package feed: they are distributed through the Visual Studio
  installer, and xwin is the tool that reads that manifest and lays the
  result out for a case sensitive filesystem. Rolling this by hand was
  tried and fails at the first include, because Windows headers include
  each other with inconsistent capitalisation and the compiler's own
  runtime headers, `excpt.h` and its neighbours, are not in the SDK at all.
- **NuGet** for the driver kit, which is not part of that feed but is
  published as an ordinary archive.

It then compiles with `clang-cl` and `lld-link`. Four things had to be dealt
with, all of which are invisible on Windows and fatal anywhere else:

- **Capitalisation.** Nothing agrees on it. The kit's headers ask each other
  for names that differ from the files on disk, they ask the Windows SDK for
  names that differ again, and this project uses a third spelling. Every kit
  header therefore gets a lowercase alias, and the specific mixed-case names
  the kit asks of the SDK are added there too. This failed three times, once
  per direction, before being fixed as a class rather than a symptom.
- **An enumeration** declared ahead of its definition with an underlying
  type and defined without one. Microsoft's compiler accepts the mismatch;
  clang refuses, and there is no flag, because the check is not a diagnostic
  that can be switched off. The declaration is patched at extraction.
- **The compiler version.** The Microsoft standard library refuses anything
  it does not recognise, by static assertion. The build machine's newest is
  a year too old, so a current one comes from the LLVM project's packages.
- **`IDDCX_VERSION_MAJOR`** and its companions are not derived from the
  include path and must be defined explicitly.

### The compiled shader is committed

`src/render/generated/convert_cs.h` is a generated file kept in the tree,
which normally deserves suspicion. The reasoning:

The compiler that produces it, `fxc`, runs only on Windows, and Direct3D 11
accepts nothing else. The modern compiler emits a different bytecode
entirely and Microsoft have declined to add the old one back; the only other
option is a by-product of a compatibility layer rather than a tool meant for
this. Building it elsewhere therefore meant running a Windows program under
an emulator, which was by a wide margin the most fragile step in the whole
build, for one small artefact.

The input is a hundred lines that change perhaps twice a year and the output
is deterministic, so committing it trades a rebuild nobody wants for a
dependency everybody pays. After editing the shader run
`scripts\build-shader.bat` on Windows and commit the result.

The risk this creates is a committed artefact drifting from its source, so
`test_convert.cpp` reads the shader and checks every arithmetic constant in
it against the processor path. Change the shader without regenerating and
the tests say so, and say which script to run.

The driver installation step is built the same way and by the same script, so
every binary a user ends up with, driver, console tool, brightness control and
the program that installs them, comes off a Linux machine.

### Catalogs and signing also work on Linux

This was asserted here to be impossible and it is not, which is worth
recording as a caution about the rest: "needs Windows" was received wisdom
rather than a finding.

A driver catalog is a PKCS#7 SignedData carrying a Microsoft certificate
trust list, content type OID `1.3.6.1.4.1.311.10.1`: a DER structure naming
every file in the package with its hash. Nothing in it is secret. Two
existing projects cover the job between them, and neither replaces the
other:

- **`LINBIT/generate-cat-file`** builds the unsigned catalog. C99, no
  library dependencies, GPL-2.0, and used in production by WinDRBD to ship a
  *kernel* driver, which is a far harder audience than this package.
- **`osslsigncode`** puts the Authenticode signature on it. An ordinary
  Ubuntu package, able to sign `.cat` files since 2022. It cannot create
  one.

`scripts/make-catalog.sh` drives both and `scripts/package.sh` assembles the
release around them. The release job lives in the Linux workflow now; the
Windows one builds and tests and publishes nothing.

Two details that are easy to get wrong:

- **Sign after cataloguing, not before.** It looks backwards, because
  signing changes the file. It works because the hash recorded for an
  executable skips the checksum and the certificate table, the two areas
  signing writes to. That is what the generator's `strip-pe-image` computes.
- **Member hashes are SHA1 and that is correct.** It is what the format has
  always used and what Windows 10 and 11 still accept. The *signature* is
  SHA256, and the signature is what carries the trust decision.

### The release is one file, and NSIS builds it

A release is `usbdisplay-setup.exe` and nothing else. It used to be an
archive of directories with an installer among them, which is one more thing
for a user to get right than it should be, and every way of getting it wrong
ends in a program that cannot find what it installs.

**NSIS builds Windows installers natively on Linux**: it is the `nsis`
package, an ordinary Linux binary, no emulation. That decided it. WiX needs
Wine to produce an MSI and Inno Setup needs Wine full stop, and the point of
this build is that nothing runs under a translation layer.

The division of labour matters more than the tool:

- `installer/usbdisplay.nsi` does what every installer does: elevation,
  compression, unpacking, the installed programs list, an uninstaller.
- `src/tools/setup` does only what this hardware needs: driver packages,
  device nodes, trusting the certificate, the brightness permission.

Writing the first half by hand was started and abandoned, correctly. None of
it is specific to this project and all of it has well known ways of being
subtly wrong. The second half cannot be done in an installer script without
either a plugin or tools the user does not have, and it is where all the
knowledge in this project lives, so it stays a program that can be run and
debugged on its own against a source build.

Neither workflow can test against hardware, so a green build means it
compiles and the logic holds, never that the panel lights up.

## Adding support for another adapter

`core/display_device.h` is the seam, and it is the only file to read first.
Everything above it, about four fifths of the code, is about Windows and
about deciding what to send; everything below is one chip.

To add a second one: implement `DisplayDevice`, and add a probe to
`OpenDisplayDevice` in `macrosilicon.cpp`. Nothing else in the tree should
need to change, and if it does, that is a bug in the seam rather than in the
new device.

The interface carries a few things that look like they belong elsewhere, and
they are there because they differ per device and the frame loop cannot be
written without them: what a transfer costs, how many times a region has to
be sent, the alignment the hardware demands, and the wire format. Each was a
MacroSilicon fact hardcoded somewhere it did not belong.

`render/rect.h` defines `TransferCostModel`, which `DisplayDevice` extends.
The damage planner merges by comparing costs and never learns what the unit
is, which is what keeps it device independent. `tests/test_damage.cpp` proves
that with two deliberately different models.

The seam has since earned its keep without a second vendor being involved.
Supporting the USB 3 MacroSilicon parts meant changing what `TransferCost`
returns and nothing above it, even though the right planning strategy for
those parts is the reverse of the USB 2 ones. Nothing in `render/` or
`driver/` needed touching.

**Identify the adapter in one place, and early.** `MacroSiliconDevice` reads
its chip id and connector once, from `SetModeLocked`, because that is the
one point every path reaches before a picture can appear. Leaving it to
callers produced the same bug twice: a path that had not asked got the
conservative defaults and quietly behaved like the wrong chip. The console
tool, which puts up a test pattern without ever asking what it is talking
to, is the one that found it.

## Settled questions, do not re-litigate

Each cost real investigation; the evidence is in `docs/protocol-notes.md`.

- **The adapter is vsync locked at 60 Hz, on both families.** Cost is
  quantised into 16.67 ms slots. **How much fits in a slot differs by chip
  and is read from it, never assumed**: about 520 KB on the USB 2 parts, a
  whole 1080p frame on the USB 3 ones. That is not just a speed difference,
  it inverts what the planner should do, because merging is ruinous on one
  and always right on the other. An unidentified chip is charged the slower
  rate: the opposite guess turns typing into full repaints. Re-measure with
  `usbdisplayctl benchsizes`.
- **Every region must reach both of the adapter's internal copies.** It
  alternates between them on every transfer, so a region sent once lands in
  one and leaves the other stale, and the two alternate visibly: with a
  moving pointer it looks like two pointers. The pipeline sends each region
  **twice back to back, taking both buffers before sending either**. Sending
  the pair a frame apart, and tracking which copy is next, were both tried
  and both failed; nothing keeps the driver's idea of the next copy in step
  with the adapter's. The vendor source confirms this from the other
  direction: the board's memory is divided into exactly two frames, on both
  families, so this is not an optimisation waiting to be removed.
- **~30 MB/s is a hardware ceiling on the USB 2 parts**; pipelined
  overlapped transfers at depth 2, 4 and 8 all measure within 1% of
  synchronous, so there are no host-side wins to find. The USB 3 parts reach
  about 240 MB/s, which is why a whole frame fits in one slot there.
- **There is no cheaper pixel format.** RGB565, RGB888, YUV422 and YUV444 are
  the options, and 4:2:2 at 16 bpp is already the cheapest.
- **`vSyncFreqDivider` above 1 breaks the topology**, even with correct
  timings. Windows rejects it outright. Pacing is done by dropping instead.
- **Mode timings must be self consistent**: `pixelRate` equals
  `totalSize.cx * totalSize.cy * vSyncFreq`, `hSyncFreq` equals
  `pixelRate / totalSize.cx`, and `totalSize` includes blanking. Monitor
  modes additionally require `vSyncFreqDivider == 0`.
- **Both conversion paths must be bit exact.** They are chosen per update by
  size, so a region redrawn at slightly different sizes takes different paths
  on consecutive frames; a one bit disagreement shimmers on small text. The
  tests enforce this.
- **Windows never calls the display driver's I2C callbacks**, so monitor
  control over the cable cannot work. Measured, and Microsoft documents it.
  Brightness is applied during conversion instead.
- **A zero dirty rectangle count means nothing changed**, not "assume
  everything changed". Repainting on those frames costs a full repaint on
  frames with nothing to draw, and lags the display by hundreds of
  milliseconds.
- **The panel needs traffic to stay lit, and a still desktop produces
  none**, because the compositor stops presenting entirely. The pipeline
  repaints a band on an interval the device reports (`KeepaliveMs`), sized
  from the cost model so it fits one transfer slot. Remove it and the panel
  goes black while every counter reports success. The one exception is the
  repaint after reprogramming, which goes out whole rather than in bands,
  because stripes filling in over four seconds are worse than paying for a
  single expensive transfer.
- **The driver has to draw the mouse pointer itself.** An indirect display
  is handed the desktop without it, on the assumption the hardware has a
  cursor of its own. Harder than drawing it is **erasing** it: moving the
  pointer changes two areas of the screen and the compositor reports
  neither, so both must be added to the damage by hand or the panel fills
  with copies. Blending happens after conversion, in the adapter's own
  format, which looks wrong and is right: the colour transform is a matrix,
  so it is linear.
- **Windows fails to load this driver at boot, every single time, and that
  is structural rather than a bug.** The device is root enumerated, so Plug
  and Play starts it during early boot device enumeration, before the user
  mode driver framework exists. The load fails with
  `STATUS_FAILED_DRIVER_ENTRY` (`0xC0000365`, System event id 219) and
  Windows never retries, so there is no second monitor for the rest of the
  session. Six boots out of six on record. Installation registers a
  scheduled task that re-enumerates the device at logon and again a minute
  after boot; `repair.ps1` is the same thing by hand, and also recovers an
  adapter that stopped for any other reason. This is why reinstalling always
  appeared to cure a dark panel, and that coincidence sent this project
  chasing faults in how the adapter is driven.
- **A device whose user mode driver failed to load still reports itself
  healthy.** `Get-PnpDevice` says `OK` / `CM_PROB_NONE` / "working
  properly", measured 44 seconds after a load failure and before anything
  had restarted it. The kernel side started; only the host process did not.
  So **never decide anything from the devnode status**: it reads identically
  in the broken case and the working one. The usable signal is the driver's
  log, because nothing else writes it. A log last written before the machine
  booted means the driver has not run this session. The startup repair uses
  exactly that, so the second trigger does not blank a display the first one
  already fixed.
- **A monitor arriving is not a screen appearing, and this failure is
  invisible from inside the driver.** Measured: driver loaded, monitor
  announced, swapchain assigned, 489 MB sent, nothing dropped, and *one*
  screen on the desktop, because Windows had never extended onto the new
  monitor. There is nothing in the driver's log to find, because nothing in
  the driver is wrong. Check the topology, with
  `[System.Windows.Forms.Screen]::AllScreens`, not the driver. The cure is
  `SetDisplayConfig(SDC_APPLY | SDC_TOPOLOGY_EXTEND)`.
  **So there are three separate ways this display looks broken**: a dark
  panel while frames flow, a driver that never loaded, and a monitor that
  is not on the desktop. They have nothing in common, and only the first
  two show up in the log.
- **The desktop layout cannot be set from a SYSTEM task.** It belongs to an
  interactive session, so the startup repair cannot do it, and
  `driversetup /extend` cannot be a standard user's task either because its
  manifest demands elevation. It is done by the tray program, which already
  autostarts in the user's session, and that is the only reason the tray
  carries display code at all.

## Rules that are easy to violate

- **Serialize the control plane.** A register read is a write then a read;
  two interleaved return each other's answers. Everything goes through
  `Chip`, which holds the lock.
- **Never block the frame thread on USB.** Convert on it, hand the bytes to
  `FrameSender`, and if no buffer is free, **drop** the region: the damage
  stays owed. Queueing builds unbounded latency and Windows declares the
  monitor hung.
- **A dropped region drops the rest of the frame.** Otherwise a later region
  arrives without the one that should have preceded it.
- **Do not discard the record of what is on screen when dropping.** Nothing
  was sent, so it is still accurate, and throwing it away turns a moment of
  congestion into a lasting one.
- **Cancel in-flight transfers on stop.** A full frame owns the bus for over
  100 ms and PnP will not wait.
- **Even x, multiple-of-four width, even y and height** on every region. UYVY
  encodes pixel pairs and the vendor masks the extent with 0xFFC.
- **Zero length bulk packet after every transfer.** Without it the adapter
  waits for data that never comes and the panel stays dark while every write
  reports success.
- **Do not enable the output during mode programming.** It is enabled once a
  frame has landed, so the panel never shows leftover memory.
- **Probe the chip id, never infer it from the USB product id.** The test
  unit reports a USB 3 product id and contains USB 2 silicon. A second
  adapter reports **the same product id** and does contain USB 3 silicon, so
  the id distinguishes nothing at all: both are `345F:9133`, and only the
  signature tells them apart. The bulk packet size, 512 against 1024, is a
  second independent hint in the log.
- **Memory is a board property, not a chip one.** Read it; two adapters with
  the same silicon ship with different amounts, and it decides which modes
  fit. A mode the memory cannot hold produces a corrupt picture rather than
  an error.
- **Object attributes must carry a context type.** Creating a monitor with
  null attributes succeeds and then arrival fails with a status that means
  nothing at all.
- Source rows from a mapped staging texture use `RowPitch`, which is not
  `width * 4`.
- **Replacing the WinUSB package detaches the pixel interface** and it does
  not come back on its own. The installer skips the package when it is
  already current; if it did replace it, run `reattach.ps1`.
- **Settings live in `HKLM\SOFTWARE\usbdisplay`, not `HKCU`.** The driver
  runs inside WUDFHost as LOCAL SERVICE and has no user hive to read. The
  installer widens the ACL on that one key so the brightness control can
  write it without elevating every time a slider moves.
- **Brightness is the gamma ramp path, applied during conversion.** Windows
  has three mechanisms: DDC/CI cannot reach a display that is not on a
  graphics card's signalling hardware, and the WMI path needs a kernel
  driver this project does not have and could not test. `brightnessprobe`
  reports which paths a given monitor answers.
- **Create the Direct3D device once, not per swapchain.** The first creation
  in a process loads the graphics driver and takes hundreds of
  milliseconds, by which time a swapchain handed to a callback has gone
  stale and fails with `DXGI_ERROR_ACCESS_LOST`. Measured here: 412 ms and
  a failure, then 60 ms and success. Hence `driver/graphics.*`.
- **Never remove a live display device node.** Disable it, wait for the stop
  to finish, then remove. Removing one outright while its monitor is still
  in the desktop bug checked a machine during development, in kernel code
  this project does not contain. A user mode driver cannot cause that; the
  installer provoking it can. See `docs/troubleshooting.md`.

## Environment realities

- VS 2022 **Build Tools** (no IDE), MSVC 14.44, WDK 10.0.26100. ARM64 driver
  builds fail: the ARM64 toolset is not installed.
- Driver installation needs **no reboot and no test signing**, because
  neither package loads a kernel binary. It does need **elevation**.
- `Inf2Cat.exe` and `signtool` are only used by `install.ps1`, which builds
  a catalog locally when installing a source build. `Inf2Cat` ships x86 only
  and its OS names are case sensitive with no plain `10_ARM64` (use
  `10_RS3_ARM64`). `signtool` needs `/sm` to see a
  certificate in the machine store.
- `pnputil` keeps the old binary if `DriverVer` has not changed. Always
  install the **build-stamped** INF from `usbdisplay/build/driver/...`.
- `pnputil` exit code 3010 means "installed, restart to tidy up". Success.
- Windows keeps every copy of a package ever added under a fresh `oemNN`
  name. Stale copies compete to claim the hardware; the first driver left 59
  behind. The installer clears its own previous copies before adding one.
- The shell here is Bash-compatible on Windows with a **minimal coreutils
  set**. `grep`, `sed`, `head`, `tail` and `wc` are *not* available. Use the
  Grep/View tools, or PowerShell.
- When shelling out to PowerShell, the wrapper expands `$_` before PowerShell
  sees it, so pipelines using `$_` fail with a wall of errors. Put the
  snippet in a `.ps1` file.
- MSVC error output is **localised to French**. Match on the error code
  (`error C2065`), not the message text.

## Known unresolved

Small text shimmers on the panel. Inherited from the first implementation.
Seven explanations have now been implemented and measured away, including
the one that looked most promising: `TRIGGER_FRAME`, which the vendor's own
driver carries and leaves commented out, turns out to be accepted by the
chip and ignored. `docs/troubleshooting.md` lists all seven so none is
retried, and `usbdisplayctl trigger --select` reruns the last of them.

The remaining suspect is that the adapter displays progressively during a
transfer, which would make a whole-screen update visible as a sweep. That
cannot be settled from this side: everything reachable by reasoning about
the protocol has now been reached, and what is left is a USB capture of the
vendor driver while small text redraws.

**The dark panel is reproducible on the MS9132 adapter and still
unexplained.** One full frame from the tool lights it every time; starting
the driver darkens it within seconds, with a clean log and every transfer
succeeding. Measured away already: the brightness tray and its gamma
repaint, the keepalive band walk, the band pattern itself including the
double transmission, failing or cancelled transfers, and skipped mode
programming. **Do not re-test those.**

**A physical replug cures it and nothing in software does.** That is the
shape of the fault: a wedge in the adapter that survives everything the
host can send. A frame from the tool lights the panel while the chip is
still wedged, and the driver re-darkens it, so **"a full frame revives a
dark adapter" is too strong: it revives the picture, not the chip.** Every
failed remedy in the history of this bug was applied to a chip that was
still in that state. `docs/troubleshooting.md` has the measurements, the
one confound not yet separated, and the test that would separate it.

## Style

C++17, Google-ish: two space indent, `CamelCase` functions and types,
`snake_case_` members, `kConstant`. Every file opens with an
`SPDX-License-Identifier: GPL-2.0-only` line, and the non-obvious ones carry a
header comment explaining their reason to exist. **Comments explain why, and
the ones present are load-bearing: most encode something that cost real time
to find out. Do not strip them.** No em dashes in source.

Tests carry a sentence explaining why each expectation exists, so a failure
says which behaviour was lost rather than only which number changed.
