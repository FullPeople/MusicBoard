param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe',
    [string]$OutDir = 'D:\Desktop\音乐板\soundboard-native\evidence'
)

# 第四轮那批改动的实机复核：
#   ① 清空按钮不再盖住 BGM/SFX 字样
#   ② 音效列的拖拽幽灵宽度 = 音效卡宽度（不再"过长"）
#   ③ 悬浮球显示封面（有封面时）
#   ④ 放一首之后再放另一首，两首的进度都对（位置基准被重置了）
#   ⑤ 清空之后唱片台真的回到空闲（音波图不残留）

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class V4 {
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
    $cb = [V4+EnumProc]{
        param($h, $x)
        $w = 0
        [void][V4]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $targetPid) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][V4]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq $className) { $script:found = $h }
        }
        return $true
    }
    [void][V4]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

$env:MUSICBOARD_TRACE_INPUT = '1'
$log = Join-Path $env:LOCALAPPDATA ('MusicBoard\logs\app-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$before = 0
if (Test-Path -LiteralPath $log) { $before = (Get-Content -LiteralPath $log -Encoding UTF8).Count }

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 8
if ($proc.HasExited) { Write-Host 'exited early'; exit 1 }

$panel = [IntPtr]::Zero; $orb = [IntPtr]::Zero
for ($i = 0; $i -lt 30; $i++) {
    if ($panel -eq [IntPtr]::Zero) { $panel = Find-Window $proc.Id 'MusicBoard.CompositionWindow' }
    if ($orb -eq [IntPtr]::Zero) { $orb = Find-Window $proc.Id 'MusicBoard.OrbWindow' }
    if ($panel -ne [IntPtr]::Zero -and $orb -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
$pr = New-Object V4+RECT
[void][V4]::GetWindowRect($panel, [ref]$pr)
$pc = New-Object V4+RECT
[void][V4]::GetClientRect($panel, [ref]$pc)
$pw = $pc.R - $pc.L; $ph = $pc.B - $pc.T

function CapturePanel([string]$name) {
    $bmp = New-Object System.Drawing.Bitmap $pw, $ph
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][V4]::PrintWindow($panel, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "captured $name"
}

function CaptureOrb([string]$name) {
    $oc = New-Object V4+RECT
    [void][V4]::GetClientRect($orb, [ref]$oc)
    $ow = $oc.R - $oc.L; $oh = $oc.B - $oc.T
    $best = 0; $bestBmp = $null
    for ($a = 1; $a -le 6; $a++) {
        $bmp = New-Object System.Drawing.Bitmap $ow, $oh
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [void][V4]::PrintWindow($orb, $hdc, 2)
        $g.ReleaseHdc($hdc); $g.Dispose()
        $count = 0
        for ($y = 0; $y -lt $oh; $y += 2) {
            for ($x = 0; $x -lt $ow; $x += 2) {
                $p = $bmp.GetPixel($x, $y)
                # 有封面时盘面上会出现明显的亮色；纯黑说明这一帧是空的
                if ($p.R + $p.G + $p.B -gt 90) { $count++ }
            }
        }
        if ($count -gt $best) { if ($null -ne $bestBmp) { $bestBmp.Dispose() }; $bestBmp = $bmp; $best = $count }
        else { $bmp.Dispose() }
        if ($best -gt 100) { break }
        Start-Sleep -Milliseconds 150
    }
    if ($null -ne $bestBmp) { $bestBmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png); $bestBmp.Dispose() }
    Write-Host ("{0}: bright={1}" -f $name, $best)
}

function MoveTo([int]$x, [int]$y) { [void][V4]::SetCursorPos($pr.L + $x, $pr.T + $y); Start-Sleep -Milliseconds 200 }
function DragTo([int]$x1, [int]$y1, [int]$x2, [int]$y2) {
    [void][V4]::SetCursorPos($pr.L + $x1, $pr.T + $y1)
    Start-Sleep -Milliseconds 250
    [V4]::mouse_event([V4]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    for ($s = 1; $s -le 14; $s++) {
        [void][V4]::SetCursorPos($pr.L + [int]($x1 + ($x2 - $x1) * $s / 14.0),
                                $pr.T + [int]($y1 + ($y2 - $y1) * $s / 14.0))
        Start-Sleep -Milliseconds 45
    }
    Start-Sleep -Milliseconds 200
}
function LeftUp() { [V4]::mouse_event([V4]::LEFTUP, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 600 }

# ① ② 清空按钮位置 + 音效列拖拽幽灵宽度（拖的过程中截图）
MoveTo 880 500
DragTo 880 500 900 200
CapturePanel 'r4-01-sfx-ghost-width.png'
LeftUp
CapturePanel 'r4-02-after-sfx-drop.png'

# ③ 悬浮球显示封面：拖一张**有封面**的卡（第 2 排那张，前面确认过有封面）
DragTo 100 584 110 180
CapturePanel 'r4-03-bgm-with-cover.png'
LeftUp
Start-Sleep -Seconds 1
CaptureOrb 'r4-04-orb-with-cover.png'

# ④ 再放另一首，看进度是否从 0 开始（位置基准重置）
::Sleep(0)
Start-Sleep -Seconds 2
DragTo 320 500 110 180
LeftUp
Start-Sleep -Seconds 2
CapturePanel 'r4-05-second-track-progress.png'

# ⑤ 清空之后唱片台回到空闲
MoveTo 79 20
[void][V4]::SetCursorPos($pr.L + 79, $pr.T + 20)
Start-Sleep -Milliseconds 250
[V4]::mouse_event([V4]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 90
[V4]::mouse_event([V4]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Seconds 1
CapturePanel 'r4-06-after-clear.png'

$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
if (-not $proc.HasExited) { $proc.Kill() }
Start-Sleep -Milliseconds 400

Write-Host '---- 日志判据 ----'
Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip $before |
    Select-String -Pattern 'status:|导入缓存|缓存导入' | Select-Object -Last 14 | ForEach-Object { $_.Line }
