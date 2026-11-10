# gen_app_icon.ps1
# エンジンの exe アイコン (build\res\MyEngine.ico) を生成する (M98c)。
#   pwsh -NoProfile -File tools\gen_app_icon.ps1 [-PreviewPng <path>]
# 絵は起動スプラッシュのメビウスの輪 (src\Editor\App\StartupMoebius.cpp と同じ式) の静止画。
# 黒地に、手前ほど明るい白寄りの灰色の線で描く (ユーザー指定: 背景黒・白 (少し灰色寄り))。
# 小さいサイズでは線の本数を減らして太くする (16px に 96 本の線を引くと潰れて灰色の塊になる)。
# 生成物はコミットする。式を変えたときだけ実行し直す (GDI+ のアンチエイリアスは環境で僅かに違いうる)
param([string]$PreviewPng = '', [double]$RotY = 2.6, [double]$RotX = 0.9, [string]$OutIco = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repo = Split-Path -Parent $PSScriptRoot
$outIco = if ($OutIco) { $OutIco } else { Join-Path $repo 'build\res\MyEngine.ico' }

# StartupMoebius.cpp と同じ形 (majorR / halfW / カメラ距離)。向きはスプラッシュの X 傾き 0.55 に、
# 帯のねじれが正面から見える Y 回転を足した静止の 1 コマ
$majorR = 1.0
$halfW = 0.28
$camDist = 4.0
$rotY = $RotY
$rotX = $RotX
$frontGray = 226 # 手前の線 (白より少し灰色)
$backGray = 120  # 奥の線

function Get-EyePoint([double]$u, [double]$v)
{
    $hv = $halfW * $v
    $ring = $majorR + $hv * [Math]::Cos($u * 0.5)
    $x = $ring * [Math]::Cos($u); $y = $ring * [Math]::Sin($u); $z = $hv * [Math]::Sin($u * 0.5)
    # RotateY → RotateX → カメラの前へ (StartupMoebius.cpp の ToEyeSpace と同じ順)
    $c = [Math]::Cos($rotY); $s = [Math]::Sin($rotY)
    $x1 = $x * $c + $z * $s; $z1 = -$x * $s + $z * $c
    $c = [Math]::Cos($rotX); $s = [Math]::Sin($rotX)
    $y2 = $y * $c - $z1 * $s; $z2 = $y * $s + $z1 * $c
    $z2 += $camDist
    $k = $camDist / $z2
    return [pscustomobject]@{ X = $x1 * $k; Y = -$y2 * $k; Z = $z2 }
}

# 線分の一覧 (投影済み、奥行き付き)。BuildWireframe と同じく v 一定の線と u 一定の線
function Get-Segments([int]$uSteps, [int]$vLines)
{
    $segs = [System.Collections.Generic.List[object]]::new()
    $twoPi = 2.0 * [Math]::PI
    for ($vi = 0; $vi -lt $vLines; $vi++) {
        $v = -1.0 + 2.0 * $vi / ($vLines - 1)
        for ($ui = 0; $ui -lt $uSteps; $ui++) {
            $a = Get-EyePoint ($twoPi * $ui / $uSteps) $v
            $b = Get-EyePoint ($twoPi * ($ui + 1) / $uSteps) $v
            $segs.Add([pscustomobject]@{ A = $a; B = $b; Z = ($a.Z + $b.Z) * 0.5 })
        }
    }
    for ($ui = 0; $ui -lt $uSteps; $ui++) {
        $u = $twoPi * $ui / $uSteps
        for ($vi = 0; $vi -lt $vLines - 1; $vi++) {
            $a = Get-EyePoint $u (-1.0 + 2.0 * $vi / ($vLines - 1))
            $b = Get-EyePoint $u (-1.0 + 2.0 * ($vi + 1) / ($vLines - 1))
            $segs.Add([pscustomobject]@{ A = $a; B = $b; Z = ($a.Z + $b.Z) * 0.5 })
        }
    }
    return $segs
}

# サイズごとの線の密度と太さ (px は最終サイズでの太さ)
function Get-Style([int]$size)
{
    if ($size -ge 128) { return @{ U = 72; V = 7; Width = [double]$size / 110.0 } }
    if ($size -ge 48) { return @{ U = 48; V = 5; Width = [double]$size / 40.0 } }
    if ($size -ge 32) { return @{ U = 36; V = 4; Width = 1.15 } }
    return @{ U = 24; V = 3; Width = 1.0 }
}

function New-IconBitmap([int]$size)
{
    $ss = 4 # 縮小前の倍率 (アンチエイリアスを滑らかにする)
    $big = $size * $ss
    $style = Get-Style $size
    $segs = Get-Segments $style.U $style.V
    $minX = [double]::MaxValue; $maxX = [double]::MinValue; $minY = [double]::MaxValue; $maxY = [double]::MinValue
    $minZ = [double]::MaxValue; $maxZ = [double]::MinValue
    foreach ($sg in $segs) {
        foreach ($p in $sg.A, $sg.B) {
            $minX = [Math]::Min($minX, $p.X); $maxX = [Math]::Max($maxX, $p.X)
            $minY = [Math]::Min($minY, $p.Y); $maxY = [Math]::Max($maxY, $p.Y)
        }
        $minZ = [Math]::Min($minZ, $sg.Z); $maxZ = [Math]::Max($maxZ, $sg.Z)
    }
    $margin = 0.09
    $scale = $big * (1.0 - 2.0 * $margin) / [Math]::Max($maxX - $minX, $maxY - $minY)
    $cx = $big * 0.5 - ($minX + $maxX) * 0.5 * $scale
    $cy = $big * 0.5 - ($minY + $maxY) * 0.5 * $scale

    $bmp = [System.Drawing.Bitmap]::new($big, $big, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Black)
    # 奥から手前へ (スプラッシュと同じ描き順)。eyeZ が大きいほど奥
    foreach ($sg in ($segs | Sort-Object -Property Z -Descending)) {
        $t = if ($maxZ -gt $minZ) { ($maxZ - $sg.Z) / ($maxZ - $minZ) } else { 1.0 }
        $gray = [int][Math]::Round($backGray + ($frontGray - $backGray) * $t)
        $pen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, $gray, $gray, $gray), $style.Width * $ss)
        $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $g.DrawLine($pen, [single]($cx + $sg.A.X * $scale), [single]($cy + $sg.A.Y * $scale),
            [single]($cx + $sg.B.X * $scale), [single]($cy + $sg.B.Y * $scale))
        $pen.Dispose()
    }
    $g.Dispose()

    $small = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($small)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.DrawImage($bmp, 0, 0, $size, $size)
    $g.Dispose()
    $bmp.Dispose()
    return $small
}

# ICO (Vista 以降の PNG 格納形式)。各サイズを PNG のまま並べる
$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = foreach ($size in $sizes) {
    $bmp = New-IconBitmap $size
    $ms = [System.IO.MemoryStream]::new()
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($PreviewPng -and $size -eq 256) { $bmp.Save($PreviewPng, [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
    , $ms.ToArray()
}

New-Item -ItemType Directory -Force (Split-Path -Parent $outIco) | Out-Null
$fs = [System.IO.File]::Create($outIco)
$w = [System.IO.BinaryWriter]::new($fs)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] } # 256 は 0 と書く決まり
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Dispose()
Write-Host "wrote $outIco ($((Get-Item $outIco).Length) bytes, sizes $($sizes -join ', '))"
