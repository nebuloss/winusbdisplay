# SPDX-License-Identifier: GPL-2.0-only
#
# Extends the Windows desktop onto all attached displays, which is what makes
# an arrived indirect monitor actually become an active screen.

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class DisplayConfig {
  [DllImport("user32.dll")]
  public static extern int SetDisplayConfig(
      uint numPathArrayElements, IntPtr pathArray,
      uint numModeInfoArrayElements, IntPtr modeInfoArray, uint flags);

  [DllImport("user32.dll")]
  public static extern int GetDisplayConfigBufferSizes(
      uint flags, out uint numPath, out uint numMode);

  public const uint SDC_APPLY           = 0x00000080;
  public const uint SDC_TOPOLOGY_EXTEND = 0x00000004;
  public const uint SDC_ALLOW_CHANGES   = 0x00000400;
  public const uint QDC_ALL_PATHS       = 2;
}
"@

$paths = 0; $modes = 0
[DisplayConfig]::GetDisplayConfigBufferSizes([DisplayConfig]::QDC_ALL_PATHS, [ref]$paths, [ref]$modes) | Out-Null
Write-Output ("before: paths=$paths modes=$modes")

$flags = [DisplayConfig]::SDC_APPLY -bor [DisplayConfig]::SDC_TOPOLOGY_EXTEND
$r = [DisplayConfig]::SetDisplayConfig(0, [IntPtr]::Zero, 0, [IntPtr]::Zero, $flags)
Write-Output ("SetDisplayConfig(extend) -> $r  (0 = success)")

Start-Sleep -Seconds 3

[DisplayConfig]::GetDisplayConfigBufferSizes([DisplayConfig]::QDC_ALL_PATHS, [ref]$paths, [ref]$modes) | Out-Null
Write-Output ("after:  paths=$paths modes=$modes")

Add-Type -AssemblyName System.Windows.Forms
Write-Output "screens:"
[System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
    Write-Output ("  " + $_.DeviceName + " " + $_.Bounds.Width + "x" + $_.Bounds.Height +
                  " at (" + $_.Bounds.X + "," + $_.Bounds.Y + ") primary=" + $_.Primary)
}
