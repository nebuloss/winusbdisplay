# SPDX-License-Identifier: GPL-2.0-only
#
# Probes every monitor with the Windows Monitor Configuration API, which is
# exactly what Twinkle Tray does. Run from the interactive session, not
# elevated, so it sees the real desktop.

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct PHYSICAL_MONITOR {
  public IntPtr hPhysicalMonitor;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
  public string szPhysicalMonitorDescription;
}

public class Ddc {
  [DllImport("user32.dll")]
  public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr rect, MonitorEnumProc proc, IntPtr data);
  public delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, IntPtr rect, IntPtr data);

  [DllImport("dxva2.dll", SetLastError = true)]
  public static extern bool GetNumberOfPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, ref uint count);

  [DllImport("dxva2.dll", SetLastError = true)]
  public static extern bool GetPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, uint count, [Out] PHYSICAL_MONITOR[] monitors);

  [DllImport("dxva2.dll", SetLastError = true)]
  public static extern bool GetVCPFeatureAndVCPFeatureReply(IntPtr h, byte code, IntPtr type, ref uint current, ref uint max);

  [DllImport("dxva2.dll", SetLastError = true)]
  public static extern bool SetVCPFeature(IntPtr h, byte code, uint value);

  [DllImport("dxva2.dll", SetLastError = true)]
  public static extern bool GetCapabilitiesStringLength(IntPtr h, ref uint len);

  [DllImport("dxva2.dll", SetLastError = true)]
  public static extern bool CapabilitiesRequestAndCapabilitiesReply(IntPtr h, StringBuilder str, uint len);

  [DllImport("dxva2.dll", SetLastError = true)]
  public static extern bool DestroyPhysicalMonitors(uint count, [In] PHYSICAL_MONITOR[] monitors);
}
"@

$handles = New-Object System.Collections.ArrayList
$cb = [Ddc+MonitorEnumProc] { param($h, $dc, $r, $d) [void]$handles.Add($h); return $true }
[void][Ddc]::EnumDisplayMonitors([IntPtr]::Zero, [IntPtr]::Zero, $cb, [IntPtr]::Zero)

Write-Output ("HMONITORs found: " + $handles.Count)

foreach ($h in $handles) {
    $count = 0
    if (-not [Ddc]::GetNumberOfPhysicalMonitorsFromHMONITOR($h, [ref]$count)) {
        Write-Output "  (no physical monitors for this HMONITOR)"
        continue
    }
    $mons = New-Object PHYSICAL_MONITOR[] $count
    if (-not [Ddc]::GetPhysicalMonitorsFromHMONITOR($h, $count, $mons)) {
        Write-Output "  GetPhysicalMonitorsFromHMONITOR failed"
        continue
    }

    foreach ($m in $mons) {
        Write-Output ""
        Write-Output ("monitor: " + $m.szPhysicalMonitorDescription)

        $len = 0
        if ([Ddc]::GetCapabilitiesStringLength($m.hPhysicalMonitor, [ref]$len) -and $len -gt 0) {
            $sb = New-Object System.Text.StringBuilder ([int]$len)
            if ([Ddc]::CapabilitiesRequestAndCapabilitiesReply($m.hPhysicalMonitor, $sb, $len)) {
                Write-Output ("  capabilities: " + $sb.ToString())
            } else {
                Write-Output ("  capabilities request FAILED err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
            }
        } else {
            Write-Output ("  capabilities length FAILED err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }

        $cur = 0; $max = 0
        if ([Ddc]::GetVCPFeatureAndVCPFeatureReply($m.hPhysicalMonitor, 0x10, [IntPtr]::Zero, [ref]$cur, [ref]$max)) {
            Write-Output ("  VCP 0x10 brightness: current=$cur max=$max")
            $probe = if ($cur -ge 50) { $cur - 20 } else { $cur + 20 }
            if ([Ddc]::SetVCPFeature($m.hPhysicalMonitor, 0x10, $probe)) {
                Write-Output ("  SetVCPFeature(0x10,$probe) ok")
                Start-Sleep -Milliseconds 700
                [void][Ddc]::SetVCPFeature($m.hPhysicalMonitor, 0x10, $cur)
            } else {
                Write-Output ("  SetVCPFeature FAILED err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
            }
        } else {
            Write-Output ("  VCP 0x10 read FAILED err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }
    }
    [void][Ddc]::DestroyPhysicalMonitors($count, $mons)
}
