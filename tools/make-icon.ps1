# Generates app\ui\icon.ico from the same drawing as icon.svg (rounded square with a blue -> orange
# gradient, dark pupil, white ellipse outline). 32-bit BMP entries in several sizes: the classic
# format, which rc.exe and Windows shell both read.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$out = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "app\ui\icon.ico"

function Render([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $r = [single]($s * 0.25)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $r * 2, $r * 2, 180, 90)
    $path.AddArc($s - $r * 2, 0, $r * 2, $r * 2, 270, 90)
    $path.AddArc($s - $r * 2, $s - $r * 2, $r * 2, $r * 2, 0, 90)
    $path.AddArc(0, $s - $r * 2, $r * 2, $r * 2, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect,
        [System.Drawing.Color]::FromArgb(255, 61, 155, 255), [System.Drawing.Color]::FromArgb(255, 255, 138, 42), 45.0)
    $g.FillPath($brush, $path)
    $dark = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 14, 17, 22))
    $d = [single]($s * 0.594)
    $g.FillEllipse($dark, [single](($s - $d) / 2), [single](($s - $d) / 2), $d, $d)
    $penW = [single][Math]::Max(1, $s * 0.078)
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 232, 237, 244), $penW)
    $ew = [single]($s * 0.25); $eh = [single]($s * 0.375)
    $g.DrawEllipse($pen, [single](($s - $ew) / 2), [single](($s - $eh) / 2), $ew, $eh)
    $g.Dispose()
    return $bmp
}

function EntryBytes([int]$s) {
    $bmp = Render $s
    $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $pixels = New-Object byte[] ($stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
    $bmp.UnlockBits($data); $bmp.Dispose()

    $maskRow = [Math]::Floor(($s + 31) / 32) * 4
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    # BITMAPINFOHEADER: height counts the XOR and the AND mask
    $bw.Write([int]40); $bw.Write([int]$s); $bw.Write([int]($s * 2))
    $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int]0)
    $bw.Write([int]($s * $s * 4)); $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0)
    # XOR: bottom-up BGRA
    for ($y = $s - 1; $y -ge 0; $y--) { $bw.Write($pixels, $y * $stride, $s * 4) }
    # AND mask: all zero (alpha already carries the shape)
    $zeros = New-Object byte[] ($maskRow * $s)
    $bw.Write($zeros, 0, $zeros.Length)
    $bw.Flush()
    return ,$ms.ToArray()   # the comma keeps it a byte[] (PowerShell would unroll it)
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$images = @()
foreach ($s in $sizes) { $images += [pscustomobject]@{ Size = $s; Data = (EntryBytes $s) } }

$ico = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ico)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
foreach ($img in $images) {
    $s = $img.Size; $bytes = $img.Data
    $bw.Write([byte]($s % 256)); $bw.Write([byte]($s % 256))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$bytes.Length); $bw.Write([uint32]$offset)
    $offset += $bytes.Length
}
foreach ($img in $images) { $bw.Write($img.Data) }
$bw.Flush()
[System.IO.File]::WriteAllBytes($out, $ico.ToArray())
"wrote $out ($($ico.Length) bytes, $($sizes.Count) sizes)"
