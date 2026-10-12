param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe'
)

# 单实例守卫的实机验证。
#
# 要证明的是两件事，缺一不可：
#   1) 第二次启动**不会**再起一套（进程退出，且没有第二个面板窗口）
#   2) 第二次启动会把已有实例的面板**叫到前面**（注册消息那条路真的通）
# 只验第 1 条不够：那可能只是"启动失败"而不是"守卫起作用"。

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class S {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder b, int m);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
  public delegate bool EnumProc(IntPtr h, IntPtr p);
}
"@

$log = Join-Path $env:LOCALAPPDATA ('MusicBoard\logs\app-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$before = 0
if (Test-Path -LiteralPath $log) { $before = (Get-Content -LiteralPath $log -Encoding UTF8).Count }

function Count-Panels {
    $count = 0
    $cb = [S+EnumProc]{
        param($h, $x)
        $sb = New-Object System.Text.StringBuilder 256
        [void][S]::GetClassNameW($h, $sb, 256)
        if ($sb.ToString() -eq 'MusicBoard.CompositionWindow' -and [S]::IsWindowVisible($h)) {
            $script:panelCount = $script:panelCount + 1
        }
        return $true
    }
    $script:panelCount = 0
    [void][S]::EnumWindows($cb, [IntPtr]::Zero)
    $count = $script:panelCount
    return $count
}

$first = Start-Process -FilePath $Exe -PassThru
Write-Host "first pid=$($first.Id)"
Start-Sleep -Seconds 8
$panelsAfterFirst = Count-Panels
Write-Host "visible panels after first start: $panelsAfterFirst"

# 把第一个实例的面板收起来 —— 必须走**程序自己的**收起路径（标题栏第三个按钮
# 是"收进通知区域"），不能从外面 ShowWindow(SW_HIDE)。
# 从外面藏的话，程序内部还认为面板是显示状态，于是它会对"呼出"这个消息
# 直接早退 —— 那是测试造了一个真实用户到不了的状态，得到的结论也就是假的。
$hideButtonX = 1162
$hideButtonY = 21
[void][S]::SetCursorPos($hideButtonX, $hideButtonY)
Start-Sleep -Milliseconds 300
[S]::mouse_event([S]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 90
[S]::mouse_event([S]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Seconds 1
$panelsAfterHide = Count-Panels
Write-Host "visible panels after the app's own hide button: $panelsAfterHide"

$second = Start-Process -FilePath $Exe -PassThru
Write-Host "second pid=$($second.Id)"
Start-Sleep -Seconds 6
$secondExited = $second.HasExited
$secondCode = if ($secondExited) { $second.ExitCode } else { -1 }
Write-Host "second exited=$secondExited code=$secondCode"

$panelsAfterSecond = Count-Panels
Write-Host "visible panels after second start: $panelsAfterSecond"

$running = @(Get-Process -Name musicboard -ErrorAction SilentlyContinue)
Write-Host "musicboard processes still running: $($running.Count)"

Write-Host '---- 日志（这一段是判据）----'
if (Test-Path -LiteralPath $log) {
    Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip $before |
        Select-String -Pattern '已有实例|呼出|启动' | Select-Object -Last 8 | ForEach-Object { $_.Line }
}

# 收尾：关掉第一个实例
if (-not $first.HasExited) { $first.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 2 }
if (-not $first.HasExited) { $first.Kill() }
foreach ($p in @(Get-Process -Name musicboard -ErrorAction SilentlyContinue)) { $p.Kill() }
Write-Host 'done'
