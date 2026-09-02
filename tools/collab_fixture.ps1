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
$srcScene = Join-Path $engineAssets 'scenes\scene_b.scene.json'
$srcTex = Join-Path $engineAssets 'textures\test.png'
foreach ($p in @($srcScene, $srcTex)) {
    if (-not (Test-Path $p)) { Write-Error "collab_fixture: engine asset not found: $p"; exit 1 }
}

New-Item -ItemType Directory -Force -Path $root | Out-Null
foreach ($sub in @('assets\scenes', 'assets\textures', '.mye')) {
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

Copy-Item $srcScene (Join-Path $root 'assets\scenes\main.scene.json')
Copy-Item $srcTex (Join-Path $root 'assets\textures\test.png')

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
