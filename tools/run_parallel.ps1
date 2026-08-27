# run_parallel.ps1 — replay_verify.bat の並列ジョブ runner
#
# ジョブ = 「Entry の bat へ `--job <名前>` で再入」の形に固定してある。コマンド文字列を
# ファイルや引数で受け渡すと cmd のエスケープ (^| ^& ^( の類) を全部踏むので、
# 「名前しか渡さない」ことで構造的に回避している。
#
# - GUI サブシステムの exe は PowerShell から直接呼ぶと待てず exit code も取れない
#   (CLAUDE.md の罠)。cmd /c を挟み、リダイレクトも cmd 側にやらせる。
# - ★子 cmd には chcp 437 (単バイト CP) を強制する。bat は UTF-8 + 日本語 rem で、
#   コンソール CP が多バイト (932/65001) だと cmd のバッチ読取りが goto の後に
#   バイト数と文字数のずれで読み位置をドリフトさせ、日本語 rem の断片をコマンドとして
#   実行して即死する (exit 255)。どの配置で壊れるかは運次第 — コメントを 1 行足しただけで
#   「通っていた呼び形」が落ちるようになることを実測済み。だから「たまたま通る配置」に
#   頼らず、1 バイト = 1 文字でドリフトが原理的に起きない単バイト CP で構造的に殺す。
#   ジョブ側の echo を ASCII に限定してあるのもこのため (日本語 echo は 437 で化ける)。
# - 出力は <LogDir>\<name>.log へ隔離し、全ジョブ完了後に投入順で全文流す —
#   並列でも CI ログが「順番に読める」形を保つ。実行中は start/done の 1 行だけ。
# - 並列度の既定は論理コア数。MYE_REPLAY_JOBS 環境変数で上書きできる。
# - exit 0 = 全ジョブ成功。1 本でも非 0 (SEH の負値含む) なら exit 1。
#   途中で失敗しても残りは最後まで回す — CI で全体像が一度に見えることを優先する。
param(
    # ★カレントディレクトリからの相対パスで、空白を含まないこと (引用符なしで call に
    #   渡すため。人間や CI が bat を叩くのと同じ呼び形に固定する)
    [Parameter(Mandatory = $true)][string]$Entry,
    [Parameter(Mandatory = $true)][string]$LogDir,
    [Parameter(Mandatory = $true)][string]$Jobs
)
$ErrorActionPreference = 'Stop'
# ★本体を try/finally で包み、途中の exit や例外を経由しても finally が必ず走るようにする
#   (PowerShell の exit は try 内で呼ばれても finally をスキップしない)。これが無いと
#   ジョブが 1 本も走らない引数エラーの早期 exit 1 でも chcp が戻らないまま抜けてしまう
try {
if ([IO.Path]::IsPathRooted($Entry) -or $Entry.Contains(' ') -or -not (Test-Path $Entry)) {
    Write-Host "[parallel] Entry must be an existing relative path without spaces: $Entry"
    exit 1
}

# -File 経由の起動では配列バインドが効かない (カンマごと 1 文字列で届く) ので自前で割る
$names = @($Jobs -split ',' | Where-Object { $_ -ne '' })
if ($names.Count -eq 0) {
    Write-Host '[parallel] no jobs given'
    exit 1
}

$maxParallel = [Environment]::ProcessorCount
if ($env:MYE_REPLAY_JOBS) { $maxParallel = [int]$env:MYE_REPLAY_JOBS }
if ($maxParallel -lt 1) { $maxParallel = 1 }

New-Item -ItemType Directory -Force $LogDir | Out-Null
$logRoot = (Resolve-Path $LogDir).Path

$watch = [System.Diagnostics.Stopwatch]::StartNew()
$stamp = { '{0,6:f1}s' -f $watch.Elapsed.TotalSeconds }

Write-Host "[parallel] $($names.Count) jobs, up to $maxParallel concurrent"
$pending = [System.Collections.Generic.Queue[string]]::new()
$names | ForEach-Object { $pending.Enqueue($_) }
$running = @{}            # name -> Process
$results = [ordered]@{}   # name -> exit code

while ($pending.Count -gt 0 -or $running.Count -gt 0) {
    while ($pending.Count -gt 0 -and $running.Count -lt $maxParallel) {
        $name = $pending.Dequeue()
        $log = Join-Path $logRoot "$name.log"
        # コンソールは親と共有する (-NoNewWindow) — Ctrl+C で子ごと止まり、隠し窓も
        # 出ない。出力は cmd 側の > で各ログへ隔離済みなので共有で混ざるものは無い
        $p = Start-Process -FilePath 'cmd.exe' `
            -ArgumentList "/c chcp 437 >nul & call $Entry --job $name > `"$log`" 2>&1" `
            -PassThru -NoNewWindow
        # ★Handle に一度触っておく — 触らないと exit 後に ExitCode が取れないことがある
        #   (Start-Process の古典的な罠)
        $null = $p.Handle
        $running[$name] = $p
        Write-Host "[parallel] $(& $stamp) start: $name (pid $($p.Id))"
    }
    Start-Sleep -Milliseconds 200
    foreach ($name in @($running.Keys)) {
        $p = $running[$name]
        if ($p.HasExited) {
            $results[$name] = $p.ExitCode
            $running.Remove($name)
            $state = if ($p.ExitCode -eq 0) { 'PASS' } else { "FAIL (exit $($p.ExitCode))" }
            Write-Host "[parallel] $(& $stamp) done : $name -> $state"
        }
    }
}

# 全ジョブのログを投入順で流す (echo は bat 側を ASCII に保ってあるので既定エンコードで読める)
foreach ($name in $names) {
    Write-Host ''
    Write-Host "========== [$name] =========="
    $log = Join-Path $logRoot "$name.log"
    if (Test-Path $log) {
        Write-Host ([IO.File]::ReadAllText($log))
    } else {
        Write-Host '(no log written)'
    }
}

$failed = @($results.GetEnumerator() | Where-Object { $_.Value -ne 0 })
Write-Host ''
if ($failed.Count -gt 0) {
    $list = ($failed | ForEach-Object { "$($_.Key)(exit $($_.Value))" }) -join ', '
    Write-Host "[parallel] FAILED: $list"
    exit 1
}
Write-Host "[parallel] all $($names.Count) jobs passed in $(& $stamp)"
exit 0
}
finally {
    # ★ジョブの子 cmd が chcp 437 (単バイト CP) で起動している (冒頭コメント参照)。
    #   chcp は**プロセスではなくコンソールの状態**を変えるので、-NoNewWindow で親と
    #   コンソールを共有するこの runner を経由すると、戻し忘れた分は呼び出し元シェルの
    #   コードページごと変わったまま残る (Claude Code の TUI 表示も同じコンソールを
    #   共有していれば道連れで文字化けする — 2026-08-27 に実際に発生し、手動で
    #   `chcp 65001` して復旧した)。normal 終了・異常終了どちらでも必ず戻す
    chcp 65001 | Out-Null
}
