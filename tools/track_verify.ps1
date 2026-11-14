# track_verify.ps1 — mye_track (AI Change Tracking の追跡サービス) の回帰検証。**エディタを起動しない**。
#
#   tools\track_verify.bat            全シナリオ
#   tools\track_verify.bat --update   期待 NDJSON を撮り直す (差分を読んでからコミットすること)
#
# 先に tools\build_track.bat で mye_track.exe を作っておく (tools\track\target\release を使う)。
# cargo test は別 (CI は cargo test && build_track.bat && track_verify.bat の順に回す)。
#
# やること: tests\track\*.ndjson の要求行を、サービスの pipe へ `mye_track client` 経由で流し、
# 応答を *.expected.ndjson と比較する。シナリオごとに一時プロジェクトを作り、サービスを起動し、終わったら
# pipe の shutdown で止める (プロセスを残さない。落ちても finally で強制終了する)。
#
# 要求行の置換: {TOKEN} = service.json のトークン、{ROOT} = プロジェクトルート (JSON 用に \ を重ねたもの)、
#   @@REP:<1 文字>:<回数>@@ = その文字の繰り返し (1 MiB の上限の試験用)。
# '# ' で始まる行はディレクティブか説明:
#   # write <相対パス> <本文>     本文の \n は改行。サービスが動いている間に書けば変更として記録される
#   # bulk <接頭辞> <件数>        <接頭辞>000.txt から <件数> 個の小さいファイルを一度に書く
#   # delete <相対パス>
#   # wait-recorded <件数>        list の件数がそれ以上になるまで待つ (最大 60 秒)
#   # page-through <limit>        list を cursor で最後までたどり、ページ数・件数・重複の有無を 1 行で出力する
#   # reconnect                   シナリオの接続を閉じて開き直す (hello から)
#   # stop                        pipe の shutdown でサービスを止める (終了コード 0 を確かめる)
#   # start                       サービスを起動し直す (ベースラインが終わるまで待つ)
# サービスは最初の要求行か wait-recorded / start のときに起動する。
#
# 出力は比較の前に正規化する: トークン・projectId・UUID (sessionId / operationId)・時刻・PID・所要時間 -> <TOKEN> / <PROJECT_ID> / <UUID> / <N>、プロジェクトのパス -> <ROOT>、
# 同じ文字の 100 回以上の連なり -> <文字>*<回数>。
[CmdletBinding()]
param(
    [switch]$Update,
    [string]$Scenario = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'tools\track\target\release\mye_track.exe'
$scenarioDir = Join-Path $repo 'tests\track'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("mye_trackvf_" + [System.Diagnostics.Process]::GetCurrentProcess().Id)
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$readyLimitMs = 60000
$requestLimitMs = 60000

Set-Location $repo
if (-not (Test-Path $exe)) { Write-Host "[track_verify] $exe not found (run tools\build_track.bat first)"; exit 1 }

# --- 1 つのシナリオの実行状態 -------------------------------------------------------------

$script:svc = $null       # 起動中のサービスの Process
$script:client = $null    # シナリオの接続 (mye_track client の Process)
$script:proj = $null      # 一時プロジェクトのパス
$script:svcErr = $null    # サービスの標準エラーの出力先
$script:svcStopping = $false  # シナリオの中で shutdown を送った (サービスは自分で終わる)
$script:failures = New-Object System.Collections.Generic.List[string]

function Escape-Json([string]$text) { return $text.Replace('\', '\\').Replace('"', '\"') }

function Get-Token {
    $path = Join-Path $script:proj '.mye\ai_changes\service.json'
    $deadline = [DateTime]::UtcNow.AddMilliseconds($readyLimitMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($script:svc.HasExited) {
            throw "mye_track serve exited (code $($script:svc.ExitCode)) before service.json appeared: $((Get-Content -LiteralPath $script:svcErr -Raw -ErrorAction SilentlyContinue) -replace '\s+', ' ')"
        }
        if (Test-Path -LiteralPath $path) {
            try {
                $json = [System.IO.File]::ReadAllText($path, $utf8NoBom) | ConvertFrom-Json
                if ($json.pid -eq $script:svc.Id) { return $json.token }
            } catch { }
        }
        Start-Sleep -Milliseconds 50
    }
    throw 'service.json did not appear'
}

function Hello-Line([string]$role) {
    $token = Get-Token
    return '{"id":1,"op":"hello","args":{"proto":1,"role":"' + $role + '","token":"' + $token + '","projectRoot":"' + (Escape-Json $script:proj) + '"}}'
}

# 1 回の接続で要求を流し、応答の行を返す (待ち合わせ用。シナリオの接続とは別)
function Invoke-Raw([string[]]$requests, [int]$waitMs = 5000) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = "client --project `"$($script:proj)`" --wait-ms $waitMs"
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.StandardInputEncoding = $utf8NoBom
    $psi.StandardOutputEncoding = $utf8NoBom
    $p = [System.Diagnostics.Process]::Start($psi)
    $p.StandardInput.NewLine = "`n"
    foreach ($r in (@(Hello-Line 'editor') + $requests)) { $p.StandardInput.WriteLine($r) }
    $p.StandardInput.Close()
    $text = $p.StandardOutput.ReadToEnd()
    $null = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return @($text -split "`n" | Where-Object { $_ -ne '' })
}

function Start-Tracker {
    if ($script:svc -and -not $script:svc.HasExited) { return }
    $errFile = Join-Path $work ("svc_" + [guid]::NewGuid().ToString('N') + '.err')
    $script:svcErr = $errFile
    # --idle-timeout-ms: 検証が途中で落ちても、孤児になったサービスは自分で終わる
    $script:svc = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden `
        -ArgumentList @('serve', '--project', "`"$($script:proj)`"", '--idle-timeout-ms', '90000') `
        -RedirectStandardError $errFile -RedirectStandardOutput ($errFile + '.out')
    $deadline = [DateTime]::UtcNow.AddMilliseconds($readyLimitMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($script:svc.HasExited) { throw "mye_track serve exited early (code $($script:svc.ExitCode))" }
        $status = Invoke-Raw @('{"id":2,"op":"status"}')
        if ($status.Count -ge 2 -and ($status[1] | ConvertFrom-Json).result.phase -eq 'watching') { return }
        Start-Sleep -Milliseconds 100
    }
    throw 'the service did not reach phase = watching'
}

function Stop-Tracker {
    if (-not $script:svc) { return }
    if (-not $script:svc.HasExited) {
        # シナリオの中で既に shutdown を送っていれば、service.json も pipe ももう無い。終了を待つだけ
        if (-not $script:svcStopping) { $null = Invoke-Raw @('{"id":2,"op":"shutdown"}') }
        if (-not $script:svc.WaitForExit(30000)) {
            $script:svc.Kill()
            throw "the service did not stop after shutdown: $((Get-Content -LiteralPath $script:svcErr -Raw -ErrorAction SilentlyContinue) -replace '\s+', ' ')"
        }
    }
    if ($script:svc.ExitCode -ne 0) { throw "the service exited with $($script:svc.ExitCode)" }
    $script:svc = $null
    $script:svcStopping = $false
}

function Close-Client {
    if (-not $script:client) { return }
    try { $script:client.StandardInput.Close() } catch { }
    if (-not $script:client.WaitForExit(10000)) { $script:client.Kill() }
    $script:client = $null
}

function Open-Client {
    Start-Tracker
    Close-Client
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = "client --project `"$($script:proj)`""
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.StandardInputEncoding = $utf8NoBom
    $psi.StandardOutputEncoding = $utf8NoBom
    $script:client = [System.Diagnostics.Process]::Start($psi)
    $script:client.StandardInput.NewLine = "`n"
    $script:client.StandardInput.AutoFlush = $true
}

# 要求を 1 行送って応答の 1 行を返す。接続が切れていれば closed の印を返す
function Send-Request([string]$line) {
    if (-not $script:client) { Open-Client }
    $closed = '{"client":"closed"}'
    if ($script:client.HasExited) { return $closed }
    try { $script:client.StandardInput.WriteLine($line) } catch { return $closed }
    $task = $script:client.StandardOutput.ReadLineAsync()
    if (-not $task.Wait($requestLimitMs)) { throw "no response within ${requestLimitMs} ms for: $($line.Substring(0, [Math]::Min(120, $line.Length)))" }
    if ($null -eq $task.Result) { return $closed }
    return $task.Result
}

function Get-ListCount {
    $resp = Invoke-Raw @('{"id":2,"op":"list","args":{"limit":200}}')
    if ($resp.Count -lt 2) { return 0 }
    $parsed = $resp[1] | ConvertFrom-Json
    if (-not $parsed.ok) { return 0 }
    return @($parsed.result.items).Count
}

function Wait-Recorded([int]$count) {
    Start-Tracker
    $deadline = [DateTime]::UtcNow.AddMilliseconds($readyLimitMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Get-ListCount) -ge $count) { return }
        Start-Sleep -Milliseconds 200
    }
    throw "list did not reach $count items"
}

# list を cursor で最後までたどる。内容は環境で変わりうるので、形だけを 1 行にまとめる
function Page-Through([int]$limit) {
    $sizes = New-Object System.Collections.Generic.List[int]
    $ids = New-Object System.Collections.Generic.HashSet[long]
    $cursor = $null
    $total = 0
    for ($i = 0; $i -lt 100; $i++) {
        $listArgs = '"limit":' + $limit
        if ($cursor) { $listArgs += ',"cursor":"' + $cursor + '"' }
        $resp = Send-Request ('{"id":' + (100 + $i) + ',"op":"list","args":{' + $listArgs + '}}') | ConvertFrom-Json
        if (-not $resp.ok) { throw "page-through: list failed: $($resp | ConvertTo-Json -Compress)" }
        $items = @($resp.result.items)
        $sizes.Add($items.Count)
        $total += $items.Count
        foreach ($item in $items) { [void]$ids.Add([long]$item.revisionId) }
        $cursor = $resp.result.nextCursor
        if (-not $cursor) { break }
    }
    return ('{"paging":{"pages":' + $sizes.Count + ',"sizes":[' + ($sizes -join ',') + '],"total":' + $total + ',"unique":' + ($ids.Count -eq $total).ToString().ToLower() + '}}')
}

function Normalize([string]$line) {
    # 一時プロジェクトのパス (プロセス ID を含む) は <ROOT> にする
    $t = $line.Replace((Escape-Json $script:proj), '<ROOT>')
    $t = [regex]::Replace($t, '"token":"[0-9a-f]{32}"', '"token":"<TOKEN>"')
    # 観測の出どころは、監視が先か管理された操作の読み取りが先かで変わる (どちらでも結論は同じ)
    $t = [regex]::Replace($t, '"source":"(watch|managed|managed_begin)"', '"source":"<SRC>"')
    $t = [regex]::Replace($t, '"projectId":"[0-9a-f]{16}"', '"projectId":"<PROJECT_ID>"')
    $t = [regex]::Replace($t, '"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"', '"<UUID>"')
    $t = [regex]::Replace($t, '"(\w*(?:At|Ms|Us)|pid|connectionPid)":-?\d+', '"$1":<N>')
    return [regex]::Replace($t, '(.)\1{99,}', { param($m) $m.Groups[1].Value + '*' + $m.Length })
}

function Remove-Tree([string]$path) {
    for ($i = 0; $i -lt 50; $i++) {
        if (-not (Test-Path -LiteralPath $path)) { return }
        try { Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop; return } catch { Start-Sleep -Milliseconds 100 }
    }
    Write-Host "[track_verify] could not remove $path"
}

# --- シナリオの一覧と実行 ---------------------------------------------------------------

$scenarios = if ($Scenario) {
    @(Join-Path $scenarioDir $Scenario)
} else {
    Get-ChildItem -File (Join-Path $scenarioDir '*.ndjson') |
        Where-Object { $_.Name -notlike '*.expected.ndjson' } |
        Sort-Object Name | ForEach-Object { $_.FullName }
}
if (-not $scenarios) { Write-Host "[track_verify] no scenarios in $scenarioDir"; exit 1 }

Remove-Tree $work
New-Item -ItemType Directory -Force -Path $work | Out-Null
$failed = 0
try {
    foreach ($path in $scenarios) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($path)
        Write-Host "=== track scenario: $name ==="
        $script:proj = Join-Path $work "proj_$name"
        New-Item -ItemType Directory -Force -Path $script:proj | Out-Null
        $actual = New-Object System.Collections.Generic.List[string]
        $errorText = $null
        try {
            foreach ($raw in [System.IO.File]::ReadAllLines($path, $utf8NoBom)) {
                $line = $raw.Trim()
                if ($line -eq '') { continue }
                if ($line.StartsWith('#')) {
                    if ($line -match '^#\swrite\s(\S+)\s(.*)$') {
                        $target = Join-Path $script:proj $Matches[1]
                        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
                        [System.IO.File]::WriteAllBytes($target, $utf8NoBom.GetBytes($Matches[2].Replace('\n', "`n")))
                    } elseif ($line -match '^#\sbulk\s(\S+)\s(\d+)$') {
                        for ($n = 0; $n -lt [int]$Matches[2]; $n++) {
                            [System.IO.File]::WriteAllBytes((Join-Path $script:proj ('{0}{1:D3}.txt' -f $Matches[1], $n)), $utf8NoBom.GetBytes("file $n`n"))
                        }
                    } elseif ($line -match '^#\sdelete\s(\S+)$') {
                        Remove-Item -LiteralPath (Join-Path $script:proj $Matches[1])
                    } elseif ($line -match '^#\swait-recorded\s(\d+)$') {
                        Wait-Recorded ([int]$Matches[1])
                    } elseif ($line -match '^#\spage-through\s(\d+)$') {
                        $actual.Add((Normalize (Page-Through ([int]$Matches[1]))))
                    } elseif ($line -match '^#\sreconnect\s*$') {
                        Open-Client
                    } elseif ($line -match '^#\sstop\s*$') {
                        Close-Client
                        Stop-Tracker
                    } elseif ($line -match '^#\sstart\s*$') {
                        Start-Tracker
                    }
                    continue
                }
                Start-Tracker
                $request = $line.Replace('{ROOT}', $script:proj.Replace('\', '\\'))
                # トークンは要る行でだけ読む (サービスが止まった後は service.json が無い)
                if ($request.Contains('{TOKEN}')) { $request = $request.Replace('{TOKEN}', (Get-Token)) }
                $request = [regex]::Replace($request, '@@REP:(.):(\d+)@@', { param($m) [string]::new($m.Groups[1].Value[0], [int]$m.Groups[2].Value) })
                $response = Send-Request $request
                if ($request -match '"op":"shutdown"' -and $response -match '"ok":true') { $script:svcStopping = $true }
                $actual.Add((Normalize $response))
            }
            Close-Client
            Stop-Tracker
        } catch {
            $errorText = $_.Exception.Message
        } finally {
            Close-Client
            if ($script:svc -and -not $script:svc.HasExited) { $script:svc.Kill(); $script:svc.WaitForExit(10000) | Out-Null }
            $script:svc = $null
            $script:svcStopping = $false
        }
        if ($errorText) {
            Write-Host "[track_verify] FAIL $name : $errorText"
            $failed++
            continue
        }

        $expectedPath = Join-Path $scenarioDir "$name.expected.ndjson"
        if ($Update) {
            [System.IO.File]::WriteAllText($expectedPath, (($actual -join "`n") + "`n"), $utf8NoBom)
            Write-Host "[track_verify] updated $expectedPath ($($actual.Count) lines)"
            continue
        }
        if (-not (Test-Path $expectedPath)) {
            Write-Host "[track_verify] FAIL $name : missing $expectedPath (run with --update)"
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
            Write-Host "[track_verify] PASS $name ($($actual.Count) lines)"
        } else {
            Write-Host "[track_verify] FAIL $name (expected $($expected.Count) lines, actual $($actual.Count) lines)"
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
} finally {
    Close-Client
    if ($script:svc -and -not $script:svc.HasExited) { $script:svc.Kill() }
    Remove-Tree $work
}

# 残ったプロセスが無いこと (このスクリプトが起動したサービスは全部止まっている)
$left = @(Get-Process -Name mye_track -ErrorAction SilentlyContinue)
if ($left.Count -gt 0) { Write-Host "[track_verify] WARNING: $($left.Count) mye_track process(es) still running (other users of the machine?)" }

if ($failed -gt 0) {
    Write-Host "=== track_verify: $failed scenario(s) FAILED ==="
    exit 1
}
Write-Host '=== track_verify: all scenarios passed ==='
exit 0
