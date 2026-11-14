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

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (5 回)
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
