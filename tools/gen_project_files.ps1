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
$engineExternal = Get-SourceItems @(
    @{ Path = 'external\imgui'; Prefix = 'external\imgui' },
    @{ Path = 'external\libtess2'; Prefix = 'external\libtess2' },
    @{ Path = 'external\meshoptimizer'; Prefix = 'external\meshoptimizer' },
    @{ Path = 'external\recastnavigation'; Prefix = 'external\recastnavigation' }
)
# external は vcxproj に手書き済み (WarningLevel 指定のため)。filters のみ反映
Update-Filters 'Engine' ($engineSrc + $engineExternal)

# ---- Editor ----
$editorSrc = Get-SourceItems @(@{ Path = 'src\Editor'; Prefix = '' })
Update-Vcxproj 'Editor' $editorSrc
Update-Filters 'Editor' $editorSrc

# ---- Runtime (エディタ UI 無しの薄いランタイム、M15) ----
$runtimeSrc = Get-SourceItems @(@{ Path = 'src\Runtime'; Prefix = '' })
Update-Vcxproj 'Runtime' $runtimeSrc
Update-Filters 'Runtime' $runtimeSrc

# ---- Server (ヘッドレス専用サーバ、M81) ----
$serverSrc = Get-SourceItems @(@{ Path = 'src\Server'; Prefix = '' })
Update-Vcxproj 'Server' $serverSrc
Update-Filters 'Server' $serverSrc

# ---- Compute.ForEach のカーネル表 (M91e): assets\shaders\kernels\*.kernel.hlsl → src\GameLogic\Generated\Kernels.gen.{h,cpp} ----
# ハッシュの手順の正本は src\Engine\Engine\Compute\KernelHash.h (BOM を 1 個除く → CR+LF の CR を除く → FNV-1a 64bit)。
# 参照値: "a" = af63dc4c8601ec8c (KernelSelfTest と同じテストベクタ)。算術は C# の unchecked で行う (PowerShell の [uint64] は乗算で溢れて例外になる)
if (-not ('MyeKernelHash' -as [type])) {
    Add-Type -TypeDefinition @'
public static class MyeKernelHash
{
    public static ulong Compute(byte[] data)
    {
        int start = 0;
        if (data.Length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) { start = 3; }
        ulong h = 14695981039346656037UL;
        for (int i = start; i < data.Length; ++i)
        {
            if (data[i] == 0x0D && i + 1 < data.Length && data[i + 1] == 0x0A) { continue; }
            h ^= data[i];
            h *= 1099511628211UL;
        }
        return h;
    }
}
'@
}

function Get-KernelSourceHash([byte[]]$bytes) {
    return [MyeKernelHash]::Compute($bytes)
}

# KERNEL(Name, ParamsType, RW(T, a) RO(U, b)) の宣言から、HLSL の変数名を宣言順に取り出す (コメントは先に除く)
function Get-KernelArgNames([string]$text, [string]$fileName) {
    $noComments = [regex]::Replace($text, '(?s)/\*.*?\*/|//[^\r\n]*', ' ')
    $m = [regex]::Match($noComments, '(?s)\bKERNEL\s*\(\s*(\w+)\s*,\s*(\w+)\s*,(.*?)\)\s*\{')
    if (-not $m.Success) { throw "$fileName : KERNEL(Name, Params, RW(...) RO(...)) の宣言が見つからない" }
    $list = @()
    foreach ($a in [regex]::Matches($m.Groups[3].Value, '\b(RW|RO)\s*\(\s*[^,()]+?\s*,\s*(\w+)\s*\)')) {
        $list += [pscustomobject]@{ Kind = $a.Groups[1].Value; Name = $a.Groups[2].Value }
    }
    if ($list.Count -eq 0) { throw "$fileName : KERNEL にバッファ引数 (RW / RO) がない" }
    return [pscustomobject]@{ Name = $m.Groups[1].Value; Params = $m.Groups[2].Value; Args = $list }
}

function Write-IfChanged([string]$path, [string]$text) {
    $existing = if (Test-Path $path) { [System.IO.File]::ReadAllText($path) } else { $null }
    if ($existing -ne $text) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
        [System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))
        Write-Host "updated $([System.IO.Path]::GetRelativePath($repo, $path))"
    }
}

function Update-KernelGenerated {
    if ((Get-KernelSourceHash ([System.Text.Encoding]::UTF8.GetBytes('a'))).ToString('x16') -ne 'af63dc4c8601ec8c') {
        throw 'カーネルのソースハッシュの参照値 ("a" = af63dc4c8601ec8c) と合わない'
    }
    $kernelDirs = @((Join-Path $repo 'assets\shaders\kernels'))
    $genDir = Join-Path $repo 'src\GameLogic\Generated'
    $kernels = @()
    foreach ($dir in $kernelDirs) {
        if (-not (Test-Path $dir)) { continue }
        foreach ($f in Get-ChildItem -File $dir -Filter '*.kernel.hlsl') {
            $base = $f.Name.Substring(0, $f.Name.Length - '.kernel.hlsl'.Length)
            $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
            $text = [System.Text.Encoding]::UTF8.GetString($bytes)
            $decl = Get-KernelArgNames $text $f.Name
            if ($decl.Name -ne $base) { throw "$($f.Name) : KERNEL の名前 '$($decl.Name)' がファイル名 '$base' と違う" }
            $rel = [System.IO.Path]::GetRelativePath($genDir, $f.FullName).Replace('\', '/')
            $kernels += [pscustomobject]@{ Name = $base; Rel = $rel; Hash = (Get-KernelSourceHash $bytes); Args = $decl.Args }
        }
    }
    # 名前の序数順 (文化圏に依らず同じ並びにする)
    $sorted = [System.Collections.Generic.List[object]]::new()
    foreach ($k in $kernels) { $sorted.Add($k) }
    $sorted.Sort([System.Comparison[object]]{ param($a, $b) [string]::CompareOrdinal($a.Name, $b.Name) })
    $kernels = $sorted.ToArray()

    $nl = "`n"
    $banner = "// 自動生成 (tools\gen_project_files.ps1)。手編集しても次の実行で上書きされる。" + $nl +
        "// assets\shaders\kernels\*.kernel.hlsl を GameLogic.dll の CPU 版として取り込み、カーネル表を作る。" + $nl +
        "// .kernel.hlsl を編集したら gen_project_files.ps1 を実行して GameLogic を再ビルドすること" + $nl +
        "// (表のハッシュとディスク上のファイルが食い違うと、エンジンは CPU 版を古いものとして扱う)。" + $nl
    $h = $banner + "#pragma once" + $nl + '#include "Shared/KernelCompat.h"' + $nl
    foreach ($k in $kernels) {
        # 無名名前空間に入れる: カーネルの自由関数は inline ではないので、複数の TU がこのヘッダを include してもリンクが衝突しない。
        # 型は TU ごとに別になるが、境界はバイト列 (void*) で越えるので問題ない
        $h += $nl + "#pragma warning(push)" + $nl + "#pragma warning(disable : 4100 4505)" + $nl
        $h += "namespace mye::kernel::cpu::$($k.Name) {" + $nl + "namespace {" + $nl + "#include `"$($k.Rel)`"" + $nl + "} // namespace" + $nl + "}" + $nl
        $h += "#pragma warning(pop)" + $nl
        $h += "namespace MyeKernels::$($k.Name) { using namespace ::mye::kernel::cpu::$($k.Name); }" + $nl
    }
    $c = $banner + '#include "GameLogic/Generated/Kernels.gen.h"' + $nl + '#include "Shared/KernelRangeFn.h"' + $nl + $nl + "namespace {" + $nl
    foreach ($k in $kernels) {
        $fn = "&::mye::kernel::cpu::$($k.Name)::$($k.Name)_Cpu"
        $names = ($k.Args | ForEach-Object { "`"$($_.Name)`"" }) -join ', '
        $c += $nl + "// $($k.Name)" + $nl
        $c += "constexpr const char* const k$($k.Name)_argNames[] = { $names };" + $nl
        $c += "constexpr auto k$($k.Name)_args = ::mye::kernel::MakeKernelArgs<$fn>(k$($k.Name)_argNames);" + $nl
        $c += "const ::mye_script_detail::KernelRegistrar k$($k.Name)_registrar(" + $nl
        $c += "    ::mye::kernel::MakeKernelDesc<$fn>(`"$($k.Name)`", 0x$($k.Hash.ToString('x16'))ull, k$($k.Name)_args.data()));" + $nl
    }
    $c += $nl + "} // namespace" + $nl
    Write-IfChanged (Join-Path $genDir 'Kernels.gen.h') $h
    Write-IfChanged (Join-Path $genDir 'Kernels.gen.cpp') $c
    Write-Host "kernels: $($kernels.Count) ($(($kernels | ForEach-Object { $_.Name }) -join ', '))"
}
Update-KernelGenerated

# ---- GameLogic: src\GameLogic + src\Shared ----
$logicSrc = Get-SourceItems @(
    @{ Path = 'src\GameLogic'; Prefix = '' },
    @{ Path = 'src\Shared'; Prefix = 'Shared' }
)
Update-Vcxproj 'GameLogic' $logicSrc
Update-Filters 'GameLogic' $logicSrc
