# sub-03: ファイル監視、ベースライン、欠落区間 (M104d)

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること

arch §4 の「ファイル監視」の経路を作る。デバウンスの幅とイベントの読み方は、sub-01 の試作 3 の観測 (`C:\HAL\MyEngin\docs\ai-change-tracking\prototype-results.md`) に合わせる。設計文書と観測が食い違っていたら、観測が正。

**sub-01 の結果による確定事項 (2026-11-14 の見直し。arch §4 の「決定」と prototype-results の試作 3 の結論 1〜7 が正)**:

- デバウンスはパスごと。最後のイベントから 300 ms で flush、最初のイベントから 2 秒を超えたら続いていても flush (初期値。定数にして、テストでは短くできるようにする)。
- **イベントの種別と件数で変更を判定しない。** flush の時点でファイルの存在と内容のハッシュを見て、スナップショットと違うときだけ変更にする。ハッシュが同じなら捨てる。ディレクトリ自身の `Modify` は捨てる。置き換えの rename に混じる `Remove` は、flush の時点でファイルがあれば削除にしない。
- **書き込み中の見分け方 (arch §4 の決定を直す。spec §2 D21)**: arch §4 は「flush は `FILE_SHARE_READ` だけで開いて読む」と書いているが、そのハンドルを開いている間、他のプロセスの書き込み open・rename・delete が共有違反で失敗する。**追跡が動いているせいで AI やエディタの保存が失敗する**形になる (要求 §1.3、arch §2.1 に反する)。次のとおりにする。
  - 探り: `share_mode(FILE_SHARE_READ)` で開けるかだけを見て、**開けたらすぐ閉じる** (内容は読まない)。共有違反なら書き手がいるので 250 ms 後にやり直す。30 秒で打ち切る。
  - 読み取り: 内容は**全共有** (`FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`) で開いて読む。読む前後でサイズと最終更新時刻が同じことを確かめ、違えば読み直す (上限回数つき)。上限を超えたらその時点の内容で `coalesced`。
  - 探りの一瞬にも窓は残る。受け入れ条件 10 で、書き手が失敗しないことを確かめる。失敗が出るなら探りをやめ、全共有の読み取り + 前後の照合だけにして、結果を arch §4 に書く。
  - このサブで arch §4 の該当の段落を直す。
- **ディレクトリ単位の操作はイベントに頼れない** (名前変更・移し入れ・移し出しでは中のファイルのイベントが来ない)。ディレクトリの `Create` / `Name(To)` / `Remove` を見たら、ベースラインの一覧と実際の中身を 1 ディレクトリ分 (その下の再帰を含む) だけ突き合わせて、中のファイルの作成・削除を作る。ルート全体は走査しない。
- 同じディレクトリ内のリネームは `Name(From)` の直後の `Name(To)` を隣り合いで対にする。別のディレクトリへの移動は `Remove` + `Create` で来る。**このサブでは別々の削除と作成として記録し、同じ窓に入っていたことが sub-04 で分かるようにする** (まとめるのは sub-04)。
- `notify` が `Err` を返す、または再走査を求めるフラグ付きのイベントが来たら、その区間を `watcher_overflow` にしてベースラインを取り直す。試作では再現しなかったので、**人工的に起こして確かめる** (受け入れ条件 11)。

**sub-02 の結果による追記 (2026-11-14、sub-02 の VERDICT)**:

- DB が壊れて作り直したとき、ジャーナルから戻るのは `events` と `coverage_gaps` だけ。**観測は必ず `Store::record_event` を通し、ペイロードに「パス、種別、Before / After の Blob ハッシュとサイズ、`completeness`、観測時刻」を入れる。** `changes` / `change_revisions` / `snapshots` はこのイベントから導ける派生の表として作る (作り直しの関数とテストは sub-04)。イベントに無い情報を派生の表だけに書かない。
- `service_start` / `service_stop` を `events` に書く (sub-02 では書いていない)。db §6 の 7 はこれを使う。
- 起動時に `blobs\` の下の書きかけの一時ファイル (`.tmp-*`) を消す (自分が書いたもので、どこからも指されない。サービスが 1 つだけ動いている前提なので、起動時なら安全)。
- Writer へは `Writer::submit` / `call` にクロージャを積む (sub-02 の形)。

1. 監視スレッド (arch §3.1): `notify` のイベントをデバウンス・重複排除して Writer へ渡す。
2. 監視範囲と無視: プロジェクトルート全体。`.git`、`.mye`、`cache`、`dist`、`target`、`bin`、`obj` と `.gitignore` に当たるパスを無視する。`.gitignore` の解釈は依存を足さずに書ける範囲 (ルートと各ディレクトリの `.gitignore` の基本的なパターン) にとどめ、対応しない書き方は文書に書く。git は起動しない。
3. ベースライン: 起動時に追跡対象の全ファイルの内容ハッシュを取り `snapshots` (`kind = baseline`) に残す。所要時間とファイル数を計測して保持する (sub-05 の `status` が返す)。
4. 停止していた間の変更: 前回のベースライン (または最後の観測) と比べ、違うファイルごとに 1 件の変更を記録する。`completeness = gap`、`actor = unknown`、`attribution = unknown`。`coverage_gaps` に `service_down` (db §6 の 7)。
5. 観測の記録: 作成・変更・削除・リネーム。Before は「最後に確認できた内容」。Before が無ければ `no_before`。デバウンス中に複数回書かれたら最初の Before と最後の After で `coalesced`。
6. 監視バッファのあふれ: その区間を `coverage_gaps` に `watcher_overflow` で残し、ベースラインを取り直す。
7. 種類の判定: UTF-8 として読めないもの、8 MiB (設定可) を超えるものは `binary` (ファイル単位の記録だけ)。除外パターンは sub-02 の判定を使う。
8. このサブの Revision は「ファイル単位の変更」まで (Before / After のスナップショットと `change_type`)。Hunk は sub-04。`change_revisions` の行はこのサブで確定させてよいが、Hunk の付与を後から足せる順序にする。
9. `serve` は監視を続ける常駐になる。終了の合図は標準入力の EOF かシグナル (pipe 経由の終了と無接続の時間切れは sub-05)。`service_stop` を `events` に残す。

## やらないこと (このサブでは)

- 行 diff、リネームの「削除 + 同じ内容の作成」のまとめ (sub-04)。
- pipe、通知 (`change_recorded`)、`editor_saved` (sub-05、sub-09)。
- 帰属の判定 (sub-06)。このサブの記録は全部 `unknown` / `unknown`。
- シーンの構造 diff と `documents` 表への登録 (sub-11)。`.meta` を特別扱いしない。
- 作業ツリーへの書き込み (全サブ共通で禁止)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\` に `watch.rs`、`baseline.rs`、`ignore.rs`、`collector.rs` など。`tests\watch.rs`。
- 参考: `C:\HAL\MyEngin\tools\collab\src\watch.rs` (無視の規則と notify の使い方)、`C:\HAL\MyEngin\tools\collab\tests\watch.rs` (時間に依存するテストの書き方)。
- `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §4 — 実装で決めた幅と、対応しない `.gitignore` の書き方。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` — 進捗表。

## 受け入れ条件 (このサブ)

1. 一時プロジェクトでファイルを作成・変更・削除・リネームすると、それぞれ 1 件の Revision が `change_type` 付きで記録され、Before / After の Blob が原本とバイト一致する → `cargo test`。
2. 「一時ファイルへ書いて rename」で保存したファイルが、対象のパスの 1 件の変更として記録される (一時ファイルの作成と削除が別の変更として残らない) → `cargo test`。
3. デバウンスの幅より短い間隔で 3 回書くと、Revision は 1 件で `coalesced`、Before は 1 回目の前、After は 3 回目の後 → `cargo test`。
4. サービスを止めてファイルを変え、起動し直すと、その変更が `gap` + Unknown で 1 件記録され、`coverage_gaps` に `service_down` の区間がある。止まっていた間に 2 回変えても 1 件 → `cargo test`。
5. 無視するディレクトリと `.gitignore` に当たるファイルの変更は記録されない。`.mye\ai_changes\` への自分の書き込みを自分で拾わない → `cargo test`。
6. UTF-8 でないファイルと上限を超えるファイルは `binary`。除外パターンのファイルは内容なしで「変わったこと」だけ → `cargo test`。
7. **上のシナリオ全部の前後で、作業ツリー (`.mye` を除く) の全ファイルのパスとハッシュの一覧が、テスト自身が書いた分を除いて変わらない** (spec §2 D17)。テストの共通の後処理で検査する → `cargo test`。
8. 1000 ファイルを一括で変えても落ちず、全件が記録されるか、あふれた区間が `watcher_overflow` で残る (黙って取りこぼさない) → `cargo test` (遅いテストは `#[ignore]` にせず、時間の上限を決めて回す)。
9. テストが時間に依存して不安定にならない: 「イベントが来るまで待つ」は固定の sleep ではなく、上限付きの待ち合わせで書く → 同じテストを 5 回続けて回して全部通る。

10. 追跡が読んでいる最中・探っている最中でも、他のプロセスの保存が失敗しない: 別スレッドが同じファイルへ「一時ファイルへ書いて rename」と `fs::write` と削除を 1 ms 間隔で 1000 回くり返し、その間サービスが監視・flush し続けて、書き手側の失敗が 0 件 (spec §2 D21) → `cargo test`。0 件にならなければ探りをやめる設計へ変え、その結果を報告する。
11. 人工的なあふれ (`notify` のコールバックへ `Err` を差し込む口、または監視バッファを小さくして大量に書く) で、区間が `coverage_gaps` に `watcher_overflow` で残り、ベースラインが取り直され、あふれの間の変更が `gap` として 1 件ずつ記録される → `cargo test`。本物のあふれを起こせたか、差し込みで確かめたかを区別して報告する。
12. ディレクトリの名前変更、外で作ったツリーの移し入れ、ツリーの移し出し、ツリーごとの削除で、中の全ファイルの変更が記録される (取りこぼしが無い) → `cargo test`。
13. 開いたままのハンドルへ 1 秒かけて書くファイルで、書き込みの途中の内容が「完全な Revision」として記録されない (最終の内容の Revision が残る。途中を読んだ場合は `coalesced`) → `cargo test`。
14. 同じ内容で上書き保存しただけ (ハッシュが同じ) では Revision ができない → `cargo test`。

15. (round 1 の VERDICT で追加) **1 回だけの保存が、負荷の下でも `complete` になる**: CPU を使い切る背景スレッド (論理コア数ぶん) を回しながら、別々のファイルへ `fs::write` を 1 回ずつ 200 件、「一時ファイルへ書いて rename」を 200 件行い、全 Revision が `complete` (`coalesced` が 0 件)。同じ負荷の下で、デバウンスの幅より短く、かつ判定のしきい値より十分長い間隔で 3 回書いたファイルは `coalesced` → `cargo test`。しきい値は定数にして文書に書く。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (5 回)
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-03 (round 1)
実装:
  - tools	rack\src\ignore.rs — 固定の 7 名 (深さを問わず) と `.gitignore` の基本的な書き方。対応しない書き方は architecture.md §4 に記載
  - tools	rack\src\content.rs — 全共有で読み、前後のサイズ・更新時刻を照合。`peek_len` (ハンドル経由のサイズ)。探りは採らず
  - tools	rack\srcaseline.rs — ベースラインの型と、DB (snapshots / change_revisions) からの前回状態の復元
  - tools	rack\src\observe.rs — Writer 側の記録 (observed / baseline / coverage_gap のイベント → snapshots / changes / change_revisions)。1 塊 1 トランザクション
  - tools	rack\src\collector.rs — flush: ディレクトリの突き合わせ、同一ディレクトリ内リネームの対、ハッシュ比較、書き手の待ち (settle)、全体の再走査 (起動時・あふれ後)
  - tools	rack\src\watch.rs — `Tracker` (監視スレッド、デバウンス、窓、service_start / service_stop、ベースライン、あふれ処理、試験用の差し込み口)
  - tools	rack\src\main.rs — `serve` を常駐化 (標準入力の EOF / Ctrl+C / Ctrl+Break / コンソールを閉じる)
  - tools	rack\src\startup.rs — 起動時に `.tmp-*` の一時 Blob を消す
  - tools	rack\srclob.rs — `glob_match_ci` を crate 内公開にしただけ
  - tools	rack\Cargo.toml — notify 8 (default-features = false) と windows-sys の feature 2 つを追加
  - tools	rack	ests\watch.rs — 18 件 (受け入れ条件 1〜14。後処理で作業ツリーの不変を検査)
  - docsi-change-trackingrchitecture.md §4 / db-schema.md §5.1・§6 / prototype-results.md (注記) / plans\m104-ai-change-tracking.md (進捗表・申し送り)
仕様との差分:
  - [逸脱] 「`FILE_SHARE_READ` だけで開く探り」は採らなかった。受け入れ条件 10 の試験で、探り付きだと書き手の操作 1667 回中 438 件が失敗した (rename の ERROR_ACCESS_DENIED)。sub-03.md の指示どおり探りをやめ、全共有の読み取りだけで同じ試験は 0 件 (3 回続けて、最終版でも)。結果は architecture.md §4 に書いた。
  - [追加] 探りに代わる書き手の見分け: `peek_len` を 100 ms おきに取り、サイズが続けて 2 回同じになるまで読まない (`settle_samples`)。変わったら coalesced。30 秒で打ち切り。受け入れ条件 13 を満たすために必要だった。1 回の保存が debounce + 100 ms で記録される (遅延が増える)。限界 (書き込み間隔が 100 ms より長い書き手は見分けられない) は architecture.md に書いた。
  - [追加] coalesced の判定 (仕様は「デバウンス中に複数回書かれたら」だけ): ①イベントが 5 ms 超で 2 つ以上の塊、②内容の `Modify` が 3 件以上、③待っている間にサイズが変わった / 共有なしで開かれていた。イベントの時刻は notify のコールバックの中で付ける。多めに付ける側に倒した。
  - [追加] 同じディレクトリ内のリネーム (旧パスがベースラインにあり、新パスが新規) は 1 件の `renamed` Revision (old_path 付き)。別ディレクトリへの移動は削除 + 作成で、同じ `window` を持つ (sub-04 がまとめる)。`window` = 近い時刻 (debounce 以内) に届いたイベントのパスを結ぶ印。
  - [追加] events の kind: `service_start` / `service_stop` / `baseline` / `baseline_complete` / `observed` / `coverage_gap`。payload は db-schema.md §5.1。
  - [追加] 試験用の公開口 `Tracker::inject_event` と `drop_incoming_events`。あふれの差し込みと取りこぼしの模擬のため。
  - [追加] 固定の無視名は深さを問わない (collab は最上位だけ)。`tools	rack	arget` のような入れ子の生成物を拾わないため。
  - [追加] `plans\m104-ai-change-tracking.md` の進捗表で M104c と M104d を「完了」にした (指示どおり)。
  - [未実装] 本物のバッファあふれの検出。notify 8.2.2 は Windows で報告しない (下の不安・質問 1)。
  - [未実装] 記録に失敗した観測 (ジャーナルへ書けない等) の再試行。ベースラインを進めないだけで、次のイベントまで再走査しない。
検証:
  - cargo test --manifest-path tools	rack\Cargo.toml → 全 51 件 (lib 14 + project_id 3 + storage 16 + watch 18) 通過。最終版を 6 回続けて通過 (受け入れ条件 9)。途中で 1 回、追加前のコードで書き手待ちの試験が負荷で落ち (CPU が詰まり、書き手が 100 ms 止まった)、間隔を 250 ms × 3 回にして直した
  - 受け入れ条件 10: 書き手 1667 操作 (一時ファイル + rename / fs::write / 削除 + 再作成を 1 ms 間隔)、デバウンス 1 ms で追跡を重ねて、失敗 0 件、Revision 38 件。探り付きだと 438 件失敗 (上記)
  - 受け入れ条件 11: 差し込み (Err と Rescan フラグ) で検証。本物のあふれ: notify だけを付けた使い捨ての試験で、16 スレッド × 4000 ファイル = 64000 件を書くと、届いた Create は 57064 件、Err 0 件、Rescan 0 件 = 黙って取りこぼした (architecture.md §4 に記載。試験コードはコミットしていない)
  - cargo clippy --all-targets → 警告 0
  - pwsh -NoProfile -File tools\check_rules.ps1 → error 0 (warning 60 は既存)。最初は error 1 件 (tools/track/tests/project_id_cases.tsv の作業ツリーが CRLF = sub-02 の残り) で、LF に直した (git の差分は無い)
  - selftest / replay_verify → 未実行 (src\ を触らないサブのため。指示どおり)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜14 は試験で通る。ただし 11 は差し込みでの検証、8 の「黙って取りこぼさない」は 1000 ファイルまでしか保証できない (notify の限界)。探り方式の取りやめは仕様の指示どおり
  正しさ: 4 — 6 回続けて通過、書き手の失敗 0 件。ただし書き手の見分けは「サイズが変わるか」の経験則で、間隔が長い書き手は途中の内容が complete で残る (最終の内容は必ず続く)
  コード品質: 4 — clippy 0 件、層を分けた。collector / watch は長め。ベースラインの除外パスは DB から戻すと大きさだけで比べる
  テスト: 4 — 受け入れ条件ごとの試験と作業ツリーの不変の検査あり。時間依存の試験は上限付きの待ち合わせ。Windows の負荷下では経験則の試験が揺れうる
不安・質問:
  1. [planner 判断] notify 8.2.2 は本物のあふれを報告しない (実機で再現)。受け入れ条件 8 の「あふれた区間が watcher_overflow で残る」は、本物のあふれでは満たせない。対策の選択肢: (a) このまま (差し込みで経路だけ検証) とし制限を文書化済みとする、(b) 大量のイベントの後に検証走査を足す (例: 1 回の flush が一定件数を超えたら、ルート全体を突き合わせて取りこぼしを gap で拾う)、(c) ReadDirectoryChangesW を自前で呼ぶ。どれにするか決めてほしい (別サブを推奨)。
  2. [planner 判断] 記録のスループットが低い見込み: 32000 ファイルの一括作成の試験 (デバッグビルド、他の負荷あり) の記録に十数分かかった (約 40 件/秒)。原因は未計測の推測だが、Blob の書き込みごとの `sync_all` と 1 件ごとのハッシュ + 読み直し。1000 ファイルは数十秒。数万ファイルの初期化とあわせて、計測する別サブが要るか。
  3. 1 回の保存の記録が debounce + 100 ms 後になる (settle)。通知の遅延が気になるなら settle_wait / settle_samples を調整できる (TrackerConfig)。
  4. 除外パス (`.env` など) は、DB から前回の状態を戻したとき内容のハッシュが無いので、サイズだけで比べる。サイズが同じ書き換えは、停止中の変更として拾えない。DB に除外ファイルのハッシュを残さない方針 (db-schema §4) との引き換えで、planner の判断が要るなら。
触ったファイル:
  tools/track/Cargo.toml、tools/track/Cargo.lock、tools/track/src/{baseline,collector,content,ignore,observe,watch}.rs (新規)、tools/track/src/{lib,main,startup,blob}.rs、tools/track/tests/watch.rs (新規)、tools/track/tests/project_id_cases.tsv (CRLF→LF のみ。git の差分なし)、docs/ai-change-tracking/{architecture,db-schema,prototype-results}.md、plans/m104-ai-change-tracking.md
申し送り:
  - sub-04: `observed` の payload と `window` を使って、派生の表の作り直しと「削除 + 同じハッシュの作成」のまとめを作る。同じ窓の判定は payload の `window` が同じ。`renamed` は同一ディレクトリ内だけ。`changes` はパスごとの `active` を再利用し、Revision の seq を足す。Hunk は `change_revisions` の行の後から足せる。
  - sub-05: `Tracker::status()` が `baseline_files` / `baseline_ms` / `baseline_bytes` / `first_run` / `overflows` / `phase` を返す。`serve` の終了は標準入力の EOF でも起こるので、DETACHED_PROCESS で起動する場合は標準入力が無効ハンドルで即 EOF になりうる (Rust の標準入力は無効ハンドルを EOF として読む)。pipe 経由の終了へ置き換えるときに外すこと。
  - 試験は debug ビルドで並列に走ると 40 秒前後かかる。CPU が詰まると書き手待ちの試験の間隔が足りなくなりうる (250 ms × 3 回にしてある)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-14)。planner が `cargo test` を実走。全体を 1 回 + `--test watch` を 4 回のうち、**全体の 1 回で `changes_while_service_was_down_are_gap_unknown` が `tests\watch.rs:412` (`assert_eq!(a[1].completeness, "complete")`) で落ちた** (他の 4 回は 18 件通過)。受け入れ条件 9 (不安定にならない) を満たしていない。
  - must 1: 1 回だけの `fs::write` が、負荷の下で `coalesced` になる。原因は試験ではなく判定 (「イベントが 5 ms 超で 2 つ以上の塊」) が実時間に依存していることと見る (`fs::write` 1 回は `Modify` 2 件で、負荷があると 5 ms 以上離れる)。`coalesced` は利用者に「中間状態を見ていない」と見せる印なので、ビルド中など負荷のある場面で普通の保存に付き続けると印の意味が無くなる。受け入れ条件 15 を足した。直し方は coder が決めてよい (しきい値を広げる、切り詰め + 書き込みの 2 件を 1 回と数える、など)。直した後、全体の `cargo test` を 10 回続けて通すこと。
  - 逸脱 (探りの取りやめ): 採る。sub-03.md の指示どおりで、438 件の失敗という実測がある。
  - 追加 (サイズが落ち着くまで待つ、`coalesced` の 3 条件のうち ②③、同じディレクトリ内の `renamed`、`window`、events の kind、試験用の差し込み口、固定の無視名を深さを問わず適用、tsv の改行): 採る。試験用の `inject_event` / `drop_incoming_events` は公開のまま残すなら `#[doc(hidden)]` を付け、製品のコードから呼ばないこと (should)。
  - 未実装 2 件 (本物のあふれの検出、記録に失敗した観測の再試行): このサブでは求めない。新しい sub-21 (M104d2) に移した。
  - 質問への回答: (1) (b) の検証走査を採る。sub-21 で行う (spec §2 D27)。(c) の自前の `ReadDirectoryChangesW` は、(b) で足りないと実測で分かったときの次の手。(2) 計測は sub-21 で行う (spec §2 D28)。(3) 保存の記録が debounce + 100 ms 遅れるのは許容。(4) 除外パスをサイズだけで比べる制限は許容 (文書にあること)。
- round 2: VERDICT OK (planner、2026-11-14)。planner が全体の `cargo test` を 4 回続けて実走し、毎回 lib 14 / project_id 3 / storage 16 / watch 19 が通過 (coder の 10 回と合わせて失敗なし)。`DEFAULT_BURST_GAP` = 200 ms (`collector.rs` 27 行目)、差し込み口 2 つに `#[doc(hidden)]` を確認。残る限界 (200 ms を超えて詰まると普通の保存にも `coalesced` が付く / 書き込みの間隔が 100 ms より長い書き手の途中の内容が `complete` で残りうる / 本物のあふれは報告されない) は文書にあり、あふれは sub-21 で扱う。nit: 実装メモの round 2 が未追記。
