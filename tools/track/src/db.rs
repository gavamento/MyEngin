//! SQLite の開閉と移行 (db-schema.md §2、§6 の 1〜2、§8)。

use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use rusqlite::{Connection, OptionalExtension};

use crate::schema::Migration;

/// UTC のミリ秒 (db-schema.md §2)。
pub fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as i64)
}

/// 開いた DB の状態。
pub enum Opened {
    /// 書き込める。
    Ready(Connection),
    /// 書き込まずに動く。`reason` は db_degraded の理由。
    Degraded(String),
}

/// `open_checked` の結果。壊れた DB を退避したら、その行き先を持つ。
pub struct OpenReport {
    pub opened: Opened,
    pub broken_moved_to: Option<PathBuf>,
}

/// 退避して作り直してよい「壊れ方」か。ロックや I/O の失敗で正常な DB を退避しないための判定。
fn is_corruption(e: &rusqlite::Error) -> bool {
    matches!(
        e,
        rusqlite::Error::SqliteFailure(f, _)
            if matches!(
                f.code,
                rusqlite::ErrorCode::DatabaseCorrupt | rusqlite::ErrorCode::NotADatabase
            )
    )
}

/// `PRAGMA quick_check` まで通る接続を開く。無ければ作る。
fn open_checked(path: &Path) -> rusqlite::Result<Connection> {
    let conn = Connection::open(path)?;
    conn.busy_timeout(std::time::Duration::from_secs(5))?;
    let verdict: String = conn.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
    if verdict != "ok" {
        return Err(rusqlite::Error::SqliteFailure(
            rusqlite::ffi::Error::new(rusqlite::ffi::SQLITE_CORRUPT),
            Some(format!("quick_check: {verdict}")),
        ));
    }
    Ok(conn)
}

/// db-schema.md §2 の PRAGMA を設定する。
fn configure(conn: &Connection) -> rusqlite::Result<()> {
    let _mode: String = conn.pragma_update_and_check(None, "journal_mode", "WAL", |r| r.get(0))?;
    conn.pragma_update(None, "synchronous", "NORMAL")?;
    conn.pragma_update(None, "foreign_keys", "ON")?;
    Ok(())
}

/// 適用済みの最大の版。`schema_migrations` が無ければ 0。
pub fn current_version(conn: &Connection) -> rusqlite::Result<i64> {
    let has_table: Option<i64> = conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'schema_migrations'",
            [],
            |r| r.get(0),
        )
        .optional()?;
    if has_table.is_none() {
        return Ok(0);
    }
    conn.query_row("SELECT COALESCE(MAX(version), 0) FROM schema_migrations", [], |r| r.get(0))
}

/// `broken-<時刻>` の接尾辞に使う UTC の時刻 (`20261114T101530123Z`)。
fn timestamp_label(ms: i64) -> String {
    let secs = ms.div_euclid(1000);
    let milli = ms.rem_euclid(1000);
    let days = secs.div_euclid(86_400);
    let rem = secs.rem_euclid(86_400);
    // days → 西暦 (Howard Hinnant の civil_from_days)
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let mut year = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    if month <= 2 {
        year += 1;
    }
    format!(
        "{year:04}{month:02}{day:02}T{:02}{:02}{:02}{milli:03}Z",
        rem / 3600,
        rem % 3600 / 60,
        rem % 60
    )
}

/// 壊れた DB を `tracking.db.broken-<時刻>` へ退避する。`-wal` / `-shm` も同じ接尾辞で連れていく
/// (残すと、作り直した空の DB に古い WAL が当たる)。
fn quarantine(db_path: &Path, now: i64) -> std::io::Result<PathBuf> {
    let label = timestamp_label(now);
    let base = db_path.as_os_str().to_owned();
    let mut target = PathBuf::new();
    for attempt in 0.. {
        let mut name = base.clone();
        name.push(format!(".broken-{label}"));
        if attempt > 0 {
            name.push(format!("-{attempt}"));
        }
        target = PathBuf::from(name);
        if !target.exists() {
            break;
        }
    }
    std::fs::rename(db_path, &target)?;
    for suffix in ["-wal", "-shm"] {
        let mut from = base.clone();
        from.push(suffix);
        let from = PathBuf::from(from);
        if from.exists() {
            let mut to = target.as_os_str().to_owned();
            to.push(suffix);
            std::fs::rename(&from, PathBuf::from(to))?;
        }
    }
    Ok(target)
}

/// db-schema.md §6 の 1 と 2: DB を開き、壊れていれば退避して作り直し、移行を当てる。
/// どれかが失敗しても panic せず `Opened::Degraded` を返す。
pub fn open_and_migrate(db_path: &Path, migrations: &[Migration], now: i64) -> OpenReport {
    let mut broken_moved_to = None;
    let conn = match open_checked(db_path) {
        Ok(conn) => conn,
        Err(first) if is_corruption(&first) => {
            // 壊れている (NOTADB / CORRUPT / quick_check 不合格)。接続は閉じてあるので退避できる
            match quarantine(db_path, now).and_then(|to| {
                broken_moved_to = Some(to);
                open_checked(db_path).map_err(std::io::Error::other)
            }) {
                Ok(conn) => conn,
                Err(second) => {
                    return OpenReport {
                        opened: Opened::Degraded(format!(
                            "DB を開けず、退避と作り直しにも失敗した: {first} / {second}"
                        )),
                        broken_moved_to,
                    };
                }
            }
        }
        // ロックなど壊れていない理由で開けないときは、触らずに書き込みなしで動く
        Err(other) => {
            return OpenReport {
                opened: Opened::Degraded(format!("DB を開けない: {other}")),
                broken_moved_to,
            };
        }
    };
    let opened = match migrate(conn, db_path, migrations) {
        Ok(conn) => Opened::Ready(conn),
        Err(reason) => Opened::Degraded(reason),
    };
    OpenReport { opened, broken_moved_to }
}

/// 足りない移行を 1 トランザクションで当てる。冪等 (当て済みなら何もしない)。
/// DB の版が自分より新しければ、何も書かずに Err (= db_degraded)。
/// 失敗したら元のまま Err (トランザクションは戻る)。
pub fn migrate(mut conn: Connection, db_path: &Path, migrations: &[Migration]) -> Result<Connection, String> {
    let latest = migrations.last().map_or(0, |m| m.version);
    let current = current_version(&conn).map_err(|e| format!("スキーマ版を読めない: {e}"))?;
    if current > latest {
        return Err(format!(
            "DB のスキーマ版 {current} がこのサービスの版 {latest} より新しい (書き込まない)"
        ));
    }
    configure(&conn).map_err(|e| format!("PRAGMA の設定に失敗: {e}"))?;
    let pending: Vec<&Migration> = migrations.iter().filter(|m| m.version > current).collect();
    if pending.is_empty() {
        return Ok(conn);
    }
    if current >= 1 {
        // 旧版の DB は、移行の前にコピーを残す。WAL を本体へ畳んでからコピーする
        let _: (i64, i64, i64) = conn
            .query_row("PRAGMA wal_checkpoint(TRUNCATE)", [], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))
            .map_err(|e| format!("バックアップ前のチェックポイントに失敗: {e}"))?;
        let mut backup = db_path.as_os_str().to_owned();
        backup.push(format!(".bak-v{current}"));
        std::fs::copy(db_path, PathBuf::from(backup)).map_err(|e| format!("移行前のコピーに失敗: {e}"))?;
    }
    let tx = conn.transaction().map_err(|e| format!("トランザクションを開始できない: {e}"))?;
    for m in pending {
        tx.execute_batch(m.sql)
            .map_err(|e| format!("移行 v{} の実行に失敗: {e}", m.version))?;
        tx.execute(
            "INSERT INTO schema_migrations (version, applied_at, note) VALUES (?1, ?2, ?3)",
            rusqlite::params![m.version, now_ms(), m.note],
        )
        .map_err(|e| format!("移行 v{} の記録に失敗: {e}", m.version))?;
    }
    tx.commit().map_err(|e| format!("移行のコミットに失敗: {e}"))?;
    Ok(conn)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timestamp_label_is_utc_calendar() {
        // 2026-11-14T10:15:30.123Z
        assert_eq!(timestamp_label(1_794_651_330_123), "20261114T101530123Z");
        assert_eq!(timestamp_label(0), "19700101T000000000Z");
        // 閏日
        assert_eq!(timestamp_label(951_782_400_000), "20000229T000000000Z");
    }
}
