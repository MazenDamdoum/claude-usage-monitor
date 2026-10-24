# Renders Claude's spinner frames (the "blossom": · ✢ ✳ ✶ ✻ ✽) to public/blossom/0-5.png for the
# phone, from the font the taskbar draws them with (Segoe UI Symbol), so both look the same: an
# iPhone has other fonts, and draws ✳ as a green emoji. Every frame is rendered at one size and
# placed with the middle of its ink at the middle of the image, so cycling them never shakes.
# Run on Windows after changing the frames:  powershell -File make-blossom.ps1
Add-Type -AssemblyName System.Drawing

$frames = @([char]0x00B7, [char]0x2722, [char]0x2733, [char]0x2736, [char]0x273B, [char]0x273D)
$px = 160                       # glyph size to render at
$orange = @(217, 119, 87)
$out = Join-Path $PSScriptRoot 'public\blossom'
New-Item -ItemType Directory -Force $out | Out-Null

$font = New-Object System.Drawing.Font('Segoe UI Symbol', $px, [System.Drawing.GraphicsUnit]::Pixel)
$renders = @()
foreach ($ch in $frames) {
    $bmp = New-Object System.Drawing.Bitmap 480, 480, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.DrawString([string]$ch, $font, [System.Drawing.Brushes]::White, 120, 80)
    $g.Dispose()
    $minx = 9999; $maxx = -1; $miny = 9999; $maxy = -1
    for ($y = 0; $y -lt 480; $y++) {
        for ($x = 0; $x -lt 480; $x++) {
            if ($bmp.GetPixel($x, $y).A -gt 0) {
                if ($x -lt $minx) { $minx = $x }; if ($x -gt $maxx) { $maxx = $x }
                if ($y -lt $miny) { $miny = $y }; if ($y -gt $maxy) { $maxy = $y }
            }
        }
    }
    $renders += , @($bmp, $minx, $maxx, $miny, $maxy)
}

# One square for all frames, big enough for the largest, so their sizes keep their proportions.
$size = 0
foreach ($r in $renders) { $size = [math]::Max($size, [math]::Max($r[2] - $r[1] + 1, $r[4] - $r[3] + 1)) }
$size += 8
for ($i = 0; $i -lt $renders.Count; $i++) {
    $r = $renders[$i]
    $cx = ($r[1] + $r[2] + 1) / 2.0; $cy = ($r[3] + $r[4] + 1) / 2.0
    $ox = [int][math]::Round($cx - $size / 2.0); $oy = [int][math]::Round($cy - $size / 2.0)
    $img = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    for ($y = 0; $y -lt $size; $y++) {
        for ($x = 0; $x -lt $size; $x++) {
            $sx = $ox + $x; $sy = $oy + $y
            if ($sx -lt 0 -or $sy -lt 0 -or $sx -ge 480 -or $sy -ge 480) { continue }
            $a = $r[0].GetPixel($sx, $sy).A
            if ($a -gt 0) { $img.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($a, $orange[0], $orange[1], $orange[2])) }
        }
    }
    $img.Save((Join-Path $out "$i.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose(); $r[0].Dispose()
}
"blossom: $($renders.Count) frames, ${size}px, in $out"
