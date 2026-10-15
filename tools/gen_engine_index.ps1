# gen_engine_index.ps1 — エンジン索引 (cache\engine_index.json) を作る (M87a)
# 実行: pwsh -NoProfile -File tools\gen_engine_index.ps1 [-CheckOnly] [-Root <repo>] [-EditorExe <path>] [-Out <path>]
#
# 実行時の表 (コンポーネント / フィールド / 共通 CLI / showcase) は Editor.exe --dump-index が出し、
# 静的に読むもの (ABI / Editor 専用 CLI / ADR / 仕様の見出し / golden / l10n) はここでパースして 1 本にする。
# 本文そのものは索引に入れない (どの見出し・ADR が何に言及するかの対応だけを生成時に計算する)。
#
# -CheckOnly: Editor.exe を起動せず、静的パースと下限チェックだけ行う (ファイルは書かない。check_rules 規則 15-f が呼ぶ)
# -Root:      リポジトリのルートを差し替える (下限が効くことを違反フィクスチャで確かめるため)
# 下限チェックに外れたら exit 1。静的パースが静かに 0 件を返し続ける事故を止めるためのもの
param(
    [switch]$CheckOnly,
    [string]$Root = '',
    [string]$EditorExe = '',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$toolRepo = Split-Path -Parent $PSScriptRoot
if ($Root -eq '') { $Root = $toolRepo }
if ($Out -eq '') { $Out = Join-Path $Root 'cache\engine_index.json' }
$utf8 = New-Object System.Text.UTF8Encoding $false

# ---- 下限 (件数) ----
# 静的パースの 90% (切り捨て)。根拠の実測は 2026-10-15: adr 31 / spec 見出し 105 / golden 30 /
# l10n 1639 / cliEditor 45。abi だけは固定値でなく check_rules の $apiVersionSlots と完全一致を要求する。
# component / field の下限はダンプを読む通常モードだけで見る (実測 81 / 932)
$floors = [ordered]@{
    adr       = 27
    spec      = 94
    golden    = 27
    l10n      = 1475
    cliEditor = 40
}
$dumpFloors = [ordered]@{
    components  = 72
    fields      = 838
    cliEngine   = 100
    cliShowcase = 19
}

$failures = New-Object System.Collections.Generic.List[string]
function Add-Failure([string]$msg) { $script:failures.Add($msg) | Out-Null }

function Read-RepoText([string]$rel) {
    $path = Join-Path $Root $rel
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Failure "missing file: $rel"
        return $null
    }
    return [System.IO.File]::ReadAllText($path)
}

# ---- abi: EngineAPI.h の MYE_API_VERSION とスロット列 ----
$abi = $null
$apiText = Read-RepoText 'src\Shared\EngineAPI.h'
if ($null -ne $apiText) {
    $versionMatch = [regex]::Match($apiText, '#define\s+MYE_API_VERSION\s+(\d+)u?')
    $structMatch = [regex]::Match($apiText, 'struct\s+MyeEngineApi\s*\{(.*?)\n\};', 'Singleline')
    if (-not $versionMatch.Success -or -not $structMatch.Success) {
        Add-Failure 'abi: MYE_API_VERSION or struct MyeEngineApi not found in EngineAPI.h'
    } else {
        $slots = New-Object System.Collections.Generic.List[object]
        foreach ($m in [regex]::Matches($structMatch.Groups[1].Value, '\(\s*\*\s*(\w+)\s*\)\s*\(([^;]*?)\)\s*;', 'Singleline')) {
            $params = $m.Groups[2].Value.Trim()
            $argc = if ($params -eq '' -or $params -eq 'void') { 0 } else { ($params -split ',').Count }
            $slots.Add([ordered]@{ index = $slots.Count; name = $m.Groups[1].Value; args = $argc }) | Out-Null
        }
        $abi = [ordered]@{ version = [int]$versionMatch.Groups[1].Value; slots = $slots.ToArray() }
        # 期待スロット数は check_rules.ps1 の $apiVersionSlots (スロットを足すときに必ず更新される表)
        $rulesText = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'check_rules.ps1'))
        $tableMatch = [regex]::Match($rulesText, '\$apiVersionSlots\s*=\s*@\{(.*?)\}', 'Singleline')
        $expected = $null
        if ($tableMatch.Success) {
            foreach ($e in [regex]::Matches($tableMatch.Groups[1].Value, '(\d+)\s*=\s*(\d+)')) {
                if ([int]$e.Groups[1].Value -eq $abi.version) { $expected = [int]$e.Groups[2].Value }
            }
        }
        if ($null -eq $expected) {
            Add-Failure "abi: version $($abi.version) is not in check_rules.ps1 `$apiVersionSlots"
        } elseif ($slots.Count -ne $expected) {
            Add-Failure "abi: $($slots.Count) slot(s) parsed, `$apiVersionSlots[$($abi.version)] = $expected"
        }
    }
}

# ---- cliEditor: EditorMain.cpp の arg == L"--..." (Editor 専用フラグの静的拾い) ----
$cliEditor = @()
$editorText = Read-RepoText 'src\Editor\App\EditorMain.cpp'
if ($null -ne $editorText) {
    $seen = New-Object System.Collections.Generic.HashSet[string]
    $list = New-Object System.Collections.Generic.List[string]
    foreach ($m in [regex]::Matches($editorText, 'arg\s*==\s*L"(--[A-Za-z0-9][A-Za-z0-9-]*)"')) {
        if ($seen.Add($m.Groups[1].Value)) { $list.Add($m.Groups[1].Value) | Out-Null }
    }
    $cliEditor = $list.ToArray()
}

# ---- adr: docs\adr\*.md ----
$adr = New-Object System.Collections.Generic.List[object]
$adrTokens = @{}
$adrDir = Join-Path $Root 'docs\adr'
if (Test-Path -LiteralPath $adrDir) {
    foreach ($f in (Get-ChildItem -LiteralPath $adrDir -Filter 'ADR-*.md' | Sort-Object Name)) {
        $text = [System.IO.File]::ReadAllText($f.FullName)
        $id = ([regex]::Match($f.BaseName, '^ADR-\d+')).Value
        $title = ''
        $headings = New-Object System.Collections.Generic.List[string]
        $inFence = $false
        foreach ($line in ($text -split "`r?`n")) {
            if ($line -match '^\s*```') { $inFence = -not $inFence; continue }
            if ($inFence) { continue }
            if ($line -match '^(#{1,6})\s+(.*\S)\s*$') {
                if ($title -eq '' -and $Matches[1].Length -eq 1) { $title = $Matches[2] } else { $headings.Add($Matches[2]) | Out-Null }
            }
        }
        $adr.Add([ordered]@{ id = $id; file = "docs/adr/$($f.Name)"; title = $title; headings = $headings.ToArray() }) | Out-Null
        $adrTokens[$id] = $text
    }
}

# ---- spec: engine_spec.md の見出し (コードフェンス内は除く) と、節ごとの識別子 / フラグ ----
$spec = New-Object System.Collections.Generic.List[object]
$specSections = New-Object System.Collections.Generic.List[object]
$specPath = Join-Path $Root 'engine_spec.md'
if (Test-Path -LiteralPath $specPath) {
    $inFence = $false
    $lineNo = 0
    $current = $null
    $body = New-Object System.Text.StringBuilder
    $flush = {
        if ($null -ne $script:current) {
            $script:specSections.Add([pscustomobject]@{ line = $script:current.line; text = $script:body.ToString() }) | Out-Null
        }
        [void]$script:body.Clear()
    }
    foreach ($line in [System.IO.File]::ReadLines($specPath)) {
        $lineNo++
        if ($line -match '^\s*```') { $inFence = -not $inFence }
        elseif (-not $inFence -and $line -match '^(#{1,6})\s+(.*\S)\s*$') {
            & $flush
            $current = [ordered]@{ level = $Matches[1].Length; title = $Matches[2]; line = $lineNo }
            $spec.Add($current) | Out-Null
            continue
        }
        [void]$body.AppendLine($line)
    }
    & $flush
} else {
    Add-Failure 'missing file: engine_spec.md'
}

# ---- golden: shot_verify.bat の call :shot <name> <args...> ----
$golden = New-Object System.Collections.Generic.List[object]
$shotText = Read-RepoText 'tools\shot_verify.bat'
if ($null -ne $shotText) {
    foreach ($line in ($shotText -split "`r?`n")) {
        if ($line -match '^\s*call\s+:shot\s+(\S+)(.*)$') {
            $name = $Matches[1]
            $argList = @($Matches[2].Trim() -split '\s+' | Where-Object { $_ -ne '' })
            $png = Test-Path -LiteralPath (Join-Path $Root "tests\golden\$name.png")
            $golden.Add([ordered]@{ n = $golden.Count + 1; name = $name; args = $argList; png = $png }) | Out-Null
        }
    }
}

# ---- l10n: LocalizationTable.inl の MYE_STR(id, en, ja) ----
$l10n = New-Object System.Collections.Generic.List[object]
$tableText = Read-RepoText 'src\Engine\Core\Localization\LocalizationTable.inl'
if ($null -ne $tableText) {
    $strSeq = '"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'
    $entryRe = 'MYE_STR\(\s*(\w+)\s*,\s*(' + $strSeq + ')\s*,\s*(' + $strSeq + ')\s*\)'
    $unescape = {
        param([string]$seq)
        $joined = ([regex]::Matches($seq, '"((?:[^"\\]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value }) -join ''
        [regex]::Replace($joined, '\\(.)', {
            param($m)
            switch ($m.Groups[1].Value) { 'n' { "`n" } 't' { "`t" } default { $m.Groups[1].Value } }
        })
    }
    foreach ($m in [regex]::Matches($tableText, $entryRe)) {
        $l10n.Add([ordered]@{ id = $m.Groups[1].Value; en = (& $unescape $m.Groups[2].Value); ja = (& $unescape $m.Groups[3].Value) }) | Out-Null
    }
}

# ---- 下限チェック (静的パース分) ----
$counts = [ordered]@{ adr = $adr.Count; spec = $spec.Count; golden = $golden.Count; l10n = $l10n.Count; cliEditor = $cliEditor.Count }
foreach ($k in $floors.Keys) {
    if ($counts[$k] -lt $floors[$k]) { Add-Failure "$k : $($counts[$k]) item(s), floor is $($floors[$k])" }
}

if ($CheckOnly) {
    $abiCount = if ($null -ne $abi) { $abi.slots.Count } else { 0 }
    Write-Host ("[gen_engine_index] -CheckOnly: abi={0} adr={1} spec={2} golden={3} l10n={4} cliEditor={5}" -f $abiCount, $adr.Count, $spec.Count, $golden.Count, $l10n.Count, $cliEditor.Count)
    if ($failures.Count -gt 0) {
        foreach ($f in $failures) { Write-Host "ERROR [engine_index] $f" }
        exit 1
    }
    exit 0
}

# ---- Editor.exe --dump-index ----
if ($EditorExe -eq '') {
    foreach ($cfg in 'Release', 'Debug') {
        $candidate = Join-Path $Root "bin\x64\$cfg\Editor.exe"
        if (Test-Path -LiteralPath $candidate) { $EditorExe = $candidate; break }
    }
}
if ($EditorExe -eq '' -or -not (Test-Path -LiteralPath $EditorExe)) {
    Write-Host 'ERROR [engine_index] Editor.exe not found (build Release|x64 or Debug|x64 first)'
    exit 1
}
$dumpPath = Join-Path ([System.IO.Path]::GetTempPath()) ("mye_dump_index_{0}.json" -f $PID)
try {
    # GUI サブシステムの exe は直接呼んでも待たないので Start-Process -Wait で終了を待つ
    $proc = Start-Process -FilePath $EditorExe -ArgumentList @('--dump-index', ('"{0}"' -f $dumpPath)) -Wait -PassThru
    if ($proc.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $dumpPath)) {
        Write-Host "ERROR [engine_index] Editor.exe --dump-index failed (exit $($proc.ExitCode))"
        exit 1
    }
    $dump = [System.IO.File]::ReadAllText($dumpPath) | ConvertFrom-Json
} finally {
    if (Test-Path -LiteralPath $dumpPath) { Remove-Item -LiteralPath $dumpPath -Force }
}

$fieldCount = 0
foreach ($c in $dump.components) { $fieldCount += @($c.fields).Count }
$dumpCounts = [ordered]@{ components = @($dump.components).Count; fields = $fieldCount; cliEngine = @($dump.cliEngine).Count; cliShowcase = @($dump.cliShowcase).Count }
foreach ($k in $dumpFloors.Keys) {
    if ($dumpCounts[$k] -lt $dumpFloors[$k]) { Add-Failure "$k : $($dumpCounts[$k]) item(s), floor is $($dumpFloors[$k]) (--dump-index output)" }
}
if ($failures.Count -gt 0) {
    foreach ($f in $failures) { Write-Host "ERROR [engine_index] $f" }
    exit 1
}

# Editor 専用 = 共通フラグ表に無いもの (EditorMain の共通フラグ分岐は ParseEngineCliFlag の後ろにも書いてある)
$engineFlagSet = New-Object System.Collections.Generic.HashSet[string]
foreach ($f in $dump.cliEngine) { [void]$engineFlagSet.Add($f.name) }
foreach ($f in $dump.cliShowcase) { [void]$engineFlagSet.Add($f.flag) }
$cliEditorOnly = @($cliEditor | Where-Object { -not $engineFlagSet.Contains($_) })

# ---- mentions: component 名 / CLI フラグごとの、言及している spec 見出し (行番号) と ADR ----
$identRe = [regex]'[A-Za-z_][A-Za-z0-9_]*'
$flagRe = [regex]'--[A-Za-z0-9][A-Za-z0-9-]*'
$specIdents = @{}
$specFlags = @{}
foreach ($sec in $specSections) {
    $idents = New-Object System.Collections.Generic.HashSet[string]
    foreach ($m in $identRe.Matches($sec.text)) { [void]$idents.Add($m.Value) }
    $flags = New-Object System.Collections.Generic.HashSet[string]
    foreach ($m in $flagRe.Matches($sec.text)) { [void]$flags.Add($m.Value.TrimEnd('-')) }
    $specIdents[$sec.line] = $idents
    $specFlags[$sec.line] = $flags
}
$adrIdents = @{}
$adrFlags = @{}
foreach ($id in $adrTokens.Keys) {
    $idents = New-Object System.Collections.Generic.HashSet[string]
    foreach ($m in $identRe.Matches($adrTokens[$id])) { [void]$idents.Add($m.Value) }
    $flags = New-Object System.Collections.Generic.HashSet[string]
    foreach ($m in $flagRe.Matches($adrTokens[$id])) { [void]$flags.Add($m.Value.TrimEnd('-')) }
    $adrIdents[$id] = $idents
    $adrFlags[$id] = $flags
}
$sortedSecLines = @($specSections | ForEach-Object { $_.line } | Sort-Object)
$sortedAdrIds = @($adrTokens.Keys | Sort-Object)
function Get-Mentions([string]$token, [bool]$isFlag) {
    $specSet = if ($isFlag) { $specFlags } else { $specIdents }
    $adrSet = if ($isFlag) { $adrFlags } else { $adrIdents }
    [ordered]@{
        spec = @($sortedSecLines | Where-Object { $specSet[$_].Contains($token) })
        adr  = @($sortedAdrIds | Where-Object { $adrSet[$_].Contains($token) })
    }
}
$mentionComponents = [ordered]@{}
foreach ($c in $dump.components) {
    $m = Get-Mentions $c.name $false
    if ($m.spec.Count -gt 0 -or $m.adr.Count -gt 0) { $mentionComponents[$c.name] = $m }
}
$mentionCli = [ordered]@{}
$allFlags = New-Object System.Collections.Generic.HashSet[string]
foreach ($f in $dump.cliEngine) { [void]$allFlags.Add($f.name) }
foreach ($f in $dump.cliShowcase) { [void]$allFlags.Add($f.flag) }
foreach ($f in $cliEditor) { [void]$allFlags.Add($f) }
foreach ($flag in ($allFlags | Sort-Object)) {
    $m = Get-Mentions $flag $true
    if ($m.spec.Count -gt 0 -or $m.adr.Count -gt 0) { $mentionCli[$flag] = $m }
}

# ---- sources: 索引の源と最終更新時刻 (MCP が stale 判定に使う) ----
$sourceRels = @('src\Shared\EngineAPI.h', 'src\Editor\App\EditorMain.cpp', 'tools\shot_verify.bat', 'engine_spec.md',
                'src\Engine\Core\Localization\LocalizationTable.inl')
$sourceRels += @($adr | ForEach-Object { $_.file.Replace('/', '\') })
$sources = New-Object System.Collections.Generic.List[object]
foreach ($rel in $sourceRels) {
    $path = Join-Path $Root $rel
    if (Test-Path -LiteralPath $path) {
        $sources.Add([ordered]@{ path = $rel.Replace('\', '/'); mtime = (Get-Item -LiteralPath $path).LastWriteTimeUtc.ToString('yyyy-MM-ddTHH:mm:ss.fffffffZ') }) | Out-Null
    }
}
$sources.Add([ordered]@{ path = ([System.IO.Path]::GetRelativePath($Root, $EditorExe)).Replace('\', '/'); mtime = (Get-Item -LiteralPath $EditorExe).LastWriteTimeUtc.ToString('yyyy-MM-ddTHH:mm:ss.fffffffZ') }) | Out-Null

$index = [ordered]@{
    schema       = 1
    generatedAt  = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    sources      = $sources.ToArray()
    abi          = $abi
    components   = @($dump.components)
    cliEngine    = @($dump.cliEngine)
    cliShowcase  = @($dump.cliShowcase)
    cliEditor    = @($cliEditorOnly | ForEach-Object { [ordered]@{ flag = $_ } })
    adr          = $adr.ToArray()
    spec         = $spec.ToArray()
    golden       = $golden.ToArray()
    l10n         = $l10n.ToArray()
    mentions     = [ordered]@{ components = $mentionComponents; cli = $mentionCli }
}

$outDir = Split-Path -Parent $Out
if (-not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$json = $index | ConvertTo-Json -Depth 12 -Compress
[System.IO.File]::WriteAllText($Out, $json, $utf8)

# ---- 結果の表示 ----
function Get-JsonBytes($value) { $utf8.GetByteCount(($value | ConvertTo-Json -Depth 12 -Compress)) }
Write-Host "[gen_engine_index] wrote $Out"
foreach ($name in 'sources', 'abi', 'components', 'cliEngine', 'cliShowcase', 'cliEditor', 'adr', 'spec', 'golden', 'l10n', 'mentions') {
    $n = switch ($name) {
        'abi' { $abi.slots.Count }
        'mentions' { $mentionComponents.Count + $mentionCli.Count }
        default { @($index[$name]).Count }
    }
    Write-Host ("  {0,-12} {1,7} item(s) {2,9} bytes" -f $name, $n, (Get-JsonBytes $index[$name]))
}
Write-Host ("  fields       {0,7} item(s) (inside components)" -f $fieldCount)
Write-Host ("  total        {0,9} bytes" -f $utf8.GetByteCount($json))

# 文書にあるが、どのパーサにも無いフラグ (情報のみ。失敗にしない)
$unknownFlags = New-Object System.Collections.Generic.SortedSet[string]
foreach ($sec in $specSections) {
    foreach ($flag in $specFlags[$sec.line]) { if (-not $allFlags.Contains($flag)) { [void]$unknownFlags.Add($flag) } }
}
Write-Host ("[gen_engine_index] info: {0} flag(s) in engine_spec.md are in no parser: {1}" -f $unknownFlags.Count, ((@($unknownFlags) | Select-Object -First 15) -join ' '))
exit 0
