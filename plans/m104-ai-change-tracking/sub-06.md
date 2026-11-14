# sub-06: セッション、操作、帰属 (M104g)

- 依存: sub-05
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-05 の VERDICT で planner が Phase 1 の実装に合わせて書き直した)

要求 Phase 2 の完了条件は「複数 AI や人間の編集が混在しても、根拠のない AI 帰属を確定しない」。正本は `C:\HAL\MyEngin\docs\ai-change-tracking\attribution-rules.md` (attr) と `ipc-contract.md` §4.2 (ipc)。この文書と食い違ったら、下の「設計文書から変える点」が優先で、このサブの中で設計文書を直す。

着手前のユーザー確認は無い。判断が要る点は planner が推奨で裁定してある (spec §2 D30〜D33)。

## 設計文書から変える点 (planner の裁定。このサブで attr / ipc を直す)

1. **セッションの基準は「全ファイルのハッシュの写し」ではなく、`session_begin` の時刻とジャーナルの位置** (spec §2 D30)。サービスは常時記録しているので、基準より後の変更 = `observed_at >= started_at` の Revision で決まる。写しを別に持たない。`preexisting` を付けるのは、(a) `observed_at` がセッションの開始より前の Revision (b) `completeness = gap` の Revision (止まっていた間、または取りこぼし。その間にセッションが動いていても `unknown`。attr §4 の最後の項)。`session_begin` がベースライン中・起動時の再走査中に来たら、走査が終わるまで待たずに受け付け、走査が作る Revision は `gap` なので (b) で落ちる。
2. **管理された操作は、監視を待たずに自分で読む** (spec §2 D31。要求 §5.2 の順序)。
   - `op_begin`: 対象パスを先に flush する (`Collector::observe_paths`。未記録の人の変更があれば、それを先に独立した Revision にする)。その後の内容が、この操作の Before。
   - `op_end`: 対象パスをすぐ flush し、できた Revision をこの操作に結び付ける。デバウンスを待たない。これで `coalesced` にならず、Before / After が操作の前後と正確に一致する。
   - `report`: 照合の前に、申告されたパスを flush する。
   - flush は上限つき (全体で 2 秒)。間に合わなければ `pending: true` で返し、帰属は後から付く (下の 5)。**記録の op は AI を待たせない** (ipc §6 の 5 秒の内側)。
3. **フックは pre と post が別のプロセス**なので、`op_end` は `operationId` の代わりに `clientOpId` (Claude Code の `tool_use_id`) と `clientSessionId` で操作を引けるようにする (spec §2 D32)。`op_begin` は `clientOpId?` を受けて覚える。`session_begin` は同じ `clientSessionId` の running / interrupted のセッションがあれば、新しく作らずそれを返す (interrupted なら running に戻し、再接続のイベントを残す)。
4. **`worktree` の根拠は、構造で確かめられるときだけ** (spec §2 D33)。attr §6.2 のままだと、`workspace` がプロジェクトのルート (普通の使い方) でセッションが 1 つなら、人の編集まで Verified になる。条件を全部満たすときだけ付ける: (a) `session_begin` が `isolated: true` を申告 (b) サービスのプロジェクトルートが git のリンクされた作業ツリー (`.git` がディレクトリではなくファイルで、`gitdir:` の行を持つ。読むだけで git は起動しない) (c) その `workspace` を登録している running のセッションが 1 つ (d) `role = editor` の接続が無い。1 つでも欠ければ付けない (Claimed 以下に落ちる)。
5. **帰属は後から変わる。** 根拠は Revision の記録より後に届くことがある (監視の方が `op_end` より遅い、`report` が後から来る)。根拠が増えるたびに、その Revision の結論を計算し直す。通知 (`attribution_updated`) は `subscribe` を作る sub-09 で。

## やること

### A. プロトコルと op (ipc §4.2)

`tools\track\src\protocol.rs` の `IMPLEMENTED_OPS` と `role_allows` に足す。役割は mcp と hook (`report` は mcp だけ)。知らない引数は `bad_args`。記録を伴う op は `submit_bounded` を通し、あふれたら `busy` (sub-05 の `hello` の証跡が見本)。

| op | 引数 | 結果 |
|---|---|---|
| `session_begin` | `provider` (claude / codex / gemini / other)、`workspace`、`source` (terminal / mcp / external)、`clientSessionId?`、`isolated?` | `sessionId`、`status`、`resumed` (既存を返したか)、`trackingCapabilities` (このセッションで出せる根拠の種類)、`baseCommit` |
| `session_end` | `sessionId` または `clientSessionId` | `status`、`changeCount` |
| `op_begin` | `sessionId` または `clientSessionId`、`operationType` (edit / write / delete / rename / report / other)、`targets` (パスの配列)、`declaredEdit?`、`clientOpId?` | `operationId`、各パスの現在のハッシュ (無ければ null) |
| `op_end` | `operationId` または (`clientSessionId` + `clientOpId`)、`result` (ok / failed) | 結び付いた Revision の ID と帰属、`pending` |
| `report` | `sessionId`、`operationId?`、`paths`、`beforeHashes?`、`afterHashes?` | パスごとの照合結果 (`matched` / `mismatch` / `not_observed`) と帰属 |

- `workspace` と `targets` / `paths` はサービス側で正規化し、プロジェクトルートの外を指すものは `bad_args` (クライアントの言うパスを信用しない。要求 §3.3)。相対パスは `workspace` 基準ではなくプロジェクトルート基準で受け、絶対パスも受ける。
- `sessionId` は OS の乱数 (sub-05 の `random_token` と同じ `BCryptGenRandom`) から作る UUID v4 の形。`operationId` も同じ。
- `baseCommit`: `.git\HEAD` → 参照ファイル → `packed-refs` の順に読む。detached HEAD はそのままのハッシュ。リンクされた作業ツリー (`.git` がファイル) は `gitdir:` と `commondir` をたどる。git でなければ空。**git を起動しない。** 表示のための情報で、判定に使わない (attr §4)。
- `declaredEdit` の形: `{"kind":"replace","oldString":..,"newString":..,"replaceAll":bool}` か `{"kind":"write","content":..}`。対象は `targets` の 1 つ目だけ。大きさは 1 行の上限 (1 MiB) に収まる分だけで、超える宣言は hook の側で送らない (sub-08)。
- プロセスの識別: `mcp` の接続は、pipe から得た PID とプロセス開始時刻をセッションに持つ (`sessions.process_id` / `process_start`)。`hook` の接続は短命なので持たない (NULL)。

### B. 記録の形 (D26 の原則を延ばす)

- **セッションと操作の op は全部 `Store::record_event` を通す** (kind: `session_begin` / `session_end` / `session_resumed` / `session_interrupted` / `op_begin` / `op_end` / `op_abandoned` / `reported`)。`sessions` / `operations` / `attribution_evidence` と `change_revisions.actor` / `attribution` / `operation_id` は、イベントから導ける派生の表。**`Deriver` (`revision.rs`) を延ばし、通常の経路と `rebuild_derived` が同じ関数を通る。** イベントに無い情報を派生の表だけに書かない。
- `declaredEdit` の中身はイベントのペイロードに直接入れず、Blob に入れてハッシュを書く (ジャーナルに内容を書かない。db §5)。`snapshots.kind = declared_edit`、`operations.declared_edit` が指す。除外パターンのパスへの宣言は保存しない。
- 宣言を当てた結果のハッシュ (`declaredAfterHash`)、`clientOpId`、`op_begin` 時点の各パスのハッシュは `operations.reported_targets` の JSON に入れる。セッションの基準 (時刻とジャーナルの位置)、`isolated` は `sessions.coverage` の JSON。**`schema\v1.sql` は変えない** (sub-05 で凍結)。列が足りなければ移行 v2 を足し、理由を SELF_EVAL に書く。
- `events` のペイロードの形を db §5.1 に足す。

### C. 宣言の適用 (`declared_edit.rs`)

sub-01 の試作 5 の規則 (attr §6.1 に書いてある) をそのまま実装する。純粋な関数にする。

- replace: `oldString` を Before の中で数え、0 件なら「当てられない」、2 件以上で `replaceAll` が偽なら「当てられない」、それ以外は置換。`replaceAll` が真なら全部置換 (0 件は当てられない)。空の `oldString` は当てられない。行末は変換しない。バイト列として扱う (Before が UTF-8 でなければ当てられない)。
- write: `content` のバイト列がそのまま After。
- 当てられないときは根拠を作らない (Verified にならないだけで、誤りではない)。

### D. 根拠と結論 (`attribution.rs`)

- 根拠の 12 種 (attr §2)。このサブで出すのは `observed`、`op_window`、`before_match`、`declared_edit_match`、`report_hash_match`、`report_only`、`worktree`、`time_near`、`overlap`、`preexisting`。`editor_saved` と `restore_expected` は結論の関数には入れておく (表の 1、2 行目) が、根拠を作る op は sub-09 / sub-15。
- 各根拠の定義 (あいまいさを残さない):
  - `op_window`: Revision の `observed_at` (監視のイベントの時刻。記録した時刻ではない) が、その操作の `op_begin` から `op_end` + 猶予 (既定 2 秒、定数) の間にあり、パスが `targets` に入っている。`op_end` の即時 flush が作った Revision は無条件で入る。
  - `before_match`: Revision の Before のハッシュが、`op_begin` で取ったそのパスのハッシュと同じ。
  - `declared_edit_match`: Revision の After のハッシュが `declaredAfterHash` と同じ、かつ `before_match` が成り立つ。
  - `report_hash_match`: 申告の Before / After のハッシュが両方あり、セッション開始より後のそのパスの Revision のどれかと両方一致。片方だけ・不一致・Revision なしは `report_only`。
  - `time_near`: 申告も窓も無い Revision の `observed_at` が、running のセッションの活動時刻 (`session_begin` / `op_begin` / `op_end` / `report` を受けた時刻) のどれかから 2 秒以内 (定数、設定可)。
  - `overlap`: 同じパスについて、Revision の `observed_at` を含む窓を持つ操作が、別々のセッションに 2 つ以上ある。
- 結論は attr §3 の表を**上から順に**当てる純粋な関数 `conclude(根拠の集合) -> (actor, attribution, 候補のセッション)`。§5 の規則 (窓が重なっている間は Verified にしない。一方にだけ `declared_edit_match` があれば候補はそのセッション 1 つで Claimed、どちらにも無ければ `ai` / `unknown` で候補は両方) を含める。
- 根拠は消さない。足すたびに `conclude` をやり直し、`change_revisions` の `actor` / `attribution` / `operation_id` を更新する (`operation_id` は、候補が 1 つの操作に絞れたときだけ)。
- `describe` に根拠の一覧と候補のセッションを足す。`list` の `sessionId` / `attribution` フィルタと `sessions` が実データで働く (sub-05 で配線済み)。`session_end` の `changeCount` は、そのセッションが候補に入っている Revision の数。

### E. セッションと操作の寿命

- `mcp` のセッション: 持っている PID + 開始時刻のプロセスが居なくなったら `interrupted` (定期的な確認。sub-02 の `process.rs`)。接続が切れただけでは変えない (MCP は要求ごとに接続する)。
- `hook` のセッション: プロセスを持たないので、最後の活動から 30 分 (定数、設定可) で `interrupted`。同じ `clientSessionId` の `session_begin` / `op_begin` が来たら running に戻す。
- 操作: `op_end` が来ないまま 10 分 (定数) で `abandoned`。その間に窓へ入った Revision の根拠は残る (`op_window` は `op_begin` から打ち切りまで)。
- 起動時の整理 (sub-02 の `startup.rs`) はそのまま。
- **無接続の時間切れ (sub-05) は、ベースライン・再走査の最中と、running のセッションがある間は発火させない** (sub-05 の「不安・質問」: 巨大なプロジェクトでベースラインの途中に終わりうる。セッションの途中でサービスが消えると、その間が `service_down` の欠落になる)。

### F. コードの置き方

- `server.rs` が約 680 行ある。op の処理を `server.rs` に足し続けず、`ops.rs` を読み取り (既存) と記録 (新規。例 `ops_record.rs` か `ops\` ディレクトリ) に分ける。分け方は coder が決めてよい。
- 新規: `session.rs`、`operation.rs`、`attribution.rs`、`declared_edit.rs`、`gitref.rs` (`.git` を読むだけ)、`tests\attribution.rs`、`tests\session.rs`、`C:\HAL\MyEngin\tests\track\` にシナリオ。

## やらないこと (このサブでは)

- MCP 側 (sub-07)、フック CLI (sub-08)。このサブの試験は pipe の op を直接叩く (`mye_track client` と `tests\pipe.rs` の形)。
- `subscribe` と通知 (sub-09)。`editor_saved` / `restore_expect` の op (sub-09 / sub-15)。
- 作業ツリーの作成 (attr §6.2: サービスは作らない)。git の起動。
- 作業ツリーへの書き込み (全サブ共通で禁止。sub-03 の後処理の検査をこのサブの試験にも使う)。

## 受け入れ条件 (このサブ)

1. `conclude` について、attr §3 の表の 9 行それぞれに、その行が採られる試験がある。上の行が下の行より優先されることを、2 つの行が同時に成り立つ入力で確かめる試験がある → `cargo test` (単体)。
2. **人の未コミットの変更を混ぜない**: ファイル A を人が変えて (記録される) から `session_begin` し、そのセッションが B を操作する。A の Revision はそのセッションの候補に入らず、`list` の `sessionId` フィルタに出ない。git の HEAD との差を読むコードが判定の経路に無い → `cargo test` (pipe 越し) (要求 §16.1 の 3 番目)。
3. **宣言と一致すれば Verified**: `op_begin` (replace の宣言) → 宣言どおりに書く → `op_end` で、その Revision が `ai` / `verified`、根拠に `declared_edit_match`・`before_match`・`op_window`、`operation_id` が付く。`op_end` の結果に Revision の ID が入っている (`pending` が偽)。Revision は `complete` で、Before / After が操作の前後とバイト一致 → `cargo test` + `tools\track_verify.bat`。
4. **人が挟まれば Verified にならない**: (a) `op_begin` と `op_end` の間に、宣言と違う内容を書く → `claimed` (b) `op_begin` の直前に人が書いた未記録の変更は、`op_begin` の flush で別の Revision (`unknown`) になり、AI の Revision に混ざらない (c) 宣言どおりの編集の後、`op_end` の前に人がさらに書く → After が宣言と一致しないので Verified にならない → `cargo test`。
5. **窓の重なり**: 同じパスに 2 セッションの操作の窓が重なると `overlap` が付く。一方にだけ `declared_edit_match` → 候補はそのセッション 1 つで `claimed`。どちらにも無い → `ai` / `unknown` で候補が 2 つ。どの場合も `verified` にならない → `cargo test` (§16.2 の 5 番目)。
6. **後から届く根拠**: 先に Revision が記録され (監視の経路)、後から `report` が届くと、その Revision の結論が `unknown` → `claimed` に変わる。根拠の行は増えるだけで消えない → `cargo test`。
7. `report` の結果がパスごとに `matched` / `mismatch` / `not_observed` を返す。`report` だけでは `verified` にならない (要求 §4.3) → `tools\track_verify.bat`。
8. 申告も窓も無い変更は `unknown` / `unknown`。running のセッションの活動から 2 秒以内なら `inferred` で、幅の設定を広げても `inferred` より上にならない。`gap` の Revision は、その間にセッションが動いていても `unknown` → `cargo test` (§16.2 の 4 番目)。
9. **PID の再利用**: セッションの登録に (PID, 開始時刻) を渡す単体の試験で、同じ PID・違う開始時刻が別のプロセスとして扱われ、先のセッションが `interrupted` になる。pipe 越しでは、`mcp` の接続のセッションに pipe から得た PID が入り、申告の `pid` と違えば pipe の値が採られる → `cargo test` (§16.2 の 2 番目)。
10. **複数セッションの区別と再接続**: 2 つの `session_begin` が別の `sessionId` を返す。同じ `clientSessionId` の 2 回目は同じ `sessionId` を返し `resumed` が真。`hook` の役割で、別の接続から `clientSessionId` + `clientOpId` で `op_end` できる → `cargo test` + `tools\track_verify.bat` (§16.2 の 1 番目)。
11. **`worktree`**: 4 条件を全部満たす一時プロジェクト (`.git` をファイルにした偽のリンクされた作業ツリー) で、申告の無い変更が `ai` / `verified` になる。条件を 1 つずつ外した 4 通りのそれぞれで `verified` にならない。普通のプロジェクト (`.git` がディレクトリ) で `isolated: true` を申告しても `verified` にならない → `cargo test`。
12. 役割: `editor` の `session_begin` / `op_begin` / `report` と、`hook` の `report` が `forbidden`。プロジェクトの外のパスは `bad_args` → `tools\track_verify.bat`。
13. `baseCommit`: 通常の参照、`packed-refs` だけにある参照、detached HEAD、git でないディレクトリ、リンクされた作業ツリーの 5 通り。試験の間に git のプロセスが起動されない (コードに `Command` が無いことを grep で確かめる) → `cargo test`。
14. **作り直し**: セッション・操作・根拠を記録した後に `tracking.db` を壊して起動し直すと、`sessions` / `operations` / `attribution_evidence` と Revision の `actor` / `attribution` / `operation_id` が壊す前と同じになる (sub-04 の `a_broken_db_is_rebuilt_...` と同じ比べ方) → `cargo test`。
15. 寿命: `op_end` の来ない操作が打ち切りの時間 (試験では短く) で `abandoned`。`hook` のセッションが無活動で `interrupted` になり、同じ `clientSessionId` で戻る。running のセッションがある間、無接続の時間切れで終わらない → `cargo test`。
16. 記録の op が 5 秒以内に返る: 対象のファイルを別のプロセスが開いて書き続けている最中の `op_end` が、2 秒ほどで `pending: true` を返す → `cargo test`。
17. Phase 1 の試験とシナリオが全部通る。全体の `cargo test` を 5 回、`tools\track_verify.bat` を 3 回続けて通し、終わった後に `mye_track.exe` が残らない。作業ツリーを書かない検査がこのサブの試験でも成り立つ。
18. attr と ipc が、上の「設計文書から変える点」と実装で決めた定数 (猶予、打ち切り、`time_near` の幅) に合っている → 文書の差分。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (5 回)
- `tools\build_track.bat Release`、`tools\track_verify.bat` (3 回)
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git diff --stat ca6a3991 -- src` (空であること。空なら selftest / replay_verify は回さなくてよい。回していないことを書く)

## 実装メモ (coder が追記)

## フィードバック履歴
