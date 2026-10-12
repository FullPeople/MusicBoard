param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe',
    [string]$OutDir = 'D:\Desktop\音乐板\soundboard-native\evidence'
)

# 唱片在转吗？
#
# 抓同一个唱片的相邻两帧，比较**像素差异**。唱片角度由播放位置算出，
# 所以只要有区别就说明它在转；完全一样说明画的东西不随位置变（那就是"卡住"）。
# 这是唯一能自动判定"转没转"的办法 —— 一张静图看不出来。

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class R {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder b, int m);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
}
"@

$env:MUSICBOARD_TRACE_INPUT = '1'
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 7

$panel = [IntPtr]::Zero
for ($i = 0; $i -lt 30 -and $panel -eq [IntPtr]::Zero; $i++) {
    $script:found = [IntPtr]::Zero
    $cb = [R+EnumProc]{
        param($h, $x)
        $w = 0
        [void][R]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $proc.Id) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][R]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq 'MusicBoard.CompositionWindow') { $script:found = $h }
        }
        return $true
    }
    [void][R]::EnumWindows($cb, [IntPtr]::Zero)
    $panel = $script:found
    if ($panel -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
if ($panel -eq [IntPtr]::Zero) { Write-Host 'no panel'; $proc.Kill(); exit 1 }

$pr = New-Object R+RECT
[void][R]::GetWindowRect($panel, [ref]$pr)
$cr = New-Object R+RECT
[void][R]::GetClientRect($panel, [ref]$cr)
$ox = $pr.L; $oy = $pr.T; $w = $cr.R - $cr.L; $h = $cr.B - $cr.T

function GrabDisc([string]$name) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][R]::PrintWindow($panel, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $crop = New-Object System.Drawing.Bitmap 180, 180
    $gc = [System.Drawing.Graphics]::FromImage($crop)
    $gc.DrawImage($bmp, (New-Object System.Drawing.Rectangle 0, 0, 180, 180),
                  (New-Object System.Drawing.Rectangle 22, 100, 180, 180),
                  [System.Drawing.GraphicsUnit]::Pixel)
    $gc.Dispose()
    $crop.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $crop.Dispose()
    $bmp.Dispose()
    return (Join-Path $OutDir $name)
}

# 第 3 排那张有封面（前面已经确认过），拖到唱片上
$cardX = 100; $cardY = 500 + 2 * 84; $discX = 110; $discY = 180
[void][R]::SetCursorPos($ox + $cardX, $oy + $cardY)
Start-Sleep -Milliseconds 250
[R]::mouse_event([R]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 200
for ($s = 1; $s -le 12; $s++) {
    [void][R]::SetCursorPos($ox + [int]($cardX + ($discX - $cardX) * $s / 12.0),
                            $oy + [int]($cardY + ($discY - $cardY) * $s / 12.0))
    Start-Sleep -Milliseconds 45
}
[R]::mouse_event([R]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 1200

$f1 = GrabDisc 'rotate-t1.png'
Start-Sleep -Milliseconds 1300
$f2 = GrabDisc 'rotate-t2.png'
Start-Sleep -Milliseconds 1300
$f3 = GrabDisc 'rotate-t3.png'

$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
if (-not $proc.HasExited) { $proc.Kill() }

# 用 .NET 直接比像素：不同像素的比例就是"转了多少"的代理指标
Add-Type -AssemblyName System.Drawing
$a = New-Object System.Drawing.Bitmap $f1
$b = New-Object System.Drawing.Bitmap $f2
$c = New-Object System.Drawing.Bitmap $f3
$diff12 = 0; $diff13 = 0; $total = 0
for ($y = 0; $y -lt 180; $y += 3) {
    for ($x = 0; $x -lt 180; $x += 3) {
        $total++
        $pa = $a.GetPixel($x, $y); $pb = $b.GetPixel($x, $y); $pc = $c.GetPixel($x, $y)
        if ([Math]::Abs($pa.R - $pb.R) + [Math]::Abs($pa.G - $pb.G) + [Math]::Abs($pa.B - $pb.B) -gt 24) { $diff12++ }
        if ([Math]::Abs($pa.R - $pc.R) + [Math]::Abs($pa.G - $pc.G) + [Math]::Abs($pa.B - $pc.B) -gt 24) { $diff13++ }
    }
}
Write-Host ("t1 vs t2 变化像素: {0}/{1}" -f $diff12, $total)
Write-Host ("t1 vs t3 变化像素: {0}/{1}" -f $diff13, $total)
if ($diff12 -gt 40 -and $diff13 -gt 40) {
    Write-Host 'RESULT 唱片在转（相邻两帧的盘面像素不同）'
} else {
    Write-Host 'RESULT 盘面几乎没变 —— 需要排查（可能没在放，或绘制没跟上）'
}
