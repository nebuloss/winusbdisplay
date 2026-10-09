# SPDX-License-Identifier: GPL-2.0-only
#
# Unpacks the vendor's Windows installer far enough to read its driver
# package. The dongle ships that installer on a small mass storage interface
# it exposes at power-on, so this is also what is on the dongle's own disk.
#
# Why this exists rather than a note saying "use innoextract": the question it
# answered could not be answered any other way, and it was a question this
# project had got wrong for months. The notes asserted that an indirect
# display driver cannot bind to the dongle's USB interface and must be root
# enumerated. The vendor's INF binds to USB\VID_345F&PID_9133&MI_03 and
# always has. The difference is one line, UmdfDispatcher, and no amount of
# reasoning about the display stack would have produced it.
#
# The format, because it is not quite as documented anywhere:
#
#   - Inno guards each compressed block with a CRC32 over a five byte header
#     of { stored size, compressed flag }. That check is strong enough to scan
#     the whole file with: exactly three offsets match in two megabytes, so
#     there is no need to parse the loader's offset table at all.
#   - Inside a block the stored bytes arrive in 4096 byte chunks, each
#     preceded by its own CRC32. Every chunk verified here, which is how this
#     was settled: with the chunk CRCs removed the blocks do not abut, and
#     with them block one ends exactly where block two begins.
#   - The file data is not a block. It is a chunk introduced by the magic
#     "zlb\x1a", which despite the name is followed by LZMA1, and has no CRCs.
#   - The payload is LZMA1 everywhere, so it is handed to 7-Zip as a .lzma
#     container: five property bytes, eight bytes of unknown length, stream.
#     That is the one container format that is a bare LZMA1 stream.
#
# The INF is UTF-16 inside the extracted blob, which is why searching the
# installer for ASCII "[Version]" finds nothing and suggests, wrongly, that
# there is no INF in there.

param(
    [string]$Installer = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) `
        'reference\MacroSilicon USBDisplay Driver Windows10_11 Installer_V4.2.8.11.exe'),
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\vendor-driver')
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Installer)) {
    throw "no installer at $Installer (the vendor archives are not in git; see AGENTS.md)"
}
if (-not (Get-Command 7z -ErrorAction SilentlyContinue)) {
    throw '7z is needed to decode the LZMA1 streams'
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;

public static class InnoSetup {
    static uint[] table = Build();
    static uint[] Build() {
        uint[] t = new uint[256];
        for (uint i = 0; i < 256; i++) {
            uint c = i;
            for (int k = 0; k < 8; k++) c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }
    public static uint Crc32(byte[] d, int at, int len) {
        uint c = 0xFFFFFFFFu;
        for (int i = at; i < at + len; i++) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
    static uint U32(byte[] d, int at) { return BitConverter.ToUInt32(d, at); }

    /* Offsets of every CRC-guarded block header, as {offset, stored size}. */
    public static List<int[]> FindBlocks(byte[] b) {
        var found = new List<int[]>();
        for (int i = 0; i + 16 < b.Length; i++) {
            if (U32(b, i) != Crc32(b, i + 4, 5)) continue;
            uint stored = U32(b, i + 4);
            if (stored == 0 || stored > 64u * 1024 * 1024) continue;
            if (b[i + 8] > 1) continue;
            if (i + 9 + (int)stored > b.Length) continue;
            found.Add(new int[] { i, (int)stored });
        }
        return found;
    }

    /* Offset of the file data chunk, found by its magic. */
    public static int FindDataChunk(byte[] b) {
        for (int i = 0; i + 9 < b.Length; i++) {
            if (b[i] == 'z' && b[i+1] == 'l' && b[i+2] == 'b' && b[i+3] == 0x1A) return i;
        }
        return -1;
    }

    static void WriteLzmaHeader(FileStream fs, byte[] b, int at) {
        fs.Write(b, at, 5);
        for (int i = 0; i < 8; i++) fs.WriteByte(0xFF);
    }

    /* A block: strip the per-chunk CRC32s, keep the LZMA1 stream. */
    public static int SaveBlock(byte[] b, int at, int stored, string path) {
        int pos = at + 9, left = stored, chunks = 0;
        using (var fs = File.Create(path)) {
            bool first = true;
            while (left > 4) {
                uint crc = U32(b, pos);
                pos += 4; left -= 4;
                int n = Math.Min(4096, left);
                if (Crc32(b, pos, n) != crc) throw new Exception("chunk CRC mismatch at " + pos);
                if (first) { WriteLzmaHeader(fs, b, pos); fs.Write(b, pos + 5, n - 5); first = false; }
                else { fs.Write(b, pos, n); }
                pos += n; left -= n; chunks++;
            }
        }
        return chunks;
    }

    /* A chunk: no framing at all past the four byte magic. */
    public static void SaveChunk(byte[] b, int at, int end, string path) {
        using (var fs = File.Create(path)) {
            WriteLzmaHeader(fs, b, at + 4);
            fs.Write(b, at + 9, end - (at + 9));
        }
    }

    public static int FindUtf16(byte[] b, string text, int from) {
        byte[] p = System.Text.Encoding.Unicode.GetBytes(text);
        for (int i = from; i <= b.Length - p.Length; i++) {
            bool ok = true;
            for (int j = 0; j < p.Length; j++) if (b[i+j] != p[j]) { ok = false; break; }
            if (ok) return i;
        }
        return -1;
    }
}
'@

if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$bytes = [System.IO.File]::ReadAllBytes($Installer)
Write-Host ("installer: {0:N0} bytes" -f $bytes.Length)

$blocks = [InnoSetup]::FindBlocks($bytes)
Write-Host ("CRC-guarded blocks: " + $blocks.Count)
foreach ($blk in $blocks) {
    $lzma = Join-Path $OutDir ('block-{0:X}.lzma' -f $blk[0])
    $chunks = [InnoSetup]::SaveBlock($bytes, $blk[0], $blk[1], $lzma)
    Write-Host ("  0x{0:X}  {1:N0} stored bytes in {2} chunk(s), all CRCs good" -f $blk[0], $blk[1], $chunks)
}

$chunkAt = [InnoSetup]::FindDataChunk($bytes)
if ($chunkAt -lt 0) { throw 'no "zlb" file data chunk found' }
# The chunk runs up to the header section, which begins with a 64 byte version
# identifier followed by the first block.
$chunkEnd = ($blocks | ForEach-Object { $_[0] } | Where-Object { $_ -gt $chunkAt } | Measure-Object -Minimum).Minimum
if ($chunkEnd) { $chunkEnd -= 64 } else { $chunkEnd = $bytes.Length }
Write-Host ("file data chunk at 0x{0:X}, {1:N0} bytes" -f $chunkAt, ($chunkEnd - $chunkAt))
$filesLzma = Join-Path $OutDir 'files.lzma'
[InnoSetup]::SaveChunk($bytes, $chunkAt, $chunkEnd, $filesLzma)

# 7-Zip reports an unexpected end of data because the stream has no end
# marker and its length was declared unknown. The bytes before that point are
# good, which is all this needs, so the exit code is deliberately ignored.
$filesBin = Join-Path $OutDir 'files.bin'
Start-Process -FilePath (Get-Command 7z).Source `
    -ArgumentList @('e', '-y', '-so', "`"$filesLzma`"") `
    -RedirectStandardOutput $filesBin -RedirectStandardError (Join-Path $OutDir '7z.err') `
    -NoNewWindow -Wait | Out-Null
Write-Host ("decompressed: {0:N0} bytes" -f (Get-Item $filesBin).Length)

$blob = [System.IO.File]::ReadAllBytes($filesBin)
$at = [InnoSetup]::FindUtf16($blob, '[Version]', 0)
if ($at -lt 0) { throw 'no INF found in the extracted data' }

# Back up to the start of the comment block above [Version], and stop at the
# first byte that is not part of the UTF-16 text.
$start = $at
while ($start -gt 1 -and $blob[$start - 1] -eq 0 -and
       ($blob[$start - 2] -ge 9 -and $blob[$start - 2] -lt 127)) { $start -= 2 }
$end = $at
while ($end -lt $blob.Length - 1 -and $blob[$end + 1] -eq 0 -and
       ($blob[$end] -ge 9 -and $blob[$end] -lt 127)) { $end += 2 }

$inf = [System.Text.Encoding]::Unicode.GetString($blob, $start, $end - $start)
$infPath = Join-Path $OutDir 'msUsbDisplayDriver.inf'
[System.IO.File]::WriteAllText($infPath, $inf, (New-Object System.Text.UTF8Encoding $false))
Write-Host ''
Write-Host "vendor INF -> $infPath"
Write-Host ''
Write-Output $inf
