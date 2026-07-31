# assets\models\skinned_beam.fbx を生成する (FBX P4 = スキン + bake_anim の回帰アセット)。
#
# 手書き ASCII FBX。外部ツール (Maya/Blender/Mixamo) 無しでスキン付き FBX を自給するためのもので、
# FbxLoader のスキンクラスタ / ウェイト / bake_anim 経路を通す最小構成になっている。
#
# ★スケルトンをわざと **非恒等な "Armature" ノードの下** に置いてある。
#   「ジョイント集合内だけで親を辿る」実装 (glTF ローダの規約) だとここが抜け落ちてポーズが
#   ズレる — その退行が黙って通らないようにするのがこのアセットの主目的。
#
# ジオメトリ: +Y 方向の角柱。5 リング (y = 0..4) x 4 隅 (x,z = ±0.5)、両面化 (巻き順による偽陰性回避)。
# バインド:   Armature T(3,0,0) -> Bone1 T(-3,0,0) -> Bone2 T(0,2,0)
#             = ボーンのワールド位置は (0,0,0) と (0,2,0) でメッシュの中を通る (正しいリグ) が、
#               Armature を無視すると Bone1 が x=-3 に飛ぶので祖先の取りこぼしは必ず露見する。
#             ウェイトは リング0,1 = Bone1 / リング2 = 50:50 / リング3,4 = Bone2
# アニメ:     Bone2 の Lcl Rotation Z が 1 秒で 0 -> 90 度 (y=2 を支点に XY 平面で曲がる)

param(
    [string]$Name = 'skinned_beam',
    [string]$OutDir = (Join-Path $PSScriptRoot '..\assets\models'),
    [double]$StartSec = 0.0   # non-zero exercises ufbx_bake_opts.trim_start_time
)

$ErrorActionPreference = 'Stop'
$out = Join-Path $OutDir "$Name.fbx"

# --- geometry -------------------------------------------------------------
$rings = 5
$corner = @(@(-0.5, -0.5), @(0.5, -0.5), @(0.5, 0.5), @(-0.5, 0.5))
$verts = New-Object System.Collections.Generic.List[string]
for ($r = 0; $r -lt $rings; $r++) {
    foreach ($c in $corner) {
        $verts.Add(('{0},{1},{2}' -f $c[0], $r, $c[1]))
    }
}

# Polygons: last index of every polygon is encoded as (-idx - 1).
$polys = New-Object System.Collections.Generic.List[string]
function Add-Quad([int]$a, [int]$b, [int]$c, [int]$d) {
    $polys.Add(('{0},{1},{2},{3}' -f $a, $b, $c, (-$d - 1)))
}
for ($r = 0; $r -lt ($rings - 1); $r++) {
    for ($c = 0; $c -lt 4; $c++) {
        $c1 = ($c + 1) % 4
        $a = $r * 4 + $c; $b = $r * 4 + $c1; $cc = ($r + 1) * 4 + $c1; $d = ($r + 1) * 4 + $c
        Add-Quad $a $b $cc $d
        Add-Quad $d $cc $b $a   # reversed copy: double-sided so winding cannot cause a false negative
    }
}
$top = ($rings - 1) * 4
Add-Quad 0 1 2 3
Add-Quad 3 2 1 0
Add-Quad $top ($top + 1) ($top + 2) ($top + 3)
Add-Quad ($top + 3) ($top + 2) ($top + 1) $top

# --- skin weights ---------------------------------------------------------
# ring0,1 -> bone1 ; ring2 -> 50/50 ; ring3,4 -> bone2
$idx1 = @(); $w1 = @(); $idx2 = @(); $w2 = @()
for ($v = 0; $v -lt ($rings * 4); $v++) {
    $ring = [int][math]::Floor($v / 4)
    if ($ring -le 1) { $idx1 += $v; $w1 += 1 }
    elseif ($ring -eq 2) { $idx1 += $v; $w1 += 0.5; $idx2 += $v; $w2 += 0.5 }
    else { $idx2 += $v; $w2 += 1 }
}

function Mat4([double]$tx, [double]$ty, [double]$tz) {
    # FBX stores 16 doubles with translation at [12..14]
    return "1,0,0,0,0,1,0,0,0,0,1,0,$tx,$ty,$tz,1"
}
function Arr($name, $values, $perLine = 12) {
    $s = ($values -join ',')
    return "`t`t$name`: *$($values.Count) {`n`t`t`ta: $s`n`t`t}"
}

$vertsCount = $verts.Count * 3
$vertsStr = ($verts -join ',')
$polyStr = ($polys -join ',')
$polyCount = ($polyStr -split ',').Count
$kTime = 46186158000
$t0 = [int64]($StartSec * 46186158000)
$t1 = $t0 + $kTime

$fbx = @"
; FBX 7.4.0 project file
; Hand-authored by MyEngine tools for FBX P4 (skin + bake_anim) verification
;----------------------------------------------------

FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
	Creator: "MyEngine P4 handmade"
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "OriginalUpAxis", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",100
		P: "OriginalUnitScaleFactor", "double", "Number", "",100
		P: "TimeMode", "enum", "", "",6
		P: "TimeSpanStart", "KTime", "Time", "",$t0
		P: "TimeSpanStop", "KTime", "Time", "",$t1
	}
}

Definitions:  {
	Version: 100
	Count: 16
	ObjectType: "GlobalSettings" {
		Count: 1
	}
	ObjectType: "Geometry" {
		Count: 1
	}
	ObjectType: "Model" {
		Count: 4
	}
	ObjectType: "NodeAttribute" {
		Count: 2
	}
	ObjectType: "Material" {
		Count: 1
	}
	ObjectType: "Deformer" {
		Count: 3
	}
	ObjectType: "AnimationStack" {
		Count: 1
	}
	ObjectType: "AnimationLayer" {
		Count: 1
	}
	ObjectType: "AnimationCurveNode" {
		Count: 1
	}
	ObjectType: "AnimationCurve" {
		Count: 3
	}
}

Objects:  {
	Geometry: 100, "Geometry::Beam", "Mesh" {
		Vertices: *$vertsCount {
			a: $vertsStr
		}
		PolygonVertexIndex: *$polyCount {
			a: $polyStr
		}
		GeometryVersion: 124
		Layer: 0 {
			Version: 100
		}
	}
	Model: 200, "Model::Beam", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,0,0
			P: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0
			P: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
		}
		Shading: T
		Culling: "CullingOff"
	}
	Model: 210, "Model::Armature", "Null" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",3,0,0
		}
		Shading: T
		Culling: "CullingOff"
	}
	Model: 220, "Model::Bone1", "LimbNode" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",-3,0,0
			P: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0
			P: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
		}
		Shading: T
		Culling: "CullingOff"
	}
	Model: 230, "Model::Bone2", "LimbNode" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,2,0
			P: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0
			P: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1
		}
		Shading: T
		Culling: "CullingOff"
	}
	NodeAttribute: 221, "NodeAttribute::Bone1", "LimbNode" {
		TypeFlags: "Skeleton"
	}
	NodeAttribute: 231, "NodeAttribute::Bone2", "LimbNode" {
		TypeFlags: "Skeleton"
	}
	Material: 300, "Material::MatBeam", "" {
		Version: 102
		ShadingModel: "phong"
		MultiLayer: 0
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.85,0.45,0.15
		}
	}
	Deformer: 400, "Deformer::SkinBeam", "Skin" {
		Version: 101
		Link_DeformAcuracy: 50
	}
	Deformer: 410, "SubDeformer::Cluster1", "Cluster" {
		Version: 100
		UserData: "", ""
		Indexes: *$($idx1.Count) {
			a: $($idx1 -join ',')
		}
		Weights: *$($w1.Count) {
			a: $($w1 -join ',')
		}
		Transform: *16 {
			a: $(Mat4 0 0 0)
		}
		TransformLink: *16 {
			a: $(Mat4 0 0 0)
		}
	}
	Deformer: 420, "SubDeformer::Cluster2", "Cluster" {
		Version: 100
		UserData: "", ""
		Indexes: *$($idx2.Count) {
			a: $($idx2 -join ',')
		}
		Weights: *$($w2.Count) {
			a: $($w2 -join ',')
		}
		Transform: *16 {
			a: $(Mat4 0 -2 0)
		}
		TransformLink: *16 {
			a: $(Mat4 0 2 0)
		}
	}
	AnimationStack: 500, "AnimStack::Take 001", "" {
		Properties70:  {
			P: "LocalStart", "KTime", "Time", "",$t0
			P: "LocalStop", "KTime", "Time", "",$t1
			P: "ReferenceStart", "KTime", "Time", "",$t0
			P: "ReferenceStop", "KTime", "Time", "",$t1
		}
	}
	AnimationLayer: 510, "AnimLayer::BaseLayer", "" {
	}
	AnimationCurveNode: 520, "AnimCurveNode::R", "" {
		Properties70:  {
			P: "d|X", "Number", "", "A",0
			P: "d|Y", "Number", "", "A",0
			P: "d|Z", "Number", "", "A",0
		}
	}
	AnimationCurve: 530, "AnimCurve::", "" {
		Default: 0
		KeyVer: 4008
		KeyTime: *2 {
			a: $t0,$t1
		}
		KeyValueFloat: *2 {
			a: 0,0
		}
		KeyAttrFlags: *1 {
			a: 24580
		}
		KeyAttrDataFloat: *4 {
			a: 0,0,0,0
		}
		KeyAttrRefCount: *1 {
			a: 2
		}
	}
	AnimationCurve: 531, "AnimCurve::", "" {
		Default: 0
		KeyVer: 4008
		KeyTime: *2 {
			a: $t0,$t1
		}
		KeyValueFloat: *2 {
			a: 0,0
		}
		KeyAttrFlags: *1 {
			a: 24580
		}
		KeyAttrDataFloat: *4 {
			a: 0,0,0,0
		}
		KeyAttrRefCount: *1 {
			a: 2
		}
	}
	AnimationCurve: 532, "AnimCurve::", "" {
		Default: 0
		KeyVer: 4008
		KeyTime: *2 {
			a: $t0,$t1
		}
		KeyValueFloat: *2 {
			a: 0,90
		}
		KeyAttrFlags: *1 {
			a: 24580
		}
		KeyAttrDataFloat: *4 {
			a: 0,0,0,0
		}
		KeyAttrRefCount: *1 {
			a: 2
		}
	}
}

Connections:  {
	C: "OO",200,0
	C: "OO",100,200
	C: "OO",300,200
	C: "OO",210,0
	C: "OO",220,210
	C: "OO",230,220
	C: "OO",221,220
	C: "OO",231,230
	C: "OO",400,100
	C: "OO",410,400
	C: "OO",420,400
	C: "OO",220,410
	C: "OO",230,420
	C: "OO",510,500
	C: "OO",520,510
	C: "OP",520,230, "Lcl Rotation"
	C: "OP",530,520, "d|X"
	C: "OP",531,520, "d|Y"
	C: "OP",532,520, "d|Z"
}
"@

[System.IO.File]::WriteAllText($out, $fbx.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding($false)))
Write-Output "wrote $out ($($verts.Count) verts, $($polys.Count) polys, cluster1=$($idx1.Count) cluster2=$($idx2.Count))"
