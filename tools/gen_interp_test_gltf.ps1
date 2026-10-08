# assets\models\interp_test.glb を生成する (M89p の回帰素材: glTF の STEP / CUBICSPLINE の読み込み)。
#
# 骨 1 本 ("Bone"、原点) の箱と、補間の違いが値で分かる 2 本のクリップを持つ。
#   Step  1.0 s  translation STEP:        t = 0 / 0.5 / 1.0 で x = 0 / 1 / 2 (間は前の値のまま)
#   Cubic 1.0 s  translation CUBICSPLINE: x が 0 → 1 (接線 0 = smoothstep。t=0.25 で 0.15625、線形なら 0.25)
#                rotation    CUBICSPLINE: 恒等 → Z 軸 90 度 (接線 0。t=0.5 は成分の平均を正規化 = 45 度)
# 使われない接線 (最初のキーの入り接線・最後のキーの出接線) には 99 を入れる。
# 要素の位置を取り違えると値に 99 が混ざるので、読み込みの誤りが selftest で見える。
# 値は x 軸・z 軸まわりだけなので、ローダの Z 反転 (z → -z、クォータニオンの x, y の符号) の影響は
# translation では受けず、rotation では z 軸まわりの角度だけが残る (向きは反転後の空間で同じ)。

param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\assets\models')
)

$ErrorActionPreference = 'Stop'
$out = Join-Path $OutDir 'interp_test.glb'

$bin = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($bin)
$bufferViews = New-Object System.Collections.Generic.List[object]
$accessors = New-Object System.Collections.Generic.List[object]
function Align4 { while (($bin.Length % 4) -ne 0) { $bw.Write([byte]0) } }
# float の並び (要素数 comps) を 1 accessor として書く。minmax なら成分ごとの min / max を付ける
function Add-FloatAccessor([System.Collections.IList]$rows, [int]$comps, [string]$type, [bool]$minmax) {
    Align4
    $offset = [int]$bin.Length
    $mn = @(); $mx = @()
    for ($k = 0; $k -lt $comps; $k++) { $mn += [double]::MaxValue; $mx += [double]::MinValue }
    foreach ($r in $rows) {
        for ($k = 0; $k -lt $comps; $k++) {
            $f = [single]$r[$k]
            $bw.Write($f)
            $d = [double]$f
            if ($d -lt $mn[$k]) { $mn[$k] = $d }
            if ($d -gt $mx[$k]) { $mx[$k] = $d }
        }
    }
    $bufferViews.Add([ordered]@{ buffer = 0; byteOffset = $offset; byteLength = ([int]$bin.Length - $offset) })
    $acc = [ordered]@{ bufferView = $bufferViews.Count - 1; componentType = 5126; count = $rows.Count; type = $type }
    if ($minmax) { $acc.min = $mn; $acc.max = $mx }
    $accessors.Add($acc)
    return $accessors.Count - 1
}
function Rows([object[]]$list) {
    $rows = New-Object System.Collections.Generic.List[double[]]
    foreach ($r in $list) { $rows.Add([double[]]$r) }
    return , $rows
}

# ---- メッシュ: 骨に重み 1 の箱 (0.1 m 角、上へ 0.2 m) ----
$positions = New-Object System.Collections.Generic.List[double[]]
$normals = New-Object System.Collections.Generic.List[double[]]
$indices = New-Object System.Collections.Generic.List[int]
$faces = @(
    @{ n = @(1, 0, 0);  u = @(0, 1, 0); v = @(0, 0, 1) },
    @{ n = @(-1, 0, 0); u = @(0, 0, 1); v = @(0, 1, 0) },
    @{ n = @(0, 1, 0);  u = @(0, 0, 1); v = @(1, 0, 0) },
    @{ n = @(0, -1, 0); u = @(1, 0, 0); v = @(0, 0, 1) },
    @{ n = @(0, 0, 1);  u = @(1, 0, 0); v = @(0, 1, 0) },
    @{ n = @(0, 0, -1); u = @(0, 1, 0); v = @(1, 0, 0) }
)
$c = @(0, 0.1, 0); $h = @(0.05, 0.1, 0.05)
foreach ($f in $faces) {
    $base = $positions.Count
    foreach ($s in @(@(-1, -1), @(1, -1), @(1, 1), @(-1, 1))) {
        $p = @(0.0, 0.0, 0.0)
        for ($k = 0; $k -lt 3; $k++) { $p[$k] = $c[$k] + ($f.n[$k] + $s[0] * $f.u[$k] + $s[1] * $f.v[$k]) * $h[$k] }
        $positions.Add($p)
        $normals.Add([double[]]$f.n)
    }
    $indices.AddRange([int[]]@($base, ($base + 1), ($base + 2), $base, ($base + 2), ($base + 3)))
}
$posAcc = Add-FloatAccessor $positions 3 'VEC3' $true
$nrmAcc = Add-FloatAccessor $normals 3 'VEC3' $false
$wRows = New-Object System.Collections.Generic.List[double[]]
foreach ($p in $positions) { $wRows.Add(@(1.0, 0.0, 0.0, 0.0)) }
$wgtAcc = Add-FloatAccessor $wRows 4 'VEC4' $false
Align4
$jOffset = [int]$bin.Length
foreach ($p in $positions) { $bw.Write([uint32]0) } # JOINTS_0 = unsigned byte x4 がすべて 0
$bufferViews.Add([ordered]@{ buffer = 0; byteOffset = $jOffset; byteLength = ([int]$bin.Length - $jOffset) })
$accessors.Add([ordered]@{ bufferView = $bufferViews.Count - 1; componentType = 5121; count = $positions.Count; type = 'VEC4' })
$jntAcc = $accessors.Count - 1
Align4
$iOffset = [int]$bin.Length
foreach ($ix in $indices) { $bw.Write([uint16]$ix) }
$bufferViews.Add([ordered]@{ buffer = 0; byteOffset = $iOffset; byteLength = ([int]$bin.Length - $iOffset) })
$accessors.Add([ordered]@{ bufferView = $bufferViews.Count - 1; componentType = 5123; count = $indices.Count; type = 'SCALAR' })
$idxAcc = $accessors.Count - 1
$ibmAcc = Add-FloatAccessor (Rows @(, @(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1))) 16 'MAT4' $false

# ---- アニメーション (ノード 2 = Bone) ----
$kBoneNode = 2
$q45 = [math]::Sqrt(0.5)
$animations = @(
    [ordered]@{
        name = 'Step'
        samplers = @([ordered]@{
                input = (Add-FloatAccessor (Rows @(@(0.0), @(0.5), @(1.0))) 1 'SCALAR' $true)
                output = (Add-FloatAccessor (Rows @(@(0, 0, 0), @(1, 0, 0), @(2, 0, 0))) 3 'VEC3' $false)
                interpolation = 'STEP' })
        channels = @([ordered]@{ sampler = 0; target = [ordered]@{ node = $kBoneNode; path = 'translation' } })
    },
    [ordered]@{
        name = 'Cubic'
        samplers = @(
            [ordered]@{
                input = (Add-FloatAccessor (Rows @(@(0.0), @(1.0))) 1 'SCALAR' $true)
                # キーごとに [入り接線, 値, 出接線]
                output = (Add-FloatAccessor (Rows @(@(99, 99, 99), @(0, 0, 0), @(0, 0, 0),
                                                   @(0, 0, 0), @(1, 0, 0), @(99, 99, 99))) 3 'VEC3' $false)
                interpolation = 'CUBICSPLINE' },
            [ordered]@{
                input = (Add-FloatAccessor (Rows @(@(0.0), @(1.0))) 1 'SCALAR' $true)
                output = (Add-FloatAccessor (Rows @(@(99, 99, 99, 99), @(0, 0, 0, 1), @(0, 0, 0, 0),
                                                   @(0, 0, 0, 0), @(0, 0, $q45, $q45), @(99, 99, 99, 99))) 4 'VEC4' $false)
                interpolation = 'CUBICSPLINE' })
        channels = @(
            [ordered]@{ sampler = 0; target = [ordered]@{ node = $kBoneNode; path = 'translation' } },
            [ordered]@{ sampler = 1; target = [ordered]@{ node = $kBoneNode; path = 'rotation' } })
    }
)

Align4
$binBytes = $bin.ToArray()
$gltf = [ordered]@{
    asset = [ordered]@{ version = '2.0'; generator = 'MyEngine tools/gen_interp_test_gltf.ps1' }
    scene = 0
    scenes = @([ordered]@{ nodes = @(0) })
    nodes = @(
        [ordered]@{ name = 'Armature'; children = @(1, $kBoneNode) },
        [ordered]@{ name = 'Body'; mesh = 0; skin = 0 },
        [ordered]@{ name = 'Bone'; translation = @(0, 0, 0) })
    meshes = @([ordered]@{ name = 'Body'; primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc; NORMAL = $nrmAcc; JOINTS_0 = $jntAcc; WEIGHTS_0 = $wgtAcc }
                indices = $idxAcc; material = 0 }) })
    materials = @([ordered]@{ name = 'InterpTestBody'; pbrMetallicRoughness = [ordered]@{
                baseColorFactor = @(0.45, 0.62, 0.80, 1.0); metallicFactor = 0.0; roughnessFactor = 0.8 } })
    skins = @([ordered]@{ inverseBindMatrices = $ibmAcc; joints = @($kBoneNode); skeleton = $kBoneNode })
    animations = $animations
    accessors = $accessors
    bufferViews = $bufferViews
    buffers = @([ordered]@{ byteLength = $binBytes.Length })
}
$json = $gltf | ConvertTo-Json -Depth 32 -Compress
$jsonBytes = [System.Text.Encoding]::UTF8.GetBytes($json)
$pad = (4 - ($jsonBytes.Length % 4)) % 4
$jsonBytes = [byte[]]($jsonBytes + ([byte[]](, 0x20 * $pad)))

$total = 12 + 8 + $jsonBytes.Length + 8 + $binBytes.Length
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter($fs)
$w.Write([uint32]0x46546C67); $w.Write([uint32]2); $w.Write([uint32]$total)
$w.Write([uint32]$jsonBytes.Length); $w.Write([uint32]0x4E4F534A); $w.Write($jsonBytes)
$w.Write([uint32]$binBytes.Length); $w.Write([uint32]0x004E4942); $w.Write($binBytes)
$w.Close()
Write-Host ('wrote {0} ({1} bytes)' -f $out, $total)
