//! 取得の op (`list` / `describe` / `sessions`) と、`status` の DB から読む部分 (ipc-contract.md §4)。
//!
//! 読み取り専用の接続で引く。Writer のキューを通らないので、ベースラインの最中も答えられる。
//! 結果の JSON は camelCase。Blob と Hunk の本文は返さない (本文は `hunks` / `blob_get`、後続のサブ)。

use rusqlite::types::Value as SqlValue;
use rusqlite::{params, params_from_iter, Connection, OpenFlags, OptionalExtension};
use serde_json::{json, Value};

use crate::attribution;
use crate::protocol::{code, Args, OpError, LIST_DEFAULT_LIMIT, LIST_MAX_LIMIT};
use crate::session::session_candidate_sql;

/// 取得用の読み取り専用接続を開く。WAL なので書き手を止めない。
pub fn open_reader(db_path: &std::path::Path) -> rusqlite::Result<Connection> {
    let conn = Connection::open_with_flags(db_path, OpenFlags::SQLITE_OPEN_READ_ONLY)?;
    conn.busy_timeout(std::time::Duration::from_secs(5))?;
    Ok(conn)
}

fn internal(e: rusqlite::Error) -> OpError {
    OpError::new(code::INTERNAL, format!("DB の読み取りに失敗: {e}"))
}

const CHANGE_TYPES: &[&str] = &["added", "modified", "deleted", "renamed"];
const ATTRIBUTIONS: &[&str] = &["verified", "claimed", "inferred", "unknown"];
const REVIEW_STATES: &[&str] = &["pending", "approved", "rejected", "needs_review", "reopened"];

fn one_of(key: &str, value: &str, allowed: &[&str]) -> Result<(), OpError> {
    if allowed.contains(&value) {
        Ok(())
    } else {
        Err(OpError::bad_args(format!("{key} は {} のどれか", allowed.join(" / "))))
    }
}

/// `limit` を読む。省略は既定、最大を超えたら最大に切る。1 未満は `bad_args`。
fn page_limit(args: &mut Args) -> Result<i64, OpError> {
    match args.int("limit")? {
        None => Ok(LIST_DEFAULT_LIMIT),
        Some(n) if n < 1 => Err(OpError::bad_args("limit は 1 以上")),
        Some(n) => Ok(n.min(LIST_MAX_LIMIT)),
    }
}

/// 変更の一覧。新しい Revision から順に返す。`cursor` は直前のページの最後の `revisionId` (これより古いものを返す)。
pub fn list(conn: &Connection, mut args: Args) -> Result<Value, OpError> {
    let path = args.string("path")?;
    let from = args.int("from")?;
    let to = args.int("to")?;
    let change_type = args.string("changeType")?;
    let session_id = args.string("sessionId")?;
    let review = args.string("review")?;
    let attribution = args.string("attribution")?;
    let cursor = args.string("cursor")?;
    let limit = page_limit(&mut args)?;
    args.finish()?;
    if let Some(t) = &change_type {
        one_of("changeType", t, CHANGE_TYPES)?;
    }
    if let Some(a) = &attribution {
        one_of("attribution", a, ATTRIBUTIONS)?;
    }
    if let Some(r) = &review {
        one_of("review", r, REVIEW_STATES)?;
    }
    let cursor_id = match &cursor {
        None => None,
        Some(text) => Some(text.parse::<i64>().map_err(|_| OpError::bad_args("cursor が読めない"))?),
    };

    let mut conditions: Vec<String> = Vec::new();
    let mut values: Vec<SqlValue> = Vec::new();
    let bind = |values: &mut Vec<SqlValue>, v: SqlValue| {
        values.push(v);
        values.len()
    };
    if let Some(path) = path {
        if path.ends_with('/') {
            let n = bind(&mut values, SqlValue::Integer(path.chars().count() as i64));
            let m = bind(&mut values, SqlValue::Text(path));
            conditions.push(format!("substr(c.path, 1, ?{n}) = ?{m}"));
        } else {
            let n = bind(&mut values, SqlValue::Text(path));
            conditions.push(format!("(c.path = ?{n} OR r.old_path = ?{n})"));
        }
    }
    if let Some(from) = from {
        let n = bind(&mut values, SqlValue::Integer(from));
        conditions.push(format!("r.observed_at >= ?{n}"));
    }
    if let Some(to) = to {
        let n = bind(&mut values, SqlValue::Integer(to));
        conditions.push(format!("r.observed_at <= ?{n}"));
    }
    if let Some(t) = change_type {
        let n = bind(&mut values, SqlValue::Text(t));
        conditions.push(format!("r.change_type = ?{n}"));
    }
    if let Some(a) = attribution {
        let n = bind(&mut values, SqlValue::Text(a));
        conditions.push(format!("r.attribution = ?{n}"));
    }
    if let Some(s) = session_id {
        let n = bind(&mut values, SqlValue::Text(s));
        conditions.push(session_candidate_sql(n));
    }
    if let Some(state) = review {
        let n = bind(&mut values, SqlValue::Text(state));
        conditions.push(format!("{REVIEW_STATE_SQL} = ?{n}"));
    }
    if let Some(id) = cursor_id {
        let n = bind(&mut values, SqlValue::Integer(id));
        conditions.push(format!("r.revision_id < ?{n}"));
    }
    let n = bind(&mut values, SqlValue::Integer(limit + 1));
    let where_clause = if conditions.is_empty() { String::new() } else { format!("WHERE {}", conditions.join(" AND ")) };
    let sql = format!(
        "SELECT r.revision_id, r.change_id, r.seq, c.path, r.old_path, r.change_type, c.kind, c.state, r.observed_at,
                r.completeness, r.actor, r.attribution, r.diff_mode, r.operation_id, sb.size, sa.size,
                (SELECT COUNT(*) FROM change_hunks h WHERE h.revision_id = r.revision_id),
                {REVIEW_STATE_SQL}
           FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
           LEFT JOIN snapshots sb ON sb.snapshot_id = r.before_snapshot
           LEFT JOIN snapshots sa ON sa.snapshot_id = r.after_snapshot
           {where_clause}
          ORDER BY r.revision_id DESC LIMIT ?{n}"
    );
    let mut stmt = conn.prepare(&sql).map_err(internal)?;
    let rows = stmt
        .query_map(params_from_iter(values.iter()), |r| {
            Ok(json!({
                "revisionId": r.get::<_, i64>(0)?,
                "changeId": r.get::<_, i64>(1)?,
                "seq": r.get::<_, i64>(2)?,
                "path": r.get::<_, String>(3)?,
                "oldPath": r.get::<_, Option<String>>(4)?,
                "changeType": r.get::<_, String>(5)?,
                "contentKind": r.get::<_, String>(6)?,
                "changeState": r.get::<_, String>(7)?,
                "observedAt": r.get::<_, i64>(8)?,
                "completeness": r.get::<_, String>(9)?,
                "actor": r.get::<_, String>(10)?,
                "attribution": r.get::<_, String>(11)?,
                "diffMode": r.get::<_, String>(12)?,
                "operationId": r.get::<_, Option<String>>(13)?,
                "beforeSize": r.get::<_, Option<i64>>(14)?,
                "afterSize": r.get::<_, Option<i64>>(15)?,
                "hunkCount": r.get::<_, i64>(16)?,
                "review": r.get::<_, String>(17)?,
            }))
        })
        .map_err(internal)?;
    let mut items: Vec<Value> = rows.collect::<rusqlite::Result<_>>().map_err(internal)?;
    let next_cursor = if items.len() as i64 > limit {
        items.truncate(limit as usize);
        items.last().and_then(|v| v["revisionId"].as_i64()).map(|id| id.to_string())
    } else {
        None
    };
    Ok(json!({ "items": items, "nextCursor": next_cursor, "limit": limit }))
}

/// Revision 単位のレビュー状態 (`reviews` の最新の行。無ければ `pending`)。
const REVIEW_STATE_SQL: &str = "COALESCE((SELECT v.state FROM reviews v WHERE v.revision_id = r.revision_id \
     AND v.scope_kind = 'revision' ORDER BY v.review_id DESC LIMIT 1), 'pending')";

/// 1 つの Revision の詳細。Hunk は行番号と行数まで (本文は返さない)。
pub fn describe(conn: &Connection, mut args: Args) -> Result<Value, OpError> {
    let revision_id = args.required_int("revisionId")?;
    args.finish()?;
    let sql = format!(
        "SELECT r.revision_id, r.change_id, r.seq, c.path, r.old_path, r.change_type, c.kind, c.state, r.observed_at,
                r.completeness, r.actor, r.attribution, r.diff_mode, r.operation_id,
                sb.blob_hash, sb.size, sb.blob_state, sa.blob_hash, sa.size, sa.blob_state,
                {REVIEW_STATE_SQL}
           FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
           LEFT JOIN snapshots sb ON sb.snapshot_id = r.before_snapshot
           LEFT JOIN snapshots sa ON sa.snapshot_id = r.after_snapshot
          WHERE r.revision_id = ?1"
    );
    let side = |hash: Option<String>, size: Option<i64>, state: Option<String>| -> Value {
        match state {
            None => Value::Null,
            Some(state) => json!({ "hash": hash, "size": size, "blobState": state }),
        }
    };
    let found: Option<Value> = conn
        .query_row(&sql, [revision_id], |r| {
            let change_type: String = r.get(5)?;
            let before_state: Option<String> = r.get(16)?;
            let before = side(r.get(14)?, r.get(15)?, before_state.clone());
            let after = side(r.get(17)?, r.get(18)?, r.get(19)?);
            // 復元 = Before へ戻すこと。新規 (Before 無し) は削除で戻せる。Before を見ていない・Blob が無いものは戻せない
            let restorable = match &before_state {
                Some(state) => state == "present",
                None => change_type == "added",
            };
            Ok(json!({
                "revisionId": r.get::<_, i64>(0)?,
                "changeId": r.get::<_, i64>(1)?,
                "seq": r.get::<_, i64>(2)?,
                "path": r.get::<_, String>(3)?,
                "oldPath": r.get::<_, Option<String>>(4)?,
                "changeType": change_type,
                "contentKind": r.get::<_, String>(6)?,
                "changeState": r.get::<_, String>(7)?,
                "observedAt": r.get::<_, i64>(8)?,
                "completeness": r.get::<_, String>(9)?,
                "actor": r.get::<_, String>(10)?,
                "attribution": r.get::<_, String>(11)?,
                "diffMode": r.get::<_, String>(12)?,
                "operationId": r.get::<_, Option<String>>(13)?,
                "before": before,
                "after": after,
                "restorable": restorable,
                "review": r.get::<_, String>(20)?,
            }))
        })
        .optional()
        .map_err(internal)?;
    let Some(mut detail) = found else {
        return Err(OpError::new(code::NOT_FOUND, format!("revisionId {revision_id} は無い")));
    };
    let mut stmt = conn
        .prepare(
            "SELECT ordinal, old_start, old_count, new_start, new_count, eol_only, state
               FROM change_hunks WHERE revision_id = ?1 ORDER BY ordinal",
        )
        .map_err(internal)?;
    let hunks = stmt
        .query_map([revision_id], |r| {
            Ok(json!({
                "ordinal": r.get::<_, i64>(0)?,
                "oldStart": r.get::<_, i64>(1)?,
                "oldCount": r.get::<_, i64>(2)?,
                "newStart": r.get::<_, i64>(3)?,
                "newCount": r.get::<_, i64>(4)?,
                "eolOnly": r.get::<_, i64>(5)? != 0,
                "state": r.get::<_, String>(6)?,
            }))
        })
        .map_err(internal)?;
    detail["hunks"] = Value::Array(hunks.collect::<rusqlite::Result<_>>().map_err(internal)?);

    // 区分だけでなく根拠の一覧と候補のセッションを出す (attribution-rules.md §8)
    let mut stmt = conn
        .prepare(
            "SELECT kind, session_id, operation_id, detail, recorded_at FROM attribution_evidence
              WHERE revision_id = ?1 ORDER BY evidence_id",
        )
        .map_err(internal)?;
    let evidence = stmt
        .query_map([revision_id], |r| {
            let detail: String = r.get(3)?;
            Ok(json!({
                "kind": r.get::<_, String>(0)?,
                "sessionId": r.get::<_, Option<String>>(1)?,
                "operationId": r.get::<_, Option<String>>(2)?,
                "detail": serde_json::from_str::<Value>(&detail).unwrap_or(Value::Null),
                "recordedAt": r.get::<_, i64>(4)?,
            }))
        })
        .map_err(internal)?;
    detail["evidence"] = Value::Array(evidence.collect::<rusqlite::Result<_>>().map_err(internal)?);
    let conclusion = attribution::current_conclusion(conn, revision_id).map_err(internal)?;
    detail["candidateSessions"] = json!(conclusion.candidates);
    Ok(detail)
}

/// セッションの一覧 (新しい順)。`cursor` は直前のページの最後の `<startedAt>:<sessionId>`。
pub fn sessions(conn: &Connection, mut args: Args) -> Result<Value, OpError> {
    let cursor = args.string("cursor")?;
    let limit = page_limit(&mut args)?;
    args.finish()?;
    let (cursor_at, cursor_id) = match &cursor {
        None => (i64::MAX, String::from("\u{10FFFF}")),
        Some(text) => match text.split_once(':') {
            Some((at, id)) => (at.parse::<i64>().map_err(|_| OpError::bad_args("cursor が読めない"))?, id.to_string()),
            None => return Err(OpError::bad_args("cursor が読めない")),
        },
    };
    let mut stmt = conn
        .prepare(
            "SELECT session_id, provider, source, client_session_id, workspace, base_commit, started_at, ended_at, status
               FROM sessions
              WHERE started_at < ?1 OR (started_at = ?1 AND session_id < ?2)
              ORDER BY started_at DESC, session_id DESC LIMIT ?3",
        )
        .map_err(internal)?;
    let rows = stmt
        .query_map(params![cursor_at, cursor_id, limit + 1], |r| {
            Ok(json!({
                "sessionId": r.get::<_, String>(0)?,
                "provider": r.get::<_, String>(1)?,
                "source": r.get::<_, String>(2)?,
                "clientSessionId": r.get::<_, Option<String>>(3)?,
                "workspace": r.get::<_, String>(4)?,
                "baseCommit": r.get::<_, Option<String>>(5)?,
                "startedAt": r.get::<_, i64>(6)?,
                "endedAt": r.get::<_, Option<i64>>(7)?,
                "status": r.get::<_, String>(8)?,
            }))
        })
        .map_err(internal)?;
    let mut items: Vec<Value> = rows.collect::<rusqlite::Result<_>>().map_err(internal)?;
    let next_cursor = if items.len() as i64 > limit {
        items.truncate(limit as usize);
        items.last().map(|v| format!("{}:{}", v["startedAt"], v["sessionId"].as_str().unwrap_or("")))
    } else {
        None
    };
    Ok(json!({ "items": items, "nextCursor": next_cursor, "limit": limit }))
}

/// `status` が DB から引く部分: Blob の総量と、欠落区間。
pub fn status_from_db(conn: &Connection) -> Result<Value, OpError> {
    // 同じ内容は 1 つの Blob なので、ハッシュで重複を除いて数える
    let (blob_count, blob_bytes): (i64, i64) = conn
        .query_row(
            "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM
               (SELECT DISTINCT blob_hash, size FROM snapshots WHERE blob_state = 'present' AND blob_hash IS NOT NULL)",
            [],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )
        .map_err(internal)?;
    let gap_count: i64 = conn.query_row("SELECT COUNT(*) FROM coverage_gaps", [], |r| r.get(0)).map_err(internal)?;
    let mut stmt = conn
        .prepare("SELECT gap_id, from_at, to_at, reason, scope FROM coverage_gaps ORDER BY gap_id DESC LIMIT 20")
        .map_err(internal)?;
    let recent = stmt
        .query_map([], |r| {
            Ok(json!({
                "gapId": r.get::<_, i64>(0)?,
                "fromAt": r.get::<_, i64>(1)?,
                "toAt": r.get::<_, i64>(2)?,
                "reason": r.get::<_, String>(3)?,
                "scope": r.get::<_, Option<String>>(4)?,
            }))
        })
        .map_err(internal)?;
    let recent: Vec<Value> = recent.collect::<rusqlite::Result<_>>().map_err(internal)?;
    Ok(json!({
        "blobs": { "count": blob_count, "bytes": blob_bytes },
        "gaps": { "count": gap_count, "recent": recent },
    }))
}
