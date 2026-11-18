# sub-02: ドック可能な「ブラウザ」窓 (追従・airspace・フォーカス・設定)

- 依存: sub-01
- 状態: OK (round 2。コミット待ち)
- 往復: 2
- **着手条件 (sub-01 round 1 で追加)**: 人の手による O4 (埋め込んだ Chrome / Edge でのキー・IME 入力) が「打てる」と確認されていること。打てなければ撤退条件 E1 に当たるので着手せず planner へ戻す

## やること

spec 4.1.5 (ImGui からの収集と適用)、4.1.6、4.2、4.3 (Idle / Launching / Embedded の表示。障害の詳しい文言と取り込み一覧は sub-03)。

1. `BrowserWindow` (ドック可能な ImGui 窓。*Window > ブラウザ* で開く、既定は閉じている)。
   - Idle: ブラウザのコンボ (既定の印、末尾「その他 (exe を指定)…」) + 再検出、その他のときの exe 表示 + 参照…、開始 URL、起動。
   - Launching: 「起動中… (n 秒)」+ 中止。
   - Embedded: 1 行ツールバー (ブラウザ名、切り離す、閉じる) + 内容矩形を記録。
   - 窓クラスは `Windows.h` を持ち込まない。ファイルダイアログ・Win32 の操作は EditorApp 側のフック (`SourceControlHost` の `openExternalUrl` と同じ流儀の `std::function`) か BrowserHost 経由。
2. 見せる条件の収集: パネルの内容矩形、ImGui の表示順でパネルより手前の窓の矩形 (パネル自身とその子・ツールチップを除く)、`MovingWindow`、モーダルの有無、メイン窓の最小化。`imgui_internal.h` の読み出しはこの薄い層に閉じ、判定は sub-01 の純関数へ渡す。
3. 適用のタイミング: **OnImGui の末尾** (全窓の提出後) に BrowserHost へ渡す。パネルが描かれなかったフレーム (タブ非アクティブ・パネルが閉じている) は「見せない」。
4. フォーカス: `ctx.window->AddMsgHandler` でマウスボタン押下 (左・右・中・X) を見て、`GetFocus()` がメイン窓でなければ `SetFocus(main)` (メッセージは消費しない)。子窓を隠すとき・切り離す/閉じるを押したときも戻す。
5. 設定: `EditorGlobalSettings` に `browserId` / `browserCustomExe` / `browserStartUrl` (3 点セット)。変更したときだけ保存。selftest で往復と「キーの無い古いファイル」を確かめる (`browserembed` に足してよい)。
6. ローカライズ: 追加文字列を全部 `LocalizationTable.inl` に ja/en で。窓タイトルは `###Browser`。
7. sub-01 で入れた probe 専用の仮配線を、BrowserWindow 経由の正式な配線に置き換える (`--browser-embed-probe` は BrowserWindow を開いてそのパネル矩形へ埋め込むように変える)。
8. パネルの UI を撮るための検証フラグ (例 `--browser-window`: パネルを開くだけでブラウザは起動しない)。`--screenshot` と組み合わせて ja/en を撮る。

9. sub-01 の申し送り: ブラウザは最大化で起動する前提 (Attach が通常矩形と最大化を控え、Detach で戻す)。`BringWindowToTop(main)` はプローブ専用のまま、パネルには入れない。

## やらないこと (このサブでは)

- Closed / Lost / TimedOut / Failed の詳しい表示と再起動導線、既存の窓を取り込む UI、応答なし表示、エディタ終了時の Detach の仕上げ (sub-03。ただし sub-01 の OnShutdown の Detach は壊さない)
- ADR / engine_spec / test_checklists (sub-03)

## 触る場所 (planner の見立て)

- 新規: `src/Editor/Windows/Project/BrowserWindow.h/.cpp` (置き場は既存の窓の分類に合わせて coder が決めてよい。決めた理由を SELF_EVAL に)
- `src/Editor/App/EditorApp.h/.cpp`: メンバ、Window メニュー項目 (`src/Editor/App/EditorApp.cpp:1857` 付近の並び)、OnImGui 末尾での適用、メッセージハンドラ、フックの実装、`SetupDockLayout` (既定レイアウトに入れるかは任意。入れないなら浮動で開く)
- `src/Editor/App/EditorGlobalSettings.h/.cpp`
- `src/Engine/Core/Localization/LocalizationTable.inl`
- `src/Editor/App/EditorMain.cpp`: パネルを開く検証フラグ

## 受け入れ条件 (このサブ)

1. spec 5-4 (適用側): パネルの上にメニュー・コンボ・コンテキストメニュー・モーダル・浮動窓が重なる / ドッキングのドラッグ中 / タブ非アクティブ / パネル折り畳み で子窓が隠れ、解消で戻る。純関数の selftest は sub-01 のまま PASS、ImGui からの収集は手動 + デスクトップキャプチャ (隠れている絵と戻った絵を最低 1 組)。
2. spec 5-7: Chrome と Edge で IME・スクロール・クリック・拡張機能のポップアップ・右クリックメニュー・新しいタブ (手動。結果を SELF_EVAL に)。
3. spec 5-8: ドックの移動・リサイズ・タブ切替・最小化と復元・浮動化に追従 (手動 + キャプチャ)。
4. spec 5-10: ブラウザで打った後に Hierarchy をクリック → Ctrl+S でシーン保存。子窓が隠れたときフォーカスがエディタへ戻る。Play 中にブラウザで打ったキーでゲームが動かない。Play 中にブラウザ上でマウスを動かしたとき Game ビューのマウスルックが動くか (R4) を記録する (動いても sub-02 の不合格にはしない。spec 7. と ADR に記録)。
5. spec 5-12: 設定の往復 (selftest) と再起動後の復元 (手動)。
6. spec 5-13: `--screenshot` で ja/en のパネル (Idle) を撮り、文字が全部翻訳されている。check_rules 0 error。
7. `Editor.exe --selftest` (Fast) PASS。
8. (sub-01 round 2 から移管) sub-02 のコードで Brave / Google Chrome / Microsoft Edge / Vivaldi の `--browser-embed-probe` を回し直して exit 0 (親の喪失の検出を足した後の実ブラウザでの再確認。spec 5-6)。Opera GX は失敗したら Failed + 普通の窓に戻ることだけ確認。
9. (sub-01 round 2 の申し送り) パネルが描かれないフレーム (タブ非アクティブ・パネルを閉じた) でも `BrowserHost::Update` を毎フレーム呼ぶ。Failed / Closed / Lost / TimedOut から「起動」をもう一度押せる (Opera GX は成否が混ざる)。

## 検証コマンド

- Debug|x64 ビルド、`tools\gen_project_files.ps1`
- `bin\x64\Debug\Editor.exe --selftest`
- `$env:MYE_SELFTEST_ONLY='browserembed,browserembedproc'; bin\x64\Debug\Editor.exe --selftest`
- パネルの撮影: (リポジトリ外の作業ディレクトリで) `Editor.exe --project <...> --browser-window --screenshot <png>` を ja / en で
- 実ブラウザ: `--browser-embed-probe "Google Chrome"` / `"Microsoft Edge"` + 手動操作 + デスクトップキャプチャ
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-02 (round 1)
要約: BrowserWindow (src/Editor/Windows/Project/BrowserWindow.h/.cpp) を新設し EditorApp に配線。OnImGui でパネルを描き内容矩形を記録、OnImGui 末尾の ApplyToHost で imgui_internal から見せる条件 (MovingWindow / モーダル / 表示順で手前の窓) を集め、sub-01 の純関数 EvaluateBrowserVisibility → BrowserHost::Update へ毎フレーム渡す (パネル非表示でも呼ぶ)。設定 3 点 (editor_global.json)、ja/en 文字列、マウス押下でのフォーカス返し (BrowserHost::ReturnFocusToMain)、--browser-window、probe の BrowserWindow 経由化。
検証: browserembed (設定の往復 + 頭なし ImGui での airspace 収集 9 件を追加) PASS / browserembedproc PASS / --selftest (Fast) 113 suites 0 failed / check_rules 0 error / check_rules_selftest PASS / ja・en スクショ / 実ブラウザ probe は Brave・Edge・Vivaldi・Opera GX が exit 0、Chrome は初回 exit 0 (その後は残った Chrome のプロファイル選択窓が単一インスタンスで --new-window を吸収し TimedOut = 環境要因)。メニュー表示中は隠れ、閉じると戻る絵を Edge で撮影 (scratchpad の air_edge_2 / 3)。
未検証 (手動): IME・スクロール・拡張・右クリック・新タブ (受け入れ 2)、ドック移動・リサイズ・タブ切替・最小化・浮動化の追従 (3)、Hierarchy クリック後の Ctrl+S・Play 中のキー・マウスルック R4 (4)、再起動後の設定復元 (5)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner, 2026-11-19)。must 2 件: (1) ダミー窓プロセスの寿命上限と selftest の失敗経路での後始末 (残ったダミーが Editor.exe を掴みビルドの post-build copy を壊した)、(2) ToastCenter の通知を重なり判定から除外 (spec 4.1.5 条件 6 を変更)。仕様変更: 4.1.6 フォーカス返しの条件 (coder の逸脱を採用)、4.5 ダミーの寿命、7. R3 確認済み・R13 追加。[追加] の結果文言 4 つと浮動 880x620 の既定は採用。手動の受け入れ 2〜5 は人の手の確認として司会に依頼 (sub-02 のコミットはこれを待たない。不合格が出たら FIX で戻す)
- round 2: VERDICT OK (planner, 2026-11-19)。must #1 解消 (ダミー窓の寿命 `kBrowserDummyLifetimeMs` = 120 s の自己終了 + `browserembedproc` の `DummyReaper` (RAII、PID の実行ファイルが自分と同じものだけ終了) + 特定待ちを 1 ms にした一時変更で失敗経路でも新規の残留なしを実測)、must #2 解消 (ToastCenter が Begin した窓の ImGuiID を毎フレーム公開し、BrowserWindow が重なり判定から除外。名前の文字列に依存しない。対照と除外の 2 件をテストに追加)。coder は実装メモ節へ round 2 の写しを追記していないため、要点をこの履歴に残す。手動の受け入れ 2〜5 と R4 は司会経由でユーザー確認待ち (不合格なら FIX)。Q-7 (通知の扱い) は Notion で回答待ち・推奨 (除外) で仮決定
