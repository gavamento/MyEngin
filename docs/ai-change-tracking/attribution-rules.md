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
| `restore_expected` | Editor が「復元でこの内容を書く」と予告し、観測した After と一致 | Editor |
| `overlap` | 同じパスに、別のセッションの操作の窓が重なっている | サービス自身 |
| `preexisting` | この変更はセッションの開始より前から作業ツリーにあった | サービス自身 (ベースライン) |

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

- セッションの開始時 (`session_begin`) に、サービスはその時点の全ファイルの内容ハッシュを基準として覚える。これはサービスの起動時のベースラインとは別に、セッションごとに持つ。
- セッションの変更として数えるのは、基準より**後に**観測された変更だけ。
- git の HEAD との差は使わない。HEAD との差には、セッションより前の未コミットの変更が全部含まれる。`base_commit` は表示のための情報で、判定には使わない。
- サービスが止まっていた間に起きた変更 (`completeness = gap`) は、その間にセッションが動いていたとしても `unknown` にする。止まっていた間のことは見ていない。

## 5. 複数のセッション

仕様書 §4.4 と §16.2 に対応する。

- セッションは `sessionId` (UUID) で区別する。プロセスは PID と開始時刻の組で区別し、PID だけでは識別しない。同じ PID でも開始時刻が違えば別のプロセス。
- 同じパスに 2 つ以上のセッションの操作の窓が重なったら、その変更に `overlap` を付ける。
  - `declared_edit_match` が一方にだけ成り立つなら、候補はそのセッション 1 つに絞る。ただし確かさは §3 の表のとおり `claimed` にとどめる (窓が重なっている間は `verified` にしない。安全側)。
  - どちらにも成り立たないなら、両方を候補として残し、結論は `actor = ai`、`attribution = unknown` にする。
- 候補は消さない。あとから `change_report` が届いて区別できるようになったら、結論を計算し直して `attribution_updated` を通知する。

## 6. 管理された編集経路

「管理された」とは、編集の直前と直後にサービスへ知らせが届き、編集の内容が分かる経路のこと。

### 6.1 CLI フック

`mye_track.exe hook pre` / `hook post` は、フックの入力 (JSON) を標準入力から読み、pipe で `op_begin` / `op_end` を送って終わる使い捨てのコマンド。

- フックは AI の編集を止めない。サービスに届かなくても終了コード 0 で返り、追跡に欠落が残るだけにする (仕様書 §1.3)。時間切れは 5 秒 ([ipc-contract.md](ipc-contract.md) §6)。
- セッションは、フックの入力に含まれるクライアント側のセッション ID を `clientSessionId` として結び付ける。同じ `clientSessionId` の `session_begin` が無ければ、フックが自分で登録する。

| クライアント | 編集前後のフック | 状況 |
|---|---|---|
| Claude Code | ファイルを書く道具の実行前後に呼ばれるフックがあり、道具の入力 (対象パス、置換前後の文字列や全文) とセッション ID を受け取れる | 設計の前提にする。実装時に、入力の項目名と設定方法を公式の文書で確かめる |
| Codex CLI | 未調査 | 実装時に調べる。同等の仕組みが無ければ `change_report` (Claimed 止まり) |
| Gemini CLI | 未調査 | 同上 |
| その他の MCP クライアント | — | `change_report` (Claimed 止まり) |

フックを設定していないクライアントの変更は、申告があれば Claimed、無ければ Inferred か Unknown になる。それで正しい。

### 6.2 専用の作業ツリー

セッションが `git worktree` などで自分専用のディレクトリを使い、`session_begin` の `workspace` がそれを指している場合、その中の変更はそのセッションのものとして `verified` にできる。
条件は「その `workspace` を登録しているセッションが 1 つだけ」。サービスは作業ツリーを作らない (作るのは利用者かクライアント)。

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
