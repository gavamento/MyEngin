# sub-04: 行 diff、Hunk、Revision、リネーム (M104e)

- 依存: sub-21 (sub-03 の VERDICT round 1 で、sub-03 と sub-04 の間に sub-21 = M104d2 を足した)
- 状態: 未着手
- 往復: 0

## やること

arch §5 を作る。sub-15 の Hunk 復元はここで作る Hunk の形に乗るので、**Hunk が Before と After を正確に再構成できること**がこのサブの中心。

1. Myers 法の行 diff をサービス内に実装する (依存を足さない)。入力は Before / After の Blob、出力は Hunk の列 (前後 3 行の文脈)。
2. 行の区切りは LF / CRLF / 末尾改行なしをそのまま保持する。`before_text` / `after_text` / `context_pre` / `context_post` は改行込みのバイト列を保てる形で持つ (db §3 は TEXT。UTF-8 として読めたファイルだけが行 diff の対象なので TEXT で足りるが、改行を正規化しないこと)。
3. 改行だけが変わった Hunk は `eol_only = 1`。本文の変更と同じ Hunk に混ざる場合の扱いを決めて文書に書く (推奨: 行ごとに判定し、Hunk の全行が改行だけの違いなら `eol_only`)。
4. Diff ワーカー 1 本 (arch §3.1)。結果は Writer へ渡す。Revision の確定は「スナップショット保存 → 差分生成 → Revision 確定 → DB 登録」(要求 §5.2) の順で、Hunk の無い Revision が外から見える瞬間を作らない (1 トランザクション、または「未確定」の状態を外へ出さない)。
5. Hunk ID は (Revision, 通し番号)。Revision が変われば Hunk ID も変わる。
6. Change と Revision: 同じパスへの変更は 1 つの Change にまとめ、観測ごとに Revision を足す (db §3.1)。新しい Revision ができたら、前の Revision の状態の扱い (`changes.state` の `superseded`) を db のとおりにする。
7. リネームのまとめ (sub-01 の試作 3 で確定: 別のディレクトリへの移動は `Remove` + `Create` でしか届かないので、ハッシュによるまとめが**主たる手段**。ディレクトリの名前変更で sub-03 が作った「中のファイルの削除と作成」も同じ規則でまとまる): 監視がリネームとして返したものに加え、同じデバウンス窓の中の「削除 + 同じ内容ハッシュの作成」を 1 件の `renamed` にする。内容が違えば別々の削除と作成のまま (推測でつながない)。同じハッシュの候補が 2 つ以上あるときはつながない。
7a. sub-03 の結果 (2026-11-14): 別のディレクトリへの移動は「削除 + 作成」の 2 件の `observed` で、ペイロードの `window` が同じ。まとめはこの `window` を見る。同じディレクトリ内のリネームは sub-03 が既に 1 件の `renamed` にしている。events の kind とペイロードは db §5.1。`changes` はパスごとの active を再利用して `seq` を足す形になっている。
7a2. sub-21 の結果 (2026-11-15): `observed` の `source` に `verify_gap` が増えた (検証の走査が拾った取りこぼし。`completeness = gap`)。`FileState` に更新時刻が増えた。`record_snapshot` は `record_snapshot_hashed` のラッパー。記録に失敗した観測は Err になり状態が進まない — Hunk の計算と Revision の確定を足すときも同じ扱い (途中で失敗したら Revision を確定させず、欠落として残す) にする。
7a3. 試験の後始末 (sub-21 の VERDICT の should): `tests\` の Fixture が作る一時ディレクトリ (`%TEMP%\mye_track_*`) を、試験が成功したら後処理で消す (失敗したときは調べられるよう残してよい)。sub-02 / sub-03 / sub-21 の試験の Fixture も同じにする。既に溜まっている分は消さない (ユーザーの判断)。
7b. 派生の表の作り直し (sub-02 の VERDICT で追加): DB が壊れて作り直した後、`events` (ジャーナルから戻ったもの) と残っている Blob から、`changes` / `change_revisions` / `change_hunks` / `snapshots` を作り直す関数を持つ。Hunk は Blob から計算し直せる。Blob が欠けている Revision は行だけ戻し、`blob_state = missing` にする。起動時の検査 (db §6 の 1) の後に呼ぶ。通常の記録の経路と作り直しの経路が同じ関数を通る形にする (2 通りの実装を持たない)。
8. 大きい入力の上限: 行数 / サイズが大きいとき、Myers の最悪計算量で Diff ワーカーが止まらないよう上限を置き、超えたら「ファイル全体を 1 Hunk」に落として、その旨を Revision に残す。上限の値は計測して決め、文書に書く。

## やらないこと (このサブでは)

- pipe 越しの `hunks` op (sub-05 は `describe` の要約まで、本文付きの `hunks` は sub-09 か sub-10 の着手時に、サービス側の op として足す。骨子の見直しで確定する)。
- シーンの構造 diff (sub-11)。JSON も、このサブではただのテキストとして行 diff する。
- Hunk の適用 (復元) の本実装。ただしテスト用に「Hunk の列を Before に当てて After を得る」関数を lib に置く (sub-06 の `declared_edit_match` と sub-15 の検証でも参考になる)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\` に `diff.rs` (Myers と Hunk の組み立て)、`revision.rs`、`rename.rs`、`tests\diff.rs`。
- `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §5、`db-schema.md` §3 — 実装で決めた点 (eol の扱い、上限)。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` — 進捗表。

## 受け入れ条件 (このサブ)

1. 1 ファイルの離れた 3 か所を変えると Hunk が 3 つでき、それぞれの `old_start` / `old_count` / `new_start` / `new_count` と文脈 (最大 3 行、ファイルの端では短くなる) が正しい → `cargo test`。
2. **性質テスト**: 固定 seed で生成した多数の (Before, After) の組について、Hunk の列を Before に当てた結果が After とバイト一致し、逆向き (After に逆適用) で Before とバイト一致する。seed はテストに固定で書く → `cargo test`。
3. 境界: 空 → 内容あり、内容あり → 空、1 行、末尾改行の有無だけの違い、CRLF と LF の混在、全行置き換え、先頭・末尾だけの変更、日本語と絵文字を含む行 → `cargo test`。
4. CRLF → LF だけを変えたファイルは `eol_only` の Hunk になり、本文の変更として数えられない → `cargo test`。
5. 同じ窓の中の「削除 + 同じ内容の作成」が 1 件の `renamed` (`old_path` 付き) になる。内容が 1 バイトでも違えば 2 件。同じ内容の候補が 2 つあればつながない → `cargo test`。
5b. 別のディレクトリへの移動 (1 ファイル) と、3 ファイルを含むディレクトリの名前変更が、それぞれ `renamed` 1 件 / 3 件になる。同じ内容のファイルが 2 本あるディレクトリの名前変更は、候補が一意に決まらないので、パスの対応 (ディレクトリ名の前方一致) で決められる場合だけつなぎ、決められなければつながない → `cargo test`。
6. 同じパスを 2 回変えると Change は 1 つ、Revision は 2 つで `seq` が連番。2 つ目の Hunk ID は 1 つ目と違う → `cargo test`。
7. 上限を超える入力で Diff ワーカーが時間内に返り、Revision に「全体を 1 Hunk に落とした」ことが残る → `cargo test` (時間の上限付き)。
8. sub-03 の受け入れ条件 7 (作業ツリーを書かない) がこのサブのシナリオでも成り立つ → `cargo test`。

9. 記録した後に `tracking.db` を壊して起動し直すと、退避 → 作り直しの後で、壊す前と同じ Change / Revision / Hunk (パス、種別、ハッシュ、Hunk の範囲と本文) が得られる。Blob を 1 つ消しておくと、その Revision は行が戻り `missing` になる → `cargo test`。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1)
実装:
  - tools\track\src\diff.rs (新規) — Myers 法 (トレース付き O(ND)、行を整数化)、Hunk 組み立て (文脈 3 行、間 6 行以下は統合)、`apply_hunks` / `unapply_hunks`、`DiffLimits` (超えたら全体 1 Hunk)。本文 (行末の LF/CRLF を除く) で比べ、本文が同じで行末だけ違う行は 1 行の変更範囲にする。
  - tools\track\src\revision.rs (新規) — `plan_diff` (binary / excluded / unavailable / whole の判定)、`write_revision` (Change の特定・superseded・Revision・Hunk を 1 SAVEPOINT)、`Deriver` / `rebuild_derived` (events から派生の表を作り直す。通常経路と同じ `write_revision`)。
  - tools\track\src\rename.rs (新規) — 同じ窓・同じハッシュの削除 + 作成を `renamed` に。1:1 はパス不問、複数は末尾パス要素の 1 対 1、さらにディレクトリの対応で残りを決める。
  - tools\track\src\diff_worker.rs (新規) — Diff ワーカー 1 スレッド。観測 + `DiffPlan` を返す。panic は観測ごとの Err。
  - tools\track\src\observe.rs — `observation_payload` を切り出し、`apply_observations` が `plans` を受け取る。`insert_revision` を廃止して `write_revision` へ。計画が Err の観測はジャーナルにも書かずに Err (状態を進めない)。`ChangeType::parse` / `Completeness::parse`。
  - tools\track\src\collector.rs — Diff ワーカーを持つ。`commit` が差分を作ってから Writer へ。`drain` でリネームのまとめ。`drain_holding` / `release_held`: 窓に待ちのパスがある間、内容ハッシュ付きの削除を預かる (実測: 新しいファイルは settle で遅れ、削除と作成は別の flush になる)。`Deferred.window`。`TrackerConfig.diff_limits`。
  - tools\track\src\watch.rs — `flush` へ待ちの窓を渡す、停止前に `release_held`、`BlobStore::reader` を Collector へ。
  - tools\track\src\startup.rs — 壊れた DB の作り直しの後に `rebuild_derived`。通常起動のジャーナル入れ直しもイベントごとに派生の表へ反映 (`Deriver`)。`StartupReport.rebuilt`。
  - tools\track\src\blob.rs (`read` / `reader`)、baseline.rs (`ContentKind::parse`)、schema\v1.sql (`diff_mode` 列)、lib.rs。
  - 試験: tests\diff.rs (新規 21 件。性質テスト 4000 組・上限・リネーム規則)、tests\revision.rs (新規 13 件。結合・作り直し・Hunk 書き込み失敗)、tests\watch.rs (別ディレクトリ移動とディレクトリ名変更を renamed の期待へ)、tests\storage.rs と src\content.rs の一時ディレクトリを成功時に後処理で消す (`TempProject` / `TempDir`)。
  - docs: db-schema.md (§3.2 新設、diff_mode、§5.1、§6)、architecture.md §5、plans\m104-ai-change-tracking.md の進捗表 (M104e 完了)。
仕様との差分:
  - [追加] `change_revisions.diff_mode` 列を v1.sql に足した (移行 v2 にしていない) — 「上限超えを Revision に残す」(やること 8) の置き場。v1 は未配布で、storage.rs の移行試験が版 2 を使うので v1 を直した。既に v1 の DB を作ってしまった環境は、Revision の記録が失敗する (消して作り直す)。
  - [追加] `diff_mode` の `binary` / `excluded` / `unavailable` — Hunk が無い理由を同じ列で表す (Before を見ていない `no_before` を「全行が追加」に見せないため)。
  - [追加] `changes.state = superseded` の規則 — db に定義が無いので、(1) 削除の Revision で終わった Change への再作成、(2) リネームで元のパスが去ったときの元のパスの Change、で `superseded`。それ以外の Revision は同じ Change へ足す。planner の確認が欲しい (下)。
  - [追加] リネームの窓またぎの預かり (`drain_holding`) — 受け入れ条件 5 / 5b は、削除と作成が別の flush に分かれると成り立たない (実測。決める前の試験では分かれた)。内容ハッシュ付きの削除を `max_hold` まで預かる。
  - [追加] diff_worker.rs — planner の見立ての 3 ファイル (diff / revision / rename) に足した。
  - [追加] 通常起動のジャーナル入れ直し (DB のトランザクションが確定する前に落ちた分) も派生の表へ反映する (`events_replayed_from_the_journal_also_rebuild_their_revision`)。7b の「同じ関数を通る」の素直な帰結で、無いとイベントだけ入って Revision が欠ける。
  - [追加] 試験で作った一時ディレクトリの後処理は storage.rs と content.rs だけ直した。watch.rs / reconcile.rs の Fixture は元から常に消している。既存の `%TEMP%\mye_track_*` (969 個) は消していない。
  - [逸脱] 受け入れ条件 8 (作業ツリーを書かない) は、tests\revision.rs の Fixture の後処理で全テストに適用した。
検証:
  - cargo test --manifest-path tools\track\Cargo.toml → 全部 PASS を 4 回連続 (lib 14 / diff 21 / project_id 3 / reconcile 9 (+1 ignored は元から) / revision 13 / storage 16 / watch 19)。
  - 成功した実行の前後で `%TEMP%\mye_track_*` の数が変わらない (989 → 989。新たに残らない)。
  - pwsh -NoProfile -File tools\check_rules.ps1 → 0 error / 60 warning (うち tools\track に関するもの無し)。
  - 計測 (一時の試験で実施後に削除): architecture.md §5 に値と実測を書いた。
  - 未実行: selftest / replay_verify (src\ を触らないサブのため)。
  - 未実行: 本物の Claude Code 等での実運用。ディレクトリ移動は tests\watch.rs / tests\revision.rs の実ファイル操作で確認。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜9 を試験で確認。superseded の意味づけは db に定義が無く推測で置いた
  正しさ: 4 — 性質テスト 4000 組で両方向のバイト一致。作り直し前後の 4 表が行単位で一致。窓またぎの預かりは実測した競合 1 件を直したが、他の競合は網羅していない
  コード品質: 4 — 純粋な計算 (diff / rename) と DB (revision) を分け、書き込みは `write_revision` 1 箇所。collector.rs の預かりは状態が増えた
  テスト: 4 — 受け入れ条件ごとに試験あり。Hunk 失敗は DB トリガで注入。複数ファイルの預かりの競合は 4 回の連続実行で安定を確認したが、負荷の下の再現は未
不安・質問:
  - `superseded` の意味づけ (db に定義無し)。上の規則で足りるか。
  - Hunk の書き込みに失敗した観測は、ジャーナルと `events` には入っている (ジャーナルが先の原則)。派生の表は作られず、検証の走査が後で `gap` の Revision を足す。そのあと DB を作り直すと、同じ変更の Revision が 2 つになる (失敗した 1 件目が events から導かれる)。許容するか、`observed` を失敗の後に取り消す印を足すか。
  - `rebuild_derived` は `coverage_gaps` を戻さない (db-schema.md §6 に明記)。要るなら別サブ。
  - diff_mode を v1.sql へ直接足したことの可否 (移行 v2 にすると storage.rs の移行試験の版番号を直す必要がある)。
触ったファイル:
  tools\track\src\{diff.rs, diff_worker.rs, rename.rs, revision.rs, observe.rs, collector.rs, watch.rs, startup.rs, blob.rs, baseline.rs, content.rs, lib.rs, schema\v1.sql}
  tools\track\tests\{diff.rs, revision.rs, watch.rs, storage.rs}
  docs\ai-change-tracking\{architecture.md, db-schema.md}、plans\m104-ai-change-tracking.md、plans\m104-ai-change-tracking\sub-04.md (実装メモのみ)
申し送り:
  - `Store::inject_journal_error` / `baseline_chunk_pause` は触っていない。製品コードから使っていない。
  - sub-05 以降: Hunk 本文の取り出しは `change_hunks` を `revision_id` で引く (tests\revision.rs の `revision_views` が見本)。`apply_hunks` / `unapply_hunks` は sub-06 の declared_edit_match / sub-15 で使える。
  - `%TEMP%\mye_m104b\` と既存の `%TEMP%\mye_track_*` は消していない。
  - 起動した mye_track.exe / cargo のプロセスは残っていない (確認済み)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `C:\HAL\MyEngin\collector.rs` は残っていない、`mye_track` / `cargo` の残存プロセスなし、`git status` は tools\track・docs\ai-change-tracking・plans\m104 だけ。planner が全体の `cargo test` を 3 回実走し、毎回 lib 14 / diff 21 / project_id 3 / reconcile 9 (+1 ignored) / revision 13 / storage 16 / watch 19 が通過。実走の前後で `%TEMP%\mye_track_*` は 989 → 989 (新しく残らない)。
  - 追加・逸脱の判定: `diff_mode` 列とその値、`superseded` の規則、窓またぎの預かり、diff_worker.rs、通常起動の入れ直しを派生の表へ反映、受け入れ条件 8 を Fixture の後処理で満たす、は全部採る。
  - 質問への回答: (1) `superseded` の規則 (削除で終わった Change への再作成 / リネームで元のパスが去った Change) で足りる。(2) Hunk の書き込みに失敗した観測が、後で DB を作り直したときに Revision 2 件になる件は許容する。二重の故障 (書き込みの失敗の後に DB の破損) でだけ起き、結果は同じ変更が 2 回見えるだけで、内容は正しい。取り消しの印は足さない。db §6 に制限として 1 行書くこと (sub-05 で。nit)。(3) `rebuild_derived` が `coverage_gaps` を戻さないのは、sub-02 の入れ直しが `coverage_gap` のイベントから戻すので足りる。(4) `diff_mode` を v1.sql に直接足したのは採る (v1 の DB はまだ試験の一時ディレクトリにしか無い)。**ただし sub-05 でサービスが実際に動くようになった後のスキーマの変更は、必ず移行 (v2 以降) にする。** (5) 削除の記録が最大 2 秒遅れるのは許容。