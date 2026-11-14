//! 観測の記録 (Writer スレッドの中で動く)。
//!
//! 観測は必ず `Store::record_event` を通す。DB を作り直したとき、`events` から派生の表
//! (`snapshots` / `changes` / `change_revisions`) を導けるようにするため、イベントの payload に
//! パス・種別・Before / After のハッシュとサイズ・completeness・観測時刻を入れる (spec §2 D26)。
//! イベントに無い情報を派生の表だけに書かない。

use rusqlite::{params, OptionalExtension};
use serde_json::{json, Value};

use crate::baseline::{ContentKind, FileState};
use crate::store::Store;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChangeType {
    Added,
    Modified,
    Deleted,
    Renamed,
}

impl ChangeType {
    pub fn as_str(self) -> &'static str {
        match self {
            ChangeType::Added => "added",
            ChangeType::Modified => "modified",
            ChangeType::Deleted => "deleted",
            ChangeType::Renamed => "renamed",
        }
    }
}

/// db-schema.md の `change_revisions.completeness`。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Completeness {
    Complete,
    Coalesced,
    Gap,
    NoBefore,
}

impl Completeness {
    pub fn as_str(self) -> &'static str {
        match self {
            Completeness::Complete => "complete",
            Completeness::Coalesced => "coalesced",
            Completeness::Gap => "gap",
            Completeness::NoBefore => "no_before",
        }
    }
}

/// 観測の出どころ。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Source {
    Watch,
    StartupGap,
    OverflowGap,
}

impl Source {
    pub fn as_str(self) -> &'static str {
        match self {
            Source::Watch => "watch",
            Source::StartupGap => "startup_gap",
            Source::OverflowGap => "overflow_gap",
        }
    }
    pub fn is_gap(self) -> bool {
        !matches!(self, Source::Watch)
    }
}

pub struct AfterContent {
    pub bytes: Vec<u8>,
    pub hash: String,
    pub kind: ContentKind,
}

/// 1 ファイルの 1 回の変更の観測。
pub struct Observation {
    pub path: String,
    pub change_type: ChangeType,
    /// リネームのときだけ。
    pub old_path: Option<String>,
    /// 最後に確認できた内容。無ければ None。
    pub before: Option<FileState>,
    /// 削除なら None。
    pub after: Option<AfterContent>,
    pub completeness: Completeness,
    pub source: Source,
    /// 同じ flush に入っていた観測を結ぶ印。別ディレクトリへの移動 (削除 + 作成) のまとめは sub-04 がこれを使う。
    pub window: String,
    pub observed_at: i64,
}

/// ベースライン 1 ファイル分。
pub struct BaselineEntry {
    pub path: String,
    pub bytes: Vec<u8>,
    pub hash: String,
    pub kind: ContentKind,
}

/// 書き込みの塊を 1 トランザクションにする。panic で開いたまま残っていたら先に戻す。
fn begin(store: &Store) -> bool {
    let Some(conn) = store.conn.as_ref() else { return false };
    if !conn.is_autocommit() {
        let _ = conn.execute_batch("ROLLBACK");
    }
    conn.execute_batch("BEGIN").is_ok()
}

fn commit(store: &Store) {
    if let Some(conn) = store.conn.as_ref() {
        if !conn.is_autocommit() {
            let _ = conn.execute_batch("COMMIT");
        }
    }
}

fn state_json(state: &FileState) -> Value {
    json!({
        "hash": state.blob_hash,
        "size": state.size,
        "kind": state.kind.map(ContentKind::as_str),
    })
}

fn after_json(after: &AfterContent, excluded: bool) -> Value {
    json!({
        "hash": if excluded { None } else { Some(after.hash.as_str()) },
        "size": after.bytes.len(),
        "kind": after.kind.as_str(),
        "excluded": excluded,
    })
}

/// 観測を記録して、各観測のあとの状態 (削除なら None) を返す。Err は記録できなかった (状態を進めない)。
pub fn apply_observations(store: &mut Store, observations: Vec<Observation>) -> Vec<Result<Option<FileState>, String>> {
    let in_tx = begin(store);
    let results = observations.into_iter().map(|o| apply_one(store, o)).collect();
    if in_tx {
        commit(store);
    }
    results
}

fn apply_one(store: &mut Store, o: Observation) -> Result<Option<FileState>, String> {
    let excluded = o.after.is_some() && store.blobs.is_excluded(&o.path);
    let payload = json!({
        "path": o.path,
        "change_type": o.change_type.as_str(),
        "old_path": o.old_path,
        "before": o.before.as_ref().map(state_json),
        "after": o.after.as_ref().map(|a| after_json(a, excluded)),
        "completeness": o.completeness.as_str(),
        "source": o.source.as_str(),
        "window": o.window,
        "observed_at": o.observed_at,
    });
    // 先にジャーナルへ。これが失敗したら観測は無かったことにして呼び出し側へ返す
    store.record_event(o.observed_at, "observed", payload).map_err(|e| e.to_string())?;

    let after_state = match &o.after {
        Some(a) => {
            let (snapshot_id, blob_hash) = match store.conn {
                Some(_) => match store.record_snapshot(&o.path, "disk", o.observed_at, &a.bytes) {
                    Ok(id) => (Some(id), (!excluded).then(|| a.hash.clone())),
                    Err(e) => {
                        eprintln!("[track] スナップショットを残せない: {}: {e}", o.path);
                        (None, None)
                    }
                },
                // DB へ書けないときも Blob は残す (次に書ける状態で起動したとき Before になる)
                None => match store.blobs.put(&o.path, &a.bytes) {
                    Ok(_) => (None, (!excluded).then(|| a.hash.clone())),
                    Err(e) => {
                        eprintln!("[track] Blob を残せない: {}: {e}", o.path);
                        (None, None)
                    }
                },
            };
            Some(FileState {
                hash: Some(a.hash.clone()),
                size: a.bytes.len() as u64,
                snapshot_id,
                blob_hash,
                kind: Some(a.kind),
            })
        }
        None => None,
    };

    if store.conn.is_some() {
        if let Err(e) = insert_revision(store, &o, after_state.as_ref()) {
            eprintln!("[track] Revision を残せない: {}: {e}", o.path);
        }
    }
    Ok(after_state)
}

fn insert_revision(store: &Store, o: &Observation, after: Option<&FileState>) -> rusqlite::Result<()> {
    let conn = store.conn.as_ref().expect("caller checked");
    let kind = after.and_then(|s| s.kind).or(o.before.as_ref().and_then(|s| s.kind));
    let existing: Option<(i64, i64)> = conn
        .query_row(
            "SELECT c.change_id, COALESCE((SELECT MAX(seq) FROM change_revisions WHERE change_id = c.change_id), 0)
               FROM changes c WHERE c.path = ?1 AND c.state = 'active' ORDER BY c.change_id DESC LIMIT 1",
            [&o.path],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )
        .optional()?;
    let (change_id, last_seq) = match existing {
        Some(found) => found,
        None => {
            conn.execute(
                "INSERT INTO changes (path, kind, first_seen, state) VALUES (?1, ?2, ?3, 'active')",
                params![o.path, kind.map_or("text", ContentKind::as_str), o.observed_at],
            )?;
            (conn.last_insert_rowid(), 0)
        }
    };
    conn.execute(
        "INSERT INTO change_revisions
           (change_id, seq, change_type, old_path, before_snapshot, after_snapshot, observed_at,
            completeness, operation_id, actor, attribution, caused_by)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, NULL, 'unknown', 'unknown', NULL)",
        params![
            change_id,
            last_seq + 1,
            o.change_type.as_str(),
            o.old_path,
            o.before.as_ref().and_then(|s| s.snapshot_id),
            after.and_then(|s| s.snapshot_id),
            o.observed_at,
            o.completeness.as_str(),
        ],
    )?;
    Ok(())
}

/// 最初の起動のベースライン (`snapshots.kind = baseline`)。変更としては記録しない。
pub fn record_baseline(store: &mut Store, entries: Vec<BaselineEntry>, taken_at: i64) -> Vec<Result<FileState, String>> {
    let listing: Vec<Value> = entries
        .iter()
        .map(|e| {
            let excluded = store.blobs.is_excluded(&e.path);
            json!({
                "path": e.path,
                "hash": if excluded { None } else { Some(e.hash.as_str()) },
                "size": e.bytes.len(),
                "kind": e.kind.as_str(),
                "excluded": excluded,
            })
        })
        .collect();
    if let Err(e) = store.record_event(taken_at, "baseline", json!({ "taken_at": taken_at, "files": listing })) {
        let reason = e.to_string();
        return entries.iter().map(|_| Err(reason.clone())).collect();
    }
    let in_tx = begin(store);
    let results = entries
        .into_iter()
        .map(|e| {
            let excluded = store.blobs.is_excluded(&e.path);
            let (snapshot_id, blob_hash) = match store.conn {
                Some(_) => match store.record_snapshot(&e.path, "baseline", taken_at, &e.bytes) {
                    Ok(id) => (Some(id), (!excluded).then(|| e.hash.clone())),
                    Err(err) => {
                        eprintln!("[track] ベースラインを残せない: {}: {err}", e.path);
                        (None, None)
                    }
                },
                None => match store.blobs.put(&e.path, &e.bytes) {
                    Ok(_) => (None, (!excluded).then(|| e.hash.clone())),
                    Err(err) => {
                        eprintln!("[track] Blob を残せない: {}: {err}", e.path);
                        (None, None)
                    }
                },
            };
            Ok(FileState {
                hash: Some(e.hash),
                size: e.bytes.len() as u64,
                snapshot_id,
                blob_hash,
                kind: Some(e.kind),
            })
        })
        .collect();
    if in_tx {
        commit(store);
    }
    results
}

/// 欠落区間を `events` (`coverage_gap`) と `coverage_gaps` の両方へ残す。
pub fn record_gap(store: &mut Store, reason: &str, from_at: i64, to_at: i64, scope: Option<&str>) -> Result<(), String> {
    store
        .record_event(
            to_at,
            "coverage_gap",
            json!({ "reason": reason, "from_at": from_at, "to_at": to_at, "scope": scope }),
        )
        .map_err(|e| e.to_string())?;
    if let Some(conn) = store.conn.as_ref() {
        conn.execute(
            "INSERT INTO coverage_gaps (from_at, to_at, reason, scope) VALUES (?1, ?2, ?3, ?4)",
            params![from_at, to_at, reason, scope],
        )
        .map_err(|e| e.to_string())?;
    }
    Ok(())
}
