# mcp_verify.ps1 — mye_mcp (エンジン MCP サーバ) の回帰検証。**エディタを起動しない**。
#
#   tools\mcp_verify.bat            cargo test → cargo build --release → 全シナリオ
#   tools\mcp_verify.bat --update   期待 NDJSON を撮り直す (差分を読んでからコミットすること)
#   pwsh -NoProfile -File tools\mcp_verify.ps1 -RealIndex
#                                   実索引に対する固定 10 問だけを回す (下記)
#
# やること:
#   1. cargo test / cargo build --release (tools\mcp)
#   2. tests\mcp\*.ndjson を mye_mcp.exe の stdin へ流し、stdout を *.expected.ndjson と比較する
#      (索引は tests\mcp\fixture\engine_index.json の固定フィクスチャ)
#
# シナリオ先頭の '# args ...' 行は、既定の引数 (--index tests\mcp\fixture\engine_index.json
# --project {PROJ}) を置き換える (索引なし / schema 違い / stale でない索引を試すため)。
# {PROJ} は cache\mcp_verify\proj_<シナリオ名> (毎回空で作る。notes の書き先)。1 シナリオ =
# 1 回の起動で、互いに状態を共有しない。
# 他のディレクティブ ('# ' + 名前 + 引数):
#   junction <相対パス>   起動前に {PROJ}\<相対パス> を、プロジェクトの外の空ディレクトリへのジャンクションにする
#   expect-untouched      起動後、プロジェクト (junction があればその行き先) にファイルが増えていないこと
# 要求行の @@REP:<1 文字>:<回数>@@ は、その文字の繰り返しに展開する (64 KB 打ち切りの試験用)。
#
# 出力は比較の前に正規化する: UTC 時刻 -> <TS>、同じ文字の 100 回以上の連なり -> <文字>*<回数>
# (notes の時刻と長い本文)。他に環境依存の値 (絶対パス) は載せない作り。
# stdout は cmd のリダイレクトでファイルへ落として UTF-8 のまま読む (コンソールの
# コードページ経由で復号すると日本語が化ける。chcp / OutputEncoding は触らない)。
#
# -RealIndex: 実索引 (cache\engine_index.json) に対する固定 10 問 (spec 条件 12) だけを回す。
# cargo は呼ばず、ビルド済みの mye_mcp.exe に直接問う。索引が無ければ SKIP (exit 0)。
# 実索引は gitignore 下の生成物なので CI には入れない。
[CmdletBinding()]
param(
    [switch]$Update,
    [string]$Scenario = '',
    [switch]$RealIndex
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$manifest = Join-Path $repo 'tools\mcp\Cargo.toml'
$exe = Join-Path $repo 'tools\mcp\target\release\mye_mcp.exe'
$scenarioDir = Join-Path $repo 'tests\mcp'
$work = Join-Path $repo 'cache\mcp_verify'
$defaultArgs = '--index tests\mcp\fixture\engine_index.json --project {PROJ}'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

Set-Location $repo

# ジャンクションは行き先を消さずにリンクだけを外す (Remove-Item -Recurse に任せない)
function Remove-Junctions([string]$dir) {
    if (-not (Test-Path -LiteralPath $dir)) { return }
    try {
        Get-ChildItem -LiteralPath $dir -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue |
            ForEach-Object { [System.IO.Directory]::Delete($_.FullName) }
    } catch { Write-Host "[mcp_verify] could not remove a junction under ${dir}: $_" }
}

if ($RealIndex) {
    $indexPath = Join-Path $repo 'cache\engine_index.json'
    if (-not (Test-Path $indexPath)) {
        Write-Host '[mcp_verify] SKIP -RealIndex: cache\engine_index.json not found (run tools\gen_engine_index.ps1)'
        exit 0
    }
    if (-not (Test-Path $exe)) { Write-Host "[mcp_verify] $exe not found (run tools\mcp_verify.bat once to build)"; exit 1 }
    # 固定 10 問: 問い -> top-5 (component / cli) に入るべき id の述語
    $questions = @(
        @{ q = '音の遮蔽'; want = { param($id) $id -like 'Acoustic*' } },
        @{ q = '剛体'; want = { param($id) $id -eq 'Rigidbody' } },
        @{ q = '経路探索'; want = { param($id) $id -like 'NavMesh*' } },
        @{ q = '霧'; want = { param($id) $id -eq 'Fog' } },
        @{ q = '破壊'; want = { param($id) $id -eq 'Destructible' -or $id -eq 'FracturePiece' } },
        @{ q = 'ラグドール'; want = { param($id) $id -eq 'Ragdoll' } },
        @{ q = '地形'; want = { param($id) $id -eq 'Terrain' } },
        @{ q = 'ビヘイビアツリー'; want = { param($id) $id -eq 'BehaviorTree' } },
        @{ q = 'スクロール'; want = { param($id) $id -eq 'UIScrollRect' -or $id -eq 'UIScrollbar' } },
        @{ q = 'タイムトラベル'; want = { param($id) $id -eq '--timetravel-selftest' } }
    )
    $lines = New-Object System.Collections.Generic.List[string]
    $n = 0
    foreach ($item in $questions) {
        $n++
        $lines.Add((@{ jsonrpc = '2.0'; id = $n; method = 'tools/call'; params = @{ name = 'search'; arguments = @{ q = $item.q; limit = 5; kind = @('component', 'cli') } } } | ConvertTo-Json -Compress -Depth 6))
    }
    $workReal = Join-Path $repo 'cache\mcp_verify_real'
    New-Item -ItemType Directory -Force -Path $workReal | Out-Null
    $inReal = Join-Path $workReal 'in.ndjson'
    $outReal = Join-Path $workReal 'out.ndjson'
    [System.IO.File]::WriteAllText($inReal, (($lines -join "`n") + "`n"), $utf8NoBom)
    # --project は存在するディレクトリが要る。notes の無い空のものを渡す (実 notes を結果に混ぜない)
    $projReal = Join-Path $workReal 'proj'
    New-Item -ItemType Directory -Force -Path $projReal | Out-Null
    & cmd /c "`"$exe`" --root `"$repo`" --project `"$projReal`" < `"$inReal`" > `"$outReal`" 2> nul"
    $responses = @([System.IO.File]::ReadAllLines($outReal, $utf8NoBom) | Where-Object { $_ -ne '' } | ForEach-Object { $_ | ConvertFrom-Json })
    $hit = 0
    for ($i = 0; $i -lt $questions.Count; $i++) {
        $r = $responses | Where-Object { $_.id -eq ($i + 1) } | Select-Object -First 1
        $ids = @()
        if ($r -and $r.result.structuredContent.hits) { $ids = @($r.result.structuredContent.hits | ForEach-Object { $_.id }) }
        $ok = $false
        foreach ($id in $ids) { if (& $questions[$i].want $id) { $ok = $true; break } }
        if ($ok) { $hit++ }
        Write-Host ("[mcp_verify] {0} {1} -> {2}" -f $(if ($ok) { 'HIT ' } else { 'MISS' }), $questions[$i].q, ($ids -join ', '))
    }
    Write-Host "=== mcp_verify -RealIndex: $hit/$($questions.Count) ==="
    if ($hit -ne $questions.Count) { exit 1 }
    exit 0
}

# notes.rs の unit test が作った一時プロジェクト (削除 API を持てないので掃除はここでやる)
$notesTestDir = Join-Path $repo 'tools\mcp\target\notes_test'
if (Test-Path $notesTestDir) { Remove-Item -Recurse -Force $notesTestDir }

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

Remove-Junctions $work
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$stampPattern = '\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ'
$failed = 0
foreach ($path in $scenarios) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($path)
    Write-Host "=== mcp scenario: $name ==="
    $extraArgs = $defaultArgs
    $junctionRel = $null
    $expectUntouched = $false
    $requests = New-Object System.Collections.Generic.List[string]
    foreach ($raw in [System.IO.File]::ReadAllLines($path)) {
        $line = $raw.Trim()
        if ($line -eq '') { continue }
        if ($line.StartsWith('#')) {
            # ディレクティブ ('# args' 'junction' 'expect-untouched') 以外の '#' 行は説明
            if ($line -match '^#\sargs\s+(.+)$') { $extraArgs = $Matches[1] }
            if ($line -match '^#\sjunction\s+(.+)$') { $junctionRel = $Matches[1] }
            if ($line -match '^#\sexpect-untouched\s*$') { $expectUntouched = $true }
            continue
        }
        $requests.Add([regex]::Replace($line, '@@REP:(.):(\d+)@@', { param($m) [string]::new($m.Groups[1].Value[0], [int]$m.Groups[2].Value) }))
    }

    # 書き先の一時プロジェクト (毎回空)。junction 指定があれば行き先 (プロジェクトの外) も用意する
    $projRel = "cache\mcp_verify\proj_$name"
    $projDir = Join-Path $repo $projRel
    New-Item -ItemType Directory -Force -Path $projDir | Out-Null
    $junctionTarget = Join-Path $work "jt_$name"
    $junctionPath = $null
    if ($junctionRel) {
        New-Item -ItemType Directory -Force -Path $junctionTarget | Out-Null
        $junctionPath = Join-Path $projDir $junctionRel
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $junctionPath) | Out-Null
        New-Item -ItemType Junction -Path $junctionPath -Target $junctionTarget | Out-Null
    }
    $extraArgs = $extraArgs.Replace('{PROJ}', $projRel)
    $before = @(Get-ChildItem -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -notin '.in', '.out', '.err' } | ForEach-Object { $_.FullName } | Sort-Object)

    $inFile = Join-Path $work "$name.in"
    $outFile = Join-Path $work "$name.out"
    $errFile = Join-Path $work "$name.err"
    [System.IO.File]::WriteAllText($inFile, (($requests -join "`n") + "`n"), $utf8NoBom)
    & cmd /c "`"$exe`" --root `"$repo`" $extraArgs < `"$inFile`" > `"$outFile`" 2> `"$errFile`""
    $exitCode = $LASTEXITCODE

    # 起動後の確認: ファイルが増えていないこと (ジャンクションがあればその行き先、無ければプロジェクト)
    $untouched = $true
    if ($expectUntouched) {
        $after = @(Get-ChildItem -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -notin '.in', '.out', '.err' } | ForEach-Object { $_.FullName } | Sort-Object)
        $watched = if ($junctionRel) { $junctionTarget } else { $projDir }
        $untouched = (($before -join "`n") -eq ($after -join "`n")) -and -not (Get-ChildItem -LiteralPath $watched -Force)
    }
    # ジャンクションは次の掃除に残さない (リンクだけを外す)
    if ($junctionPath -and (Test-Path -LiteralPath $junctionPath)) { [System.IO.Directory]::Delete($junctionPath) }

    if ($exitCode -ne 0) {
        Write-Host "[mcp_verify] FAIL $name : mye_mcp.exe exited with $exitCode"
        Get-Content $errFile | ForEach-Object { Write-Host "  $_" }
        $failed++
        continue
    }
    if (-not $untouched) {
        Write-Host "[mcp_verify] FAIL $name : files were created where nothing may be written"
        $failed++
        continue
    }
    $actual = @([System.IO.File]::ReadAllLines($outFile, $utf8NoBom) | Where-Object { $_ -ne '' } | ForEach-Object {
        $t = [regex]::Replace($_, $stampPattern, '<TS>')
        [regex]::Replace($t, '(.)\1{99,}', { param($m) $m.Groups[1].Value + '*' + $m.Length })
    })

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
