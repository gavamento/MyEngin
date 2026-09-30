param([ValidateRange(20, 500)][int]$Count = 100)

$ErrorActionPreference = 'Stop'
$utf8 = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8
try {
    $head = (& git rev-parse --verify HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not read HEAD.' }
    Write-Output "HEAD=$head"
    $parent = & git rev-parse --verify 'HEAD^1' 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Output "PARENT=$($parent.Trim())" }
    $commits = & git -c i18n.logOutputEncoding=UTF-8 --no-pager log HEAD --all --topo-order -n $Count --format='%H%x1f%P%x1f%aI%x1f%s'
    if ($LASTEXITCODE -ne 0) { throw 'Could not read Git history.' }
    foreach ($commit in $commits) { Write-Output "COMMIT=$commit" }
} catch {
    Write-Output "ERROR=$($_.Exception.Message)"
    exit 1
}
