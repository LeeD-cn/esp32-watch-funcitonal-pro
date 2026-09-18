param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot)
)

Add-Type -AssemblyName System.Drawing

function Write-LvglImage {
    param(
        [System.Drawing.Bitmap]$Bitmap,
        [string]$Name,
        [string]$OutputPath
    )

    $rect = [System.Drawing.Rectangle]::new(0, 0, $Bitmap.Width, $Bitmap.Height)
    $data = $Bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = [byte[]]::new([Math]::Abs($data.Stride) * $Bitmap.Height)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $Bitmap.UnlockBits($data)

    $writer = [IO.StreamWriter]::new($OutputPath, $false, [Text.UTF8Encoding]::new($false))
    try {
        $writer.WriteLine('#include "lvgl/lvgl.h"')
        $writer.WriteLine()
        $writer.WriteLine("static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t ${Name}_map[] = {")
        for($i = 0; $i -lt $bytes.Length; $i += 16) {
            $end = [Math]::Min($i + 15, $bytes.Length - 1)
            $line = ($bytes[$i..$end] | ForEach-Object { '0x{0:x2}' -f $_ }) -join ','
            $writer.WriteLine("    $line,")
        }
        $writer.WriteLine('};')
        $writer.WriteLine()
        $writer.WriteLine("const lv_image_dsc_t $Name = {")
        $writer.WriteLine('  .header = {')
        $writer.WriteLine('    .magic = LV_IMAGE_HEADER_MAGIC,')
        $writer.WriteLine('    .cf = LV_COLOR_FORMAT_ARGB8888,')
        $writer.WriteLine("    .w = $($Bitmap.Width), .h = $($Bitmap.Height), .stride = $($Bitmap.Width * 4),")
        $writer.WriteLine('  },')
        $writer.WriteLine("  .data_size = sizeof(${Name}_map), .data = ${Name}_map,")
        $writer.WriteLine('};')
    } finally {
        $writer.Dispose()
    }
}

$source = [System.Drawing.Bitmap]::new((Join-Path $ProjectRoot 'SuCai\clockicon.bmp'))
$icon = [System.Drawing.Bitmap]::new(120, 120, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($icon)
$graphics.Clear([System.Drawing.Color]::Transparent)
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.DrawImage($source, 0, 0, 120, 120)
$graphics.Dispose()
$source.Dispose()
for($y = 0; $y -lt 120; $y++) {
    for($x = 0; $x -lt 120; $x++) {
        $pixel = $icon.GetPixel($x, $y)
        if($pixel.R -lt 8 -and $pixel.G -lt 8 -and $pixel.B -lt 8) {
            $icon.SetPixel($x, $y, [System.Drawing.Color]::Transparent)
        }
    }
}
Write-LvglImage $icon 'timer_icon' (Join-Path $ProjectRoot 'main\picture_c\timer_icon.c')
$icon.Dispose()

$title = [System.Drawing.Bitmap]::new(96, 32, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($title)
$graphics.Clear([System.Drawing.Color]::Transparent)
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$font = [System.Drawing.Font]::new('Microsoft YaHei', 20, [System.Drawing.FontStyle]::Regular,
                                  [System.Drawing.GraphicsUnit]::Pixel)
$format = [System.Drawing.StringFormat]::new()
$format.Alignment = [System.Drawing.StringAlignment]::Center
$format.LineAlignment = [System.Drawing.StringAlignment]::Center
$graphics.DrawString('计时器', $font, [System.Drawing.Brushes]::White,
                     [System.Drawing.RectangleF]::new(0, 0, 96, 32), $format)
$format.Dispose()
$font.Dispose()
$graphics.Dispose()
Write-LvglImage $title 'timer_title' (Join-Path $ProjectRoot 'main\picture_c\timer_title.c')
$title.Dispose()

$focusTitle = [System.Drawing.Bitmap]::new(140, 32, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($focusTitle)
$graphics.Clear([System.Drawing.Color]::Transparent)
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$font = [System.Drawing.Font]::new('Microsoft YaHei', 20, [System.Drawing.FontStyle]::Regular,
                                  [System.Drawing.GraphicsUnit]::Pixel)
$format = [System.Drawing.StringFormat]::new()
$format.Alignment = [System.Drawing.StringAlignment]::Center
$format.LineAlignment = [System.Drawing.StringAlignment]::Center
$graphics.DrawString('结束专注？', $font, [System.Drawing.Brushes]::White,
                     [System.Drawing.RectangleF]::new(0, 0, 140, 32), $format)
$format.Dispose()
$font.Dispose()
$graphics.Dispose()
Write-LvglImage $focusTitle 'focus_end_title' (Join-Path $ProjectRoot 'main\picture_c\focus_end_title.c')
$focusTitle.Dispose()

$source = [System.Drawing.Bitmap]::new((Join-Path $ProjectRoot 'SuCai\SPORT.bmp'))
$sport = [System.Drawing.Bitmap]::new(120, 120, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($sport)
$graphics.Clear([System.Drawing.Color]::Transparent)
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.DrawImage($source, 0, 0, 120, 120)
$graphics.Dispose()
$source.Dispose()
for($y = 0; $y -lt 120; $y++) {
    for($x = 0; $x -lt 120; $x++) {
        $pixel = $sport.GetPixel($x, $y)
        if($pixel.R -lt 8 -and $pixel.G -lt 8 -and $pixel.B -lt 8) {
            $sport.SetPixel($x, $y, [System.Drawing.Color]::Transparent)
        }
    }
}
Write-LvglImage $sport 'step_icon' (Join-Path $ProjectRoot 'main\picture_c\step_icon.c')
$sport.Dispose()

function Write-LvglAlphaImage {
    param(
        [System.Drawing.Bitmap]$Bitmap,
        [string]$Name,
        [string]$OutputPath
    )

    $rect = [System.Drawing.Rectangle]::new(0, 0, $Bitmap.Width, $Bitmap.Height)
    $data = $Bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pixels = [byte[]]::new([Math]::Abs($data.Stride) * $Bitmap.Height)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
    $Bitmap.UnlockBits($data)
    $alpha = [byte[]]::new($Bitmap.Width * $Bitmap.Height)
    for($y = 0; $y -lt $Bitmap.Height; $y++) {
        for($x = 0; $x -lt $Bitmap.Width; $x++) {
            $alpha[$y * $Bitmap.Width + $x] = $pixels[$y * $data.Stride + $x * 4 + 3]
        }
    }

    $writer = [IO.StreamWriter]::new($OutputPath, $false, [Text.UTF8Encoding]::new($false))
    try {
        $writer.WriteLine('#include "lvgl/lvgl.h"')
        $writer.WriteLine()
        $writer.WriteLine("static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t ${Name}_map[] = {")
        for($i = 0; $i -lt $alpha.Length; $i += 16) {
            $end = [Math]::Min($i + 15, $alpha.Length - 1)
            $line = ($alpha[$i..$end] | ForEach-Object { '0x{0:x2}' -f $_ }) -join ','
            $writer.WriteLine("    $line,")
        }
        $writer.WriteLine('};')
        $writer.WriteLine()
        $writer.WriteLine("const lv_image_dsc_t $Name = {")
        $writer.WriteLine('  .header = {')
        $writer.WriteLine('    .magic = LV_IMAGE_HEADER_MAGIC,')
        $writer.WriteLine('    .cf = LV_COLOR_FORMAT_A8,')
        $writer.WriteLine("    .w = $($Bitmap.Width), .h = $($Bitmap.Height), .stride = $($Bitmap.Width),")
        $writer.WriteLine('  },')
        $writer.WriteLine("  .data_size = sizeof(${Name}_map), .data = ${Name}_map,")
        $writer.WriteLine('};')
    } finally {
        $writer.Dispose()
    }
}

function Write-TextAsset {
    param([string]$Text, [int]$Width, [int]$Height, [string]$Name)

    $bitmap = [System.Drawing.Bitmap]::new($Width, $Height,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $font = [System.Drawing.Font]::new('Microsoft YaHei', 20,
        [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $format = [System.Drawing.StringFormat]::new()
    $format.Alignment = [System.Drawing.StringAlignment]::Center
    $format.LineAlignment = [System.Drawing.StringAlignment]::Center
    $graphics.DrawString($Text, $font, [System.Drawing.Brushes]::White,
        [System.Drawing.RectangleF]::new(0, 0, $Width, $Height), $format)
    $format.Dispose()
    $font.Dispose()
    $graphics.Dispose()
    Write-LvglAlphaImage $bitmap $Name (Join-Path $ProjectRoot "main\picture_c\$Name.c")
    $bitmap.Dispose()
}

Write-TextAsset '计步器' 96 32 'step_title'
Write-TextAsset '今日步数：' 112 28 'steps_today_caption'
Write-TextAsset '历史步数' 100 28 'steps_history_label'
