# selftest が CRT のダイアログで止まるのを検知できない

- 記録: 2026-10-05 (M85 sub-05 の作業中にユーザーが発見)
- 状態: 対応済み (2026-10-05、M85 sub-05 の後に別コミット)

## 対応結果

- `SuppressCrtDialogs()` を `CrashHandler` に切り出し (`_set_abort_behavior`、CRT 報告先を stderr、
  `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)`、不正パラメータは stderr に 1 行出して exit 3)。
  `InstallCrashHandler` も中でこれを呼び、その後でバンドルを書く本物のハンドラに置き換える。
- `Editor.exe` の `wWinMain` の先頭 (`AttachParentConsole` の直後) で呼ぶ。`--selftest` ほか全ヘッドレス経路が対象。
- デバッガが付いているときの報告先はダイアログのまま (従来と同じ条件)。
- 検証:
  - 一時プローブ (selftest の先頭で `std::vector` の範囲外添字) で Debug `Editor.exe --selftest` を実行 →
    ダイアログなし、stderr に `vector(1931) : Assertion failed: vector subscript out of range`、
    終了コード 0xC0000409 (STL は fast fail で落ちる) で返った。プローブは削除済み。
  - プローブなしの Debug / Release `Editor.exe --selftest` は exit 0。
  - Debug `Runtime.exe --crash-test invalidparam` は従来どおりバンドルを書いて exit 3。
  - Debug / Release ビルド警告 0、`tools\check_rules.ps1` 0 件。
- 範囲外: Runtime / Server は従来どおり (Runtime は EngineLoop、Server は main の先頭で InstallCrashHandler)。
  bat 側のタイムアウトは足していない (ダイアログが出なくなったので、止まる経路はデッドロック等に限られる)。

## 起きたこと

`bin\x64\Debug\Editor.exe` で次のダイアログが 2 つ出て、プロセスが止まった。
どの実行 (どの selftest) で出たかは、ダイアログが閉じられたため特定できていない。
M85 sub-05 の coder による調査 (推定・未確認):
- 観測した事実: sub-05 の作業途中の版で、BT 節の書式が新旧食い違い、`NavAgentSelfTest.cpp` の
  「空の NavSystem へ復元できる」(816 行付近。同じ形が 1338 / 1952 / 2213 / 3064) が RestoreSimSnapshot 失敗で FAIL していた。
- 推定: その失敗の後でテスト側が vector を添字アクセスし、範囲外になった。アサート自体は再現できず、行は未特定。
- sub-05 の修正後は再現しない。テスト側の「失敗後も添字アクセスする」頑丈さの問題は残っている可能性がある。

```
Debug Assertion Failed!
File: ...\MSVC\14.44.35207\include\vector  Line: 1941
Expression: vector subscript out of range
```

## 問題

ダイアログが出ている間、プロセスは終了コードを返さない。selftest を回す側 (人・エージェント・bat) は
「失敗した」とも「終わった」とも分からず、待ち続けるか、ダイアログが閉じられた後の結果だけを見る。
**Debug の selftest の FAIL / 停止を検知する経路が無い。**

## 原因 (確認済み)

- CRT のアサート・エラーの報告先を stderr へ落とす設定は `InstallCrashHandler` の中にある
  (`src\Engine\Platform\CrashHandler.cpp:632-646`、`_set_abort_behavior` と `_CrtSetReportMode`)。
- `InstallCrashHandler` を呼ぶのは `EngineLoop.cpp:552` (エディタ / Runtime の通常起動) と
  `ServerMain.cpp:119` だけ。
- `Editor.exe --selftest` は `EditorMain.cpp:504` から EngineLoop を経ずに走るので、
  Debug CRT の既定 (モーダルダイアログ) のまま。`--modal-bake` などほかのヘッドレス早期 return も同じ。

## 直し方 (案)

1. CrashHandler の「CRT のダイアログを出さない」部分を、クラッシュバンドルと切り離した関数に分ける。
2. その関数を Editor のヘッドレス経路 (`--selftest` ほか、`EditorMain.cpp` の早期 return 群) の先頭で呼ぶ。
   - `_CrtSetReportMode` を stderr にし、`_CrtSetReportHook` でアサートの内容 (ファイル・行・式) をログへ出す。
   - アサートが 1 回でも出たら selftest を失敗扱い (終了コード ≠ 0) にする。
   - `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)` で WER のダイアログも出さない。
3. デバッガが付いているときは今までどおりダイアログ / ブレークにする (`CrashHandler.cpp:641` と同じ条件)。
4. 検証: 一時的にわざと範囲外の添字を踏むテストで、ダイアログが出ずに終了コード ≠ 0 で返り、
   ログにファイル・行が出ることを確かめてから戻す。
