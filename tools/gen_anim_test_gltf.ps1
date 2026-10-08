# assets\models\anim_test.glb と anim_test_zup.glb を生成する (M89 の骨アニメの回帰・デモ素材)。
#
# 外部ツール無しで「名前付きの複数クリップを持つスキン付き glTF」を自給するためのもの。
# CesiumMan.glb はクリップが 1 本で名前も無いので、名前で骨クリップを引くコントローラ (M89b) や
# ブレンドツリー (M89d/e)、ルートモーション (M89j)、2 ボーン IK (M89l) の試験に使えない。
#
# スケルトン (glTF の Y-up、正面 = +Z、左 = +X。バインドの回転はすべて恒等):
#   Root (足元、ルートモーションを持つ) -> Hips -> Spine -> Chest -> Head
#   Chest -> UpperArm.L/R -> LowerArm.L/R -> Hand.L/R
#   Hips  -> UpperLeg.L/R -> LowerLeg.L/R -> Foot.L/R     (脚は 2 ボーン IK の対象になる長さ)
# メッシュ: 骨ごとの箱 (頂点はその骨に重み 1)。Root には向きの分かる板、Head には鼻を付ける。
# クリップ (名前 / 長さ / ループ前提):
#   Idle   2.0 s  ループ  その場で呼吸
#   Walk   1.0 s  ループ  Root が +Z へ 1.4 m 進む (1.4 m/s)。t=0 で左脚が中立から前へ出始める
#   Run    0.6 s  ループ  Root が +Z へ 2.4 m 進む (4.0 m/s)。Walk と同じ位相の取り方 (ブレンドの位相同期用)
#   Attack 0.8 s  一度きり 右腕を振りかぶって振り下ろす。その場
# キーは 30 fps で両端を含めて打つ (LINEAR)。Walk / Run の Root の最終キーは 1 周ぶんの移動量
# (ループの折り返しで Root が戻る = ルートモーションの抽出側が折り返しを扱う前提の素材)。
#
# -ZUp を付けると同じ骨格を Z-up で書き、非ジョイントの "Z_UP" ノード (X 軸 -90 度) の下に置いた版を出す。
# CesiumMan と同じ構図 (ルートジョイントの親空間の上向きが +Z) で、ルートモーションの上向きの求め方を試す。

param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\assets\models'),
    [switch]$ZUp
)

$ErrorActionPreference = 'Stop'
$name = if ($ZUp) { 'anim_test_zup' } else { 'anim_test' }
$out = Join-Path $OutDir "$name.glb"
$inv = [System.Globalization.CultureInfo]::InvariantCulture

# ---- 座標の書き方 -----------------------------------------------------------
# Y-up で組み立てた値を、出力する空間へ写す。Z-up 版は (x, y, z) -> (x, -z, y)。
# Z_UP ノードの回転 Rx(-90) がこれを Y-up へ戻すので、見た目は Y-up 版と同じになる
function Out-Vec([double[]]$v) {
    if ($ZUp) { return @($v[0], - $v[2], $v[1]) }
    return @($v[0], $v[1], $v[2])
}
# クォータニオン (x, y, z, w) は回転軸を同じ写像で移す
function Out-Quat([double[]]$q) {
    if ($ZUp) { return @($q[0], - $q[2], $q[1], $q[3]) }
    return @($q[0], $q[1], $q[2], $q[3])
}
function Deg([double]$d) { return $d * [math]::PI / 180.0 }
function Quat-Axis([double]$ax, [double]$ay, [double]$az, [double]$rad) {
    $s = [math]::Sin($rad / 2.0)
    return @(($ax * $s), ($ay * $s), ($az * $s), [math]::Cos($rad / 2.0))
}
function Quat-Mul([double[]]$a, [double[]]$b) {
    return @(
        ($a[3] * $b[0] + $a[0] * $b[3] + $a[1] * $b[2] - $a[2] * $b[1]),
        ($a[3] * $b[1] - $a[0] * $b[2] + $a[1] * $b[3] + $a[2] * $b[0]),
        ($a[3] * $b[2] + $a[0] * $b[1] - $a[1] * $b[0] + $a[2] * $b[3]),
        ($a[3] * $b[3] - $a[0] * $b[0] - $a[1] * $b[1] - $a[2] * $b[2]))
}
function RotX([double]$deg) { return Quat-Axis 1 0 0 (Deg $deg) }
function RotY([double]$deg) { return Quat-Axis 0 1 0 (Deg $deg) }
function RotZ([double]$deg) { return Quat-Axis 0 0 1 (Deg $deg) }

# ---- スケルトン ---------------------------------------------------------------
# t = 親からの平行移動 (Y-up)。X 回転の正 = 下向きの骨が後ろ (-Z) へ振れる / 上向きの骨が前へ倒れる
$joints = @(
    @{ n = 'Root';       p = -1; t = @(0, 0, 0) },
    @{ n = 'Hips';       p = 0;  t = @(0, 0.95, 0) },
    @{ n = 'Spine';      p = 1;  t = @(0, 0.12, 0) },
    @{ n = 'Chest';      p = 2;  t = @(0, 0.24, 0) },
    @{ n = 'Head';       p = 3;  t = @(0, 0.30, 0) },
    @{ n = 'UpperArm.L'; p = 3;  t = @(0.22, 0.22, 0) },
    @{ n = 'LowerArm.L'; p = 5;  t = @(0, -0.28, 0) },
    @{ n = 'Hand.L';     p = 6;  t = @(0, -0.25, 0) },
    @{ n = 'UpperArm.R'; p = 3;  t = @(-0.22, 0.22, 0) },
    @{ n = 'LowerArm.R'; p = 8;  t = @(0, -0.28, 0) },
    @{ n = 'Hand.R';     p = 9;  t = @(0, -0.25, 0) },
    @{ n = 'UpperLeg.L'; p = 1;  t = @(0.10, -0.05, 0) },
    @{ n = 'LowerLeg.L'; p = 11; t = @(0, -0.42, 0) },
    @{ n = 'Foot.L';     p = 12; t = @(0, -0.40, 0) },
    @{ n = 'UpperLeg.R'; p = 1;  t = @(-0.10, -0.05, 0) },
    @{ n = 'LowerLeg.R'; p = 14; t = @(0, -0.42, 0) },
    @{ n = 'Foot.R';     p = 15; t = @(0, -0.40, 0) }
)
$jointIndex = @{}
for ($i = 0; $i -lt $joints.Count; $i++) { $jointIndex[$joints[$i].n] = $i }
# バインドのグローバル位置 (Y-up)。回転が恒等なので平行移動の和
$global = @()
for ($i = 0; $i -lt $joints.Count; $i++) {
    $t = $joints[$i].t
    if ($joints[$i].p -lt 0) { $global += , @($t[0], $t[1], $t[2]) }
    else {
        $g = $global[$joints[$i].p]
        $global += , @(($g[0] + $t[0]), ($g[1] + $t[1]), ($g[2] + $t[2]))
    }
}

# ---- メッシュ (骨ごとの箱) -----------------------------------------------------
# c = 箱の中心 (その骨のバインド位置からの相対、Y-up)、h = 半径
$boxes = @(
    @{ j = 'Root';       c = @(0, 0.005, 0);    h = @(0.12, 0.005, 0.12) },
    @{ j = 'Root';       c = @(0, 0.005, 0.17); h = @(0.03, 0.005, 0.05) },
    @{ j = 'Hips';       c = @(0, 0.04, 0);     h = @(0.17, 0.08, 0.10) },
    @{ j = 'Spine';      c = @(0, 0.12, 0);     h = @(0.15, 0.12, 0.09) },
    @{ j = 'Chest';      c = @(0, 0.15, 0);     h = @(0.19, 0.15, 0.11) },
    @{ j = 'Head';       c = @(0, 0.12, 0);     h = @(0.10, 0.12, 0.10) },
    @{ j = 'Head';       c = @(0, 0.12, 0.12);  h = @(0.03, 0.03, 0.04) },
    @{ j = 'UpperArm.L'; c = @(0, -0.14, 0);    h = @(0.05, 0.14, 0.05) },
    @{ j = 'LowerArm.L'; c = @(0, -0.125, 0);   h = @(0.045, 0.125, 0.045) },
    @{ j = 'Hand.L';     c = @(0, -0.06, 0);    h = @(0.04, 0.06, 0.03) },
    @{ j = 'UpperArm.R'; c = @(0, -0.14, 0);    h = @(0.05, 0.14, 0.05) },
    @{ j = 'LowerArm.R'; c = @(0, -0.125, 0);   h = @(0.045, 0.125, 0.045) },
    @{ j = 'Hand.R';     c = @(0, -0.06, 0);    h = @(0.04, 0.06, 0.03) },
    @{ j = 'UpperLeg.L'; c = @(0, -0.21, 0);    h = @(0.065, 0.21, 0.065) },
    @{ j = 'LowerLeg.L'; c = @(0, -0.20, 0);    h = @(0.055, 0.20, 0.055) },
    @{ j = 'Foot.L';     c = @(0, -0.04, 0.05); h = @(0.05, 0.04, 0.11) },
    @{ j = 'UpperLeg.R'; c = @(0, -0.21, 0);    h = @(0.065, 0.21, 0.065) },
    @{ j = 'LowerLeg.R'; c = @(0, -0.20, 0);    h = @(0.055, 0.20, 0.055) },
    @{ j = 'Foot.R';     c = @(0, -0.04, 0.05); h = @(0.05, 0.04, 0.11) }
)
# 面ごとに (法線, 2 本の接線) を持つ。頂点は c + n*h + (±u)*h + (±v)*h
$faces = @(
    @{ n = @(1, 0, 0);  u = @(0, 1, 0); v = @(0, 0, 1) },
    @{ n = @(-1, 0, 0); u = @(0, 0, 1); v = @(0, 1, 0) },
    @{ n = @(0, 1, 0);  u = @(0, 0, 1); v = @(1, 0, 0) },
    @{ n = @(0, -1, 0); u = @(1, 0, 0); v = @(0, 0, 1) },
    @{ n = @(0, 0, 1);  u = @(1, 0, 0); v = @(0, 1, 0) },
    @{ n = @(0, 0, -1); u = @(0, 1, 0); v = @(1, 0, 0) }
)
$positions = New-Object System.Collections.Generic.List[double[]]
$normals = New-Object System.Collections.Generic.List[double[]]
$jointsAttr = New-Object System.Collections.Generic.List[int]
$indices = New-Object System.Collections.Generic.List[int]
foreach ($b in $boxes) {
    $ji = $jointIndex[$b.j]
    $g = $global[$ji]
    foreach ($f in $faces) {
        $base = $positions.Count
        foreach ($s in @(@(-1, -1), @(1, -1), @(1, 1), @(-1, 1))) {
            $p = @(0.0, 0.0, 0.0)
            for ($k = 0; $k -lt 3; $k++) {
                $p[$k] = $g[$k] + $b.c[$k] + ($f.n[$k] + $s[0] * $f.u[$k] + $s[1] * $f.v[$k]) * $b.h[$k]
            }
            $positions.Add((Out-Vec $p))
            $normals.Add((Out-Vec $f.n))
            $jointsAttr.Add($ji)
        }
        # u x v = n になるように並べてあるので、この順で反時計回り (glTF の表)
        $indices.AddRange([int[]]@($base, ($base + 1), ($base + 2), $base, ($base + 2), ($base + 3)))
    }
}

# ---- クリップ -----------------------------------------------------------------
# 各クリップは「時刻 (秒) -> ジョイント名ごとの回転 / Root と Hips の平行移動」を返す関数で書き、30 fps で標本化する
$fps = 30
function Pose-Idle([double]$t) {
    $p = 2.0 * [math]::PI * $t / 2.0
    $breath = [math]::Sin($p)
    return @{
        rot = @{
            'Chest'      = RotX (2.0 * $breath)
            'Head'       = RotY (5.0 * [math]::Sin($p * 0.5))
            'UpperArm.L' = RotZ (4.0 + 2.0 * $breath)
            'UpperArm.R' = RotZ (-4.0 - 2.0 * $breath)
            'LowerArm.L' = RotX (-10.0)
            'LowerArm.R' = RotX (-10.0)
        }
        hips = @(0, (0.95 - 0.01 * (1.0 - [math]::Cos($p)) / 2.0), 0)
        root = @(0, 0, 0)
    }
}
# 歩き / 走りの共通形。位相 0 で両脚が中立、左脚が前へ出始める (Walk と Run で位相をそろえる)
function Pose-Gait([double]$t, [double]$period, [double]$speed, [double]$legAmp, [double]$kneeAmp,
                   [double]$armAmp, [double]$elbow, [double]$lean, [double]$bob) {
    $p = 2.0 * [math]::PI * $t / $period
    $s = [math]::Sin($p)
    return @{
        rot = @{
            'UpperLeg.L' = RotX (- $legAmp * $s)
            'UpperLeg.R' = RotX ($legAmp * $s)
            'LowerLeg.L' = RotX (8.0 + $kneeAmp * [math]::Max(0.0, - $s))
            'LowerLeg.R' = RotX (8.0 + $kneeAmp * [math]::Max(0.0, $s))
            'Foot.L'     = RotX (-5.0 * $s)
            'Foot.R'     = RotX (5.0 * $s)
            'UpperArm.L' = RotX ($armAmp * $s)
            'UpperArm.R' = RotX (- $armAmp * $s)
            'LowerArm.L' = RotX ($elbow)
            'LowerArm.R' = RotX ($elbow)
            'Spine'      = RotX ($lean)
            'Chest'      = RotY (6.0 * $s)
        }
        hips = @(0, (0.95 - $bob * (1.0 - [math]::Cos(2.0 * $p)) / 2.0), 0)
        root = @(0, 0, ($speed * $t))
    }
}
function Pose-Walk([double]$t) { return Pose-Gait $t 1.0 1.4 25.0 35.0 20.0 -15.0 3.0 0.03 }
function Pose-Run([double]$t) { return Pose-Gait $t 0.6 4.0 45.0 75.0 35.0 -70.0 12.0 0.05 }
# 区分的に滑らかに補間する (0..1 の smoothstep)
function Ease([double]$a, [double]$b, [double]$x) {
    $u = [math]::Min(1.0, [math]::Max(0.0, $x))
    $u = $u * $u * (3.0 - 2.0 * $u)
    return $a + ($b - $a) * $u
}
function Pose-Attack([double]$t) {
    # 0 -> 0.30 s 振りかぶる / 0.30 -> 0.42 s 振り下ろす / 0.42 -> 0.80 s 戻す
    if ($t -lt 0.30) { $arm = Ease 0 -160 ($t / 0.30); $twist = Ease 0 -20 ($t / 0.30) }
    elseif ($t -lt 0.42) { $arm = Ease -160 -40 (($t - 0.30) / 0.12); $twist = Ease -20 25 (($t - 0.30) / 0.12) }
    else { $arm = Ease -40 0 (($t - 0.42) / 0.38); $twist = Ease 25 0 (($t - 0.42) / 0.38) }
    return @{
        rot = @{
            'UpperArm.R' = RotX $arm
            'LowerArm.R' = RotX (-20.0)
            'Chest'      = RotY $twist
            'UpperLeg.L' = RotX (-15.0)
            'UpperLeg.R' = RotX (10.0)
            'LowerLeg.R' = RotX (15.0)
        }
        hips = @(0, 0.93, 0)
        root = @(0, 0, 0)
    }
}
$clips = @(
    @{ name = 'Idle';   dur = 2.0; fn = 'Pose-Idle' },
    @{ name = 'Walk';   dur = 1.0; fn = 'Pose-Walk' },
    @{ name = 'Run';    dur = 0.6; fn = 'Pose-Run' },
    @{ name = 'Attack'; dur = 0.8; fn = 'Pose-Attack' }
)

# ---- バイナリと JSON --------------------------------------------------------
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

$posAcc = Add-FloatAccessor $positions 3 'VEC3' $true
$nrmAcc = Add-FloatAccessor $normals 3 'VEC3' $false
$wRows = New-Object System.Collections.Generic.List[double[]]
foreach ($ji in $jointsAttr) { $wRows.Add(@(1.0, 0.0, 0.0, 0.0)) }
$wgtAcc = Add-FloatAccessor $wRows 4 'VEC4' $false
# JOINTS_0 は unsigned byte x4
Align4
$jOffset = [int]$bin.Length
foreach ($ji in $jointsAttr) { $bw.Write([byte]$ji); $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([byte]0) }
$bufferViews.Add([ordered]@{ buffer = 0; byteOffset = $jOffset; byteLength = ([int]$bin.Length - $jOffset) })
$accessors.Add([ordered]@{ bufferView = $bufferViews.Count - 1; componentType = 5121; count = $jointsAttr.Count; type = 'VEC4' })
$jntAcc = $accessors.Count - 1
# インデックスは unsigned short
Align4
$iOffset = [int]$bin.Length
foreach ($ix in $indices) { $bw.Write([uint16]$ix) }
$bufferViews.Add([ordered]@{ buffer = 0; byteOffset = $iOffset; byteLength = ([int]$bin.Length - $iOffset) })
$accessors.Add([ordered]@{ bufferView = $bufferViews.Count - 1; componentType = 5123; count = $indices.Count; type = 'SCALAR' })
$idxAcc = $accessors.Count - 1

# inverse bind = バインドのグローバル位置の逆の平行移動 (列優先、平行移動は [12..14])
$ibm = New-Object System.Collections.Generic.List[double[]]
foreach ($g in $global) {
    $o = Out-Vec $g
    $ibm.Add(@(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, (- $o[0]), (- $o[1]), (- $o[2]), 1))
}
$ibmAcc = Add-FloatAccessor $ibm 16 'MAT4' $false

# ノード: 0 = 包み (Armature / Z_UP)、1 = メッシュ、2.. = ジョイント
$kFirstJointNode = 2
$nodes = New-Object System.Collections.Generic.List[object]
$wrapper = [ordered]@{ name = $(if ($ZUp) { 'Z_UP' } else { 'Armature' }); children = @(1, $kFirstJointNode) }
if ($ZUp) { $wrapper.rotation = @((-[math]::Sqrt(0.5)), 0, 0, [math]::Sqrt(0.5)) }
$nodes.Add($wrapper)
$nodes.Add([ordered]@{ name = 'Body'; mesh = 0; skin = 0 })
for ($i = 0; $i -lt $joints.Count; $i++) {
    $node = [ordered]@{ name = $joints[$i].n; translation = (Out-Vec $joints[$i].t) }
    $kids = @()
    for ($c = 0; $c -lt $joints.Count; $c++) { if ($joints[$c].p -eq $i) { $kids += ($c + $kFirstJointNode) } }
    if ($kids.Count -gt 0) { $node.children = $kids }
    $nodes.Add($node)
}

# アニメーション。回転は「どこかの時刻で恒等でない」ジョイントだけチャネルを持つ (残りはバインド = 恒等)
$animations = New-Object System.Collections.Generic.List[object]
foreach ($clip in $clips) {
    $frames = [int][math]::Round($clip.dur * $fps)
    $times = New-Object System.Collections.Generic.List[double[]]
    $samples = @()
    for ($k = 0; $k -le $frames; $k++) {
        $t = $k / [double]$fps
        $times.Add(@($t))
        $samples += , (& $clip.fn $t)
    }
    $timeAcc = Add-FloatAccessor $times 1 'SCALAR' $true
    $samplers = New-Object System.Collections.Generic.List[object]
    $channels = New-Object System.Collections.Generic.List[object]
    $addChannel = {
        param([string]$jointName, [string]$path, [System.Collections.IList]$rows, [int]$comps, [string]$type)
        $acc = Add-FloatAccessor $rows $comps $type $false
        $samplers.Add([ordered]@{ input = $timeAcc; output = $acc; interpolation = 'LINEAR' })
        $channels.Add([ordered]@{ sampler = $samplers.Count - 1;
                                  target = [ordered]@{ node = ($jointIndex[$jointName] + $kFirstJointNode); path = $path } })
    }
    foreach ($tn in @('Root', 'Hips')) {
        $rows = New-Object System.Collections.Generic.List[double[]]
        foreach ($s in $samples) { $rows.Add((Out-Vec $(if ($tn -eq 'Root') { $s.root } else { $s.hips }))) }
        & $addChannel $tn 'translation' $rows 3 'VEC3'
    }
    foreach ($j in $joints) {
        if (-not $samples[0].rot.ContainsKey($j.n)) { continue }
        $rows = New-Object System.Collections.Generic.List[double[]]
        foreach ($s in $samples) { $rows.Add((Out-Quat $s.rot[$j.n])) }
        & $addChannel $j.n 'rotation' $rows 4 'VEC4'
    }
    $animations.Add([ordered]@{ name = $clip.name; samplers = $samplers; channels = $channels })
}

Align4
$binBytes = $bin.ToArray()
$jointNodes = @(); for ($i = 0; $i -lt $joints.Count; $i++) { $jointNodes += ($i + $kFirstJointNode) }
$gltf = [ordered]@{
    asset = [ordered]@{ version = '2.0'; generator = 'MyEngine tools/gen_anim_test_gltf.ps1' }
    scene = 0
    scenes = @([ordered]@{ nodes = @(0) })
    nodes = $nodes
    meshes = @([ordered]@{ name = 'Body'; primitives = @([ordered]@{
                attributes = [ordered]@{ POSITION = $posAcc; NORMAL = $nrmAcc; JOINTS_0 = $jntAcc; WEIGHTS_0 = $wgtAcc }
                indices = $idxAcc; material = 0 }) })
    materials = @([ordered]@{ name = 'AnimTestBody'; pbrMetallicRoughness = [ordered]@{
                baseColorFactor = @(0.80, 0.62, 0.45, 1.0); metallicFactor = 0.0; roughnessFactor = 0.8 } })
    skins = @([ordered]@{ inverseBindMatrices = $ibmAcc; joints = $jointNodes; skeleton = $kFirstJointNode })
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
Write-Host ('wrote {0} ({1} bytes, {2} joints, {3} clips)' -f $out, $total, $joints.Count, $clips.Count)
