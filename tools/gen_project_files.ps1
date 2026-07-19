# gen_project_files.ps1
# src\ 配下のソースを走査し、各 .vcxproj の <!-- BEGIN FILES --> ～ <!-- END FILES --> 区間と
# .vcxproj.filters を再生成する。ファイルを追加/削除したらこれを実行する:
#   powershell -ExecutionPolicy Bypass -File tools\gen_project_files.ps1
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo 'build'

function New-FilterGuid([string]$name) {
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $bytes = $md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes("myengine-filter-$name"))
    return "{$([Guid]::new($bytes).ToString().ToUpper())}"
}

# roots: @{ Path='src\Engine'; Prefix='' } の配列。Prefix はフィルタ表示上の親フォルダ名
function Get-SourceItems([object[]]$roots) {
    $items = @()
    foreach ($root in $roots) {
        $rootFull = Join-Path $repo $root.Path
        if (-not (Test-Path $rootFull)) { continue }
        Get-ChildItem -Recurse -File $rootFull |
            Where-Object { $_.Extension -in '.cpp', '.c', '.h', '.hpp', '.inl' } |
            ForEach-Object {
                $rel = [System.IO.Path]::GetRelativePath($buildDir, $_.FullName)
                $dirRel = [System.IO.Path]::GetRelativePath($rootFull, $_.DirectoryName)
                if ($dirRel -eq '.') { $dirRel = '' }
                $filter = if ($root.Prefix) {
                    if ($dirRel) { Join-Path $root.Prefix $dirRel } else { $root.Prefix }
                } else { $dirRel }
                $kind = if ($_.Extension -in '.cpp', '.c') { 'ClCompile' } else { 'ClInclude' }
                $items += [pscustomobject]@{ Rel = $rel; Filter = $filter; Kind = $kind }
            }
    }
    return $items | Sort-Object Kind, Rel
}

function Update-Vcxproj([string]$projName, [object[]]$srcItems) {
    $projPath = Join-Path $buildDir "$projName.vcxproj"
    $lines = foreach ($i in $srcItems) { "    <$($i.Kind) Include=`"$($i.Rel)`" />" }
    $body = "  <ItemGroup>`r`n" + (($lines -join "`r`n") + "`r`n") + "  </ItemGroup>"
    $text = Get-Content -Raw $projPath
    $pattern = '(?s)(<!-- BEGIN FILES[^>]*-->).*?(<!-- END FILES -->)'
    if ($text -notmatch $pattern) { throw "$projName.vcxproj: FILES マーカーが見つからない" }
    $newText = [regex]::Replace($text, $pattern, { param($m) $m.Groups[1].Value + "`r`n" + $body + "`r`n  " + $m.Groups[2].Value })
    Set-Content -Path $projPath -Value $newText -Encoding utf8 -NoNewline
    Write-Host "updated $projName.vcxproj ($($srcItems.Count) files)"
}

function Update-Filters([string]$projName, [object[]]$allItems) {
    $filtersPath = Join-Path $buildDir "$projName.vcxproj.filters"
    # 親フィルタを全て列挙 (Engine\HotReload → Engine も必要)
    $filterSet = [System.Collections.Generic.SortedSet[string]]::new()
    foreach ($i in $allItems) {
        if (-not $i.Filter) { continue }
        $parts = $i.Filter -split '\\'
        for ($n = 1; $n -le $parts.Count; $n++) {
            [void]$filterSet.Add(($parts[0..($n - 1)] -join '\'))
        }
    }
    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine('<?xml version="1.0" encoding="utf-8"?>')
    [void]$sb.AppendLine('<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">')
    [void]$sb.AppendLine('  <ItemGroup>')
    foreach ($f in $filterSet) {
        [void]$sb.AppendLine("    <Filter Include=`"$f`">")
        [void]$sb.AppendLine("      <UniqueIdentifier>$(New-FilterGuid $f)</UniqueIdentifier>")
        [void]$sb.AppendLine('    </Filter>')
    }
    [void]$sb.AppendLine('  </ItemGroup>')
    [void]$sb.AppendLine('  <ItemGroup>')
    foreach ($i in $allItems) {
        if ($i.Filter) {
            [void]$sb.AppendLine("    <$($i.Kind) Include=`"$($i.Rel)`">")
            [void]$sb.AppendLine("      <Filter>$($i.Filter)</Filter>")
            [void]$sb.AppendLine("    </$($i.Kind)>")
        } else {
            [void]$sb.AppendLine("    <$($i.Kind) Include=`"$($i.Rel)`" />")
        }
    }
    [void]$sb.AppendLine('  </ItemGroup>')
    [void]$sb.AppendLine('</Project>')
    Set-Content -Path $filtersPath -Value $sb.ToString() -Encoding utf8 -NoNewline
    Write-Host "updated $projName.vcxproj.filters"
}

# ---- Engine: src\Engine + src\Shared (+ 表示用に external\imgui) ----
$engineSrc = Get-SourceItems @(
    @{ Path = 'src\Engine'; Prefix = '' },
    @{ Path = 'src\Shared'; Prefix = 'Shared' }
)
Update-Vcxproj 'Engine' $engineSrc
$engineExternal = Get-SourceItems @(@{ Path = 'external\imgui'; Prefix = 'external\imgui' })
# external は vcxproj に手書き済み (WarningLevel 指定のため)。filters のみ反映
Update-Filters 'Engine' ($engineSrc + $engineExternal)

# ---- Editor ----
$editorSrc = Get-SourceItems @(@{ Path = 'src\Editor'; Prefix = '' })
Update-Vcxproj 'Editor' $editorSrc
Update-Filters 'Editor' $editorSrc

# ---- GameLogic: src\GameLogic + src\Shared ----
$logicSrc = Get-SourceItems @(
    @{ Path = 'src\GameLogic'; Prefix = '' },
    @{ Path = 'src\Shared'; Prefix = 'Shared' }
)
Update-Vcxproj 'GameLogic' $logicSrc
Update-Filters 'GameLogic' $logicSrc
