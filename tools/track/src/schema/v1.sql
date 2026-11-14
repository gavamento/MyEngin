-- スキーマ版 1。正本は docsi-change-tracking\db-schema.md §3 と同じ形で、このファイルが実装の正。
-- 版は schema/mod.rs の MIGRATIONS で 1 つずつ前へ進める (戻す移行は書かない)。

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
  diff_mode       TEXT NOT NULL DEFAULT 'lines',  -- lines / whole (上限超えで全体 1 Hunk) / binary / excluded / unavailable (後ろ 3 つは Hunk 無し)
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

CREATE INDEX idx_change_revisions_change_seq ON change_revisions(change_id, seq);
CREATE INDEX idx_change_revisions_observed_at ON change_revisions(observed_at);
CREATE INDEX idx_change_revisions_operation ON change_revisions(operation_id);
CREATE INDEX idx_changes_path ON changes(path);
CREATE INDEX idx_scene_changes_document_file ON scene_changes(document_id, file_id);
CREATE INDEX idx_attribution_evidence_revision ON attribution_evidence(revision_id);
CREATE INDEX idx_reviews_revision ON reviews(revision_id);
CREATE INDEX idx_snapshots_blob_hash ON snapshots(blob_hash);

-- 起動時の入れ直しを冪等にする (同じジャーナル行を二重に入れない)
CREATE UNIQUE INDEX idx_events_journal_pos ON events(journal_pos);
