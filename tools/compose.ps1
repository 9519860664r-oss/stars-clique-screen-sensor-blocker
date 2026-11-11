# Dev-only: turns preview BMPs into PNGs for the README and draws the banners.
# Usage: tools\compose.ps1   (after tools\build_preview.bat)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$tmp  = Join-Path $env:TEMP 'sssb_preview'
$img  = Join-Path $root 'docs\img'

function Load-Bmp([string]$path) {
    [byte[]]$b = [IO.File]::ReadAllBytes($path)
    $w = [BitConverter]::ToInt32($b, 18); $h = -[BitConverter]::ToInt32($b, 22)
    $bmp = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format32bppPArgb)
    $r = $bmp.LockBits((New-Object Drawing.Rectangle 0, 0, $w, $h), 'WriteOnly', $bmp.PixelFormat)
    [Runtime.InteropServices.Marshal]::Copy($b, 54, $r.Scan0, $w * $h * 4)
    $bmp.UnlockBits($r)
    return $bmp
}

function Font([string]$face, [float]$px, [string]$style = 'Regular') {
    try { return New-Object Drawing.Font $face, $px, ([Drawing.FontStyle]$style), ([Drawing.GraphicsUnit]::Pixel) }
    catch { return New-Object Drawing.Font 'Segoe UI', $px, ([Drawing.FontStyle]$style), ([Drawing.GraphicsUnit]::Pixel) }
}

function Banner([string]$lang, [string]$tagline, [string]$foot) {
    $W = 1280; $H = 640
    $bmp = New-Object Drawing.Bitmap $W, $H
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.Clear([Drawing.Color]::FromArgb(18, 15, 30))
    $g.TextRenderingHint = 'AntiAliasGridFit'; $g.InterpolationMode = 'HighQualityBicubic'
    $rect = New-Object Drawing.Rectangle 0, 0, $W, $H
    # brush rect larger than the image: GDI+ gradients wrap a light line at the edges otherwise
    $brushRect = New-Object Drawing.Rectangle -4, -4, ($W + 8), ($H + 8)
    $bg = New-Object Drawing.Drawing2D.LinearGradientBrush $brushRect, ([Drawing.Color]::FromArgb(22, 18, 38)), ([Drawing.Color]::FromArgb(12, 12, 16)), 35
    $bg.WrapMode = 'TileFlipXY'
    $g.FillRectangle($bg, $rect)
    $g.SmoothingMode = 'AntiAlias'
    # soft violet glow behind the cards
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse(560, 60, 760, 560)
    $glow = New-Object Drawing.Drawing2D.PathGradientBrush $path
    $glow.CenterColor = [Drawing.Color]::FromArgb(90, 139, 92, 246)
    $glow.SurroundColors = @([Drawing.Color]::FromArgb(0, 139, 92, 246))
    $g.FillPath($glow, $path)

    $white = [Drawing.Brushes]::White
    $muted = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(168, 168, 182))
    $violet = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(167, 139, 250))
    $g.DrawString('STARS CLIQUE', (Font 'Segoe UI Variable Display' 22 'Bold'), $violet, 72, 150)
    $g.DrawString("Screen Sensor`nBlocker", (Font 'Segoe UI Variable Display' 64 'Bold'), $white, 66, 182)
    $g.DrawString($tagline, (Font 'Segoe UI Variable Text' 24), $muted, (New-Object Drawing.RectangleF 72, 360, 520, 120))
    $g.DrawString($foot, (Font 'Segoe UI Variable Text' 18), $violet, 72, 500)

    $locked = Load-Bmp "$tmp\$lang\locked.bmp"; $unlocked = Load-Bmp "$tmp\$lang\unlocked.bmp"
    $s = 0.78
    $g.DrawImage($unlocked, 640, 170, [int]($unlocked.Width * $s), [int]($unlocked.Height * $s))
    $g.DrawImage($locked, 700, 330, [int]($locked.Width * $s), [int]($locked.Height * $s))
    $bmp.Save("$img\banner-$lang.png", [Drawing.Imaging.ImageFormat]::Png)
}

foreach ($lang in 'ru', 'en') {
    $out = Join-Path $tmp $lang
    New-Item -ItemType Directory -Force $out, "$img\$lang" | Out-Null
    $p = Start-Process (Join-Path $PSScriptRoot "preview_$lang.exe") -ArgumentList "`"$out`"" -NoNewWindow -Wait -PassThru
    if ($p.ExitCode) { throw "preview_$lang failed: $($p.ExitCode)" }
    foreach ($f in Get-ChildItem $out -Filter *.bmp) {
        (Load-Bmp $f.FullName).Save("$img\$lang\$($f.BaseName).png", [Drawing.Imaging.ImageFormat]::Png)
    }
}
Banner 'en' 'Turn your touch screen off and on with one key. Tiny, fast, no setup.' 'Windows 10 / 11  ·  150 KB  ·  ~2 MB RAM'
Banner 'ru' 'Выключай и включай сенсорный экран одной клавишей. Маленькая, быстрая, без настройки.' 'Windows 10 / 11  ·  150 КБ  ·  ~2 МБ памяти'
"done: $img"
