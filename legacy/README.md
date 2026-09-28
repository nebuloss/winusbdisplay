<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Frozen first implementation

This directory is the original driver, kept whole and no longer developed.
It is here for two reasons.

1. **It works, or at least it did.** An earlier revision drove the panel
   correctly. Later changes regressed it and the regression was never
   isolated, which is what prompted the rewrite in `../usbdisplay`. The proven
   parts, chiefly the control plane and the wire framing, were carried across
   rather than rediscovered.
2. **It is the written record of the protocol.** Almost every comment in
   `src/common` encodes something that cost real time to find out. The
   conclusions were lifted into `../docs/protocol-notes.md`, but the code is
   the primary source.

Nothing here is built by the new project's scripts, and nothing in the new
project includes a header from here. If you find yourself wanting to, copy the
file across and adapt it instead, so the freeze stays meaningful.

## Building it anyway

The scripts still work and still resolve paths relative to this directory, so
output lands in `legacy/build/` rather than the repository root.

```
legacy\scripts\build-tool.bat
legacy\scripts\build-driver.bat
legacy\scripts\install-all.ps1
```

Note that the old and new packages install the same root device node and
compete for the same exclusive USB pipe, so only one of them can be present
at a time. Run `legacy\scripts\purge.ps1` before installing the other.

## Where the history is

`git log` covers this tree from the point version control was introduced. For
the period before that, the `reconstructed` branch replays the changes out of
the development transcript, one commit each, backdated and interleaved with
the feedback that prompted them. It is a reconstruction rather than a
recording: most files match byte for byte, some drift by a few lines, so trust
it for sequence and intent rather than exact content.

```
git log --oneline -S<symbol> reconstructed -- legacy/src/driver/device.cpp
```
