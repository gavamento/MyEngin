//! DB・Blob・ジャーナル・起動時の検査・Writer の結合テスト (M104c)。

use std::path::{Path, PathBuf};

use rusqlite::Connection;
use serde_json::json;

use mye_track::blob::PutOutcome;
use mye_track::hash::sha256_hex;
use mye_track::journal::{self, Journal, JournalEntry};
use mye_track::process::process_start_ms;
use mye_track::schema::{Migration, MIGRATIONS};
use mye_track::startup::{
    open_project, open_project_with, ServiceState, StartupOptions, GAP_JOURNAL_UNREADABLE,
};
use mye_track::store::{Mode, Paths, StoreError};
use mye_track::writer::{Writer, WriterError};

/// テストごとの空のプロジェクトルート。成功したテストは終わりに消す (失敗したときは調べられるよう残す)。
struct TempProject(PathBuf);

impl std::ops::Deref for TempProject {
    type Target = Path;
    fn deref(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempProject {
    fn drop(&mut self) {
        if !std::thread::panicking() {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
}

fn temp_project(name: &str) -> TempProject {
    let dir = std::env::temp_dir().join(format!("mye_track_{}_{}", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    TempProject(dir)
}

fn db_conn(root: &Path) -> Connection {
    Connection::open(Paths::for_project(root).db).unwrap()
}

fn count(conn: &Connection, sql: &str) -> i64 {
    conn.query_row(sql, [], |r| r.get(0)).unwrap()
}

fn blob_files(root: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for prefix in std::fs::read_dir(Paths::for_project(root).blobs).unwrap().flatten() {
        for f in std::fs::read_dir(prefix.path()).unwrap().flatten() {
            out.push(f.path());
        }
    }
    out
}

const NOW: i64 = 1_794_651_330_123;

#[test]
fn fresh_project_creates_layout_and_all_tables() {
    let root = temp_project("fresh");
    let (store, report) = open_project(&root, NOW);
    assert_eq!(report.state, Some(ServiceState::Recording));
    let store = store.unwrap();
    assert_eq!(store.mode, Mode::Recording);
    drop(store);

    let paths = Paths::for_project(&root);
    assert!(paths.db.is_file() && paths.blobs.is_dir() && paths.journal.is_dir());

    let conn = db_conn(&root);
    let expected = [
        "schema_migrations", "sessions", "operations", "documents", "changes", "change_revisions",
        "change_hunks", "scene_changes", "attribution_evidence", "snapshots", "reviews",
        "restore_operations", "coverage_gaps", "events",
    ];
    for table in expected {
        let n = count(&conn, &format!("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='{table}'"));
        assert_eq!(n, 1, "表 {table}");
    }
    assert_eq!(count(&conn, "SELECT MAX(version) FROM schema_migrations"), 1);
    // db-schema.md §3 の索引 8 本 + events の一意索引
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name LIKE 'idx_%'"), 9);
    // PRAGMA は接続ごと。WAL はファイルに残る設定
    let mode: String = conn.query_row("PRAGMA journal_mode", [], |r| r.get(0)).unwrap();
    assert_eq!(mode, "wal");
}

#[test]
fn migrations_are_idempotent() {
    let root = temp_project("idem");
    for _ in 0..2 {
        let (store, report) = open_project(&root, NOW);
        assert_eq!(report.state, Some(ServiceState::Recording));
        drop(store);
    }
    let conn = db_conn(&root);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM schema_migrations"), 1);
    // 版 1 のままなら移行前のコピーは作らない
    let dir = Paths::for_project(&root).dir;
    assert!(!std::fs::read_dir(dir).unwrap().flatten().any(|e| e.file_name().to_string_lossy().contains(".bak-")));
}

const V2: Migration = Migration { version: 2, note: "テスト用", sql: "CREATE TABLE extra_v2 (x INTEGER);" };
const V2_BROKEN: Migration = Migration {
    version: 2,
    note: "テスト用 (失敗する)",
    sql: "CREATE TABLE half_done (x INTEGER); CREATE TABLE half_done (x INTEGER);",
};

#[test]
fn newer_db_is_not_written_and_degrades() {
    let root = temp_project("newer");
    let with_v2: Vec<Migration> = MIGRATIONS.iter().copied().chain([V2]).collect();
    let (store, report) = open_project_with(&root, NOW, StartupOptions { migrations: &with_v2, ..Default::default() });
    assert_eq!(report.state, Some(ServiceState::Recording));
    drop(store);
    // 旧版 (v1) から v2 へ上げたので、移行前のコピーがある、は別テストで見る。ここは新しい DB を作った状態

    let db = Paths::for_project(&root).db;
    let before = sha256_hex(&std::fs::read(&db).unwrap());

    // このサービス (版 1 まで) で開く
    let (store, report) = open_project(&root, NOW);
    assert!(matches!(report.state, Some(ServiceState::DbDegraded(_))), "{:?}", report.state);
    let mut store = store.expect("degraded でも Store は返る");
    assert!(matches!(store.mode, Mode::DbDegraded(_)));
    assert!(store.conn.is_none());

    // DB には書かない。ジャーナルには残す
    assert!(matches!(store.record_snapshot("a.txt", "disk", NOW, b"x"), Err(StoreError::Degraded(_))));
    let rec = store.record_event(NOW, "observed", json!({"path": "a.txt"})).unwrap();
    assert!(!rec.in_db);
    drop(store);
    assert_eq!(before, sha256_hex(&std::fs::read(&db).unwrap()), "新しい版の DB のバイト列が変わった");
    assert_eq!(journal::read_after(&Paths::for_project(&root).journal, None).unwrap().len(), 1);
}

#[test]
fn upgrade_backs_up_old_db_and_failed_migration_rolls_back() {
    let root = temp_project("upgrade");
    drop(open_project(&root, NOW).0);

    // 失敗する v2: 元のまま db_degraded。コピーは取ってある
    let broken: Vec<Migration> = MIGRATIONS.iter().copied().chain([V2_BROKEN]).collect();
    let (_, report) = open_project_with(&root, NOW, StartupOptions { migrations: &broken, ..Default::default() });
    assert!(matches!(report.state, Some(ServiceState::DbDegraded(_))));
    let conn = db_conn(&root);
    assert_eq!(count(&conn, "SELECT MAX(version) FROM schema_migrations"), 1);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM sqlite_master WHERE name='half_done'"), 0, "途中の表が残った");
    drop(conn);

    // 成功する v2
    let ok: Vec<Migration> = MIGRATIONS.iter().copied().chain([V2]).collect();
    let (store, report) = open_project_with(&root, NOW, StartupOptions { migrations: &ok, ..Default::default() });
    assert_eq!(report.state, Some(ServiceState::Recording));
    drop(store);
    let conn = db_conn(&root);
    assert_eq!(count(&conn, "SELECT MAX(version) FROM schema_migrations"), 2);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM schema_migrations"), 2);
    let backup = Paths::for_project(&root).dir.join("tracking.db.bak-v1");
    assert!(backup.is_file(), "移行前のコピーが無い");
    // コピーは旧版のまま (v2 の表を持たない)
    let old = Connection::open(&backup).unwrap();
    assert_eq!(count(&old, "SELECT COUNT(*) FROM sqlite_master WHERE name='extra_v2'"), 0);
}

fn event_json(at: i64, kind: &str) -> String {
    serde_json::to_string(&JournalEntry { at, kind: kind.into(), payload: json!({"k": kind}) }).unwrap()
}

#[test]
fn corrupt_db_is_quarantined_and_rebuilt_from_journal_with_gap() {
    let root = temp_project("corrupt");
    let (store, _) = open_project(&root, NOW);
    let mut store = store.unwrap();
    for (i, kind) in ["observed", "reported", "op_end"].iter().enumerate() {
        store.record_event(1000 + i as i64 * 100, kind, json!({"k": kind})).unwrap();
    }
    drop(store);

    // ジャーナルの 2 行目を壊し、DB の先頭を上書きする
    let paths = Paths::for_project(&root);
    let jpath = paths.journal.join("000001.ndjson");
    let text = std::fs::read_to_string(&jpath).unwrap();
    let mut lines: Vec<&str> = text.lines().collect();
    lines[1] = "{ this is not json";
    std::fs::write(&jpath, lines.join("\n") + "\n").unwrap();
    let mut bytes = std::fs::read(&paths.db).unwrap();
    bytes[..256].fill(0xA5);
    std::fs::write(&paths.db, bytes).unwrap();

    let (store, report) = open_project(&root, NOW);
    assert_eq!(report.state, Some(ServiceState::Recording));
    drop(store);

    let moved = report.broken_db.expect("退避先がある");
    assert!(moved.file_name().unwrap().to_string_lossy().starts_with("tracking.db.broken-"));
    assert!(moved.is_file());
    assert_eq!(report.replayed_events, 2);
    assert_eq!(report.unreadable_journal_lines, 1);

    let conn = db_conn(&root);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM events WHERE kind IN ('observed','op_end')"), 2);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM schema_migrations"), 1, "空の DB に移行が当たっていない");
    // 区間は壊れた行の前後の観測の時刻 (1000 〜 1200)
    let (from, to, reason): (i64, i64, String) = conn
        .query_row("SELECT from_at, to_at, reason FROM coverage_gaps", [], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))
        .unwrap();
    assert_eq!((from, to, reason.as_str()), (1000, 1200, GAP_JOURNAL_UNREADABLE));
    drop(conn);

    // もう一度起動しても二重にならない
    let (store, report) = open_project(&root, NOW);
    drop(store);
    assert_eq!(report.replayed_events, 0);
    let conn = db_conn(&root);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM events"), 3);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM coverage_gaps"), 1);
}

#[test]
fn truncated_journal_tail_becomes_gap_up_to_now_once() {
    let root = temp_project("tail");
    drop(open_project(&root, NOW).0);
    let good = event_json(500, "observed");
    // 2 行目は書きかけ (改行なし)
    let path = Paths::for_project(&root).journal.join("000001.ndjson");
    std::fs::write(&path, format!("{good}\n{{\"at\":600,\"ki")).unwrap();

    let (store, report) = open_project(&root, 9000);
    assert_eq!(report.unreadable_journal_lines, 1);
    let mut store = store.unwrap();
    // 書きかけの行は閉じられ、続きは次の行番号に入る
    let rec = store.record_event(700, "reported", json!({})).unwrap();
    assert_eq!(rec.pos.to_string(), "000001.ndjson:3");
    drop(store);

    let conn = db_conn(&root);
    let (from, to): (i64, i64) = conn
        .query_row("SELECT from_at, to_at FROM coverage_gaps", [], |r| Ok((r.get(0)?, r.get(1)?)))
        .unwrap();
    assert_eq!((from, to), (500, 9000));

    let (store, report) = open_project(&root, 12_000);
    drop(store);
    assert_eq!(report.unreadable_journal_lines, 0, "二度目の起動で同じ壊れ行を数え直した");
    assert_eq!(count(&db_conn(&root), "SELECT COUNT(*) FROM coverage_gaps"), 1);
}

#[test]
fn journal_lines_not_yet_in_db_are_replayed_exactly_once() {
    let root = temp_project("replay");
    let (store, _) = open_project(&root, NOW);
    drop(store);

    // 「ジャーナルに追記した後、DB へ入れる前に落ちた」状態: ジャーナルだけに 2 行ある
    let jdir = Paths::for_project(&root).journal;
    let mut j = Journal::open(&jdir, journal::DEFAULT_MAX_FILE_BYTES).unwrap();
    for (i, kind) in ["observed", "reported"].iter().enumerate() {
        let entry = JournalEntry { at: 10 + i as i64, kind: kind.to_string(), payload: json!({"i": i}) };
        j.append(&entry).unwrap();
    }
    drop(j);
    assert_eq!(count(&db_conn(&root), "SELECT COUNT(*) FROM events"), 0);

    for expected_replayed in [2, 0, 0] {
        let (store, report) = open_project(&root, NOW);
        drop(store);
        assert_eq!(report.replayed_events, expected_replayed);
        assert_eq!(count(&db_conn(&root), "SELECT COUNT(*) FROM events"), 2);
    }
    let positions: Vec<String> = {
        let conn = db_conn(&root);
        let mut stmt = conn.prepare("SELECT journal_pos FROM events ORDER BY event_id").unwrap();
        stmt.query_map([], |r| r.get(0)).unwrap().map(|r| r.unwrap()).collect()
    };
    assert_eq!(positions, ["000001.ndjson:1", "000001.ndjson:2"]);
}

#[test]
fn journal_rotates_to_next_file_and_replays_in_order() {
    let root = temp_project("rotate");
    let jdir = Paths::for_project(&root).journal;
    std::fs::create_dir_all(&jdir).unwrap();
    let mut j = Journal::open(&jdir, 200).unwrap();
    let mut last = None;
    for i in 0..8 {
        let entry = JournalEntry { at: i, kind: "observed".into(), payload: json!({"i": i}) };
        last = Some(j.append(&entry).unwrap());
    }
    assert!(last.unwrap().file >= 2, "200 バイトで次の番号へ進まない");
    for f in std::fs::read_dir(&jdir).unwrap().flatten() {
        assert!(f.metadata().unwrap().len() <= 200);
    }
    let lines = journal::read_after(&jdir, None).unwrap();
    let ats: Vec<i64> = lines.iter().map(|l| l.entry.as_ref().unwrap().at).collect();
    assert_eq!(ats, (0..8).collect::<Vec<_>>());

    // 開き直すと続きから書く
    drop(j);
    let mut j = Journal::open(&jdir, 200).unwrap();
    let next = j.append(&JournalEntry { at: 8, kind: "observed".into(), payload: json!({}) }).unwrap();
    assert!(next > last.unwrap());
}

#[test]
fn identical_blobs_share_one_file_and_bytes_are_preserved() {
    let root = temp_project("dedupe");
    let mut store = open_project(&root, NOW).0.unwrap();
    // BOM、CRLF、LF 単独、CR 単独、末尾の改行なし
    let bytes: &[u8] = b"\xEF\xBB\xBFline1\r\nline2\nline3\rtail";
    let a = store.record_snapshot("assets/a.txt", "disk", NOW, bytes).unwrap();
    let b = store.record_snapshot("assets/b.txt", "baseline", NOW + 1, bytes).unwrap();
    assert_ne!(a, b);

    let files = blob_files(&root);
    assert_eq!(files.len(), 1);
    assert_eq!(std::fs::read(&files[0]).unwrap(), bytes, "Blob のバイト列が変わった");

    let hash = sha256_hex(bytes);
    // 先頭 2 桁のディレクトリ + 残り 62 桁
    assert_eq!(files[0].parent().unwrap().file_name().unwrap().to_string_lossy(), hash[..2]);
    assert_eq!(files[0].file_name().unwrap().to_string_lossy(), hash[2..]);

    let conn = store.conn.as_ref().unwrap();
    assert_eq!(count(conn, &format!("SELECT COUNT(*) FROM snapshots WHERE blob_hash='{hash}' AND blob_state='present'")), 2);
    assert_eq!(count(conn, &format!("SELECT COUNT(*) FROM snapshots WHERE size={}", bytes.len())), 2);
}

#[test]
fn verification_failure_adds_no_snapshot_row_and_leaves_no_files() {
    let root = temp_project("verify");
    let mut store = open_project(&root, NOW).0.unwrap();
    // 読み直しが別の内容を返す = 書いた内容が壊れた、の差し込み
    store.blobs.read_back = |_| Ok(b"corrupted".to_vec());

    let err = store.record_snapshot("a.txt", "disk", NOW, b"payload").unwrap_err();
    assert!(matches!(err, StoreError::Blob(_)), "{err}");
    assert_eq!(count(store.conn.as_ref().unwrap(), "SELECT COUNT(*) FROM snapshots"), 0);
    assert!(blob_files(&root).is_empty(), "最終名または一時ファイルが残った");

    // 口を戻せば同じ内容が通る (失敗が状態を汚していない)
    store.blobs.read_back = mye_track::blob::read_from_start;
    store.record_snapshot("a.txt", "disk", NOW, b"payload").unwrap();
    assert_eq!(count(store.conn.as_ref().unwrap(), "SELECT COUNT(*) FROM snapshots"), 1);
}

#[test]
fn excluded_paths_keep_no_blob_and_are_marked_excluded() {
    let root = temp_project("excluded");
    let mut store = open_project(&root, NOW).0.unwrap();
    let secret = b"API_KEY=hunter2";
    for path in [".env", "cfg/.env.local", "a/server.PEM", "k.key", "x.pfx", "y.p12", "id_rsa", "id_rsa.pub", "credentials.json", "credentials-prod.json"] {
        store.record_snapshot(path, "disk", NOW, secret).unwrap();
    }
    assert!(blob_files(&root).is_empty(), "除外パターンの内容が Blob に残った");
    let conn = store.conn.as_ref().unwrap();
    assert_eq!(count(conn, "SELECT COUNT(*) FROM snapshots WHERE blob_hash IS NULL AND blob_state='excluded'"), 10);
    assert_eq!(count(conn, &format!("SELECT COUNT(*) FROM snapshots WHERE size={}", secret.len())), 10);

    // 当たらない名前は保存される
    store.record_snapshot("env.txt", "disk", NOW, secret).unwrap();
    assert_eq!(blob_files(&root).len(), 1);
    assert_eq!(
        store.blobs.put("notes.env.txt", secret).unwrap(),
        PutOutcome::Stored { hash: sha256_hex(secret), size: secret.len() as u64, existed: true }
    );
}

#[test]
fn startup_marks_missing_blobs_and_counts_orphans_without_deleting() {
    let root = temp_project("blobcheck");
    let mut store = open_project(&root, NOW).0.unwrap();
    store.record_snapshot("a.txt", "disk", NOW, b"alive").unwrap();
    store.record_snapshot("b.txt", "disk", NOW, b"gone").unwrap();
    let gone = store.blobs.path_for_hash(&sha256_hex(b"gone"));
    drop(store);
    std::fs::remove_file(&gone).unwrap();
    // どの snapshots にも指されない Blob と、書きかけの一時ファイル
    let orphan_hash = sha256_hex(b"orphan");
    let orphan = Paths::for_project(&root).blobs.join(&orphan_hash[..2]).join(&orphan_hash[2..]);
    std::fs::create_dir_all(orphan.parent().unwrap()).unwrap();
    std::fs::write(&orphan, b"orphan").unwrap();
    std::fs::write(orphan.parent().unwrap().join(".tmp-1-1"), b"partial").unwrap();

    let (store, report) = open_project(&root, NOW);
    drop(store);
    assert_eq!(report.blobs_marked_missing, 1);
    assert_eq!(report.orphan_blobs, 1);
    assert!(orphan.is_file(), "起動時に孤児 Blob を消した");
    let conn = db_conn(&root);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM snapshots WHERE path='b.txt' AND blob_state='missing'"), 1);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM snapshots WHERE path='a.txt' AND blob_state='present'"), 1);
}

fn insert_session(conn: &Connection, id: &str, status: &str, pid: Option<i64>, start: Option<i64>) {
    conn.execute(
        "INSERT INTO sessions (session_id, provider, source, process_id, process_start, workspace, started_at, status, coverage)
         VALUES (?1, 'claude', 'terminal', ?2, ?3, 'c:\\proj', 1, ?4, '{}')",
        rusqlite::params![id, pid, start, status],
    )
    .unwrap();
}

#[test]
fn stale_sessions_become_interrupted_and_open_operations_abandoned() {
    let root = temp_project("sessions");
    drop(open_project(&root, NOW).0);

    let mut child = std::process::Command::new("cmd").args(["/C", "exit"]).spawn().unwrap();
    let dead_pid = child.id() as i64;
    child.wait().unwrap();
    let own_pid = std::process::id() as i64;
    let own_start = process_start_ms(std::process::id()).expect("自分の開始時刻");

    let conn = db_conn(&root);
    insert_session(&conn, "dead", "running", Some(dead_pid), Some(own_start));
    insert_session(&conn, "alive", "running", Some(own_pid), Some(own_start));
    insert_session(&conn, "reused-pid", "running", Some(own_pid), Some(own_start - 86_400_000));
    insert_session(&conn, "no-pid", "running", None, None);
    insert_session(&conn, "done", "finished", Some(dead_pid), None);
    conn.execute(
        "INSERT INTO operations (operation_id, session_id, operation_type, started_at, finished_at, reported_targets, observed_targets, result)
         VALUES ('op-open', 'dead', 'edit', 1, NULL, '[]', '[]', NULL), ('op-done', 'dead', 'edit', 1, 5, '[]', '[]', 'ok')",
        [],
    )
    .unwrap();
    drop(conn);

    let (store, report) = open_project(&root, NOW);
    drop(store);
    assert_eq!(report.sessions_interrupted, 3);
    assert_eq!(report.operations_abandoned, 1);

    let conn = db_conn(&root);
    let status = |id: &str| -> String {
        conn.query_row("SELECT status FROM sessions WHERE session_id=?1", [id], |r| r.get(0)).unwrap()
    };
    assert_eq!(status("dead"), "interrupted");
    assert_eq!(status("alive"), "running", "生きているプロセスの session を止めた");
    assert_eq!(status("reused-pid"), "interrupted", "PID が使い回された session を生きていると見なした");
    assert_eq!(status("no-pid"), "interrupted");
    assert_eq!(status("done"), "finished");
    let result = |id: &str| -> Option<String> {
        conn.query_row("SELECT result FROM operations WHERE operation_id=?1", [id], |r| r.get(0)).unwrap()
    };
    assert_eq!(result("op-open").as_deref(), Some("abandoned"));
    assert_eq!(result("op-done").as_deref(), Some("ok"));
}

#[test]
fn unusable_location_returns_unavailable_without_panicking() {
    let root = temp_project("unavailable");
    // `.mye` がファイルなので、その下に記録領域を作れない
    std::fs::write(root.join(".mye"), b"not a directory").unwrap();
    let (store, report) = open_project(&root, NOW);
    assert!(store.is_none());
    assert!(matches!(report.state, Some(ServiceState::Unavailable(_))), "{:?}", report.state);
}

#[test]
fn writer_serializes_jobs_in_queue_order_and_survives_a_panic() {
    let root = temp_project("writer");
    let writer = Writer::spawn(open_project(&root, NOW).0.unwrap());

    // 4 スレッドから同時に積む。DB に入る順 (event_id) とジャーナルの順が一致する
    std::thread::scope(|s| {
        for t in 0..4 {
            let writer = &writer;
            s.spawn(move || {
                for i in 0..25 {
                    writer
                        .call(move |store| store.record_event(t * 100 + i, "observed", json!({"t": t, "i": i})).unwrap())
                        .unwrap();
                }
            });
        }
    });
    // panic したジョブは JobPanicked を返し、後続は実行される
    assert_eq!(writer.call(|_| -> () { panic!("テスト用の panic") }), Err(WriterError::JobPanicked));
    let rx = writer.submit(|store| store.record_event(999, "observed", json!({}))).unwrap();
    assert!(rx.recv().unwrap().is_ok());
    writer.shutdown();

    let conn = db_conn(&root);
    assert_eq!(count(&conn, "SELECT COUNT(*) FROM events"), 101);
    let mut stmt = conn.prepare("SELECT event_id, journal_pos FROM events ORDER BY event_id").unwrap();
    let rows: Vec<(i64, String)> = stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?))).unwrap().map(|r| r.unwrap()).collect();
    for (n, (id, pos)) in rows.iter().enumerate() {
        assert_eq!(*id, n as i64 + 1);
        assert_eq!(pos, &format!("000001.ndjson:{}", n + 1));
    }
}

#[test]
fn shutdown_runs_queued_jobs_before_stopping() {
    let root = temp_project("stopped");
    let writer = Writer::spawn(open_project(&root, NOW).0.unwrap());
    let rx = writer.submit(|_| 7).unwrap();
    writer.shutdown();
    assert_eq!(rx.recv().unwrap(), 7, "shutdown は積まれたジョブを実行してから止まる");
}
