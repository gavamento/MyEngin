# make_dist.ps1
# 他の人へ渡すエンジン配布物 (ビルド済み exe 一式 + Setup.bat) を dist_engine\MyEngine\ に集め、zip にする。
#   pwsh -NoProfile -File tools\make_dist.ps1             # Release をビルドしてから詰める
#   pwsh -NoProfile -File tools\make_dist.ps1 -SkipBuild  # 今の bin\x64\Release をそのまま詰める
# 受け取った側は zip を展開して Setup.bat を実行する (足りない依存を winget で入れて Editor を起動)。
# ★出力先は dist\ ではなく dist_engine\。dist\ はエディタのゲーム書き出し (Build Settings) の既定先
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'bin\x64\Release'
$outRoot = Join-Path $repo 'dist_engine'
$stage = Join-Path $outRoot 'MyEngine'
$zipPath = Join-Path $outRoot ('MyEngine-{0}.zip' -f (Get-Date -Format 'yyyyMMdd-HHmm'))

# git の出力は UTF-8 で受ける。[Console]::OutputEncoding を書き換えるとコンソールのコードページが
# プロセス終了後も残る (chcp と同じ) ので、子プロセス単位で読む
function Get-TrackedFiles([string[]]$paths)
{
    $psi = [System.Diagnostics.ProcessStartInfo]::new('git')
    foreach ($a in @('-C', $repo, 'ls-files', '-z', '--') + $paths) {
        $psi.ArgumentList.Add($a)
    }
    $psi.RedirectStandardOutput = $true
    $psi.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    $text = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { throw "git ls-files failed ($($p.ExitCode))" }
    return $text.Split([char]0, [System.StringSplitOptions]::RemoveEmptyEntries)
}

function Copy-RepoFile([string]$rel)
{
    $dst = Join-Path $stage $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $rel) -Destination $dst
}

# 改行は CRLF に揃える (.bat は LF だと cmd.exe が行を途中で切る)。
# BOM は .ps1 / .txt だけ: 配布先の Windows PowerShell 5.1 は BOM の無い UTF-8 を CP932 として読む。
# .bat に BOM を付けると先頭行のコマンドが壊れる
function Copy-Text([string]$src, [string]$dst, [bool]$bom)
{
    $text = [System.IO.File]::ReadAllText($src) -replace "`r?`n", "`r`n"
    [System.IO.File]::WriteAllText($dst, $text, [System.Text.UTF8Encoding]::new($bom))
}

if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
        Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild not found' }
    Write-Host '=== MyEngine.sln (Release|x64) ==='
    & $msbuild (Join-Path $repo 'MyEngine.sln') /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "MyEngine.sln Release build failed ($LASTEXITCODE)" }
    # C# ホストと Collab は sln の外 (それぞれ dotnet / cargo でビルドする)
    foreach ($bat in 'build_managed.bat', 'build_collab.bat') {
        & (Join-Path $PSScriptRoot $bat) Release
        if ($LASTEXITCODE -ne 0) { throw "$bat failed ($LASTEXITCODE)" }
    }
}

# 欠けたまま配ると、受け取った側で「C# だけ動かない」のような原因の分かりにくい縮退になる
$required = 'Editor.exe', 'Runtime.exe', 'Server.exe', 'GameLogic.dll', 'nethost.dll', 'MyeScripting.dll',
    'MyeScripting.runtimeconfig.json', 'MyeCollab.dll', 'MyeCollabCli.exe', 'libcrypto-3-x64.dll', 'libssl-3-x64.dll'
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $bin $_)) })
if ($missing.Count -gt 0) { throw "missing in ${bin}: $($missing -join ', ')" }

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$binOut = Join-Path $stage 'bin\x64\Release'
New-Item -ItemType Directory -Force $binOut | Out-Null

# bin は実行時に save\ crash\ shots\ などが生まれる場所なので、拾う物を形で絞る。
# 直下は exe / dll / json だけ (pdb・lib・exp・ログは渡さない)、
# サブフォルダは Roslyn の翻訳リソース (*.resources.dll だけを含む cs\ ja\ など) だけ
Get-ChildItem -LiteralPath $bin -File |
    Where-Object { $_.Extension -in '.exe', '.dll', '.json' } |
    Copy-Item -Destination $binOut
Get-ChildItem -LiteralPath $bin -Directory | ForEach-Object {
    $files = @(Get-ChildItem -LiteralPath $_.FullName -File -Recurse)
    $isResources = $files.Count -gt 0 -and @($files | Where-Object { $_.Name -notlike '*.resources.dll' }).Count -eq 0
    if ($isResources) { Copy-Item -LiteralPath $_.FullName -Destination $binOut -Recurse }
}

# リポジトリ側は git 管理下の物だけ (作業中の未追跡ファイルや生成物を混ぜない)。
# src\Shared と build\Common.props は C++ スクリプトのビルド用:
# エディタは src\Shared\ScriptAPI.h を目印にエンジンのルートを探し (FindEngineRepoRoot)、
# 生成する GameLogic.vcxproj が Common.props を import する
$repoFiles = @(Get-TrackedFiles @('assets', 'src/Shared', 'build/Common.props'))
$repoFiles += @(Get-TrackedFiles @('external') |
    Where-Object { $_ -match '(^|/)(LICENSE|License|NOTICE)[^/]*$' -or $_ -eq 'external/VERSIONS.md' })
foreach ($rel in $repoFiles) {
    Copy-RepoFile $rel
}

$distSrc = Join-Path $PSScriptRoot 'engine_dist'
Copy-Text (Join-Path $distSrc 'Setup.bat') (Join-Path $stage 'Setup.bat') $false
Copy-Text (Join-Path $distSrc 'Editor.bat') (Join-Path $stage 'Editor.bat') $false
Copy-Text (Join-Path $distSrc 'Setup.ps1') (Join-Path $stage 'Setup.ps1') $true
Copy-Text (Join-Path $distSrc 'README.txt') (Join-Path $stage 'README.txt') $true

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath,
    [System.IO.Compression.CompressionLevel]::Optimal, $true)

$fileCount = @(Get-ChildItem -LiteralPath $stage -File -Recurse).Count
$zipMb = [math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 1)
Write-Host ''
Write-Host "=== $fileCount files -> $stage"
Write-Host "=== $zipPath ($zipMb MB)"
