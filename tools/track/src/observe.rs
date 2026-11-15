//! 観測の記録 (Writer スレッドの中で動く)。
//!
//! 観測は必ず `Store::record_event` を通す。DB を作り直したとき、`events` から派生の表
//! (`snapshots` / `changes` / `change_revisions`) を導けるようにするため、イベントの payload に
//! パス・種別・Before / After のハッシュとサイズ・completeness・観測時刻を入れる (spec §2 D26)。
//! イベントに無い情報を派生の表だけに書かない。

use std::time::SystemTime;

use rusqlite::params;
use serde_json::{json, Value};

use crate::attribution;
use crate::baseline::{ContentKind, FileState};
use crate::blob::BlobError;
use crate::revision::{write_revision, DiffPlan, RevisionInput};
use crate::store::{Store, StoreError};

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

impl ChangeType {
    pub fn parse(text: &str) -> Option<ChangeType> {
        match text {
            "added" => Some(ChangeType::Added),
            "modified" => Some(ChangeType::Modified),
            "deleted" => Some(ChangeType::Deleted),
            "renamed" => Some(ChangeType::Renamed),
            _ => None,
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

impl Completeness {
    pub fn parse(text: &str) -> Option<Completeness> {
        match text {
            "complete" => Some(Completeness::Complete),
            "coalesced" => Some(Completeness::Coalesced),
            "gap" => Some(Completeness::Gap),
            "no_before" => Some(Completeness::NoBefore),
            _ => None,
        }
    }
}

/// 観測の出どころ。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Source {
    Watch,
    StartupGap,
    OverflowGap,
    /// 検証の走査 (取りこぼしの検出、記録に失敗した後の突き合わせ)。
    VerifyGap,
    /// 管理された操作 (`op_begin`) の前に、未記録の変更を先に確定する読み取り。操作の変更とは別の Revision になる。
    ManagedBegin,
    /// 管理された操作の終わり (`op_end` / `report`) の即時の読み取り。デバウンスを待たない。
    Managed,
}

impl Source {
    pub fn as_str(self) -> &'static str {
        match self {
            Source::Watch => "watch",
            Source::StartupGap => "startup_gap",
            Source::OverflowGap => "overflow_gap",
            Source::VerifyGap => "verify_gap",
            Source::ManagedBegin => "managed_begin",
            Source::Managed => "managed",
        }
    }
    pub fn is_gap(self) -> bool {
        !matches!(self, Source::Watch | Source::ManagedBegin | Source::Managed)
    }
}

pub struct AfterContent {
    pub bytes: Vec<u8>,
    pub hash: String,
    pub kind: ContentKind,
    pub modified: Option<SystemTime>,
}

/// 記録できなかった理由。`reason` は `coverage_gaps.reason` に入る値 (`disk_full` / `write_failed`)。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RecordFailure {
    pub reason: &'static str,
    pub detail: String,
}

pub const GAP_DISK_FULL: &str = "disk_full";
pub const GAP_WRITE_FAILED: &str = "write_failed";

fn io_disk_full(e: &std::io::Error) -> bool {
    // 112 = ERROR_DISK_FULL、39 = ERROR_HANDLE_DISK_FULL
    matches!(e.raw_os_error(), Some(112) | Some(39)) || e.kind() == std::io::ErrorKind::StorageFull
}

impl RecordFailure {
    pub fn from_store(e: &StoreError) -> Self {
        let full = match e {
            StoreError::Io(io) | StoreError::Blob(BlobError::Io(io)) => io_disk_full(io),
            StoreError::Sqlite(rusqlite::Error::SqliteFailure(f, _)) => f.code == rusqlite::ErrorCode::DiskFull,
            _ => false,
        };
        RecordFailure { reason: if full { GAP_DISK_FULL } else { GAP_WRITE_FAILED }, detail: e.to_string() }
    }
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
    pub modified: Option<SystemTime>,
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
            let started = std::time::Instant::now();
            let _ = conn.execute_batch("COMMIT");
            store.stats.tx_commit_ns.fetch_add(started.elapsed().as_nanos() as u64, std::sync::atomic::Ordering::Relaxed);
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

/// `observed` イベントの payload (db-schema.md §5.1)。Diff ワーカーと Writer が同じものから Revision の入力を作る。
pub fn observation_payload(o: &Observation, excluded: bool) -> Value {
    json!({
        "path": o.path,
        "change_type": o.change_type.as_str(),
        "old_path": o.old_path,
        "before": o.before.as_ref().map(state_json),
        "after": o.after.as_ref().map(|a| after_json(a, excluded)),
        "completeness": o.completeness.as_str(),
        "source": o.source.as_str(),
        "window": o.window,
        "observed_at": o.observed_at,
    })
}

/// 観測を記録して、各観測のあとの状態 (削除なら None) を返す。Err は記録できなかった (状態を進めない)。
/// `plans` は Diff ワーカーが観測ごとに作った差分 (`observations` と同じ順・同じ長さ)。
pub fn apply_observations(
    store: &mut Store,
    observations: Vec<Observation>,
    plans: Vec<Result<DiffPlan, String>>,
) -> Vec<Result<Option<FileState>, RecordFailure>> {
    let in_tx = begin(store);
    let results =
        observations.into_iter().zip(plans).map(|(o, plan)| apply_one(store, o, plan)).collect();
    if in_tx {
        commit(store);
    }
    results
}

fn apply_one(store: &mut Store, o: Observation, plan: Result<DiffPlan, String>) -> Result<Option<FileState>, RecordFailure> {
    // 差分を計算できなかった観測は、何も書かずに Err で返す (状態を進めず、欠落として扱う)
    let plan = plan.map_err(|detail| RecordFailure { reason: GAP_WRITE_FAILED, detail })?;
    let excluded = o.after.is_some() && store.blobs.is_excluded(&o.path);
    let payload = observation_payload(&o, excluded);
    let input = RevisionInput::from_payload(&store.blobs, &payload)
        .ok_or_else(|| RecordFailure { reason: GAP_WRITE_FAILED, detail: "観測の payload を読めない".to_string() })?;
    // 先にジャーナルへ。これが失敗したら観測は無かったことにして呼び出し側へ返す
    store.record_event(o.observed_at, "observed", payload).map_err(|e| RecordFailure::from_store(&e))?;

    let after_state = match &o.after {
        Some(a) => {
            // スナップショットを残せなければ Revision だけ残さない: 状態を進めず、呼び出し側が欠落として扱う
            let (snapshot_id, blob_hash) = match store.conn {
                Some(_) => {
                    let id = store
                        .record_snapshot_hashed(&o.path, "disk", o.observed_at, &a.bytes, &a.hash)
                        .map_err(|e| RecordFailure::from_store(&e))?;
                    (Some(id), (!excluded).then(|| a.hash.clone()))
                }
                // DB へ書けないときも Blob は残す (次に書ける状態で起動したとき Before になる)
                None => {
                    store.blobs.put_hashed(&o.path, &a.bytes, &a.hash).map_err(|e| RecordFailure::from_store(&StoreError::Blob(e)))?;
                    (None, (!excluded).then(|| a.hash.clone()))
                }
            };
            Some(FileState {
                hash: Some(a.hash.clone()),
                size: a.bytes.len() as u64,
                snapshot_id,
                blob_hash,
                kind: Some(a.kind),
                modified: a.modified,
            })
        }
        None => None,
    };

    if let Some(conn) = store.conn.as_ref() {
        // Revision と Hunk は 1 つの塊で確定する (revision.rs)。失敗したら何も残さず Err
        let before_snapshot = o.before.as_ref().and_then(|s| s.snapshot_id);
        let after_snapshot = after_state.as_ref().and_then(|s| s.snapshot_id);
        let revision_id = write_revision(conn, &input, before_snapshot, after_snapshot, &plan)
            .map_err(|e| RecordFailure::from_store(&StoreError::Sqlite(e)))?;
        // 帰属の計算に失敗しても Revision は残す (結論は unknown のまま。次の作り直しで直る)
        if let Err(e) = attribution::evaluate_new_revision(conn, &store.attribution, revision_id, o.observed_at, o.source.as_str(), None) {
            eprintln!("[track] 帰属を計算できない: {}: {e}", o.path);
        }
    }
    Ok(after_state)
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
                Some(_) => match store.record_snapshot_hashed(&e.path, "baseline", taken_at, &e.bytes, &e.hash) {
                    Ok(id) => (Some(id), (!excluded).then(|| e.hash.clone())),
                    Err(err) => {
                        eprintln!("[track] ベースラインを残せない: {}: {err}", e.path);
                        (None, None)
                    }
                },
                None => match store.blobs.put_hashed(&e.path, &e.bytes, &e.hash) {
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
                modified: e.modified,
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
