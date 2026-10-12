param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe',
    [string]$OutDir = 'D:\Desktop\音乐板\soundboard-native\evidence'
)

# 悬浮球到底画出来没有？
#
# 判据不是"窗口存在"（它一直存在），而是**窗口里真的有像素**：
# 抓球的窗口区域，数一下非透明像素。原来那段渲染回调是空的，
# 抓出来是全透明的 —— 用户说的"我的悬浮球呢"就是这么来的。

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class O {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
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
    $cb = [O+EnumProc]{
        param($h, $x)
        $w = 0
        [void][O]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $targetPid) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][O]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq $className) { $script:found = $h }
        }
        return $true
    }
    [void][O]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

function Count-Painted([System.Drawing.Bitmap]$bmp) {
    # 数"画上去的像素"：球的底色是暗色，所以看非全透明的像素
    $painted = 0
    for ($y = 0; $y -lt $bmp.Height; $y += 2) {
        for ($x = 0; $x -lt $bmp.Width; $x += 2) {
            $p = $bmp.GetPixel($x, $y)
            if ($p.A -gt 24) { $painted++ }
        }
    }
    return $painted
}

function Grab([IntPtr]$hwnd, [int]$w, [int]$h, [string]$name) {
    # 抓 DComp 合成的窗口是**有竞态**的：它每帧换一块合成表面，
    # 正巧撞在换的那一下就会抓到一张空的。所以重试几次取最好的那张 ——
    # 单次结果当证据会得出"球没画"这种错误结论（就是这么被坑过一次）。
    $best = 0
    $bestBmp = $null
    for ($attempt = 1; $attempt -le 6; $attempt++) {
        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        $ok = [O]::PrintWindow($hwnd, $hdc, 2)
        $g.ReleaseHdc($hdc); $g.Dispose()
        $count = Count-Painted $bmp
        if ($count -gt $best) {
            if ($null -ne $bestBmp) { $bestBmp.Dispose() }
            $bestBmp = $bmp
            $best = $count
        } else {
            $bmp.Dispose()
        }
        if ($best -gt 200) { break }
        Start-Sleep -Milliseconds 120
    }
    $path = Join-Path $OutDir $name
    if ($null -ne $bestBmp) {
        $bestBmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $bestBmp.Dispose()
    }
    Write-Host ("{0}: painted={1}" -f $name, $best)
    return $best
}

$env:MUSICBOARD_TRACE_INPUT = '1'
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 8
if ($proc.HasExited) { Write-Host 'exited early'; exit 1 }

$panel = [IntPtr]::Zero
$orb = [IntPtr]::Zero
for ($i = 0; $i -lt 30; $i++) {
    if ($panel -eq [IntPtr]::Zero) { $panel = Find-Window $proc.Id 'MusicBoard.CompositionWindow' }
    if ($orb -eq [IntPtr]::Zero) { $orb = Find-Window $proc.Id 'MusicBoard.OrbWindow' }
    if ($panel -ne [IntPtr]::Zero -and $orb -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
Write-Host "panel=$panel orb=$orb visible=$([O]::IsWindowVisible($orb))"
if ($orb -eq [IntPtr]::Zero) { Write-Host 'no orb window'; $proc.Kill(); exit 1 }

$orr = New-Object O+RECT
[void][O]::GetWindowRect($orb, [ref]$orr)
$orc = New-Object O+RECT
[void][O]::GetClientRect($orb, [ref]$orc)
$ow = $orc.R - $orc.L; $oh = $orc.B - $orc.T
Write-Host "orb rect=$($orr.L),$($orr.T) size=${ow}x${oh}"

# ① 先看球自己
$painted = Grab $orb $ow $oh 'orb-01-idle.png'

# ② 放一首 BGM，再看球（应该有封面/进度环/辉光）
$pr = New-Object O+RECT
[void][O]::GetWindowRect($panel, [ref]$pr)
# 第 1 排卡片拖到 BGM 唱片（坐标来自前面的网格扫描）
$cardX = 100; $cardY = 500; $discX = 110; $discY = 180
[void][O]::SetCursorPos($pr.L + $cardX, $pr.T + $cardY)
Start-Sleep -Milliseconds 250
[O]::mouse_event([O]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 200
for ($s = 1; $s -le 12; $s++) {
    [void][O]::SetCursorPos($pr.L + [int]($cardX + ($discX - $cardX) * $s / 12.0),
                            $pr.T + [int]($cardY + ($discY - $cardY) * $s / 12.0))
    Start-Sleep -Milliseconds 45
}
[O]::mouse_event([O]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Seconds 2
$paintedPlaying = Grab $orb $ow $oh 'orb-02-playing.png'

# ③ 收起面板（标题栏第三个按钮）→ 球要保持可见
$rx = $pr.L + 1162; $ry = $pr.T + 21
[void][O]::SetCursorPos($rx, $ry)
Start-Sleep -Milliseconds 300
[O]::mouse_event([O]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 90
[O]::mouse_event([O]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Seconds 2
$panelVisible = [O]::IsWindowVisible($panel)
Write-Host "panel visible after collapse: $panelVisible"
$paintedCollapsed = Grab $orb $ow $oh 'orb-03-collapsed.png'

$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
if (-not $proc.HasExited) { $proc.Kill() }

Write-Host ''
if ($painted -gt 200 -and $paintedPlaying -gt 200) {
    Write-Host ('RESULT 悬浮球画出来了（空闲 {0} 个上色像素，播放中 {1} 个）' -f $painted, $paintedPlaying)
} else {
    Write-Host ('RESULT 球面几乎没像素 —— 没画出来（{0} / {1}）' -f $painted, $paintedPlaying)
}
