# Setup.ps1 — MyEngine 配布物のセットアップ (Setup.bat から呼ばれる)。
# 足りない依存を winget で入れ、最後にエディタを起動する。入っている物は飛ばすので、何度実行してもよい。
# ★Windows 標準の PowerShell 5.1 で動くこと (配布先に pwsh があるとは限らない)。
#   三項演算子・??・ArgumentList などの 7 系の書き方を使わない。
#   tools\make_dist.ps1 が BOM 付き UTF-8 + CRLF に変換して同梱する (5.1 は BOM 無しを CP932 で読む)
param([switch]$NoLaunch)
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$editor = Join-Path $root 'bin\x64\Release\Editor.exe'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

function Write-Step([string]$text)
{
    Write-Host ''
    Write-Host "== $text" -ForegroundColor Cyan
}

function Test-VCRedist
{
    foreach ($key in @('HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
                       'HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64')) {
        $v = Get-ItemProperty -Path $key -Name Installed -ErrorAction SilentlyContinue
        if ($v -and $v.Installed -eq 1) { return $true }
    }
    return $false
}

# C# スクリプトのホスト (MyeScripting.runtimeconfig.json) は Microsoft.NETCore.App 8.x を要求する
# (rollForward = LatestMinor なので 9 以降では代わりにならない)
function Test-DotNet8
{
    $dirs = @(Join-Path $env:ProgramFiles 'dotnet')
    $reg = Get-ItemProperty -Path 'HKLM:\SOFTWARE\dotnet\Setup\InstalledVersions\x64' -Name InstallLocation -ErrorAction SilentlyContinue
    if ($reg -and $reg.InstallLocation) { $dirs += $reg.InstallLocation }
    foreach ($d in $dirs) {
        if (Test-Path (Join-Path $d 'shared\Microsoft.NETCore.App\8.*')) { return $true }
    }
    return $false
}

# Collab (MyeCollab.dll) は git.exe を起動して動く。Git Credential Manager も Git for Windows に同梱
function Test-Git
{
    if (Get-Command git.exe -ErrorAction SilentlyContinue) { return $true }
    return (Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'Git\cmd\git.exe'))
}

# Visual Studio の MSBuild があれば、そのインストール先を返す (無ければ空)。
# 選び方はエディタが生成する cache\build_scripts.bat と同じ (-latest = 最も新しい VS)
function Get-LatestMSBuildInstall
{
    if (-not (Test-Path -LiteralPath $vswhere)) { return '' }
    $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if ($path) { return [string]$path }
    return ''
}

# 生成される GameLogic.vcxproj は PlatformToolset v143 を指定する。
# 版の番号では判定しない: VS 2026 でも「MSVC v143」を入れていればビルドできる
function Test-CppBuildTools
{
    $install = Get-LatestMSBuildInstall
    if (-not $install) { return $false }
    return (Test-Path (Join-Path $install 'MSBuild\Microsoft\VC\*\Platforms\x64\PlatformToolsets\v143'))
}

function Install-WingetPackage([string]$id, [string[]]$extra)
{
    $wingetArgs = @('install', '--id', $id, '--exact', '--source', 'winget',
        '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity')
    if ($extra) { $wingetArgs += $extra }
    & winget.exe @wingetArgs
    Write-Host "  (winget の終了コード: $LASTEXITCODE)"
}

# zip を展開したファイルにはインターネット由来の印 (Zone.Identifier) が付き、exe ごとに
# SmartScreen の確認が出る。このフォルダの中身に限って印を外す
Write-Step 'ファイルの準備'
Get-ChildItem -LiteralPath $root -Recurse -File | Unblock-File

$deps = @(
    @{ Label = 'Visual C++ 再頒布可能パッケージ'; Id = 'Microsoft.VCRedist.2015+.x64'; Test = ${function:Test-VCRedist};
       Extra = @(); Lost = 'Collab や OpenSSL を使う機能が起動しない可能性があります' },
    @{ Label = '.NET 8 ランタイム'; Id = 'Microsoft.DotNet.Runtime.8'; Test = ${function:Test-DotNet8};
       Extra = @(); Lost = 'C# スクリプトが使えません' },
    @{ Label = 'Git for Windows'; Id = 'Git.Git'; Test = ${function:Test-Git};
       Extra = @(); Lost = 'Collab (共同編集) とソース管理が使えません' }
)

Write-Step 'C++ スクリプト'
$latestVs = Get-LatestMSBuildInstall
if (Test-CppBuildTools) {
    Write-Host "C++ スクリプトをビルドできます ($latestVs)。"
} elseif ($latestVs) {
    # 新しい VS が既にあると、Build Tools 2022 を足してもビルドには最新の VS が選ばれる (効かない)
    Write-Host "Visual Studio ($latestVs) に C++ のツールセット v143 がありません。" -ForegroundColor Yellow
    Write-Host 'C++ スクリプトを使う場合は Visual Studio Installer の「変更」→「個別のコンポーネント」で'
    Write-Host '「MSVC v143 - VS 2022 C++ x64/x86 ビルド ツール」を追加してください。'
} else {
    Write-Host 'C++ スクリプトを書くには Visual Studio 2022 Build Tools (C++、数 GB) が必要です。'
    Write-Host 'C# スクリプトだけ使う場合は不要です。'
    $answer = Read-Host 'C++ スクリプトも使いますか？ [y/N]'
    if ($answer -match '^[yY]') {
        $deps += @{ Label = 'Visual Studio 2022 Build Tools (C++)'; Id = 'Microsoft.VisualStudio.2022.BuildTools';
            Test = ${function:Test-CppBuildTools};
            # --override は winget 既定の引数を置き換えるので --wait を自分で付ける (無いと完了を待たずに戻る)
            Extra = @('--override', '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended');
            Lost = 'C++ スクリプトのビルドができません' }
    }
}

$hasWinget = [bool](Get-Command winget.exe -ErrorAction SilentlyContinue)
$notInstalled = @()
foreach ($dep in $deps) {
    Write-Step $dep.Label
    if (& $dep.Test) {
        Write-Host '入っています。'
        continue
    }
    if (-not $hasWinget) {
        Write-Host 'winget が無いため入れられません。' -ForegroundColor Yellow
        $notInstalled += $dep
        continue
    }
    Write-Host "winget で入れます ($($dep.Id))。管理者の確認が出たら「はい」を選んでください。"
    Install-WingetPackage $dep.Id $dep.Extra
    if (& $dep.Test) {
        Write-Host '入りました。' -ForegroundColor Green
    } else {
        Write-Host '入ったことを確認できませんでした。' -ForegroundColor Yellow
        $notInstalled += $dep
    }
}

Write-Step '結果'
if (-not $hasWinget) {
    Write-Host 'winget (アプリ インストーラー) が見つかりませんでした。' -ForegroundColor Yellow
    Write-Host 'Microsoft Store で「アプリ インストーラー」を入れてから、もう一度 Setup.bat を実行してください。'
}
if ($notInstalled.Count -eq 0) {
    Write-Host '必要な物はすべて揃っています。' -ForegroundColor Green
} else {
    foreach ($dep in $notInstalled) {
        Write-Host "未導入: $($dep.Label) — $($dep.Lost)" -ForegroundColor Yellow
    }
    Write-Host 'エディタはこのまま起動できます。入れ直すときは Setup.bat をもう一度実行してください。'
}

if (-not (Test-Path -LiteralPath $editor)) {
    Write-Host "Editor.exe が見つかりません: $editor" -ForegroundColor Red
    exit 1
}
if (-not $NoLaunch) {
    # winget で入れた物の PATH (git など) は、このプロセスの環境には反映されていない。
    # 読み直してからエディタへ引き継ぐ (しないと再ログインまで Collab が git を見つけられない)
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
        [Environment]::GetEnvironmentVariable('Path', 'User')
    Write-Step 'エディタを起動します'
    Start-Process -FilePath $editor -WorkingDirectory $root
}
exit 0
