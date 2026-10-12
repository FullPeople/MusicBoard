param(
    [Parameter(Mandatory)][int]$Hwnd,
    [int]$W = 1256,
    [int]$H = 788,
    [string]$Out = ''
)

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Cap {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

# 用 PrintWindow(PW_RENDERFULLCONTENT) 而不是 CopyFromScreen：
# 这块面板是 DirectComposition 合成的**无重定向位图**窗口（WS_EX_NOREDIRECTIONBITMAP），
# BitBlt 那条路抓到的可能是陈旧内容 —— 实测就这么被误导过一次
# （以为面板不刷新，其实是截图看不见它）。
# PW_RENDERFULLCONTENT = 2 会请窗口把内容画进这个 DC，DComp 窗口能正确回应。
# 尺寸直接给：GetClientRect 通过 P/Invoke 取结构体在这个环境下拿到了全 0，
# 而窗口尺寸是已知的（1256x788），没必要为这件事绕。
$w = $W
$h = $H

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Cap]::PrintWindow([IntPtr]$Hwnd, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()

if (-not $ok) {
    # 有些驱动不支持，退回 CopyFromScreen（可能陈旧，但至少有一张图）
    Write-Host 'PrintWindow failed, falling back to CopyFromScreen'
    $g2 = [System.Drawing.Graphics]::FromImage($bmp)
    $g2.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
    $g2.Dispose()
}

if ([string]::IsNullOrWhiteSpace($Out)) {
    $Out = Join-Path $env:TEMP 'panel-print.png'
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $Out ($w x $h, printwindow=$ok)"
