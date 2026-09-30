param(
    [string]$TargetSha = '',
    [string]$BaseSha = '',
    [string]$OutputDir = ''
)

$ErrorActionPreference = 'Stop'
Set-Location (Resolve-Path (Join-Path $PSScriptRoot '..'))

function Resolve-Commit([string]$ref) {
    $sha = (& git rev-parse --verify "$ref^{commit}" 2>$null)
    if ($LASTEXITCODE -ne 0 -or $sha -notmatch '^[0-9a-fA-F]{40}$') {
        throw "Commit is unavailable: $ref"
    }
    return $sha.Trim().ToLowerInvariant()
}

if (-not $TargetSha) { $TargetSha = 'HEAD' }
$TargetSha = Resolve-Commit $TargetSha
if (-not $BaseSha) { $BaseSha = (& git rev-parse "$TargetSha^1" 2>$null) }
$BaseSha = Resolve-Commit $BaseSha

if (-not $OutputDir) {
    $root = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { Join-Path (Get-Location) 'cache' }
    $OutputDir = Join-Path $root ("perf_" + [guid]::NewGuid().ToString('N'))
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild was not found' }

function Measure-Commit([string]$sha, [string]$label) {
    $source = Join-Path $OutputDir $label
    $archive = Join-Path $OutputDir "$label.zip"
    & git archive --format=zip "--output=$archive" $sha
    if ($LASTEXITCODE -ne 0) { throw "git archive failed: $sha" }
    Expand-Archive -LiteralPath $archive -DestinationPath $source
    if (-not (Test-Path (Join-Path $source 'src\Engine\Engine\PerfBenchmark.cpp'))) {
        return $false
    }
    & $msbuild (Join-Path $source 'build\Editor.vcxproj') /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Release build failed: $sha" }
    $result = Join-Path $OutputDir "$label.json"
    $editor = Join-Path $source 'bin\x64\Release\Editor.exe'
    $process = Start-Process -FilePath $editor -ArgumentList @('--perf-bench', ('"' + $result + '"'), '--perf-commit', $sha) -WorkingDirectory $source -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0 -or -not (Test-Path $result)) { throw "Benchmark failed: $sha" }
    return $true
}

if (-not (Measure-Commit $TargetSha 'target')) { throw "Target has no benchmark: $TargetSha" }
$hasBase = Measure-Commit $BaseSha 'base'
$target = Get-Content -LiteralPath (Join-Path $OutputDir 'target.json') -Raw | ConvertFrom-Json
if ($target.commit -ne $TargetSha -or $target.schema -ne 1) { throw 'Invalid target result' }
$base = $null
if ($hasBase) {
    $base = Get-Content -LiteralPath (Join-Path $OutputDir 'base.json') -Raw | ConvertFrom-Json
    if ($base.commit -ne $BaseSha -or $base.schema -ne 1) { throw 'Invalid baseline result' }
    if ($base.configuration -ne $target.configuration -or
        $base.warmups -ne $target.warmups -or
        $base.samples -ne $target.samples -or
        ($base.fixture | ConvertTo-Json -Compress) -ne ($target.fixture | ConvertTo-Json -Compress)) {
        throw 'Benchmark conditions differ between commits'
    }
}

$expected = @('ecs_update_10k', 'transform_10k', 'broadphase_10k', 'xpbd_particles_10k',
              'draw_submission_1000', 'rollback_snapshot', 'replay_hash', 'asset_database_10k')
$lines = @('# Performance regression', '', "Target: ``$TargetSha``", "Baseline: ``$BaseSha``", '',
           '| Metric | Target ms | Baseline ms | Change | Target ratio |', '|---|---:|---:|---:|---:|')
foreach ($name in $expected) {
    $t = $target.metrics.$name
    if ($null -eq $t -or $t.median_ms -lt 0 -or $t.target_ms -le 0) { throw "Missing or invalid metric: $name" }
    $baselineText = 'unavailable'
    $changeText = 'unavailable'
    if ($hasBase) {
        $b = $base.metrics.$name
        if ($null -eq $b -or $b.median_ms -le 0) { throw "Missing or invalid baseline metric: $name" }
        $baselineText = '{0:N4}' -f $b.median_ms
        $changeText = '{0:+0.0;-0.0;0.0}%' -f (100.0 * ($t.median_ms / $b.median_ms - 1.0))
    }
    $lines += "| $name | $('{0:N4}' -f $t.median_ms) | $baselineText | $changeText | $('{0:N2}' -f ($t.median_ms / $t.target_ms))x |"
}
if (-not $hasBase) {
    $lines += ''
    $lines += 'Baseline benchmark is unavailable in the selected commit (bootstrap run).'
}
$report = Join-Path $OutputDir 'summary.md'
$lines | Set-Content -LiteralPath $report -Encoding utf8
if ($env:GITHUB_STEP_SUMMARY) { Get-Content -LiteralPath $report -Raw | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding utf8 }
Write-Output "PERF_OUTPUT_DIR=$OutputDir"
Write-Output ($lines -join "`n")
