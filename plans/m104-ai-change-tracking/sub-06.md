# sub-06: セッション、操作、帰属 (M104g)

- 依存: sub-05
- 状態: OK (M104g のコミット)
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

19. (round 1 で追加、**round 2 の VERDICT で書き直し**。spec §2 D34) round 2 の調査で、落ちた回は本物の取りこぼしだったと分かった (`notify` の 16 KiB の読み取りバッファが、読む側が CPU を取れないとあふれる。sub-05 の版でも再現。落ちた回の `gap` は連番のファイル 54〜130 件で、処理待ちは 0)。検証の走査が `gap` として拾ったのは設計どおり (D27) で、**試験の前提 (「2500 件では取りこぼしは起きない」) の方が誤り**だった。試験を 2 つに分け直す:
    - (a) **決定的な試験**: 「処理待ちのイベントを取りこぼしと取り違えない」は、取りこぼしが起きえない件数 (数十ファイル) で、記録を人為的に遅らせて (既存の試験用の口。足りなければ `#[doc(hidden)]` の口を足してよい) 処理待ちを溜めた状態で検証の走査を走らせ、`gap` が 0 件であることを確かめる。sub-21 で入れた修正 (`drain_events`) を外すと必ず落ちること。
    - (b) **大量の試験** (今の 2500 件): 取りこぼしが起きても起きなくても通る形にする。全ファイルが「通常の Revision」か「`gap` の Revision」のどちらか 1 つとして記録されている (記録なし 0、二重 0)、`gap` があるときは `coverage_gaps` に `watcher_overflow` の区間がある、無いときは区間も無い、最後の Revision の After が実際の内容と一致する。
    - `cargo test --test reconcile` を 20 回、全体の `cargo test` を 10 回続けて通す。20. (同) `worktree` の条件 (b) は、`.git` のファイルの `gitdir:` が指す先が `worktrees` ディレクトリの下にあるときだけ成り立つ (サブモジュールの `.git` ファイルは `modules` の下を指すので成り立たない) → `cargo test`。

21. (round 2 の VERDICT で追加。spec §2 D35) **取りこぼしを拾った Revision (`source = verify_gap`) の帰属**: 止まっていた間の `gap` (起動時の再走査) とは分けて扱う。
    - 起動時の再走査が作った `gap` は今までどおり `preexisting` で `unknown` (止まっていた間は見ていない。attr §4)。
    - `verify_gap` (サービスが動いている間の取りこぼし) は `preexisting` にしない。ただし `observed_at` は走査の時刻で、書かれた時刻ではないので、**時刻による根拠 (`op_window` だけ、`time_near`) は作らない。内容による根拠だけを数える**: `before_match` + `declared_edit_match` がそろえば `verified` (内容が宣言と一致しているという §3.1 の理由がそのまま成り立つ)、`report_hash_match` なら `claimed`。どちらも無ければ `unknown`。`overlap` の規則はそのまま。
    - `op_end` の即時 flush が「変更なし」だったとき (監視の取りこぼしを検証の走査が先に拾っていた)、その操作の対象パスの、`op_begin` より後の `verify_gap` の Revision を見直しの対象にする。
    - 試験: 取りこぼしを人為的に起こし (`drop_incoming_events`)、`op_begin` (宣言つき) → 書く → 検証の走査が `gap` を作る → `op_end`、で `ai` / `verified` になる。宣言と違う内容なら `verified` にならない。宣言なし・`report` なしの `verify_gap` は、running のセッションがあっても `unknown` (`inferred` にならない)。起動時の `gap` は宣言と一致しても `unknown` のまま → `cargo test`。attr §4 と §2.1 を直す。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (5 回)
- `tools\build_track.bat Release`、`tools\track_verify.bat` (3 回)
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git diff --stat ca6a3991 -- src` (空であること。空なら selftest / replay_verify は回さなくてよい。回していないことを書く)

## 実装メモ (coder が追記)

### round 1 (coder)

実装: `tools\track` の中だけ (`src\` は未変更)。新規 `attribution.rs` (根拠 12 種・`conclude`・計算し直し)、`session.rs` (派生の反映・寿命・`record_and_apply`)、`operation.rs`、`declared_edit.rs`、`gitref.rs`、`random.rs`、`ops_record.rs` (5 つの op)。既存の変更: `observe.rs` (`Source::ManagedBegin` / `Managed`、Revision の直後に帰属)、`revision.rs` (`Deriver` が新しいイベントを反映、`rebuild_derived` が `sessions` / `operations` / `attribution_evidence` も作り直す)、`collector.rs` (`observe_managed`)、`watch.rs` (`FlushHandle`)、`server.rs` (配線・寿命の定期確認・無接続の時間切れの条件)、`startup.rs` (整理をイベント経由に)、`ops.rs` (`sessionId` の絞り込みと `describe` の根拠)、`protocol.rs`。

検証: `cargo test` を 5 回続けて全部通過 (lib 18 + attribution 22 + diff 21 + pipe 7 + project_id 3 + reconcile 9 + revision 13 + session 23 + storage 18 + watch 19)。`tools\build_track.bat Release` → `tools\track_verify.bat` を 3 回続けて 15 シナリオ全部 PASS。終了後に `mye_track.exe` は残らない。`check_rules.ps1` は 0 error (warning 61 は既存)。`git diff --stat ca6a3991 -- src` は空。selftest / replay_verify は `src\` を触らないので回していない。変異で確認: `managed_begin` の除外と `worktree` の「1 つだけ」を外すと該当テストが落ちる。

設計・仕様との差分 (planner への報告):
- [追加] `session_end` / `op_begin` / `report` も `clientSessionId` で引ける (ipc は `sessionId` のみ)。静的な NDJSON シナリオが ID を持ち回れないため。`beforeHashes` / `afterHashes` は配列に加えてパスをキーにしたオブジェクトも受ける。
- [追加] `Source::ManagedBegin` (`managed_begin`) と `Source::Managed` (`managed`)。`op_begin` の直前の読み取りを `time_near` から外すため、`observed` の `source` を根拠の `detail` に残して計算し直しでも引き継ぐ。
- [追加] 起動時の整理 (db-schema §6 の 5、6) を、表の直接の書き換えからイベント (`session_interrupted` / `op_abandoned`) 経由に変えた。作り直しで同じ状態になるために必要。副作用: 放棄した操作の `finished_at` は NULL ではなく整理した時刻になる (db-schema §6 を更新)。2 つの段は 1 つの「残った session の整理」に統合 (status の `failedSteps` の名前は session 側が残る)。
- [追加] `preexisting` を 2 種にした: セッションに属さない (`gap`) は表の 3 行目として他の根拠に勝つ。セッションに付く (開始より前の変更を申告した、開始のすぐ前だった) は、そのセッションの根拠だけを結論から外す。他のセッションの正当な根拠を、無関係なセッションの `report` が潰さないため。
- [追加] `report` の対象: 申告から 10 秒以内に観測された Revision と、申告の時点でそのパスの最後の Revision (10 分まで遡る) に `report_only` を付ける (前の変更まで AI のものにしない)。`report_hash_match` は同じ窓の中のセッション開始後の Revision。窓の定数は `attribution.rs`。`report` の結果は、ハッシュを渡していない場合を `mismatch` にする (ipc の 3 値に収めた)。
- [追加] `list` の `sessionId` は「結論の操作がそのセッション」または「操作が無く、根拠 (`preexisting` / `observed` / `overlap` 以外) にそのセッションがある」。スキーマ v1 に候補の列が無く、移行 v2 は足していない。
- [追加] パスの突き合わせは ASCII の大文字小文字を無視する (Windows)。`op_begin` の `operationType` に `report` も受ける (ipc の列挙どおり。窓は持たない)。
- スキーマは v1 のまま。移行 v2 は足していない。`tests\revision.rs` の手動削除に `DELETE FROM attribution_evidence` を足した (新しい派生の表への外部キー)。
- 無接続の時間切れ (sub-05 の nit 1): 「ベースライン・再走査の最中」は `phase` が starting / baselining、または `scan_done < scan_total`。running のセッションの有無は発火の判断のときだけ読み取り接続で引く。試験は pipe 越し (`the_service_does_not_idle_out_while_a_session_is_running`)。
- sub-05 の nit 2 (busy): pipe 越しではなく、`RecordEnv.queue_limit = 0` で `busy` を返す試験にした (`recording_ops_answer_busy_...`)。pipe 越しに Writer を詰まらせる試験は足していない。
- シナリオ: `tests\track` に 12 (管理された編集)・13 (役割と引数)・14 (report)・15 (再起動と再接続) を足し、03 / 09 のコメントと期待、06 / 11 の期待 (`describe` に `evidence` / `candidateSessions`) を更新。`track_verify.ps1` の正規化に UUID・プロジェクトのパス (`<ROOT>`)・観測の出どころ (`<SRC>`) を足した。

不安・質問:
- `time_near` の計算し直し: `op_begin` / `session_begin` の ±2 秒にある既存の Revision (別のパス、人の変更を含む) を再評価するので、AI の活動の直後に人が別のファイルを保存すると `inferred` になりうる (仕様どおり。誤りではなく弱い根拠)。`op_begin` 前の読み取りが作る Revision と、セッションの開始より前の Revision は外している。
- `.git` がファイルで `gitdir:` の行を持つ、という構造確認はサブモジュールも通る (spec D33 の字面どおり)。厳しくするなら `gitdir` の親が `worktrees` であることまで見る。
- 記録の op は `observed` の根拠を Revision ごとに 1 行足すので、記録 1 件あたりの DB の書き込みが増える。`mye_track bench` で再測定していない (sub-21 の計測値は未更新)。
- 最低 Rust 1.89 での実ビルドは未確認 (開発機は 1.98)。CI での実走も未確認。

### round 2 (coder)

- #2 (worktree): `gitref.rs` の `is_linked_worktree` を、`gitdir:` の指す先の親ディレクトリ名が `worktrees` のときだけ真にした。サブモジュール (`modules` の下) は偽。`teststtribution.rs` に試験を足し、`tests\session.rs` の偽の作業ツリーを `...\worktrees\wt` を指す形にした。attribution-rules.md §6.2 も更新。
- #3 (性能): `mye_track bench` (release、生成 32000 ファイル、2026-11-15): ベースライン 48 秒 (sub-21 の別の時間帯の値は 71〜103 秒)、1 件の変更 468〜470 ms (sub-21 は 422〜474 ms)、一括 1000 件 2869 ms (sub-21 は 3723 ms)。機械の状態が違うので絶対値の比較はできないが、悪化は見えない。32000 件の一括は回していない。
- #1 (取りこぼし): 原因は notify 8 の Windows 版の読み取りバッファ (16 KiB 固定。`notify-8.2.2\src\windows.rs` の `BUF_SIZE`) で、読む側が CPU を取れないとあふれてイベントを黙って失う (あふれの報告も無い)。再現: 24 個の CPU を食うプロセスを走らせたまま `reconcile` の該当試験を 4 本同時に起動 (全体で 12 論理コア)。sub-06 の版で 3 ラウンドのうち 3 回、HEAD (sub-05、`14ddde31`) の版でも 1 回落ちた = sub-06 が入れた不具合ではなく、既存の取りこぼしが負荷で表に出る。落ちた回の様子: 検証の走査の候補がファイル連番 (`bulk/f1418.txt` から約 130 件) で、`pending` が 0、最後のイベントから 2.4 秒経っている = 遅れて届くのではなく届いていない。そのファイルの Revision は `verify_gap` の `gap` で、帰属は `preexisting` により unknown 固定。
  - 製品側の対処として入れたもの: セッションが 1 つも無いときの帰属の計算を省いた (`attribution.rs` の `evaluate`。Revision ごとの根拠の読み直しと結論の UPDATE を省く)。記録の経路の CPU を元に近づけるため。
  - 自前の `ReadDirectoryChangesW` の監視 (1 MiB のバッファ、あふれを検出) への置き換えを試作したが、バースト直後の最初の 3〜4 件のイベントが届かない現象が残り (原因は未特定。依頼を 2 本重ねても同じ)、採らずに戻した。コードは残していない。
  - 結果 (負荷なし、コミット前の作業ツリー): `reconcile` を 20 回 → 1 回失敗 (同じ試験、`gap` が 54 件)。全体の `cargo test` を 10 回 → 全部通過。受け入れ条件 19 は満たせていない (20 回全通過ではない)。判断を planner へ (不安・質問)。

### round 3 (coder)

- 条件 21 (`verify_gap` の帰属): `attribution.rs` の `evaluate` が `source = verify_gap` の `gap` を `preexisting` にせず、内容だけの根拠にする (`session_evidence` の `content_only`)。`op_window` / `overlap` / `worktree` / `time_near` / `report_only` は作らず、窓の終わりを問わず (`op_begin` より後の操作で) `before_match` + `declared_edit_match`、申告は `report_hash_match` だけを数える。起動時・あふれの `gap` は従来どおり `preexisting` で `unknown`。`op_end` の見直しは既存の範囲指定 (`op_begin` から `op_end` + 猶予) が `verify_gap` の Revision も含むので、追加のコードは無い。試験 (`tests\session.rs`): 宣言と一致 → `ai` / `verified` (`op_window` も `preexisting` も無い、`op_end` の結果に Revision が入る)、宣言と違う → `unknown`、宣言なし + running のセッション → `unknown` で根拠は `observed` だけ (既存の gap の試験を直した)、起動時の `gap` は宣言と一致しても `unknown` + `preexisting`。attribution-rules.md §2.1 / §4 を更新。
- 条件 19 (試験の分け直し): `testseconcile.rs`。(a) `backlog_of_unprocessed_events_is_not_mistaken_for_a_miss` を決定的にした: Writer を 1.5 秒塞いで flush を止め、その間にルート直下へ 20 ファイルを書き、イベント 1 件の処理を 40 ms に遅らせる (`Tracker::delay_event_handling`、`#[doc(hidden)]` の試験用の口を足した)。`gap` 0 件・Revision 21 件ちょうどを確かめる。確認: `maintenance` と `verify` の末尾の `drain_events` を**両方**外すと 3 回中 3 回落ちる (N=40 の版、N=20 でも 2 回中 2 回)。片方だけ外すと通る (もう片方が受ける。冗長な 2 重の防御)。(b) 2500 件の試験は `bulk_writes_are_each_recorded_once_as_a_normal_or_a_gap_revision` に改名し、全ファイルがちょうど 1 つの Revision、`gap` があれば `watcher_overflow` の区間がある (無ければ無い)、After が実際の内容と一致、の形にした。途中で分かったこと: 新しいディレクトリの下に書くと、ディレクトリ作成の flush が中のファイルをまとめて読むので、(a) はルート直下に書く。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-15)。planner が全体の `cargo test` を 4 回、`--test reconcile` を 6 回、`tools\track_verify.bat` を 1 回実走。**全体の 2 回目で `backlog_of_unprocessed_events_is_not_mistaken_for_a_miss` が `tests\reconcile.rs:257` (`gap` の Revision が 0 件であること) で落ちた** (他は全部通過。`track_verify` は 15 シナリオ PASS、残存プロセスなし)。sub-21 で入れた試験で、sub-21 から sub-05 までの planner の実走 (全体 6 回) では落ちていない。
  - must 1: 検証の走査が、記録の途中の変更を取りこぼし (`gap`) と取り違えることがある。`gap` の Revision は `preexisting` で `unknown` に固定されるので、このサブで入れた帰属と組み合わさると「宣言と一致した AI の変更が Unknown になる」。受け入れ条件 19 を足した。このサブで監視スレッドに足したもの (即時 flush の依頼、1 秒ごとの寿命の確認、Revision ごとの根拠の書き込みで記録が長くなったこと) が関係していないかを先に見ること。
  - should 2: `worktree` の条件 (b) をサブモジュールで成り立たなくする (受け入れ条件 20)。
  - should 3: Revision ごとに根拠の行が増えた分の記録のコストを、`mye_track bench changes` で 1 回測り、sub-21 の値 (`C:\HAL\MyEngin\docs\ai-change-tracking\performance.md` §4) の隣に書く。
  - 追加・逸脱の判定: `clientSessionId` で引ける op を増やした、`managed_begin` / `managed` の source、起動時の整理をイベント経由にした、`preexisting` を 2 種に分けた、`report` の対象の窓、`list` の `sessionId` の意味、パスの突き合わせで ASCII の大文字小文字を無視、は全部採る。pipe 越しの `busy` の試験が同じプロセスの試験になったのも可。
  - 質問への回答: (1) `time_near` の計算し直しの範囲はそのままでよい (弱い根拠で `inferred` 止まり)。(2) should 2 のとおり厳しくする。(3) `report` の窓 (10 秒先、10 分遡る) はそのままでよい。(4) should 3。(5) 定数のままでよい。
- round 2: VERDICT REWORK (planner、2026-11-15)。指摘 2 (サブモジュール) と 3 (計測) は済。指摘 1 は、原因の調査を受け入れる: 落ちた回は本物の取りこぼしで (`notify` 8.2.2 の `windows.rs` の `BUF_SIZE = 16384`、処理待ち 0、連番の 54〜130 件、sub-05 の版でも再現)、検証の走査が `gap` として拾ったのは設計どおり。**誤っていたのは試験の前提と、それを条件にした planner の受け入れ条件 19。** 条件 19 を書き直し、条件 21 を足した。coder の誤りではない。
  - 質問への回答: (1) 自前の `ReadDirectoryChangesW` への置き換えは、このサブではやらない。先に安い手 (無視するディレクトリを監視から外して、ビルドの出力などの洪水が追跡対象とバッファを取り合わないようにする) を新しい sub-22 で試し、足りなければ自前の監視を同じサブで続ける (spec §2 D34)。(2) `gap` の Revision に `verified` を許すか: 起動時の `gap` には許さない。`verify_gap` には、内容の一致があるときだけ許す (条件 21、spec §2 D35)。(3) sub-06 が悪化させたとは見ない。
  - `evaluate` の早期 return (セッションが 1 つも無いときは根拠の読み直しを省く) は採る。
- round 3: VERDICT OK (planner、2026-11-15)。planner が全体の `cargo test` を 3 回 (毎回 158 件: lib 18 / attribution 23 / diff 21 / pipe 7 / project_id 3 / reconcile 10 (+1 ignored) / revision 13 / session 26 / storage 18 / watch 19)、`--test reconcile` を 6 回、`tools\track_verify.bat` を 1 回実走し、全部通過。残存プロセスなし、スキーマの差分なし。
  - 条件 19: 決定的な試験 (a) と、取りこぼしの有無によらず通る (b) に分かれた。(a) は `drain_events` を 2 か所とも外すと落ちる、との変異の確認を受け入れる。
  - 条件 21: 4 通りの試験がある。`verify_gap` で `overlap` を作らない点は条件の文面 (「`overlap` の規則はそのまま」) と違うが、時刻の窓が使えない以上、重なりを時刻で判定できないので採る。内容の一致 (Before と After の両方) が 1 つの操作にだけ成り立つことが絞り込みになる。
  - 質問への回答: (1) 窓の終わりを問わない照合は、打ち切り前の操作 (最大 10 分) に限られ、Before・After の両方の一致が要るので、上限は足さない。(2) `drain_events` の 2 重の防御はそのままでよい。