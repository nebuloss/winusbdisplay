# winusbdisplay

Open Windows drivers for MacroSilicon MS912x / MS913x USB display adapters,
the cheap dongles that turn a USB port into an HDMI or VGA output.

The active project is **[`usbdisplay/`](usbdisplay/README.md)**. Start there.

```
usbdisplay/  the driver, the tool and the tests. This is the project.
docs/        what the hardware actually does, where the published
             descriptions of it are wrong, and what has already been
             ruled out. Read before changing anything protocol related.
reference/   vendor sources and other people's drivers, not redistributed
             here; see docs/protocol-notes.md for what they are.
scripts/     shared operational helpers, chiefly one elevation prompt per
             session instead of one per command.
```

Licensed GPL-2.0, because it derives from the GPL-2.0 `ms912x` Linux driver
and MacroSilicon's own GPL-2.0 Linux sources. That was a deliberate decision
and an expensive one to reverse.

## Why this was rewritten

An earlier implementation worked, then regressed, and the regression was
never isolated. Rather than bisect a tree that had no history, it was set
aside and rebuilt. The rewrite kept everything that had been established
about the hardware and changed the parts that decide performance:

- Updates are planned by what they **cost** rather than merged into one
  bounding box. The adapter finishes each transfer on its own 60 Hz
  boundary, so two small changes in opposite corners cost two slots sent
  apart and eight merged. That shape is what dragging a window produces.
- Each converted region is compared against what the panel is already
  showing, and only the part that genuinely differs is sent.
- Large updates convert on the GPU and small ones on the processor, because
  each wins decisively in its own range.
- There is a test suite, which runs with no hardware attached and which
  found three real faults the first time it was run.

The original is preserved at the `legacy-final` tag, since its comments are
the primary record of several protocol details. Recover it with
`git checkout legacy-final -- legacy/`.
