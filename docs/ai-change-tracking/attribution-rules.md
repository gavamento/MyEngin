# AI Change Tracking: 変更の帰属ルール

- 状態: 設計のみ (2026-11-13)
- 全体像は [architecture.md](architecture.md)。値の保存先は [db-schema.md](db-schema.md) の `change_revisions` と `attribution_evidence`

## 1. 考え方

仕様書 §1.3 の 2 つの原則をそのまま規則にする。

- **検出と帰属を分ける。** 「ファイルが変わった」は監視が観測する事実。「誰が変えた」は別の根拠から決める。根拠が無くても変更は記録する。
- **根拠が足りなければ断定しない。** 迷ったら低い方の区分にする。区分は後から根拠が増えれば上がるが、推測で上げることはしない。

帰属は 2 つの軸で持つ。

| 軸 | 値 | 意味 |
|---|---|---|
| 行為者 (`actor`) | `ai` / `editor_user` / `restore` / `unknown` | どの種類の書き手か |
| 確かさ (`attribution`) | `verified` / `claimed` / `inferred` / `unknown` | その判断がどれだけ確かか |

`actor = ai` のとき、どのセッションかは `attribution_evidence` の候補から決まる。候補が 2 つ以上残るなら 1 つに絞らない (§5)。

## 2. 根拠の種類

| kind | 何が起きたか | 出どころ |
|---|---|---|
| `observed` | 監視がこのパスの変更を見た (時刻、Before / After のハッシュ) | サービス自身 |
| `op_window` | 変更の観測時刻が、ある操作の `op_begin` と `op_end` の間に入っている | フック、MCP |
| `before_match` | `op_begin` で取った Before と、変更の Before が同じ内容 | サービス自身 |
| `declared_edit_match` | 申告された編集を Before に当てた結果が、観測した After と同じ内容 | フックの申告 + サービスの照合 |
| `report_hash_match` | `change_report` の Before / After のハッシュが観測と一致 | MCP の申告 + サービスの照合 |
| `report_only` | `change_report` はあるが、観測と照合できない (ハッシュ不一致、観測なし) | MCP の申告 |
| `worktree` | 変更が、そのセッション専用の作業ツリーの中で起きた | セッションの登録情報 |
| `time_near` | 変更の時刻が、稼働中のセッションの活動時刻に近い | サービス自身 |
| `editor_saved` | Editor が「この内容で保存した」と通知し、観測した After と一致 | Editor |
| `restore_expected` | Editor が「復元でこの内容を書く」と予告し (`restore_expect`。書き込みの前に届く)、観測した After と一致 (予告の時刻の 10 秒前から 120 秒後までの観測) | Editor |
| `overlap` | 同じパスに、別のセッションの操作の窓が重なっている | サービス自身 |
| `preexisting` | この変更はセッションの開始より前から作業ツリーにあった | サービス自身 (ベースライン) |

### 2.1 各根拠の定義 (M104g の実装)

正本は `tools\track\src\attribution.rs`。時刻はすべて、監視のイベントの時刻 (`observed_at`。記録した時刻ではない)。

| kind | 定義 |
|---|---|
| `op_window` | Revision の `observed_at` が、その操作の `op_begin` から `op_end` + 猶予 (既定 2 秒) の間にあり、Revision のパス (リネームなら旧パスも) が操作の `targets` に入っている。`op_end` が来ていない操作は、打ち切り (既定 10 分) まで開いている |
| `before_match` | Revision の Before のハッシュが、`op_begin` で取ったそのパスのハッシュと同じ (新規ファイル = 無い、同士も一致)。除外パターンのファイルと、Before を見ていない変更 (`no_before`) は照合に使えない |
| `declared_edit_match` | `op_begin` の `declaredEdit` を、その時点の内容 (`targets` の 1 つ目) に当てた結果のハッシュが、Revision の After と同じ。かつ `before_match` が成り立つ |
| `report_hash_match` | `report` が Before / After の**両方**のハッシュを渡し、セッション開始より後のそのパスの Revision のどれかと両方一致 |
| `report_only` | 上以外 (片方だけ・不一致・ハッシュなし)。申告の時点でそのパスの最後の Revision (と、申告から 10 秒以内に観測された Revision) にだけ付ける。申告が先に届いて Revision が後から来ても、10 秒以内なら付く |
| `time_near` | 申告も窓も無い Revision の `observed_at` が、running のセッションの活動時刻 (`session_begin` / `op_begin` / `op_end` / `report` を受けた時刻) のどれかから 2 秒以内 (定数、設定可)。操作の直前の読み取り (`op_begin` が先に確定する、人の未記録の変更) には付けない |
| `overlap` | 同じパスについて、Revision の `observed_at` を含む窓を持つ操作が、別々のセッションに 2 つ以上ある |
| `preexisting` | (a) セッションの開始より前の Revision に、そのセッションが申告した、または開始のすぐ前 (`time_near` の幅以内) だった。そのセッションに付き、そのセッションの根拠を結論から外す (b) `completeness = gap` の Revision のうち、止まっていた間の再走査 (`startup_gap`) と監視のあふれ (`overflow_gap`) が作ったもの。セッションに属さず、他の根拠より強い (結論は `unknown`)。サービスが動いている間の取りこぼしを検証の走査が拾った Revision (`verify_gap`) は `preexisting` にしない (§4) |

## 3. 区分の決め方

上から順に当てはめ、最初に成り立ったものを採る。

| 条件 | actor | attribution |
|---|---|---|
| `editor_saved` がある | `editor_user` | `verified` |
| `restore_expected` がある | `restore` | `verified` |
| `preexisting` がある | `unknown` | `unknown` |
| `declared_edit_match` があり、`overlap` が無い | `ai` | `verified` |
| `worktree` があり、その作業ツリーを使うセッションが 1 つだけ | `ai` | `verified` |
| `report_hash_match` がある、または `op_window` + `before_match` がある | `ai` | `claimed` |
| `report_only` がある | `ai` | `claimed` (照合できなかったことを根拠に残す) |
| `time_near` だけがある | `ai` | `inferred` |
| どれも無い | `unknown` | `unknown` |

`editor_saved` が表の先頭なのは、Editor の保存と AI の宣言が**同じ内容**の Revision に成り立つ (AI の編集とまったく同じ結果をその場で Editor が保存した) ときに、Editor を採るため (M104k2)。
AI の根拠 (`declared_edit_match` など) は消さずに残し、結論だけが `editor_user` / `verified` になる。このとき Revision は AI の操作に結び付けない (`operation_id` は NULL)。
内容が違えば `editor_saved` は付かず、AI の判定は変わらない。`gap` の Revision には付けない (観測の時刻が書いた時刻ではないので、知らせとの時刻の照合が成り立たない)。

`editor_saved` の限界 (誤って Editor のものにしうる唯一の形): Editor が保存した内容と**同じ内容**を AI が同じパスへ、知らせの前後 10 秒以内に書いた場合。
内容は同じなので、Hunk もファイルの中身も同じ。人の保存を AI のせいにするよりは害が小さいと判断して許している。Editor は保存の直後 (知らせの口が呼ばれた時点) にファイルの大きさと最終更新時刻を控え、ハッシュを取る直前と直後にも同じであることを確かめる。違えば、その間に別のプロセスが書き換えたとみなして `editor_saved` を送らない (その変更は `unknown` のまま。外の変更を Editor の利用者として確定しない)。
残る穴は、控えた時点より前に別のプロセスが書き換えていた場合と、大きさも更新時刻も変わらない書き換え (更新時刻を揃える道具) だけ。Engine 層の書き込みを変えない都合で、Editor は書いた後のファイルを読んでハッシュを取る ([editor-save-routes.md](editor-save-routes.md))。

### 3.1 なぜ Verified の条件を内容の照合にするか

仕様書 §4.3 は「`change_report` を受けただけでは Verified にしない」「Before / After、操作識別子、隔離された作業領域を照合できたら Verified」としている。
時刻の窓と Before の一致だけでは足りない。フックの直前から直後までの間に人が同じファイルを保存する可能性を、窓では排除できないため。

`declared_edit_match` は、**編集の内容そのもの**を突き合わせる。フックは道具の入力 (置換前の文字列と置換後の文字列、または書き込む全文) を
`op_begin` で渡す。サービスは `op_begin` の時点の内容にその編集を当て、結果のハッシュを計算しておく。
監視が観測した After がそれと一致すれば、その変更は申告された編集の結果そのものと言える。人が同時に別の編集をしていれば、一致しない。

一致しなかった場合は Claimed に下がる。Verified に近いからといって丸めない。

### 3.2 Claimed と Inferred の違い

- Claimed は「AI 側が自分の変更だと言っていて、矛盾する観測が無い」。
- Inferred は「誰も申告していないが、時刻だけ見ると AI のセッションが動いていた」。
- `time_near` の幅は既定 2 秒 (設定可)。幅を広げても Inferred より上にはならない。

## 4. 人の変更を混ぜないための規則

仕様書 §16.1「変更前から存在した人間の未コミット変更を AI 変更として混入させない」に対応する。

- セッションの基準は、`session_begin` の**時刻とジャーナルの位置**。全ファイルのハッシュの写しは持たない (サービスは常時記録しているので、基準より後の変更 = `observed_at >= started_at` の Revision で決まる。時刻とジャーナルの位置は `sessions.coverage`)。
- セッションの変更として数えるのは、開始より**後に**観測された変更だけ。開始より前の Revision は `preexisting`。ベースライン・起動時の再走査の最中に `session_begin` が来ても待たずに受け付け、走査が作る Revision は `gap` なので `preexisting` で落ちる。
- git の HEAD との差は使わない。HEAD との差には、セッションより前の未コミットの変更が全部含まれる。`base_commit` は表示のための情報で、判定には使わない。
- サービスが止まっていた間に起きた変更 (起動時の再走査が作った `gap`) と、監視のあふれの後の再走査が作った `gap` は、その間にセッションが動いていたとしても `unknown` にする。止まっていた間のことは見ていない。
- **サービスが動いている間の取りこぼし** (`source = verify_gap`。検証の走査が拾った) は別扱いにする (spec D35)。`observed_at` が走査の時刻で、書かれた時刻ではないので、**時刻による根拠 (`op_window`、`time_near`、`worktree`、`report_only`) は作らない**。内容による根拠だけを数える: `before_match` + `declared_edit_match` がそろえば `verified` (内容が宣言と一致していることは、いつ観測したかに依らない)、`report_hash_match` なら `claimed`、どちらも無ければ `unknown`。操作の窓は終わりを問わず (`op_begin` より後なら) 照合する。`op_end` の即時の読み取りが「変更なし」だったときも、窓の中の `verify_gap` の Revision はこの規則で見直される。

## 5. 複数のセッション

仕様書 §4.4 と §16.2 に対応する。

- セッションは `sessionId` (UUID) で区別する。プロセスは PID と開始時刻の組で区別し、PID だけでは識別しない。同じ PID でも開始時刻が違えば別のプロセス。
- 同じパスに 2 つ以上のセッションの操作の窓が重なったら、その変更に `overlap` を付ける。
  - `declared_edit_match` が一方にだけ成り立つなら、候補はそのセッション 1 つに絞る。ただし確かさは §3 の表のとおり `claimed` にとどめる (窓が重なっている間は `verified` にしない。安全側)。
  - どちらにも成り立たないなら、両方を候補として残し、結論は `actor = ai`、`attribution = unknown` にする。
- `verify_gap` の Revision (§4) では `overlap` を作らない。時刻の窓が使えないので、窓の重なりは数えない。代わりに、内容の一致 (`before_match` + `declared_edit_match`) が 1 つのセッションの操作にだけ成り立つことで絞る (その場合は §4 のとおり `verified`)。2 つ以上のセッションに成り立てば絞れず、それらを候補にした `ai` / `unknown` になる。
- 候補は消さない。あとから `change_report` が届いて区別できるようになったら、結論を計算し直して `attribution_updated` を通知する (通知は M104j)。
- **帰属は後から変わる**: 根拠は Revision の記録より後に届くことがある (監視の方が `op_end` より遅い、`report` が後から来る)。根拠が増えるたびに、その Revision の結論を計算し直す。根拠の行は増えるだけで消えない。
- 候補のセッションは `list` の `sessionId` で絞れる。結論が 1 つの操作に絞れていればその操作で、複数候補・操作の無い根拠 (申告・時刻・作業ツリー) は根拠のセッションで引く。`describe` は根拠の一覧と `candidateSessions` を返す。

## 6. 管理された編集経路

「管理された」とは、編集の直前と直後にサービスへ知らせが届き、編集の内容が分かる経路のこと。

### 6.1 CLI フック

`mye_track.exe hook pre` / `hook post` は、フックの入力 (JSON) を標準入力から読み、pipe で `op_begin` / `op_end` を送って終わる使い捨てのコマンド。

- フックは AI の編集を止めない。サービスに届かなくても終了コード 0 で返り、追跡に欠落が残るだけにする (仕様書 §1.3)。時間切れは 5 秒 ([ipc-contract.md](ipc-contract.md) §6)。
- セッションは、フックの入力に含まれるクライアント側のセッション ID を `clientSessionId` として結び付ける。同じ `clientSessionId` の `session_begin` が無ければ、フックが自分で登録する。同じ `clientSessionId` の running / interrupted のセッションがあれば、新しく作らずそれを返す (interrupted なら running に戻す)。
- フックは pre と post が別のプロセスなので、`op_end` は `operationId` の代わりに `clientOpId` (Claude Code の `tool_use_id`) と `clientSessionId` で操作を引く。`op_begin` が `clientOpId` を覚える。
- 管理された操作は、監視のデバウンスを待たずにサービスが自分でパスを読む (要求 §5.2)。`op_begin` は対象パスを先に確定する (未記録の人の変更があれば、それを先に独立した Revision にする。その後の内容がこの操作の Before)。`op_end` は対象パスをすぐ確定し、できた Revision をこの操作に結び付ける。`report` も照合の前に申告されたパスを確定する。確定は全体で 2 秒まで。間に合わなければ `pending: true` で返し、帰属は後から付く (AI を待たせない。[ipc-contract.md](ipc-contract.md) §6 の 5 秒の内側)。

| クライアント | 編集前後のフック | 状況 |
|---|---|---|
| Claude Code | ファイルを書く道具の実行前後に呼ばれるフックがあり、道具の入力 (対象パス、置換前後の文字列や全文) とセッション ID を受け取れる | **確認済み** (試作 5。公式文書 <https://code.claude.com/docs/en/hooks>、2026-11-14 に取得)。項目名は下の表 |
| Codex CLI | 公式ページ <https://developers.openai.com/codex/hooks> の存在だけ確認 (中身は未取得) | **未確認**。実装時に調べる。同等の仕組みが無ければ `change_report` (Claimed 止まり) |
| Gemini CLI | 公式ページ <https://geminicli.com/docs/hooks/reference/> の存在だけ確認 (中身は未取得) | **未確認**。同上 |
| その他の MCP クライアント | — | `change_report` (Claimed 止まり) |

#### Claude Code のフックの入力と設定 (試作 5)

| 項目 | 内容 |
|---|---|
| 共通の入力 | `session_id` / `transcript_path` / `cwd` / `hook_event_name` |
| 道具のイベント (`PreToolUse` / `PostToolUse`) | `tool_name` / `tool_input` / `tool_use_id`。`PostToolUse` はさらに `tool_response` |
| Write の `tool_input` | `file_path` / `content` |
| Edit の `tool_input` | `file_path` / `old_string` / `new_string` / `replace_all` |
| 設定 | `.claude\settings.json` などの `hooks.PreToolUse[].matcher` + `hooks[].type = "command"` |
| 終了コード | **2 は PreToolUse で道具を止める。** 他の 0 以外は止めない |
| 時間切れ | 既定は 600 秒 |
| 未確認 | MultiEdit / NotebookEdit の入力の項目名 (公式文書で確認できなかった)。対応しない道具は Claimed 止まりでよい |

フック CLI の制約 3 つ:

1. **終了コード 2 を絶対に返さない。** 入力が読めない・サービスが居ない・時間切れ・内部のエラーのどれでも 0 で返す (2 を返すと AI の編集を止めてしまう)。標準出力へ JSON を出さない (Claude Code が解釈する)。
2. **設定の例に `timeout` を明示する。** 既定の 600 秒に頼らない (サービスへの時間切れは 5 秒)。
3. **pre と post の対応は `tool_use_id` で取る。** 時刻や順序で対にしない。同時に複数の道具が走ってもずれない。

宣言された編集の適用 (`declared_edit_match`): Edit は `old_string` を Before の中で数え、0 件なら当てられない、2 件以上で `replace_all` が偽なら当てられない (Claude Code の Edit がこの場合に実際に拒否するかは未確認)、それ以外は置換して After のハッシュを計算する。`replace_all` が真なら全部を置換する。Write は `content` がそのまま After。
試作では、この計算の結果が、別の実装で実際に行った編集の After と 5 件すべてで一致した。ただし After を作ったのは試作の駆動スクリプトで、本物の Claude Code が渡す入力と Edit の実際の振る舞い (CRLF や引用符の扱い) は未確認 (sub-08 = M104i で確かめる)。当てられない・一致しないときは Verified にならず、Claimed 止まりに落ちる (安全側)。

フックを設定していないクライアントの変更は、申告があれば Claimed、無ければ Inferred か Unknown になる。それで正しい。

### 6.2 専用の作業ツリー

セッションが `git worktree` などで自分専用のディレクトリを使い、`session_begin` の `workspace` がそれを指している場合、その中の変更はそのセッションのものとして `verified` にできる。サービスは作業ツリーを作らない (作るのは利用者かクライアント)。

`workspace` がプロジェクトのルートで、セッションが 1 つだけ、という普通の使い方では、人の編集まで Verified になってしまう。**根拠は構造で確かめられるときだけ**付ける。条件を全部満たすとき:

1. `session_begin` が `isolated: true` を申告した。
2. サービスのプロジェクトルートが git のリンクされた作業ツリー (`.git` がディレクトリではなくファイルで、`gitdir:` の行を持ち、その指す先が `worktrees` ディレクトリの下。サブモジュールの `.git` ファイルは `modules` の下を指すので成り立たない。読むだけで git は起動しない)。
3. その `workspace` を登録している running のセッションが 1 つだけ (変更ごとに確かめる)。
4. `role = editor` の接続が無い (開始時に確かめる。Editor が居れば、人が同じ作業ツリーを編集している)。

1 つでも欠ければ `worktree` を付けない (`claimed` 以下に落ちる)。

### 6.3 MCP 経由

`mye_mcp.exe` 自体はファイルを編集する道具を持たない (既存の 5 ツールに編集は無い)。
MCP が関わるのは `change_session_begin` / `change_session_end` / `change_report` の申告だけなので、MCP 経由だけでは Claimed が上限になる。

## 7. 受け入れ条件との対応

| 仕様書 §16 の項目 | この文書の規則 |
|---|---|
| 人間の未コミット変更を混入させない | §4 (セッションの基準、`preexisting`) |
| 取得できなかった中間状態を完全な履歴として扱わない | `completeness` ([db-schema.md](db-schema.md) §3) |
| 複数 AI セッションを区別できる | §5 (`sessionId`) |
| PID の再利用で混同しない | §5 (PID + 開始時刻) |
| Editor 停止中の MCP 操作を記録できる | サービスは Editor と独立 ([architecture.md](architecture.md) §3) |
| 編集元が不明な変更は Unknown | §3 の最後の行 |
| 同一ファイルを複数 AI が編集しても誤った確定帰属を行わない | §5 (`overlap`) |

## 8. 表示

- 一覧と詳細には、区分と一緒に**根拠の一覧**を出す。区分だけを見せない。
- Unknown と Inferred は、AI の変更の一覧に混ぜるかどうかを切り替えられるようにする。既定では別の節に分けて出す。
- `completeness` が `complete` でない Revision には、その旨を必ず出す。
