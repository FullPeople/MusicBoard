param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe',
    [string]$OutDir = 'D:\Desktop\音乐板\soundboard-native\evidence'
)

# 收尾复核：
#   ① 先放 A 再放 B —— B 的进度必须从 0 附近开始（位置基准重置了）
#   ② 点"清空"—— 唱片台回到空闲，音波图不残留
#   ③ 底栏显示预热进度（不是状态胶囊）
#
# 卡片行位置是扫出来的：y = 480/532/588/640/692（行高 48 + 缝 8）。

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class V7 {
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
    $cb = [V7+EnumProc]{
        param($h, $x)
        $w = 0
        [void][V7]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $targetPid) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][V7]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq $className) { $script:found = $h }
        }
        return $true
    }
    [void][V7]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

$env:MUSICBOARD_TRACE_INPUT = '1'
$log = Join-Path $env:LOCALAPPDATA ('MusicBoard\logs\app-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$before = 0
if (Test-Path -LiteralPath $log) { $before = (Get-Content -LiteralPath $log -Encoding UTF8).Count }

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 8
$panel = [IntPtr]::Zero
for ($i = 0; $i -lt 30 -and $panel -eq [IntPtr]::Zero; $i++) {
    $panel = Find-Window $proc.Id 'MusicBoard.CompositionWindow'
    if ($panel -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
$pr = New-Object V7+RECT
[void][V7]::GetWindowRect($panel, [ref]$pr)
$pc = New-Object V7+RECT
[void][V7]::GetClientRect($panel, [ref]$pc)
$pw = $pc.R - $pc.L; $ph = $pc.B - $pc.T
Write-Host "panel at $($pr.L),$($pr.T) size ${pw}x${ph}"

function CapturePanel([string]$name) {
    $bmp = New-Object System.Drawing.Bitmap $pw, $ph
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][V7]::PrintWindow($panel, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "captured $name"
}
function DragCardToDisc([int]$cx, [int]$cy) {
    [void][V7]::SetCursorPos($pr.L + $cx, $pr.T + $cy)
    Start-Sleep -Milliseconds 250
    [V7]::mouse_event([V7]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    for ($s = 1; $s -le 14; $s++) {
        [void][V7]::SetCursorPos($pr.L + [int]($cx + (110 - $cx) * $s / 14.0),
                                $pr.T + [int]($cy + (180 - $cy) * $s / 14.0))
        Start-Sleep -Milliseconds 45
    }
    [V7]::mouse_event([V7]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
}
function Click([int]$x, [int]$y) {
    [void][V7]::SetCursorPos($pr.L + $x, $pr.T + $y)
    Start-Sleep -Milliseconds 250
    [V7]::mouse_event([V7]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 90
    [V7]::mouse_event([V7]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 600
}

# ① A -> 等 3 秒 -> B，B 的进度应当接近 0
DragCardToDisc 100 504
Start-Sleep -Seconds 3
CapturePanel 'r6-01-track-a.png'
DragCardToDisc 100 556
Start-Sleep -Milliseconds 1200
CapturePanel 'r6-02-track-b-early.png'

# ② 清空（清空按钮在列头：x 约 120..144，y 约 57..76）
Click 132 66
Start-Sleep -Milliseconds 800
CapturePanel 'r6-03-after-clear.png'

$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 1
if (-not $proc.HasExited) { $proc.Kill() }
Start-Sleep -Milliseconds 400

Write-Host '---- 日志判据 ----'
Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip $before |
    Select-String -Pattern 'status:' | Select-Object -Last 12 | ForEach-Object { $_.Line }
