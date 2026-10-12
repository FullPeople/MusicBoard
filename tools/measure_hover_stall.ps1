param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Debug\musicboard.exe'
)

# 量"悬停卡不卡"：拿真光标在**曲目卡网格**上密集移动（每 40ms 一格），
# 再从程序日志里读相邻两次鼠标消息之间的毫秒间隔。
#
# 为什么要这么量：悬停延迟的根因是"在鼠标消息里同步解码整首歌"，
# 那是 100~300ms 级的停顿。截图看不出来，秒级时间戳也看不出来 ——
# 只有毫秒级的相邻消息间隔能指出来。脚本每 40ms 挪一次，所以
# 正常的间隔应当在 40~80ms；出现几百毫秒就是卡了。

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class M {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder b, int m);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
}
"@

$log = Join-Path $env:LOCALAPPDATA ('MusicBoard\logs\app-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$before = 0
if (Test-Path -LiteralPath $log) { $before = (Get-Content -LiteralPath $log -Encoding UTF8).Count }

$env:MUSICBOARD_TRACE_INPUT = '1'
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 7
if ($proc.HasExited) { Write-Host 'exited early'; exit 1 }

# 面板在屏幕 0,0（无边框窗口），直接按客户区坐标走
# 左列卡片网格：x 40..700，y 480..760，密集横扫 —— 这一片全是没悬停过的卡片，
# 正是"同步解码"会卡住的地方
foreach ($pass in 1..2) {
    foreach ($y in @(500, 560, 620, 680, 740)) {
        for ($x = 60; $x -le 700; $x += 40) {
            [void][M]::SetCursorPos($x, $y)
            Start-Sleep -Milliseconds 40
        }
    }
}
Start-Sleep -Milliseconds 500
$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
if (-not $proc.HasExited) { $proc.Kill() }
Start-Sleep -Milliseconds 300

Write-Host '---- 逐条间隔（毫秒）----'
$lines = Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip $before
$stamps = @()
foreach ($line in $lines) {
    if ($line -match '^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\s+\[info\] move:') {
        $stamps += [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss.fff', $null)
    }
}
Write-Host "样本: $($stamps.Count)"
if ($stamps.Count -lt 20) { Write-Host '样本太少，不做结论'; exit 2 }
$gaps = @()
for ($i = 1; $i -lt $stamps.Count; $i++) {
    $gaps += ($stamps[$i] - $stamps[$i - 1]).TotalMilliseconds
}
$sorted = $gaps | Sort-Object
$median = $sorted[[int]($sorted.Count / 2)]
$p95 = $sorted[[int]($sorted.Count * 0.95)]
$worst = $sorted[-1]
Write-Host ("中位间隔 {0:N0}ms   95分位 {1:N0}ms   最大 {2:N0}ms" -f $median, $p95, $worst)
$stalls = @($gaps | Where-Object { $_ -gt 250 })
Write-Host ("超过 250ms 的间隔: {0} 次" -f $stalls.Count)
if ($stalls.Count -gt 0) {
    Write-Host ("最长几次: " + (($stalls | Sort-Object -Descending | Select-Object -First 5) -join ', '))
}
if ($p95 -lt 200) {
    Write-Host 'RESULT 悬停没有明显停顿（95 分位仍在 200ms 内）'
} else {
    Write-Host 'RESULT 存在停顿，需要继续排查'
}
