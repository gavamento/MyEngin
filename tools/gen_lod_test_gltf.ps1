# assets\models\lod_sphere.glb を生成する (M90e のメッシュ LOD の検証素材。render_bench が使う)。
#
# 高ポリの UV 球 1 個 (64 x 32 分割 = 3968 三角形、継ぎ目とポールを持つ)。単純化の入力として実戦的な形で、
# 隣の lod_sphere.glb.meta に "lod" を書いて段を作る。インデックスは uint16 (頂点 2145 個)。
# マテリアルは持たない (使う側が差し替える)。
#
#   pwsh tools\gen_lod_test_gltf.ps1

param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\assets\models'),
    [int]$Rings = 32,
    [int]$Segments = 64
)

$ErrorActionPreference = 'Stop'
$out = Join-Path $OutDir 'lod_sphere.glb'
$radius = 0.5

# 頂点: 位置 / 法線 / UV
$vertexCount = ($Rings + 1) * ($Segments + 1)
$pos = New-Object 'single[]' ($vertexCount * 3)
$nrm = New-Object 'single[]' ($vertexCount * 3)
$uv = New-Object 'single[]' ($vertexCount * 2)
$v = 0
for ($r = 0; $r -le $Rings; $r++) {
    $phi = [math]::PI * $r / $Rings
    for ($s = 0; $s -le $Segments; $s++) {
        $theta = 2.0 * [math]::PI * $s / $Segments
        $nx = [math]::Sin($phi) * [math]::Cos($theta)
        $ny = [math]::Cos($phi)
        $nz = [math]::Sin($phi) * [math]::Sin($theta)
        $nrm[$v * 3 + 0] = $nx; $nrm[$v * 3 + 1] = $ny; $nrm[$v * 3 + 2] = $nz
        $pos[$v * 3 + 0] = $nx * $radius; $pos[$v * 3 + 1] = $ny * $radius; $pos[$v * 3 + 2] = $nz * $radius
        $uv[$v * 2 + 0] = $s / $Segments; $uv[$v * 2 + 1] = $r / $Rings
        $v++
    }
}

# インデックス: 極の縮退三角形は作らない
$indices = New-Object System.Collections.Generic.List[uint16]
for ($r = 0; $r -lt $Rings; $r++) {
    for ($s = 0; $s -lt $Segments; $s++) {
        $a = $r * ($Segments + 1) + $s
        $b = $a + $Segments + 1
        if ($r -ne 0) { $indices.Add($a); $indices.Add($b); $indices.Add($a + 1) }
        if ($r -ne $Rings - 1) { $indices.Add($a + 1); $indices.Add($b); $indices.Add($b + 1) }
    }
}

function Pad4([byte[]]$bytes) {
    $pad = (4 - ($bytes.Length % 4)) % 4
    if ($pad -eq 0) { return $bytes }
    return $bytes + (New-Object 'byte[]' $pad)
}
function FloatBytes([single[]]$a) { $b = New-Object 'byte[]' ($a.Length * 4); [Buffer]::BlockCopy($a, 0, $b, 0, $b.Length); return $b }

$posBytes = FloatBytes $pos
$nrmBytes = FloatBytes $nrm
$uvBytes = FloatBytes $uv
$idxArray = $indices.ToArray()
$idxBytes = New-Object 'byte[]' ($idxArray.Length * 2)
[Buffer]::BlockCopy($idxArray, 0, $idxBytes, 0, $idxBytes.Length)
$idxPadded = Pad4 $idxBytes

$posOffset = 0
$nrmOffset = $posOffset + $posBytes.Length
$uvOffset = $nrmOffset + $nrmBytes.Length
$idxOffset = $uvOffset + $uvBytes.Length
[byte[]]$bin = $posBytes + $nrmBytes + $uvBytes + $idxPadded

$inv = [System.Globalization.CultureInfo]::InvariantCulture
$json = '{"asset":{"version":"2.0","generator":"gen_lod_test_gltf.ps1"},"scene":0,"scenes":[{"nodes":[0]}],' +
    '"nodes":[{"name":"LodSphere","mesh":0}],' +
    '"meshes":[{"name":"LodSphere","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3}]}],' +
    ('"accessors":[{{"bufferView":0,"componentType":5126,"count":{0},"type":"VEC3","min":[-{1},-{1},-{1}],"max":[{1},{1},{1}]}},' -f $vertexCount, $radius.ToString($inv)) +
    ('{{"bufferView":1,"componentType":5126,"count":{0},"type":"VEC3"}},' -f $vertexCount) +
    ('{{"bufferView":2,"componentType":5126,"count":{0},"type":"VEC2"}},' -f $vertexCount) +
    ('{{"bufferView":3,"componentType":5123,"count":{0},"type":"SCALAR"}}],' -f $idxArray.Length) +
    ('"bufferViews":[{{"buffer":0,"byteOffset":{0},"byteLength":{1}}},' -f $posOffset, $posBytes.Length) +
    ('{{"buffer":0,"byteOffset":{0},"byteLength":{1}}},' -f $nrmOffset, $nrmBytes.Length) +
    ('{{"buffer":0,"byteOffset":{0},"byteLength":{1}}},' -f $uvOffset, $uvBytes.Length) +
    ('{{"buffer":0,"byteOffset":{0},"byteLength":{1}}}],' -f $idxOffset, $idxBytes.Length) +
    ('"buffers":[{{"byteLength":{0}}}]}}' -f $bin.Length)
$jsonBytes = Pad4 ([System.Text.Encoding]::UTF8.GetBytes($json))
# JSON チャンクの余白は空白 (0x20) で埋める決まり
for ($i = [System.Text.Encoding]::UTF8.GetByteCount($json); $i -lt $jsonBytes.Length; $i++) { $jsonBytes[$i] = 0x20 }

$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($ms)
$total = 12 + 8 + $jsonBytes.Length + 8 + $bin.Length
$w.Write([uint32]0x46546C67); $w.Write([uint32]2); $w.Write([uint32]$total)
$w.Write([uint32]$jsonBytes.Length); $w.Write([uint32]0x4E4F534A); $w.Write([byte[]]$jsonBytes)
$w.Write([uint32]$bin.Length); $w.Write([uint32]0x004E4942); $w.Write([byte[]]$bin)
$w.Flush()
[System.IO.File]::WriteAllBytes($out, $ms.ToArray())
Write-Host ("wrote {0} ({1} vertices, {2} triangles, {3} bytes)" -f $out, $vertexCount, ($idxArray.Length / 3), $total)
