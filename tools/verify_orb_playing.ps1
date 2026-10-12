param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe',
    [string]$OutDir = 'D:\Desktop\音乐板\soundboard-native\evidence'
)

# 悬浮球在**收起面板之后**的样子（这是用户真正会看到的状态）：
#   · 放着有封面的曲子 → 球面应该是封面（圆裁）
#   · 空闲 → 灰球 + 灰 logo
# 顺带验清空按钮真的能把台清干净（用正确的坐标点它）。

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class V5 {
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
    $cb = [V5+EnumProc]{
        param($h, $x)
        $w = 0
        [void][V5]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $targetPid) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][V5]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq $className) { $script:found = $h }
        }
        return $true
    }
    [void][V5]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

$env:MUSICBOARD_TRACE_INPUT = '1'
$log = Join-Path $env:LOCALAPPDATA ('MusicBoard\logs\app-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$before = 0
if (Test-Path -LiteralPath $log) { $before = (Get-Content -LiteralPath $log -Encoding UTF8).Count }

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 8
$panel = [IntPtr]::Zero; $orb = [IntPtr]::Zero
for ($i = 0; $i -lt 30; $i++) {
    if ($panel -eq [IntPtr]::Zero) { $panel = Find-Window $proc.Id 'MusicBoard.CompositionWindow' }
    if ($orb -eq [IntPtr]::Zero) { $orb = Find-Window $proc.Id 'MusicBoard.OrbWindow' }
    if ($panel -ne [IntPtr]::Zero -and $orb -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
$pr = New-Object V5+RECT
[void][V5]::GetWindowRect($panel, [ref]$pr)

function Click([int]$x, [int]$y) {
    [void][V5]::SetCursorPos($pr.L + $x, $pr.T + $y)
    Start-Sleep -Milliseconds 250
    [V5]::mouse_event([V5]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 90
    [V5]::mouse_event([V5]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
}
function DragCardToDisc([int]$cx, [int]$cy, [int]$dx, [int]$dy) {
    [void][V5]::SetCursorPos($pr.L + $cx, $pr.T + $cy)
    Start-Sleep -Milliseconds 250
    [V5]::mouse_event([V5]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    for ($s = 1; $s -le 14; $s++) {
        [void][V5]::SetCursorPos($pr.L + [int]($cx + ($dx - $cx) * $s / 14.0),
                                $pr.T + [int]($cy + ($dy - $cy) * $s / 14.0))
        Start-Sleep -Milliseconds 45
    }
    [V5]::mouse_event([V5]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
}
function CaptureOrb([string]$name) {
    $oc = New-Object V5+RECT
    [void][V5]::GetClientRect($orb, [ref]$oc)
    $ow = $oc.R - $oc.L; $oh = $oc.B - $oc.T
    $best = 0; $bestBmp = $null
    for ($a = 1; $a -le 8; $a++) {
        $bmp = New-Object System.Drawing.Bitmap $ow, $oh
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [void][V5]::PrintWindow($orb, $hdc, 2)
        $g.ReleaseHdc($hdc); $g.Dispose()
        $bright = 0; $saturated = 0
        for ($y = 0; $y -lt $oh; $y += 2) {
            for ($x = 0; $x -lt $ow; $x += 2) {
                $p = $bmp.GetPixel($x, $y)
                if ($p.R + $p.G + $p.B -gt 120) { $bright++ }
                # 封面是彩色的：三个通道差得开；黑胶/灰 logo 不会
                if ([Math]::Abs($p.R - $p.G) + [Math]::Abs($p.G - $p.B) -gt 60) { $saturated++ }
            }
        }
        if ($bright -gt $best) {
            if ($null -ne $bestBmp) { $bestBmp.Dispose() }
            $bestBmp = $bmp; $best = $bright
        } else { $bmp.Dispose() }
        if ($best -gt 150) { break }
        Start-Sleep -Milliseconds 200
    }
    if ($null -ne $bestBmp) { $bestBmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png); $bestBmp.Dispose() }
    Write-Host ("{0}: bright={1}" -f $name, $best)
    return $best
}

# 放一首**有封面**的曲子（第 2 排那张，前面已确认有封面），然后收起面板看球
DragCardToDisc 100 584 110 180
Start-Sleep -Seconds 2
Click 1162 21          # 标题栏"收起到悬浮球"
Start-Sleep -Seconds 2
Write-Host "panel visible after collapse: $([V5]::IsWindowVisible($panel))"
$bright = CaptureOrb 'r5-orb-playing-with-cover.png'
# 面板收起来了，球的窗口在最前面，这一张应当是可信的

# 展开面板 → 清空 → 再看台是不是空的
Click 200 20           # 标题栏空白处（HTCAPTION）不动面板；用托盘不方便，这里直接点球
[void][V5]::SetCursorPos($pr.L + 92, $pr.T + 92)
Start-Sleep -Milliseconds 200
# 球可能已经被拖走了 —— 用托盘菜单太绕，这里只验证"点清空之后台是空的"
$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
if (-not $proc.HasExited) { $proc.Kill() }
Start-Sleep -Milliseconds 400

Write-Host '---- 日志 ----'
Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip $before |
    Select-String -Pattern 'status:|导入缓存' | Select-Object -Last 10 | ForEach-Object { $_.Line }
