//! 起動時の検査 (db-schema.md §6 の 1〜6 と一時 Blob の掃除。7 のベースラインは watch.rs)。
//!
//! どの段で失敗してもプロセスは落とさない。DB へ書けないときは `Mode::DbDegraded` の Store を、
//! ディレクトリすら作れないときは Store 無し (`ServiceState::Unavailable`) を返す。

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::time::{Duration, UNIX_EPOCH};

use rusqlite::{params, Connection};

use crate::blob::BlobStore;
use crate::db::{open_and_migrate, Opened};
use crate::diff::DiffLimits;
use crate::hash::sha256_hex;
use crate::journal::{self, Journal, JournalPos};
use crate::revision::{rebuild_derived, Deriver, RebuildReport};
use crate::schema::{Migration, MIGRATIONS};
use crate::session;
use crate::store::{Mode, Paths, Store};

/// 前回の最後のイベントからこれだけ遡って書かれた Blob の中身を、起動時に確かめる。
/// Blob は 1 件ごとに `sync_all` しないので、電源断の直前の書き込みは中身が欠けうる (db-schema.md §4)。
const RECENT_BLOB_WINDOW: Duration = Duration::from_secs(10 * 60);

/// 入れ直せなかったジャーナルの行を表す `coverage_gaps.reason` と `events.kind`。
pub const GAP_JOURNAL_UNREADABLE: &str = "journal_unreadable";

/// サービスの記録の状態 (pipe の応答や標準エラーへ出す)。
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ServiceState {
    Recording,
    DbDegraded(String),
    /// 記録の置き場所を作れない。Store が無い。
    Unavailable(String),
}

/// 起動時の検査の結果。数は「その起動で行ったこと」。
#[derive(Debug, Default)]
pub struct StartupReport {
    pub state: Option<ServiceState>,
    /// 壊れた DB の退避先 (退避したときだけ)。
    pub broken_db: Option<PathBuf>,
    pub replayed_events: usize,
    pub unreadable_journal_lines: usize,
    /// 壊れた DB を作り直したとき、イベントから派生の表を作り直した結果。
    pub rebuilt: Option<RebuildReport>,
    pub blobs_marked_missing: usize,
    /// 直近に書かれた Blob のうち、中身がハッシュと違ったので消した数 (消すと `blobs_marked_missing` に数えられる)。
    pub blobs_corrupted: usize,
    pub orphan_blobs: usize,
    /// 前回の書きかけの一時 Blob (`.tmp-*`) を消した数。
    pub temp_blobs_removed: usize,
    pub sessions_interrupted: usize,
    pub operations_abandoned: usize,
    /// 起動時の検査 (3〜6) で失敗した段。記録は続けるが、`status` が DB の健全性として返す。
    pub failed_steps: Vec<StepFailure>,
}

/// 失敗した検査の段とその理由。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StepFailure {
    pub step: &'static str,
    pub reason: String,
}

/// テストが差し替える設定。
pub struct StartupOptions<'a> {
    pub migrations: &'a [Migration],
    pub journal_max_file_bytes: u64,
    pub exclude_patterns: Vec<String>,
    /// 記録の置き場所を `<project>\.myei_changes` 以外へ向ける (計測用。追跡対象のリポジトリを汚さない)。
    pub data_dir: Option<PathBuf>,
}

impl Default for StartupOptions<'_> {
    fn default() -> Self {
        StartupOptions {
            migrations: MIGRATIONS,
            journal_max_file_bytes: journal::DEFAULT_MAX_FILE_BYTES,
            exclude_patterns: crate::blob::DEFAULT_EXCLUDE_PATTERNS.iter().map(|s| s.to_string()).collect(),
            data_dir: None,
        }
    }
}

/// プロジェクトの記録領域を開き、起動時の検査を行う。
pub fn open_project(project_root: &Path, now: i64) -> (Option<Store>, StartupReport) {
    open_project_with(project_root, now, StartupOptions::default())
}

pub fn open_project_with(
    project_root: &Path,
    now: i64,
    options: StartupOptions<'_>,
) -> (Option<Store>, StartupReport) {
    let mut report = StartupReport::default();
    let paths = match &options.data_dir {
        Some(dir) => Paths::at(dir),
        None => Paths::for_project(project_root),
    };

    // ディレクトリとジャーナルが開けなければ、記録できない状態
    for dir in [&paths.dir, &paths.blobs, &paths.journal] {
        if let Err(e) = std::fs::create_dir_all(dir) {
            report.state = Some(ServiceState::Unavailable(format!("{} を作れない: {e}", dir.display())));
            return (None, report);
        }
    }
    // サービスは 1 つだけ動く前提 (mutex) なので、起動時に残っている `.tmp-*` は前回の書きかけ
    report.temp_blobs_removed = remove_stale_temp_blobs(&paths.blobs);
    let journal = match Journal::open(&paths.journal, options.journal_max_file_bytes) {
        Ok(j) => j,
        Err(e) => {
            report.state = Some(ServiceState::Unavailable(format!("ジャーナルを開けない: {e}")));
            return (None, report);
        }
    };

    // §6 の 1・2: DB を開く (壊れていれば退避して作り直す) → 移行
    let opened = open_and_migrate(&paths.db, options.migrations, now);
    report.broken_db = opened.broken_moved_to;
    let (conn, mode) = match opened.opened {
        Opened::Ready(conn) => (Some(conn), Mode::Recording),
        Opened::Degraded(reason) => (None, Mode::DbDegraded(reason)),
    };
    let mut store = Store {
        project_root: project_root.to_path_buf(),
        stats: Default::default(),
        inject_journal_error: None,
        attribution: Default::default(),
        blobs: BlobStore::new(paths.blobs.clone(), options.exclude_patterns),
        paths,
        mode,
        conn,
        journal,
    };

    if store.conn.is_some() {
        // 退避して作り直した DB は、全イベントを入れてから派生の表を作り直す。通常の起動は、入れ直すイベントごとに派生の表へも反映する
        let derive_while_replaying = report.broken_db.is_none();
        // 入れ直しに失敗したときは、DB の内容を信用できないので書き込みを止める
        if let Err(e) = replay_journal(&mut store, now, &mut report, derive_while_replaying) {
            store.conn = None;
            store.mode = Mode::DbDegraded(format!("ジャーナルの入れ直しに失敗: {e}"));
        }
    }
    if store.conn.is_some() && report.broken_db.is_some() {
        match rebuild_derived(&mut store, &DiffLimits::default()) {
            Ok(rebuilt) => report.rebuilt = Some(rebuilt),
            Err(e) => report.failed_steps.push(StepFailure { step: "派生の表の作り直し", reason: e.to_string() }),
        }
    }
    if let Some(conn) = store.conn.as_mut() {
        // §6 の 3〜6。保守の段なので、失敗しても記録は続ける
        run_step(&mut report, "直近の Blob の内容確認", |r| verify_recent_blobs(conn, &store.paths.blobs, r));
        run_step(&mut report, "Blob の有無の確認", |r| mark_missing_blobs(conn, &store.blobs, r));
        run_step(&mut report, "孤児 Blob の計数", |r| count_orphan_blobs(conn, &store.paths.blobs, r));
    }
    if store.conn.is_some() {
        // 整理はイベントにして派生の表へ反映する (作り直した DB が同じ状態になる)
        match session::startup_cleanup(&mut store, now) {
            Ok(done) => {
                report.sessions_interrupted = done.interrupted;
                report.operations_abandoned = done.abandoned;
            }
            Err(e) => report.failed_steps.push(StepFailure { step: "残った session の整理", reason: e.to_string() }),
        }
    }
    report.state = Some(match &store.mode {
        Mode::Recording => ServiceState::Recording,
        Mode::DbDegraded(reason) => ServiceState::DbDegraded(reason.clone()),
    });
    (Some(store), report)
}

/// `blobs\<2 桁>\.tmp-*` を消す。自分が書いたもので、どこからも指されない。消した数を返す。
fn remove_stale_temp_blobs(blobs_dir: &Path) -> usize {
    let Ok(prefixes) = std::fs::read_dir(blobs_dir) else { return 0 };
    let mut removed = 0;
    for prefix in prefixes.flatten() {
        let Ok(files) = std::fs::read_dir(prefix.path()) else { continue };
        for file in files.flatten() {
            if file.file_name().to_string_lossy().starts_with(".tmp-") && std::fs::remove_file(file.path()).is_ok() {
                removed += 1;
            }
        }
    }
    removed
}

fn run_step(
    report: &mut StartupReport,
    name: &'static str,
    step: impl FnOnce(&mut StartupReport) -> rusqlite::Result<()>,
) {
    if let Err(e) = step(report) {
        report.failed_steps.push(StepFailure { step: name, reason: e.to_string() });
    }
}

/// §6 の 1 の後半 (および通常起動の埋め直し): DB が持つ最後の位置より後ろのジャーナルを DB へ入れる。
/// 読めない行は `journal_unreadable` のイベントにして位置を進め (= 二度目の起動で重複させない)、
/// 前後の観測の時刻で挟んだ区間を `coverage_gaps` に残す。
/// `derive` が真なら、入れ直したイベントを派生の表 (Revision など) へも反映する。
fn replay_journal(store: &mut Store, now: i64, report: &mut StartupReport, derive: bool) -> Result<(), String> {
    let conn = store.conn.as_mut().expect("caller checked");
    let last: Option<(String, i64)> = conn
        .query_row("SELECT journal_pos, at FROM events ORDER BY event_id DESC LIMIT 1", [], |r| {
            Ok((r.get(0)?, r.get(1)?))
        })
        .ok();
    let after = last.as_ref().and_then(|(pos, _)| JournalPos::parse(pos));
    let lines = journal::read_after(&store.paths.journal, after).map_err(|e| e.to_string())?;
    if lines.is_empty() {
        return Ok(());
    }

    // 壊れた行の区間の始まり = 直前の良い観測の時刻
    let mut prev_good_at = last.map(|(_, at)| at);
    let mut open_gap_from: Option<i64> = None;
    let mut gaps: Vec<(i64, i64)> = Vec::new();
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    let mut deriver: Option<Deriver> = None;
    for line in &lines {
        match &line.entry {
            Some(entry) => {
                if let Some(from) = open_gap_from.take() {
                    gaps.push((from, entry.at));
                }
                let inserted = tx
                    .execute(
                        "INSERT OR IGNORE INTO events (at, kind, journal_pos, payload) VALUES (?1, ?2, ?3, ?4)",
                        params![entry.at, entry.kind, line.pos.to_string(), entry.payload.to_string()],
                    )
                    .map_err(|e| e.to_string())?;
                if derive && inserted == 1 && Deriver::wants(&entry.kind) {
                    if deriver.is_none() {
                        let mut fresh = Deriver::from_db(&tx).map_err(|e| e.to_string())?;
                        fresh.root = Some(store.project_root.clone());
                        deriver = Some(fresh);
                    }
                    if let Some(deriver) = deriver.as_mut() {
                        deriver
                            .apply_event(&tx, &store.blobs, &entry.kind, entry.at, &entry.payload, &line.pos.to_string(), &DiffLimits::default())
                            .map_err(|e| e.to_string())?;
                    }
                }
                prev_good_at = Some(entry.at);
                report.replayed_events += 1;
            }
            None => {
                let at = prev_good_at.unwrap_or(now);
                tx.execute(
                    "INSERT OR IGNORE INTO events (at, kind, journal_pos, payload) VALUES (?1, ?2, ?3, '{}')",
                    params![at, GAP_JOURNAL_UNREADABLE, line.pos.to_string()],
                )
                .map_err(|e| e.to_string())?;
                open_gap_from.get_or_insert(at);
                report.unreadable_journal_lines += 1;
            }
        }
    }
    // 末尾が壊れた行なら、次の観測が無いので今までを区間にする
    if let Some(from) = open_gap_from {
        gaps.push((from, now.max(from)));
    }
    for (from, to) in gaps {
        tx.execute(
            "INSERT INTO coverage_gaps (from_at, to_at, reason, scope) VALUES (?1, ?2, ?3, NULL)",
            params![from, to, GAP_JOURNAL_UNREADABLE],
        )
        .map_err(|e| e.to_string())?;
    }
    tx.commit().map_err(|e| e.to_string())
}

/// §6 の 3 の前段: 前回の最後のイベントの 10 分前から後に書かれた Blob を読み、ハッシュが名前と違えば消す
/// (最終名のまま残すと、同じ内容の `put` が「あった」と判断して書き直さない)。消した Blob を指す行は、
/// 続く `mark_missing_blobs` が `missing` にする。
fn verify_recent_blobs(conn: &mut Connection, blobs_dir: &Path, report: &mut StartupReport) -> rusqlite::Result<()> {
    let last_at: Option<i64> = conn.query_row("SELECT MAX(at) FROM events", [], |r| r.get(0))?;
    let Some(last_at) = last_at else { return Ok(()) };
    let since = UNIX_EPOCH + Duration::from_millis((last_at.max(0) as u64).saturating_sub(RECENT_BLOB_WINDOW.as_millis() as u64));
    let Ok(prefixes) = std::fs::read_dir(blobs_dir) else { return Ok(()) };
    for prefix in prefixes.flatten() {
        let prefix_name = prefix.file_name().to_string_lossy().into_owned();
        if prefix_name.len() != 2 {
            continue;
        }
        let Ok(files) = std::fs::read_dir(prefix.path()) else { continue };
        for file in files.flatten() {
            let rest = file.file_name().to_string_lossy().into_owned();
            if rest.len() != 62 || !file.metadata().and_then(|m| m.modified()).is_ok_and(|t| t >= since) {
                continue;
            }
            let Ok(bytes) = std::fs::read(file.path()) else { continue };
            if sha256_hex(&bytes) != format!("{prefix_name}{rest}") && std::fs::remove_file(file.path()).is_ok() {
                report.blobs_corrupted += 1;
            }
        }
    }
    Ok(())
}

/// §6 の 3: `snapshots` が指す Blob が無ければ `blob_state = missing` にする。
fn mark_missing_blobs(conn: &mut Connection, blobs: &BlobStore, report: &mut StartupReport) -> rusqlite::Result<()> {
    let candidates: Vec<(i64, String)> = {
        let mut stmt = conn.prepare(
            "SELECT snapshot_id, blob_hash FROM snapshots WHERE blob_state = 'present' AND blob_hash IS NOT NULL",
        )?;
        let rows = stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?)))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    for (id, hash) in candidates {
        if !blobs.path_for_hash(&hash).is_file() {
            conn.execute("UPDATE snapshots SET blob_state = 'missing' WHERE snapshot_id = ?1", [id])?;
            report.blobs_marked_missing += 1;
        }
    }
    Ok(())
}

/// §6 の 4: どの `snapshots` からも指されていない Blob を数える。消すのは容量の回収のときだけ。
fn count_orphan_blobs(conn: &mut Connection, blobs_dir: &Path, report: &mut StartupReport) -> rusqlite::Result<()> {
    let referenced: HashSet<String> = {
        let mut stmt = conn.prepare("SELECT DISTINCT blob_hash FROM snapshots WHERE blob_hash IS NOT NULL")?;
        let rows = stmt.query_map([], |r| r.get::<_, String>(0))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    let Ok(prefixes) = std::fs::read_dir(blobs_dir) else { return Ok(()) };
    for prefix in prefixes.flatten() {
        let prefix_name = prefix.file_name().to_string_lossy().into_owned();
        if prefix_name.len() != 2 {
            continue;
        }
        let Ok(files) = std::fs::read_dir(prefix.path()) else { continue };
        for file in files.flatten() {
            let rest = file.file_name().to_string_lossy().into_owned();
            // 書きかけの一時ファイル (".tmp-...") は Blob ではない
            if rest.len() == 62 && !referenced.contains(&format!("{prefix_name}{rest}")) {
                report.orphan_blobs += 1;
            }
        }
    }
    Ok(())
}
