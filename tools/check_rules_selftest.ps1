# check_rules_selftest.ps1 — check_rules.ps1 の規則自身の自己検査
# 実行: pwsh -File tools\check_rules_selftest.ps1  (期待どおりでなければ exit 1)
# 違反を仕込んだ一時ファイルを -SourceRoot で読ませ、規則が実際に error を出す (当たらない正規表現のまま
# 0 件を返し続けない) ことと、正当なコードに誤検出しないことを確かめる。今は規則 18 (M96)・規則 19 (M104)・規則 20 (M105)
$ErrorActionPreference = 'Stop'
$checker = Join-Path $PSScriptRoot 'check_rules.ps1'
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("mye_rules_selftest_" + [Guid]::NewGuid().ToString('N'))
$dir = Join-Path $root 'Engine\Engine\Disturbance'
New-Item -ItemType Directory -Force $dir | Out-Null
try {
    $bad = @(
        'float A(float x) { return std::exp(x); }',
        'float B(float x) { return pow(x, 2.0f); }',
        'float C(float x) { return sinf(x) + cos(x) + std::tan(x); }',
        'float D(float x) { return std::log(x) + std::hypot(x, x) + std::cbrt(x); }',
        'float E(float x) { return XMScalarSin(x); }'
    )
    $good = @(
        'float A(float x) { return std::sqrt(x) + std::floor(x) + std::min(x, 1.0f) + std::max(x, 0.0f); }',
        '// std::exp(x) はコメントの中なので許容',
        'void B() { MYE_LOG_WARN("single cosine"); }',
        'float single(float x) { return x; }'
    )
    Set-Content -Path (Join-Path $dir 'Bad.cpp') -Value $bad -Encoding UTF8
    Set-Content -Path (Join-Path $dir 'Good.cpp') -Value $good -Encoding UTF8
    # selftest ファイルは許可リスト (期待値の計算に CRT を使ってよい)
    Set-Content -Path (Join-Path $dir 'AnythingSelfTest.cpp') -Value $bad -Encoding UTF8

    # 規則 19 (M104): 追跡 (Editor\ChangeTracking) を下位層から include しない。Editor 側からの include と、コメント内の言及は通す
    $trackBadDir = Join-Path $root 'Engine\Engine\Asset'
    $trackRuntimeDir = Join-Path $root 'Runtime'
    $trackEditorDir = Join-Path $root 'Editor\App'
    New-Item -ItemType Directory -Force $trackBadDir, $trackRuntimeDir, $trackEditorDir | Out-Null
    Set-Content -Path (Join-Path $trackBadDir 'Bad19.cpp') -Value @(
        '#include "Editor/ChangeTracking/TrackClient.h"',
        '#  include <Editor\ChangeTracking\ChangeModel.h>',
        '// #include "Editor/ChangeTracking/Commented.h" はコメントの中なので許容'
    ) -Encoding UTF8
    Set-Content -Path (Join-Path $trackRuntimeDir 'Bad19Runtime.h') -Value @('#include "../Editor/ChangeTracking/TrackProtocol.h"') -Encoding UTF8
    Set-Content -Path (Join-Path $trackEditorDir 'Good19.cpp') -Value @(
        '#include "Editor/ChangeTracking/ChangeTrackingSession.h"',
        '#include "Editor/SourceControl/SourceControlState.h"'
    ) -Encoding UTF8
    # 規則 20 (M105): 埋め込みブラウザ (Editor\Tools\Browser) を下位層から include しない。Editor 側と、コメント内の言及は通す
    $browserBadDir = Join-Path $root 'Server'
    New-Item -ItemType Directory -Force $browserBadDir | Out-Null
    Set-Content -Path (Join-Path $browserBadDir 'Bad20.cpp') -Value @(
        '#include "Editor/Tools/Browser/BrowserHost.h"',
        '#  include <Editor\Tools\Browser\BrowserCatalog.h>',
        '// #include "Editor/Tools/Browser/Commented.h" はコメントの中なので許容'
    ) -Encoding UTF8
    Set-Content -Path (Join-Path $trackRuntimeDir 'Bad20Runtime.h') -Value @('#include "../Editor/Tools/Browser/BrowserProbe.h"') -Encoding UTF8
    Set-Content -Path (Join-Path $trackEditorDir 'Good20.cpp') -Value @(
        '#include "Editor/Tools/Browser/BrowserHost.h"',
        '#include "Editor/Tools/ModalTools.h"'
    ) -Encoding UTF8
    $out = & pwsh -NoProfile -File $checker -SourceRoot $root 2>&1 | Out-String
    $rule18 = @($out -split "`n" | Where-Object { $_ -match '\[rule 18\]' })
    $fail = 0
    $badLines = @($rule18 | Where-Object { $_ -match 'Bad\.cpp' })
    if ($badLines.Count -ne 5) { Write-Host "FAIL: expected 5 rule 18 errors in Bad.cpp, got $($badLines.Count)"; $fail++ }
    if (@($rule18 | Where-Object { $_ -match 'Good\.cpp' }).Count -ne 0) { Write-Host 'FAIL: Good.cpp must not be flagged'; $fail++ }
    if (@($rule18 | Where-Object { $_ -match 'SelfTest\.cpp' }).Count -ne 0) { Write-Host 'FAIL: SelfTest files are on the allow list'; $fail++ }
    if ($fail -eq 0) { Write-Host 'PASS: rule 18 flags the violations (5), spares clean code, comments and selftest files' }

    $rule19 = @($out -split "`n" | Where-Object { $_ -match '\[rule 19\]' })
    $fail19 = 0
    if (@($rule19 | Where-Object { $_ -match 'Bad19\.cpp' }).Count -ne 2) { Write-Host "FAIL: expected 2 rule 19 errors in Bad19.cpp (comment excluded), got $(@($rule19 | Where-Object { $_ -match 'Bad19\.cpp' }).Count)"; $fail19++ }
    if (@($rule19 | Where-Object { $_ -match 'Bad19Runtime\.h' }).Count -ne 1) { Write-Host 'FAIL: Runtime must not include ChangeTracking'; $fail19++ }
    if (@($rule19 | Where-Object { $_ -match 'Good19\.cpp' }).Count -ne 0) { Write-Host 'FAIL: an Editor file including ChangeTracking must not be flagged'; $fail19++ }
    if ($rule19.Count -ne 3) { Write-Host "FAIL: expected exactly 3 rule 19 errors in total, got $($rule19.Count)"; $fail19++ }
    if ($fail19 -eq 0) { Write-Host 'PASS: rule 19 flags includes of ChangeTracking from Engine / Runtime (3), spares the Editor layer and comments' }
    $fail += $fail19

    $rule20 = @($out -split "`n" | Where-Object { $_ -match '\[rule 20\]' })
    $fail20 = 0
    if (@($rule20 | Where-Object { $_ -match 'Bad20\.cpp' }).Count -ne 2) { Write-Host "FAIL: expected 2 rule 20 errors in Bad20.cpp (comment excluded), got $(@($rule20 | Where-Object { $_ -match 'Bad20\.cpp' }).Count)"; $fail20++ }
    if (@($rule20 | Where-Object { $_ -match 'Bad20Runtime\.h' }).Count -ne 1) { Write-Host 'FAIL: Runtime must not include Editor\Tools\Browser'; $fail20++ }
    if (@($rule20 | Where-Object { $_ -match 'Good20\.cpp' }).Count -ne 0) { Write-Host 'FAIL: an Editor file including Tools\Browser must not be flagged'; $fail20++ }
    if ($rule20.Count -ne 3) { Write-Host "FAIL: expected exactly 3 rule 20 errors in total, got $($rule20.Count)"; $fail20++ }
    if ($fail20 -eq 0) { Write-Host 'PASS: rule 20 flags includes of Editor\Tools\Browser from Server / Runtime (3), spares the Editor layer and comments' }
    $fail += $fail20
    exit $(if ($fail -eq 0) { 0 } else { 1 })
}
finally {
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}
