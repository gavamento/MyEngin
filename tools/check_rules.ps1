# check_rules.ps1 — コーディング規則の静的検査 (engine_spec.md 11.2)
# 実行: pwsh -File tools\check_rules.ps1  (違反があれば exit 1)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$srcDirs = @("$repo\src")
$errors = 0
$warnings = 0

function Get-Sources {
    Get-ChildItem -Recurse -File $srcDirs | Where-Object { $_.Extension -in '.cpp', '.h', '.hpp' }
}

Write-Host "=== MyEngine coding rule check (spec 11.2) ==="

# C++ ソースの行から // コメント部を除去して検査する (コメント内の言及は許容)
function Test-CodeLines([System.IO.FileInfo]$file, [string]$pattern, [string]$rule, [string]$msg) {
    $lineNo = 0
    foreach ($line in [System.IO.File]::ReadLines($file.FullName)) {
        $lineNo++
        $code = ($line -split '//', 2)[0]
        if ($code -match $pattern) {
            Write-Host "ERROR [$rule] $($file.FullName):${lineNo}: $msg"
            $script:errors++
        }
    }
}

# 規則 8: エンジン RNG 以外の乱数禁止
foreach ($f in Get-Sources) {
    Test-CodeLines $f '\brand\s*\(\s*\)|\bsrand\s*\(|std::random_device|std::mt19937' 'rule 8' 'use engine Pcg32 instead'
}

# 規則 1: ロジックの #ifdef _DEBUG 分岐禁止 (許可リスト: 診断出力のみのファイル)
$debugWhitelist = @('GraphicsDevice.cpp')
foreach ($f in Get-Sources) {
    if ($debugWhitelist -contains $f.Name) { continue }
    $hits = Select-String -Path $f.FullName -Pattern '#\s*if(def)?\s+.*\b(_DEBUG|NDEBUG)\b'
    foreach ($h in $hits) {
        Write-Host "ERROR [rule 1] $($h.Path):$($h.LineNumber): $($h.Line.Trim())"
        $script:errors++
    }
}

# 規則 2: assert 内の副作用 (CRT assert 自体を禁止 — MYE_CHECK を使う)
foreach ($f in Get-Sources) {
    $hits = Select-String -Path $f.FullName -Pattern '^\s*assert\s*\('
    foreach ($h in $hits) {
        Write-Host "ERROR [rule 2] $($h.Path):$($h.LineNumber): use MYE_CHECK instead of assert"
        $script:errors++
    }
}

# 規則 4: /fp:fast 禁止 (ビルド設定 — 実際の設定値のみ検査、XML コメントは無視)
$buildFiles = Get-ChildItem -Recurse -File "$repo\build" | Where-Object { $_.Extension -in '.vcxproj', '.props' }
foreach ($f in $buildFiles) {
    $hits = Select-String -Path $f.FullName -Pattern '<FloatingPointModel>Fast|<AdditionalOptions>[^<]*fp:fast'
    foreach ($h in $hits) {
        Write-Host "ERROR [rule 4] $($h.Path):$($h.LineNumber): /fp:fast is forbidden"
        $script:errors++
    }
}

# 規則 7 (参考警告): unordered コンテナの range-for (順序がロジックに影響しないか要確認)
foreach ($f in Get-Sources) {
    $hits = Select-String -Path $f.FullName -Pattern 'for\s*\(.*:\s*\w*unordered_(map|set)'
    foreach ($h in $hits) {
        Write-Host "WARN  [rule 7] $($h.Path):$($h.LineNumber): unordered iteration (verify order-independence)"
        $script:warnings++
    }
}

# 規則 9: C++ と HLSL で共有する定数の一致
# (食い違うと定数バッファ不一致やトラバーサル破綻として静かに壊れる)
$constGroups = @(
    @{
        label = 'kMaxBones / MYE_MAX_BONES'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'            = 'constexpr\s+int\s+kMaxBones\s*=\s*(\d+)'
            'assets\shaders\forward_skinned.hlsl'          = '#\s*define\s+MYE_MAX_BONES\s+(\d+)'
            'assets\shaders\deferred_gbuffer_skinned.hlsl' = '#\s*define\s+MYE_MAX_BONES\s+(\d+)'
        }
    },
    @{
        label = 'kEmissiveMaxIntensity / MYE_EMISSIVE_MAX'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h' = 'constexpr\s+int\s+kEmissiveMaxIntensity\s*=\s*(\d+)'
            'assets\shaders\common.hlsli'       = '#\s*define\s+MYE_EMISSIVE_MAX\s+(\d+)'
        }
    },
    @{
        label = 'kRtStackDepth / MYE_RT_STACK_DEPTH'
        sites = @{
            'src\Engine\Renderer\RayTracing\RtTypes.h' = 'constexpr\s+int\s+kRtStackDepth\s*=\s*(\d+)'
            'assets\shaders\rt_common.hlsli'           = '#\s*define\s+MYE_RT_STACK_DEPTH\s+(\d+)'
        }
    },
    @{
        label = 'kRtMaxVisit / MYE_RT_MAX_VISIT'
        sites = @{
            'src\Engine\Renderer\RayTracing\RtTypes.h' = 'constexpr\s+int\s+kRtMaxVisit\s*=\s*(\d+)'
            'assets\shaders\rt_common.hlsli'           = '#\s*define\s+MYE_RT_MAX_VISIT\s+(\d+)'
        }
    },
    @{
        label = 'kRtTemporalMaxHistory / MYE_RT_TEMPORAL_MAX_HISTORY'
        sites = @{
            'src\Engine\Renderer\RayTracing\RtTypes.h' = 'constexpr\s+int\s+kRtTemporalMaxHistory\s*=\s*(\d+)'
            'assets\shaders\rt_temporal.cs.hlsl'       = '#\s*define\s+MYE_RT_TEMPORAL_MAX_HISTORY\s+(\d+)'
        }
    },
    @{
        label = 'kRtAtrousRadius / MYE_RT_ATROUS_RADIUS'
        sites = @{
            'src\Engine\Renderer\RayTracing\RtTypes.h' = 'constexpr\s+int\s+kRtAtrousRadius\s*=\s*(\d+)'
            'assets\shaders\rt_atrous.cs.hlsl'         = '#\s*define\s+MYE_RT_ATROUS_RADIUS\s+(\d+)'
            'assets\shaders\rt_variance.cs.hlsl'       = '#\s*define\s+MYE_RT_ATROUS_RADIUS\s+(\d+)'
            'assets\shaders\rt_shadow_filter.cs.hlsl'  = '#\s*define\s+MYE_RT_ATROUS_RADIUS\s+(\d+)'
        }
    }
)
foreach ($g in $constGroups) {
    $values = @{}
    foreach ($rel in $g.sites.Keys) {
        $path = Join-Path $repo $rel
        if (-not (Test-Path $path)) {
            Write-Host "ERROR [rule 9] missing file: $rel"
            $errors++
            continue
        }
        $hit = Select-String -Path $path -Pattern $g.sites[$rel] | Select-Object -First 1
        if (-not $hit) {
            Write-Host "ERROR [rule 9] ${rel}: $($g.label) definition not found"
            $errors++
            continue
        }
        $values[$rel] = [int]$hit.Matches[0].Groups[1].Value
    }
    if ($values.Count -eq $g.sites.Count -and ($values.Values | Select-Object -Unique).Count -ne 1) {
        foreach ($rel in $values.Keys) { Write-Host "ERROR [rule 9] ${rel}: $($values[$rel])" }
        Write-Host "ERROR [rule 9] $($g.label) must match across C++ and HLSL"
        $errors++
    }
}

# 規則 10: ローカライズ (M47)
# 10-a  Tr() の戻り値を printf 系の書式引数に渡さない
#       ImGui::Text 系 / SetTooltip は printf。訳文に % が入った瞬間に未定義動作になる
# 10-b  LocalizationTable.inl の en/ja が整合していること
#       - どちらも非空
#       - "###" を含む行は "###" 以降 (= ImGui の ID) が完全一致
#       - 変換指定子の並びが一致 (MSVC printf は "%1$s" 形式に非対応で語順を変えられない)
#       - "###" の右辺がテーブル内で一意 (ウィンドウ ID の衝突防止)
# 注: 日本語を含む行を読むので Select-String は使わない
#     (Windows PowerShell 5.1 は BOM 無しファイルを ANSI として読み、マッチが不発になる)
$trFmtPattern = 'ImGui::(Text|TextDisabled|TextWrapped|TextColored|BulletText|LabelText|SetTooltip|SetItemTooltip)\s*\(\s*(::)?(mye::)?Tr\s*\('
foreach ($f in Get-Sources) {
    Test-CodeLines $f $trFmtPattern 'rule 10' 'Tr() must not be a printf format argument - use TextUnformatted(Tr(x)) or Text("%s", Tr(x))'
}

$tablePath = Join-Path $repo 'src\Engine\Core\LocalizationTable.inl'
if (-not (Test-Path $tablePath)) {
    Write-Host "ERROR [rule 10] missing file: src\Engine\Core\LocalizationTable.inl"
    $errors++
} else {
    $seenIds = @{}
    $lineNo = 0
    foreach ($line in [System.IO.File]::ReadLines($tablePath)) {
        $lineNo++
        $m = [regex]::Match($line, '^\s*MYE_STR\(\s*(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)')
        if (-not $m.Success) {
            if ($line -match '^\s*MYE_STR\(') {
                Write-Host "ERROR [rule 10] ${tablePath}:${lineNo}: malformed MYE_STR entry"
                $errors++
            }
            continue
        }
        $id = $m.Groups[1].Value
        $en = $m.Groups[2].Value
        $ja = $m.Groups[3].Value

        if ($en -eq '' -or $ja -eq '') {
            Write-Host "ERROR [rule 10] ${tablePath}:${lineNo}: ${id}: empty string"
            $errors++
        }
        $enId = if ($en -match '###(.*)$') { $Matches[1] } else { '' }
        $jaId = if ($ja -match '###(.*)$') { $Matches[1] } else { '' }
        if ($enId -ne $jaId) {
            Write-Host "ERROR [rule 10] ${tablePath}:${lineNo}: ${id}: ### id differs ('$enId' vs '$jaId')"
            $errors++
        }
        if ($enId -ne '') {
            if ($seenIds.ContainsKey($enId)) {
                Write-Host "ERROR [rule 10] ${tablePath}:${lineNo}: ${id}: duplicate ### id '$enId' (also $($seenIds[$enId]))"
                $errors++
            } else {
                $seenIds[$enId] = $id
            }
        }
        # 変換指定子 ("%%" はリテラルなので除外) の並びを比較
        $specRe = '%(?!%)[-+ #0-9.*]*(?:hh|h|ll|l|z|j|t|L|I32|I64|I)?[a-zA-Z]'
        $enSpecs = ([regex]::Matches($en, $specRe) | ForEach-Object { $_.Value }) -join ','
        $jaSpecs = ([regex]::Matches($ja, $specRe) | ForEach-Object { $_.Value }) -join ','
        if ($enSpecs -ne $jaSpecs) {
            Write-Host "ERROR [rule 10] ${tablePath}:${lineNo}: ${id}: format specifiers differ ('$enSpecs' vs '$jaSpecs')"
            $errors++
        }
    }
}

Write-Host "=== result: $errors error(s), $warnings warning(s) ==="
if ($errors -gt 0) { exit 1 }
exit 0
