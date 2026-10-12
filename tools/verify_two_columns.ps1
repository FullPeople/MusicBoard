param(
    [string]$Exe = 'D:\Desktop\音乐板\soundboard-native\build\windows-x64\Release\musicboard.exe',
    [string]$OutDir = 'D:\Desktop\音乐板\soundboard-native\evidence'
)

# 实机复核右列（音效库）。
#
# 上一版脚本栽在"猜坐标"上：MainWindowHandle 给的是那个 184x184 的悬浮球，
# 我却在用它当主面板的坐标系，于是点出来全是别的东西。
# 这一版按**窗口类名**找主面板，坐标靠"拿真光标扫一遍、从日志读命中的 id"
# 现场测出来，不再推算。
#
# 判据（全部来自程序自己的输入日志）：
#   悬停右列某点 → 命中 id 落在 2000+（音效卡段）
#   从右列拖到音效台 1 的唱片上 → 落下时命中 id 落在 2xx（音效台控件段）
# 只有这两条都成立，才谈得上"右列能悬停、能拖"。

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class V {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumWindows(EnumProc cb, IntPtr param);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hwnd, StringBuilder buf, int max);
  public delegate bool EnumProc(IntPtr hwnd, IntPtr param);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
}
"@

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (-not (Test-Path -LiteralPath $Exe)) { Write-Host "no exe: $Exe"; exit 1 }

$log = Join-Path $env:LOCALAPPDATA ('MusicBoard\logs\app-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$logLinesBefore = 0
if (Test-Path -LiteralPath $log) { $logLinesBefore = (Get-Content -LiteralPath $log -Encoding UTF8).Count }

$env:MUSICBOARD_TRACE_INPUT = '1'
$proc = Start-Process -FilePath $Exe -PassThru
Write-Host "started pid=$($proc.Id)"
Start-Sleep -Seconds 5
if ($proc.HasExited) { Write-Host 'exited early'; exit 1 }

# 按**类名**找窗口，但走 EnumWindows 按 pid 过滤，不用 FindWindowW。
#
# 两个坑都踩过，记在这儿免得下次再踩：
#   1) 不能用进程的 MainWindowHandle —— 它给的是那个 184x184 的悬浮球，
#      拿它当主面板的坐标系，点出来全是别的东西；
#   2) FindWindowW 这里一直返回 0（同一进程枚举却能拿到类名），
#      所以别跟它较劲，按 pid 枚举更可靠。
# 另外要轮询等：曲库扫描要几秒，窗口是扫完之后才建的。
function Find-ByClass([int]$targetPid, [string]$className) {
    $found = [IntPtr]::Zero
    $cb = [V+EnumProc]{
        param($h, $x)
        $w = 0
        [void][V]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $targetPid) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][V]::GetClassNameW($h, $sb, 256)
            if ($sb.ToString() -eq $className) { $script:foundHwnd = $h }
        }
        return $true
    }
    [void][V]::EnumWindows($cb, [IntPtr]::Zero)
    if ($null -ne $script:foundHwnd) { $found = $script:foundHwnd }
    return $found
}

$panel = [IntPtr]::Zero
for ($i = 0; $i -lt 40; $i++) {
    $script:foundHwnd = $null
    $panel = Find-ByClass $proc.Id 'MusicBoard.CompositionWindow'
    if ($panel -ne [IntPtr]::Zero) { break }
    if ($proc.HasExited) { break }
    Start-Sleep -Milliseconds 500
}
$script:foundHwnd = $null
$orb = Find-ByClass $proc.Id 'MusicBoard.OrbWindow'
$script:foundHwnd = $null
Write-Host "panel=$panel visible=$([V]::IsWindowVisible($panel))  orb=$orb visible=$([V]::IsWindowVisible($orb))"
if ($panel -eq [IntPtr]::Zero) { Write-Host 'panel window not found'; $proc.Kill(); exit 1 }

function PanelOrigin() {
    $pr = New-Object V+RECT
    [void][V]::GetWindowRect($panel, [ref]$pr)
    $cr = New-Object V+RECT
    [void][V]::GetClientRect($panel, [ref]$cr)
    # 无边框窗口，客户区原点就是窗口原点；尺寸按客户区校正
    return @{ X = $pr.L; Y = $pr.T; W = $cr.R - $cr.L; H = $cr.B - $cr.T }
}

# 面板没展开就先点一下悬浮球（球是"点一下展开面板"，左键不做切换）
if (-not [V]::IsWindowVisible($panel) -and $orb -ne [IntPtr]::Zero) {
    $orr = New-Object V+RECT
    [void][V]::GetWindowRect($orb, [ref]$orr)
    $cx = [int](($orr.L + $orr.R) / 2); $cy = [int](($orr.T + $orr.B) / 2)
    Write-Host "expanding panel via orb click at $cx,$cy"
    [void][V]::SetCursorPos($cx, $cy); Start-Sleep -Milliseconds 300
    [V]::mouse_event([V]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 90
    [V]::mouse_event([V]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Seconds 2
}

$o = PanelOrigin
Write-Host "panel client origin=($($o.X),$($o.Y)) size=$($o.W)x$($o.H)"

function Capture([string]$name) {
    $bmp = New-Object System.Drawing.Bitmap $o.W, $o.H
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [V]::PrintWindow($panel, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $path = Join-Path $OutDir $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "captured $name (PrintWindow=$ok)"
}

function MoveTo([int]$x, [int]$y) {
    [void][V]::SetCursorPos($o.X + $x, $o.Y + $y)
    Start-Sleep -Milliseconds 120
}

# ---- 第一遍：扫出布局 ----
#
# 把整个面板按 20px 网格扫一遍，每一步都挪真光标，然后从程序自己的日志里
# 读"这一步命中了哪个 id"。这是"问程序"，不是"猜程序" —— 上一版按布局代码
# 推坐标，推错了，于是得出了"右列没反应"这种假结论。
# 网格必须盖满整个面板：上一版只扫了 y=120/280/430 三行，正好全在音效卡
# 网格**上方**，于是根本没扫到任何一张音效卡。
$stepX = 20
$stepY = 20
for ($y = 30; $y -lt $o.H; $y += $stepY) {
    for ($x = 20; $x -lt $o.W; $x += $stepX) { MoveTo $x $y }
}
Start-Sleep -Milliseconds 800

$lines = Get-Content -LiteralPath $log -Encoding UTF8
$samples = New-Object System.Collections.ArrayList
foreach ($line in ($lines | Select-Object -Skip $logLinesBefore)) {
    if ($line -match '\] move: .*?(\d+),(\d+) .*id=(-?\d+)') {
        [void]$samples.Add([pscustomobject]@{ X = [int]$Matches[1]; Y = [int]$Matches[2]; Id = [int]$Matches[3] })
    }
}
Write-Host "sweep samples: $($samples.Count)"

function Bucket([int]$id) {
    if ($id -lt 0) { return 'empty' }
    if ($id -ge 2000) { return 'sfx-card' }
    if ($id -ge 1000) { return 'bgm-card' }
    if ($id -ge 500 -and $id -lt 600) { return 'sfx-chip' }
    if ($id -ge 400 -and $id -lt 500) { return 'bgm-chip' }
    if ($id -eq 301) { return 'sfx-search' }
    if ($id -eq 300) { return 'bgm-search' }
    if ($id -ge 200 -and $id -lt 300) { return 'sfx-deck' }
    return "other($id)"
}

# 每张卡的"第一张"用 id 反解：1000 / 2000 就是各自 0 号卡。
# 取同一 id 所有采样点的包围盒中点当中心 —— 卡片是矩形，这么估够准。
function CenterOf([int]$wantId) {
    $pts = @($samples | Where-Object { $_.Id -eq $wantId })
    if ($pts.Count -eq 0) { return $null }
    $xs = $pts | ForEach-Object { $_.X }
    $ys = $pts | ForEach-Object { $_.Y }
    return @{ X = [int](($xs | Measure-Object -Minimum).Minimum + ($xs | Measure-Object -Maximum).Maximum) / 2
              Y = [int](($ys | Measure-Object -Minimum).Minimum + ($ys | Measure-Object -Maximum).Maximum) / 2
              N = $pts.Count }
}

Write-Host '---- bucket histogram ----'
$samples | Group-Object { Bucket $_.Id } | Sort-Object Name | ForEach-Object {
    Write-Host ("  {0,-12} {1,5} samples" -f $_.Name, $_.Count)
}

$sfxCard = CenterOf 2000
$bgmCard = CenterOf 1000
$sfxDeck1 = CenterOf 200
$bgmDisc = CenterOf 100
foreach ($pair in @(@('sfx-card#0', $sfxCard), @('bgm-card#0', $bgmCard),
                    @('sfx-deck1 disc', $sfxDeck1), @('bgm disc', $bgmDisc))) {
    if ($null -eq $pair[1]) { Write-Host ("  {0}: not found" -f $pair[0]) }
    else { Write-Host ("  {0}: center=({1},{2}) from {3} samples" -f $pair[0], $pair[1].X, $pair[1].Y, $pair[1].N) }
}

if ($null -eq $sfxCard -or $null -eq $sfxDeck1) {
    Write-Host 'inconclusive: 扫不到音效卡或音效台 1 的唱片，不做结论'
    Capture 'runtime-sweep-only.png'
    $proc.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 2
    if (-not $proc.HasExited) { $proc.Kill() }
    exit 2
}

# ---- 悬停那张音效卡 ----
MoveTo $sfxCard.X $sfxCard.Y
Start-Sleep -Milliseconds 500
Capture 'runtime-01-hover-sfx-card.png'

# ---- 从右列拖到音效台 1 的唱片 ----
$mark = (Get-Content -LiteralPath $log -Encoding UTF8).Count
[void][V]::SetCursorPos($o.X + $sfxCard.X, $o.Y + $sfxCard.Y)
Start-Sleep -Milliseconds 300
[V]::mouse_event([V]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 200
for ($s = 1; $s -le 16; $s++) {
    $mx = [int]($sfxCard.X + ($sfxDeck1.X - $sfxCard.X) * $s / 16.0)
    $my = [int]($sfxCard.Y + ($sfxDeck1.Y - $sfxCard.Y) * $s / 16.0)
    [void][V]::SetCursorPos($o.X + $mx, $o.Y + $my)
    Start-Sleep -Milliseconds 55
}
Start-Sleep -Milliseconds 300
Capture 'runtime-02-drag-over-sfx-deck.png'
[V]::mouse_event([V]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900
Capture 'runtime-03-after-drop.png'
Start-Sleep -Seconds 1
Capture 'runtime-04-playing.png'

# ---- 左列回归：拖一张音乐卡到 BGM 唱片 ----
# 左列也用包围盒中心（$bgmCard / $bgmDisc 在上面已经算好）
if ($null -ne $bgmCard -and $null -ne $bgmDisc) {
    [void][V]::SetCursorPos($o.X + $bgmCard.X, $o.Y + $bgmCard.Y)
    Start-Sleep -Milliseconds 300
    [V]::mouse_event([V]::LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    for ($s = 1; $s -le 16; $s++) {
        $mx = [int]($bgmCard.X + ($bgmDisc.X - $bgmCard.X) * $s / 16.0)
        $my = [int]($bgmCard.Y + ($bgmDisc.Y - $bgmCard.Y) * $s / 16.0)
        [void][V]::SetCursorPos($o.X + $mx, $o.Y + $my)
        Start-Sleep -Milliseconds 55
    }
    Start-Sleep -Milliseconds 250
    [V]::mouse_event([V]::LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Seconds 1
    Capture 'runtime-05-bgm-regression.png'
}

Start-Sleep -Milliseconds 500
$proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
if (-not $proc.HasExited) { $proc.Kill() }
Start-Sleep -Milliseconds 500

Write-Host '---- program input log (this is the evidence) ----'
Get-Content -LiteralPath $log -Encoding UTF8 | Select-Object -Skip $mark | Select-String -Pattern 'left_down|left_up|drop:|status:' | Select-Object -Last 16 | ForEach-Object { $_.Line }
Write-Host "log: $log"
