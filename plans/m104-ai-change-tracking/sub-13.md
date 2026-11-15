# sub-13: 未保存シーンへの外部変更の確認 (M104n)

- 依存: sub-25 (順序は sub-24 → sub-25 → sub-13 → sub-14。コード上の依存は sub-08 だけ) (Phase 2 の完了。sub-09〜12 とコード上の依存は無いが、Phase 3 の中で行う。spec §2 D13)
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: **着手可** (2026-11-15、sub-24 の VERDICT で planner が事実を書き足した)

このマイルストーンで唯一の Engine 層の変更で、唯一の既存挙動の変更 (ADR-039 決定 10、ユーザー判断 2026-11-13)。

## 着手前にユーザー確認が要る未決事項

司会が着手時に聞く (計画の未決事項 1)。

- 聞きたいこと: Play 中に、開いているシーンへ外部の変更が来たときの扱い。今は Play 中の World へそのまま適用され、Stop で消える。
- 推奨: Play 中は保留し、Stop の後で、未保存かどうかに従って適用するか確認を出す。
- 逆を選ぶと何が変わるか: 「今のまま (Play 中も適用)」ならこのサブは dirty の判定だけになり、要求 §16.5「Play 中の危険な変更適用を停止する」のうち外部変更の分は満たさない (復元の分は sub-15 のゲートで満たす)。「Play 中は捨てる」は、Stop 後にディスクとメモリが食い違ったまま気付けないので勧めない。

## 着手前の確認の扱い (2026-11-15: 推奨で裁定済み)

- **裁定 (推奨どおり)**: Play 中に来た外部の変更は保留し、Stop の後で、未保存かどうかに従って適用するか確認を出す。逆の答え (今のまま Play 中も適用する) が来たら、差し戻す範囲は Play 中の分岐と、その試験 (受け入れ条件 6) だけ。
- このサブは追跡サービスと無関係に働く (追跡が無効でも確認は出る)。sub-09〜12 とコード上の依存は無いので、司会は sub-09 の後ならいつ着手してもよい。

## 確認済みの事実 (2026-11-15 に planner が調べた。行番号はずれうる)

- `C:\HAL\MyEngin\src\Engine\Engine\HotReload\ReloadHub.cpp`: 開いているシーンの変更は 345 行目から `ReloadActiveScene(normPath)` (711 行目) へ行き、729 行目で `SceneSerializer::ApplyDiff(*scene_, root)` を確認なしで当てる。呼び手が渡す口の前例は `SetKernelHooks` (183 行目) と `SetActiveScenePath` (189 行目)。`Update()` はフェーズ 2 で毎フレーム呼ばれる (`ReloadHub.h` 109 行目)。一括適用 (`BeginBatch` / `EndBatch`) の間は別の経路 (203〜242 行目)。`EndBatch` の後に同じ内容の `ApplyDiff` がもう一度走ることがある、という既存のコメントがある (211〜217 行目) — 保留の実装がこの二重の適用と食い違わないことを確かめる。
- Editor 側: `EditorApp.cpp` 144 行目で `ctx.reloadHub->SetActiveScenePath(scenePath_)`。dirty の判定は `undo_.StateSerial() != savedStateSerial_` (343、395 行目で clean にしている)。Play の状態は `playMode_.State()` (`PlayState::Editing` かどうか。471、689 行目)。Play を止めると Play 前のスナップショットへ戻る (599 行目付近の `PrePlaySnapshot`)。
- 関係するスイート: `reloadhub` (Fast 層)。回帰テストはここへ足す。
- sub-23 以降、Editor は自分の保存を `NoteFileSaved` で 1 か所から知らせている。自分の保存で起きる監視イベントで確認が出ないこと (保存の直後は dirty でない) は、今までどおりの条件で成り立つ。

## やること

arch §8。

1. `ReloadHub` に「開いているシーンの外部変更を今適用してよいか」を問う口を足す (`SetKernelHooks` と同じ、呼び手が渡す形)。**未設定なら今までどおり適用する** (Runtime と既存テストは変わらない)。
2. 口が「保留」を返したら、`ReloadHub` はそのパスを保留として覚えるだけ。保留中に同じパスの変更がもう一度来たら、保留は 1 つのまま (最新のディスクの内容を適用対象にする)。
3. Editor: dirty (`undo_.StateSerial() != savedStateSerial_`) のとき保留を返し、「外部で変更された。読み込む / 自分の編集を保つ」を出す。「読み込む」は今までと同じ `ApplyDiff`、「保つ」は保留を捨て、次の保存でディスク側を上書きすることを確認文に書く。
4. dirty でなければ今までどおり自動適用。自分の保存で起きる監視イベントは保存直後で dirty でないので、今と同じ。
5. Play 中の扱い (未決事項 1 の答えしだい)。
6. 口が呼ばれるのはセーフポイント (フェーズ 2) で、sim の Tick の中ではないことをコードで確かめ、コメントに理由を書く。
7. 文書: `engine_spec.md` のホットリロードの章、`docs\test_checklists.md`、文字列は英日。

8. 追加の取り決め (2026-11-15):
   - 口の形: `ReloadHub` に、`std::function<bool(const std::wstring& path)>` (真 = 今適用してよい、偽 = 保留) を設定する関数を足す。Engine 層は Editor を知らない (include しない)。未設定なら常に適用。
   - 保留の状態は `ReloadHub` が持つ (保留中のパス。1 つのシーンにつき 1 件)。Editor は「保留があるか」を問い、「適用する」「捨てる」を頼む。口の呼び出し・保留の適用は、今までの `ReloadActiveScene` と同じフェーズ (フェーズ 2 のセーフポイント) で行い、sim の Tick の中では行わない。
   - 「適用する」は、その時点のディスクの内容を読んで当てる (保留した時点の内容を覚えておかない)。ファイルが消えていたら、保留を捨てて利用者に知らせる。
   - 一括適用 (`BeginBatch` / `EndBatch`。git の操作や、sub-15 の復元) の経路では、口を呼ばない (その経路は、ゲートで未保存のシーンを先に止めている)。この前提がコードで成り立っているかを確かめ、成り立たない経路があれば「不安・質問」で返す。
   - 確認の出し方: 既存のモーダルかトーストの前例に合わせる (未保存のシーンを閉じるときの確認の作りを着手時に見る)。確認が出ている間も Editor は操作できる必要は無いが、Play の開始など状態を変える操作と競合しないこと。
   - シーンを切り替えた・閉じたら、保留は捨てる。
   - AI Changes 窓 / `SceneChangeOverlay` (sub-12) との関係: 保留中は、ライブのシーンとディスクが食い違っている。オーバーレイは「ディスクの変更」を出すので、保留中であることを AI Changes 窓の帯に 1 行出す (`ChangeTracking\` から `ReloadHub` を直接見ず、`EditorApp` が渡す)。
   - `Runtime.exe` と `Server.exe` は口を設定しない (挙動が変わらない)。
   - Engine 層を触るので、`tools\replay_verify.bat` を回す。ADR-039 の影響の節 (Engine 層の変更は `ReloadHub` の口 1 つ) と一致していること。

## やらないこと (このサブでは)

- 追跡サービスとの連携 (この確認は追跡が無効でも働く、Editor の安全機能)。
- `FileWatcher` の挙動変更 (ADR-039 却下案)。3 方向のマージ。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\src\Engine\Engine\HotReload\ReloadHub.h` / `.cpp` (`ReloadActiveScene` の手前)
- `C:\HAL\MyEngin\src\Editor\App\EditorApp.h` / `.cpp`、`LocalizationTable.inl`
- ホットリロードの既存スイート (名前は `Editor.exe --selftest-list` で確かめる) に回帰テストを足す (AGENTS.md §7)
- `C:\HAL\MyEngin\engine_spec.md`、`C:\HAL\MyEngin\docs\test_checklists.md`

## 受け入れ条件 (このサブ)

1. 口が未設定のとき、外部変更は今までどおり適用される。既存のスイートが期待値の変更なしで通る → `Editor.exe --selftest-full`。
2. 口が「保留」を返すと、ライブのシーンが変わらず (保存前に作ったエンティティが残る)、保留が 1 件ある → スイート (要求 §16.3 の 6 番目)。
3. 「読み込む」で `ApplyDiff` が当たり、「保つ」で保留が消えてシーンが変わらない → スイート。
4. dirty でなければ確認なしで適用される。自分の保存の直後に確認が出ない → スイート。
5. 保留中に同じパスが 2 回変わっても確認は 1 つで、「読み込む」は最新の内容を当てる → スイート。
6. Play 中の扱いが、ユーザーの答えどおり → スイート。
7. 確認の画面のスクショ (英日) → `--screenshot`。
8. `tools\replay_verify.bat` が通る (Engine 層を触ったので回す) → 実行結果。
9. `Runtime.exe` の挙動が変わらない (口を設定する呼び出しが Editor にしか無い) → grep。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL: sub-13 (round 1)
実装:
  - src\Engine\Engine\HotReload\ReloadHub.h/.cpp: SetSceneApplyGate (std::function<bool(const std::wstring&)>。未設定なら従来どおり適用)、保留 (pendingScenePath_。1 件)、HasPendingScene / PendingScenePath / RequestApplyPendingScene / DiscardPendingScene / TakePendingSceneResult (None / Applied / Gone / Failed)。ReloadActiveScene を「口 → ApplySceneFile」に分割。適用の要求は次の Update (フェーズ 2) で処理する。EndBatch の適用中は applyingBatch_ で口を通さない。SetActiveScenePath で保留を捨てる。Update の shaders_ を null 許容に (試験が Init を通さないため)
  - src\Editor\App\EditorApp.h/.cpp: OnStart で口を設定 (Play 中・ミニシーン編集中・未保存なら保留)、OnShutdown で外す。DrawExternalSceneChangeModal (未保存なら モーダル「読み込む / 自分の編集を保つ」、未保存でなければ自動で適用要求、Gone / Failed はトースト)。SaveCurrentScene の成功時に保留を捨てる。AiChangesHost.externalChangePending を渡す
  - src\Editor\Windows\Project\AiChangesWindow.h/.cpp: AiChangesHost::externalChangePending、DrawSceneBand に host を渡して帯に 1 行出す
  - src\Engine\Core\Localization\LocalizationTable.inl: Popup_ExternalSceneChange / ExtScene_* / AiChg_PendingExternal (英日)
  - src\Engine\Engine\HotReload\ReloadHubSelfTest.cpp: ReloadHubTestAccess (friend) と 8 ケースの回帰試験
  - engine_spec.md §8.3、docs\test_checklists.md (M104n)
仕様との差分:
  - [追加] ミニシーン編集中 (actorEdit_) も Play 中と同じく保留にする — ctx.scene がアセット側に差し替わっており、本シーンへ向けた ApplyDiff を当てる経路が元から怪しいため。sub の文面は Play 中だけ
  - [追加] 保存 (SaveCurrentScene 成功) で保留を捨てる — 「保つ」の続きの「次の保存でディスクを上書き」を、確認を経ない保存でも成り立たせるため
  - [追加] ReloadHub の公開が口 1 つではなく 7 関数 — 保留を ReloadHub が持つ取り決め (8) のため。ADR-039 の影響の節の「口 1 つ」は文面を直さず、概念上 1 つの口 + 保留の操作とみなした (直すなら planner)
  - [追加] 試験用に ReloadHub へ `friend struct ReloadHubTestAccess`、Update の shaders_ の null 判定
  - [未実装] 「読み込む」後も dirty (タイトルの *) は残す — ApplyDiff は fileId の無い未保存のエンティティを残すので、clean 扱いにできない
検証:
  - Debug ビルド / Release ビルド (MyEngine.sln) → 成功
  - MYE_SELFTEST_ONLY=reloadhub (Debug) → PASS (新規 11 チェック含む)。Release でも reloadhub / changetracking / localization PASS
  - Debug: --selftest-list の 125 スイートから physics を除いた 124 本を MYE_SELFTEST_ONLY で → 124 PASS、0 failed (physics は D38 のため除外。調べていない)。最初に別名 (m94 等) を混ぜて physics が走り exit -1073741571 で落ちたのは、名前の取り方の誤り (D38 と同じ症状)
  - Release: --selftest-full → 125 suites 0 failed
  - tools\check_rules.ps1 → 0 error (66 warning は既存)
  - tools\replay_verify.bat → PASS (24 ジョブ、17 シーン、Debug/Release 一致)
  - スクショ: cache\m104n_shots\1_modal_ja.png / 1_modal_en.png (一時の probe で保留・未保存を強制して撮った。probe は戻した。見た目の合否は目視未確認)
  - grep: SetSceneApplyGate の呼び出しは EditorApp.cpp のみ (Runtime / Server は設定しない)
自己採点 (1-5):
  仕様適合: 4 — 条件 1〜9 を試験・実行で確認。Editor の経路 (口の実体・自動適用・モーダル・Play 後の持ち越し) は ReloadHub 単体の試験と目視用スクショまでで、実 Editor での操作は未実施
  正しさ: 4 — 8 ケースの試験と replay_verify が通る。Editor 側の分岐は読んだだけで実操作は未確認
  コード品質: 4 — 責務は ReloadHub (保留) / EditorApp (判断・UI)。公開関数が増えた点は差分に記載
  テスト: 4 — ReloadHub 側は網羅。Editor 側 (モーダル・Stop 後の挙動・AI Changes 帯) は自動試験なし
不安・質問:
  - Play 中に保留 → Stop 後に未保存なら確認、未保存でなければ自動適用、の実機確認はユーザーの目視が必要 (チェックリストに追加)
  - EndBatch の後、150 ms 以内にもう一度来る通常経路のイベントは口を通る。その間に利用者が編集すると確認が出る (今までは ApplyDiff が編集を戻していた)。許容でよいか
  - 保存の直後 150 ms 以内にまた編集すると、自分の保存の監視イベントで確認が出うる
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\HotReload\ReloadHub.h / ReloadHub.cpp / ReloadHubSelfTest.cpp
  - C:\HAL\MyEngin\src\Editor\App\EditorApp.h / EditorApp.cpp
  - C:\HAL\MyEngin\src\Editor\Windows\Project\AiChangesWindow.h / AiChangesWindow.cpp
  - C:\HAL\MyEngin\src\Engine\Core\Localization\LocalizationTable.inl
  - C:\HAL\MyEngin\engine_spec.md、C:\HAL\MyEngin\docs\test_checklists.md、本ファイル
  (生成物: cache\m104n_shots\ の画像とログ、%TEMP%\m104n_*.txt は自分の一時物。消していない)
申し送り:
  - AI Changes 窓の帯の 1 行は、窓にデータがあるとき (hasData) だけ出る。追跡が無効だと出ない (モーダル自体は追跡と無関係に出る)
  - 消したもの・止めたものは無し

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし。`src\Engine` の差分は `HotReload\ReloadHub.h` / `.cpp` / `ReloadHubSelfTest.cpp` と `LocalizationTable.inl` だけ。Runtime / Server / GameLogic / Shared / Scripting / tools に差分なし。`SetSceneApplyGate` の呼び手は `EditorApp.cpp` の 2 か所 (設定と解除) と試験だけ。`ReloadHub` は Editor のヘッダを include していない。スクショ用のプローブの残りなし。planner の実走: Debug の `MYE_SELFTEST_ONLY=reloadhub,changetracking,localization` が 3 スイート 0 失敗、Release の `--selftest` 全体が 112 スイート 0 失敗、`tools\check_rules.ps1` 0 error。スクショ `cache\m104n_shots\1_modal_ja.png` を開いて見た: 確認の文面 (「読み込む」「自分の編集を保つ」の説明つき) が切れずに出ている。**見た目の合否と、実 Editor での操作はユーザーの確認 (未確認)。** `tools\replay_verify.bat` と Release の `--selftest-full`、Debug の physics を除いた 124 スイートは coder の結果による (planner は再実行していない)。
  - 追加の判定: ミニシーンの編集中も保留にする、保存の成功で保留を捨てる、`ReloadHub` の公開関数が 6〜7 個になった (口 1 つ + 保留の操作)、試験用の friend と `shaders_` の null 許容、は全部採る。「読み込む」の後も dirty を残すのも妥当。
  - 質問への回答: (1) Play 中の保留 → Stop の後の動きは、ユーザーの手動確認として残す。(2) `EndBatch` の後の 150 ms 以内のイベントが口を通り、その間に編集していると確認が出るのは許容 (今までは編集が黙って戻されていた)。(3) 保存の直後に編集を続けると自分の保存で確認が出うる件は、sub-14 で直す (保存の直後に控えた大きさと更新時刻が同じなら、確認を出さずに保留を捨てる)。
  - nit: Debug の `Editor.exe` (13:58) が、最後に変わったソース `EditorApp.cpp` (14:46、プローブを外した時刻) より古い。Release は外した後にビルドし直されている。内容はプローブを入れる前に戻っているはずだが、Debug を最終のソースからビルドし直すのは sub-14 で。ADR-039 の「口 1 つ」の文面も sub-14 で直す。