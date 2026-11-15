//! 取りこぼしの検出・記録に失敗した観測・ベースラインの中断の結合テスト (M104d2)。
//!
//! 待ちは `tests/watch.rs` と同じく、固定の sleep ではなく上限付きの待ち合わせ (`wait_until`) で書く。
//! 全テストの後処理で、テスト自身が書いた分を除いて作業ツリー (`.mye` を除く) が変わっていないことを検査する (spec §2 D17)。

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use rusqlite::Connection;

use mye_track::collector::TrackerConfig;
use mye_track::db::now_ms;
use mye_track::hash::sha256_hex;
use mye_track::startup::{open_project, ServiceState};
use mye_track::store::Paths;
use mye_track::watch::{Phase, Tracker};

const WAIT_LIMIT: Duration = Duration::from_secs(60);

/// 検証の走査を短い間隔で回す設定 (既定は 5 分おき)。
fn cfg() -> TrackerConfig {
    TrackerConfig {
        debounce: Duration::from_millis(150),
        max_hold: Duration::from_secs(1),
        busy_retry: Duration::from_millis(50),
        settle_wait: Duration::from_millis(100),
        settle_give_up: Duration::from_secs(10),
        verify_quiet: Duration::from_millis(250),
        verify_interval: Duration::from_millis(500),
        unrecorded_retry: Duration::from_millis(100),
        ..TrackerConfig::default()
    }
}

fn wait_until(what: &str, limit: Duration, mut cond: impl FnMut() -> bool) {
    let deadline = Instant::now() + limit;
    loop {
        if cond() {
            return;
        }
        assert!(Instant::now() < deadline, "待ちの上限を超えた: {what}");
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// テストごとの作業ツリー。テスト自身の書き込みを記録し、後処理でツリーと突き合わせる。
struct Fixture {
    root: PathBuf,
    model: BTreeMap<String, Vec<u8>>,
}

impl Fixture {
    fn new(name: &str) -> Fixture {
        let root = std::env::temp_dir().join(format!("mye_track_r_{}_{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        Fixture { root, model: BTreeMap::new() }
    }

    fn path(&self, rel: &str) -> PathBuf {
        self.root.join(rel)
    }

    fn write(&mut self, rel: &str, bytes: &[u8]) {
        let p = self.path(rel);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(&p, bytes).unwrap();
        self.model.insert(rel.to_string(), bytes.to_vec());
    }

    fn remove(&mut self, rel: &str) {
        fs::remove_file(self.path(rel)).unwrap();
        self.model.remove(rel);
    }

    fn open_store(&self) -> mye_track::store::Store {
        let (store, report) = open_project(&self.root, now_ms());
        assert_eq!(report.state, Some(ServiceState::Recording));
        store.unwrap()
    }

    fn start(&self, cfg: TrackerConfig) -> Tracker {
        let tracker = Tracker::start(&self.root, self.open_store(), cfg);
        let status = tracker.wait_ready(WAIT_LIMIT);
        assert_eq!(status.phase, Phase::Watching, "{status:?}");
        tracker
    }

    fn db(&self) -> Connection {
        let conn = Connection::open(Paths::for_project(&self.root).db).unwrap();
        conn.busy_timeout(Duration::from_secs(10)).unwrap();
        conn
    }

    fn revisions(&self, path: &str) -> Vec<Rev> {
        revisions(&self.db(), path)
    }

    fn blob(&self, hash: &str) -> Vec<u8> {
        fs::read(Paths::for_project(&self.root).blobs.join(&hash[..2]).join(&hash[2..])).unwrap()
    }

    fn count(&self, sql: &str) -> i64 {
        self.db().query_row(sql, [], |r| r.get(0)).unwrap()
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        if !std::thread::panicking() {
            let actual = tree(&self.root);
            let expected: BTreeMap<String, String> =
                self.model.iter().map(|(k, v)| (k.clone(), sha256_hex(v))).collect();
            assert_eq!(actual, expected, "作業ツリーがテストの書いた分と違う (追跡が書き込んだ?)");
        }
        let _ = fs::remove_dir_all(&self.root);
    }
}

/// `.mye` を除く作業ツリーの (相対パス → 内容ハッシュ)。
fn tree(root: &Path) -> BTreeMap<String, String> {
    fn walk(root: &Path, dir: &Path, out: &mut BTreeMap<String, String>) {
        for entry in fs::read_dir(dir).unwrap().flatten() {
            let path = entry.path();
            let rel = path.strip_prefix(root).unwrap().to_string_lossy().replace('\\', "/");
            if rel == ".mye" {
                continue;
            }
            if entry.file_type().unwrap().is_dir() {
                walk(root, &path, out);
            } else {
                out.insert(rel, sha256_hex(&fs::read(&path).unwrap()));
            }
        }
    }
    let mut out = BTreeMap::new();
    walk(root, root, &mut out);
    out
}

#[derive(Debug, Clone)]
struct Rev {
    change_type: String,
    completeness: String,
    before: Option<String>,
    after: Option<String>,
}

fn revisions(conn: &Connection, path: &str) -> Vec<Rev> {
    let mut stmt = conn
        .prepare(
            "SELECT r.change_type, r.completeness, sb.blob_hash, sa.blob_hash
               FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
               LEFT JOIN snapshots sb ON sb.snapshot_id = r.before_snapshot
               LEFT JOIN snapshots sa ON sa.snapshot_id = r.after_snapshot
              WHERE c.path = ?1 ORDER BY r.revision_id",
        )
        .unwrap();
    let rows = stmt
        .query_map([path], |r| {
            Ok(Rev { change_type: r.get(0)?, completeness: r.get(1)?, before: r.get(2)?, after: r.get(3)? })
        })
        .unwrap();
    rows.map(Result::unwrap).collect()
}

/// 受け入れ条件 4: 取りこぼし (イベントを捨てて模す) の作成・変更・削除が、検証の走査で `gap` として拾われる。
#[test]
fn verification_scan_catches_missed_create_modify_delete_as_gap() {
    let mut fx = Fixture::new("missed");
    fx.write("x.txt", b"x0");
    fx.write("z.txt", b"z0");
    fx.write("keep.txt", b"keep");
    let tracker = fx.start(cfg());
    // 更新時刻が基準の読み取りと同じ刻みに入らないようにする
    std::thread::sleep(Duration::from_millis(60));

    tracker.drop_incoming_events(true);
    fx.write("x.txt", b"x1");
    fx.write("y.txt", b"y0");
    fx.remove("z.txt");
    // 最後の操作 (z.txt の削除) のイベントが捨てられるまで待つ。OS はイベントを起きた順に届ける
    wait_until("イベントを捨て終わる", WAIT_LIMIT, || tracker.dropped_event_paths().iter().any(|p| p.ends_with("z.txt")));
    tracker.drop_incoming_events(false);

    wait_until("gap revisions", WAIT_LIMIT, || {
        ["x.txt", "y.txt", "z.txt"].iter().all(|p| fx.revisions(p).len() == 1)
    });
    for (path, change_type) in [("x.txt", "modified"), ("y.txt", "added"), ("z.txt", "deleted")] {
        let revs = fx.revisions(path);
        assert_eq!((revs[0].change_type.as_str(), revs[0].completeness.as_str()), (change_type, "gap"), "{path}");
    }
    assert_eq!(fx.blob(fx.revisions("x.txt")[0].before.as_ref().unwrap()), b"x0");
    assert_eq!(fx.blob(fx.revisions("x.txt")[0].after.as_ref().unwrap()), b"x1");
    assert!(fx.revisions("keep.txt").is_empty());
    assert!(fx.count("SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'") >= 1);
    assert!(tracker.status().verify_gaps >= 1);

    // ベースラインは更新されている: 以後の走査は同じ変更をもう一度記録しない
    let runs = tracker.status().verify_runs;
    wait_until("more scans", WAIT_LIMIT, || tracker.status().verify_runs >= runs + 2);
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions"), 3);
    assert_eq!(fx.count("SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'"), 1);
    tracker.stop("test");
}

/// 受け入れ条件 5 (前半): 何も変わっていないとき、検証の走査は Revision も `coverage_gaps` も作らない。
/// 通常の経路で記録した変更も、走査で二重にならない。
#[test]
fn verification_scan_with_no_changes_makes_nothing() {
    let mut fx = Fixture::new("quiet");
    for i in 0..50 {
        fx.write(&format!("d/f{i:02}.txt"), format!("v{i}").as_bytes());
    }
    let tracker = fx.start(cfg());
    let runs = tracker.status().verify_runs;
    wait_until("scans", WAIT_LIMIT, || tracker.status().verify_runs >= runs + 3);
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions"), 0);
    assert_eq!(fx.count("SELECT COUNT(*) FROM coverage_gaps"), 0);

    std::thread::sleep(Duration::from_millis(60));
    fx.write("d/f07.txt", b"edited");
    wait_until("watch revision", WAIT_LIMIT, || fx.revisions("d/f07.txt").len() == 1);
    let runs = tracker.status().verify_runs;
    wait_until("scans after edit", WAIT_LIMIT, || tracker.status().verify_runs >= runs + 3);
    let revs = fx.revisions("d/f07.txt");
    assert_eq!(revs.len(), 1);
    assert_eq!(revs[0].completeness, "complete");
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions"), 1);
    assert_eq!(fx.count("SELECT COUNT(*) FROM coverage_gaps"), 0);
    assert_eq!(tracker.status().verify_gaps, 0);
    tracker.stop("test");
}

/// 長い flush の間に溜まった処理待ちのイベントを、取りこぼしと取り違えない (検証の走査の前にキューを空にする)。
/// 取りこぼしが起きえない件数 (20 ファイル) で、処理待ちを人為的に作る決定的な試験: Writer を塞いで flush を止め、
/// その間に書いたファイルのイベントを 1 件 40 ms かけて処理させる。キューを空にする `drain_events` を外すと、
/// 検証の走査が処理待ちのファイルを `gap` として拾い、必ず落ちる。
#[test]
fn backlog_of_unprocessed_events_is_not_mistaken_for_a_miss() {
    let mut fx = Fixture::new("backlog");
    let tracker = fx.start(TrackerConfig {
        verify_quiet: Duration::from_millis(100),
        verify_interval: Duration::from_millis(100),
        ..cfg()
    });
    tracker.delay_event_handling(Duration::from_millis(40));
    // Writer を 1.5 秒塞ぐ。A の flush はそこで止まり、監視スレッドは待つ
    let blocker = tracker.writer().submit(|_| std::thread::sleep(Duration::from_millis(1500))).unwrap();
    fx.write("a.txt", b"a");
    std::thread::sleep(Duration::from_millis(500));
    const N: usize = 20;
    for i in 0..N {
        // ディレクトリを作ると、その作成の flush が中のファイルをまとめて読んでしまう。ルートの直下に書く
        fx.write(&format!("f{i:02}.txt"), format!("v1 {i}").as_bytes());
    }
    blocker.recv().unwrap();
    wait_until("all recorded", Duration::from_secs(60), || {
        fx.count("SELECT COUNT(DISTINCT change_id) FROM change_revisions") >= (N + 1) as i64
    });
    let runs = tracker.status().verify_runs;
    wait_until("scans", WAIT_LIMIT, || tracker.status().verify_runs >= runs + 2);
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions"), (N + 1) as i64, "二重または欠け");
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions WHERE completeness = 'gap'"), 0, "処理待ちを取りこぼしと取り違えた");
    assert_eq!(fx.count("SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'"), 0);
    tracker.stop("test");
}

/// 大量 (2500 ファイル) の一括書き込み。notify の読み取りバッファ (16 KiB) は、読む側が CPU を取れないとあふれて
/// イベントを黙って失う (spec D34)。取りこぼしが起きても起きなくても通る: 全ファイルが「通常の Revision」か
/// 「`gap` の Revision」のどちらか 1 つとして記録され、`gap` があれば `watcher_overflow` の区間がある。
#[test]
fn bulk_writes_are_each_recorded_once_as_a_normal_or_a_gap_revision() {
    let mut fx = Fixture::new("bulk");
    let tracker = fx.start(TrackerConfig {
        verify_quiet: Duration::from_millis(100),
        verify_interval: Duration::from_millis(100),
        ..cfg()
    });
    const N: usize = 2500;
    for i in 0..N {
        fx.write(&format!("bulk/f{i:04}.txt"), format!("v1 {i}").as_bytes());
    }
    wait_until("all recorded", Duration::from_secs(120), || {
        fx.count("SELECT COUNT(DISTINCT change_id) FROM change_revisions") >= N as i64
    });
    let runs = tracker.status().verify_runs;
    wait_until("scans", WAIT_LIMIT, || tracker.status().verify_runs >= runs + 2);
    // 記録なし 0、二重 0: 全ファイルがちょうど 1 つの Revision を持つ
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions"), N as i64);
    assert_eq!(fx.count("SELECT COUNT(DISTINCT change_id) FROM change_revisions"), N as i64);
    let gaps = fx.count("SELECT COUNT(*) FROM change_revisions WHERE completeness = 'gap'");
    let intervals = fx.count("SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'");
    if gaps > 0 {
        assert!(intervals > 0, "gap の Revision が {gaps} 件あるのに watcher_overflow の区間が無い");
    } else {
        assert_eq!(intervals, 0, "取りこぼしが無いのに区間がある");
    }
    // 最後の Revision の After が実際の内容と一致する
    for i in [0, N / 2, N - 1] {
        let path = format!("bulk/f{i:04}.txt");
        let revs = fx.revisions(&path);
        assert_eq!(revs.len(), 1, "{path}");
        assert_eq!(revs[0].after.as_deref(), Some(sha256_hex(format!("v1 {i}").as_bytes()).as_str()), "{path}");
    }
    tracker.stop("test");
}

/// 受け入れ条件 5 (後半) と 6: 検証の走査と重ねて書き続けても、書き手は失敗せず、
/// 最後の Revision の After が実際の内容と一致する。
#[test]
fn writers_overlapping_verification_scans_never_fail_and_end_current() {
    let mut fx = Fixture::new("overlap");
    const BACKGROUND: usize = 400;
    for i in 0..BACKGROUND {
        fx.write(&format!("bg/f{i:03}.txt"), format!("bg {i}").as_bytes());
    }
    fx.write("w.txt", b"initial");
    let tracker = fx.start(TrackerConfig {
        verify_quiet: Duration::from_millis(40),
        verify_interval: Duration::from_millis(30),
        ..cfg()
    });

    let target = fx.path("w.txt");
    let failures = Arc::new(AtomicUsize::new(0));
    let first_failure = Arc::new(std::sync::Mutex::new(String::new()));
    let writer = {
        let (target, failures, first_failure) = (target.clone(), Arc::clone(&failures), Arc::clone(&first_failure));
        std::thread::spawn(move || {
            let tmp = target.with_extension("txt.tmp");
            let record = |what: &str, r: std::io::Result<()>| {
                if let Err(e) = r {
                    if failures.fetch_add(1, Ordering::SeqCst) == 0 {
                        *first_failure.lock().unwrap() = format!("{what}: {e} (os error {:?})", e.raw_os_error());
                    }
                }
            };
            // 短い連続書き込みと、走査が始まる静かな時間を交互に置く
            for burst in 0..30 {
                for i in 0..20 {
                    let n = burst * 20 + i;
                    match n % 3 {
                        0 => {
                            record("write tmp", fs::write(&tmp, format!("tmp {n}")));
                            record("rename", fs::rename(&tmp, &target));
                        }
                        1 => record("fs::write", fs::write(&target, format!("direct {n}"))),
                        _ => {
                            record("remove", fs::remove_file(&target));
                            record("recreate", fs::write(&target, format!("recreated {n}")));
                        }
                    }
                    std::thread::sleep(Duration::from_millis(1));
                }
                std::thread::sleep(Duration::from_millis(120));
            }
        })
    };
    writer.join().unwrap();
    assert_eq!(failures.load(Ordering::SeqCst), 0, "書き手が失敗した: {}", first_failure.lock().unwrap());
    let final_text = fs::read(&target).unwrap();
    let _ = fs::remove_file(fx.path("w.txt.tmp"));
    fx.model.insert("w.txt".to_string(), final_text.clone());

    // 追跡が追いつくまで待つ: 最後の Revision の After が実際の内容になる
    let last_after = |fx: &Fixture| fx.revisions("w.txt").last().and_then(|r| r.after.clone());
    wait_until("w.txt current", WAIT_LIMIT, || {
        last_after(&fx).is_some_and(|h| h == sha256_hex(&final_text))
    });
    let runs = tracker.status().verify_runs;
    wait_until("scans", WAIT_LIMIT, || tracker.status().verify_runs >= runs + 3);
    let revs = fx.revisions("w.txt");
    assert_eq!(fx.blob(revs.last().unwrap().after.as_ref().unwrap()), final_text);
    // 背景のファイルは一度も変わっていない
    assert_eq!(fx.count("SELECT COUNT(*) FROM changes WHERE path LIKE 'bg/%'"), 0);
    tracker.stop("test");
}

/// 受け入れ条件 7: ジャーナルへ書けない間の観測は、復帰後に `coverage_gaps` の区間として残り、
/// 検証の走査で実際の状態が `gap` として拾われる。理由は失敗の種類 (`disk_full` / `write_failed`) で分かれる。
#[test]
fn journal_failure_becomes_a_gap_and_the_real_state_is_reconciled() {
    let mut fx = Fixture::new("journal");
    fx.write("b.txt", b"b0");
    fx.write("c.txt", b"c0");
    let tracker = fx.start(cfg());
    std::thread::sleep(Duration::from_millis(60));

    for (code, reason, tag) in [(112, "disk_full", "1"), (5, "write_failed", "2")] {
        tracker.writer().call(move |store| store.inject_journal_error = Some(code)).unwrap();
        let before_failure = now_ms();
        fx.write(&format!("a{tag}.txt"), format!("a{tag}").as_bytes());
        fx.write("b.txt", format!("b{tag}").as_bytes());
        wait_until("unrecorded", WAIT_LIMIT, || tracker.status().unrecorded_reason == Some(reason));
        let status = tracker.status();
        assert!(status.unrecorded_since_ms.unwrap() >= before_failure - 1000);
        // 書けていない間は Revision が増えない
        assert!(fx.revisions(&format!("a{tag}.txt")).is_empty());

        tracker.writer().call(|store| store.inject_journal_error = None).unwrap();
        wait_until("gap recorded", WAIT_LIMIT, || {
            tracker.status().unrecorded_since_ms.is_none()
                && fx.count(&format!("SELECT COUNT(*) FROM coverage_gaps WHERE reason = '{reason}'")) == 1
        });
        wait_until("reconciled", WAIT_LIMIT, || {
            let revs = fx.revisions(&format!("a{tag}.txt"));
            revs.len() == 1 && fx.revisions("b.txt").len() == tag.parse::<usize>().unwrap()
        });
        let revs = fx.revisions(&format!("a{tag}.txt"));
        assert_eq!((revs[0].change_type.as_str(), revs[0].completeness.as_str()), ("added", "gap"));
        assert_eq!(fx.blob(revs[0].after.as_ref().unwrap()), format!("a{tag}").as_bytes());
        let (from, to): (i64, i64) = fx
            .db()
            .query_row(
                &format!("SELECT from_at, to_at FROM coverage_gaps WHERE reason = '{reason}'"),
                [],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )
            .unwrap();
        assert!(from <= to);
    }
    // 復帰後は通常の経路で記録できる
    std::thread::sleep(Duration::from_millis(60));
    fx.write("c.txt", b"c1");
    wait_until("normal path", WAIT_LIMIT, || fx.revisions("c.txt").len() == 1);
    assert_eq!(fx.revisions("c.txt")[0].completeness, "complete");
    tracker.stop("test");
}

/// 1500 本 + 先頭に並ぶ `a_first.txt`。ベースラインは 500 件ずつの塊 3 つになる。
fn baseline_fixture(name: &str) -> Fixture {
    let mut fx = Fixture::new(name);
    fx.write("a_first.txt", b"first v0");
    for i in 0..1500 {
        fx.write(&format!("d/f{i:04}.txt"), format!("file {i}").as_bytes());
    }
    fx
}

fn slow_baseline() -> TrackerConfig {
    TrackerConfig { baseline_chunk_pause: Duration::from_millis(300), ..cfg() }
}

/// 受け入れ条件 9 (前半): ベースラインの途中で止めて起動し直すと、最終的に全ファイルのベースラインがそろう。
#[test]
fn interrupted_baseline_restarts_and_covers_every_file() {
    let fx = baseline_fixture("interrupted");
    let tracker = Tracker::start(&fx.root, fx.open_store(), slow_baseline());
    wait_until("first chunk", WAIT_LIMIT, || tracker.status().scan_done >= 500);
    assert_eq!(tracker.status().phase, Phase::Baselining);
    let status = tracker.status();
    assert!(status.scan_total >= 1501 && status.scan_done < status.scan_total, "{status:?}");
    tracker.stop("test");
    assert_eq!(fx.count("SELECT COUNT(*) FROM events WHERE kind = 'baseline_complete'"), 0);

    let tracker = fx.start(cfg());
    assert_eq!(fx.count("SELECT COUNT(*) FROM events WHERE kind = 'baseline_complete'"), 1);
    assert_eq!(fx.count("SELECT COUNT(DISTINCT path) FROM snapshots WHERE kind = 'baseline'"), 1501);
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions"), 0);
    tracker.stop("test");
}

/// 受け入れ条件 9 (後半): ベースラインの最中に変わったファイルは、ベースラインの内容を Before に持つ。
#[test]
fn file_changed_during_baseline_has_the_baseline_content_as_before() {
    let mut fx = baseline_fixture("changed_during");
    let tracker = Tracker::start(&fx.root, fx.open_store(), slow_baseline());
    wait_until("first chunk", WAIT_LIMIT, || tracker.status().scan_done >= 500);
    assert_eq!(tracker.status().phase, Phase::Baselining);
    // 先頭のファイルは最初の塊で読み終えている
    std::thread::sleep(Duration::from_millis(60));
    fx.write("a_first.txt", b"first v1 changed during the baseline");
    let status = tracker.wait_ready(WAIT_LIMIT);
    assert_eq!(status.phase, Phase::Watching, "{status:?}");

    wait_until("revision", WAIT_LIMIT, || fx.revisions("a_first.txt").len() == 1);
    let revs = fx.revisions("a_first.txt");
    assert_eq!(revs[0].change_type, "modified");
    assert_eq!(fx.blob(revs[0].before.as_ref().unwrap()), b"first v0");
    assert_eq!(fx.blob(revs[0].after.as_ref().unwrap()), b"first v1 changed during the baseline");
    tracker.stop("test");
}

/// 受け入れ条件 8: 直近に書いた Blob の中身が名前のハッシュと違えば、起動時に見つけて消し、`blob_state` を `missing` にする。
/// 前回の終わりから 10 分より前の Blob は確かめない (限界)。
#[test]
fn startup_finds_corrupt_recent_blob_and_marks_it_missing() {
    let fx = Fixture::new("corrupt_blob");
    let (recent_hash, old_hash);
    {
        let mut store = fx.open_store();
        store.record_snapshot("recent.txt", "disk", now_ms(), b"recent payload").unwrap();
        store.record_snapshot("old.txt", "disk", now_ms(), b"old payload").unwrap();
        store.record_event(now_ms(), "service_stop", serde_json::json!({ "reason": "test" })).unwrap();
        recent_hash = sha256_hex(b"recent payload");
        old_hash = sha256_hex(b"old payload");
    }
    let blobs = Paths::for_project(&fx.root).blobs;
    let recent = blobs.join(&recent_hash[..2]).join(&recent_hash[2..]);
    let old = blobs.join(&old_hash[..2]).join(&old_hash[2..]);
    // 電源断で最終名のまま中身が欠けた状態: 大きさは同じで中身が違う
    fs::write(&recent, b"RECENT PAYLOAD").unwrap();
    fs::write(&old, b"OLD PAYLOAD").unwrap();
    let long_ago = std::time::SystemTime::now() - Duration::from_secs(3600);
    fs::File::options().write(true).open(&old).unwrap().set_modified(long_ago).unwrap();

    let (store, report) = open_project(&fx.root, now_ms());
    assert!(store.is_some());
    assert_eq!(report.blobs_corrupted, 1, "{report:?}");
    assert_eq!(report.blobs_marked_missing, 1);
    assert!(!recent.exists());
    assert!(old.exists());
    let state = |path: &str| -> String {
        fx.db()
            .query_row("SELECT blob_state FROM snapshots WHERE path = ?1", [path], |r| r.get(0))
            .unwrap()
    };
    assert_eq!(state("recent.txt"), "missing");
    assert_eq!(state("old.txt"), "present");

    // 消したので、同じ内容は書き直される
    let mut store = store.unwrap();
    store.record_snapshot("recent.txt", "disk", now_ms(), b"recent payload").unwrap();
    assert_eq!(fs::read(&recent).unwrap(), b"recent payload");
}

/// 呼び出し側のハッシュを渡す `put_hashed`: 合っていれば保存でき、内容と違えば読み直しの検証が落ちて何も残らない。
#[test]
fn put_hashed_trusts_the_caller_hash_only_after_verification() {
    let fx = Fixture::new("put_hashed");
    let mut store = fx.open_store();
    let good = sha256_hex(b"payload");
    store.record_snapshot_hashed("a.txt", "disk", now_ms(), b"payload", &good).unwrap();
    assert_eq!(fx.blob(&good), b"payload");

    let wrong = sha256_hex(b"something else");
    let err = store.record_snapshot_hashed("b.txt", "disk", now_ms(), b"payload2", &wrong).unwrap_err();
    assert!(matches!(err, mye_track::store::StoreError::Blob(_)), "{err}");
    assert_eq!(fx.count("SELECT COUNT(*) FROM snapshots"), 1);
    assert!(!Paths::for_project(&fx.root).blobs.join(&wrong[..2]).join(&wrong[2..]).exists());
}

/// 受け入れ条件 3: 本物の取りこぼしが起きうる量 (16 スレッド × 4000 ファイル) を書いた後、検証の走査が終わった時点で、
/// 全ファイルが記録されている。時間がかかるので `#[ignore]`。
/// `cargo test --release --test reconcile -- --ignored --nocapture`
#[test]
#[ignore]
fn real_overflow_is_fully_covered_after_verification() {
    let mut fx = Fixture::new("overflow64k");
    let tracker = fx.start(TrackerConfig {
        verify_quiet: Duration::from_secs(2),
        verify_interval: Duration::from_secs(20),
        ..TrackerConfig::default()
    });
    const THREADS: usize = 16;
    const PER_THREAD: usize = 4000;
    let started = Instant::now();
    let handles: Vec<_> = (0..THREADS)
        .map(|t| {
            let root = fx.root.clone();
            std::thread::spawn(move || {
                let dir = root.join(format!("t{t:02}"));
                fs::create_dir_all(&dir).unwrap();
                let mut written = Vec::with_capacity(PER_THREAD);
                for i in 0..PER_THREAD {
                    let rel = format!("t{t:02}/long_file_name_for_overflow_probe_{t:02}_{i:05}_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.txt");
                    let bytes = format!("thread {t} file {i}").into_bytes();
                    fs::write(root.join(&rel), &bytes).unwrap();
                    written.push((rel, bytes));
                }
                written
            })
        })
        .collect();
    for h in handles {
        for (rel, bytes) in h.join().unwrap() {
            fx.model.insert(rel, bytes);
        }
    }
    eprintln!("書き込み {} ファイル: {:.1} 秒", fx.model.len(), started.elapsed().as_secs_f64());

    let total = (THREADS * PER_THREAD) as i64;
    let recorded = |fx: &Fixture| fx.count("SELECT COUNT(DISTINCT change_id) FROM change_revisions");
    // 検証の走査 (2 回以上、静かな時間の後) が終わるまで待つ
    let runs_at_end = tracker.status().verify_runs;
    wait_until("all recorded", Duration::from_secs(90 * 60), || {
        recorded(&fx) >= total && tracker.status().verify_runs >= runs_at_end + 2
    });
    let gap_revisions = fx.count("SELECT COUNT(*) FROM change_revisions WHERE completeness = 'gap'");
    let gaps = fx.count("SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'");
    eprintln!(
        "記録 {} / {total}、gap の Revision {gap_revisions}、watcher_overflow の区間 {gaps}、検証の走査 {} 回 (うち見つけた {} 回)、経過 {:.1} 秒",
        recorded(&fx),
        tracker.status().verify_runs,
        tracker.status().verify_gaps,
        started.elapsed().as_secs_f64()
    );
    // 記録の無いファイルが 0 本。通常か gap の Revision の After が実際の内容と一致する
    assert_eq!(fx.count("SELECT COUNT(*) FROM changes c WHERE NOT EXISTS (SELECT 1 FROM change_revisions r WHERE r.change_id = c.change_id)"), 0);
    assert_eq!(recorded(&fx), total);
    let db = fx.db();
    for (rel, bytes) in fx.model.iter() {
        let revs = revisions(&db, rel);
        assert!(!revs.is_empty(), "記録が無い: {rel}");
        let last = revs.last().unwrap();
        assert!(matches!(last.completeness.as_str(), "complete" | "coalesced" | "gap"), "{rel}: {last:?}");
        assert_eq!(fx.blob(last.after.as_ref().unwrap()), *bytes, "{rel}");
    }
    // 取りこぼしが実際に起きた回は、区間が残っている
    if gap_revisions > 0 {
        assert!(gaps >= 1);
    }
    tracker.stop("test");
}
