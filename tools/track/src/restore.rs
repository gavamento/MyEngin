//! 復元の記録 (`restore_record` / `restore_expect` / `restore_list`)。サービスは**記録するだけ**で、ファイルを書かない
//! (適用するのは Editor。restore-safety.md §1)。
//!
//! `restore_record` は復元の進行 (`restore_operations`) を `events` へ記録して派生の表へ反映する。`committed` のとき、
//! 戻した対象の `changes.state` / `change_hunks.state` を `restored` / `partially_restored` にする (レビューの記録には触れない。
//! 要求 §10.5)。`applying` のとき、適用前の内容を `pre_restore` のスナップショットとして残す。どちらも `events` から
//! 作り直せる (`session::apply_event` が `rebuild_derived` と同じ関数を通る。spec D26)。

use rusqlite::{params, Connection, OptionalExtension};
use serde_json::{json, Value};

use crate::blob::BlobStore;
use crate::protocol::{code, OpError};

/// `restore_record` の `state`。最初の 6 つは restore-safety.md §6、`recovered` は起動時の回復の画面で利用者が選んで済ませたもの。
pub const STATES: &[&str] =
    &["prepared", "applying", "committed", "rolling_back", "rolled_back", "recovery_required", "recovered"];

/// 1 回の要求が指せる対象の数・パスの数の上限。
pub const MAX_TARGETS: usize = 64;
pub const MAX_PATHS: usize = 256;
/// 1 つの対象が指せる構造の変更の行 (`sceneChangeIds`) の数の上限。
pub const MAX_SCENE_IDS: usize = 4096;
/// `error` の長さの上限 (文字数)。
pub const MAX_ERROR_CHARS: usize = 1000;
/// `restore_list` の既定と上限。
pub const LIST_DEFAULT: i64 = 50;
pub const LIST_MAX: i64 = 200;

/// 完了した (これ以上、進まない) 状態。`finished_at` を入れる。
fn is_terminal(state: &str) -> bool {
    matches!(state, "committed" | "rolled_back" | "recovered" | "recovery_required")
}

pub fn is_restore_id(text: &str) -> bool {
    !text.is_empty() && text.len() <= 64 && text.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
}

pub fn is_blob_hash(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// `restore_record` のイベントを派生の表へ反映する。`at` はイベントの時刻。
pub fn apply_record(conn: &Connection, blobs: &BlobStore, at: i64, payload: &Value) -> rusqlite::Result<()> {
    let (Some(id), Some(state)) = (payload["restore_id"].as_str(), payload["state"].as_str()) else { return Ok(()) };
    let targets = payload["targets"].to_string();
    let pre = payload.get("pre_hashes").filter(|v| !v.is_null()).map(Value::to_string);
    let post = payload.get("post_hashes").filter(|v| !v.is_null()).map(Value::to_string);
    let error = payload["error"].as_str();
    let finished = is_terminal(state).then_some(at);
    // 最初の記録で行を作り、以後は状態・対象・ハッシュ・結果を更新する (開始の時刻は変えない)
    conn.execute(
        "INSERT INTO restore_operations (restore_id, state, targets, pre_hashes, post_hashes, error, started_at, finished_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
         ON CONFLICT(restore_id) DO UPDATE SET
           state = excluded.state,
           targets = excluded.targets,
           pre_hashes = COALESCE(excluded.pre_hashes, restore_operations.pre_hashes),
           post_hashes = COALESCE(excluded.post_hashes, restore_operations.post_hashes),
           error = excluded.error,
           finished_at = excluded.finished_at",
        params![id, state, targets, pre, post, error, at, finished],
    )?;
    if state == "applying" {
        record_pre_restore(conn, blobs, at, payload)?;
    }
    if state == "committed" {
        mark_restored(conn, payload)?;
    }
    Ok(())
}

/// 適用前の内容を `pre_restore` のスナップショットにする。Blob はその内容が観測されたときに既に保存されている (あれば present)。
fn record_pre_restore(conn: &Connection, blobs: &BlobStore, at: i64, payload: &Value) -> rusqlite::Result<()> {
    let Some(map) = payload["pre_hashes"].as_object() else { return Ok(()) };
    let mut paths: Vec<&String> = map.keys().collect();
    paths.sort();
    for path in paths {
        let entry = &map[path];
        let Some(hash) = entry["hash"].as_str() else { continue };
        let state = if blobs.path_for_hash(hash).is_file() { "present" } else { "missing" };
        conn.execute(
            "INSERT INTO snapshots (blob_hash, size, path, kind, taken_at, blob_state) VALUES (?1, ?2, ?3, 'pre_restore', ?4, ?5)",
            params![hash, entry["size"].as_i64().unwrap_or(0), path, at, state],
        )?;
    }
    Ok(())
}

/// `committed` の復元が戻した対象の状態を更新する。ファイル全体 → `restored`。Hunk 単位 → その Hunk を `restored` にして、
/// 全部戻れば変更も `restored`、一部なら `partially_restored`。
fn mark_restored(conn: &Connection, payload: &Value) -> rusqlite::Result<()> {
    for target in payload["targets"].as_array().into_iter().flatten() {
        let Some(revision_id) = target["revision_id"].as_i64() else { continue };
        let change_id: Option<i64> = conn
            .query_row("SELECT change_id FROM change_revisions WHERE revision_id = ?1", [revision_id], |r| r.get(0))
            .optional()?;
        let Some(change_id) = change_id else { continue };
        if target["scope"].as_str() == Some("scene") {
            // ライブのシーンの復元: 戻した構造の変更の行だけ `restored` にする (強調の表 `scene_overlay` は active の行だけを返す)。
            // 変更 (ファイル) の状態は変えない: ファイルは戻していない
            for id in target["scene_change_ids"].as_array().into_iter().flatten().filter_map(Value::as_i64) {
                conn.execute(
                    "UPDATE scene_changes SET state = 'restored' WHERE scene_change_id = ?1 AND revision_id = ?2",
                    params![id, revision_id],
                )?;
            }
        } else if target["scope"].as_str() == Some("hunks") {
            for hunk in target["hunk_ids"].as_array().into_iter().flatten().filter_map(Value::as_i64) {
                conn.execute(
                    "UPDATE change_hunks SET state = 'restored' WHERE hunk_id = ?1 AND revision_id = ?2",
                    params![hunk, revision_id],
                )?;
            }
            let remaining: i64 = conn.query_row(
                "SELECT COUNT(*) FROM change_hunks WHERE revision_id = ?1 AND state <> 'restored'",
                [revision_id],
                |r| r.get(0),
            )?;
            let state = if remaining == 0 { "restored" } else { "partially_restored" };
            conn.execute("UPDATE changes SET state = ?1 WHERE change_id = ?2 AND state <> 'restored'", params![state, change_id])?;
        } else {
            conn.execute("UPDATE change_hunks SET state = 'restored' WHERE revision_id = ?1", [revision_id])?;
            conn.execute("UPDATE changes SET state = 'restored' WHERE change_id = ?1", [change_id])?;
        }
    }
    Ok(())
}

/// `restore_list` (role = editor): 新しい復元から。
pub fn list(conn: &Connection, limit: i64) -> Result<Value, OpError> {
    let db_err = |e: rusqlite::Error| OpError::new(code::INTERNAL, format!("DB の読み取りに失敗: {e}"));
    let mut stmt = conn
        .prepare(
            "SELECT restore_id, state, targets, pre_hashes, post_hashes, error, started_at, finished_at
               FROM restore_operations ORDER BY started_at DESC, restore_id DESC LIMIT ?1",
        )
        .map_err(db_err)?;
    let rows = stmt
        .query_map([limit], |r| {
            let parse = |text: Option<String>| text.and_then(|t| serde_json::from_str::<Value>(&t).ok()).unwrap_or(Value::Null);
            // 保存はイベントの形 (snake_case)。応答は他の op と同じ camelCase にする
            let targets: Vec<Value> = parse(r.get(2)?)
                .as_array()
                .into_iter()
                .flatten()
                .map(|t| {
                    let mut item = json!({ "revisionId": t["revision_id"], "scope": t["scope"], "hunkIds": t["hunk_ids"], "paths": t["paths"] });
                    if t.get("scene_change_ids").is_some() {
                        item["sceneChangeIds"] = t["scene_change_ids"].clone();
                    }
                    item
                })
                .collect();
            Ok(json!({
                "restoreId": r.get::<_, String>(0)?,
                "state": r.get::<_, String>(1)?,
                "targets": targets,
                "preHashes": parse(r.get(3)?),
                "postHashes": parse(r.get(4)?),
                "error": r.get::<_, Option<String>>(5)?,
                "startedAt": r.get::<_, i64>(6)?,
                "finishedAt": r.get::<_, Option<i64>>(7)?,
            }))
        })
        .map_err(db_err)?;
    let items: Vec<Value> = rows.collect::<rusqlite::Result<_>>().map_err(db_err)?;
    Ok(json!({ "items": items }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ids_and_hashes_are_checked_strictly() {
        assert!(is_restore_id("a1b2-c3") && !is_restore_id("") && !is_restore_id("a/b") && !is_restore_id(&"a".repeat(65)));
        assert!(is_blob_hash(&"a".repeat(64)) && !is_blob_hash(&"A".repeat(64)) && !is_blob_hash("abc"));
    }

    #[test]
    fn only_finished_states_get_a_finish_time() {
        for s in STATES {
            assert_eq!(is_terminal(s), matches!(*s, "committed" | "rolled_back" | "recovered" | "recovery_required"));
        }
    }
}
