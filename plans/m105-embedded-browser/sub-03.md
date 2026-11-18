# sub-03: 後始末と障害時の表示、取り込み、文書

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること

spec 4.1.7〜4.1.9 の UI 側と仕上げ、文書。

1. 障害・終了の表示 (spec 4.3 の 1 行の結果): Closed (ブラウザ側で閉じた)、Lost (落ちた)、TimedOut (15 秒で見つからない)、Failed (理由: DPI 違い・権限・SetParent 失敗・exe が無い)。どれからも「起動」「既存の窓を取り込む」で再開できる。
2. 応答なし (hung 印): ツールバーに「応答なし」を出す。印が立っている間は位置合わせ・切り離しをしない (spec 4.4)。戻ったら消す。
3. 既存の窓を取り込む: 選んだブラウザの今ある窓を「タイトル (PID)」で一覧するポップアップ → 選んで AttachExisting。Idle と TimedOut から使える。一覧は押したときだけ列挙する (毎フレームしない)。
4. 後始末の確定: 「切り離す」・パネルの ×・エディタの正常終了 (OnShutdown) で Detach、「閉じる」で `PostMessage(WM_CLOSE)`。応答なしのときのエディタ終了は Detach せずにログだけ残す。
5. 文書:
   - `docs/adr/ADR-040-embedded-browser.md`: 方式 (SetParent 埋め込み)、却下案 (WebView2 / CEF / CDP / 専用プロファイル / `--app` / SetWindowRgn / 追従トップレベル窓)、入力キュー結合の既知の制限、sub-01 の観測結果 O1〜O9 (特にクラッシュ時の挙動 O7、`WS_CLIPCHILDREN` の要否 O6)、出典 URL (spec 2. の D2 / D12 / D13)。
   - `engine_spec.md` §9.3 *Browser window (M105)*: 使い方、保存先 (editor_global.json のキー)、制限 (全画面非対応・新しい窓は外に出る・エディタの長いフレーム中はブラウザ入力も止まる・Play 中のキー)。§9.2 の書き方に合わせて英語。
   - `docs/test_checklists.md` に M105 節 (spec 5-7〜5-11 の手動確認手順)。
6. 最終確認: `--selftest-full`、check_rules、`replay_verify` (1 回。sim 非接触の確認として結果を記録)。

## やらないこと (このサブでは)

- 静止画のプレースホルダー (後回し)、複数パネル、自動起動
- D3 の代案

## 触る場所 (planner の見立て)

- `src/Editor/Windows/.../BrowserWindow.*`、`src/Editor/Tools/Browser/BrowserHost.*`
- `src/Editor/App/EditorApp.cpp` (OnShutdown、フック)
- `src/Engine/Core/Localization/LocalizationTable.inl`
- `docs/adr/ADR-040-embedded-browser.md` (新規)、`engine_spec.md`、`docs/test_checklists.md`

## 受け入れ条件 (このサブ)

1. spec 5-11: 切り離す・パネル ×・エディタの正常終了で窓が元のスタイルの普通の窓として残りタブが失われない (Chrome で手動 + キャプチャ)。閉じるで窓が閉じる。ブラウザ側で閉じる → Closed 表示、`taskkill /F` → Lost 表示、エディタは動き続ける (手動 + `browserembedproc` の遷移が PASS)。
2. 応答なし: `browserembedproc` の hang 版で印が立ち、1 フレーム 50 ms 以内 (sub-01 の条件が維持されている)。パネルに「応答なし」が出る (ダミー窓を手で埋め込む手順か probe で確認。手段は coder が選んでよい)。
3. 既存の窓を取り込む: Chrome を先に開いておき、一覧から選んで埋め込める (手動 + キャプチャ)。
4. spec 5-15: `Editor.exe --selftest-full` 全体 PASS (Debug)。`tools\replay_verify.bat` を 1 回回して結果を記録 (sim 非接触なので FAIL なら既存の FAIL か切り分けて報告)。
5. spec 5-16: ADR-040・engine_spec §9.3・test_checklists M105 節。ADR に O1〜O9 の観測が写っている。
6. `tools\check_rules.ps1` 0 error。

## 検証コマンド

- Debug|x64 ビルド
- `bin\x64\Debug\Editor.exe --selftest-full`
- `tools\check_rules.ps1`
- `tools\replay_verify.bat` (`MYE_REPLAY_JOBS=3`)
- 実ブラウザの手動確認 (test_checklists の M105 節を実際に通す)

## 実装メモ (coder が追記)

## フィードバック履歴
