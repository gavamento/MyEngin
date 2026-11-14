//! 書き込み側の状態 (DB 接続 + ジャーナル + Blob)。Writer スレッドだけが持つ (architecture.md §3.1)。

use std::fmt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

use rusqlite::{params, Connection};

use crate::blob::{BlobError, BlobStore, PutOutcome};
use crate::hash::sha256_hex;
use crate::journal::{Journal, JournalEntry, JournalPos};

/// `<project>\.mye\ai_changes\` の中の置き場所 (db-schema.md §1)。
#[derive(Clone, Debug)]
pub struct Paths {
    pub dir: PathBuf,
    pub db: PathBuf,
    pub blobs: PathBuf,
    pub journal: PathBuf,
}

impl Paths {
    pub fn for_project(project_root: &Path) -> Paths {
        Self::at(&project_root.join(".mye").join("ai_changes"))
    }

    /// 置き場所のディレクトリを直接指す。
    pub fn at(dir: &Path) -> Paths {
        let dir = dir.to_path_buf();
        Paths {
            db: dir.join("tracking.db"),
            blobs: dir.join("blobs"),
            journal: dir.join("journal"),
            dir,
        }
    }
}

/// 記録の状態。
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Mode {
    /// DB・ジャーナル・Blob のすべてへ書ける。
    Recording,
    /// DB へ書けない (新しい版 / 開けない / 移行の失敗)。ジャーナルへの追記だけ続ける。
    /// 次に書ける状態で起動したとき、ジャーナルから DB へ入る。
    DbDegraded(String),
}

#[derive(Debug)]
pub enum StoreError {
    Io(std::io::Error),
    Sqlite(rusqlite::Error),
    Blob(BlobError),
    /// DB へ書けない状態で DB への書き込みを求められた。
    Degraded(String),
}

impl fmt::Display for StoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            StoreError::Io(e) => write!(f, "I/O: {e}"),
            StoreError::Sqlite(e) => write!(f, "SQLite: {e}"),
            StoreError::Blob(e) => write!(f, "{e}"),
            StoreError::Degraded(r) => write!(f, "db_degraded: {r}"),
        }
    }
}

impl std::error::Error for StoreError {}

impl From<std::io::Error> for StoreError {
    fn from(e: std::io::Error) -> Self {
        StoreError::Io(e)
    }
}
impl From<rusqlite::Error> for StoreError {
    fn from(e: rusqlite::Error) -> Self {
        StoreError::Sqlite(e)
    }
}
impl From<BlobError> for StoreError {
    fn from(e: BlobError) -> Self {
        StoreError::Blob(e)
    }
}

/// 書き込みの所要時間の内訳 (ナノ秒の累計)。計測用 (`mye_track bench`) で、記録の判断には使わない。
#[derive(Default)]
pub struct StoreStats {
    pub journal_append_ns: AtomicU64,
    pub event_insert_ns: AtomicU64,
    pub snapshot_insert_ns: AtomicU64,
    pub tx_commit_ns: AtomicU64,
}

pub struct Store {
    pub stats: StoreStats,
    /// 試験用: Some(OS のエラー番号) の間、`record_event` がそのエラーで失敗する (ジャーナルへ書けない状態を模す)。
    #[doc(hidden)]
    pub inject_journal_error: Option<i32>,
    pub paths: Paths,
    pub mode: Mode,
    pub conn: Option<Connection>,
    pub journal: Journal,
    pub blobs: BlobStore,
}

/// `record_event` の結果。`in_db` が false のときはジャーナルだけに入った (db_degraded)。
#[derive(Debug)]
pub struct RecordedEvent {
    pub pos: JournalPos,
    pub in_db: bool,
}

impl Store {
    /// 観測・要求を記録する。必ずジャーナルが先で、その後に DB の `events` へ入れる。
    /// DB への挿入が失敗してもジャーナルには残るので、次の起動で入る。
    pub fn record_event(
        &mut self,
        at: i64,
        kind: &str,
        payload: serde_json::Value,
    ) -> Result<RecordedEvent, StoreError> {
        let entry = JournalEntry { at, kind: kind.to_string(), payload };
        if let Some(code) = self.inject_journal_error {
            return Err(StoreError::Io(std::io::Error::from_raw_os_error(code)));
        }
        let started = Instant::now();
        let pos = self.journal.append(&entry)?;
        self.stats.journal_append_ns.fetch_add(started.elapsed().as_nanos() as u64, Ordering::Relaxed);
        let Some(conn) = self.conn.as_ref() else {
            return Ok(RecordedEvent { pos, in_db: false });
        };
        let started = Instant::now();
        conn.execute(
            "INSERT OR IGNORE INTO events (at, kind, journal_pos, payload) VALUES (?1, ?2, ?3, ?4)",
            params![at, kind, pos.to_string(), entry.payload.to_string()],
        )?;
        self.stats.event_insert_ns.fetch_add(started.elapsed().as_nanos() as u64, Ordering::Relaxed);
        Ok(RecordedEvent { pos, in_db: true })
    }

    /// 内容のスナップショットを記録する。Blob を検証して確定してから `snapshots` へ行を足す
    /// (検証に失敗したら行を足さない)。除外パスは `blob_hash = NULL`、`blob_state = excluded`。
    /// 戻り値は `snapshot_id`。
    pub fn record_snapshot(
        &mut self,
        rel_path: &str,
        kind: &str,
        taken_at: i64,
        bytes: &[u8],
    ) -> Result<i64, StoreError> {
        self.record_snapshot_hashed(rel_path, kind, taken_at, bytes, &sha256_hex(bytes))
    }

    /// 呼び出し側が内容のハッシュを持っているときの `record_snapshot`。
    pub fn record_snapshot_hashed(
        &mut self,
        rel_path: &str,
        kind: &str,
        taken_at: i64,
        bytes: &[u8],
        hash: &str,
    ) -> Result<i64, StoreError> {
        let Some(conn) = self.conn.as_ref() else {
            let reason = match &self.mode {
                Mode::DbDegraded(r) => r.clone(),
                Mode::Recording => "DB 接続が無い".to_string(),
            };
            return Err(StoreError::Degraded(reason));
        };
        let (hash, size, state) = match self.blobs.put_hashed(rel_path, bytes, hash)? {
            PutOutcome::Stored { hash, size, .. } => (Some(hash), size, "present"),
            PutOutcome::Excluded { size } => (None, size, "excluded"),
        };
        let started = Instant::now();
        conn.execute(
            "INSERT INTO snapshots (blob_hash, size, path, kind, taken_at, blob_state)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![hash, size as i64, rel_path, kind, taken_at, state],
        )?;
        self.stats.snapshot_insert_ns.fetch_add(started.elapsed().as_nanos() as u64, Ordering::Relaxed);
        Ok(conn.last_insert_rowid())
    }
}
