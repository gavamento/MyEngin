# check_rules_selftest.ps1 — check_rules.ps1 の規則自身の自己検査
# 実行: pwsh -File tools\check_rules_selftest.ps1  (期待どおりでなければ exit 1)
# 違反を仕込んだ一時ファイルを -SourceRoot で読ませ、規則が実際に error を出す (当たらない正規表現のまま
# 0 件を返し続けない) ことと、正当なコードに誤検出しないことを確かめる。今は規則 18 (M96) だけ
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
    $out = & pwsh -NoProfile -File $checker -SourceRoot $root 2>&1 | Out-String
    $rule18 = @($out -split "`n" | Where-Object { $_ -match '\[rule 18\]' })
    $fail = 0
    $badLines = @($rule18 | Where-Object { $_ -match 'Bad\.cpp' })
    if ($badLines.Count -ne 5) { Write-Host "FAIL: expected 5 rule 18 errors in Bad.cpp, got $($badLines.Count)"; $fail++ }
    if (@($rule18 | Where-Object { $_ -match 'Good\.cpp' }).Count -ne 0) { Write-Host 'FAIL: Good.cpp must not be flagged'; $fail++ }
    if (@($rule18 | Where-Object { $_ -match 'SelfTest\.cpp' }).Count -ne 0) { Write-Host 'FAIL: SelfTest files are on the allow list'; $fail++ }
    if ($fail -eq 0) { Write-Host 'PASS: rule 18 flags the violations (5), spares clean code, comments and selftest files' }
    exit $(if ($fail -eq 0) { 0 } else { 1 })
}
finally {
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}
