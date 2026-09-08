# ビルド済みのDebug/Releaseを使う。既存のシーンやキャッシュを削除しない。
$ErrorActionPreference = 'Stop'
$sankoRoot = Split-Path -Parent $PSScriptRoot
$sankoLogs = Join-Path $sankoRoot 'cache\sanko_light_replay'
New-Item -ItemType Directory -Path $sankoLogs -Force | Out-Null

function Invoke-SankoEditor([string]$Configuration, [string]$Name, [string[]]$EditorArguments)
{
    $sankoExe = Join-Path $sankoRoot "bin\x64\$Configuration\Editor.exe"
    $sankoProcess = Start-Process -FilePath $sankoExe -ArgumentList $EditorArguments `
        -WorkingDirectory $sankoRoot -WindowStyle Hidden -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $sankoLogs "$Name.log") `
        -RedirectStandardError (Join-Path $sankoLogs "$Name.error.log")
    if ($sankoProcess.ExitCode -ne 0) {
        throw "$Name failed: exit $($sankoProcess.ExitCode). See $sankoLogs"
    }
    Write-Output "PASS: $Name"
}

$sankoScenes = @(
    @{Name='demo'; Flags=@(); RecordFlags=@()},
    @{Name='parts'; Flags=@('--parts-demo'); RecordFlags=@()},
    @{Name='flow'; Flags=@('--flow-demo'); RecordFlags=@('--synth-input')},
    @{Name='mp'; Flags=@('--local-demo'); RecordFlags=@('--local-players','2','--synth-input')},
    @{Name='physics'; Flags=@('--physics-demo'); RecordFlags=@()},
    @{Name='joints'; Flags=@('--joint-demo'); RecordFlags=@()},
    @{Name='acoustic'; Flags=@('--acoustic-demo'); RecordFlags=@('--synth-input')}
)
foreach ($sankoScene in $sankoScenes) {
    $sankoName = $sankoScene.Name
    $sankoRep = "cache\sanko_light_replay\$sankoName.rep"
    Invoke-SankoEditor 'Debug' "$sankoName-record" ($sankoScene.Flags + $sankoScene.RecordFlags + @(
        '--replay-record', $sankoRep, '--replay-ticks', '600', '--replay-fast', '--warp', '--no-audio'))
    Invoke-SankoEditor 'Debug' "$sankoName-snapshot" ($sankoScene.Flags + @(
        '--replay-verify', $sankoRep, '--snapshot-stress', '37', '--warp', '--no-audio'))
    Invoke-SankoEditor 'Release' "$sankoName-release" ($sankoScene.Flags + @(
        '--replay-verify', $sankoRep, '--warp', '--no-audio'))
}
foreach ($sankoConfig in @('Debug', 'Release')) {
    Invoke-SankoEditor $sankoConfig "timetravel-$sankoConfig" @(
        '--timetravel-selftest', '400', '--warp', '--no-audio')
}
