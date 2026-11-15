//! 操作 (`operations`) の派生の表への反映と、`report` の照合結果の取得。
//!
//! `op_begin` / `op_end` / `op_abandoned` / `reported` のイベントから `operations` を作る。
//! 通常の記録も `rebuild_derived` も `apply` を通る (spec D26)。

use rusqlite::{params, Connection, OptionalExtension};
use serde_json::{json, Value};

use crate::attribution::{self, AttributionConfig, REPORT_BACKLOG_MS, REPORT_LOOKAHEAD_MS};
use crate::blob::BlobStore;
use crate::session::touch;

/// 操作の種類 (`operations.operation_type`)。
pub const OPERATION_TYPES: &[&str] = &["edit", "write", "delete", "rename", "report", "other"];

/// 操作系のイベントを反映する。対象外の kind は何もしない (戻り値は反映したか)。
pub fn apply(
    conn: &Connection,
    blobs: &BlobStore,
    cfg: &AttributionConfig,
    kind: &str,
    at: i64,
    payload: &Value,
) -> rusqlite::Result<bool> {
    match kind {
        "op_begin" => op_begin(conn, blobs, cfg, at, payload)?,
        "op_end" => op_finish(conn, cfg, at, payload, payload["result"].as_str().unwrap_or("ok"))?,
        "op_abandoned" => op_finish(conn, cfg, at, payload, "abandoned")?,
        "reported" => reported(conn, cfg, at, payload)?,
        _ => return Ok(false),
    }
    Ok(true)
}

fn op_begin(conn: &Connection, blobs: &BlobStore, cfg: &AttributionConfig, at: i64, p: &Value) -> rusqlite::Result<()> {
    let (Some(op_id), Some(session_id)) = (p["operation_id"].as_str(), p["session_id"].as_str()) else { return Ok(()) };
    let targets: Vec<Value> = p["targets"].as_array().cloned().unwrap_or_default();
    let declared_path = p["declared_path"].as_str();
    let mut declared_snapshot: Option<i64> = None;
    if let (Some(hash), Some(path)) = (p["declared_edit"]["blob_hash"].as_str(), declared_path) {
        let state = if blobs.path_for_hash(hash).is_file() { "present" } else { "missing" };
        conn.execute(
            "INSERT INTO snapshots (blob_hash, size, path, kind, taken_at, blob_state) VALUES (?1, ?2, ?3, 'declared_edit', ?4, ?5)",
            params![hash, p["declared_edit"]["size"].as_i64().unwrap_or(0), path, at, state],
        )?;
        declared_snapshot = Some(conn.last_insert_rowid());
    }
    let reported = json!({
        "targets": targets,
        "clientOpId": p["client_op_id"],
        "declaredAfterHash": p["declared_after_hash"],
        "declaredPath": declared_path,
        "beginFlushed": p["begin_flushed"],
    });
    conn.execute(
        "INSERT OR IGNORE INTO operations
           (operation_id, session_id, operation_type, started_at, finished_at, reported_targets, observed_targets, declared_edit, result)
         VALUES (?1, ?2, ?3, ?4, NULL, ?5, '[]', ?6, NULL)",
        params![op_id, session_id, p["operation_type"].as_str().unwrap_or("other"), at, reported.to_string(), declared_snapshot],
    )?;
    touch(conn, session_id, at)?;
    // 近い時刻の変更は、新しい活動の分だけ `time_near` の根拠が増えうる
    attribution::reevaluate_range(conn, cfg, at, at - cfg.time_near_ms, at + cfg.time_near_ms, None)
}

fn op_finish(conn: &Connection, cfg: &AttributionConfig, at: i64, p: &Value, result: &str) -> rusqlite::Result<()> {
    let Some(op_id) = p["operation_id"].as_str() else { return Ok(()) };
    let row: Option<(String, i64, String)> = conn
        .query_row(
            "SELECT session_id, started_at, reported_targets FROM operations
              WHERE operation_id = ?1 AND finished_at IS NULL AND result IS NULL",
            [op_id],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .optional()?;
    let Some((session_id, started_at, raw)) = row else { return Ok(()) };
    conn.execute("UPDATE operations SET finished_at = ?1, result = ?2 WHERE operation_id = ?3", params![at, result, op_id])?;
    touch(conn, &session_id, at)?;
    let json: Value = serde_json::from_str(&raw).unwrap_or(Value::Null);
    let paths: Vec<String> =
        json["targets"].as_array().into_iter().flatten().filter_map(|t| t["path"].as_str().map(str::to_string)).collect();
    // ok で閉じると、開いている間は数えなかった窓の根拠が結論に入る。変わった Revision を通知できるよう前後を比べる
    let before = if result == "ok" { window_conclusions(conn, op_id)? } else { Vec::new() };
    attribution::reevaluate_range(conn, cfg, at, started_at, at + cfg.window_grace_ms, Some(&paths))?;
    for (revision_id, old) in before {
        let now = window_conclusions_of(conn, revision_id)?;
        if now != old {
            // 根拠の行は増えないので、通知 (attribution_updated は根拠の行の追加から作る) のための印を足す
            conn.execute(
                "INSERT INTO attribution_evidence (revision_id, session_id, operation_id, kind, detail, recorded_at)
                 VALUES (?1, NULL, ?2, 'op_closed', ?3, ?4)",
                params![revision_id, op_id, json!({ "result": result }).to_string(), at],
            )?;
        }
    }
    refresh_observed_targets(conn, op_id)
}

type Concluded = (String, String, Option<String>);

fn window_conclusions_of(conn: &Connection, revision_id: i64) -> rusqlite::Result<Concluded> {
    conn.query_row("SELECT actor, attribution, operation_id FROM change_revisions WHERE revision_id = ?1", [revision_id], |r| {
        Ok((r.get(0)?, r.get(1)?, r.get(2)?))
    })
}

/// この操作の窓の根拠を持つ Revision と、いまの結論。
fn window_conclusions(conn: &Connection, op_id: &str) -> rusqlite::Result<Vec<(i64, Concluded)>> {
    let ids: Vec<i64> = {
        let mut stmt = conn.prepare_cached(&format!(
            "SELECT DISTINCT revision_id FROM attribution_evidence WHERE operation_id = ?1 AND kind IN ({}) ORDER BY revision_id",
            attribution::WINDOW_KINDS_SQL
        ))?;
        let rows = stmt.query_map([op_id], |r| r.get(0))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    ids.into_iter().map(|id| Ok((id, window_conclusions_of(conn, id)?))).collect()
}

/// そのセッションで開いている操作 (`op_end` 待ち) を `abandoned` で閉じる。閉じた数を返す。
/// セッションの終わりとターンの終わりが使う。拒否された操作は終わりの知らせが来ないので、ここで窓を閉じる。
pub fn close_open_ops(conn: &Connection, cfg: &AttributionConfig, at: i64, session_id: &str) -> rusqlite::Result<usize> {
    let open = open_op_ids(conn, session_id)?;
    for id in &open {
        op_finish(conn, cfg, at, &json!({ "operation_id": id }), "abandoned")?;
    }
    Ok(open.len())
}

/// そのセッションで開いている操作の ID (開始の順)。
pub fn open_op_ids(conn: &Connection, session_id: &str) -> rusqlite::Result<Vec<String>> {
    let mut stmt = conn.prepare_cached(
        "SELECT operation_id FROM operations WHERE session_id = ?1 AND finished_at IS NULL AND result IS NULL
          ORDER BY started_at, operation_id",
    )?;
    let rows = stmt.query_map([session_id], |r| r.get(0))?;
    rows.collect()
}

/// 操作に結び付いた Revision のパスと After のハッシュを `observed_targets` に写す (派生。イベントからいつでも作り直せる)。
fn refresh_observed_targets(conn: &Connection, op_id: &str) -> rusqlite::Result<()> {
    let mut stmt = conn.prepare_cached(
        "SELECT c.path, sa.blob_hash FROM change_revisions r
           JOIN changes c ON c.change_id = r.change_id
           LEFT JOIN snapshots sa ON sa.snapshot_id = r.after_snapshot
          WHERE r.operation_id = ?1 ORDER BY r.revision_id",
    )?;
    let rows = stmt.query_map([op_id], |r| Ok(json!({ "path": r.get::<_, String>(0)?, "afterHash": r.get::<_, Option<String>>(1)? })))?;
    let observed: Vec<Value> = rows.collect::<rusqlite::Result<_>>()?;
    conn.execute("UPDATE operations SET observed_targets = ?1 WHERE operation_id = ?2", params![Value::Array(observed).to_string(), op_id])?;
    Ok(())
}

fn reported(conn: &Connection, cfg: &AttributionConfig, at: i64, p: &Value) -> rusqlite::Result<()> {
    let (Some(op_id), Some(session_id)) = (p["operation_id"].as_str(), p["session_id"].as_str()) else { return Ok(()) };
    let entries: Vec<Value> = p["paths"]
        .as_array()
        .into_iter()
        .flatten()
        .map(|e| json!({ "path": e["path"], "beforeHash": e["before_hash"], "afterHash": e["after_hash"] }))
        .collect();
    let paths: Vec<String> = entries.iter().filter_map(|e| e["path"].as_str().map(str::to_string)).collect();
    let reported = json!({ "report": { "paths": entries }, "linkedOperationId": p["linked_operation_id"] });
    conn.execute(
        "INSERT OR IGNORE INTO operations
           (operation_id, session_id, operation_type, started_at, finished_at, reported_targets, observed_targets, result)
         VALUES (?1, ?2, 'report', ?3, ?3, ?4, '[]', 'ok')",
        params![op_id, session_id, at, reported.to_string()],
    )?;
    touch(conn, session_id, at)?;
    attribution::reevaluate_range(conn, cfg, at, at - REPORT_BACKLOG_MS, at + REPORT_LOOKAHEAD_MS, Some(&paths))
}

/// `report` のパスごとの照合結果。`matched` = ハッシュが Before / After の両方と一致した変更がある、
/// `mismatch` = 変更はあるが一致しない (ハッシュを渡していない場合も含む)、`not_observed` = セッション開始後の変更が無い。
pub fn report_results(conn: &Connection, report_op_id: &str, session_id: &str, paths: &[String]) -> rusqlite::Result<Vec<Value>> {
    let started: i64 = conn.query_row("SELECT started_at FROM sessions WHERE session_id = ?1", [session_id], |r| r.get(0))?;
    let mut out = Vec::new();
    for path in paths {
        let matched: Option<i64> = conn
            .query_row(
                "SELECT r.revision_id FROM attribution_evidence e
                   JOIN change_revisions r ON r.revision_id = e.revision_id
                   JOIN changes c ON c.change_id = r.change_id
                  WHERE e.operation_id = ?1 AND e.kind = 'report_hash_match' AND c.path = ?2 COLLATE NOCASE
                  ORDER BY r.revision_id DESC LIMIT 1",
                params![report_op_id, path],
                |r| r.get(0),
            )
            .optional()?;
        let latest: Option<i64> = conn
            .query_row(
                "SELECT r.revision_id FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
                  WHERE c.path = ?1 COLLATE NOCASE AND r.observed_at >= ?2 AND r.completeness <> 'gap'
                  ORDER BY r.revision_id DESC LIMIT 1",
                params![path, started],
                |r| r.get(0),
            )
            .optional()?;
        let (result, revision) = match (matched, latest) {
            (Some(id), _) => ("matched", Some(id)),
            (None, Some(id)) => ("mismatch", Some(id)),
            (None, None) => ("not_observed", None),
        };
        let (actor, attribution): (Option<String>, Option<String>) = match revision {
            Some(id) => conn.query_row(
                "SELECT actor, attribution FROM change_revisions WHERE revision_id = ?1",
                [id],
                |r| Ok((Some(r.get(0)?), Some(r.get(1)?))),
            )?,
            None => (None, None),
        };
        out.push(json!({ "path": path, "result": result, "revisionId": revision, "actor": actor, "attribution": attribution }));
    }
    Ok(out)
}

/// `op_end` の結果に入れる、操作に結び付いた Revision (ID と帰属)。
pub fn bound_revisions(conn: &Connection, op_id: &str) -> rusqlite::Result<Vec<Value>> {
    let mut stmt = conn.prepare_cached(
        "SELECT r.revision_id, c.path, r.completeness, r.actor, r.attribution FROM change_revisions r
           JOIN changes c ON c.change_id = r.change_id WHERE r.operation_id = ?1 ORDER BY r.revision_id",
    )?;
    let rows = stmt.query_map([op_id], |r| {
        Ok(json!({
            "revisionId": r.get::<_, i64>(0)?,
            "path": r.get::<_, String>(1)?,
            "completeness": r.get::<_, String>(2)?,
            "actor": r.get::<_, String>(3)?,
            "attribution": r.get::<_, String>(4)?,
        }))
    })?;
    rows.collect()
}

/// 操作の窓の中に入った Revision のうち、結論が別の操作・複数候補・未確定のもの (`bound_revisions` に入らないもの)。
/// `op_end` の結果で、AI が「結び付かなかった変更」を知れるようにする。
pub fn unbound_revisions_in_window(conn: &Connection, op_id: &str) -> rusqlite::Result<Vec<Value>> {
    let mut stmt = conn.prepare_cached(
        "SELECT DISTINCT r.revision_id, c.path, r.completeness, r.actor, r.attribution FROM attribution_evidence e
           JOIN change_revisions r ON r.revision_id = e.revision_id
           JOIN changes c ON c.change_id = r.change_id
          WHERE e.operation_id = ?1 AND e.kind = 'op_window' AND (r.operation_id IS NULL OR r.operation_id <> ?1)
          ORDER BY r.revision_id",
    )?;
    let rows = stmt.query_map([op_id], |r| {
        Ok(json!({
            "revisionId": r.get::<_, i64>(0)?,
            "path": r.get::<_, String>(1)?,
            "completeness": r.get::<_, String>(2)?,
            "actor": r.get::<_, String>(3)?,
            "attribution": r.get::<_, String>(4)?,
        }))
    })?;
    rows.collect()
}
