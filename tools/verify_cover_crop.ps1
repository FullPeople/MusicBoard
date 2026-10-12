param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe',
    [string]$OutDir = 'D:\Desktop\音乐板\soundboard-native\evidence',
    [int]$FirstRow = 1,
    [int]$Rows = 5
)

# 验证"封面按圆形裁剪 + 跟着转动"。
#
# 曲库里 52% 的文件带封面，但**第一首恰好没有** —— 所以不能只拖第一张就下结论。
# 这里依次把第 1..N 排的卡片拖到 BGM 唱片上，各抓一张，由人来挑出有封面的那张。
#
# 判据（都来自程序自己的输入日志）：
#   drop: dropped=100 ...  —— 确实落到唱片上了
#   抓到的图里封面是**圆**的、且盘缘仍是黑胶

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class C {
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

$log = Join-Path $env:LOCALAPPDATA ('MusicBoard\logs\app-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$before = 0
if (Test-Path -LiteralPath $log) { $before = (Get-Content -LiteralPath $log -Encoding UTF8).Count }

$env:MUSICBOARD_TRACE_INPUT = '1'
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 7
if ($proc.HasExited) { Write-Host 'exited early'; exit 1 }

$panel = [IntPtr]::Zero
for ($i = 0; $i -lt 30 -and $panel -eq [IntPtr]::Zero; $i++) {
    $script:found = [IntPtr]::Zero
    $cb = [C+EnumProc]{
        param($h, $x)
        $w = 0
        [void][C]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $proc.Id) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][C]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq 'MusicBoard.CompositionWindow') { $script:found = $h }
        }
        return $true
    }
    [void][C]::EnumWindows($cb, [IntPtr]::Zero)
    $panel = $script:found
    if ($panel -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
if ($panel -eq [IntPtr]::Zero) { Write-Host 'no panel'; $proc.Kill(); exit 1 }

$pr = New-Object C+RECT
[void][C]::GetWindowRect($panel, [ref]$pr)
$cr = New-Object C+RECT
[void][C]::GetClientRect($panel, [ref]$cr)
$ox = $pr.L; $oy = $pr.T; $w = $cr.R - $cr.L; $h = $cr.B - $cr.T
Write-Host "panel $w x $h at $ox,$oy"

function Capture([string]$name) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][C]::PrintWindow($panel, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "captured $name"
}

# 第一张卡与唱片的位置由调用方给（默认值来自前面的网格扫描）
$discX = 110; $discY = 180
for ($row = $FirstRow; $row -lt $FirstRow + $Rows; $row++) {
    $cardX = 100
    $cardY = 500 + ($row - 1) * 84
    [void][C]::SetCursorPos($ox + $cardX, $oy + $cardY)
    Start-Sleep -Milliseconds 250
    [C]::mouse_event([C]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    for ($s = 1; $s -le 12; $s++) {
        $mx = [int]($cardX + ($discX - $cardX) * $s / 12.0)
        $my = [int]($cardY + ($discY - $cardY) * $s / 12.0)
        [void][C]::SetCursorPos($ox + $mx, $oy + $my)
        Start-Sleep -Milliseconds 45
    }
    [C]::mouse_event([C]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 900
    # 只裁唱片那一块：整屏图看不出封面是不是圆的
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][C]::PrintWindow($panel, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $crop = New-Object System.Drawing.Bitmap 200, 200
    $gc = [System.Drawing.Graphics]::FromImage($crop)
    $gc.DrawImage($bmp, (New-Object System.Drawing.Rectangle 0, 0, 200, 200),
                  (New-Object System.Drawing.Rectangle 15, 110, 200, 200),
                  [System.Drawing.GraphicsUnit]::Pixel)
    $gc.Dispose()
    $crop.Save((Join-Path $OutDir ("cover-row$row.png")), [System.Drawing.Imaging.ImageFormat]::Png)
    $crop.Dispose()
    $bmp.Dispose()
    Write-Host "cover-row$row.png"
}

$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
if (-not $proc.HasExited) { $proc.Kill() }
Write-Host '---- 日志判据 ----'
Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip $before |
    Select-String -Pattern 'drop:|status: 正在播放' | Select-Object -Last 8 | ForEach-Object { $_.Line }
