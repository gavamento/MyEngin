# GameLift Server SDK 5.x を /MT・/MTd の静的ライブラリとしてビルドして external\gamelift-server-sdk\lib へ置く。
# 理由と引数の意味は同じフォルダの BUILD.md。
#   pwsh -File external\gamelift-server-sdk\build_sdk.ps1 -OpenSslRoot external\openssl
param(
    # OpenSSL 3 のルート (include\openssl と lib\libcrypto.lib がある場所)
    [Parameter(Mandatory = $true)][string]$OpenSslRoot,
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'mye_gamelift_sdk_build'),
    [string]$Tag = 'v5.6.0',
    # 出力先 (既定 = このフォルダの lib)。既存のライブラリを上書きしたくないときに変える
    [string]$OutDir = (Join-Path $PSScriptRoot 'lib'),
    [string]$Generator = 'Visual Studio 18 2026',
    [string]$CMake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$openssl = (Resolve-Path $OpenSslRoot).Path -replace '\\', '/'
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$src = Join-Path $WorkDir 'sdk'
$top = Join-Path $WorkDir 'top'
$mt = Join-Path $WorkDir 'mt'
# CMake 4 は 3.5 未満の cmake_minimum_required を拒否する。環境変数でないと ExternalProject の下位へ届かない
$env:CMAKE_POLICY_VERSION_MINIMUM = '3.5'

if (-not (Test-Path $src)) {
    git clone --depth 1 --branch $Tag https://github.com/amazon-gamelift/amazon-gamelift-servers-cpp-server-sdk $src
    git -C $src apply (Join-Path $here 'patches\0001-no-debug-info.patch')
}
Write-Host "SDK commit: $(git -C $src rev-parse HEAD)"

# ヘッダオンリーの依存を prefix\include へ取得する
& $CMake -S $src -B $top -G $Generator -A x64 -T v143 '-DBUILD_SHARED_LIBS=0' '-DGAMELIFT_USE_STD=1' '-DRUN_UNIT_TESTS=0' '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
foreach ($t in 'asio', 'concurrentqueue', 'rapidjson', 'spdlog', 'websocketpp') {
    & $CMake --build $top --config Release --target $t
}

# SDK 本体を直接 configure (CRT を /MT・/MTd にするため)
$flags = @(
    '-S', (Join-Path $src 'gamelift-server-sdk'), '-B', $mt, '-G', $Generator, '-A', 'x64', '-T', 'v143',
    '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>',
    '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /utf-8',
    '-DCMAKE_CXX_FLAGS_DEBUG=/Ob0 /Od /RTC1',
    '-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG',
    '-DCMAKE_UNITY_BUILD=ON', '-DCMAKE_UNITY_BUILD_BATCH_SIZE=0',
    ('-DPREFIX_INCLUDE_DIR=' + ((Join-Path $top 'prefix\include') -replace '\\', '/')),
    '-DGAMELIFT_USE_STD=ON', '-DBUILD_SHARED_LIBS=OFF',
    "-DOPENSSL_ROOT_DIR=$openssl",
    '-DCLANG_FORMAT_EXECUTABLE_PATH='
)
& $CMake @flags
foreach ($c in 'Debug', 'Release') {
    & $CMake --build $mt --config $c -- /m /v:minimal /nologo
    $dst = Join-Path $OutDir $c
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item (Join-Path $mt "$c\aws-cpp-sdk-gamelift-server.lib") $dst -Force
}
Write-Host "done: $OutDir\Debug and $OutDir\Release updated"
