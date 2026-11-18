# sub-01: 候補の検出と埋め込みの中核 (実現性の試作を兼ねる)

- 依存: なし
- 状態: OK (round 2。コミット待ち)
- 往復: 2

## やること

spec 4.1.1〜4.1.4、4.1.7 (遷移関数)、4.1.5 (見せる条件の純関数のみ)、4.4、4.5 を実装し、**SetParent 方式がこの PC の実ブラウザで成り立つか**を最初に確かめる。
このサブの一番の目的は「荷重のかかる未知」(spec 7. の R1〜R3、R5、R7) を潰すこと。UI (ドック窓) は作らない。

1. `BrowserCatalog`: StartMenuInternet の読み出し (Win32 部分) と、読み出した生データ → 候補一覧の**純関数** (重複除去・並び・除外・exe 抽出・既定の印・引数の系統) を分ける。
2. `BrowserHost`: Launch (snapshot → CreateProcessW)、窓特定の純関数 + ポーリング、Attach / Detach、毎フレームの観測と遷移関数、表示/矩形の適用 (`ShowWindowAsync` / 矩形が変わったときだけ `SetWindowPos`、1 秒 10 回上限)、Close (`PostMessage(WM_CLOSE)`)、AttachExisting(hwnd)、候補窓の列挙 (取り込み用)。HWND は `void*` で受け渡し、`Windows.h` はヘッダに出さない。
3. 見せる条件 (spec 4.1.5 の 1〜6) を「パネル矩形 + 手前の窓の矩形配列 + フラグ → bool」の純関数として用意する (ImGui からの収集は sub-02)。
4. 検証フラグ: `--browser-list`、`--browser-dummy-window [--browser-dummy-hang-after-ms N]` (どちらもエンジン初期化前の早期分岐。`src/Editor/App/EditorMain.cpp` の `--img-diff` 等と同じ流儀)、`--browser-embed-probe <id> [--browser-probe-url URL] [--browser-probe-hold-ms N]` (通常起動のエディタで、メイン窓のクライアント中央の固定矩形 (例: 左上 (100,100) から 1000x700) へ埋め込む。パネルは sub-02 なので固定矩形でよい)。
5. selftest: `browserembed` (Fast)、`browserembedproc` (Full)。`src/Editor/SelfTest/SelfTestSuites.cpp` の `kSuites` に登録。
6. check_rules 規則 20 (Engine / Runtime / GameLogic / Shared / Server から `Editor\Tools\Browser\` を include しない) と `check_rules_selftest.ps1` の違反フィクスチャ (規則 19 の作りを真似る)。
7. **実ブラウザでの観測** (撤退判定の材料)。下の「観測すること」を全部 SELF_EVAL の検証欄に書く。

## やらないこと (このサブでは)

- ドック窓 (`BrowserWindow`)、メニュー項目、ローカライズ文字列 (ログは英語でよい)、設定の保存 (sub-02)
- フォーカスを戻すメッセージハンドラ (sub-02)
- パネル上の障害表示・取り込みの UI・ADR / engine_spec / test_checklists (sub-03)。ただし観測結果は SELF_EVAL に残す (sub-03 が ADR に写す)

## 触る場所 (planner の見立て)

- 新規: `src/Editor/Tools/Browser/BrowserCatalog.h/.cpp`、`BrowserHost.h/.cpp`、`BrowserEmbedSelfTest.h/.cpp` (名前は coder が既存の流儀で決めてよい)。追加後に `tools\gen_project_files.ps1`。
- `src/Editor/App/EditorMain.cpp`: 早期分岐 2 つと `--browser-embed-probe` の受け取り (EditorApp のメンバへ渡す。`openTimeline` 等の既存の検証フラグと同じ流儀)。
- `src/Editor/App/EditorApp.h/.cpp`: probe のときだけ BrowserHost を持って毎フレーム更新し、OnShutdown で Detach する最小の配線 (sub-02 で窓に置き換わる前提で薄く)。
- `src/Editor/SelfTest/SelfTestSuites.cpp`: 2 スイートの登録。
- `tools/check_rules.ps1`、`tools/check_rules_selftest.ps1` (+ フィクスチャ): 規則 20。
- メイン窓の HWND は `ctx.window->Hwnd()` (`src/Engine/Platform/Win32Window.h`)。Engine 層は変更しない想定 (`WS_CLIPCHILDREN` が要るなら Editor から `SetWindowLongPtrW` で足す)。
- 子プロセス起動の既存部品 `src/Editor/App/ChildProcess.*` は `CREATE_NO_WINDOW` + ログ転送用なのでブラウザには使えない。BrowserHost 内で `CreateProcessW` を直接呼ぶ (ハンドルはすぐ閉じる)。

## 観測すること (実ブラウザ。SELF_EVAL の検証欄へ)

この PC の 5 候補 (Brave / Google Chrome / Microsoft Edge / Opera GX Stable / Vivaldi) それぞれで `--browser-embed-probe` を回し、保持中にデスクトップキャプチャを撮る (PowerShell の `System.Drawing.Graphics.CopyFromScreen` 等。画像はスクラッチ領域に置き、パスを書く)。
**エディタはリポジトリの外を作業ディレクトリにして起動する** (imgui.ini をリポジトリに書かないため)。

- O1 特定にかかった時間、`GetParent`、スタイル、矩形一致、DPI 認識の一致
- O2 子窓の中身が描かれるか (キャプチャで判断)。タブストリップ・キャプションボタンがどう見えるか
- O3 そのブラウザが**既に起動中**のときも特定できるか (Chrome か Edge で必ず。D5)
- O4 埋め込み中にアドレスバーをクリックして文字を打てるか (手動で 1 回。IME も)
- O5 Detach 後に普通の窓として戻り、タブが残っているか
- O6 `WS_CLIPCHILDREN` の要否 (付けない状態でちらつき・欠けが出るか)
- O7 エディタを `taskkill /F` したとき、埋め込んでいた窓が道連れになるか (ダミー窓と Chrome の両方)
- O8 子窓がフォーカス中、エディタに `WM_INPUT` (生マウス) が届くか (ログ 1 行でよい)
- O9 Opera GX のランチャーが `--new-window` を本体へ渡すか

## 撤退条件

次のどれかに当たったら、**ドック窓 (sub-02) に進む前に止める**。coder は実装を広げず、観測結果を SELF_EVAL の「不安・質問」に書いて返す。planner は D3 の代案 (追従する独立トップレベル窓) への切替を `[ユーザーに聞ける]` で司会に上げる。

- E1 Chrome と Edge の**両方**で、埋め込んだ子窓の中身が描かれない、またはキー入力 (O4) が効かない
- E2 Attach / Detach / 毎フレームの適用のどれかでエディタが 1 秒以上固まることが再現する (ブラウザが正常なときに)
- E3 Detach 後にブラウザの窓が壊れる (描かれない・閉じられない・タブが消える) ことが Chrome と Edge の両方で起きる

5 候補のうち一部 (例: Opera GX) だけが失敗するのは撤退ではない。失敗した候補と理由を書き、spec 7. に積む。

## 受け入れ条件 (このサブ)

1. spec 5-1: `browserembed` selftest で候補の解析が 4.1.1 どおり (重複・引用符/引数/`%1`・IE 除外・存在しない exe・日本語パス・既定の印・並び)。
2. spec 5-2: `Editor.exe --browser-list` がこの PC で 5 件を出し IE を出さない (ログを SELF_EVAL に貼る)。
3. spec 5-3: `browserembed` selftest で窓特定の純関数が 4.1.3 どおり (Opera GX 型の版ディレクトリ、除外条件、Z 順先頭、別ブラウザを拾わない)。
4. spec 5-4 のうち純関数部分: 状態機械の全遷移 (Closed / Lost の区別、TimedOut、Failed、hung 印の上げ下げ) と見せる条件 1〜6 の判定。
5. spec 5-5: `browserembedproc` (Full) がダミー窓プロセスで全部 PASS (Attach / 矩形 / Detach の復元 / Lost・Closed を 1 秒以内 / 応答なしの印と 1 フレーム 50 ms 以内)。
6. spec 5-6: Brave / Chrome / Edge / Vivaldi で `--browser-embed-probe` exit 0 とキャプチャ。Opera GX はベストエフォート (失敗時は Failed + 理由、窓が元のスタイルで画面に残る)。撤退条件 E2 / E3 に当たらないこと。O1〜O9 の観測を書く (O4 / O8 は人の手の確認に回した。E1 の判定は司会経由でユーザーの確認待ち。sub-01 のコミットはこの確認を待たない)。
9. (round 1 で追加) 親の喪失: Attach 直後と毎フレームに `GetParent(child) != main` を検出し、元のスタイルへ戻して Failed (spec 4.1.4 / 4.1.7)。`browserembed` の遷移と、ダミー窓プロセスが自分で親を外すケースの `browserembedproc` で確認。
7. spec 5-14: check_rules 規則 20 + セルフテストのフィクスチャ。`tools\check_rules.ps1` 0 error、`tools\check_rules_selftest.ps1` PASS。
8. 既存: `Editor.exe --selftest` (Fast) が PASS、Fast 層の所要時間を目に見えて伸ばさない (`browserembed` は 1 秒以内)。

## 検証コマンド

- Debug|x64 ビルド (MSBuild / VS)
- `tools\gen_project_files.ps1` (ファイル追加後)
- `$env:MYE_SELFTEST_ONLY='browserembed'; bin\x64\Debug\Editor.exe --selftest`
- `$env:MYE_SELFTEST_ONLY='browserembedproc'; bin\x64\Debug\Editor.exe --selftest`
- `bin\x64\Debug\Editor.exe --selftest`
- `bin\x64\Debug\Editor.exe --browser-list`
- `bin\x64\Debug\Editor.exe --browser-embed-probe "<id>" --browser-probe-hold-ms 8000` (5 候補。保持中にデスクトップキャプチャ)
- `tools\check_rules.ps1`、`tools\check_rules_selftest.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1) の要約。全文は司会への返答に添付。

- 実装: src/Editor/Tools/Browser/ に BrowserCatalog / BrowserHost / BrowserProbe / BrowserEmbedSelfTest を新設。EditorMain.cpp に --browser-list / --browser-dummy-window / --browser-embed-probe を追加。EditorApp はプローブ時だけ BrowserEmbedProbe を持つ。check_rules 規則 20 とフィクスチャを追加。
- 検証: browserembed (Fast) PASS 2 ms / browserembedproc (Full) PASS 10.3 s / --selftest 113 suites 0 failed / check_rules 0 error / check_rules_selftest PASS / --browser-list 5 件 (IE なし)。
- 実ブラウザ: 5 候補で --browser-embed-probe を実行。Brave / Chrome / Edge / Vivaldi = PASS (描画をキャプチャで確認)。Opera GX = 1 回 PASS の後 3 回連続 FAIL (親がデスクトップへ戻される)。
- 未確認 (重要): O4 (埋め込み中のキー入力)。SendInput の自動注入は単体の Chrome でも効かず検証手段として成立しなかった。手動確認が必要。E1 は判定保留。
- round 2: 親の喪失 (GetParent != main) を Attach 直後と毎フレーム (150 ms 継続で確定) に検出し、元のスタイルへ戻して Failed。browserembed / browserembedproc (ダミーが自分で親を外す) に追加、Opera GX 実機で Failed + 窓が普通の窓で残ることをログで確認。--browser-probe-input と SendProbeInput を削除。
- O7: エディタを強制終了すると埋め込み中の窓は道連れで破棄される (ダミーは窓だけ消えてプロセスは残り、Chrome は最後の窓なのでプロセスごと終了)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner, 2026-11-18)。must 1 件 (親の喪失の検出と復元)、should 2 件 (`--browser-probe-input` / `SendProbeInput` の削除、`SetParent` 失敗判定の `SetLastError(0)`)。仕様変更: spec 4.1.4 Detach 順序 (coder の逸脱を採用)、4.1.7 Closed/Lost 判定 (coder の追加を採用)、親の喪失、5-6 の Opera GX をベストエフォートへ。O4 / O8 は人の手の確認に回し、sub-02 の着手条件にした
- round 2: VERDICT OK (planner, 2026-11-18)。must #1 解消 (Attach 直後と毎フレームの親の喪失、RestoreTopLevel、ダミーの unparent ケース、Opera GX 実機で Failed + 普通の窓に戻ることをログで確認)、should #2 解消 (入力注入の削除)、#3 は round 1 から充足。should 1 件 (Brave / Chrome / Edge / Vivaldi の probe を round 2 のコードで回し直していない) は sub-02 の受け入れ条件へ移した。仕様変更: spec 4.1.7 に 150 ms の確定と Update の呼び方、7. に R12
