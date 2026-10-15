# mcp_verify.ps1 — mye_mcp (エンジン MCP サーバ) の回帰検証。**エディタを起動しない**。
#
#   tools\mcp_verify.bat            cargo test → cargo build --release → 全シナリオ
#   tools\mcp_verify.bat --update   期待 NDJSON を撮り直す (差分を読んでからコミットすること)
#
# やること:
#   1. cargo test / cargo build --release (tools\mcp)
#   2. tests\mcp\*.ndjson を mye_mcp.exe の stdin へ流し、stdout を *.expected.ndjson と比較する
#      (索引は tests\mcp\fixture\engine_index.json の固定フィクスチャ)
#
# シナリオ先頭の '# args ...' 行は、既定の引数 (--index tests\mcp\fixture\engine_index.json) を
# 置き換える (索引なし / schema 違い / stale でない索引を試すため)。1 シナリオ = 1 回の起動で、
# 互いに状態を共有しない。
#
# 出力に環境依存の値 (絶対パス・時刻) を載せない作りなので、collab_verify のような正規化は無い。
# stdout は cmd のリダイレクトでファイルへ落として UTF-8 のまま読む (コンソールの
# コードページ経由で復号すると日本語が化ける。chcp / OutputEncoding は触らない)。
[CmdletBinding()]
param(
    [switch]$Update,
    [string]$Scenario = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$manifest = Join-Path $repo 'tools\mcp\Cargo.toml'
$exe = Join-Path $repo 'tools\mcp\target\release\mye_mcp.exe'
$scenarioDir = Join-Path $repo 'tests\mcp'
$work = Join-Path $repo 'cache\mcp_verify'
$defaultArgs = '--index tests\mcp\fixture\engine_index.json'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

Set-Location $repo

Write-Host '=== mcp_verify: cargo test ==='
& cargo test --manifest-path $manifest
if ($LASTEXITCODE -ne 0) { Write-Host '[mcp_verify] cargo test failed'; exit 1 }
Write-Host '=== mcp_verify: cargo build --release ==='
& cargo build --release --manifest-path $manifest
if ($LASTEXITCODE -ne 0) { Write-Host '[mcp_verify] cargo build failed'; exit 1 }
if (-not (Test-Path $exe)) { Write-Host "[mcp_verify] $exe not found"; exit 1 }

$scenarios = if ($Scenario) {
    @(Join-Path $scenarioDir $Scenario)
} else {
    Get-ChildItem -File (Join-Path $scenarioDir '*.ndjson') |
        Where-Object { $_.Name -notlike '*.expected.ndjson' } |
        Sort-Object Name | ForEach-Object { $_.FullName }
}
if (-not $scenarios) { Write-Host "[mcp_verify] no scenarios in $scenarioDir"; exit 1 }

if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$failed = 0
foreach ($path in $scenarios) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($path)
    Write-Host "=== mcp scenario: $name ==="
    $extraArgs = $defaultArgs
    $requests = New-Object System.Collections.Generic.List[string]
    foreach ($raw in [System.IO.File]::ReadAllLines($path)) {
        $line = $raw.Trim()
        if ($line -eq '') { continue }
        if ($line.StartsWith('#')) {
            # '# args <引数>' だけがディレクティブ。それ以外の '#' 行は説明
            if ($line -match '^#\sargs\s+(.+)$') { $extraArgs = $Matches[1] }
            continue
        }
        $requests.Add($line)
    }

    $inFile = Join-Path $work "$name.in"
    $outFile = Join-Path $work "$name.out"
    $errFile = Join-Path $work "$name.err"
    [System.IO.File]::WriteAllText($inFile, (($requests -join "`n") + "`n"), $utf8NoBom)
    & cmd /c "`"$exe`" --root `"$repo`" $extraArgs < `"$inFile`" > `"$outFile`" 2> `"$errFile`""
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[mcp_verify] FAIL $name : mye_mcp.exe exited with $LASTEXITCODE"
        Get-Content $errFile | ForEach-Object { Write-Host "  $_" }
        $failed++
        continue
    }
    $actual = @([System.IO.File]::ReadAllLines($outFile, $utf8NoBom) | Where-Object { $_ -ne '' })

    $expectedPath = Join-Path $scenarioDir "$name.expected.ndjson"
    if ($Update) {
        [System.IO.File]::WriteAllText($expectedPath, (($actual -join "`n") + "`n"), $utf8NoBom)
        Write-Host "[mcp_verify] updated $expectedPath ($($actual.Count) lines)"
        continue
    }
    if (-not (Test-Path $expectedPath)) {
        Write-Host "[mcp_verify] FAIL $name : missing $expectedPath (run with --update)"
        $failed++
        continue
    }
    $expected = @([System.IO.File]::ReadAllLines($expectedPath, $utf8NoBom) | Where-Object { $_ -ne '' })
    $same = $expected.Count -eq $actual.Count
    if ($same) {
        for ($i = 0; $i -lt $expected.Count; $i++) {
            if ($expected[$i] -ne $actual[$i]) { $same = $false; break }
        }
    }
    if ($same) {
        Write-Host "[mcp_verify] PASS $name ($($actual.Count) lines)"
    } else {
        # 食い違った最初の行だけを並べる (1 行が長いので全文を出すと読めない)。全文は cache\mcp_verify\
        Write-Host "[mcp_verify] FAIL $name (expected $($expected.Count) lines, actual $($actual.Count) lines; full output: $outFile)"
        $n = [Math]::Max($expected.Count, $actual.Count)
        for ($i = 0; $i -lt $n; $i++) {
            $e = if ($i -lt $expected.Count) { $expected[$i] } else { '(none)' }
            $a = if ($i -lt $actual.Count) { $actual[$i] } else { '(none)' }
            if ($e -ne $a) {
                Write-Host "  line $($i + 1)"
                Write-Host "  - expected: $e"
                Write-Host "  - actual  : $a"
                break
            }
        }
        $failed++
    }
}

if ($failed -gt 0) {
    Write-Host "=== mcp_verify: $failed scenario(s) FAILED ==="
    exit 1
}
Write-Host '=== mcp_verify: all scenarios passed ==='
exit 0
