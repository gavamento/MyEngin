# OpenSSL 3 (GameLift Server SDK の依存。DLL)

M81g。GameLift Server SDK 5.x は OpenSSL 3 を **DLL でしかリンクできない** (SDK の CMakeLists.txt の注記)。
`build\Server.vcxproj` が `lib\*.lib` をリンクし、ビルド後に `redist\*.dll` を出力先 (`bin\x64\<構成>\`) へコピーする。
Server.exe 以外は使わない。

| 項目 | 値 |
|---|---|
| 版 | OpenSSL 3.6.5 (https://github.com/openssl/openssl/archive/openssl-3.6.5.tar.gz) |
| ビルド | vcpkg (microsoft/vcpkg `fbb0f7bb200b07a9eb9081c7a3cf51d1aa1c51a1`、port `openssl`) の `x64-windows` (動的) |
| ライセンス | Apache-2.0 (`LICENSE.txt`) |
| 置いたもの | `include\openssl\` / `lib\libssl.lib`・`lib\libcrypto.lib` (import lib、Release) / `redist\libssl-3-x64.dll`・`redist\libcrypto-3-x64.dll` (Release) |

## 補足

- Debug 構成も Release 版の DLL / import lib をリンクする。OpenSSL の DLL は自前の CRT を持ち、CRT のオブジェクト
  (FILE* や確保したメモリ) を SDK と受け渡さない前提で、`/MTd` の Server.exe と混ぜている。確認済みなのは
  DLL がロードされ SDK が動くところまで (繋がらない接続先への再試行)。TLS の実接続は M81 sub-08 で初めて確かめる。
- `legacy.dll` (ossl-modules) は SDK が使わないので置かない。SDK の TLS は証明書検証を行わない設定なので
  OpenSSL の CA ストア / `OPENSSLDIR` も要らない。
- 再現: `git clone https://github.com/microsoft/vcpkg && .\bootstrap-vcpkg.bat` のあと
  `vcpkg install openssl:x64-windows` (Perl / NASM は vcpkg が取得する。約 13 分)。出力の
  `installed\x64-windows\{include,lib,bin}` から上の 3 種を取り出す。
