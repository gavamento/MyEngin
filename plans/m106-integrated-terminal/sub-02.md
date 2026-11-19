# sub-02: Win32 非依存のターミナルコア

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること
spec 4.1・4.2。`/home/user/MyEngin/src/Editor/Tools/Terminal/` に Win32 / ImGui 非依存 (標準 C++20 + libvterm のみ) の部品を置く:
- 画面モデル: libvterm の VTerm/VTermScreen を所有し、バイト投入 → セル (コードポイント列・幅・前景/背景 RGB 解決済み・属性) の読み出し、カーソル位置/表示、代替画面中か、スクロールバック (上限付きリング、sb_pushline/popline)、リサイズ、bracketed paste 状態、libvterm が出す応答バイト (DA 等) の取り出しを提供。
- キー→VT 変換: エンジン非依存のキー列挙 + 修飾 → libvterm keyboard API 経由でバイト列。貼付文字列の包み (2004 有効時)、改行正規化 (CRLF→CR)。
- シェル解決: 設定文字列と「存在確認関数」(注入) から実行パス+引数を返す純関数。順序 pwsh→powershell→cmd、明示指定が無効ならエラー。
- 環境ブロック組立: 親環境のキー値列 + 追加変数 + PATH 前置 → Windows 環境ブロック (UTF-16 化は呼び出し側、ここでは UTF-8 のソート済み列と二重NUL 規則の検証可能な形) を返す純関数。大文字小文字無視の重複処理、既存変数非上書き。
- SelfTest: `TerminalCoreSelfTest.{h,cpp}` を `*SelfTest` パターンで追加し `/home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp:144` の kSuites に Fast で登録。spec 5 の 2・3・4 を網羅 (空入力、分割された UTF-8/エスケープ列の境界、大量出力、最小サイズ、スクロールバック上限)。
- SelfTest 本体を Linux の g++ でも回せるよう、テスト関数は SelfTest 基盤に依存しない形で書くか、最小のスタブで g++ ビルドできるようにする (方法は coder 裁量、SELF_EVAL に書く)。

## やらないこと (このサブでは)
- プロセス起動、描画、設定保存。

## 触る場所 (planner の見立て)
- 新規 `/home/user/MyEngin/src/Editor/Tools/Terminal/` (名前例: TerminalScreen, TerminalInput, TerminalShellResolve, TerminalEnvironment)。`tools\gen_project_files.ps1` 再生成相当の vcxproj/filters 更新 (Linux では手で同等の差分を入れ、整合を検査)。
- 手本: `/home/user/MyEngin/src/Editor/Tools/Browser/BrowserCatalog.cpp` (Win32 分離) と BrowserEmbedSelfTest。

## 受け入れ条件 (このサブ)
1. spec 5-2/3/4 を SelfTest で網羅し、Linux g++ -std=c++20 -Wall -Wextra で警告 0、全件成功。
2. 新ファイルが Win32 ヘッダ・ImGui を include しない (grep)。
3. vcxproj / filters に新ファイルが登録され整形式。
4. Windows (要確認): ビルド成功、`Editor.exe --selftest` Fast 層で新スイート成功・60 秒以内。

## 検証コマンド
- Linux: g++ ビルド+実行、grep、XML 検査。
- Windows: `MYE_SELFTEST_ONLY=<新スイート名>` で `bin\x64\Debug\Editor.exe --selftest`。
