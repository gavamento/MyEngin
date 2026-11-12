# AI Change Tracking: DB スキーマと保存形式

- 状態: 設計のみ (2026-11-13)。スキーマ版 1 の案
- 全体像は [architecture.md](architecture.md)。帰属の値の意味は [attribution-rules.md](attribution-rules.md)、復元の状態は [restore-safety.md](restore-safety.md)

## 1. 置き場所と所有者

```
<project>\.mye\ai_changes\
  tracking.db            SQLite 本体 (+ -wal、-shm)
  service.json           稼働中のサービスの PID・開始時刻・pipe 名・トークン
  blobs\ab\cdef...       内容アドレスの Blob (先頭 2 桁でディレクトリを分ける)
  journal\000001.ndjson  受け取った観測の追記ログ
  recovery\<restoreId>\  復元ジャーナル (Editor が所有。restore-safety.md §5)
```

- `recovery\` 以外を書くのは追跡サービスだけ。
- `.mye\` は既にエンジンのリポジトリ (`.gitignore`) とプロジェクトのテンプレート (`ProjectTemplates.cpp`) で除外されている。collab のファイル監視も `.mye` を無視するので、DB の更新が git の status 更新を誘発しない。追加の設定は要らない。
- プロジェクトなしの起動 (`projectRoot` が無い) では追跡しない。

## 2. SQLite の設定

| 設定 | 値 | 理由 |
|---|---|---|
| `journal_mode` | `WAL` | 仕様書 §13.3。書き込み中も読み取りを止めない |
| `synchronous` | `NORMAL` | プロセスが落ちても DB は壊れない。電源断では直前の数件が消えうるが、`journal\` から埋め直す |
| `foreign_keys` | `ON` | 参照の切れた行を作らない |
| 接続 | Writer スレッドに書き込み用 1 本。読み取り用は別に持つ | 単一の書き手 (仕様書 §13.3) |
| 時刻 | UTC のミリ秒 (整数) | 並べ替えと区間の計算を整数で行う |
| ID | セッション・操作・復元は UUID の文字列。それ以外は整数の連番 | セッション ID は外へ渡す値なので推測しにくくする |

## 3. テーブル

仕様書 §13.2 の 12 表に、文書の同一性と欠落区間のための 2 表 (`documents`、`coverage_gaps`) を足す。
以下は設計上の形で、実装時の正本は `tools\track\src\schema\` の移行スクリプトにする。

```sql
CREATE TABLE schema_migrations (
  version     INTEGER PRIMARY KEY,
  applied_at  INTEGER NOT NULL,
  note        TEXT    NOT NULL
);

CREATE TABLE sessions (
  session_id        TEXT PRIMARY KEY,           -- UUID
  provider          TEXT NOT NULL,              -- claude / codex / gemini / other
  source            TEXT NOT NULL,              -- terminal / mcp / external
  client_session_id TEXT,                       -- クライアント側の ID (任意)
  process_id        INTEGER,
  process_start     INTEGER,                    -- プロセスの開始時刻。PID と組で識別する
  workspace         TEXT NOT NULL,              -- 正規化した絶対パス
  base_commit       TEXT,                       -- 開始時の HEAD (git でなければ NULL)
  started_at        INTEGER NOT NULL,
  ended_at          INTEGER,
  status            TEXT NOT NULL,              -- running / finished / interrupted
  coverage          TEXT NOT NULL               -- JSON: 追跡できた経路と欠落
);

CREATE TABLE operations (
  operation_id     TEXT PRIMARY KEY,            -- UUID
  session_id       TEXT NOT NULL REFERENCES sessions(session_id),
  operation_type   TEXT NOT NULL,               -- edit / write / delete / rename / report / other
  started_at       INTEGER NOT NULL,
  finished_at      INTEGER,
  reported_targets TEXT NOT NULL,               -- JSON: 申告されたパスとハッシュ
  observed_targets TEXT NOT NULL,               -- JSON: 観測されたパスとハッシュ
  declared_edit    INTEGER REFERENCES snapshots(snapshot_id),  -- フックが渡した編集内容
  result           TEXT                         -- ok / failed / abandoned
);

CREATE TABLE documents (
  document_id  TEXT PRIMARY KEY,                -- .meta の guid、またはパスのハッシュ
  id_source    TEXT NOT NULL,                   -- meta / path
  kind         TEXT NOT NULL,                   -- scene / prefab / actor / other
  current_path TEXT,                            -- 最後に観測したパス (消えたら NULL)
  broken_at    INTEGER,                         -- 同一性が切れた時刻 (guid が変わった、など)
  broken_note  TEXT
);

CREATE TABLE changes (
  change_id    INTEGER PRIMARY KEY,
  path         TEXT NOT NULL,                   -- ルートからの相対、'/' 区切り
  document_id  TEXT REFERENCES documents(document_id),
  kind         TEXT NOT NULL,                   -- text / binary / scene / prefab / meta
  first_seen   INTEGER NOT NULL,
  state        TEXT NOT NULL                    -- active / superseded / partially_restored / restored / conflicted
);

CREATE TABLE change_revisions (
  revision_id     INTEGER PRIMARY KEY,
  change_id       INTEGER NOT NULL REFERENCES changes(change_id),
  seq             INTEGER NOT NULL,             -- change 内の通し番号
  change_type     TEXT NOT NULL,                -- added / modified / deleted / renamed
  old_path        TEXT,                         -- renamed のときだけ
  before_snapshot INTEGER REFERENCES snapshots(snapshot_id),  -- 無ければ NULL (新規、または見ていない)
  after_snapshot  INTEGER REFERENCES snapshots(snapshot_id),  -- 無ければ NULL (削除)
  observed_at     INTEGER NOT NULL,
  completeness    TEXT NOT NULL,                -- complete / coalesced / gap / no_before
  operation_id    TEXT REFERENCES operations(operation_id),   -- 結び付いた操作 (無ければ NULL)
  actor           TEXT NOT NULL,                -- ai / editor_user / restore / unknown
  attribution     TEXT NOT NULL,                -- verified / claimed / inferred / unknown
  caused_by       INTEGER REFERENCES change_revisions(revision_id),  -- プレハブ伝播の原因
  UNIQUE (change_id, seq)
);

CREATE TABLE change_hunks (
  hunk_id     INTEGER PRIMARY KEY,
  revision_id INTEGER NOT NULL REFERENCES change_revisions(revision_id),
  ordinal     INTEGER NOT NULL,                 -- Revision 内の通し番号
  old_start   INTEGER NOT NULL,
  old_count   INTEGER NOT NULL,
  new_start   INTEGER NOT NULL,
  new_count   INTEGER NOT NULL,
  before_text TEXT NOT NULL,
  after_text  TEXT NOT NULL,
  context_pre  TEXT NOT NULL,                   -- 前の文脈 (最大 3 行)
  context_post TEXT NOT NULL,                   -- 後ろの文脈 (最大 3 行)
  eol_only    INTEGER NOT NULL DEFAULT 0,       -- 改行だけの変更か
  state       TEXT NOT NULL DEFAULT 'active',   -- active / restored / conflicted
  UNIQUE (revision_id, ordinal)
);

CREATE TABLE scene_changes (
  scene_change_id INTEGER PRIMARY KEY,
  revision_id     INTEGER NOT NULL REFERENCES change_revisions(revision_id),
  document_id     TEXT NOT NULL REFERENCES documents(document_id),
  file_id         INTEGER,                      -- エンティティの fileId (文書全体の変更なら NULL)
  change_type     TEXT NOT NULL,                -- 仕様書 §7.2 の 11 種
  component       TEXT,
  field           TEXT,
  before_value    TEXT,                         -- JSON の字句のまま
  after_value     TEXT,
  caused_by       INTEGER REFERENCES scene_changes(scene_change_id),
  state           TEXT NOT NULL DEFAULT 'active'
);

CREATE TABLE attribution_evidence (
  evidence_id  INTEGER PRIMARY KEY,
  revision_id  INTEGER NOT NULL REFERENCES change_revisions(revision_id),
  session_id   TEXT REFERENCES sessions(session_id),          -- 候補のセッション (無ければ NULL)
  operation_id TEXT REFERENCES operations(operation_id),
  kind         TEXT NOT NULL,                   -- attribution-rules.md §2 の種類
  detail       TEXT NOT NULL,                   -- JSON
  recorded_at  INTEGER NOT NULL
);

CREATE TABLE snapshots (
  snapshot_id INTEGER PRIMARY KEY,
  blob_hash   TEXT,                             -- SHA-256 の hex。内容を持たない記録は NULL
  size        INTEGER NOT NULL,
  path        TEXT NOT NULL,
  kind        TEXT NOT NULL,                    -- disk / editor / baseline / declared_edit / pre_restore
  taken_at    INTEGER NOT NULL,
  blob_state  TEXT NOT NULL                     -- present / excluded / evicted / missing
);

CREATE TABLE reviews (
  review_id   INTEGER PRIMARY KEY,
  revision_id INTEGER NOT NULL REFERENCES change_revisions(revision_id),
  scope_kind  TEXT NOT NULL,                    -- revision / hunk / entity / component / field
  scope_ref   TEXT NOT NULL,                    -- hunk_id、または "fileId/Component/field"
  state       TEXT NOT NULL,                    -- pending / approved / rejected / needs_review / reopened
  note        TEXT,
  decided_at  INTEGER NOT NULL
);

CREATE TABLE restore_operations (
  restore_id   TEXT PRIMARY KEY,                -- UUID。recovery\<restoreId>\ と同じ
  state        TEXT NOT NULL,                   -- prepared / applying / committed / rolling_back / recovery_required / rolled_back
  targets      TEXT NOT NULL,                   -- JSON: 対象 (revision、hunk、entity、field)
  pre_hashes   TEXT,                            -- JSON: 適用前の内容ハッシュ
  post_hashes  TEXT,                            -- JSON: 適用後の内容ハッシュ
  error        TEXT,
  started_at   INTEGER NOT NULL,
  finished_at  INTEGER
);

CREATE TABLE coverage_gaps (
  gap_id     INTEGER PRIMARY KEY,
  from_at    INTEGER NOT NULL,
  to_at      INTEGER NOT NULL,
  reason     TEXT NOT NULL,                     -- service_down / watcher_overflow / disk_full / queue_overflow / excluded
  scope      TEXT                               -- 影響したパス (全体なら NULL)
);

CREATE TABLE events (
  event_id    INTEGER PRIMARY KEY,
  at          INTEGER NOT NULL,
  kind        TEXT NOT NULL,                    -- observed / reported / op_begin / op_end / editor_saved / service_start / service_stop / ...
  journal_pos TEXT NOT NULL,                    -- journal\ のファイル名と行番号
  payload     TEXT NOT NULL                     -- JSON
);
```

索引は `change_revisions(change_id, seq)`、`change_revisions(observed_at)`、`change_revisions(operation_id)`、
`changes(path)`、`scene_changes(document_id, file_id)`、`attribution_evidence(revision_id)`、`reviews(revision_id)`、`snapshots(blob_hash)` に張る。

### 3.1 形の理由

- **Change と Revision を分ける。** 同じファイルへの変更を 1 つの Change にまとめ、観測ごとに Revision を足す。レビューと復元は Revision を指す。Revision が増えても過去のレビューは動かない (仕様書 §10.3)。
- **レビュー状態と復元状態を別の列に置く。** レビューは `reviews` の追記、復元は `changes.state` / `change_hunks.state` / `scene_changes.state`。承認済みの Revision を後で復元しても、承認の記録はそのまま残る (仕様書 §10.5)。
- **`reviews` は追記だけ。** 今の状態は「その範囲の最新の行」。Reopen は `reopened` の行を足すことで表し、過去の行を書き換えない (仕様書 §10.1)。
- **親の集約状態は保存しない。** セッション・操作・ファイルの承認状態は、子の最新の状態から問い合わせのたびに計算する。保存すると子とずれる。部分承認は計算結果として出す。
- **帰属の候補は複数持てる。** `attribution_evidence` は 1 つの Revision に何行でも付く。`change_revisions.attribution` は根拠から決めた現在の結論で、根拠が増えたら計算し直す (仕様書 §4.4)。
- **`actor` と `attribution` を分ける。** 「誰の種類か」と「どれだけ確かか」は別の軸。Editor の利用者による保存や、復元による書き込みを AI の変更と混ぜない。
- **`completeness` を Revision ごとに持つ。** 取り逃した中間状態を、完全な履歴として見せない (仕様書 §5.3、§16.1)。
- **`documents` を別表にする。** `fileId` はシーンの中でしか一意でないので、`scene_changes` は必ず `document_id` と組で引く (仕様書 §16.3「別 Scene の同じ fileId を混同しない」)。

## 4. Blob ストア

- 名前は内容の SHA-256 (hex)。同じ内容は 1 つしか持たない。
- 中身はファイルのバイト列そのまま。改行も BOM も変えない。圧縮はしない (版 1。容量が問題になったら計測してから決める)。
- 書き込みは「一時ファイルへ書く → 読み直してハッシュを確かめる → 最終名へ rename」。確かめてから `snapshots` に行を足す (仕様書 §13.3)。
- 構造比較のための解析結果 (シーン JSON を読んだもの) は Blob に入れない。`scene_changes` が結果で、原本はいつでも Blob から読み直せる。
- 除外パターンに当たるファイルは `blob_hash = NULL`、`blob_state = excluded` で、サイズと時刻だけを残す。

## 5. イベントジャーナル

- `journal\NNNNNN.ndjson` は、サービスが受け取った観測と要求を **DB へ入れる前に** 追記するログ。1 行 1 JSON。
- 役目は 2 つ。DB のトランザクションが確定する前にサービスが落ちたときの埋め直しと、DB が壊れたときの作り直しの元。
- 1 ファイルが 16 MiB を超えたら次の番号へ進む。`events.journal_pos` が DB とジャーナルの対応を持つ。
- 起動時、DB に入っている最後の位置より後ろの行を読み直して DB へ入れる。
- ジャーナルに Blob の中身は書かない (ハッシュだけ)。

## 6. 起動時の検査

1. `tracking.db` を開く。開けない、または `PRAGMA quick_check` が通らなければ、`tracking.db.broken-<時刻>` へ退避し、空の DB を作って `journal\` から入れ直す。入れ直せなかった区間は `coverage_gaps` に残す。
2. `schema_migrations` を見て、足りない移行を順に当てる。DB の版が自分より新しければ、書き込まずに `db_degraded` で動く。
3. `snapshots` が指す Blob の有無を確かめ、無ければ `blob_state = missing` にする。
4. どの `snapshots` からも指されていない Blob を数える。消すのは §7 の回収のときだけで、起動時には消さない。
5. `sessions.status = running` のまま残っている行を、プロセスの生死 (PID と開始時刻) で確かめ、居なければ `interrupted` にする。
6. `operations.finished_at` が NULL のまま残っている行を `abandoned` にする。
7. 前回の `service_stop` (無ければ最後のイベント) から今までを `coverage_gaps` に `service_down` で足し、ベースラインを取り直す。

どの段で失敗しても、サービスは「記録できない状態」で pipe には答え続ける。Editor の起動は待たせない。

## 7. 容量と保存期間

- 設定は 2 つ: Blob の総量の上限 (既定 2 GiB) と保存期間 (既定 90 日)。値は `<project>\.mye\editor_settings.json` に置き、Editor が `hello` の後でサービスへ渡す。
- 回収の順番:
  1. どこからも指されていない Blob。
  2. 保存期間を過ぎ、かつ `restored` か `superseded` になった Revision の Blob。
  3. 保存期間を過ぎた Revision の Blob (古い順)。
- **消さないもの**: `pending` か `needs_review` の Revision の Before と After、進行中の復元の `pre_restore`、最新のベースライン。
- 上限に達しても消せるものが無いときは、新しい Blob の保存を止めて `coverage_gaps` に `disk_full` を残す。既存の行は触らない。
- Blob を消しても `snapshots` の行は残し、`blob_state = evicted` にする。一覧と Hunk の本文 (DB にある) は見られるが、ファイル全体の復元はできなくなる。
- 2 と 3 で復元できなくなる Revision があるときは、Editor が件数を出して確認を取ってから回収する。サービスが黙って消すのは 1 だけ。

## 8. スキーマの移行

- 版は整数。移行は前へ進むだけ (戻す移行は書かない)。
- 移行の前に `tracking.db` を `tracking.db.bak-v<旧版>` へコピーする。
- 移行は 1 トランザクション。失敗したら元のまま `db_degraded` で動く。
- スキーマ版はプロトコル版 ([ipc-contract.md](ipc-contract.md) §7) とは別に数える。

**未検証**: `rusqlite` の bundled が開発機と CI のランナーでビルドできること (C コンパイラが要る)。試作で確かめる。
