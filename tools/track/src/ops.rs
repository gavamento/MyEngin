//! 取得の op (`list` / `describe` / `sessions` / `hunks`) と、`status` の DB から読む部分 (ipc-contract.md §4)。
//! `blob_get` は Blob のファイルを読む (DB は引かない)。
//!
//! 読み取り専用の接続で引く。Writer のキューを通らないので、ベースラインの最中も答えられる。
//! 結果の JSON は camelCase。`list` / `describe` は Blob と Hunk の本文を返さない (本文は `hunks` / `blob_get`)。

use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use rusqlite::types::Value as SqlValue;
use rusqlite::{params, params_from_iter, Connection, OpenFlags, OptionalExtension};
use serde_json::{json, Value};

use crate::attribution;
use crate::document;
use crate::scene_diff::change_type;
use crate::review;
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
    // 1 つの操作 (operation) の Revision だけ。Editor が「この操作の変更をまとめてレビュー」の対象を集めるのに使う
    let operation_id = args.string("operationId")?;
    let review = args.string("review")?;
    // 帰属は 1 つの文字列か、文字列の配列 (どれか 1 つに合えばよい。「不明 / 推定」を 1 回で取る)
    let attributions: Vec<String> = match args.value("attribution") {
        None => Vec::new(),
        Some(Value::String(one)) => vec![one],
        Some(Value::Array(items)) => items
            .into_iter()
            .map(|v| match v {
                Value::String(s) => Ok(s),
                _ => Err(OpError::bad_args("attribution の要素は文字列でなければならない")),
            })
            .collect::<Result<_, _>>()?,
        Some(_) => return Err(OpError::bad_args("attribution は文字列か文字列の配列でなければならない")),
    };
    let cursor = args.string("cursor")?;
    let limit = page_limit(&mut args)?;
    args.finish()?;
    if let Some(t) = &change_type {
        one_of("changeType", t, CHANGE_TYPES)?;
    }
    for a in &attributions {
        one_of("attribution", a, ATTRIBUTIONS)?;
    }
    if let Some(r) = &review {
        one_of("review", r, review::AGGREGATE_STATES)?;
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
    if !attributions.is_empty() {
        let marks: Vec<String> =
            attributions.into_iter().map(|a| format!("?{}", bind(&mut values, SqlValue::Text(a)))).collect();
        conditions.push(format!("r.attribution IN ({})", marks.join(", ")));
    }
    if let Some(s) = session_id {
        let n = bind(&mut values, SqlValue::Text(s));
        conditions.push(session_candidate_sql(n));
    }
    if let Some(operation) = operation_id {
        let n = bind(&mut values, SqlValue::Text(operation));
        conditions.push(format!("r.operation_id = ?{n}"));
    }
    // レビューの状態は葉からの集約なので SQL では絞れない。絞るときは対象の行為者に限り、行を読みながら集約して選ぶ
    if review.is_some() {
        conditions.push("r.actor IN ('ai', 'unknown')".to_string());
    }
    let where_clause = if conditions.is_empty() { String::new() } else { format!("WHERE {}", conditions.join(" AND ")) };
    let run_query = |after: Option<i64>, take: i64| -> Result<Vec<Value>, OpError> {
        let mut values = values.clone();
        let mut clause = where_clause.clone();
        if let Some(id) = after {
            values.push(SqlValue::Integer(id));
            let joiner = if clause.is_empty() { "WHERE" } else { " AND" };
            clause = format!("{clause}{joiner} r.revision_id < ?{}", values.len());
        }
        values.push(SqlValue::Integer(take));
        let n = values.len();
        let sql = format!(
            "SELECT r.revision_id, r.change_id, r.seq, c.path, r.old_path, r.change_type, c.kind, c.state, r.observed_at,
                    r.completeness, r.actor, r.attribution, r.diff_mode, r.operation_id, sb.size, sa.size,
                    (SELECT COUNT(*) FROM change_hunks h WHERE h.revision_id = r.revision_id)
               FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
               LEFT JOIN snapshots sb ON sb.snapshot_id = r.before_snapshot
               LEFT JOIN snapshots sa ON sa.snapshot_id = r.after_snapshot
               {clause}
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
                }))
            })
            .map_err(internal)?;
        let mut items: Vec<Value> = rows.collect::<rusqlite::Result<_>>().map_err(internal)?;
        // レビューの集約 (対象外の行為者は null)
        for item in &mut items {
            let revision_id = item["revisionId"].as_i64().unwrap_or(0);
            let reviewable = item["actor"].as_str().is_some_and(review::is_reviewable_actor);
            item["review"] = if reviewable {
                json!(review::revision_counts(conn, revision_id).map_err(internal)?.aggregate().as_str())
            } else {
                Value::Null
            };
        }
        Ok(items)
    };

    let mut items: Vec<Value>;
    let next_cursor: Option<String>;
    match &review {
        None => {
            items = run_query(cursor_id, limit + 1)?;
            next_cursor = if items.len() as i64 > limit {
                items.truncate(limit as usize);
                items.last().and_then(|v| v["revisionId"].as_i64()).map(|id| id.to_string())
            } else {
                None
            };
        }
        Some(wanted) => {
            // 1 回の問い合わせで読む行数に上限を置く。上限に達したら、読んだ所までを返して続きの cursor を渡す
            items = Vec::new();
            let mut scanned = 0i64;
            let mut after = cursor_id;
            let mut more = false;
            let mut last_scanned: Option<i64> = None;
            'scan: loop {
                let batch = run_query(after, REVIEW_SCAN_BATCH)?;
                let full = batch.len() as i64 == REVIEW_SCAN_BATCH;
                for item in batch {
                    let id = item["revisionId"].as_i64().unwrap_or(0);
                    last_scanned = Some(id);
                    scanned += 1;
                    after = Some(id);
                    if item["review"].as_str() == Some(wanted.as_str()) {
                        items.push(item);
                        if items.len() as i64 > limit {
                            more = true;
                            break 'scan;
                        }
                    }
                    if scanned >= REVIEW_SCAN_LIMIT {
                        more = full;
                        break 'scan;
                    }
                }
                if !full {
                    break;
                }
            }
            if items.len() as i64 > limit {
                items.truncate(limit as usize);
                next_cursor = items.last().and_then(|v| v["revisionId"].as_i64()).map(|id| id.to_string());
            } else {
                next_cursor = if more { last_scanned.map(|id| id.to_string()) } else { None };
            }
        }
    }
    Ok(json!({ "items": items, "nextCursor": next_cursor, "limit": limit }))
}

/// `review` で絞るときに 1 回の問い合わせで読む Revision の数の上限と、1 回の読み出しの大きさ。
const REVIEW_SCAN_LIMIT: i64 = 2000;
const REVIEW_SCAN_BATCH: i64 = 200;

/// 1 つの Revision の詳細。Hunk は行番号と行数まで (本文は返さない)。
pub fn describe(conn: &Connection, mut args: Args) -> Result<Value, OpError> {
    let revision_id = args.required_int("revisionId")?;
    args.finish()?;
    let sql = format!(
        "SELECT r.revision_id, r.change_id, r.seq, c.path, r.old_path, r.change_type, c.kind, c.state, r.observed_at,
                r.completeness, r.actor, r.attribution, r.diff_mode, r.operation_id,
                sb.blob_hash, sb.size, sb.blob_state, sa.blob_hash, sa.size, sa.blob_state
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
            }))
        })
        .optional()
        .map_err(internal)?;
    let Some(mut detail) = found else {
        return Err(OpError::new(code::NOT_FOUND, format!("revisionId {revision_id} は無い")));
    };
    let mut stmt = conn
        .prepare(
            "SELECT ordinal, old_start, old_count, new_start, new_count, eol_only, state, hunk_id
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
                "hunkId": r.get::<_, i64>(7)?,
            }))
        })
        .map_err(internal)?;
    let change_id = detail["changeId"].as_i64().unwrap_or(0);
    let operation_id = detail["operationId"].as_str().map(str::to_string);
    if let Value::Object(fields) = review::describe_fields(conn, revision_id, change_id, operation_id.as_deref()).map_err(internal)? {
        for (key, value) in fields {
            detail[key.as_str()] = value;
        }
    }
    let mut hunk_items: Vec<Value> = hunks.collect::<rusqlite::Result<_>>().map_err(internal)?;
    let index = review::LatestIndex::new(&review::load_rows(conn, revision_id).map_err(internal)?);
    for item in &mut hunk_items {
        let id = item["hunkId"].as_i64().unwrap_or(0);
        item["review"] = json!(index.state_of(&review::Leaf::Hunk(id)).as_str());
    }
    detail["hunks"] = Value::Array(hunk_items);
    if let Some(summary) = scene_summary(conn, revision_id).map_err(internal)? {
        detail["sceneSummary"] = summary;
    }

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
            let session_id: String = r.get(0)?;
            let (counts, truncated) =
                review::group_counts(conn, &session_candidate_sql(1), &[&session_id as &dyn rusqlite::ToSql])?;
            Ok(json!({
                "changeCount": crate::session::change_count(conn, &session_id)?,
                "review": review::group_json(&counts, truncated),
                "sessionId": session_id,
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

/// `hunks` の 1 ページに入れる Hunk の JSON の合計の上限 (バイト)。1 行の上限 (1 MiB) に、他の項目と余裕を残す値。
pub const HUNKS_PAGE_BUDGET: usize = 768 * 1024;

/// `blob_get` の 1 回の最大。
pub const BLOB_GET_MAX_LENGTH: i64 = 512 * 1024;

/// 1 つの Revision の Hunk を、本文つきでページ送りして返す。`cursor` は直前のページの最後の `ordinal`。
///
/// 1 ページは Hunk の JSON の合計が `HUNKS_PAGE_BUDGET` 以内。1 つだけで収まらない Hunk は本文を入れず
/// `oversize: true` と `beforeBytes` / `afterBytes` だけを返す (本文は `blob_get` で原本から取る)。
pub fn hunks(conn: &Connection, mut args: Args) -> Result<Value, OpError> {
    let revision_id = args.required_int("revisionId")?;
    let cursor = args.string("cursor")?;
    args.finish()?;
    let after_ordinal = match &cursor {
        None => -1,
        Some(text) => text.parse::<i64>().map_err(|_| OpError::bad_args("cursor が読めない"))?,
    };
    let exists: bool = conn
        .query_row("SELECT EXISTS (SELECT 1 FROM change_revisions WHERE revision_id = ?1)", [revision_id], |r| r.get(0))
        .map_err(internal)?;
    if !exists {
        return Err(OpError::new(code::NOT_FOUND, format!("revisionId {revision_id} は無い")));
    }
    let mut stmt = conn
        .prepare(
            "SELECT hunk_id, ordinal, old_start, old_count, new_start, new_count, before_text, after_text,
                    context_pre, context_post, eol_only, state
               FROM change_hunks WHERE revision_id = ?1 AND ordinal > ?2 ORDER BY ordinal",
        )
        .map_err(internal)?;
    let mut rows = stmt.query(params![revision_id, after_ordinal]).map_err(internal)?;
    let index = review::LatestIndex::new(&review::load_rows(conn, revision_id).map_err(internal)?);
    let mut items: Vec<Value> = Vec::new();
    let mut used = 0usize;
    let mut next_cursor: Option<String> = None;
    while let Some(r) = rows.next().map_err(internal)? {
        let ordinal: i64 = r.get(1).map_err(internal)?;
        let before: String = r.get(6).map_err(internal)?;
        let after: String = r.get(7).map_err(internal)?;
        let pre: String = r.get(8).map_err(internal)?;
        let post: String = r.get(9).map_err(internal)?;
        let mut item = json!({
            "hunkId": r.get::<_, i64>(0).map_err(internal)?,
            "ordinal": ordinal,
            "oldStart": r.get::<_, i64>(2).map_err(internal)?,
            "oldCount": r.get::<_, i64>(3).map_err(internal)?,
            "newStart": r.get::<_, i64>(4).map_err(internal)?,
            "newCount": r.get::<_, i64>(5).map_err(internal)?,
            "eolOnly": r.get::<_, i64>(10).map_err(internal)? != 0,
            "state": r.get::<_, String>(11).map_err(internal)?,
            "review": index.state_of(&review::Leaf::Hunk(r.get::<_, i64>(0).map_err(internal)?)).as_str(),
            "oversize": false,
        });
        // JSON の長さは生の長さ以上なので、生の長さで単独でも収まらないと分かれば JSON にしない
        let mut fits = before.len() + after.len() + pre.len() + post.len() <= HUNKS_PAGE_BUDGET;
        if fits {
            item["beforeText"] = json!(before);
            item["afterText"] = json!(after);
            item["contextPre"] = json!(pre);
            item["contextPost"] = json!(post);
            fits = serde_json::to_string(&item).is_ok_and(|text| text.len() <= HUNKS_PAGE_BUDGET);
        }
        if !fits {
            for key in ["beforeText", "afterText", "contextPre", "contextPost"] {
                item.as_object_mut().expect("object").remove(key);
            }
            item["oversize"] = json!(true);
            item["beforeBytes"] = json!(before.len());
            item["afterBytes"] = json!(after.len());
        }
        let size = serde_json::to_string(&item).map_or(usize::MAX, |text| text.len());
        if !items.is_empty() && used + size > HUNKS_PAGE_BUDGET {
            // 入らない。次のページは直前に入れた Hunk の次から
            next_cursor = items.last().and_then(|v| v["ordinal"].as_i64()).map(|n| n.to_string());
            break;
        }
        used += size;
        items.push(item);
    }
    Ok(json!({ "items": items, "nextCursor": next_cursor }))
}

/// `describe` のシーン変更の要約: 種別ごとの件数、文書の ID、解析不可ならその理由。構造の変更が無い Revision は None。
fn scene_summary(conn: &Connection, revision_id: i64) -> rusqlite::Result<Option<Value>> {
    let mut stmt = conn.prepare(
        "SELECT document_id, change_type, after_value FROM scene_changes WHERE revision_id = ?1 ORDER BY scene_change_id",
    )?;
    let rows: Vec<(String, String, Option<String>)> =
        stmt.query_map([revision_id], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?.collect::<rusqlite::Result<_>>()?;
    let Some(first) = rows.first() else { return Ok(None) };
    let mut counts = serde_json::Map::new();
    let mut unparsed: Option<String> = None;
    for (_, kind, after) in &rows {
        if kind == change_type::UNPARSED {
            unparsed = after.as_deref().and_then(|text| serde_json::from_str::<String>(text).ok());
        }
        let n = counts.get(kind).and_then(Value::as_i64).unwrap_or(0);
        counts.insert(kind.clone(), json!(n + 1));
    }
    Ok(Some(json!({
        "documentId": first.0,
        "resolvedDocumentId": document::resolve(conn, &first.0)?,
        "counts": counts,
        "unparsedReason": unparsed,
    })))
}

/// 1 つの Revision の構造の変更 (`scene_changes`) を、ページ送りして返す。`cursor` は直前のページの最後の `sceneChangeId`。
///
/// 1 ページは JSON の合計が `HUNKS_PAGE_BUDGET` 以内。値が単独で収まらない変更は、値を入れず `oversize: true` と
/// `beforeBytes` / `afterBytes` だけを返す (切らない)。
pub fn scene_diff(conn: &Connection, mut args: Args) -> Result<Value, OpError> {
    let revision_id = args.required_int("revisionId")?;
    let cursor = args.string("cursor")?;
    args.finish()?;
    let after_id = match &cursor {
        None => 0,
        Some(text) => text.parse::<i64>().map_err(|_| OpError::bad_args("cursor が読めない"))?,
    };
    let exists: bool = conn
        .query_row("SELECT EXISTS (SELECT 1 FROM change_revisions WHERE revision_id = ?1)", [revision_id], |r| r.get(0))
        .map_err(internal)?;
    if !exists {
        return Err(OpError::new(code::NOT_FOUND, format!("revisionId {revision_id} は無い")));
    }
    let mut stmt = conn
        .prepare(
            "SELECT scene_change_id, document_id, file_id, change_type, component, field, before_value, after_value, state, caused_by
               FROM scene_changes WHERE revision_id = ?1 AND scene_change_id > ?2 ORDER BY scene_change_id",
        )
        .map_err(internal)?;
    let mut rows = stmt.query(params![revision_id, after_id]).map_err(internal)?;
    let index = review::LatestIndex::new(&review::load_rows(conn, revision_id).map_err(internal)?);
    let mut items: Vec<Value> = Vec::new();
    let mut used = 0usize;
    let mut next_cursor: Option<String> = None;
    while let Some(r) = rows.next().map_err(internal)? {
        let scene_change_id: i64 = r.get(0).map_err(internal)?;
        let document_id: String = r.get(1).map_err(internal)?;
        let before: Option<String> = r.get(6).map_err(internal)?;
        let after: Option<String> = r.get(7).map_err(internal)?;
        let leaf = review::Leaf::Scene {
            file_id: r.get(2).map_err(internal)?,
            component: r.get(4).map_err(internal)?,
            field: r.get(5).map_err(internal)?,
        };
        let mut item = json!({
            "review": index.state_of(&leaf).as_str(),
            "sceneChangeId": scene_change_id,
            "documentId": document_id,
            "resolvedDocumentId": document::resolve(conn, &document_id).map_err(internal)?,
            "fileId": r.get::<_, Option<i64>>(2).map_err(internal)?,
            "changeType": r.get::<_, String>(3).map_err(internal)?,
            "component": r.get::<_, Option<String>>(4).map_err(internal)?,
            "field": r.get::<_, Option<String>>(5).map_err(internal)?,
            "state": r.get::<_, String>(8).map_err(internal)?,
            "oversize": false,
        });
        // PrefabPropagation の原因 (同じ Revision のプレハブ側の行)。伝播の行だけに付ける (他の行の応答の形は変えない)
        if let Some(cause) = r.get::<_, Option<i64>>(9).map_err(internal)? {
            item["causedBy"] = json!(cause);
        }
        let raw = before.as_ref().map_or(0, String::len) + after.as_ref().map_or(0, String::len);
        item["beforeValue"] = json!(before);
        item["afterValue"] = json!(after);
        let fits = raw <= HUNKS_PAGE_BUDGET && serde_json::to_string(&item).is_ok_and(|text| text.len() <= HUNKS_PAGE_BUDGET);
        if !fits {
            item["beforeValue"] = Value::Null;
            item["afterValue"] = Value::Null;
            item["oversize"] = json!(true);
            item["beforeBytes"] = json!(before.as_ref().map(String::len));
            item["afterBytes"] = json!(after.as_ref().map(String::len));
        }
        let size = serde_json::to_string(&item).map_or(usize::MAX, |text| text.len());
        if !items.is_empty() && used + size > HUNKS_PAGE_BUDGET {
            next_cursor = items.last().and_then(|v| v["sceneChangeId"].as_i64()).map(|n| n.to_string());
            break;
        }
        used += size;
        items.push(item);
    }
    Ok(json!({ "items": items, "nextCursor": next_cursor }))
}

/// 16 進 64 桁 (SHA-256、小文字) か。Blob のパスへ変換するので、これ以外の文字列は通さない。
fn is_blob_hash(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// `restore_list`: 復元の履歴 (`restore_operations`)。新しい順。`limit` は既定 50、最大 200。
pub fn restore_list(conn: &Connection, mut args: Args) -> Result<Value, OpError> {
    let limit = args.int("limit")?.unwrap_or(crate::restore::LIST_DEFAULT);
    args.finish()?;
    if limit < 1 {
        return Err(OpError::bad_args("limit は 1 以上"));
    }
    crate::restore::list(conn, limit.min(crate::restore::LIST_MAX))
}

/// Blob の中身の一部。`hash` は 64 桁の 16 進 (小文字) だけ。`length` は最大 512 KiB。結果は base64 の `data` と総サイズ。
/// 範囲読みなので中身とハッシュの照合はしない (書き込み時の検証と起動時の確認で済んでいる)。
pub fn blob_get(blobs_dir: &Path, mut args: Args) -> Result<Value, OpError> {
    let hash = args.required_string("hash")?;
    let offset = args.int("offset")?.unwrap_or(0);
    let length = args.int("length")?.unwrap_or(BLOB_GET_MAX_LENGTH);
    args.finish()?;
    if !is_blob_hash(&hash) {
        return Err(OpError::bad_args("hash は 64 桁の 16 進 (小文字)"));
    }
    if offset < 0 {
        return Err(OpError::bad_args("offset は 0 以上"));
    }
    if length < 1 {
        return Err(OpError::bad_args("length は 1 以上"));
    }
    let length = length.min(BLOB_GET_MAX_LENGTH) as u64;
    let path = blobs_dir.join(&hash[..2]).join(&hash[2..]);
    let missing = || OpError::new(code::BLOB_MISSING, "Blob が無い (除外された・回収された)");
    let mut file = std::fs::File::open(&path).map_err(|_| missing())?;
    let total = file.metadata().map_err(|_| missing())?.len();
    let offset = offset as u64;
    let mut bytes = Vec::new();
    if offset < total {
        file.seek(SeekFrom::Start(offset)).map_err(|_| missing())?;
        file.take(length).read_to_end(&mut bytes).map_err(|_| missing())?;
    }
    Ok(json!({ "data": base64_encode(&bytes), "size": total, "offset": offset, "length": bytes.len() }))
}

/// 標準の base64 (パディングあり)。
pub fn base64_encode(bytes: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let n = (u32::from(chunk[0]) << 16) | (u32::from(*chunk.get(1).unwrap_or(&0)) << 8) | u32::from(*chunk.get(2).unwrap_or(&0));
        out.push(TABLE[(n >> 18) as usize & 63] as char);
        out.push(TABLE[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { TABLE[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { TABLE[n as usize & 63] as char } else { '=' });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_matches_the_rfc_vectors() {
        assert_eq!(base64_encode(b""), "");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_encode(b"foob"), "Zm9vYg==");
        assert_eq!(base64_encode(b"fooba"), "Zm9vYmE=");
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
        assert_eq!(base64_encode(&[0xff, 0xfe, 0xfd]), "//79");
    }

    #[test]
    fn blob_hash_must_be_64_lowercase_hex() {
        assert!(is_blob_hash(&"a".repeat(64)));
        assert!(!is_blob_hash(&"a".repeat(63)));
        assert!(!is_blob_hash(&"A".repeat(64)));
        assert!(!is_blob_hash(&format!("..\\{}", "a".repeat(62))));
        assert!(!is_blob_hash(&format!("{}/", "a".repeat(63))));
    }
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
