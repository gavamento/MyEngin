# sub-26: サービス側の時間に依存する試験を、機械の負荷に強くする (M104o2)

- 依存: sub-14
- 状態: OK (commit は台帳 harness.md を参照)
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

### round 1

原因の区分 (根拠。詳細は `docs\ai-change-tracking\architecture.md` §4「sub-26」):

| 試験 | 区分 | 根拠 |
|---|---|---|
| `pipe.rs` の `scene_diff_is_paged_...` (`items` が空でない) | (c) クライアントの不具合 | 通知は 200 ms のポーラが `id` なしの 1 行として差し込む (ipc-contract §4.5)。`Client::request` は次の 1 行を応答として返していた。`list` の直後に `note.txt` の `change_recorded` が届くと、次の応答として通知の行を読み、`reply["result"]` が null になる (観測した症状と一致)。製品のサービス側は正しい。単体試験 (`client.rs`) で再現・固定。実走で落ちた回の生の応答は取れていない (planner の観測のみ)。メッセージに応答を出すようにした |
| `watch.rs` の `waiting_for_a_writer_gives_up_...` | (b) 経験則が負荷で外れた | 負荷をかけた初回の実行で再現。Revision は 1 件 `[coalesced, after = 最終の内容]` (打ち切りの前に書き手が終わった)。最終の内容は残っており、製品は仕様どおり |
| `watch.rs` の `single_saves_stay_complete_under_cpu_load` | (b) | 負荷で 1 回の保存のイベントが `burst_gap` (200 ms) を超えて離れた場合の仕様。今回の実行では再現せず (落ちた回の記録は planner の観測のみ) |
| `reconcile.rs` の `verification_scan_catches_missed_...` (負荷の実行で 1 回落ちた。x.txt が `modified/complete`) | (a) 試験の待ち方 | 「イベントを捨てる」のを固定の 100 ms で待っていた。監視スレッドが遅れて取り出すと旗が戻った後に通常経路で処理される |
| `watch.rs` の `writes_within_debounce_make_one_coalesced_revision` (負荷の実行で 1 回落ちた。Revision が `complete` + `coalesced` の 2 件) | (b) | 書き込む側が 600 ms のデバウンスを超えて止まった |

実装:
- `src/debounce.rs` (新規): 経験則の判定を `Instant` の引数を取る純粋な型・関数にした (`Pending` + `touch` / `is_multiple` / `due` / `waited_too_long` / `deferred` / `carry_over`、`window_expired`、`keep_holding`、`verify_due`)。単体試験 10 件 (実時間の `sleep` なし)。
- `src/watch.rs`: `Pending` を移し、判定を上の関数へ。実時間を渡す点は同じ (挙動は変えない)。`dropped_event_paths` (試験用)。捨てる旗の判定を `handle_event` の先頭へ寄せた (3 か所の呼び出し元から外した)。
- `src/collector.rs`: `flush` が `now` を受け、預かりの期限を `keep_holding` で判定。
- `src/client.rs`: `request` が同じ `id` の応答まで読み、先に来た通知を `wait_line` 用に取っておく。単体試験 2 件。
- `tests\watch.rs`: 3 + 1 試験を `#[ignore]` (計測・負荷) へ移し、負荷に依らない対の試験を既定に置いた。固定の 100 ms 待ちを 1 か所置き換え。`tests\reconcile.rs` も 1 か所。
- 文書: `architecture.md` §4 に「sub-26」(時刻の口、試験の区分、`#[ignore]` の一覧、切り分け)。

検証 (すべて `tools\track`。負荷 = 自分で起動した `bench load --threads 12` (論理コア 12)、終了後に止めた):
- 負荷あり・既定の `cargo test --manifest-path tools\track\Cargo.toml` (全体、fail-fast): 最終版で **10 回 / 10 回通過** (15 バイナリ × 10 = `test result: ok` 150 行、FAILED 0)。1 回約 1 分 35 秒〜2 分 20 秒。
  - 経緯: 変更前 2 回 (1 回失敗: giveup)。client / watch の修正後の 10 回は 9/10 (run 6 で reconcile の 1 件)。reconcile を直した後の 10 回も 9/10 (run 6 で `writes_within_debounce...`)。両方を直した最終版の 10 回が 10/10。つまり 10 回連続通過の確認は最終版の 1 セットだけで、それ以前の失敗は上の表の (a) / (b) で直した。
- 負荷なし 5 回: 5/5 通過。
- `cargo test --test watch -- --ignored` (負荷なし): 4 件通過 (5.4 秒)。`cargo test -- --ignored` の全体は既存の巨大試験 (`reconcile.rs` の 64000 ファイルなど、最長 90 分) を含むので回さなかった。
- `tools\build_track.bat` は `bin\x64\*` の `mye_track.exe` を置き換えるので使わず、`cargo build --release` + `pwsh tools\track_verify.ps1` を 3 回: 3 回とも全シナリオ PASS。
- `mye_track bench changes --counts 1,1000` (1000 ファイル): 1 件 470〜473 ms、一括 1000 件 2935 ms (performance.md の 422〜474 ms、3723 ms と同じ桁)。
- `check_rules.ps1`: 0 error (66 warning は既存の docs の規則 17)。`git diff --stat HEAD -- src tools/mcp tools/track/src/schema`: 空。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: 差分は `tools\track\src` (`debounce.rs` 新規、`client.rs`、`collector.rs`、`lib.rs`、`watch.rs`)、`tools\track\tests` (`watch.rs`、`reconcile.rs`、`pipe.rs`)、`architecture.md` だけ。`src`、`tools\mcp`、`tools\track\src\schema` に差分なし。planner の実走 (CPU 使用率 20〜60%、ユーザーの別のプログラムが動いている状態): 全体の `cargo test` を 5 回続けて、5 回とも 277 件通過・7 件 ignored (sub-14 の VERDICT のときは同じコマンドが 5 回中 3 回失敗)。残存プロセスなし。`#[ignore]` へ移ったのは `tests\watch.rs` の 4 件 (`writes_within_debounce_make_one_coalesced_revision`、`file_held_open_for_a_second_is_not_recorded_mid_write`、`waiting_for_a_writer_gives_up_as_coalesced_and_the_final_content_follows`、`single_saves_stay_complete_under_cpu_load`) で、どれも「負荷で製品の経験則の結果が変わる」性質。負荷をかけた 10 回、`-- --ignored`、`track_verify` 3 回、計測は coder の結果による。
  - 原因の区分を受け入れる: `tests\pipe.rs:811` は (c) = クライアント (`Client::request`) が、応答の前に届いた通知の行を応答として返していた不具合。サービスは正しい。ただし落ちた回の生の応答は取れておらず、仕様の読みと単体での再現による結論 (coder が正直に書いている)。影響するのは通知を購読する接続だけで、`mye_track client` を使う検証と試験。フックは購読しないので影響しない。Editor 側 (C++ の `TrackClient`) は `id` で対にしているので別。
  - 逸脱 (時計の trait ではなく、判定を時刻引数の純粋な関数に切り出した): 採る。経験則の判定を偽の時刻で確かめる、という目的を満たしている。監視スレッドの待ちが実時間のままなのは製品として正しい。
  - 質問への回答: 単発の保存の `complete` を表明する既定の試験 (`watch.rs`、`reconcile.rs`、`pipe.rs`、`session.rs` に点在) は、既定に残す。200 ms を超える停止でだけ落ちうるが、普通の条件で成り立つべき性質で、coder の負荷 20 回と planner の 5 回で落ちていない。今後落ちたら、落ちた試験名と停止の長さを見て、その時に移すかを決める。
  - `bin\x64\Debug|Release\mye_track.exe` が sub-26 の変更を含まない古いままである件: `bin\` はコミットしない生成物なので、コミットには影響しない。sub-15 の最初に `tools\build_track.bat Release` を回す (sub-15.md に書いた)。