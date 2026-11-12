# AI Change Tracking: IPC 契約

- 状態: 設計のみ (2026-11-13)。プロトコル版 1 の案
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

**未検証**: Rust から上の DACL とリモート拒否を張れること。Win32 API を呼ぶ手段は 2 案ある。
`tools\collab\src\git.rs` のように `extern "system"` を自前で宣言するか、`windows-sys` を依存に足すか。試作で比べて決める。

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
| `status` | 全部 | なし | 稼働時間、追跡中のファイル数、Blob の総量、欠落区間、DB の健全性、直近の処理時間 |
| `subscribe` | editor | `events`: 受け取りたい通知の名前 | なし |

### 4.2 セッションと操作

| op | 役割 | 引数 | 結果 |
|---|---|---|---|
| `session_begin` | mcp, hook | `provider`, `workspace`, `clientSessionId?`, `source` | `sessionId`, `status`, `trackingCapabilities`, `baseCommit` |
| `session_end` | mcp, hook | `sessionId` | `status`, `changeCount` |
| `op_begin` | mcp, hook | `sessionId`, `operationType`, `targets`: パスの配列, `declaredEdit?` | `operationId`, 各パスの現在のハッシュ |
| `op_end` | mcp, hook | `sessionId`, `operationId`, `result` | 結び付いた変更の ID と帰属 |
| `report` | mcp | `sessionId`, `operationId?`, `paths`, `beforeHashes`, `afterHashes` | 各パスの照合結果 (一致 / 不一致 / 観測なし) と帰属 |

- `baseCommit` はサービスが自分で `git rev-parse HEAD` 相当を読んで決める (`.git\HEAD` と参照ファイルを読むだけで、git を起動しない)。git のリポジトリでなければ空。
- `op_begin` を受けた時点で、サービスは対象パスの内容を Blob に保存する。これが管理された操作の Before になる (仕様書 §5.2)。
- `declaredEdit` はフックが渡す編集内容 (置換前後の文字列、または書き込む全文)。帰属の Verified 判定に使う ([attribution-rules.md](attribution-rules.md) §3)。
- `report` は MCP の `change_report` の中身。申告と観測を突き合わせた結果を返すので、AI は自分の申告が通ったかどうかを知れる。

### 4.3 取得

| op | 役割 | 引数 | 結果 |
|---|---|---|---|
| `list` | mcp, editor | フィルタ (`sessionId`, `path`, `from`, `to`, `review`, `attribution`, `changeType`)、`cursor?`, `limit` | 変更の要約の配列、`nextCursor` |
| `describe` | mcp, editor | `revisionId` | Before / After のハッシュとサイズ、Hunk の要約、シーン変更の要約、帰属と根拠、レビュー状態、復元可否、競合状態 |
| `hunks` | editor | `revisionId`, `cursor?` | Hunk の配列 (行の本文付き) |
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

### 4.5 通知

| event | いつ | 中身 |
|---|---|---|
| `change_recorded` | Revision が確定した | `revisionId`, `changeId`, `path`, `changeType`, `attribution` |
| `attribution_updated` | あとから帰属が変わった (報告が届いた、など) | `revisionId`, 新しい帰属 |
| `coverage_gap` | 取り逃しや停止区間を記録した | 区間、理由 |
| `session_changed` | セッションの開始・終了・中断 | `sessionId`, `status` |
| `shutting_down` | サービスが終了する | 理由 |

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
