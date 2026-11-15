//! Revision の確定と、イベントからの再構成 (architecture.md §5、db-schema.md §3)。
//!
//! Revision と Hunk は `write_revision` の 1 箇所だけが書く。通常の記録 (observe.rs) も、DB を作り直したあとの
//! 再構成 (`rebuild_derived`) も、観測の payload から `RevisionInput` を作ってこの関数へ渡す。
//! 2 通りの実装を持たないので、作り直した DB は元の DB と同じ Change / Revision / Hunk になる。

use std::collections::HashMap;

use rusqlite::{params, Connection, OptionalExtension};
use serde_json::Value;

use crate::attribution::{self, AttributionConfig};
use crate::baseline::ContentKind;
use crate::blob::BlobStore;
use crate::diff::{diff_lines, DiffLimits, Hunk};
use crate::document;
use crate::observe::{ChangeType, Completeness};
use crate::scene_diff::{self, DocKind, SceneOutcome};
use crate::session;
use crate::store::Store;

/// Revision の Hunk の作り方 (`change_revisions.diff_mode`)。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiffMode {
    /// 行 diff の結果 (リネームだけなど、Hunk が 0 件のこともある)。
    Lines,
    /// 上限を超えたので、ファイル全体を 1 Hunk にした。
    Whole,
    /// バイナリ (UTF-8 でない・上限を超える)。ファイル単位の記録だけで Hunk は無い。
    Binary,
    /// 除外パターンのファイル。内容を持たないので Hunk は無い。
    Excluded,
    /// 必要な内容 (Blob) を読めない、または Before を見ていない。Hunk は無い。
    Unavailable,
}

impl DiffMode {
    pub fn as_str(self) -> &'static str {
        match self {
            DiffMode::Lines => "lines",
            DiffMode::Whole => "whole",
            DiffMode::Binary => "binary",
            DiffMode::Excluded => "excluded",
            DiffMode::Unavailable => "unavailable",
        }
    }
}

/// 観測の Before / After 側の内容の記述 (payload の `before` / `after`)。
#[derive(Clone, Debug)]
pub struct StateRef {
    /// Blob のハッシュ。除外パスは None。
    pub hash: Option<String>,
    pub size: u64,
    pub kind: Option<ContentKind>,
    pub excluded: bool,
}

/// Revision を作るのに要る情報。`observed` イベントの payload から作る。
#[derive(Clone, Debug)]
pub struct RevisionInput {
    pub path: String,
    pub change_type: ChangeType,
    pub old_path: Option<String>,
    pub before: Option<StateRef>,
    pub after: Option<StateRef>,
    pub completeness: Completeness,
    pub observed_at: i64,
    /// 観測の時点の `.meta` の guid (scene / prefab / actor と `.meta` の観測だけ。無ければ None)。
    pub doc_guid: Option<String>,
}

impl RevisionInput {
    /// `observed` イベントの payload から作る。読めなければ None。
    /// Before の除外は、payload が除外の印を持たないので、Before のパスで判定し直す。
    pub fn from_payload(blobs: &BlobStore, payload: &Value) -> Option<RevisionInput> {
        let path = payload["path"].as_str()?.to_string();
        let old_path = payload["old_path"].as_str().map(str::to_string);
        let before_path = old_path.clone().unwrap_or_else(|| path.clone());
        let state = |value: &Value, excluded: bool| -> Option<StateRef> {
            value.is_object().then(|| StateRef {
                hash: value["hash"].as_str().map(str::to_string),
                size: value["size"].as_u64().unwrap_or(0),
                kind: value["kind"].as_str().and_then(ContentKind::parse),
                excluded,
            })
        };
        Some(RevisionInput {
            change_type: ChangeType::parse(payload["change_type"].as_str()?)?,
            completeness: Completeness::parse(payload["completeness"].as_str()?)?,
            observed_at: payload["observed_at"].as_i64()?,
            doc_guid: payload["doc_guid"].as_str().map(str::to_string),
            before: state(&payload["before"], blobs.is_excluded(&before_path)),
            after: state(&payload["after"], payload["after"]["excluded"].as_bool().unwrap_or(false)),
            path,
            old_path,
        })
    }
}

/// 差分の計算結果。Revision と一緒に確定する。
#[derive(Clone, Debug)]
pub struct DiffPlan {
    pub mode: DiffMode,
    pub hunks: Vec<Hunk>,
    /// scene / prefab / actor の構造比較の結果 (それ以外・比べられない状態では None)。
    pub scene: Option<SceneOutcome>,
    /// Diff ワーカーが作業ツリーの `.meta` から読んだ guid。Writer が `observed` の payload (`doc_guid`) へ入れる。
    pub doc_guid: Option<String>,
}

impl DiffPlan {
    fn without_hunks(mode: DiffMode) -> DiffPlan {
        DiffPlan { mode, hunks: Vec::new(), scene: None, doc_guid: None }
    }
}

/// 構造比較 (scene / prefab / actor の拡張子のときだけ)。片側しか無い (追加・削除) ときは文書全体の扱い。
fn scene_outcome(input: &RevisionInput, before: &[u8], after: &[u8], limits: &DiffLimits) -> Option<SceneOutcome> {
    let kind = DocKind::from_path(&input.path)?;
    match (input.before.is_some(), input.after.is_some()) {
        // 行 diff の上限は Before と After の合計。1 つの文書はその半分まで
        (true, true) => Some(scene_diff::diff(kind, before, after, limits.max_bytes / 2)),
        (false, false) => None,
        _ => Some(scene_diff::whole_file(kind)),
    }
}

/// Blob を読む。無い・壊れている (Ok(None)) と、読み取りの失敗 (Err) を分ける。
fn read_blob(blobs: &BlobStore, hash: &str) -> Result<Option<Vec<u8>>, String> {
    match blobs.read(hash) {
        Ok(bytes) => Ok(Some(bytes)),
        Err(e) if matches!(e.kind(), std::io::ErrorKind::NotFound | std::io::ErrorKind::InvalidData) => Ok(None),
        Err(e) => Err(format!("Blob を読めない ({hash}): {e}")),
    }
}

/// Before / After の内容から Hunk を作る。`after_bytes` は呼び出し側が持っているときだけ (無ければ Blob から読む)。
/// Err は計算できなかった (一時的な読み取りの失敗)。呼び出し側は Revision を確定させず、欠落として扱う。
pub fn plan_diff(
    blobs: &BlobStore,
    input: &RevisionInput,
    after_bytes: Option<&[u8]>,
    limits: &DiffLimits,
) -> Result<DiffPlan, String> {
    let excluded = |s: &Option<StateRef>| s.as_ref().is_some_and(|s| s.excluded);
    if excluded(&input.before) || excluded(&input.after) {
        return Ok(DiffPlan::without_hunks(DiffMode::Excluded));
    }
    let binary = |s: &Option<StateRef>| s.as_ref().is_some_and(|s| s.kind == Some(ContentKind::Binary));
    if binary(&input.before) || binary(&input.after) {
        return Ok(DiffPlan::without_hunks(DiffMode::Binary));
    }
    // Before を見ていない (no_before) 変更は、全行が追加されたように見せない
    if input.before.is_none() && input.change_type != ChangeType::Added {
        return Ok(DiffPlan::without_hunks(DiffMode::Unavailable));
    }

    let mut before_bytes: Vec<u8> = Vec::new();
    if let Some(state) = &input.before {
        let Some(hash) = &state.hash else { return Ok(DiffPlan::without_hunks(DiffMode::Unavailable)) };
        match read_blob(blobs, hash)? {
            Some(bytes) => before_bytes = bytes,
            None => return Ok(DiffPlan::without_hunks(DiffMode::Unavailable)),
        }
    }
    let after_owned: Vec<u8>;
    let after_view: &[u8] = match (&input.after, after_bytes) {
        (None, _) => &[],
        (Some(_), Some(bytes)) => bytes,
        (Some(state), None) => {
            let Some(hash) = &state.hash else { return Ok(DiffPlan::without_hunks(DiffMode::Unavailable)) };
            match read_blob(blobs, hash)? {
                Some(bytes) => {
                    after_owned = bytes;
                    &after_owned
                }
                None => return Ok(DiffPlan::without_hunks(DiffMode::Unavailable)),
            }
        }
    };
    let scene = scene_outcome(input, &before_bytes, after_view, limits);
    let (Ok(before_text), Ok(after_text)) = (std::str::from_utf8(&before_bytes), std::str::from_utf8(after_view)) else {
        return Ok(DiffPlan { scene, ..DiffPlan::without_hunks(DiffMode::Binary) });
    };
    let diff = diff_lines(before_text, after_text, limits);
    Ok(DiffPlan {
        mode: if diff.whole_file { DiffMode::Whole } else { DiffMode::Lines },
        hunks: diff.hunks,
        scene,
        doc_guid: None,
    })
}

/// Revision と Hunk を 1 つの塊として確定する。途中で失敗したら全部取り消す (Hunk の無い Revision を作らない)。
/// 戻り値は `revision_id`。
///
/// Change は同じパスの active なものを使い、無ければ作る。次の場合は前の Change を `superseded` にする:
/// 削除のあとの再作成 (新しい Change を始める)、リネームで元のパスが去ったとき (元のパスの active な Change)。
pub fn write_revision(
    conn: &Connection,
    input: &RevisionInput,
    before_snapshot: Option<i64>,
    after_snapshot: Option<i64>,
    plan: &DiffPlan,
) -> rusqlite::Result<i64> {
    conn.execute_batch("SAVEPOINT write_revision")?;
    match write_revision_rows(conn, input, before_snapshot, after_snapshot, plan) {
        Ok(id) => {
            conn.execute_batch("RELEASE write_revision")?;
            Ok(id)
        }
        Err(e) => {
            let _ = conn.execute_batch("ROLLBACK TO write_revision; RELEASE write_revision");
            Err(e)
        }
    }
}

fn write_revision_rows(
    conn: &Connection,
    input: &RevisionInput,
    before_snapshot: Option<i64>,
    after_snapshot: Option<i64>,
    plan: &DiffPlan,
) -> rusqlite::Result<i64> {
    if let Some(old) = &input.old_path {
        conn.execute("UPDATE changes SET state = 'superseded' WHERE path = ?1 AND state = 'active'", [old])?;
    }
    let existing: Option<(i64, i64, String)> = conn
        .query_row(
            "SELECT c.change_id,
                    COALESCE((SELECT MAX(seq) FROM change_revisions WHERE change_id = c.change_id), 0),
                    COALESCE((SELECT change_type FROM change_revisions WHERE change_id = c.change_id ORDER BY seq DESC LIMIT 1), '')
               FROM changes c WHERE c.path = ?1 AND c.state = 'active' ORDER BY c.change_id DESC LIMIT 1",
            [&input.path],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .optional()?;
    let reuse = match existing {
        Some((id, seq, last_type)) if last_type != ChangeType::Deleted.as_str() => Some((id, seq)),
        Some((id, _, _)) => {
            conn.execute("UPDATE changes SET state = 'superseded' WHERE change_id = ?1", [id])?;
            None
        }
        None => None,
    };
    let (change_id, last_seq) = match reuse {
        Some(found) => found,
        None => {
            let content = input.after.as_ref().and_then(|s| s.kind).or(input.before.as_ref().and_then(|s| s.kind));
            // scene / prefab / actor / meta は拡張子が正 (内容の判定より優先)
            let kind = document::path_kind(&input.path).unwrap_or_else(|| content.map_or("text", ContentKind::as_str));
            conn.execute(
                "INSERT INTO changes (path, kind, first_seen, state) VALUES (?1, ?2, ?3, 'active')",
                params![input.path, kind, input.observed_at],
            )?;
            (conn.last_insert_rowid(), 0)
        }
    };
    conn.execute(
        "INSERT INTO change_revisions
           (change_id, seq, change_type, old_path, before_snapshot, after_snapshot, observed_at,
            completeness, operation_id, actor, attribution, caused_by, diff_mode)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, NULL, 'unknown', 'unknown', NULL, ?9)",
        params![
            change_id,
            last_seq + 1,
            input.change_type.as_str(),
            input.old_path,
            before_snapshot,
            after_snapshot,
            input.observed_at,
            input.completeness.as_str(),
            plan.mode.as_str(),
        ],
    )?;
    let revision_id = conn.last_insert_rowid();
    let mut insert = conn.prepare_cached(
        "INSERT INTO change_hunks
           (revision_id, ordinal, old_start, old_count, new_start, new_count,
            before_text, after_text, context_pre, context_post, eol_only)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
    )?;
    for (ordinal, hunk) in plan.hunks.iter().enumerate() {
        insert.execute(params![
            revision_id,
            ordinal as i64,
            hunk.old_start as i64,
            hunk.old_count as i64,
            hunk.new_start as i64,
            hunk.new_count as i64,
            hunk.before_text,
            hunk.after_text,
            hunk.context_pre,
            hunk.context_post,
            i64::from(hunk.eol_only),
        ])?;
    }
    write_document_rows(conn, input, plan, change_id, revision_id)?;
    Ok(revision_id)
}

/// 文書の同一性 (`documents`) と構造の変更 (`scene_changes`)。Revision と同じ SAVEPOINT の中で書く。
fn write_document_rows(
    conn: &Connection,
    input: &RevisionInput,
    plan: &DiffPlan,
    change_id: i64,
    revision_id: i64,
) -> rusqlite::Result<()> {
    if let Some(kind) = DocKind::from_path(&input.path) {
        let document_id = if input.after.is_some() {
            Some(document::assign(conn, &input.path, input.old_path.as_deref(), kind, input.doc_guid.as_deref(), input.observed_at)?)
        } else {
            document::mark_deleted(conn, &input.path)?
        };
        let Some(document_id) = document_id else { return Ok(()) };
        conn.execute("UPDATE changes SET document_id = ?1 WHERE change_id = ?2", params![document_id, change_id])?;
        if let Some(outcome) = &plan.scene {
            let mut insert = conn.prepare_cached(
                "INSERT INTO scene_changes
                   (revision_id, document_id, file_id, change_type, component, field, before_value, after_value)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
            )?;
            for row in outcome.rows() {
                insert.execute(params![
                    revision_id,
                    document_id,
                    row.file_id,
                    row.change_type,
                    row.component,
                    row.field,
                    row.before_value,
                    row.after_value,
                ])?;
            }
        }
    } else if let Some((asset_path, kind)) = document::meta_target(&input.path) {
        // `.meta` の変更は、対象の文書の同一性を見直す引き金。削除なら guid は無い
        let guid = if input.after.is_some() { input.doc_guid.as_deref() } else { None };
        document::rebind_for_meta(conn, asset_path, kind, guid, input.observed_at)?;
    }
    Ok(())
}

/// 再構成した数。
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RebuildReport {
    pub snapshots: usize,
    pub revisions: usize,
    pub hunks: usize,
    /// Blob が無くて `blob_state = missing` にしたスナップショット。
    pub missing_blobs: usize,
}

/// `events` を順に読んで派生の表 (`snapshots` / `changes` / `change_revisions` / `change_hunks`) を作る。
/// 通常の記録の経路と同じ `write_revision` を通る。
pub struct Deriver {
    /// (パス, Blob のハッシュ) → その内容の最新のスナップショット。Revision の Before の解決に使う。
    latest: HashMap<(String, Option<String>), i64>,
    /// 帰属の計算の設定。通常の記録と同じ値を使う (違うと作り直した DB が元と変わる)。
    pub cfg: AttributionConfig,
    pub report: RebuildReport,
}

impl Deriver {
    pub fn empty() -> Deriver {
        Deriver { latest: HashMap::new(), cfg: AttributionConfig::default(), report: RebuildReport::default() }
    }

    /// 今の DB の `snapshots` から始める (起動時の入れ直しが、途中のイベントから続けるとき)。
    pub fn from_db(conn: &Connection) -> rusqlite::Result<Deriver> {
        let mut deriver = Deriver::empty();
        let mut stmt = conn.prepare("SELECT path, blob_hash, MAX(snapshot_id) FROM snapshots GROUP BY path, blob_hash")?;
        let rows = stmt.query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, Option<String>>(1)?, r.get::<_, i64>(2)?)))?;
        for row in rows {
            let (path, hash, id) = row?;
            deriver.latest.insert((path, hash), id);
        }
        Ok(deriver)
    }

    /// 派生の表へ反映するイベントか (`baseline` / `observed` / セッション・操作のイベント)。
    pub fn wants(kind: &str) -> bool {
        matches!(kind, "baseline" | "observed") || session::is_session_event(kind)
    }

    /// 1 つのイベントを派生の表へ反映する。対象外の kind は何もしない。`pos` はそのイベントのジャーナルの位置。
    pub fn apply_event(
        &mut self,
        conn: &Connection,
        blobs: &BlobStore,
        kind: &str,
        at: i64,
        payload: &Value,
        pos: &str,
        limits: &DiffLimits,
    ) -> rusqlite::Result<()> {
        match kind {
            "baseline" => {
                let taken_at = payload["taken_at"].as_i64().unwrap_or(at);
                for file in payload["files"].as_array().into_iter().flatten() {
                    let Some(path) = file["path"].as_str() else { continue };
                    let excluded = file["excluded"].as_bool().unwrap_or(false);
                    let hash = if excluded { None } else { file["hash"].as_str() };
                    self.insert_snapshot(conn, blobs, path, "baseline", taken_at, hash, file["size"].as_u64().unwrap_or(0))?;
                }
            }
            "observed" => {
                let Some(input) = RevisionInput::from_payload(blobs, payload) else { return Ok(()) };
                let after_snapshot = match &input.after {
                    Some(after) => Some(self.insert_snapshot(
                        conn,
                        blobs,
                        &input.path,
                        "disk",
                        input.observed_at,
                        after.hash.as_deref(),
                        after.size,
                    )?),
                    None => None,
                };
                let before_path = input.old_path.as_deref().unwrap_or(&input.path);
                let before_snapshot = input
                    .before
                    .as_ref()
                    .and_then(|b| self.latest.get(&(before_path.to_string(), b.hash.clone())).copied());
                // 計算できなかった (読み取りの失敗) ときは、Hunk の無い Revision として残す。作り直しは止めない
                let plan = plan_diff(blobs, &input, None, limits)
                    .unwrap_or_else(|_| DiffPlan::without_hunks(DiffMode::Unavailable));
                let revision_id = write_revision(conn, &input, before_snapshot, after_snapshot, &plan)?;
                // 作り直しは全イベントが入っているので、いま処理しているイベントまでの知らせだけを数える
                let through: Option<i64> =
                    conn.query_row("SELECT event_id FROM events WHERE journal_pos = ?1", [pos], |r| r.get(0)).optional()?;
                attribution::evaluate_new_revision(conn, &self.cfg, revision_id, at, payload["source"].as_str().unwrap_or(""), through)?;
                self.report.revisions += 1;
                self.report.hunks += plan.hunks.len();
            }
            other => session::apply_event(conn, blobs, &self.cfg, other, at, payload, pos)?,
        }
        Ok(())
    }

    fn insert_snapshot(
        &mut self,
        conn: &Connection,
        blobs: &BlobStore,
        path: &str,
        kind: &str,
        taken_at: i64,
        hash: Option<&str>,
        size: u64,
    ) -> rusqlite::Result<i64> {
        let state = match hash {
            None => "excluded",
            Some(h) if blobs.path_for_hash(h).is_file() => "present",
            Some(_) => {
                self.report.missing_blobs += 1;
                "missing"
            }
        };
        conn.execute(
            "INSERT INTO snapshots (blob_hash, size, path, kind, taken_at, blob_state) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![hash, size as i64, path, kind, taken_at, state],
        )?;
        let id = conn.last_insert_rowid();
        self.latest.insert((path.to_string(), hash.map(str::to_string)), id);
        self.report.snapshots += 1;
        Ok(id)
    }
}

/// 派生の表を捨てて、`events` から作り直す (db-schema.md §6 の 1 の後)。1 トランザクションで、失敗したら元のまま。
/// `events` は壊れた DB を退避して作り直したあと、ジャーナルから入れ直したもの。
pub fn rebuild_derived(store: &mut Store, limits: &DiffLimits) -> rusqlite::Result<RebuildReport> {
    let Some(conn) = store.conn.as_mut() else {
        return Err(rusqlite::Error::InvalidParameterName("DB 接続が無い".into()));
    };
    let tx = conn.transaction()?;
    // 参照する側から先に消す (外部キー)
    tx.execute_batch(
        "DELETE FROM reviews; DELETE FROM attribution_evidence; DELETE FROM scene_changes; DELETE FROM change_hunks; DELETE FROM change_revisions;
         DELETE FROM operations; DELETE FROM sessions; DELETE FROM changes; DELETE FROM documents; DELETE FROM snapshots;",
    )?;
    let mut deriver = Deriver::empty();
    deriver.cfg = store.attribution.clone();
    {
        let mut stmt = tx.prepare(
            "SELECT at, kind, payload, journal_pos FROM events
              WHERE kind IN ('baseline', 'observed', 'session_begin', 'session_resumed', 'session_end', 'session_interrupted',
                             'op_begin', 'op_end', 'op_abandoned', 'reported', 'editor_saved', 'review_set')
              ORDER BY event_id",
        )?;
        let mut rows = stmt.query([])?;
        while let Some(row) = rows.next()? {
            let (at, kind, payload, pos): (i64, String, String, String) = (row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?);
            let Ok(payload) = serde_json::from_str::<Value>(&payload) else { continue };
            deriver.apply_event(&tx, &store.blobs, &kind, at, &payload, &pos, limits)?;
        }
    }
    tx.commit()?;
    Ok(deriver.report)
}
