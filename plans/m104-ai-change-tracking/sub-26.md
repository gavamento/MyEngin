# sub-26: サービス側の時間に依存する試験を、機械の負荷に強くする (M104o2)

- 依存: sub-14
- 状態: 未着手
- 往復: 0
- 順序: sub-14 → **sub-26** → sub-15 (番号は末尾。sub-14 の VERDICT で足した。spec §2 D39)
- 書き込みの深さ: 詳細
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。

## 事実 (2026-11-15 に planner が観測した。機械はユーザーの別のプログラムで CPU 使用率 27〜100% の間を動いていた)

`cargo test --manifest-path tools\track\Cargo.toml` (全体) を 5 回回して、**3 回失敗**。落ちた試験は毎回違い、どれも sub-14 より前からある:

| 回 | 落ちた試験 | 場所 | 中身 |
|---|---|---|---|
| 1 | `scene_diff_is_paged_oversize_values_are_flagged_and_only_the_editor_may_ask` | `tests\pipe.rs:811` | テキストの Revision への `scene_diff` の `items` が `[]` でない (実際の値は取れていない) |
| 2 | `single_saves_stay_complete_under_cpu_load` | `tests\watch.rs:988` | `coalesced` が 1 件 (期待 0) |
| 5 | `waiting_for_a_writer_gives_up_as_coalesced_and_the_final_content_follows` | `tests\watch.rs:780` | 「待ちを打ち切った Revision」と「最終の Revision」の 2 件のはずが 1 件 |

`--test pipe` だけを 6 回回すと 6 回通る (全体を並列で回したときにだけ出る)。sub-14 が足した試験 (`tests\review.rs` ほか) は、通った回も落ちた回も失敗していない。

Phase 5 (復元) は安全性が P0 で、検証がこれらの試験の結果に乗る。**赤が「本物の不具合」か「機械が重かっただけ」か分からない状態で復元へ進まない。**

## やること

**先に原因を分ける。** 上の 3 件と、同じ型の試験 (実時間の待ち・しきい値に依存するもの) について、失敗が (a) 試験の待ち方の問題 (b) 製品の経験則が負荷で外れた (c) 製品の不具合、のどれかを、落ちた回の記録 (Revision の列、イベントの順、応答の中身) で決める。推測で直さない。再現には、coder が自分で起動して自分で止める負荷 (`mye_track bench load`) を使う。**ユーザーのプロセスを止めない。**

1. **`tests\pipe.rs:811`**: 何が返っていたかを取る (失敗のメッセージに応答を出す)。考えられる原因は、応答ではなく通知の行を読んだ、`note.txt` の Revision の ID の取り違え、テキストの Revision に構造の変更が付いた、など。(c) なら製品を直す。
2. **製品の時刻の出どころを差し替えられるようにする**: `Collector` / `Tracker` のデバウンス (300 ms / 2 秒)、塊の間隔 (`DEFAULT_BURST_GAP` 200 ms)、書き手の待ち (`settle_wait` / `settle_samples`)、検証の走査の間隔が、実時間 (`Instant::now()`) を直接読んでいるなら、小さい時計の口 (trait か関数) を通す。製品は今までどおり実時間。**経験則の判定 (`coalesced` の 3 条件、デバウンス、預かり、打ち切り) を、差し込みのイベント (`inject_event`) と偽の時計で確かめる決定的な試験**を持つ。受け入れ条件として数えるのは、この決定的な試験。
3. **実ファイル監視を通す結合の試験** (本物の `notify`、本物のファイル) は残す。ただし:
   - 固定の `sleep` や「N ms 以内」のしきい値で合否を決めない。「条件が成り立つまで、上限つきで待つ」形にする。上限は十分に長く (失敗は「遅い」ではなく「起きなかった」を意味するように)。
   - **機械が重いと製品の経験則そのものが変わる性質** (例: 負荷の下で 1 回の保存が `complete` のままであること。`DEFAULT_BURST_GAP` を超えて詰まれば `coalesced` になるのが仕様) を、既定の実行で合否にしない。そういう試験は `#[ignore]` にして「計測・負荷の試験」として明示して回す (`-- --ignored`)。既定の実行には、同じ判定を偽の時計で確かめる決定的な試験 (2) を置く。どの試験を移したかを一覧にして文書に書く。
   - 「負荷に依らず成り立つべきこと」(記録の欠落が無い、作業ツリーを書かない、書き手を失敗させない、最終の内容の Revision が残る) は、既定の実行に残す。
4. `tests\reconcile.rs`、`tests\session.rs`、`tests\hook.rs`、`tests\pipe.rs` の、時間に依存する箇所も同じ基準で洗う (`sleep`、`Duration` のしきい値、`wait_until` の上限)。
5. 文書: `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §4 (時刻の口)、試験の区分 (決定的 / 結合 / 計測・負荷) の説明を `tools\track` の README 相当か `performance.md` に。CI (`ci.yml` の track service の段) は既定の `cargo test` のままでよいことを確かめる。

## やらないこと (このサブでは)

- 経験則の値 (300 ms、200 ms、100 ms など) の変更。監視の作り直し。
- 失敗した試験を黙って再試行する仕組み (リトライで緑にしない)。
- `src\` (Editor) と `tools\mcp` の変更。スキーマの変更。
- Debug の `physics` の件 (spec §2 D38)。

## 受け入れ条件 (このサブ)

1. 上の 3 件それぞれについて、原因の区分 (a / b / c) と根拠 (落ちた回の記録) が `sub-26.md` の実装メモか文書にある。(c) があれば製品の修正と、その不具合を決定的に再現する試験がある。
2. 経験則の判定が、偽の時計と差し込みのイベントで確かめられている: 1 回の保存 (イベント 2 件、間隔 0) は `complete`。間隔がしきい値ちょうど・しきい値 + 1 の境界。`Modify` 3 件以上。デバウンスの 300 ms と最大 2 秒。書き手の待ちの打ち切り。預かりの期限 → `cargo test` (実時間の `sleep` を含まない)。
3. **負荷の下で通る**: coder が自分でかけた CPU 負荷 (論理コア数ぶん) の下で、既定の `cargo test --manifest-path tools\track\Cargo.toml` (全体) を 10 回続けて、10 回とも通る。負荷なしで 5 回 → 回数と失敗数をそのまま書く。落ちたら、落ちた試験と区分を書く (成功扱いにしない)。
4. `#[ignore]` へ移した試験の一覧と、移した理由 (負荷で製品の経験則が変わる性質) が文書にある。移した試験は、負荷なしで `-- --ignored` を 1 回回して通る。**「負荷に依らず成り立つべきこと」の試験を `#[ignore]` に移していない** (一覧で確かめる)。
5. 製品の挙動が変わっていない: `tools\track_verify.bat` が全シナリオ PASS (3 回)。計測 (`mye_track bench changes --counts 1,1000`) が sub-21 / sub-06 の値と同じ桁。
6. `tools\check_rules.ps1` が 0 error。`git diff --stat HEAD -- src tools\mcp tools\track\src\schema` が空。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (負荷あり 10 回、負荷なし 5 回)
- `cargo test --manifest-path tools\track\Cargo.toml -- --ignored` (負荷なし。時間がかかるものは `--release`)
- `tools\build_track.bat Release`、`tools\track_verify.bat` (3 回)
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴