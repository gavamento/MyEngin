# AI Change Tracking: IPC 契約

- 状態: プロトコル版 1。サービス側は M104f で実装済み (§9)。MCP (M104h)・フック (M104i)・Editor (M104j。`src\Editor\ChangeTracking\`) のクライアントも実装済み。`subscribe` と通知は M104j (§9.8)
- 全体像は [architecture.md](architecture.md)。この文書は追跡サービス (`mye_track.exe serve`) とクライアント (MCP、Editor、フック CLI) の間の取り決めだけを書く

## 1. 伝送路

| 項目 | 取り決め |
|---|---|
| 種類 | Windows の名前付きパイプ。バイトストリーム、双方向 |
| 名前 | `\\.\pipe\mye-track-<projectId>`。`projectId` は正規化したプロジェクトルートの SHA-256 の先頭 16 桁 |
| 範囲 | 同じマシンの中だけ。リモートからの接続を拒否する (`PIPE_REJECT_REMOTE_CLIENTS`) |
| アクセス制御 | サービスを起動したユーザーだけに許可する DACL を張る |
| 同時接続 | 複数。接続ごとに独立した要求 ID の空間を持つ |
| 文字コード | UTF-8 |
| 区切り | 1 行 1 JSON。行末は LF。JSON の中に生の改行を入れない |
| 1 行の上限 | 1 MiB。超える内容は `blob_get` で分けて取る |

1 行 1 JSON は `mye_mcp.exe` の stdio と collab の CLI と同じ形で、テストの照合 (`*.ndjson` と `*.expected.ndjson`) をそのまま流用できる。

**確認済み** (試作 1、2026-11-14。[prototype-results.md](prototype-results.md)): Rust から DACL とリモート拒否を張り、C++ の overlapped I/O (スレッドなし) と 1 行 1 JSON で往復できた。
100 本の短い行と 1 MiB 近い 1 行が 47 ms で全部一致し、`Poll()` 1 回は最長 0.2 ms。`GetNamedPipeClientProcessId` はクライアントの PID を返す。

**決定** (Win32 の呼び方): `windows-sys` 0.61 を依存に足す。`extern "system"` の自前宣言は採らない。
使う関数が 14 個と構造体・定数が多く、`windows-sys` ならコンパイラが定数名と型を検査する。`notify` が同じ版を引き込むので、依存の実質的な増加は無い。
feature は `Win32_Foundation`、`Win32_Security`、`Win32_Security_Authorization`、`Win32_Security_Cryptography` (トークンの乱数)、`Win32_System_Pipes`、`Win32_Storage_FileSystem`、`Win32_System_IO`、`Win32_System_Threading`、`Win32_System_Console`。

サーバが pipe を張る呼び方:

- DACL は、自分のトークンの SID を取り (`OpenProcessToken` → `GetTokenInformation(TokenUser)` → `ConvertSidToStringSidW`)、SDDL `D:P(A;;GA;;;<SID>)` から作る。読み戻すと許可は自分の SID だけ (継承なし)。
- `CreateNamedPipeW` は `PIPE_ACCESS_DUPLEX`、`PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS`、`PIPE_UNLIMITED_INSTANCES`、入出力バッファ 64 KiB。
  最初の 1 本だけ `FILE_FLAG_FIRST_PIPE_INSTANCE` を付ける (同名の 2 本目のサーバは `ERROR_ACCESS_DENIED` で失敗する。名前付き mutex ([architecture.md](architecture.md) §3) と二重の防御になる)。
- リモート拒否は、`\\localhost\pipe\<名前>` で開く試験で、フラグありは拒否 (`ERROR_ACCESS_DENIED`)、フラグなしは開けたことで確かめた。

クライアントの作法:

- **`ERROR_PIPE_BUSY` (231) を再試行する。** 接続を引き渡した直後は空きインスタンスが無く、2 本目の接続が一度失敗した。`WaitNamedPipeW` で待って (最大 2 秒) 開き直す。サーバは `ConnectNamedPipe` が返ったらすぐ次のインスタンスを作る (試作もそうしたが、隙間は残った)。
- Editor の `Poll()` は、`ERROR_IO_PENDING` なら保留し、次の `Poll()` で `GetOverlappedResult(bWait = FALSE)` で見る。待たない。

**未確認**: 別のユーザーからの接続の拒否 (開発機に別のアカウントが無く、DACL の読み戻しだけ)、本物のリモートからの接続、Editor 側の実装 (sub-09 = M104j で確かめる)。

## 2. メッセージの形

collab の取り決め (`tools\collab\src\protocol.rs`) に合わせる。

```
要求    {"id": 12, "op": "list", "args": {...}}
応答    {"id": 12, "ok": true,  "result": {...}}
失敗    {"id": 12, "ok": false, "error": {"code": "not_found", "detail": "..."}}
通知    {"event": "change_recorded", ...}          ← id を持たない。サービスから一方的に届く
```

- `id` はクライアントが振る正の整数。応答は同じ `id` を返す。順番は保証しない。
- 知らない `op` は `unknown_op`、知らない引数は `bad_args`。黙って無視しない (MCP の `reject_unknown` と同じ方針)。
- 通知を受け取るのは `subscribe` を呼んだ接続だけ。

## 3. 接続の手順

1. クライアントが接続する。
2. 最初の要求は必ず `hello`。それ以外が来たら `need_hello` を返して切る。
3. サービスは版・トークン・プロジェクトを確かめ、合わなければ理由を返して切る。

```
{"id":1,"op":"hello","args":{"proto":1,"role":"editor","token":"<service.json の値>","projectRoot":"<絶対パス>","pid":1234}}
{"id":1,"ok":true,"result":{"proto":1,"projectId":"...","serviceStartedAt":"...","coverage":{...}}}
```

| 検査 | 内容 |
|---|---|
| 版 | `proto` が一致しなければ `proto_mismatch`。サービスは自分の版を返す |
| トークン | `<project>\.mye\ai_changes\service.json` にサービスが起動ごとに書く乱数。一致しなければ `bad_token` |
| プロジェクト | `projectRoot` を正規化して自分の projectId と比べる。違えば `wrong_project`。**クライアントが言ったパスを信用して DB を開き直すことはしない** |
| 接続元 | `GetNamedPipeClientProcessId` で得た PID と、そのプロセスの開始時刻を証跡として残す。クライアントが申告した `pid` と違えば、パイプから得た値を採る |

トークンは同じユーザーの他プロセスからも読める。防ぐのは「別プロジェクトのサービスへの誤接続」と「古いサービスへの接続」で、
同じユーザーの悪意あるプロセスを防ぐ境界ではない (それは DACL でも防げない)。この限界は [restore-safety.md](restore-safety.md) §8 にも書く。

### 3.1 役割

`role` は `mcp` / `editor` / `hook` の 3 つ。役割ごとに呼べる `op` を分ける。

| op の種類 | mcp | editor | hook |
|---|---|---|---|
| セッション・操作の登録、報告 | ○ | — | ○ |
| 一覧・詳細の取得 | ○ | ○ | — |
| サービスの終了 (`shutdown`) | — | ○ | — |
| Editor の状態の通知 (保存、スナップショット) | — | ○ | — |
| レビューの記録 | — | ○ | — |
| 復元の記録 | — | ○ | — |

役割の外の `op` は `forbidden`。仕様書 §12.6 のとおり、MCP から承認と復元を呼べる `op` は用意しない。
これは事故を防ぐための区分けで、認証ではない (役割は自己申告)。

## 4. op の一覧

版 1 で定義するもの。引数と結果の詳しい形は実装時に `tools\track\src\protocol.rs` を正本にし、この表はその要約にする。

### 4.1 共通

| op | 役割 | 引数 | 結果 |
|---|---|---|---|
| `hello` | 全部 | §3 | §3 |
| `status` | 全部 | なし | 稼働時間、追跡中のファイル数、Blob の総量、欠落区間、DB の健全性、直近の処理時間 (§9.4) |
| `shutdown` | editor | なし | 応答のあと接続を切り、サービスが終わる (`service_stop` を残す。M104f で追加) |
| `subscribe` | editor | `events`: 受け取りたい通知の名前 (§4.5。1 つ以上、知らない名前は `bad_args`)。接続ごとに 1 回 | `{}` |

### 4.2 セッションと操作

| op | 役割 | 引数 | 結果 |
|---|---|---|---|
| `session_begin` | mcp, hook | `provider` (`claude` / `codex` / `gemini` / `other`), `workspace`, `source` (`terminal` / `mcp` / `external`), `clientSessionId?`, `isolated?` | `sessionId`, `status`, `resumed`, `trackingCapabilities`, `baseCommit` |
| `session_end` | mcp, hook | `sessionId` または `clientSessionId` | `status`, `changeCount` |
| `op_begin` | mcp, hook | `sessionId` または `clientSessionId`, `operationType` (`edit` / `write` / `delete` / `rename` / `report` / `other`), `targets`: パスの配列, `declaredEdit?`, `clientOpId?` | `operationId`, 各パスの現在のハッシュ (無ければ null) |
| `op_end` | mcp, hook | `operationId`、または `clientSessionId` + `clientOpId`。`result` (`ok` / `failed`) | 結び付いた Revision の ID と帰属 (`revisions`)、`pending` |
| `report` | mcp | `sessionId` または `clientSessionId`, `operationId?`, `paths`, `beforeHashes?`, `afterHashes?` | パスごとの照合結果 (`matched` / `mismatch` / `not_observed`) と帰属、`pending` |

- `baseCommit` はサービスが自分で `git rev-parse HEAD` 相当を読んで決める (`.git\HEAD` と参照ファイルを読むだけで、git を起動しない)。git のリポジトリでなければ空。
- `op_begin` を受けた時点で、サービスは対象パスを先に確定する (未記録の変更があれば独立した Revision にする)。その後の内容が管理された操作の Before になる (仕様書 §5.2)。`op_end` と `report` も、監視を待たずに対象パスを自分で読む (全体で 2 秒まで。間に合わなければ `pending: true`。[attribution-rules.md](attribution-rules.md) §6.1)。
- `declaredEdit` はフックが渡す編集内容。`{"kind":"replace","oldString":..,"newString":..,"replaceAll":bool}` か `{"kind":"write","content":..}`。対象は `targets` の 1 つ目だけ。1 行の上限 (1 MiB) に収まる分だけで、超える宣言はフックの側で送らない。サービスは内容を Blob に入れてハッシュだけを記録する (除外パターンのパスへの宣言は保存しない)。帰属の Verified 判定に使う ([attribution-rules.md](attribution-rules.md) §3)。
- `workspace` と `targets` / `paths` はサービス側で正規化する。相対パスは `workspace` 基準ではなくプロジェクトルート基準で、絶対パスも受ける。プロジェクトルートの外を指すもの (`..` で出るもの、別のドライブ、UNC、`:` などの使えない文字) は `bad_args`。字面の正規化だけで、シンボリックリンクは辿らない。
- `sessionId` と `operationId` は OS の乱数から作る UUID v4。`session_begin` は同じ `clientSessionId` の running / interrupted のセッションがあれば、新しく作らずそれを返す (`resumed: true`。interrupted なら running に戻す)。`trackingCapabilities` はこのセッションで出せる根拠の種類。`isolated: true` は専用の作業ツリーの申告で、構造で確かめられたときだけ `worktree` の根拠になる ([attribution-rules.md](attribution-rules.md) §6.2)。
- `op_end` の結果: `revisions` (操作に結び付いた Revision の `revisionId` / `path` / `completeness` / `actor` / `attribution`)、`otherChanges` (操作の窓に入ったが別の操作・複数候補に結び付いたもの)、`pending`、`alreadyFinished`。フックは pre と post が別のプロセスなので、`operationId` の代わりに `clientSessionId` + `clientOpId` (Claude Code の `tool_use_id`) で引ける。
- `report` の結果の `results[]` は `path`、`result` (`matched` = ハッシュが Before / After の両方と一致した変更がある、`mismatch` = 変更はあるが一致しない (ハッシュを渡していない場合を含む)、`not_observed` = セッション開始後の変更が無い)、`revisionId`、`actor`、`attribution`。`beforeHashes` / `afterHashes` は `paths` と同じ並びの配列、またはパスをキーにしたオブジェクト。申告だけでは `verified` にならない (要求 §4.3)。
- 接続の PID と開始時刻: `mcp` の接続は、pipe から得た PID とプロセスの開始時刻をセッションに持つ (申告の値は使わない)。`hook` の接続は短命なので持たない。
- 記録を伴う op は Writer のキューを通し、あふれていたら `busy`。DB へ書けない状態では `db_degraded`。
- `report` は MCP の `change_report` の中身。申告と観測を突き合わせた結果を返すので、AI は自分の申告が通ったかどうかを知れる。

### 4.3 取得

| op | 役割 | 引数 | 結果 |
|---|---|---|---|
| `list` | mcp, editor | フィルタ (`sessionId`, `path`, `from`, `to`, `review`, `attribution` (文字列、または文字列の配列 = どれかに合う), `changeType`)、`cursor?`, `limit` | 変更の要約の配列、`nextCursor` |
| `describe` | mcp, editor | `revisionId` | Before / After のハッシュとサイズ、Hunk の要約、シーン変更の要約、帰属と根拠、レビュー状態、復元可否、競合状態 |
| `hunks` | editor | `revisionId`, `cursor?` | Hunk の配列 (行の本文付き) |
| `scene_diff` | editor | `revisionId`, `cursor?` | シーン / プレハブ / アクターの構造の変更の配列 (M104l。§9.4) |
| `scene_diff` | editor | `revisionId` | シーン構造差分の配列 |
| `blob_get` | editor | `hash`, `offset`, `length` | base64 の中身、総サイズ |
| `sessions` | mcp, editor | フィルタ、`cursor?` | セッションの配列 |

- `list` の 1 ページは既定 50 件、最大 200 件。Blob の中身も Hunk の本文も入れない (仕様書 §12.4)。
- `describe` が返す Hunk は要約 (行番号と行数) まで。MCP へ返すとき、本文は合計 64 KiB で切り、切ったことを `truncated` で示す。
- `blob_get` は 1 回 512 KiB まで。Editor が左右比較や復元のために原本を取るのに使う。

### 4.4 Editor からの通知と記録

| op | 引数 | 用途 |
|---|---|---|
| `editor_saved` | `path`, `hash` | Editor がこのファイルをこの内容で保存した。監視が同じ内容を観測したら、行為者を「Editor の利用者」にする |
| `editor_snapshot` | `documentId`, `path`, `json` の Blob ハッシュ, `dirty` | Editor Snapshot の登録 (中身は先に `blob_put` で送る) |
| `blob_put` | `hash`, `offset`, `data` (base64), `last` | Editor から Blob を送る。サービスが受け取った全体のハッシュを確かめてから確定する |
| `review_set` | `revisionId`, `scope`, `state`, `note?` | レビューの記録。過去の記録は消さず、追記する |
| `restore_record` | `restoreId`, `state`, `targets`, `preHashes?`, `postHashes?`, `error?` | 復元の進行を履歴へ記録する。**適用するのは Editor で、サービスは記録だけ** |
| `restore_expect` | `restoreId`, `paths`, `hashes` | これから Editor が書くファイルと内容。監視が同じ内容を観測したら、行為者を「復元」にする |

`editor_saved` の取り決め (M104k2。実装は `tools\track\src\ops_record.rs` の `editor_saved` と `attribution.rs` の `apply_editor_saved`):

- `role = editor` だけ (`mcp` / `hook` は `forbidden`。AI 側が「Editor が書いた」と名乗って帰属を書き換えられないようにする)。引数は `path` (プロジェクトルート基準の相対パス、または絶対パス。外を指すものは `bad_args`) と `hash` (内容の SHA-256、64 桁の 16 進。大文字も受けて小文字にする)。結果は `{ path, pending }`。
- サービスは受けたらすぐ対象パスを読んで観測にし (デバウンスを待たない。`op_end` と同じ即時の読み取り、全体で 2 秒まで)、知らせを `editor_saved` のイベントとして記録する。**照合**: 観測した Revision の After のハッシュが知らせの `hash` と同じで、観測時刻が知らせの時刻の前後 `EDITOR_SAVED_WINDOW_MS` (10 秒) 以内なら、`editor_saved` の根拠を付けて行為者を `editor_user` / `verified` にする。**知らせが先でも観測が先でも同じ結果**になる (知らせは窓の間だけ効く)。ハッシュが違えば付かない。`gap` の Revision は観測の時刻が書いた時刻ではないので対象外。
- 知らせは `events` から派生の表 (`attribution_evidence`) を作り直せる (db-schema.md §6。作り直した DB は元と同じ根拠になる)。引く速さのため、スキーマ版 2 で `events(kind, at)` の索引を足した。
- Editor の保存と AI の `declared_edit_match` が同じ Revision に成り立つのは、内容が同じときだけ。このとき [attribution-rules.md](attribution-rules.md) §3 の表の先頭の行 (`editor_saved`) が勝つ。AI の根拠は消さない。
- 送らなくても保存は成り立つ (その変更が `unknown` / `inferred` になるだけ)。Editor の側の送り方と経路の一覧は [editor-save-routes.md](editor-save-routes.md)。

### 4.5 通知

| event | いつ | 中身 |
|---|---|---|
| `change_recorded` | Revision が確定した | `revisionId`, `changeId`, `path`, `changeType`, `attribution` (+ `actor`、シーン / プレハブ / アクターなら `documentId`) |
| `attribution_updated` | あとから帰属が変わった (報告が届いた、など) | `revisionId`, 新しい帰属 |
| `coverage_gap` | 取り逃しや停止区間を記録した | 区間、理由 |
| `session_changed` | セッションの開始・終了・中断 | `sessionId`, `status` |
| `shutting_down` | サービスが終了する | 理由 |
| `resync` | 通知のキューがあふれて、古い通知を捨てた | `reason` (`queue_overflow`)。**購読の対象ではなく、捨てたときは必ず届く**。受け取ったら一覧・状態・詳細を取り直す |

通知は `subscribe` を呼んだ `editor` の接続だけに届く。実装の取り決めは §9.8。

## 5. エラーコード

| code | 意味 | クライアントの扱い |
|---|---|---|
| `need_hello` | `hello` より先に別の要求が来た | 実装の誤り |
| `proto_mismatch` | 版が違う | 追跡を止め、版が違うことを表示する |
| `bad_token` | トークンが違う | `service.json` を読み直して 1 回だけやり直す |
| `wrong_project` | プロジェクトが違う | 追跡を止める |
| `forbidden` | 役割の外の op | 実装の誤り |
| `unknown_op` / `bad_args` | 形が悪い | 実装の誤り |
| `not_found` | ID が無い | 表示を更新する |
| `blob_missing` | Blob が欠けている | 「復元不可」と表示する |
| `too_large` | 1 行の上限を超えた | 分割してやり直す |
| `busy` | Writer のキューが上限を超えた | 間を置いてやり直す。記録の op が落ちた場合は欠落として残る |
| `db_degraded` | DB が使えない状態 | 追跡を止め、状態を表示する |
| `internal` | サービス内の失敗 | 表示する。サービスは続行する |

## 6. 時間切れと再接続

| 場面 | 取り決め |
|---|---|
| 接続 | pipe が無ければサービスを起動し、最大 5 秒まで待ってやり直す。だめなら追跡なしで続行する |
| 読み取りの op | 30 秒で時間切れ (collab の Read 系と同じ) |
| 記録の op | 5 秒で時間切れ。**AI の編集を待たせない** (仕様書 §1.3)。フックは時間切れでも成功として返り、欠落だけが残る |
| 切断 | クライアントは次の要求のときに接続し直す。Editor は数秒おきに試す |
| サービスの再起動 | トークンが変わるので、`bad_token` を受けたら `service.json` を読み直す |

## 7. 版の管理

- プロトコル版は整数 1 つ。C++ の定数 (`src\Editor\ChangeTracking\TrackProtocol.h` の `kTrackProtoVersion`) と Rust の定数 (`tools\track\src\protocol.rs` の `PROTO_VERSION`) を持ち、`tools\check_rules.ps1` 規則 9 の `$constGroups` に 1 組足して突き合わせる。前例は `kCollabProtoVersion` と `PROTO_VERSION`。
- `tools\mcp` の pipe クライアントは同じ版を定数で持ち、規則 9 の同じ組に入れる。
- op や引数を足すだけなら版を上げない (知らない引数を拒否するので、古いサービスに新しい引数を送ると失敗が見える)。意味を変える、または削るときに版を上げる。

## 8. MCP のツールとの対応

| MCP ツール | 使う op |
|---|---|
| `change_session_begin` | `session_begin` |
| `change_session_end` | `session_end` |
| `change_report` | `report` (必要なら先に `op_begin` / `op_end`) |
| `change_list` | `list` |
| `change_describe` | `describe` |

MCP のツールは引数を検査して pipe へ渡し、結果を既存の `wrap()` の形で返すだけにする。判定はすべてサービス側で行う。

実装 (M104h、`tools\mcp\src\track_client.rs`): 道具の呼び出しごとに接続し、`hello` (`role = mcp`) → op の順に 1 往復する。サービスのエラーは `isError` の結果にコードと文面をそのまま入れる。
追跡するプロジェクトは MCP の notes と同じ決め方 (`--project`、無ければ `--root` / 見つけたリポジトリのルート)。projectId は `tools\mcp` が自前で計算し (SHA-256 も自前。依存を足さない)、`hello` の結果の `projectId` が自分の計算と一致することを確かめる。
読み取りは別スレッドで行い、時間切れ (記録の op 5 秒、取得の op 30 秒) でも MCP 全体を止めない。時間切れの接続は読み取りで止まったスレッドごと残るが、次の呼び出しは別の接続を張る。
`change_describe` は `hunks` と `evidence` を合計 64 KB で切り、切ったら `truncated: true` を返す。

## 9. 実装 (M104f)

正本は `tools\track\src\protocol.rs` (版・上限・op と役割の表・エラーコード)、`server.rs` (常駐)、`ops.rs` (取得の op)、`client.rs` (クライアント)。この節は実装で決めたことの要約。

### 9.1 起動と単一インスタンス

`mye_track.exe serve --project <root> [--idle-timeout-ms <n>]`。終了コード: 0 = 正常、1 = 失敗 (または DB へ書けない状態で終了)、2 = 引数の誤り、**3 = 同じ DB を持つサービスが既に居る**。

二重起動は 3 段で守る。どれかに当たったら、DB を開く前に終了コード 3 で終わる (起動を頼んだ側は、居るサービスへ繋ぐか、繋げなければ追跡なしで続行する)。

| 段 | 守るもの | 破れる場合 |
|---|---|---|
| 名前付き mutex `Local\mye-track-<projectId>` | 同じ projectId | 別名のパス (`subst` のドライブ、ジャンクション、8.3 名、UNC) は projectId が変わる |
| `.mye\ai_changes\service.lock` を共有なし (share mode 0) で開き続ける | **同じディレクトリを指すなら名前によらず** (spec D25)。標準エラーに、動いているサービスの `projectRoot` (`service.json` から) を出す | 別のボリュームへの複製など、別の DB |
| pipe の最初の 1 本 (`FILE_FLAG_FIRST_PIPE_INSTANCE`) | 同名の pipe | — |

起動の順序: mutex → `service.lock` → トークン → pipe の最初の 1 本 → `service.json` → DB を開く → Tracker (ベースラインは別スレッド)。pipe は DB を開く前から繋がるので、クライアントは先に繋げて、応答は受付が始まってから返る。

**終了の合図** (標準入力の EOF では終わらない): pipe の `shutdown`、接続が 0 のまま `--idle-timeout-ms` (既定 10 分。起動からも数える)。ベースライン・再走査の最中と、running のセッションがある間は数えない (§9.7)、Ctrl+C / Ctrl+Break / コンソールを閉じる。どれも `service_stop` (理由は `shutdown_op` / `idle_timeout` / `signal`) を残し、`service.json` を消す。

### 9.2 `service.json`

`<project>\.mye\ai_changes\service.json`: `protoVersion`、`pid`、`startedAt` (UTC ミリ秒)、`pipe`、`token` (起動ごとに OS の乱数源から 16 バイトを 16 進で)、`projectId`、`projectRoot`。一時ファイルへ書いてから置き換える。

### 9.3 接続

- 1 行は 1 MiB まで (改行を除く)。超えたら `too_large` を返し、その行を LF まで捨てて、次の行から読み続ける (接続は切らない)。ちょうど 1 MiB は通る。
- 空行は無視する。行末の CR は取る。JSON として読めない・`id` が正の整数でない・知らない項目がある行は `bad_args` (`id` が読めればそれを返し、読めなければ `null`)。`hello` の前でも同じ (形が読めないので `need_hello` にはならない)。
- `hello` の検査の順: 形 (`proto` / `role` / `token` / `projectRoot` が要り、`pid` は任意、他は `bad_args`) → 版 (`proto_mismatch`。detail にサービスの版) → 役割 (知らない値は `bad_args`) → トークン (`bad_token`) → プロジェクト (`wrong_project`。`projectId` が合うかだけを見て、DB は開き直さない)。失敗したら応答のあと切る。`hello` より前の要求 (知らない op を含む) は `need_hello` で切る。2 回目の `hello` は `bad_args` (接続は残る)。
- 成功した `hello` は証跡を `events` に残す (`client_hello`。db-schema.md §5.1)。申告した `pid` と違えば、パイプから得た値を採る。Writer のキューが上限 (1024 件) を超えていたら `busy` で `hello` を断る。
- op の判定: 実装している op (`hello`、`status`、`list`、`describe`、`sessions`、`hunks`、`scene_diff`、`blob_get`、`shutdown`、`subscribe`、`session_begin`、`session_end`、`op_begin`、`op_end`、`report`) 以外は `unknown_op` (未実装の op を含む。detail の名前は 64 文字に切る)。役割の外は `forbidden`。知らない引数は `bad_args`。
- 取得の op は読み取り専用の SQLite 接続で引く (WAL なので書き手を止めない。接続ごとに 1 本、最初に必要になったとき開く)。ベースラインの最中も答える。DB へ書けない状態 (`db_degraded`) では `list` / `describe` / `sessions` が `db_degraded` を返し、`status` は答える。

### 9.4 op の引数と結果 (JSON は camelCase、時刻は UTC ミリ秒)

- `status`: `proto`、`projectId`、`pid`、`startedAt`、`uptimeMs`、`phase` (`starting` / `baselining` / `watching` / `failed` / `stopped`)、`failure`、`trackedFiles`、`baseline` (`firstRun`、`files`、`bytes`、`elapsedMs`、`scanDone`、`scanTotal`、`walkMs` / `readMs` / `hashMs` / `commitMs`)、`verify` (`runs`、`gaps`、`lastScanMs` = 直近の検証の走査の前半だけの所要時間)、`recent.recordUs` (直近に記録した観測 1 件あたりの Writer への往復、マイクロ秒)、`watches` (`count` = いま張っている OS の監視の本数、`fallback` = 分けた結果が上限 256 本を超えてルートの再帰 1 本へ戻している)、`overflows`、`unrecorded` (記録に失敗している区間 `sinceMs` / `reason`、無ければ `null`)、`db` (`state` = `recording` / `db_degraded`、`reason`、`failedSteps` = 起動時の検査 3〜6 で失敗した段と理由、`brokenDb`、`replayedEvents`、`blobsMarkedMissing`、`blobsCorrupted`、`orphanBlobs` ほか)、`blobs` (`count`、`bytes` = 重複を除いた存在する Blob)、`gaps` (`count`、`recent` = 新しい順に 20 件)。
- `list`: 引数は `path` (完全一致。末尾が `/` なら前方一致。リネーム前のパスにも当たる)、`from` / `to` (`observedAt` の範囲、両端を含む)、`changeType` (`added` / `modified` / `deleted` / `renamed`)、`sessionId`、`review`、`attribution`、`cursor`、`limit` (既定 50、最大 200。超えたら 200 に切る。1 未満は `bad_args`)。新しい Revision から順に返し、`cursor` は直前のページの最後の `revisionId` (それより古いものを返す)。結果は `items`、`nextCursor` (最後なら `null`)、`limit`、`coverage` (`phase`、`baselining`、`baselineScanned` / `baselineTotal`、`unrecorded`。**`baselining` が真の間は、一覧が不完全でありうる**)。1 件は `revisionId`、`changeId`、`seq`、`path`、`oldPath`、`changeType`、`contentKind`、`changeState` (`superseded` の Change も出す)、`observedAt`、`completeness`、`actor`、`attribution`、`diffMode`、`operationId`、`beforeSize`、`afterSize`、`hunkCount`、`review`。Blob と Hunk の本文は入れない。`sessionId` は、そのセッションが候補に入っている Revision (結論が 1 つの操作に絞れていればその操作、複数候補・操作の無い根拠は根拠のセッション)。`attribution` は M104g から実データ、`review` は全部 `pending` (レビューは後続のサブ)。`attribution` の引数は 1 つの文字列か文字列の配列 (M104k2。「不明 / 推定」を 1 回で取る `["unknown","inferred"]` のため。要素が知らない値なら `bad_args`)。`sessionId` で絞るとき、Editor の保存・復元で確定した変更 (`actor` が `editor_user` / `restore`) は、AI の根拠が残っていても入れない。
- `describe` (`revisionId`): 一覧の項目に加えて `before` / `after` (`hash`、`size`、`blobState`。無ければ `null`)、`hunks` (`ordinal`、`oldStart`、`oldCount`、`newStart`、`newCount`、`eolOnly`、`state`。本文は返さない)、`restorable` (Before の Blob が有る、または新規 = 削除で戻せる。Before を見ていない・Blob が無いものは `false`)、`evidence[]` (`kind`、`sessionId`、`operationId`、`detail`、`recordedAt`。増えるだけ)、`candidateSessions` (根拠から決まる候補のセッション)、`sceneSummary` (M104l。構造の変更が記録された Revision だけ: `documentId`、`resolvedDocumentId` (引き継ぎをたどった先)、`counts` (種別ごとの件数)、`unparsedReason` (解析不可ならその理由、そうでなければ `null`)。値は含めない。`change_describe` はそのまま通す)。無ければ `not_found`。`diffMode` が `lines` 以外のときは Hunk が無い理由が分かる (db-schema.md §3.2)。
- `sessions`: 引数は `cursor` (直前のページの最後の `<startedAt>:<sessionId>`)、`limit`。結果は `items` (セッションの一覧。`sessionId`、`provider`、`source`、`clientSessionId`、`workspace`、`baseCommit`、`startedAt`、`endedAt`、`status`、`changeCount` = そのセッションが候補に入っている Revision の数 (`list` の `sessionId` と同じ条件))、`nextCursor`、`limit`。
- `hunks` (`revisionId`、`cursor?`。**role = editor だけ**): 結果は `items` と `nextCursor` (最後なら `null`)。1 件は `hunkId`、`ordinal`、`oldStart`、`oldCount`、`newStart`、`newCount`、`beforeText`、`afterText`、`contextPre`、`contextPost`、`eolOnly`、`state`、`oversize`。`cursor` は直前のページの最後の `ordinal` (それより後ろを返す)。1 ページは Hunk の JSON の合計が 768 KiB 以内 (1 行の上限 1 MiB に余裕を残す)。**単独で 768 KiB に収まらない Hunk** は本文を入れず `oversize: true` と `beforeBytes` / `afterBytes` だけを返す (`diff_mode = whole` の大きいファイルがこれに当たる)。Editor は `blob_get` で原本を取って自分で扱う。無い `revisionId` は `not_found`、`cursor` が整数でなければ `bad_args`。Hunk の無い Revision (`binary` / `excluded` / `unavailable`) は `items` が空。
- `scene_diff` (`revisionId`、`cursor?`。**role = editor だけ**。M104l): 結果は `items` と `nextCursor` (最後なら `null`)。1 件は `sceneChangeId`、`documentId` (記録した時点の ID)、`resolvedDocumentId` (引き継ぎをたどった先。通常は同じ)、`fileId` (文書全体の変更なら `null`)、`changeType` (要求 §7.2 の種別と、解析不可の `unparsed`)、`component`、`field`、`beforeValue`、`afterValue` (元のファイルのバイト列から切り出した JSON の断片。`unparsed` の `afterValue` は理由の JSON 文字列)、`state`、`oversize`。`cursor` は直前のページの最後の `sceneChangeId`。1 ページは JSON の合計が 768 KiB 以内で、**値が単独で収まらない変更**は値を入れず `oversize: true` と `beforeBytes` / `afterBytes` だけを返す (切らない。原本は `blob_get`)。無い `revisionId` は `not_found`、`cursor` が整数でなければ `bad_args`。構造の変更が無い Revision は `items` が空。
- `blob_get` (`hash`、`offset?`、`length?`。**role = editor だけ**): 結果は `data` (base64、パディングあり)、`size` (Blob の総バイト数)、`offset`、`length` (返したバイト数)。`length` の既定と最大は 512 KiB (超えたら切る)、末尾を越える `offset` は空の `data`。**`hash` は 64 桁の 16 進 (小文字) だけを受け**、それ以外は `bad_args` (Blob のパスとして解釈される余地を残さない)。Blob が無ければ (除外されたスナップショット・回収された) `blob_missing`。範囲読みなので中身とハッシュの照合はしない。DB を引かないので `db_degraded` でも答える。
- `shutdown`: 結果は `{}`。応答を返したあと、サービスが止まる。

### 9.5 クライアント

`mye_track.exe client (--project <root> | --pipe <name>) [--wait-ms <n>]`: 標準入力の 1 行を要求として pipe へ送り、応答の 1 行を標準出力へ出す (hello は自動で行わない = hello の失敗も流せる)。pipe が無ければ `--wait-ms` (既定 5000) の間待ち、それでも無ければ終了コード 4。サービスが接続を切っていたら `{"client":"closed"}` を出して終わる。`ERROR_PIPE_BUSY` は `WaitNamedPipeW` で最大 2 秒待って開き直す (§1)。この接続の実装 (`client.rs`) は、フックの CLI (sub-08) と共有する。

MCP の起動の作法 (`track_client.rs`): `mye_track.exe serve --project <root>` を、標準入出力を 3 本とも null にして、`CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS` で起動する (`ERROR_ACCESS_DENIED` なら `CREATE_BREAKAWAY_FROM_JOB` を外してやり直す)。子は待たない。
Rust の `Command` は継承可能なハンドルを子へ複製するため、起動のあいだだけ自分の標準入出力のハンドルを継承不可にする (これをしないと、サービスが MCP の stdout を掴んで、クライアントから MCP の終了が見えない。`tools\track_verify.ps1` の `# mcp` がこれを見る)。

`mye_track.exe` を探す順 (Editor・MCP・フックが起動するとき。MCP は M104h で実装、フックと Editor は sub-08 / sub-09): (1) 環境変数 `MYE_TRACK_EXE` (2) 自分の exe の隣 (3) MCP だけ `..\..\..\track\target\release\mye_track.exe`。**`MYE_TRACK_EXE` が設定されているときは (1) だけを使い、そのファイルが無ければ (2) (3) を探さずに起動に失敗する** (検証でサービスの不在を決定的に作るため。空文字列は未設定と同じ)。`tools\build_track.bat` が `bin\x64\Debug` と `bin\x64\Release` の両方へ置くので、どちらの構成の Editor からも (2) で見つかる。

フック CLI (`hook.rs`、M104i): `mye_track.exe hook <pre|post|session-start|session-end> [--project <root>]`。`client.rs` の `Client` で `role = hook` として繋ぎ、`session_begin` / `op_begin` / `op_end` / `session_end` を送る。入力と設定は [hooks-setup.md](hooks-setup.md)。終了コードは常に 0、標準出力は常に空 (Claude Code が解釈するため)、全体で 5 秒 (§6) を超えない。サービスを起動するのは `--project` があり、サブコマンドが `pre` / `session-start` のときだけ (起動の作法と exe の探し方は上と同じ。ただし (2) は自分自身)。`--project` が無いときは、対象のファイル (無ければ `cwd`) から親へ `.mye\ai_changes\service.json` を探し、見つかったプロジェクトの動いているサービスへ繋ぐ。見つからなければ何もしない。1 MiB を超える要求になる `declaredEdit` は付けずに `op_begin` する (Claimed 止まり)。

### 9.6 検証

`tools\track_verify.bat` が `tests\track\*.ndjson` を `mye_track client` 経由でサービスへ流し、`*.expected.ndjson` と比べる (書式と置換、`# write` などのディレクティブは `tools\track_verify.ps1` の冒頭)。`cargo test` の `tests\pipe.rs` は、二重起動 (同じ projectId、ジャンクション経由の別名)、再起動をまたぐ履歴と `gap`、接続 0 の時間切れと `service_stop`、pipe の DACL の読み戻しとリモート形式の拒否、複数接続を、実際に `mye_track.exe serve` を起動して確かめる。

**未確認**: 別のユーザーの接続の拒否 (DACL の読み戻しだけ)、本物のリモートからの接続。

### 9.7 セッションと操作の寿命 (M104g)

| 対象 | 変わる条件 | 値 |
|---|---|---|
| `mcp` のセッション | 持っている PID + 開始時刻のプロセスが居なくなったら `interrupted` (1 秒ごとの確認)。接続が切れただけでは変えない (MCP は要求ごとに接続する) | — |
| `hook` のセッション | 最後の活動から無活動で `interrupted`。同じ `clientSessionId` の `session_begin` / `op_begin` / `report` が来たら running に戻る | 30 分 |
| 操作 | `op_end` が来ないまま打ち切りで `abandoned`。その間に窓へ入った Revision の根拠は残る | 10 分 |
| 起動時の整理 | 前の起動から残った running のセッション (プロセスの居ないもの、プロセスを持たないもの) を `interrupted`、終わっていない操作を `abandoned` | db-schema.md §6 の 5、6 |
| サービス | 接続が 0 でも、ベースライン・再走査の最中と、running のセッションがある間は無接続の時間切れで終わらない | — |

どの変化も `events` に残り (`session_interrupted` / `op_abandoned`)、作り直しで同じ状態になる (db-schema.md §5.1)。定数は `tools\track\src\session.rs` の `LifecycleConfig`、窓の猶予と `time_near` の幅は `attribution.rs`。

### 9.8 通知 (M104j)

正本は `tools\track\src\notify.rs`。

- `subscribe` (`role = editor` だけ。他は `forbidden`) は、`events` の名前 (`change_recorded` / `attribution_updated` / `coverage_gap` / `session_changed` / `shutting_down`) を検査して購読を登録する。検査が全部通ってから登録するので、失敗した要求で購読は残らない。2 回目は `bad_args` (接続と通知は続く)。結果は `{}`。
- **通知は記録の経路に触れずに作る。** 購読者がいる間だけ、読み取り専用の DB 接続が 200 ms ごとに新しい行を引き、通知にする (購読者が 0 の間は引かない)。位置は購読したときの DB の末尾で、購読より前の出来事は通知にならない (取り直しは `list` で行う)。Writer も監視も通知を待たない。通知の遅れの上限は 200 ms + 接続の送り出しの間隔 (10 ms)。
  - `change_recorded`: 新しい `change_revisions` の行ごとに 1 件。`revisionId`、`changeId`、`path`、`changeType`、`actor`、`attribution` (その時点の値。`actor` は仕様の項目に足した)。シーン / プレハブ / アクターのときは `documentId` が付く (M104l。Editor が開いている文書の変更かをすぐ判定できる。項目の追加なのでプロトコル版は上げない)。
  - `attribution_updated`: **すでに知らせた** Revision に `attribution_evidence` が足されたら 1 件 (回ごとに Revision 1 つにつき 1 件)。`revisionId`、`actor`、`attribution` (その時点の値)。同じ回で初めて知らせる Revision の根拠は `change_recorded` に含まれるので、重ねて出さない。
  - `coverage_gap`: 新しい `coverage_gaps` の行ごとに 1 件。`gapId`、`fromAt`、`toAt`、`reason`、`scope`。
  - `session_changed`: セッションの `status` が変わった (新しいセッションを含む) ごとに 1 件。`sessionId`、`status`。
  - `shutting_down`: サービスが終わるとき、購読者へ `reason` (`shutdown_op` / `idle_timeout` / `signal` ...) を送り、書き終わるのを最大 500 ms 待ってから止める。
- **購読ごとの上限つきキュー** (既定 256 件。`serve --notify-queue <n>` で変える。試験用)。あふれたら一番古い通知を捨て、次の送り出しの先頭に `resync` を 1 つ付ける (あふれの区間で 1 つ)。読まないクライアントがいても、積むだけなので Writer と監視は止まらない。サービスの接続スレッドは、読み取りで止まる前に通知を送る (同期ハンドルの `ReadFile` で止まっている間は別スレッドから `WriteFile` できないので、購読した接続は `PeekNamedPipe` で入力を見ながら 10 ms ごとに送り出しを見る)。
- プロトコル版は上げない (op の追加だけ。§7)。
- 検証: `tools\track\tests\pipe.rs` (`subscribe_is_editor_only_and_checks_its_arguments`、`subscribed_editor_receives_the_notifications`、`an_unread_subscriber_does_not_stop_recording_and_gets_one_resync`)、`notify.rs` の単体試験 (キューのあふれ、`coverage_gap` を含む引き)、`tests\track\23_subscribe.ndjson`。

### 9.9 Editor のクライアント (M104j)

`src\Editor\ChangeTracking\` (`TrackClient` / `TrackIo` / `ChangeModel` / `ChangeTrackingSession`)。**スレッドを作らず、`Poll()` は待たない。**

- 接続: `CreateFileW(FILE_FLAG_OVERLAPPED)`。`ERROR_FILE_NOT_FOUND` は起動を 1 回頼んで 500 ms おきに見に行き (10 秒の猶予)、以後は 3 秒おき。`ERROR_PIPE_BUSY` は待たずに次のフレームで開き直す (5 回続いたら 3 秒おき)。`WaitNamedPipeW` は使わない。起動は 1 接続の切れ目につき 1 回 (hello が通るたびに戻る)。`mye_track.exe` が見つからなければ `NoService` で、探し直しは `Start` し直すまで行わない。
- `hello` (`role = editor`)、`bad_token` は `service.json` を読み直して 1 回だけやり直す。結果の `projectId` が自分の計算と違えば `WrongProject`。`proto_mismatch` は `ProtoMismatch`。この 2 つは再接続しない。`db_degraded` は `DbDegraded` (3 秒おきに再接続を試す)。
- 1 行の上限は 1 MiB (超えたら切断)。1 回の `Poll()` が処理する行は 256 行・2 ms まで、読み取りは 4 回まで、`TrackIo` の呼び出しは 8 回まで (時計を除く)。残りは次のフレーム。
- 時間切れは単調時計で測る: 読み取りの op (`status` / `list` / `describe` / `sessions` / `hunks` / `scene_diff` / `blob_get`) は 30 秒、それ以外 (hello・subscribe・記録) は 5 秒。`hello` の時間切れは切断。時間切れのあとに届いた応答は捨てる。
- 設定 `changeTrackingEnabled` (既定 false) と、非対話起動 (`allowChangeTracking = !automation`) の両方が真のときだけ接続・起動する。
