# cull_counts.ps1 — --render-stats-dump の "counts" 節を機械で比べる (M99i、cull_verify.bat から呼ぶ)
#   counts は決定的 (Debug / Release / --warp で一致) な欄だけを持つ。gpuMs / cpuMs は実測の参考値なので比べない。
#
#   使い方:
#     cull_counts.ps1 -A a.json -B b.json                       … counts 全体が完全一致なら exit 0
#     cull_counts.ps1 -A a.json -B b.json -Ignore culledNonMesh … 指定した欄を除く (基点 exe の dump は M99 の新しい欄を持たない)
#     cull_counts.ps1 -A a.json -B b.json -Only culled -View 1  … viewKey 1 の culled だけ比べる
#     cull_counts.ps1 -A a.json -B b.json -ExpectDiff           … 差があれば exit 0 (「カットで本当に値が動く」ことの確認用)
#   exit 0 = 期待どおり、1 = 期待と違う、2 = 引数・ファイルの不備
param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B,
    [string[]]$Ignore = @(),
    [string[]]$Only = @(),
    [int]$View = -1,
    [switch]$ExpectDiff,
    [switch]$Greater,
    [switch]$Zero
)

$ErrorActionPreference = 'Stop'
$Ignore = @($Ignore | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

# counts を "パス = 値" の辞書へ平らにする。views は配列の添字ではなく viewKey で引く
# (片方にだけ現れるビューがあっても、ずれずに「そのビューが無い」と報告できる)
function Get-CountsMap([string]$path)
{
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "[cull_counts] not found: $path"
        exit 2
    }
    $json = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($null -eq $json.counts) {
        Write-Host "[cull_counts] no counts section: $path"
        exit 2
    }
    $map = [ordered]@{}
    function Add-Leaves($prefix, $node)
    {
        foreach ($p in $node.PSObject.Properties) {
            $v = $p.Value
            if ($v -is [System.Management.Automation.PSCustomObject]) {
                Add-Leaves "$prefix$($p.Name)." $v
            } elseif ($v -is [System.Array]) {
                $map["$prefix$($p.Name)"] = ($v | ForEach-Object { "$_" }) -join ','
            } else {
                $map["$prefix$($p.Name)"] = "$v"
            }
        }
    }
    Add-Leaves 'total.' $json.counts.total
    foreach ($view in $json.counts.views) {
        $body = $view | Select-Object -Property * -ExcludeProperty viewKey
        Add-Leaves "views[$($view.viewKey)]." $body
    }
    Add-Leaves 'shadowAtlas.' $json.counts.shadowAtlas
    return $map
}

$ma = Get-CountsMap $A
$mb = Get-CountsMap $B

function Test-Wanted([string]$key)
{
    $leaf = $key.Substring($key.LastIndexOf('.') + 1)
    if ($Ignore -contains $leaf) { return $false }
    if ($Only.Count -gt 0 -and -not ($Only -contains $leaf)) { return $false }
    if ($View -ge 0 -and -not $key.StartsWith("views[$View].")) { return $false }
    return $true
}

$keys = @($ma.Keys) + @($mb.Keys) | Select-Object -Unique | Where-Object { Test-Wanted $_ }
$diffs = @()
if ($keys.Count -eq 0) {
    Write-Host "[cull_counts] no field was compared (check -Only / -View): $A vs $B"
    exit 2
}

if ($Zero) {
    $nonZero = @($keys | Where-Object { $ma[$_] -ne '0' })
    if ($nonZero.Count -gt 0) {
        Write-Host "[cull_counts] expected 0 but found: $(($nonZero | ForEach-Object { "$_=$($ma[$_])" }) -join ', ') in $A"
        exit 1
    }
    Write-Host "[cull_counts] all zero ($($keys.Count) fields): $A"
    exit 0
}
if ($Greater) {
    $notGreater = @($keys | Where-Object { -not ($mb.Contains($_) -and $ma.Contains($_) -and [double]$mb[$_] -gt [double]$ma[$_]) })
    if ($notGreater.Count -gt 0) {
        Write-Host "[cull_counts] expected B > A but not: $(($notGreater | ForEach-Object { "$_ A=$($ma[$_]) B=$($mb[$_])" }) -join ', ')"
        exit 1
    }
    Write-Host "[cull_counts] B is greater ($(($keys | ForEach-Object { "$_ $($ma[$_]) -> $($mb[$_])" }) -join ', '))"
    exit 0
}

foreach ($k in $keys) {
    $va = if ($ma.Contains($k)) { $ma[$k] } else { '<missing>' }
    $vb = if ($mb.Contains($k)) { $mb[$k] } else { '<missing>' }
    if ($va -ne $vb) { $diffs += "  $k : $va  !=  $vb" }
}

if ($ExpectDiff) {
    if ($diffs.Count -gt 0) {
        Write-Host "[cull_counts] differs as expected ($($diffs.Count) of $($keys.Count) fields)"
        exit 0
    }
    Write-Host "[cull_counts] expected a difference but all $($keys.Count) fields are equal: $A vs $B"
    exit 1
}
if ($diffs.Count -gt 0) {
    Write-Host "[cull_counts] $($diffs.Count) of $($keys.Count) fields differ: $A vs $B"
    $diffs | ForEach-Object { Write-Host $_ }
    exit 1
}
Write-Host "[cull_counts] equal ($($keys.Count) fields): $A vs $B"
exit 0
