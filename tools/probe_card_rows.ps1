param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe'
)

# 卡片行到底在哪、有没有命中区？
# 竖直扫一条线（x=100），把每一段的命中 id 打出来 —— 这是"问程序"。
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class S4 {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder b, int m);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
}
"@

$log = Join-Path $env:LOCALAPPDATA ('MusicBoard\logs\app-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$before = 0
if (Test-Path -LiteralPath $log) { $before = (Get-Content -LiteralPath $log -Encoding UTF8).Count }

$env:MUSICBOARD_TRACE_INPUT = '1'
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 8
# **窗口可能不在 (0,0)**（标题栏拖动会把它挪走，位置还记在设置里）。
# 一律按窗口矩形换算 —— 之前写死绝对坐标，于是光标落在桌面上了，
# 探针什么都没收到，还以为是"第二条卡片行没有命中区"。
$panel = [IntPtr]::Zero
for ($i = 0; $i -lt 30 -and $panel -eq [IntPtr]::Zero; $i++) {
    $script:found = [IntPtr]::Zero
    $cb = [S4+EnumProc]{
        param($h, $x)
        $w = 0
        [void][S4]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $proc.Id) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][S4]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq 'MusicBoard.CompositionWindow') { $script:found = $h }
        }
        return $true
    }
    [void][S4]::EnumWindows($cb, [IntPtr]::Zero)
    $panel = $script:found
    if ($panel -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
if ($panel -eq [IntPtr]::Zero) { Write-Host 'no panel'; $proc.Kill(); exit 1 }
$pr = New-Object S4+RECT
[void][S4]::GetWindowRect($panel, [ref]$pr)
Write-Host "panel at $($pr.L),$($pr.T)"
for ($y = 460; $y -le 760; $y += 4) {
    [void][S4]::SetCursorPos($pr.L + 100, $pr.T + $y)
    Start-Sleep -Milliseconds 35
}
Start-Sleep -Milliseconds 500
$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
if (-not $proc.HasExited) { $proc.Kill() }
Start-Sleep -Milliseconds 300

$lines = Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip $before
$prev = $null
foreach ($line in $lines) {
    if ($line -match '\] move: 客户区 100,(\d+) .*id=(-?\d+)') {
        $y = [int]$Matches[1]; $id = [int]$Matches[2]
        if ($id -ne $prev) {
            Write-Host ("  y={0,4}  id={1}" -f $y, $id)
            $prev = $id
        }
    }
}
