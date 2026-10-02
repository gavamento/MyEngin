# Amazon GameLift Servers Server SDK for C++ (ビルド済み静的ライブラリ)

M81g で `Server.exe` (build\Server.vcxproj) だけが使う。Engine / Runtime / Editor / GameLogic は参照しない
(tools\check_rules.ps1 規則 13-c)。

| 項目 | 値 |
|---|---|
| 取得元 | https://github.com/amazon-gamelift/amazon-gamelift-servers-cpp-server-sdk |
| 版 | v5.6.0 (タグ) = コミット `7c2a5a7cae6616b1ca2f217aaceff36b06393c80` |
| ライセンス | Apache-2.0 (`LICENSE`)。同梱ライブラリの帰属は `NOTICE` |
| 置いたもの | `include\` (SDK の `gamelift-server-sdk\include` をそのまま) / `lib\Debug\` / `lib\Release\` (`aws-cpp-sdk-gamelift-server.lib`) |
| CRT | Debug = `/MTd`、Release = `/MT` (build\Common.props と同じ。LNK2038 なし) |
| 定義 | ライブラリも利用側も `GAMELIFT_USE_STD=1` と `WIN32` (公開クラスのレイアウトが変わるので必ず揃える) |
| 依存 | OpenSSL 3 (DLL。`external\openssl\`)。asio / websocketpp / rapidjson / spdlog / concurrentqueue はヘッダオンリーで .lib に取り込み済み (公開ヘッダには漏れない) |
| ビルド環境 | Visual Studio 2026 (v18) の v143 ツールセット / CMake 4.3.1 (VS 同梱)。生成された .lib はツールセット v143 (14.44) |

## なぜ手順が特殊か

1. SDK の CMake は既定で `/MD`・`/MDd`。MyEngine は全プロジェクト `/MT`・`/MTd` で、そのままリンクすると LNK2038 になる。
   最上位の CMakeLists.txt は ExternalProject 経由で下位へ引数を渡さないため、**`gamelift-server-sdk\` を直接 configure**
   して `CMAKE_MSVC_RUNTIME_LIBRARY` を渡す。SDK の CMake が `cmake_minimum_required(VERSION 3.1)` なので、
   `CMAKE_POLICY_DEFAULT_CMP0091=NEW` を足さないと `CMAKE_MSVC_RUNTIME_LIBRARY` が無視される。
2. 既定のまま (`/Zi` + 非 unity) だと .lib が Release 281MB / Debug 347MB になる。原因は型情報ではなく、
   websocketpp / asio のテンプレート由来の長い装飾名がオブジェクトごとに重複して入ること。次の 2 つで
   Release 53MB / Debug 72MB まで下がる:
   - `patches\0001-no-debug-info.patch` で `/Zi` と `/DEBUG` を外す (これだけでは 145MB / 254MB)。
   - `-DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=0` で全ソースを 1 翻訳単位にまとめる (コンパイルは通る)。
3. CMake 4 は 3.5 未満の `cmake_minimum_required` を拒否する (rapidjson / websocketpp の ExternalProject)。
   環境変数 `CMAKE_POLICY_VERSION_MINIMUM=3.5` が要る (コマンドライン引数では下位へ届かない)。

## 再現手順

`build_sdk.ps1` が下を全部行う (`pwsh -File external\gamelift-server-sdk\build_sdk.ps1 -OpenSslRoot <dir>`)。
`<dir>` は OpenSSL 3 の dev 一式 (`include\openssl`、`lib\libcrypto.lib`) があるフォルダ (`external\openssl\` でよい)。

1. SDK を v5.6.0 で shallow clone し、`patches\0001-no-debug-info.patch` を `git apply` する。
2. 最上位を configure し、ヘッダオンリーの依存 (asio / concurrentqueue / rapidjson / spdlog / websocketpp) だけを
   ビルドして `prefix\include` へ取得する (版は SDK の `cmake\External_*.cmake` の固定タグ:
   asio-1-20-0 / concurrentqueue v1.0.4 / rapidjson v1.1.0 / spdlog v1.14.0 / websocketpp 0.8.2)。
3. `gamelift-server-sdk\` を直接 configure (引数は下記) して Debug / Release をビルドする。
4. `lib\<構成>\aws-cpp-sdk-gamelift-server.lib` にコピーする。

```
cmake -S gamelift-server-sdk -B build_mt -G "Visual Studio 18 2026" -A x64 -T v143
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>"
  "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /utf-8"
  "-DCMAKE_CXX_FLAGS_DEBUG=/Ob0 /Od /RTC1"  "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG"
  -DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=0
  -DPREFIX_INCLUDE_DIR=<最上位ビルドの prefix\include>
  -DGAMELIFT_USE_STD=ON -DBUILD_SHARED_LIBS=OFF -DOPENSSL_ROOT_DIR=<OpenSSL のルート>
  -DCLANG_FORMAT_EXECUTABLE_PATH=
```

`/utf-8` は SDK のソースが BOM 無し UTF-8 で、日本語ロケール (CP932) だと C4819 になるため。

## 使い方 (参考)

`build\Server.vcxproj` が `include\` を外部インクルードとして追加し、`lib\$(Configuration)\...lib` をリンクし、
`external\openssl\redist\*.dll` をビルド後に出力先へコピーする。SDK の TLS は証明書を検証しない設定のままで
(SDK のソース `WebSocketppClientWrapper::OnTlsInit`)、OpenSSL の CA ストアは要らない。
