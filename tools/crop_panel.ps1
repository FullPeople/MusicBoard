param(
    [int]$X = 0,
    [int]$Y = 0,
    [int]$W = 1256,
    [int]$H = 788,
    [string]$Out = '',
    # 给了 Source 就从这张 PNG 上裁，而不是重新截屏幕 ——
    # 复核时用的是 PrintWindow 抓到的那一帧，从屏幕再截一次会拿到别的画面。
    [string]$Source = ''
)

Add-Type -AssemblyName System.Drawing

if ($Source -ne '') {
    $full = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $Source).Path)
    $crop = New-Object System.Drawing.Bitmap $W, $H
    $g = [System.Drawing.Graphics]::FromImage($crop)
    $g.DrawImage($full, (New-Object System.Drawing.Rectangle 0, 0, $W, $H),
                 (New-Object System.Drawing.Rectangle $X, $Y, $W, $H),
                 [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $crop.Save((Join-Path (Get-Location) $Out), [System.Drawing.Imaging.ImageFormat]::Png)
    $crop.Dispose()
    $full.Dispose()
    Write-Host "saved $Out"
    exit 0
}

# 以**原生分辨率**截一块屏幕（CUA 的全屏截图会被缩到 1280 宽，看细节不够）。
$rect = New-Object System.Drawing.Rectangle $X, $Y, $W, $H
$bmp = New-Object System.Drawing.Bitmap $rect.Width, $rect.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.X, $rect.Y, 0, 0, $bmp.Size)
if ([string]::IsNullOrWhiteSpace($Out)) {
    $Out = Join-Path $env:TEMP 'panel-crop.png'
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Host "saved $Out"
