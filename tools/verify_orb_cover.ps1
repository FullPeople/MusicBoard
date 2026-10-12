param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe',
    [string]$OutDir = 'D:\Desktop\音乐板\soundboard-native\evidence'
)

# 悬浮球显示封面：逐排试，找到有封面的那一张，收起面板抓球。
#
# 卡片行的真实位置是**扫出来的**（y = 480 / 532 / 588 / 640 / 692，行高 48 + 缝 8），
# 不是猜的 —— 之前我按 84 的行距猜，落进了行与行之间的缝里，
# 于是"拖拽没反应"，还差点当成命中区的 bug。

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class V6 {
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

function Find-Window([int]$targetPid, [string]$className) {
    $script:found = [IntPtr]::Zero
    $cb = [V6+EnumProc]{
        param($h, $x)
        $w = 0
        [void][V6]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $targetPid) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][V6]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq $className) { $script:found = $h }
        }
        return $true
    }
    [void][V6]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 8
$panel = [IntPtr]::Zero; $orb = [IntPtr]::Zero
for ($i = 0; $i -lt 30; $i++) {
    if ($panel -eq [IntPtr]::Zero) { $panel = Find-Window $proc.Id 'MusicBoard.CompositionWindow' }
    if ($orb -eq [IntPtr]::Zero) { $orb = Find-Window $proc.Id 'MusicBoard.OrbWindow' }
    if ($panel -ne [IntPtr]::Zero -and $orb -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
$pr = New-Object V6+RECT
[void][V6]::GetWindowRect($panel, [ref]$pr)
$pc = New-Object V6+RECT
[void][V6]::GetClientRect($panel, [ref]$pc)
$pw = $pc.R - $pc.L; $ph = $pc.B - $pc.T

function GrabPixels([IntPtr]$hwnd, [int]$w, [int]$h) {
    $best = 0; $bestBmp = $null
    for ($a = 1; $a -le 8; $a++) {
        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [void][V6]::PrintWindow($hwnd, $hdc, 2)
        $g.ReleaseHdc($hdc); $g.Dispose()
        $sat = 0
        for ($y = 4; $y -lt $h - 4; $y += 2) {
            for ($x = 4; $x -lt $w - 4; $x += 2) {
                $p = $bmp.GetPixel($x, $y)
                # 封面是彩色的：通道之间差得开
                if ([Math]::Abs($p.R - $p.G) + [Math]::Abs($p.G - $p.B) -gt 60) { $sat++ }
            }
        }
        if ($sat -gt $best) { if ($null -ne $bestBmp) { $bestBmp.Dispose() }; $bestBmp = $bmp; $best = $sat }
        else { $bmp.Dispose() }
        if ($best -gt 60) { break }
        Start-Sleep -Milliseconds 150
    }
    return @{ Bmp = $bestBmp; Sat = $best }
}

function DragCardToDisc([int]$cx, [int]$cy, [int]$dx, [int]$dy) {
    [void][V6]::SetCursorPos($pr.L + $cx, $pr.T + $cy)
    Start-Sleep -Milliseconds 250
    [V6]::mouse_event([V6]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    for ($s = 1; $s -le 14; $s++) {
        [void][V6]::SetCursorPos($pr.L + [int]($cx + ($dx - $cx) * $s / 14.0),
                                $pr.T + [int]($cy + ($dy - $cy) * $s / 14.0))
        Start-Sleep -Milliseconds 45
    }
    [V6]::mouse_event([V6]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 900
}
function Click([int]$x, [int]$y) {
    [void][V6]::SetCursorPos($pr.L + $x, $pr.T + $y)
    Start-Sleep -Milliseconds 250
    [V6]::mouse_event([V6]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 90
    [V6]::mouse_event([V6]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
}

# 卡片行中心（扫出来的行起点 + 24）
$rows = 504, 556, 612, 664
$disc = @(110, 180)
$found = 0
foreach ($ry in $rows) {
    DragCardToDisc 100 $ry $disc[0] $disc[1]
    Start-Sleep -Seconds 1
    # 面板上的唱片：有封面就是彩色的
    $bmp = New-Object System.Drawing.Bitmap 200, 200
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][V6]::PrintWindow($panel, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $crop = New-Object System.Drawing.Bitmap 200, 200
    $gc = [System.Drawing.Graphics]::FromImage($crop)
    $gc.DrawImage($bmp, (New-Object System.Drawing.Rectangle 0, 0, 200, 200),
                  (New-Object System.Drawing.Rectangle 15, 90, 200, 200),
                  [System.Drawing.GraphicsUnit]::Pixel)
    $gc.Dispose()
    $sat = 0
    for ($y = 0; $y -lt 200; $y += 3) {
        for ($x = 0; $x -lt 200; $x += 3) {
            $p = $crop.GetPixel($x, $y)
            if ([Math]::Abs($p.R - $p.G) + [Math]::Abs($p.G - $p.B) -gt 60) { $sat++ }
        }
    }
    $crop.Save((Join-Path $OutDir ("rc-row-{0}.png" -f $ry)), [System.Drawing.Imaging.ImageFormat]::Png)
    $crop.Dispose(); $bmp.Dispose()
    Write-Host ("row y={0}: 唱片彩色像素={1}" -f $ry, $sat)
    if ($sat -gt 40) {
        $found = $ry
        # 收起面板，看球
        Click 1162 21
        Start-Sleep -Seconds 2
        $oc = New-Object V6+RECT
        [void][V6]::GetClientRect($orb, [ref]$oc)
        $res = GrabPixels $orb ($oc.R - $oc.L) ($oc.B - $oc.T)
        if ($null -ne $res.Bmp) {
            $res.Bmp.Save((Join-Path $OutDir 'rc-orb-with-cover.png'), [System.Drawing.Imaging.ImageFormat]::Png)
            $res.Bmp.Dispose()
        }
        Write-Host ("  → 收起面板后球面的彩色像素={0}（有封面应当明显大于 0）" -f $res.Sat)
        break
    }
}
if ($found -eq 0) { Write-Host '这几排都没有封面，换几排再试' }

$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 1
if (-not $proc.HasExited) { $proc.Kill() }
