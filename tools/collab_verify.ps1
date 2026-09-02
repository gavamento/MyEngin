# collab_verify.ps1 — MyeCollab (Rust サービス) の回帰検証。**エディタを起動しない**。
#
#   tools\collab_verify.bat            全シナリオ
#   tools\collab_verify.bat --update   期待 NDJSON を撮り直す (差分を読んでからコミットすること)
#
# やること:
#   1. cache\collab_verify\repo に collab_fixture.ps1 で一時プロジェクト + git リポジトリを作る
#   2. tests\collab\*.ndjson のシナリオを MyeCollabCli.exe の stdin へ流す
#      (ディレクティブ '# write' / '# delete' / '# git' の位置で区切り、fixture を変えてから再開)
#   3. 出力を正規化 (<sha> / <time> / <author> / <gitversion> / <root>) して
#      *.expected.ndjson と比較する
#
# 正規化が要る理由: SHA も author も日時も走るたびに変わる。素で比較すると
# 「毎回赤い」= 誰も見なくなるテストになる。逆に**正規化しすぎる**と検査が空になるので、
# 環境依存の 5 種だけを置換する。
[CmdletBinding()]
param(
    [switch]$Update,
    [string]$Scenario = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$work = Join-Path $repo 'cache\collab_verify'
$root = Join-Path $work 'repo'
$scenarioDir = Join-Path $repo 'tests\collab'

# CLI は Debug / Release どちらでも中身が同じ (build_collab.bat が同じ物を両方へ置く)
$cli = $null
foreach ($cfg in @('Debug', 'Release')) {
    $p = Join-Path $repo "bin\x64\$cfg\MyeCollabCli.exe"
    if (Test-Path $p) { $cli = $p; break }
}
if (-not $cli) {
    Write-Host "[collab_verify] MyeCollabCli.exe not found - run tools\build_collab.bat first"
    exit 1
}

$scenarios = if ($Scenario) {
    @(Join-Path $scenarioDir $Scenario)
} else {
    Get-ChildItem -File (Join-Path $scenarioDir '*.ndjson') |
        Where-Object { $_.Name -notlike '*.expected.ndjson' } |
        Sort-Object Name | ForEach-Object { $_.FullName }
}
if (-not $scenarios) { Write-Host "[collab_verify] no scenarios in $scenarioDir"; exit 1 }

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# fixture は**シナリオ 1 本ごとに作り直す** (M66b で shared → per-scenario に変更)。
# ★共用にすると N 本目の期待ファイルが 1..N-1 本目の実行結果に依存する。
#   「新しいシナリオを足しただけで既存の期待が動く」「単体で --job 的に 1 本流すと
#   落ちる」という、原因が読めない壊れ方をする。git init + commit 1 回は
#   1 秒程度なので、独立性の方を買う
function New-Fixture {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work }
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'collab_fixture.ps1') $root
    if ($LASTEXITCODE -ne 0) { Write-Host "[collab_verify] fixture creation failed"; exit 1 }
    # サービスが起動する git にも開発者設定を見せない (fixture と同じ隔離)
    $env:GIT_CONFIG_GLOBAL = "$root.gitconfig"
    $env:GIT_CONFIG_NOSYSTEM = '1'
}

# 環境依存の値を伏せる。$root は '\' と '/' の両方の綴りで出うる
$rootFull = [System.IO.Path]::GetFullPath($root)
$rootSlash = $rootFull -replace '\\', '/'
function Normalize-Line([string]$line) {
    $s = $line
    $s = $s -replace [regex]::Escape($rootSlash), '<root>'
    $s = $s -replace [regex]::Escape($rootFull.Replace('\', '\\')), '<root>'
    $s = $s -replace [regex]::Escape($rootFull), '<root>'
    # ★40 桁ちょうどだけを伏せる。短縮 SHA (7-39 桁) まで拾うと "deadbeef" のような
    #   **普通の単語**まで <sha> に化けて検査が空洞化する。短縮 SHA を返す op を
    #   足すときは、その op のフィールド名を指定して置換すること
    $s = $s -replace '\b[0-9a-f]{40}\b', '<sha>'
    # M66c: diff の "index 1a2b3c4..5d6e7f8 100644" 行。**短縮長は git がリポジトリの
    # オブジェクト数から決める**ので、fixture が育つと桁が変わる = 期待が理由もなく赤くなる。
    # 差分の +/- 行はそのまま残るので、ここを伏せても検査は空にならない
    $s = $s -replace 'index [0-9a-f]{4,40}\.\.[0-9a-f]{4,40}', 'index <blob>..<blob>'
    $s = $s -replace '"gitVersion":"[^"]*"', '"gitVersion":"<gitversion>"'
    $s = $s -replace '"(date|time|when)":"[^"]*"', '"$1":"<time>"'
    $s = $s -replace '"author":"[^"]*"', '"author":"<author>"'
    return $s
}

# 1 セグメント = ディレクティブに挟まれた要求の並び。CLI を 1 回起動して流す。
# ★fixture のパスと中身は ASCII に限る — cmd 経由の stdout はコンソール CP で復号される
#   ので、非 ASCII を混ぜると CP932 の機体で化ける。ここで chcp / [Console]::OutputEncoding を
#   触ってはいけない (呼び出し元のコンソールに残留して端末ごと化ける)
function Invoke-Segment([string[]]$lines, [int]$index) {
    if ($lines.Count -eq 0) { return @() }
    $inFile = Join-Path $work "seg$index.in"
    [System.IO.File]::WriteAllText($inFile, (($lines -join "`n") + "`n"), $utf8NoBom)
    $out = & cmd /c "`"$cli`" --root `"$root`" < `"$inFile`""
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[collab_verify] MyeCollabCli.exe exited with $LASTEXITCODE"
        exit 1
    }
    return @($out)
}

$failed = 0
foreach ($path in $scenarios) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($path)
    Write-Host "=== collab scenario: $name ==="
    New-Fixture
    $actual = New-Object System.Collections.Generic.List[string]
    $pending = New-Object System.Collections.Generic.List[string]
    $seg = 0
    foreach ($raw in [System.IO.File]::ReadAllLines($path)) {
        $line = $raw.Trim()
        if ($line -eq '') { continue }
        if ($line.StartsWith('#')) {
            # ★ディレクティブは「'#' + 半角空白 1 個 + 動詞」に限る。Trim してから
            #   先頭語を見る書き方だと、**動詞で始まる地の文**がディレクティブに化ける
            #   (M66c で実際に踏んだ: '#   git は OS アカウント名から…' という説明行が
            #    git 実行に化けて落ちた)。字下げされたコメントはこれで確実に外れる
            if ($line -notmatch '^#\s(write|delete|git)\s') { continue } # ただのコメント
            $body = $line.Substring(1).Trim()
            $verb = ($body -split '\s+', 2)[0]
            # ディレクティブの前に溜まった要求を流し切ってからファイルを触る
            foreach ($o in (Invoke-Segment $pending.ToArray() $seg)) { $actual.Add($o) }
            $pending.Clear()
            $seg++
            $rest = ($body -split '\s+', 2)[1]
            if ($verb -eq 'write') {
                $parts = $rest -split '\s+', 2
                $target = Join-Path $root ($parts[0] -replace '/', '\')
                $text = if ($parts.Count -gt 1) { $parts[1] } else { '' }
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
                [System.IO.File]::WriteAllText($target, "$text`n", $utf8NoBom)
            } elseif ($verb -eq 'git') {
                # M66c: fixture の**前提条件**を作る口 (identity 未設定など)。
                # ★op を git で代行するためではない — サービスが叩く git と同じ
                #   隔離設定 (GIT_CONFIG_GLOBAL / NOSYSTEM は New-Fixture が設定済み) で
                #   走るので、「開発者のマシンにはこう設定されている」を再現できる。
                #   出力は捨てる: 期待 NDJSON に git の生の文言を混ぜると版で割れる
                $gitArgs = [System.Text.RegularExpressions.Regex]::Split($rest, '\s+')
                $null = & git -C $root @gitArgs 2>$null
                if ($LASTEXITCODE -ne 0) {
                    # ★黙って続けない。前提が作れていないシナリオは「通ったが何も
                    #   検査していない」= 一番たちの悪い緑になる
                    Write-Host "[collab_verify] directive failed: git $rest (exit $LASTEXITCODE)"
                    exit 1
                }
            } else {
                Remove-Item -Force (Join-Path $root ($rest -replace '/', '\'))
            }
            continue
        }
        $pending.Add($line)
    }
    foreach ($o in (Invoke-Segment $pending.ToArray() $seg)) { $actual.Add($o) }

    $normalized = @($actual | ForEach-Object { Normalize-Line $_ })
    $expectedPath = Join-Path $scenarioDir "$name.expected.ndjson"
    if ($Update) {
        [System.IO.File]::WriteAllText($expectedPath, (($normalized -join "`n") + "`n"), $utf8NoBom)
        Write-Host "[collab_verify] updated $expectedPath ($($normalized.Count) lines)"
        continue
    }
    if (-not (Test-Path $expectedPath)) {
        Write-Host "[collab_verify] FAIL $name : missing $expectedPath (run with --update)"
        $failed++
        continue
    }
    $expected = @([System.IO.File]::ReadAllLines($expectedPath) | Where-Object { $_ -ne '' })
    $same = $expected.Count -eq $normalized.Count
    if ($same) {
        for ($i = 0; $i -lt $expected.Count; $i++) {
            if ($expected[$i] -ne $normalized[$i]) { $same = $false; break }
        }
    }
    if ($same) {
        Write-Host "[collab_verify] PASS $name ($($normalized.Count) lines)"
    } else {
        # 差分は**全文**出す。1 行の JSON なので、どのフィールドが動いたかは
        # 目で追える (行数が食い違うときに片側だけ出すと原因が読めない)
        Write-Host "[collab_verify] FAIL $name"
        Write-Host "--- expected ($($expected.Count) lines) ---"
        $expected | ForEach-Object { Write-Host "  $_" }
        Write-Host "--- actual ($($normalized.Count) lines) ---"
        $normalized | ForEach-Object { Write-Host "  $_" }
        $failed++
    }
}

if ($failed -gt 0) {
    Write-Host "=== collab_verify: $failed scenario(s) FAILED ==="
    exit 1
}
Write-Host '=== collab_verify: all scenarios passed ==='
exit 0
