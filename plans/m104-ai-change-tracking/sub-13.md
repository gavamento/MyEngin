# sub-13: 未保存シーンへの外部変更の確認 (M104n)

- 依存: sub-25 (順序は sub-24 → sub-25 → sub-13 → sub-14。コード上の依存は sub-08 だけ) (Phase 2 の完了。sub-09〜12 とコード上の依存は無いが、Phase 3 の中で行う。spec §2 D13)
- 状態: 未着手
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

## フィードバック履歴
