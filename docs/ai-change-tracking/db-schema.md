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
  kind         TEXT NOT NULL,                   -- scene / prefab / actor
  current_path TEXT,                            -- 最後に観測したパス (消えたら NULL)
  broken_at    INTEGER,                         -- 同一性が切れた時刻 (guid が変わった、など)
  broken_note  TEXT,
  successor_id TEXT REFERENCES documents(document_id)  -- 版 3: 仮の ID から guid の文書へ引き継いだ先 (§3.3)
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
  diff_mode       TEXT NOT NULL DEFAULT 'lines',  -- lines / whole / binary / excluded / unavailable (§3.2)
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
  change_type     TEXT NOT NULL,                -- 仕様書 §7.2 の 11 種 (PrefabPropagation は sub-19) と、解析不可の unparsed (§3.3)
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
  reason     TEXT NOT NULL,                     -- service_down / watcher_overflow / disk_full / queue_overflow / write_failed / excluded / journal_unreadable
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
`changes(path)`、`scene_changes(document_id, file_id)`、(版 3 で `documents(current_path)` と `scene_changes(revision_id)` を足した)`attribution_evidence(revision_id)`、`reviews(revision_id)`、`snapshots(blob_hash)` に張る。
これに加えて `events(journal_pos)` に一意索引を張る (実装で追加。起動時の入れ直しが同じジャーナル行を二重に入れないための最後の砦)。

### 3.1 形の理由

- **Change と Revision を分ける。** 同じファイルへの変更を 1 つの Change にまとめ、観測ごとに Revision を足す。レビューと復元は Revision を指す。Revision が増えても過去のレビューは動かない (仕様書 §10.3)。
- **レビュー状態と復元状態を別の列に置く。** レビューは `reviews` の追記、復元は `changes.state` / `change_hunks.state` / `scene_changes.state`。承認済みの Revision を後で復元しても、承認の記録はそのまま残る (仕様書 §10.5)。
- **`reviews` は追記だけ。** 今の状態は「その範囲の最新の行」。Reopen は `reopened` の行を足すことで表し、過去の行を書き換えない (仕様書 §10.1)。
- **親の集約状態は保存しない。** セッション・操作・ファイルの承認状態は、子の最新の状態から問い合わせのたびに計算する。保存すると子とずれる。部分承認は計算結果として出す。
- **帰属の候補は複数持てる。** `attribution_evidence` は 1 つの Revision に何行でも付く。`change_revisions.attribution` は根拠から決めた現在の結論で、根拠が増えたら計算し直す (仕様書 §4.4)。
- **`actor` と `attribution` を分ける。** 「誰の種類か」と「どれだけ確かか」は別の軸。Editor の利用者による保存や、復元による書き込みを AI の変更と混ぜない。
- **`completeness` を Revision ごとに持つ。** 取り逃した中間状態を、完全な履歴として見せない (仕様書 §5.3、§16.1)。
- **`documents` を別表にする。** `fileId` はシーンの中でしか一意でないので、`scene_changes` は必ず `document_id` と組で引く (仕様書 §16.3「別 Scene の同じ fileId を混同しない」)。

### 3.2 Revision と Hunk の確定 (M104e)

- **`write_revision` の 1 箇所だけが書く** (`tools\track\src\revision.rs`)。Change の特定、Revision、Hunk を 1 つの SAVEPOINT に入れ、途中で失敗したら全部取り消す。Hunk の無い Revision が外から見える瞬間も、Revision だけが残ることも無い。通常の記録も、DB を作り直したあとの再構成 (§6) も、`observed` の payload から同じ入力を作ってこの関数を通る。
- **Change の寿命** (`changes.state`): 同じパスの `active` な Change があれば、そこへ Revision を足す (`seq` は連番)。次の 2 つで `superseded` にする。(1) 削除の Revision で終わった Change に、同じパスの再作成が来たとき (新しい Change を始める)。(2) リネームで元のパスが去ったとき (元のパスの `active` な Change)。`partially_restored` / `restored` / `conflicted` は sub-15 以降。
- **`diff_mode`** (Hunk の作り方):

  | 値 | 意味 | Hunk |
  |---|---|---|
  | `lines` | 行 diff の結果 (内容が同じリネームでは 0 件) | 0 件以上 |
  | `whole` | 計算量の上限を超えたので、ファイル全体を 1 Hunk にした ([architecture.md](architecture.md) §5) | 1 件 |
  | `binary` | UTF-8 でない・テキストの上限を超える | 無し |
  | `excluded` | 除外パターンのファイル (内容を持たない) | 無し |
  | `unavailable` | Before を見ていない (`no_before`)、または必要な Blob が無い・壊れている | 無し |

- **Hunk の座標**: `old_start` / `new_start` は 1 始まりの行番号で、変更範囲の最初の行。行が 0 件の側 (純粋な挿入・削除) は、その行が入る位置 (次の行の番号)。`before_text` / `after_text` / `context_*` は改行 (LF / CRLF / 末尾なし) をそのまま持つ。文脈は前後 3 行 (ファイルの端では短い)。変更の間の同じ行が 6 行以下なら 1 つの Hunk にまとめる (文脈が重なるため)。
- **`eol_only`**: Hunk の全行が、本文が同じで行末 (LF / CRLF / 末尾の改行の有無) だけ違うとき 1。本文の変更と同じ Hunk に混ざったら 0。
- **Hunk ID**: `hunk_id` は表の通し番号で、Revision が変われば変わる。`(revision_id, ordinal)` が Revision 内の位置。

### 3.3 シーン構造と文書の同一性 (M104l)

正本は `tools\track\src\scene_diff.rs` (比較。純粋な関数)、`lexjson.rs` (字句を保つ読み手)、`document.rs` (同一性)、`revision.rs` の `write_revision` (書く 1 箇所)。

- **書き方**: `scene_changes` と `documents` は Revision と同じ SAVEPOINT で書く (構造の変更の無い Revision が外から見える瞬間を作らない)。Diff ワーカーが `plan_diff` の中で比較し、Writer が書く。作り直し (§6) も同じ `plan_diff` / `write_revision` を通る。
- **種類**: 拡張子で決まる (`.scene.json` / `.prefab.json` / `.actor.json`)。`changes.kind` と `documents.kind` に入れる (`changes.kind` は `.meta` の変更で `meta`)。それ以外の JSON はただのテキスト。
- **documentId**: 作業ツリーの `<asset>.meta` の `guid` (16 桁の 16 進、小文字にそろえる。`id_source = meta`)。無い・読めない・不正なら正規化したパス (`/` 区切り) の SHA-256 の先頭 16 桁 (`id_source = path`)。サービスが自分で読む (Editor を経由しない)。
- **観測の時点の guid** は `observed` の payload の `doc_guid` に入れる (§5.1。無ければ項目を持たない)。scene / prefab / actor は Diff ワーカーが読む時点のディスクの `.meta`、`.meta` 自身の観測は書かれた内容から。作り直しはこの値から同じ `document_id` を導く。
- **結び付け** (`document::bind`): (1) 同じ guid は同じ文書。`.meta` ごと動いたリネーム・移動は `current_path` だけ更新。(2) 同じパスで仮の ID から guid へ変わったら引き継ぐ: 仮の文書の `successor_id` に guid の文書を書き、`current_path` を NULL にする。**`scene_changes` / `changes` の `document_id` は書き換えない** (履歴の行は元のまま)。たどるのは `document::resolve` で、`scene_diff` / `describe` は `resolvedDocumentId` に出す。(3) guid が変わった・`.meta` が無くなった・パスだけが変わった (guid が無い) ときは別の文書とし、古い文書に `broken_at` / `broken_note` を書いて `current_path` を NULL にする。内容が似ていても同じ文書とは決めない。(4) 削除の観測は `current_path` を NULL にするだけ。
- **`.meta` の変更**は 1 つのテキストの変更として記録され (`changes.kind = meta`)、対象のアセットの文書が既にあるときだけ、新しい guid で結び付けを見直す引き金になる。
- **構造の変更の種別**: `EntityAdded` / `EntityRemoved` (`fileId` の増減。値は断片)、`HierarchyChanged` (`parent` / `childIndex` ごとに 1 件。`field` に名前)、`ComponentAdded` / `ComponentRemoved`、`FieldModified` (`field` はコンポーネントの最上位のキー。入れ子の中だけが変わっても 1 件)、`EntityModified` (`name` / `tagMask` など)、`PrefabAssetChanged` (プレハブ / アクターは、中の変更が 1 件以上、または追加・削除のとき 1 件。`file_id` NULL)、`PrefabInstanceChanged` (`PrefabInstanceComponent.prefabHash`)、`PrefabOverrideChanged` (`overrides` は文字列の集合として比べ、増減ごとに 1 件。override 済みの `Comp.field` / `name` / `tagMask` の**値の変化はこの種別で、`FieldModified` と二重に出さない**。増減の `component` / `field` は `Comp.field` → (Comp, field)、`+Comp` → (Comp, `+`)、`-Comp` → (Comp, `-`)、`name` → (NULL, `name`)。値は集合の要素の文字列リテラル)。
- **ルート直下の変化** (`sceneName` など。`entities` と版の宣言と `nextFileId` を除く) は、`file_id = NULL`・`component = NULL` の `FieldModified` (`field` にキー)。`nextFileId` はエンティティの追加に付随するので出さない。**`DocumentModified` の種別は足さない**。
- **追加・削除**: Before の無い (追加) / After の無い (削除) 文書は、シーンは構造の変更を出さず、プレハブ / アクターは `PrefabAssetChanged` 1 件だけ。
- **解析不可**: `change_type = unparsed` の 1 行 (`file_id` NULL、`after_value` に理由の JSON 文字列。列は足さない)。理由: 空・壊れた JSON・UTF-8 でない・ルートがオブジェクトでない・`entities` が無い / 配列でない・`fileId` が無い / 整数でない / 重複・版が前後で違う・知らない版 (シーンは 1〜4 (無ければ 1)、プレハブ / アクターは `prefab` / `actor` = 1)・1 つの文書が上限 (行 diff の合計上限の半分、既定 8 MiB) を超える。ファイル単位の Revision と行 diff は残る。Blob が読めない・バイナリ・除外パスの Revision は行だけで、構造の変更も `unparsed` も無い。
- **値**: `before_value` / `after_value` は元のバイト列から切り出した JSON の断片 (数値は字句のまま。そのまま書き戻せる)。キーの重複した JSON は曖昧なので解析不可。

### 3.4 レビューの状態 (M104o)

正本は `tools\track\src\review.rs` (状態の決め方と集約は DB に触れない純粋な関数)。**スキーマは変えない** (版は 3 のまま。`reviews` は版 1 の表と `idx_reviews_revision` で足りる)。

- **記録**: `review_set` が 1 回の要求を 1 つの `events` 行 (`kind = review_set`) として記録し、対象ごとに `reviews` へ 1 行ずつ足す (`record_and_apply`)。`reviews` は派生の表で、`events` から作り直せる (§6。作り直しは `reviews` を先に消し、`review_set` のイベントを順に入れ直す。`review_id` は挿入順なので、作り直した DB は元と同じ行・同じ状態になる)。行は足すだけで、過去の行は書き換えない・消さない。`scope_ref` は `revision` → 空、`hunk` → `hunk_id`、`entity` → `fileId`、`component` → `fileId/Component`、`field` → `fileId/Component/field` (コンポーネントを持たないフィールドは `fileId//field`)。`state` は `approved` / `rejected` / `needs_review` / `reopened` (`pending` は記録しない。未確認へ戻すのは `reopened`)。
- **検査** (記録の前。1 件でも不正なら何も記録しない): Revision が存在する。行為者が `ai` か `unknown` (`editor_user` / `restore` は対象外で `bad_args`)。`hunk` は `hunk_id` がその Revision のもので、構造の変更を持たない Revision だけ (持つ Revision の葉は構造の変更なので、Hunk への記録は状態に効かず `bad_args`)。`entity` / `component` / `field` はその Revision の `scene_changes` に実在する。別の Revision の Hunk ID や存在しない範囲は `not_found`。`ref` は正規の書き方だけ (先頭の 0 は不可)。
- **葉**: 構造の変更 (`scene_changes` の行) を持つ Revision はその各行、無ければ Hunk、どちらも無い (バイナリ・除外・解析不可で行が無い・削除だけ) Revision は Revision 自身。復元の状態 (`state`) は見ない。
- **葉の今の状態**: その葉を含む範囲 (葉自身 → フィールド → コンポーネント → エンティティ → Revision) に付いた行のうち、**いちばん新しい行** (`review_id` が最大) の状態。行が無ければ `pending`、`reopened` が最新なら `pending`。**範囲の広さではなく新しさで決まる** (Revision 全体を承認したあとで 1 つの Hunk を却下すればその Hunk だけ却下、そのあと Revision 全体を承認すれば全部承認)。文書全体の変更 (`file_id` が無い行) は Revision の範囲の行だけが効く。
- **集約** (Revision / 変更 (ファイル) / 操作 / セッション。子から問い合わせのたびに計算し、保存しない):

  | 葉の状態 | 集約 |
  |---|---|
  | 全部 `pending` / 全部 `approved` / 全部 `rejected` / 全部 `needs_review` | その状態 |
  | `needs_review` が 1 つでもある (全部ではない) | `needs_review` |
  | `approved` が 1 つでもある (上に当たらない) | `partially_approved` |
  | それ以外 (`rejected` と `pending` の混在) | `partially_rejected` |

  変更・操作・セッションは、属する Revision の葉を全部集めて同じ規則で数える (件数 = 状態ごとの葉の数)。**対象は行為者が `ai` か `unknown` の Revision だけ**で、`editor_user` / `restore` の Revision は数えない (Revision 自身の集約も `null`)。1 回の集約で読む Revision は 5000 まで (超えたら `truncated`)。`list` の `review` フィルタは集約で絞り、1 回に読む Revision は 2000 まで (上限に達したら読んだ所までを返して続きの `nextCursor` を渡す)。
- **`scene_overlay`**: 今の状態が `approved` の構造の変更は強調から外す (既定。`excludeApproved: false` で戻せる)。`rejected` と `needs_review` は強調したまま。Editor の「ここまで確認済みにする」(表示だけの印) とは別。

## 4. Blob ストア

- 名前は内容の SHA-256 (hex)。同じ内容は 1 つしか持たない。
- 中身はファイルのバイト列そのまま。改行も BOM も変えない。圧縮はしない (版 1。容量が問題になったら計測してから決める)。
- 書き込みは「一時ファイルへ書く → 読み直してハッシュを確かめる → 最終名へ rename」。確かめてから `snapshots` に行を足す (仕様書 §13.3)。読み直しは書いたハンドルで行う (閉じて開き直すと、書いたばかりのファイルを OS のスキャンが開き直しの時点で検査して遅い。[performance.md](performance.md))。
- **1 件ごとの `sync_all` はしない** (M104d2。一括記録の所要時間の過半を占めた。[performance.md](performance.md))。電源断で起こりうること: 最終名のまま、大きさは合っているが中身が欠けた Blob が残る。放っておくと、同じ内容の `put` が「あった」(大きさだけで判断) と見て書き直さず、その内容の Revision が壊れた Blob を指し続ける。見つけ方は §6 の 3 の前段 (起動時に、前回の最後のイベントの 10 分前から後に書かれた Blob を読み直してハッシュを確かめ、違えば消して `blob_state = missing` にする)。**限界**: 10 分より前に書かれた Blob は確かめない (OS が書き戻し済みとみなす)。`snapshots` の行 (SQLite、`synchronous = NORMAL`) とジャーナル (`flush` だけ) も `sync` しないので、電源断で直近の記録そのものが失われうるのは今までと同じ。
- 構造比較のための解析結果 (シーン JSON を読んだもの) は Blob に入れない。`scene_changes` が結果で、原本はいつでも Blob から読み直せる。
- 除外パターンに当たるファイルは `blob_hash = NULL`、`blob_state = excluded` で、サイズと時刻だけを残す。

## 5. イベントジャーナル

- `journal\NNNNNN.ndjson` は、サービスが受け取った観測と要求を **DB へ入れる前に** 追記するログ。1 行 1 JSON。
- 役目は 2 つ。DB のトランザクションが確定する前にサービスが落ちたときの埋め直しと、DB が壊れたときの作り直しの元。
- 1 ファイルが 16 MiB を超えたら次の番号へ進む。`events.journal_pos` が DB とジャーナルの対応を持つ。
- 起動時、DB に入っている最後の位置より後ろの行を読み直して DB へ入れる。
- JSON として読めない行 (書きかけの末尾、壊れた行) は、`kind = journal_unreadable` の `events` 行にして位置を進め (次の起動で数え直さない)、前後の観測の時刻で挟んだ区間を `coverage_gaps` に `journal_unreadable` で残す。末尾が壊れているときは、起動時刻までを区間にする。
- 書きかけの行が残ったファイルへ続けて追記するときは、先に改行を足してその行を閉じる (行番号は壊れた 1 行として数える)。
- ジャーナルに Blob の中身は書かない (ハッシュだけ)。

### 5.1 監視が書くイベント (M104d)

DB を作り直したとき、`events` から `snapshots` / `changes` / `change_revisions` / `change_hunks` を導けるよう、観測はすべて `Store::record_event` を通し、次の payload を持つ (作り直しは §6 の 1)。時刻はすべて UTC ミリ秒。

| kind | payload |
|---|---|
| `service_start` | `project`、`pid`、`version`、`first_run` |
| `service_stop` | `reason` (`stdin_eof` / `signal` / `dropped` ...) |
| `baseline` | `taken_at`、`files[]` = `{path, hash, size, kind, excluded}` (最初の起動のベースライン。500 件ごとに 1 イベント) |
| `baseline_complete` | `files`、`tracked_files`、`bytes`、`changes`、`elapsed_ms`、`first_run`。これがあれば「ベースラインを取り終えた」 |
| `observed` | `path`、`change_type`、`old_path`、`before` / `after` = `{hash, size, kind[, excluded]}` または null、`completeness`、`source` (`watch` / `startup_gap` / `overflow_gap` / `verify_gap`)、`window`、`observed_at`、`doc_guid` (M104l。scene / prefab / actor と `.meta` の観測だけ。観測の時点の guid。無ければ項目を持たない) |
| `coverage_gap` | `reason`、`from_at`、`to_at`、`scope` |
| `client_hello` | pipe の `hello` を通った接続の証跡 (M104f): `role`、`pid` (pipe から得た値)、`claimedPid` (申告)、`processStartedAt` (PID のプロセスの開始時刻)。派生の表には反映しない |

セッションと操作の op (M104g) も `Store::record_event` を通し、`sessions` / `operations` / `attribution_evidence` と `change_revisions` の `actor` / `attribution` / `operation_id` は、このイベントから導ける派生の表になる。通常の記録も `rebuild_derived` も `Deriver` (`revision.rs`) → `session::apply_event` を通る。イベントに無い情報を派生の表だけに書かない。

| kind | payload |
|---|---|
| `session_begin` | `session_id`、`provider`、`source`、`client_session_id`、`workspace` (正規化した絶対パス)、`workspace_rel` (ルートからの相対。ルートなら空)、`base_commit`、`process_id` / `process_start` (`mcp` の接続だけ。pipe から得た値)、`isolated`、`worktree_eligible` (申告 + リンクされた作業ツリー + Editor 不在。開始時に決まる 3 条件)、`capabilities` |
| `session_resumed` | `session_id`、`process_id` / `process_start` (`mcp` の再接続で更新) |
| `session_end` | `session_id` |
| `session_interrupted` | `session_id`、`reason` (`process_exited` / `idle` / `startup`) |
| `op_begin` | `operation_id`、`session_id`、`operation_type`、`targets[]` = `{path, hash}` (`op_begin` で取ったハッシュ。無ければ null)、`client_op_id`、`declared_edit` = `{blob_hash, size}` または null (編集の中身は Blob)、`declared_after_hash`、`declared_path`、`begin_flushed` |
| `op_end` | `operation_id`、`result` (`ok` / `failed`) |
| `op_abandoned` | `operation_id`、`reason` (`timeout` / `startup`) |
| `reported` | `operation_id` (申告そのものの ID。`operation_type = report` の操作になる)、`session_id`、`linked_operation_id`、`paths[]` = `{path, before_hash, after_hash}` |

- `observed` の `source` に、管理された操作の読み取りとして `managed_begin` (`op_begin` の前に未記録の変更を確定する) と `managed` (`op_end` / `report` の即時の読み取り) が加わる。どちらも `completeness` は `gap` にならない。
- 派生の表の JSON 列 (列は v1 のまま): `sessions.coverage` = `{startedAt, journalPos (session_begin のジャーナルの位置)、lastActivityAt、isolated、worktreeEligible、workspaceRel、capabilities}`。`operations.reported_targets` = `{targets[], clientOpId, declaredAfterHash, declaredPath, beginFlushed}` (操作)、または `{report: {paths[]}, linkedOperationId}` (`report`)。`operations.observed_targets` は操作に結び付いた Revision の `{path, afterHash}`。`operations.declared_edit` が指す `snapshots` は `kind = declared_edit`。
- 時刻は、記録した時刻ではなくイベントの時刻 `at` を使う (作り直しで同じ値になる)。根拠の `recorded_at` も `at`。

- 除外パターンのファイルは `hash` を null にする (ハッシュも残さない)。`excluded: true`。
- `window` は、近い時刻 (デバウンスの幅以内) に届いたイベントのパスを結ぶ印。別ディレクトリへの移動は削除と作成のイベントで、同じ `window` に入る。`window` が同じで内容ハッシュも同じ削除と作成は、記録する前に 1 件の `change_type = renamed` (`old_path` 付き) にまとめる (M104e。規則は [architecture.md](architecture.md) §5)。まとめた後の 1 件だけが `observed` に入る。
- `before` は「最後に確認できた内容」で、無ければ null (新規、または `no_before`)。

## 6. 起動時の検査

0. (起動の最初) `blobs\` の下の書きかけの一時ファイル (`.tmp-*`) を消す。自分が書いたもので、どこからも指されない。サービスは 1 つだけ動く前提なので、起動時なら安全。
1. `tracking.db` を開く。開けない、または `PRAGMA quick_check` が通らなければ、`tracking.db.broken-<時刻>` へ退避し、空の DB を作って `journal\` から入れ直す。入れ直せなかった区間は `coverage_gaps` に残す。
   作り直した DB は、全イベントを入れ直したあとで `rebuild_derived` が `events` (`baseline` / `observed` とセッション・操作のイベント、`editor_saved`) を順に読み、`snapshots` / `changes` / `change_revisions` / `change_hunks` と、`documents` / `scene_changes` (M104l。Hunk と同じく Blob から計算し直す。Blob が欠けた Revision は構造の変更なし)、`sessions` / `operations` / `attribution_evidence` (M104g) を作り直す (M104e)。Hunk は Blob から計算し直す。Blob が欠けている Revision は行だけ戻し、そのスナップショットを `blob_state = missing`、Revision を `diff_mode = unavailable` (Hunk 無し) にする。通常の起動で入れ直す (DB のトランザクションが確定する前に落ちた) イベントも、同じ関数で派生の表へ反映する。作り直さないもの: `coverage_gaps` の `watcher_overflow` / `service_down` など (イベントには残るが、表へは戻さない。M104e の範囲外)。
   退避するのは SQLite が「壊れている」と返したとき (`NOTADB` / `CORRUPT` / `quick_check` 不合格) だけ。ロックなどで開けないときは退避せず `db_degraded` で動く。`-wal` / `-shm` も同じ接尾辞で連れていく。接尾辞の時刻は UTC の `YYYYMMDDTHHMMSSmmmZ`。
2. `schema_migrations` を見て、足りない移行を順に当てる。DB の版が自分より新しければ、書き込まずに `db_degraded` で動く。
3. (前段) 前回の最後のイベントの 10 分前から後に書かれた Blob を読み、ハッシュが名前と違えば消す (§4)。続けて、`snapshots` が指す Blob の有無を確かめ、無ければ `blob_state = missing` にする (前段で消したものを含む)。
4. どの `snapshots` からも指されていない Blob を数える。消すのは §7 の回収のときだけで、起動時には消さない。
5. `sessions.status = running` のまま残っている行を、プロセスの生死 (PID と開始時刻) で確かめ、居なければ `interrupted` にする。
   PID が記録されていない行も `interrupted` にする (生死を確かめられない)。プロセスの同一性は PID と開始時刻 (誤差 2 秒) の組。
6. `operations.finished_at` が NULL のまま残っている行を `abandoned` にする (`result = abandoned`。M104g から、5 と 6 は表を直接書き換えず `session_interrupted` / `op_abandoned` のイベントにして反映する。`finished_at` は整理した時刻)。
7. 前回の `service_stop` (無ければ最後のイベント) から今までを `coverage_gaps` に `service_down` で足し、ベースラインを取り直す。

どの段で失敗しても、サービスは「記録できない状態」で pipe には答え続ける。Editor の起動は待たせない。3〜6 で失敗した段と理由は `status` の `db.failedSteps` に出る ([ipc-contract.md](ipc-contract.md) §9.4)。

**制限** (M104f): Hunk の書き込みに失敗した観測は `events` に残る。その後に DB を作り直す (1) と、`events` から Revision を作り直す一方、検証の走査が足した `gap` の Revision も残るので、同じ変更が 2 件の Revision になりうる。重複は消さない (履歴の欠落よりは重複を選ぶ)。

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

**確認済み** (試作 2、2026-11-14。[prototype-results.md](prototype-results.md)): 開発機 (rustc 1.98、MSVC 19.51) で `rusqlite` 0.42 の `bundled` が edition 2021 のままビルドでき、`journal_mode=WAL` / `synchronous=NORMAL` / `foreign_keys=ON` で読み書きできた。
書き込みトランザクションを開いたまま別接続から読め、未コミットの行は見えない。外部キー違反は拒否され、`integrity_check` は ok。まっさらなデバッグビルドは 8.8 秒、`target` は 97 MiB。
最後の接続を閉じると `-wal` / `-shm` は消える。

**決定**: 依存は `rusqlite = { version = "0.42", features = ["bundled"] }` と `sha2 = "0.11"` (出力は `{:x}` で整形できないので 16 進化は自前)。この 2 つが要求する Rust の最低版は 1.89 なので、`tools\track\Cargo.toml` に `rust-version = "1.89"` を明示する (collab の「古い stable でも通る」方針は track では成り立たない)。

**未検証**: CI のランナー (`windows-2022`、Visual Studio 2022) で `bundled` がビルドできること。push しないと確かめられないので (spec D3)、sub-05 (M104f) で `.github\workflows\ci.yml` に `tools\track` の段 (cargo test → `build_track.bat` → `track_verify.bat`) を足した。**CI での実走は未確認**で、最初に push した後の実行が確認になる。通らなければ `CC` 環境変数で C コンパイラを指定する。
**未検証**: 最低の Rust (1.89) での実ビルド。開発機の rustc は 1.98 だけで、1.89 のツールチェーンは足していない (ユーザーの判断、2026-11-15)。`rust-version = "1.89"` は依存 (rusqlite 0.42、sha2 0.11) の要求から読んだ値。

**版 2** (M104k2): `events(kind, at)` の索引 `idx_events_kind_at` だけを足した (`schema\v2.sql`)。表と列は変えない。`editor_saved` の知らせを観測の時刻の前後で引くため (全イベントを走査しない)。
`editor_saved` のイベントの payload は `{ path, hash }` (path はプロジェクトルート基準、`/` 区切り。`at` は受けた時刻)。作り直し (§6 の 1) はこのイベントも順に読み、観測が先でも後でも同じ `attribution_evidence` (`kind = editor_saved`、`session_id` / `operation_id` は NULL) になる。

**版 3** (M104l): `documents.successor_id` (仮の ID の文書から guid の文書への引き継ぎ、§3.3) と、`idx_documents_current_path`、`idx_scene_changes_revision` を足した (`schema\v3.sql`)。

**スキーマの凍結** (M104f 以降): `tools\track\src\schema\v1.sql` はこのサブのコミット以降、書き換えない。表や列を変えるときは `schema\mod.rs` の `MIGRATIONS` へ移行 (v2 以降) を足す (上の方針どおり、旧版の DB は移行の前に `.bak-v<旧版>` へコピーされる)。
