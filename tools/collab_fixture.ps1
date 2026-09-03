# collab_fixture.ps1 <dir> — Source Control の検証用に「git 管理下の最小プロジェクト」を作る。
#
# なぜ要るか (spec §2 S9): replay / shot / CI は全部**裸起動** (--project が 1 件も無い) =
# Collab は常に OFF。エンジンリポ自身は project.mye.json を持たないので --project で開けない。
# つまり **Source Control を動かせる場所がリポジトリ内に存在しない**。ここで作る。
#
#   pwsh -File tools\collab_fixture.ps1 cache\collab_fixture
#   bin\x64\Debug\Editor.exe --project cache\collab_fixture     (実機目視)
#   tools\collab_verify.bat                                     (自動検証。自前で作る)
#
# 決定論のため (spec §4.4)、git は開発者の設定から隔離して呼ぶ:
#   GIT_CONFIG_GLOBAL=<空ファイル> / GIT_CONFIG_NOSYSTEM=1 … global/system の設定・hook・
#     commit.gpgsign・core.autocrlf を遮断する (署名を要求されると commit が固まる)
#   core.autocrlf=false … 改行変換で blob が環境依存になるのを止める
#   user.name / user.email はリポジトリ設定に書く (サービスが叩く git にも効かせるため)
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Dir
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# 既存は拒否する。上書きすると「前回の残骸込みの状態」を検証してしまい、
# 落ちたときに fixture のせいか実装のせいか切り分けられなくなる
$root = if ([System.IO.Path]::IsPathRooted($Dir)) {
    [System.IO.Path]::GetFullPath($Dir)
} else {
    # 相対パスは**呼び出し側の cwd** 基準 (.NET の GetFullPath は PowerShell の
    # カレントではなくプロセスのカレントを見るので、明示的に組み立てる)
    [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Dir))
}
if (Test-Path $root) {
    Write-Error "collab_fixture: directory already exists: $root (delete it first)"
    exit 1
}

$engineAssets = Join-Path $repo 'assets'
$srcTex = Join-Path $engineAssets 'textures\test.png'
foreach ($p in @($srcTex)) {
    if (-not (Test-Path $p)) { Write-Error "collab_fixture: engine asset not found: $p"; exit 1 }
}

New-Item -ItemType Directory -Force -Path $root | Out-Null
foreach ($sub in @('assets\scenes', 'assets\textures', 'assets\materials', '.mye')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $root $sub) | Out-Null
}

# project.mye.json — SaveProjectManifest と同じキー (formatVersion / name / engineVersion / bootScene)
$manifest = @'
{
  "formatVersion": 1,
  "name": "collab_fixture",
  "engineVersion": "0.66",
  "bootScene": "scenes/main.scene.json"
}
'@
# UTF-8 (BOM 無し) + LF。BOM を付けると nlohmann のパースが落ちる
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $root 'project.mye.json'), ($manifest -replace "`r`n", "`n"), $utf8NoBom)

Copy-Item $srcTex (Join-Path $root 'assets\textures\test.png')

# ---- テクスチャを参照するエンティティ (M66e) ----
# ★空のシーンだと ReloadHub の HandleChange が no-op になり、段階 A
#   (「その場で差し替え → 絵が変わる」) の**画素証拠が 1 枚も撮れない**。
#   床 + 立方体の 2 個に test.png を貼ったマテリアルを割り当てておく。
#   guid は .meta に固定値で書く — EnsureMeta の既定は「正規化した絶対パスのハッシュ」で、
#   チェックアウト先によって変わる = シーンの参照が解けなくなる
$materialGuidHex = '0a11ce5000000117'  # 16 桁の hex (固定値)
$materialGuidDec = [System.UInt64]::Parse($materialGuidHex, 'AllowHexSpecifier')

$material = @'
{
  "engine": "MyEngine",
  "material": 1,
  "name": "fixture_checker",
  "shader": "forward_lit",
  "baseColor": [1.0, 1.0, 1.0, 1.0],
  "metallic": 0.0,
  "roughness": 0.6,
  "texture": "textures/test.png",
  "normalMap": "",
  "transparent": false
}
'@
$materialMeta = @"
{
  "guid": "$materialGuidHex",
  "type": "material",
  "version": 1
}
"@
$scene = @"
{
  "engine": "MyEngine",
  "version": 2,
  "sceneName": "main",
  "nextFileId": 5,
  "entities": [
    {
      "fileId": 1,
      "childIndex": 0,
      "name": "Main Camera",
      "components": {
        "Camera": { "farZ": 1000.0, "fovYDeg": 60.0, "isPrimary": 1, "nearZ": 0.1 },
        "LocalTransform": {
          "position": [0.0, 3.0, -9.0],
          "rotation": [0.1305261999368668, 0.0, 0.0, 0.9914448857307434],
          "scale": [1.0, 1.0, 1.0]
        }
      }
    },
    {
      "fileId": 2,
      "childIndex": 1,
      "name": "Sun",
      "components": {
        "Light": {
          "ambient": [0.25, 0.26, 0.28],
          "castShadow": 0,
          "color": [1.0, 1.0, 1.0],
          "intensity": 1.0,
          "type": 0
        },
        "LocalTransform": {
          "position": [0.0, 5.0, 0.0],
          "rotation": [0.4082179069519043, -0.23456971347332, 0.10938165336847305, 0.8754260540008545],
          "scale": [1.0, 1.0, 1.0]
        }
      }
    },
    {
      "fileId": 3,
      "childIndex": 2,
      "name": "Textured Floor",
      "components": {
        "LocalTransform": {
          "position": [0.0, -0.25, 0.0],
          "rotation": [0.0, 0.0, 0.0, 1.0],
          "scale": [18.0, 0.5, 18.0]
        },
        "MeshRenderer": { "material": $materialGuidDec, "mesh": 1506918697593860217 }
      }
    },
    {
      "fileId": 4,
      "childIndex": 3,
      "name": "Textured Cube",
      "components": {
        "LocalTransform": {
          "position": [0.0, 2.0, 0.0],
          "rotation": [0.0, 0.0, 0.0, 1.0],
          "scale": [4.0, 4.0, 4.0]
        },
        "MeshRenderer": { "material": $materialGuidDec, "mesh": 1506918697593860217 }
      }
    }
  ]
}
"@
# ★.meta は**こちらで置いておく** (エディタに作らせない)。
#   AssetDatabase::EnsureMeta の既定 guid は「正規化した絶対パスのハッシュ」なので、
#   エディタ任せにすると (1) チェックアウト先ごとに guid が変わり (2) 起動しただけで
#   未追跡ファイルが 2 つ増える。後者は checkout を
#   `local_changes_overwritten` で弾く原因になり、実機検証の邪魔になる (M66e で踏んだ)
$sceneMeta = @"
{
  "guid": "0a11ce5000000217",
  "type": "scene",
  "version": 1
}
"@
$texMeta = @"
{
  "guid": "0a11ce5000000317",
  "type": "texture",
  "version": 1
}
"@
foreach ($pair in @(
        @{ p = 'assets\materials\fixture_checker.mat.json'; t = $material },
        @{ p = 'assets\materials\fixture_checker.mat.json.meta'; t = $materialMeta },
        @{ p = 'assets\scenes\main.scene.json.meta'; t = $sceneMeta },
        @{ p = 'assets\textures\test.png.meta'; t = $texMeta },
        @{ p = 'assets\scenes\main.scene.json'; t = $scene })) {
    [System.IO.File]::WriteAllText((Join-Path $root $pair.p), (($pair.t -replace "`r`n", "`n") + "`n"),
                                   $utf8NoBom)
}

# ProjectTemplates.cpp の CreateProject が書くのと同じ 3 行 (M66h でここへ 4 行足す)
[System.IO.File]::WriteAllText((Join-Path $root '.gitignore'), "/.mye/`n/cache/`n/dist/`n", $utf8NoBom)

# ---- git ----
# 空の global config はリポジトリの**外**に置く (中に置くと自分でコミットしてしまう)
$emptyConfig = "$root.gitconfig"
[System.IO.File]::WriteAllText($emptyConfig, '', $utf8NoBom)
$env:GIT_CONFIG_GLOBAL = $emptyConfig
$env:GIT_CONFIG_NOSYSTEM = '1'

function Invoke-Git {
    param([string[]]$GitArgs)
    $out = & git -C $root @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "collab_fixture: git $($GitArgs -join ' ') failed:`n$out"
        exit 1
    }
    return $out
}

# -b main は git 2.28+。**サービス本体の下限は 2.11** だが、fixture を作るのは
# 開発者/CI の手元なので新しい git を前提にしてよい (既定ブランチ名を環境依存にしない方が大事)
Invoke-Git @('init', '-b', 'main') | Out-Null
Invoke-Git @('config', 'core.autocrlf', 'false') | Out-Null
Invoke-Git @('config', 'user.name', 'mye') | Out-Null
Invoke-Git @('config', 'user.email', 'mye@example.com') | Out-Null
Invoke-Git @('add', '-A') | Out-Null
Invoke-Git @('commit', '-m', 'fixture: initial') | Out-Null

Write-Host "[collab_fixture] created $root"
exit 0
