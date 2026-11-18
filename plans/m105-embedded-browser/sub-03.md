# sub-03: 後始末と障害時の表示、取り込み、文書

- 依存: sub-02
- 状態: OK (round 1。コミット待ち)
- 往復: 1

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

SELF_EVAL: sub-03 (round 1) の要約。全文は司会への返答に添付。

- 実装: BrowserHost に `BrowserFailReason` (Failed の理由の種別) を追加し、BrowserWindow が ja/en の文言を選ぶ。結果の 1 行 (Closed / Lost / TimedOut / Failed) を spec 4.3 の文言に仕上げ、TimedOut から「既存の窓を取り込む」へ案内。ツールバーに「応答なし」(hung 中は「切り離す」を無効化)、「既存の窓を取り込む...」ボタン + 一覧ポップアップ (押したときだけ列挙)。応答なしのままの OnShutdown は Detach せずログ。probe に `dummy-hang` (応答なし表示の確認用)。ADR-040、engine_spec §9.3、test_checklists M105 節。
- 検証: browserembed / browserembedproc PASS (7b: Lost / Failed からの再開と AttachExisting、FailReason を追加)。`--selftest-full` 127 suites (リポジトリ外 cwd では part / fractureskin / skinbounds が実アセット不在で FAIL = cwd 要因、リポジトリルートで 3 件 PASS)。check_rules 0 error、check_rules_selftest PASS。replay_verify は bat のビルド手順が古い Editor.exe (PID 2412 / 38416 / 14332) の nethost.dll 掴みで失敗するため、Release を手動ビルドして bat の並列検証部分 (24 ジョブ) を同一コマンドで実行 → 全 PASS (583 s)。
- 実ブラウザ: Edge の probe は exit 0 (O5 切り離しの復元を確認)。Chrome は残存のプロファイル選択窓で TimedOut (R13、環境要因)。「応答なし」表示は `--browser-embed-probe dummy-hang` + `--screenshot` で撮影 (パネルに赤字の「応答なし」、「切り離す」は無効)。
- 未検証 (手動): 「既存の窓を取り込む」ポップアップのクリック操作、Chrome での Detach / 閉じる / taskkill の表示、エディタ正常終了時の Detach の実ブラウザ確認 (test_checklists M105 節の手順)。

## フィードバック履歴
- round 1: VERDICT OK (planner, 2026-11-19)。受け入れ 2 (応答なし: selftest + dummy-hang の目視)、5 (ADR-040 / engine_spec §9.3 / test_checklists M105)、6 (check_rules 0 error) は充足。受け入れ 4 は代替で充足と判定: `--selftest-full` は cwd=TEMP で 127 中 124 PASS + cwd 依存の 3 件をリポジトリルートで PASS、`replay_verify.bat` は残存ダミーの nethost.dll 掴みで bat の Debug ビルド手順が止まったため同じ 24 ジョブを同一引数で回して all PASS。bat 一発と full のルート一発は reviewer に回す (残存プロセスが閉じられた後)。受け入れ 1 / 3 の実機 (Chrome の後始末・取り込みのクリック) は人の手の確認待ち
