# sub-21: 取りこぼしの検出と、ベースライン・記録の計測 (M104d2)

- 依存: sub-03
- 状態: 未着手
- 往復: 0
- 順序: sub-03 → **sub-21** → sub-04 (番号は末尾だが Phase 1 の中。sub-03 の VERDICT round 1 で足した)

sub-03 で分かった 3 つの事実を片付ける。どれも「変更を正しく記録する」(要求 §19 の 1 番目) の土台で、Phase 2 へ進む前に要る。

1. `notify` 8.2.2 は Windows で監視バッファのあふれを報告せず、黙って取りこぼす (spec §2 D27。64000 件の作成で約 7000 件が届かず、`Err` も再走査のフラグも 0 件)。
2. ベースラインと記録が遅い見込み (spec §2 D28。32000 ファイルで十数分。原因は未計測)。
3. 記録に失敗した観測 (ジャーナルへ書けないなど) が、log に出るだけで欠落として残らない (sub-03 の未実装)。

## やること

**先に計測、次に直す** (AGENTS.md §3.5、§6)。

### A. 計測 (最初にやる。結果で B と C の中身が変わる)

1. 計測の口: `mye_track.exe` に計測用のサブコマンド (例 `bench baseline --project <root>`) を足すか、`cargo test` の `#[ignore]` 付きの計測試験にする。**release ビルドで測る。** 所要時間の内訳 (走査、読み取り、ハッシュ、Blob の書き込み、`sync_all`、読み直し、DB) を取れるようにする。
2. 対象: (a) このリポジトリのコピーではなく、**リポジトリそのものを読むだけ**で、`.mye\ai_changes\` は一時ディレクトリに向ける (作業ツリーを書かない。リポジトリの `.mye` も汚さない。向け先を変える口が要るなら試験用に足す) (b) 生成した 32000 ファイル。
3. 測るもの: ベースラインの所要時間、ファイル数、Blob の総量 (重複排除の後)、1 件の変更の記録にかかる時間、一括 1000 / 32000 件の記録のスループット。
4. 結果を `C:\HAL\MyEngin\docs\ai-change-tracking\prototype-results.md` か新しい計測の文書に、環境・コマンド・値つきで残す。

### B. スループット (計測で遅いと分かった箇所だけ)

- 候補 (推測。計測で確かめてから手を付ける): Blob 1 件ごとの `sync_all`、読み直しの検証、1 件 1 トランザクション、Writer への 1 件ずつの受け渡し。
- 守ること: 「検証してから `snapshots` に行を足す」(要求 §13.3)、「ジャーナルが DB より先」、「落ちても DB は壊れない」。`sync_all` をやめるなら、電源断で起こりうること (中身の欠けた Blob が最終名で残る) と、それを起動時の検査 (db §6 の 3 を「存在」だけでなく必要なら内容まで) でどう見つけるかを db に書く。
- ベースラインの間、サービスは応答を続ける (進捗を `status` で返せる。sub-05 が使う)。ベースラインの途中で止められても、次の起動で続きから、または最初から正しくやり直せる。
- 目標値は置かない。前後の値を示す。ただし「このリポジトリのベースラインが分単位で終わらない」ままなら、理由と次の手を「不安・質問」で返す。

### C. 取りこぼしの検出 (spec §2 D27 の (b))

1. **検証の走査**: ベースライン (最後に確認した状態) と、実際のディレクトリの中身を突き合わせる。安い順に比べる (存在 → サイズと更新時刻 → 違うものだけハッシュ)。更新時刻を持つために保持する情報が増えるなら、メモリ上の状態に足す (DB の形を変えるなら db も直す)。
2. 走らせる時点: (a) 1 回の flush、または短い時間のイベントの数がしきい値を超えた後 (b) 静かな時間が続いたとき、一定の間隔で (c) `notify` が `Err` / 再走査のフラグを返したとき (sub-03 の経路)。しきい値と間隔は定数にし、計測を根拠に決める。
3. 見つけたずれは、`completeness = gap`、`unknown` / `unknown` の変更として記録し、`coverage_gaps` に `watcher_overflow` (疑い) の区間を残す。区間は「前回の検証の走査 (または起動) から今まで」。
4. 走査の最中に届くイベントと競合しない (走査が読んだ後に書かれたファイルを、古い内容で上書きしない)。順序は Writer のキューで 1 本に決まる。
5. 走査は読むだけ。sub-03 の受け入れ条件 10 (書き手が失敗しない) を、走査の最中にも満たす。

### D. 記録に失敗した観測

- ジャーナルや DB へ書けなかった観測は、メモリ上に「未記録の区間」として持ち、書けるようになったら `coverage_gaps` に残す (`disk_full` / `queue_overflow` / その他)。その間の状態は `status` に出せる形で保持する。書けるようになった後、検証の走査で実際の状態と突き合わせる。

## やらないこと (このサブでは)

- `ReadDirectoryChangesW` を自前で呼ぶ実装 (spec §2 D27 で却下。C で足りないと実測で分かったら、実装せず「不安・質問」で返す)。
- 容量の回収 (db §7)。ベースラインの Blob を持たない設計への変更 (spec §2 D28 で却下)。
- 行 diff (sub-04)、pipe (sub-05)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\collector.rs`、`watch.rs`、`observe.rs`、`blob.rs`、`baseline.rs`、`main.rs`、`tests\watch.rs` (または新しい `tests\reconcile.rs`)
- `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §4、§11、`db-schema.md`、計測の記録
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` — 進捗表に M104d2 を足す

## 受け入れ条件 (このサブ)

1. 計測の記録が文書にあり、同じコマンドで取り直せる。このリポジトリのベースラインの所要時間・ファイル数・Blob の総量と、所要時間の内訳がある → 文書とコマンドの実行。
2. 軽くした箇所ごとに、前後の値がある。軽くしなかった候補は、計測でそう判断した根拠がある → 文書。
3. **本物の取りこぼし**: sub-03 で取りこぼしを再現した条件 (16 スレッド × 4000 ファイル、またはそれで再現しなければ再現する条件) で書いた後、検証の走査が終わった時点で、全ファイルが「通常の Revision」か「`gap` の Revision」のどちらかとして記録されていて、記録の無いファイルが 0 本。取りこぼしが実際に起きた回では `coverage_gaps` に区間がある → `cargo test` (時間がかかるなら `#[ignore]` にして、検証コマンドで明示して回す。回した結果を報告する)。
4. 取りこぼしを人工的に起こす (sub-03 の `drop_incoming_events`) と、作成・変更・削除のそれぞれが検証の走査で `gap` として拾われる → `cargo test`。
5. 何も変わっていないとき、検証の走査は Revision も `coverage_gaps` も作らない。走査の最中に書かれたファイルが、古い内容の Revision で終わらない (最後の Revision の After が実際の内容と一致) → `cargo test`。
6. 検証の走査と重ねて、sub-03 の受け入れ条件 10 の書き手を回し、書き手の失敗が 0 件 → `cargo test`。
7. ジャーナルへの書き込みを失敗させる差し込みで、失敗の間の観測が、復帰後に `coverage_gaps` の区間として残り、検証の走査で実際の状態が `gap` として拾われる → `cargo test`。
8. `sync_all` や検証のやり方を変えた場合: 中身の欠けた Blob (最終名で、内容がハッシュと違う) を置いて起動すると、見つかって `blob_state` が直る → `cargo test`。変えなかった場合はこの条件は対象外と書く。
9. ベースラインの途中でサービスを止めて起動し直すと、最終的に全ファイルのベースラインがそろい、ベースラインの最中に変わったファイルが正しい Before を持つ → `cargo test`。
10. sub-02 と sub-03 の試験が全部通り、全体の `cargo test` を 5 回続けて通る。sub-03 の受け入れ条件 7 (作業ツリーを書かない) がこのサブの試験でも成り立つ → `cargo test`。
11. `tools\check_rules.ps1` が 0 error。計測の最中と後で、リポジトリの `git status` に `.mye` 以外の変化が無い。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (5 回)
- `cargo test --manifest-path tools\track\Cargo.toml --release -- --ignored` (計測と本物の取りこぼしの試験)
- 計測のコマンド (文書に書いたもの)
- `pwsh -NoProfile -File tools\check_rules.ps1`、`git status --short`

## 実装メモ (coder が追記)

SELF_EVAL: sub-21 (round 1)
実装:
  - tools/track/src/bench.rs (新規) / main.rs / lib.rs — `mye_track bench gen|baseline|changes`。記録の置き場所を `--data` で別ディレクトリへ向け、追跡対象は読むだけ。所要時間の内訳を出す
  - tools/track/src/startup.rs (`StartupOptions::data_dir`、`verify_recent_blobs`、`StartupReport::blobs_corrupted`) / store.rs (`Paths::at`、`StoreStats`、`record_snapshot_hashed`、試験用 `inject_journal_error`) — 計測の口と、起動時の直近 Blob の内容確認
  - tools/track/src/blob.rs — 1 件ごとの `sync_all` をやめた。読み直しを書いたハンドルで行う (`read_from_start`)。`put_hashed`。`BlobStats` (内訳の累計)
  - tools/track/src/content.rs / baseline.rs — `ReadOutcome::Content` と `FileState` に更新時刻。`peek_stat`
  - tools/track/src/observe.rs — `RecordFailure` (`disk_full` / `write_failed`)。ジャーナル・スナップショット・Revision のどれが失敗しても観測を Err にし、状態を進めない。`Source::VerifyGap`
  - tools/track/src/collector.rs — `detect_changed` / `observe_paths` (検証の走査の前半・後半)。`rescan` はサイズと更新時刻が同じなら読まない。`Unrecorded` (記録に失敗した区間) と `retry_unrecorded`。`ScanProgress`、止める合図で走査を打ち切る、`ScanTimings`、設定 (`verify_*`、`unrecorded_retry`、試験用 `baseline_chunk_pause`)
  - tools/track/src/watch.rs — `maintenance` / `verify` / `drain_events`。ベースラインの途中停止 (`baseline_complete` を残さない)。`TrackerStatus` に `scan_done` / `scan_total` / `baseline_timings` / `verify_runs` / `verify_gaps` / `unrecorded_since_ms` / `unrecorded_reason`
  - tools/track/tests/reconcile.rs (新規) — 受け入れ条件 3〜9 の試験 9 件 (うち 1 件が `#[ignore]`) と `put_hashed`、処理待ちイベントの取り違え防止
  - docs/ai-change-tracking/performance.md (新規) / architecture.md §4・§11 / db-schema.md §4・§5.1・§6 / plans/m104-ai-change-tracking.md (進捗表に M104d2) / sub-03.md (round 2 の SELF_EVAL を追記)
仕様との差分:
  - [追加] 1 件ごとの `sync_all` をやめた (計測で 231.6 秒のうち 151.4 秒)。代わりに起動時、前回の最後のイベントの 10 分前から後に書かれた Blob を読み直し、ハッシュが違えば消して `blob_state = missing` にする (db §4・§6 に記載)。限界: 10 分より前の Blob は確かめない。受け入れ条件 8 の試験あり
  - [追加] 書いた Blob の読み直しを、閉じて開き直す方式から「書いたハンドルで読む」方式へ。検証の意味は同じ。`BlobStore::read_back` の型が `fn(&Path)` から `fn(&mut File)` に変わり、sub-02 の試験 `tests/storage.rs` の 1 行 (口を戻す行) を直した
  - [追加] `put_hashed` / `record_snapshot_hashed` (`put` の二度目のハッシュを省く。渡したハッシュが違えば読み直しで失敗する)
  - [追加] 失敗した記録の扱いを広げた: ジャーナルだけでなく、スナップショットや Revision を残せなかった場合も観測を Err にして状態を進めない (今までは log に出して Revision なしで続けていた)。理由の種類は `disk_full` / `write_failed` の 2 つ。`coverage_gaps.reason` に `write_failed` を足した (db §2 の列挙)
  - [追加] 検証の走査は 2 段 (違いを集める → イベントが遅れて届く時間だけ待つ → 残ったものだけを `gap` にする)。「走査の最中に届くイベントと競合しない」「何も変わっていなければ何も作らない」ため。実測で、長い flush の間に溜まった処理待ちのイベントを取りこぼしと取り違える不具合を見つけて直した (走査の前にキューを空にする。試験 `backlog_of_unprocessed_events_is_not_mistaken_for_a_miss` は修正を外すと 3 回中 2 回落ちる)
  - [追加] 試験用の公開口: `Store::inject_journal_error` (`#[doc(hidden)]`)、`TrackerConfig::baseline_chunk_pause`
  - [追加] ベースラインの途中停止は「最初からやり直す」(続きからは実装していない。仕様の「または」の一方)
  - [追加] 検証の走査の既定値: `verify_quiet` 3 秒、`verify_interval` 5 分、`verify_burst_events` 1000 件、`unrecorded_retry` 1 秒。根拠は §5 の計測 (32000 件の書き換えで検証の走査が動く条件) と、5 分おきの全走査は 2968 ファイルで数百 ms (更新時刻だけ見る) という見積もり。最後の「数百 ms」の実測は未取得 (検証の走査だけの時間は測っていない)
  - [未実装] 「ベースラインを読み取りの間もサービスが応答する」の状態遷移は `status` の値までで、pipe は sub-05。`scan_done` / `scan_total` は `Tracker::status()` で読める
  - [未実装] 軽くしなかった候補の一部 (`create_dir_all` を省く、ベースラインの並列化、rename を省く) は、performance.md §3 に理由 (計測または設計) を書いて見送った
検証:
  - cargo test --manifest-path tools/track/Cargo.toml (debug) を 5 回続けて通過: 毎回 lib 14 / project_id 3 / reconcile 9 (+1 ignored) / storage 16 / watch 19 = 61 件
  - cargo test --release --test reconcile -- --ignored --nocapture → 通過。16 スレッド × 4000 ファイル (64000) を 78.8 秒で書き、検証の走査が 2693 件の取りこぼしを見つけた (`Err` も再走査フラグも無し)。記録 64000 / 64000、記録の無いファイル 0、gap の Revision 2693、`watcher_overflow` の区間 1、最後の Revision の After が全件で実際の内容と一致 (受け入れ条件 3)
  - 負のテスト: `drain_events` を外すと `backlog_of_unprocessed_events_is_not_mistaken_for_a_miss` が 3 回中 2 回落ちた (gap 1120 件、445 件)。戻して通過
  - cargo clippy --all-targets → 警告 0
  - pwsh -NoProfile -File tools/check_rules.ps1 → error 0 (warning 60 は既存と同数)。途中で Python の書き込みが改行を CRLF にして規則 14-c の error 15 件が出たので、LF に直した
  - 計測 (release): performance.md に環境・コマンド・値。このリポジトリのベースライン 231.6 秒 → 12.4〜20.8 秒 (同じ版が時間帯で 34.6〜36.7 秒だった回もあり、値は 2 倍以上揺れる)。32000 ファイルのベースライン 72〜103 秒、1 件の変更 422〜474 ms (debounce 300 ms と書き手の待ち 100 ms を含む)、一括 1000 件 268 件/秒、32000 件 229 件/秒 (書き換え側の 62 秒を含む。Writer だけなら約 355 件/秒が上限)
  - 計測の最中と後の `git status --short`: tools/track・docs/ai-change-tracking・plans/m104 以外の変化なし。リポジトリの `.mye` は作られていない。一時ディレクトリの計測用ファイルは消した
  - selftest / replay_verify → 未実行 (src\ を触らないサブのため。指示どおり)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜11 を試験・実測で満たした。ただし B の「ベースライン進捗」は status の値までで、検証の走査自体の所要時間 (5 分おきの全走査の費用) は測っていない
  正しさ: 4 — 本物の取りこぼし (2693 件) を検出して全件が記録された。処理待ちイベントの取り違えは負のテストで確認。5 回続けて通過。限界 (更新時刻とサイズが同じ書き換え、10 分より前の欠けた Blob) は文書に書いた
  コード品質: 4 — clippy 0 件。collector / watch がさらに長くなった。計測用の累計 (BlobStats / StoreStats) は製品のコードに残した (理由は performance.md)
  テスト: 4 — 受け入れ条件ごとの試験あり。時間依存の試験は上限付きの待ち合わせ。Windows の負荷下で揺れうる
不安・質問:
  1. [planner 判断] 計測の絶対値は時間帯で 2 倍以上揺れた (ウイルス対策のスキャンと見ているが未確認)。受け入れ条件 2 の「前後の値」は、同じ時間帯の連続した測定とプロセス内の累計を根拠にした。足りないと見るか
  2. [planner 判断] 32000 本の小さなファイルのベースラインは 72〜103 秒で、残りの大半は Blob 1 本ごとのファイル作成・rename (NTFS のメタデータ操作)。「分単位」ではないが、数万本でさらに短くするには小さい Blob のパック (保存形式の変更) が要る。このサブでは見送った。続けるなら別サブ
  3. [planner 判断] `sync_all` をやめたことで、電源断の直前 (10 分以内) の Blob は起動時に確かめるが、それより前は確かめない。受け入れてよいか (SQLite とジャーナルも `sync` しないので、直近の記録そのものが電源断で失われうるのは今までと同じ)
  4. 検証の走査の既定 (静かな 3 秒、5 分おき、1000 イベント) は計測を根拠にした初期値。実運用 (AI が長く編集し続ける) で調整が要るかもしれない
  5. 一時ディレクトリに、sub-02/03 の試験が残した `mye_track_*_<pid>` のディレクトリが数百個ある (試験が消していない)。このサブの範囲外なので触っていない
触ったファイル:
  tools/track/src/{bench(新規),baseline,blob,collector,content,lib,main,observe,startup,store,watch}.rs、tools/track/tests/{reconcile(新規),storage}.rs、docs/ai-change-tracking/{performance(新規),architecture,db-schema}.md、plans/m104-ai-change-tracking.md、plans/m104-ai-change-tracking/sub-03.md (実装メモの追記のみ)、plans/m104-ai-change-tracking/sub-21.md (実装メモ)
申し送り:
  - sub-04: `observed` の `source` に `verify_gap` が増えた。`ChangeType` / `Completeness` は変わらない。`record_snapshot` は `record_snapshot_hashed` のラッパー。`FileState.modified` (更新時刻) が増えた
  - sub-05: `Tracker::status()` に `scan_done` / `scan_total` / `baseline_timings` / `verify_runs` / `verify_gaps` / `unrecorded_since_ms` / `unrecorded_reason` が増えた。`serve` は従来どおり。`mye_track bench` は pipe と無関係の計測用
  - `Store::inject_journal_error` と `TrackerConfig::baseline_chunk_pause` は試験用。製品のコードから使わないこと
  - 編集で Python の `open(p, 'w')` を使うと Windows では CRLF で書かれて check_rules の規則 14-c に落ちる (`newline=''` か bytes で書くこと)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。判定の前に確かめたこと: `mye_track` / `cargo` / `rustc` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、`git status` は tools\track・docs\ai-change-tracking・plans\m104 だけ。planner が全体の `cargo test` を 2 回実走し、毎回 lib 14 / project_id 3 / reconcile 9 (+1 ignored) / storage 16 / watch 19 が通過。`--ignored` の本物の取りこぼしの試験 (6 分) は planner は再実行せず、coder の結果 (64000 / 64000、gap 2693、区間 1) と performance.md §6 の記録による。
  - 追加の判定: `sync_all` をやめて起動時に直近 10 分の Blob を確かめる、読み直しを書いたハンドルで行う、`put_hashed`、記録の失敗を Err にして状態を進めない、`write_failed`、検証の走査を 2 段にする、ベースラインは最初からやり直す、は全部採る。
  - 質問への回答: (1) 前後の値は、同じ時間帯の連続した測定とプロセス内の累計で足りる。揺れを文書に書いてあるのがよい。(2) 小さい Blob のパックはやらない (32000 本で 1 分半〜2 分は、応答を続けられる限り許容。必要になったら sub-19 の計測で再検討)。(3) 受け入れる。電源断で危ないのは直近の書き込みだけで、10 分は十分に広い。(4) 初期値でよい。(5) 下の should。(6) 了解。
  - should (sub-04 で対応): 試験の Fixture が一時ディレクトリを消していない (`%TEMP%` に `mye_track_*` が 931 個)。試験が自分で作った一時ディレクトリは、成功したら後処理で消す。
  - nit: 検証の走査だけの所要時間が未計測。`walk_ms` (このリポジトリで 0.2〜0.3 秒) から軽いと見積もれるが、sub-05 の `status` に直近の検証の走査の所要時間を出して実測に置き換える。