# track_verify.ps1 — mye_track (AI Change Tracking の追跡サービス) の回帰検証。**エディタを起動しない**。
#
#   tools\track_verify.bat            全シナリオ
#   tools\track_verify.bat --update   期待 NDJSON を撮り直す (差分を読んでからコミットすること)
#
# 先に tools\build_track.bat で mye_track.exe を作っておく (tools\track\target\release を使う)。
# cargo test は別 (CI は cargo test && build_track.bat && track_verify.bat の順に回す)。
# `# mcp` 用の mye_mcp.exe は、このスクリプトが cache\track_verify\mcp_target へ別にビルドする
# (tools\mcp\target\release の mye_mcp.exe は、開発機で Claude Code が使っていて置き換えられない)。
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
#   # mcp <行>                    mye_mcp.exe を --project <一時プロジェクト> で 1 回起動し、JSON-RPC の 1 行を流して stdin を閉じ、
#                               出力の終わりまで読む (サービスが MCP の stdout を掴んでいると、ここで止まって失敗する)。
#                               MYE_TRACK_EXE は mye_track.exe を指すので、サービスが居なければ MCP が起動する。応答は
#                               {"mcp":{"isError":..,"body":<structuredContent>}} の 1 行で出力に足す。
#                               行の {SESSION} は、直前までの change_session_begin の sessionId。MCP が起動したサービスは、
#                               以後このスクリプトが管理する (# stop や終了時の shutdown の対象)
#   # mcp-nostart <行>            # mcp と同じだが、MYE_TRACK_EXE を存在しないファイルにする (既に動いているサービスへ
#                               繋がることの確認。起動しようとすると isError になる)
#   # wait-watching               サービスが phase = watching になるまで待つ (MCP が起動したサービスのベースライン待ち)
#   # hook <pre|post|session-start|session-end> <JSON>
#                               mye_track.exe hook <sub> --project <一時プロジェクト> を別プロセスで起動し、JSON を標準入力へ渡す。
#                               JSON の {ROOT} はプロジェクトのパス (\ を重ねたもの)。終了コード・標準出力のバイト数を
#                               {"hook":{"exit":..,"stdoutBytes":..}} の 1 行で出力に足す。標準出力が閉じない (サービスが掴んでいる)・
#                               5 秒を超える・標準出力が空でないなら失敗。フックが起動したサービスは、以後このスクリプトが管理する
#   # hook-noproject <sub> <JSON> # --project を付けずに起動する (cwd / file_path から親をたどる経路の確認)
#   # exists <相対パス>           パスの有無を {"exists":{"path":..,"value":bool}} の 1 行で出力に足す (.mye が作られていないことの確認)
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
$mcpManifest = Join-Path $repo 'tools\mcp\Cargo.toml'
$mcpTarget = Join-Path $repo 'cache\track_verify\mcp_target'
$mcpExe = Join-Path $mcpTarget 'release\mye_mcp.exe'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$mcpLimitMs = 30000
$readyLimitMs = 60000
$requestLimitMs = 60000
$hookLimitMs = 15000   # フックの終了を待つ上限 (フック自身の上限は 5 秒)
$hookBudgetMs = 6000   # 5 秒の上限 + プロセスの起動と終了

Set-Location $repo
if (-not (Test-Path $exe)) { Write-Host "[track_verify] $exe not found (run tools\build_track.bat first)"; exit 1 }
# `# mcp` のシナリオが使う mye_mcp.exe (release)。専用の出力先なので、使用中の mye_mcp.exe とぶつからない。変更が無ければ cargo は何もしない
& cargo build --release --manifest-path $mcpManifest --target-dir $mcpTarget 2>&1 | ForEach-Object { Write-Host "[track_verify] cargo: $_" }
if ($LASTEXITCODE -ne 0) { Write-Host "[track_verify] cargo build of tools\mcp failed"; exit 1 }
if (-not (Test-Path $mcpExe)) { Write-Host "[track_verify] $mcpExe not found"; exit 1 }

# --- 1 つのシナリオの実行状態 -------------------------------------------------------------

$script:svc = $null       # 起動中のサービスの Process
$script:client = $null    # シナリオの接続 (mye_track client の Process)
$script:proj = $null      # 一時プロジェクトのパス
$script:svcErr = $null    # サービスの標準エラーの出力先
$script:svcStopping = $false  # シナリオの中で shutdown を送った (サービスは自分で終わる)
$script:svcAdopted = $false   # svc は MCP が起動したものを service.json の pid で掴んだ (子ではない)
$script:session = $null   # 直近の change_session_begin (MCP) が返した sessionId
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
    Wait-Watching
}

function Wait-Watching {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($readyLimitMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($script:svc.HasExited) { throw "mye_track serve exited early (code $($script:svc.ExitCode))" }
        $status = Invoke-Raw @('{"id":2,"op":"status"}')
        if ($status.Count -ge 2 -and ($status[1] | ConvertFrom-Json).result.phase -eq 'watching') { return }
        Start-Sleep -Milliseconds 100
    }
    throw 'the service did not reach phase = watching'
}

# MCP が起動したサービスを、このスクリプトが起動したものと同じに扱う (service.json の pid で掴む)
function Adopt-Service {
    if ($script:svc) { return }
    $path = Join-Path $script:proj '.mye\ai_changes\service.json'
    if (-not (Test-Path -LiteralPath $path)) { return }
    $json = [System.IO.File]::ReadAllText($path, $utf8NoBom) | ConvertFrom-Json
    $script:svc = [System.Diagnostics.Process]::GetProcessById([int]$json.pid)
    $script:svcErr = Join-Path $work 'adopted.err'
    $script:svcAdopted = $true
}

# mye_mcp.exe を 1 回起動して 1 行を流す。stdin を閉じて出力の終わりまで読めること自体が、サービスが MCP の stdio を掴んでいない確認
function Invoke-Mcp([string]$requestLine, [bool]$noStart) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $mcpExe
    $psi.Arguments = "--project `"$($script:proj)`""
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.StandardInputEncoding = $utf8NoBom
    $psi.StandardOutputEncoding = $utf8NoBom
    $psi.StandardErrorEncoding = $utf8NoBom
    $psi.Environment['MYE_TRACK_EXE'] = if ($noStart) { Join-Path $work 'no_such\mye_track.exe' } else { $exe }
    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $p.StandardInput.NewLine = "`n"
    $p.StandardInput.WriteLine($requestLine)
    $p.StandardInput.Close()
    if (-not $outTask.Wait($mcpLimitMs)) {
        $p.Kill()
        throw "mye_mcp did not reach the end of its output within ${mcpLimitMs} ms (does the service hold its stdout?)"
    }
    if (-not $p.WaitForExit($mcpLimitMs)) { $p.Kill(); throw 'mye_mcp did not exit after its stdin was closed' }
    if ($p.ExitCode -ne 0) { throw "mye_mcp exited with $($p.ExitCode): $($errTask.Result)" }
    $lines = @($outTask.Result -split "`n" | Where-Object { $_ -ne '' })
    if ($lines.Count -ne 1) { throw "mye_mcp wrote $($lines.Count) lines for 1 request" }
    $response = $lines[0]
    $body = if ($response -match '"structuredContent":(\{.*\})\}\}$') { $Matches[1] } else { $response }
    $isError = if ($response -match '"isError":true') { 'true' } else { 'false' }
    if ($requestLine -match '"change_session_begin"' -and $body -match '"sessionId":"([0-9a-f-]{36})"') { $script:session = $Matches[1] }
    Adopt-Service
    return '{"mcp":{"isError":' + $isError + ',"body":' + $body + '}}'
}

# mye_track.exe hook を別プロセスで 1 回起動する。標準出力は Claude Code が読む想定で、空で、閉じること自体が検査
function Invoke-Hook([string]$sub, [string]$json, [bool]$withProject) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = if ($withProject) { "hook $sub --project `"$($script:proj)`"" } else { "hook $sub" }
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.StandardInputEncoding = $utf8NoBom
    $psi.StandardOutputEncoding = $utf8NoBom
    $psi.Environment['MYE_TRACK_EXE'] = $exe
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $p.StandardInput.NewLine = "`n"
    $p.StandardInput.WriteLine($json.Replace('{ROOT}', $script:proj.Replace('\', '\\')))
    $p.StandardInput.Close()
    if (-not $p.WaitForExit($hookLimitMs)) { $p.Kill(); throw "hook $sub did not exit within ${hookLimitMs} ms" }
    # フックが終わったら、読み側はすぐに EOF を受ける。サービスが標準出力を掴んでいれば、ここで止まる
    if (-not $outTask.Wait($hookLimitMs)) { throw "hook $sub exited but its stdout did not close (does the service hold it?)" }
    $out = $outTask.Result
    if ($out.Length -ne 0) { throw "hook $sub wrote to stdout: $out" }
    if ($p.ExitCode -ne 0) { throw "hook $sub exited with $($p.ExitCode): $($errTask.Result)" }
    if ($watch.ElapsedMilliseconds -gt $hookBudgetMs) { throw "hook $sub took $($watch.ElapsedMilliseconds) ms" }
    Adopt-Service
    return '{"hook":{"exit":0,"stdoutBytes":0}}'
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
    # MCP が起動したサービスは子ではないので、終了コードを読めない (終了したことだけを確かめる)
    if (-not $script:svcAdopted -and $script:svc.ExitCode -ne 0) { throw "the service exited with $($script:svc.ExitCode)" }
    $script:svc = $null
    $script:svcAdopted = $false
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
                    } elseif ($line -match '^#\swait-watching\s*$') {
                        Wait-Watching
                    } elseif ($line -cmatch '^#\shook(-noproject)?\s(pre|post|session-start|session-end)\s(.+)$') {
                        $actual.Add((Normalize (Invoke-Hook $Matches[2] $Matches[3] ($Matches[1] -ne '-noproject'))))
                    } elseif ($line -cmatch '^#\sexists\s(\S+)$') {
                        $present = (Test-Path -LiteralPath (Join-Path $script:proj $Matches[1])).ToString().ToLower()
                        $actual.Add('{"exists":{"path":"' + $Matches[1].Replace('\', '/') + '","value":' + $present + '}}')
                    } elseif ($line -cmatch '^#\smcp(-nostart)?\s(.+)$') {
                        $mcpLine = $Matches[2].Replace('{SESSION}', [string]$script:session)
                        $actual.Add((Normalize (Invoke-Mcp $mcpLine ($Matches[1] -eq '-nostart'))))
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
            # フック・MCP が起動したサービスを、掴み損ねたまま残さない
            try { Adopt-Service } catch { }
            if ($script:svc -and -not $script:svc.HasExited) { $script:svc.Kill(); $script:svc.WaitForExit(10000) | Out-Null }
            $script:svc = $null
            $script:svcAdopted = $false
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
