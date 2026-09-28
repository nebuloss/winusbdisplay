<!-- SPDX-License-Identifier: GPL-2.0-only -->
# usbdisplay

An open Windows driver for MacroSilicon MS912x / MS913x USB display adapters,
the cheap dongles that turn a USB port into an HDMI or VGA output. It makes
one work as an ordinary second monitor without the manufacturer's driver.

Licensed GPL-2.0, because it derives from the GPL-2.0 `ms912x` Linux driver
and MacroSilicon's own GPL-2.0 Linux sources.

## Status

Working. The adapter appears in Windows as a second screen and shows the
desktop.

| | |
|---|---|
| Adapter identification, connector, hotplug | verified on hardware |
| Reading the panel's capabilities | verified on hardware |
| Mode programming | verified on hardware |
| Colour conversion and framing | verified byte for byte |
| Monitor appears and shows the desktop | working |
| Damage tracking, window drags, idle repaint | working |
| Brightness | working, but not through the display cable (see below) |
| Faint shimmer on small text | unresolved, inherited; see `../docs/troubleshooting.md` |

## The one number that explains everything

The adapter does not stream at a byte rate. It finishes each transfer on its
own 60 Hz boundary, so an update costs a whole number of 16.7 ms slots
whatever its size:

| Update size | Cost | Updates per second |
|---|---|---|
| up to ~520 KB | 1 slot | **60** |
| up to ~1.0 MB | 2 slots | 30 |
| a full 1080p screen, 4.1 MB | 8 slots | 7.5 |

Two things follow, and between them they are most of this project.

**Below ~520 KB, size is free.** A 2 KB update and a 490 KB update cost
exactly the same. Ordinary desktop activity is far below the threshold, so
interactive use runs at the full 60 updates a second. Only a full repaint is
slow.

**Every extra transfer costs a whole slot, and merging can be ruinous.** Two
small changes in opposite corners of the screen cost two slots sent
separately and **eight** merged, because their bounding box is the whole
screen. That shape is exactly what dragging a window produces. So updates are
planned by cost: two regions are merged only when the merged update is no
more expensive than sending them apart.

On top of that, each converted region is compared against what the panel is
already showing and only the part that genuinely differs is sent. This
matters because the compositor sometimes reports the entire screen as changed
when almost nothing has; believing it costs a full repaint.

## Two conversion paths

Pixels have to be converted from what the compositor produces to what the
adapter accepts. There are two implementations and the driver picks between
them per update:

| | Large updates | Small updates |
|---|---|---|
| GPU, compute shader | **faster**, and uses no processor | fixed setup cost dominates |
| Processor, vector code | moves twice as many bytes | **faster** |

The crossover defaults to about a quarter of a 1080p screen and is adjustable
(see Settings). Both paths produce **byte-identical** output, which is a hard
requirement rather than a nicety: a region redrawn at slightly different sizes
takes different paths on consecutive frames, and if they disagreed by even one
least significant bit that region would flicker between two values.

## Quick start

Nothing needs installing for the read-only commands, because the control side
of the adapter is a plain HID device Windows already knows how to talk to:

```
scripts\build-tool.bat
build\usbdisplayctl.exe list
build\usbdisplayctl.exe info
build\usbdisplayctl.exe edid --out monitor.edid
```

To use it as a monitor:

```
scripts\build-driver.bat
powershell -ExecutionPolicy Bypass -File ..\scripts\elev.ps1 -Start
powershell -ExecutionPolicy Bypass -File ..\scripts\elev.ps1 ^
    -Script %CD%\scripts\install.ps1
```

No reboot, no relaxed signing policy, and it works with Secure Boot on:
nothing here loads into the kernel, so all Windows wants is a package signed
by a certificate it trusts, which the installer creates. It does need
administrator rights, which is what `elev.ps1` is for: one prompt for the
whole session instead of one per step.

Remove everything with `scripts\purge.ps1`.

## Tests

```
scripts\test.bat            # all of it
scripts\test.bat planner    # just the group whose name matches
make test                   # the same tests, any compiler, no Windows
```

Everything runs with no adapter plugged in, including the protocol sequences,
which talk to a stand-in that records what was sent and answers with canned
data. That matters because this hardware fails silently: every transfer
reports success and the screen simply stays black or shows the wrong thing.
The sequence is the only thing that can be checked, so it is.

Every expectation carries a sentence explaining why it exists, so a failure
says which behaviour was lost rather than only which number changed.

The suite has no dependency on Windows. That is not an accident: the
protocol, the damage planner, the cost model and the colour conversion all
sit behind interfaces that name no operating system, so they build and run
anywhere. The benefit is quick feedback without a Windows machine, and a
second compiler's opinion on the same code, which catches the undefined
behaviour that any one compiler happens to forgive.

The driver can be built on Linux too:

```
scripts/cross-build.sh
```

That surprises people, so it is worth saying why it works. A user-mode
driver of this kind is an ordinary Windows DLL, and it links against no
driver runtime at all: both the framework and the display extension are
bound at load time through function tables. The only things needed from the
driver kit are two small static stubs and a few headers, and every piece of
it is published on NuGet, which is a plain file server. The script downloads
four archives and compiles with clang.

It needs `clang`, `lld`, `curl` and `unzip`, and nothing else.

What cross-compiling cannot do is sign the result or build an installable
catalog, both of which need Windows tools. The install script does both on
the machine where the driver is actually used, so this matters only if you
wanted to ship from Linux.

## Diagnosing

The tool is also the diagnostic:

```
build\usbdisplayctl.exe plan                  # what the planner does, and why
build\usbdisplayctl.exe benchsizes            # re-measure the cost model
build\usbdisplayctl.exe bench                 # sustained throughput
build\usbdisplayctl.exe testpattern --bars    # put something on the panel
build\usbdisplayctl.exe --dump out testpattern  # ...or into files, no hardware
```

The raw USB pipe is exclusive, so the tool and the driver cannot both hold
it. Disable the display device while using the tool:

```
powershell -File ..\scripts\elev.ps1 -Command ^
    "Disable-PnpDevice -InstanceId 'ROOT\DISPLAY\0000' -Confirm:$false"
```

When the driver misbehaves, read `C:\Windows\Temp\usbdisplaydd.log` first. It
records the exact status of every step. The Windows event log will generally
only tell you "problem code 10".

## Settings

`HKLM\SOFTWARE\usbdisplay`, re-read every half second so changes take effect
without restarting anything.

| Value | Default | Effect |
|---|---|---|
| `Brightness` | 100 | 0..100, applied while converting |
| `Contrast` | 50 | 0..100 |
| `IdleRefresh` | 1 | repaint periodically so the panel keeps its signal |
| `GpuThresholdPixels` | 518400 | updates this large convert on the GPU; 0 disables it |

The machine hive rather than the user's, because the driver runs as a service
account with no user hive to read. The installer widens permissions on that
one key so a brightness control can write it without elevation.

## Brightness

Windows does not route monitor controls over the display cable to indirect
displays, so tools like Twinkle Tray report this monitor as unsupported.
That is an operating system limitation, not a gap here: Microsoft's own
documentation says the OS does not call those driver callbacks, and the
previous driver implemented them completely and correctly and was never
called once. Brightness is applied while converting instead, which genuinely
dims the picture rather than just remembering a number.

## Layout

```
src/core/     display_device.h  what any adapter must provide
              macrosilicon.*    the MS912x/MS913x implementation of it
              usb.*, proto.h    how it is reached, and its wire format
src/render/   rectangles, cost model, damage planning, conversion
src/driver/   the Windows display driver
src/tools/    the console tool, which is also the bring-up harness
tests/        runs without hardware
inf/          the two installable packages
scripts/      build, test, install
```

About a fifth of the code is specific to this adapter, and it is all under
`src/core/` behind one interface. Supporting a different chip means writing
another implementation of `DisplayDevice` and adding a probe; the planner,
the conversion paths and the whole Windows side are unaffected. The interface
carries the things that genuinely differ: what a transfer costs, how many
times a region must be sent, the alignment the hardware demands, and the wire
format.

## Further reading

- `../docs/protocol-notes.md` — what the hardware actually does, and where the
  published descriptions of it are wrong. Read this before touching anything
  in `src/core`.
- `../docs/troubleshooting.md` — symptoms and their usual causes, including
  several hypotheses that were measured and ruled out, so they are not
  retried.
- The `legacy-final` tag holds the earlier implementation, whose comments
  are the primary record of several protocol details.
