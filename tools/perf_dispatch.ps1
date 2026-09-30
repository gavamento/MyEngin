param(
    [ValidatePattern('^$|^[0-9a-fA-F]{40}$')][string]$BaseSha = '',
    [ValidatePattern('^$|^[0-9a-fA-F]{40}$')][string]$TargetSha = '',
    [string]$ResultDir = ''
)

$ErrorActionPreference = 'Stop'
trap {
    Write-Output "PERF_ERROR=$($_.Exception.Message)"
    exit 1
}
function Fail([string]$message) {
    Write-Output "PERF_ERROR=$message"
    exit 1
}

if (-not (Test-Path '.github/workflows/ci.yml')) { Fail 'This repository has no performance CI workflow.' }
& gh auth status *> $null
if ($LASTEXITCODE -ne 0) { Fail 'GitHub CLI is not authenticated. Run gh auth login.' }

$branch = (& git symbolic-ref --quiet --short HEAD 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $branch) { Fail 'HEAD is detached. Check out a pushed branch.' }
$target = (& git rev-parse --verify HEAD).Trim()
if ($TargetSha -and $target -ne $TargetSha) { Fail 'HEAD changed after the comparison was selected. Refresh Gitline.' }
& git cat-file -e 'HEAD:src/Engine/Engine/PerfBenchmark.cpp' 2>$null
if ($LASTEXITCODE -ne 0) { Fail 'Commit and push the performance benchmark before starting CI.' }
if (-not $BaseSha) { $BaseSha = (& git rev-parse --verify 'HEAD^1').Trim() }
if ($BaseSha -notmatch '^[0-9a-fA-F]{40}$') { Fail 'No first-parent commit is available.' }
& git fetch origin --quiet
if ($LASTEXITCODE -ne 0) { Fail 'Could not fetch origin.' }
foreach ($sha in @($target, $BaseSha)) {
    $contains = (& git branch -r --contains $sha 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $contains) { Fail "Commit is not pushed to origin: $sha" }
}
$remoteBranch = "refs/remotes/origin/$branch"
& git show-ref --verify --quiet $remoteBranch
if ($LASTEXITCODE -ne 0) { Fail "Current branch is not pushed: $branch" }
$repo = (& gh repo view --json nameWithOwner --jq .nameWithOwner).Trim()
if ($LASTEXITCODE -ne 0 -or $repo -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    Fail 'Could not resolve the GitHub repository.'
}
$response = & gh api -X POST "repos/$repo/actions/workflows/ci.yml/dispatches" `
    -f "ref=$branch" -f "inputs[target_sha]=$target" -f "inputs[base_sha]=$BaseSha" 2>&1
if ($LASTEXITCODE -ne 0) { Fail (($response | Out-String).Trim()) }
$json = ($response | Out-String).Trim()
$data = if ($json) { $json | ConvertFrom-Json } else { $null }
if ($data -and $data.html_url) {
    Write-Output "PERF_URL=$($data.html_url)"
} else {
    Write-Output "PERF_URL=https://github.com/$repo/actions/workflows/ci.yml"
}
if (-not $ResultDir) { exit 0 }
if (-not $data -or -not $data.workflow_run_id) {
    Fail 'GitHub did not return a run ID; open the CI page to inspect the run.'
}
New-Item -ItemType Directory -Path $ResultDir -Force | Out-Null
$runId = [long]$data.workflow_run_id
$deadline = (Get-Date).AddMinutes(95)
Write-Output 'PERF_STATUS=waiting'
while ((Get-Date) -lt $deadline) {
    $artifacts = & gh api "repos/$repo/actions/runs/$runId/artifacts" --jq '.artifacts[] | select(.name == "mye-performance") | .id' 2>&1
    if ($LASTEXITCODE -ne 0) { Fail (($artifacts | Out-String).Trim()) }
    if ($artifacts) {
        & gh run download $runId -R $repo -n mye-performance -D $ResultDir 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $ResultDir 'target.json'))) {
            Write-Output "PERF_RESULT_DIR=$ResultDir"
            exit 0
        }
        Write-Output 'PERF_STATUS=downloading'
    }
    $run = & gh api "repos/$repo/actions/runs/$runId" --jq '{status: .status, conclusion: .conclusion}' | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { Fail 'Could not read the CI run status.' }
    if ($run.status -eq 'completed') {
        Fail "CI completed without a usable performance artifact ($($run.conclusion))."
    }
    Start-Sleep -Seconds 15
}
Fail 'Timed out waiting for the performance result artifact.'
