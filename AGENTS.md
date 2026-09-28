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

An earlier implementation lives at the `legacy-final` tag. Its comments are
the primary record of several protocol details, so reach for it when
`docs/protocol-notes.md` is ambiguous:
`git checkout legacy-final -- legacy/`.

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
  before git existed, replayed from a session transcript. Use it to find
  which change introduced a symptom. It is a reconstruction, not a
  recording: trust it for sequence and intent, not exact bytes.

## Commands

```
usbdisplay\scripts\build-tool.bat         # usbdisplayctl -> usbdisplay\build\
usbdisplay\scripts\test.bat [filter]      # build and run the tests
usbdisplay\scripts\build-driver.bat       # the driver
usbdisplay\scripts\install.ps1            # both packages, elevated
usbdisplay\scripts\reattach.ps1           # after the USB package is replaced
usbdisplay\scripts\purge.ps1              # remove everything

scripts\elev.ps1 -Start                # one UAC prompt per session
scripts\elev.ps1 -Script <abs path>
scripts\elev.ps1 -Command '<powershell>'
scripts\elev.ps1 -Stop
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
```

Everything the tests cover is free of any operating system, which is why
they also build with an ordinary compiler on an ordinary Linux box. Keep it
that way: if a change to `src/core` or `src/render` starts needing
`windows.h`, it has been put in the wrong layer. Platform-specific code goes
in `src/core/usb.*`, `src/core/open_device.cpp` or `src/driver`.

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

The driver writes `C:\Windows\Temp\usbdisplaydd.log`. **Read it first.** The
Windows event log will generally only say "problem code 10".

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
                      macrosilicon.*    the MS912x/MS913x implementation
                      proto.h           its wire constants, no logic
                      mode.h            device independent mode and connector
                      usb.*             HID control plus WinUSB bulk handles
usbdisplay/src/render/ rect.*        rectangles and the cost model interface
                      damage.*       the planner, and refinement
                      convert.*      conversion kernels and framing
                      converter.*    RegionConverter, processor and graphics
usbdisplay/src/driver/   driver.cpp     the display callbacks
                      device.*       adapter, monitor, mode list
                      pipeline.*     the frame loop
                      sender.*       the thread that owns the USB write
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

Two workflows, split by what each machine can actually do.

- `.github/workflows/logic.yml` runs on Linux and covers the whole test
  suite, twice over with two compilers, and once more with the address and
  undefined behaviour checkers on. Quick, and it runs on every push.
- `.github/workflows/windows.yml` builds the tool, the brightness control
  and the driver, and publishes a package when a tag starting with `v` is
  pushed. The driver job installs the driver kit on the fly, which is the
  most fragile step in either file; it is deliberately a separate job so a
  problem there does not mask a real compile error in everything else.

Neither can test against hardware, so a green build means it compiles and
the logic holds, not that the panel lights up. That still needs a person
with an adapter.

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

## Settled questions, do not re-litigate

Each cost real investigation; the evidence is in `docs/protocol-notes.md`.

- **The adapter is vsync locked at 60 Hz.** Cost is quantised into 16.67 ms
  slots: up to ~520 KB is one slot, the next byte costs a whole extra one. So
  a 2 KB update and a 490 KB update cost the same, and ordinary desktop
  damage runs at full rate. Only full repaints are slow. Re-measure with
  `usbdisplayctl benchsizes`.
- **Every region must reach both of the adapter's internal copies.** It
  alternates between them on every transfer, so a region sent once lands in
  one and leaves the other stale, and the two alternate visibly: with a
  moving pointer it looks like two pointers. The pipeline sends each region
  **twice back to back, taking both buffers before sending either**. Sending
  the pair a frame apart, and tracking which copy is next, were both tried
  and both failed; nothing keeps the driver's idea of the next copy in step
  with the adapter's.
- **~30 MB/s is a hardware ceiling.** USB 2 silicon; pipelined overlapped
  transfers at depth 2, 4 and 8 all measure within 1% of synchronous. There
  are no host-side throughput wins to find.
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
  unit reports a USB 3 product id and contains USB 2 silicon.
- **Object attributes must carry a context type.** Creating a monitor with
  null attributes succeeds and then arrival fails with a status that means
  nothing at all.
- Source rows from a mapped staging texture use `RowPitch`, which is not
  `width * 4`.
- **Replacing the WinUSB package detaches the pixel interface** and it does
  not come back on its own. The installer skips the package when it is
  already current; if it did replace it, run `reattach.ps1`.

## Environment realities

- VS 2022 **Build Tools** (no IDE), MSVC 14.44, WDK 10.0.26100. ARM64 driver
  builds fail: the ARM64 toolset is not installed.
- Driver installation needs **no reboot and no test signing**, because
  neither package loads a kernel binary. It does need **elevation**.
- `Inf2Cat.exe` ships x86 only and its OS names are case sensitive with no
  plain `10_ARM64` (use `10_RS3_ARM64`). `signtool` needs `/sm` to see a
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

Small text shimmers on the panel. Inherited from the first implementation;
five plausible causes were implemented and measured away, and
`docs/troubleshooting.md` lists them so they are not retried. The remaining
suspect is that the adapter displays progressively during a transfer, which
would make a whole-screen update visible as a sweep. Settling it needs a USB
capture of the vendor driver.

## Style

C++17, Google-ish: two space indent, `CamelCase` functions and types,
`snake_case_` members, `kConstant`. Every file opens with an
`SPDX-License-Identifier: GPL-2.0-only` line, and the non-obvious ones carry a
header comment explaining their reason to exist. **Comments explain why, and
the ones present are load-bearing: most encode something that cost real time
to find out. Do not strip them.** No em dashes in source.

Tests carry a sentence explaining why each expectation exists, so a failure
says which behaviour was lost rather than only which number changed.
