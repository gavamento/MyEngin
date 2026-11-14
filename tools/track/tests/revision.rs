//! Revision・Hunk・リネームのまとめ・派生の表の作り直しの結合テスト (M104e)。
//!
//! 待ちは固定の sleep ではなく上限付きの待ち合わせ (`wait_until`)。全テストの後処理で、
//! テスト自身が書いた分を除いて作業ツリー (`.mye` を除く) が変わっていないことを検査する (spec §2 D17)。

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use rusqlite::types::ValueRef;
use rusqlite::Connection;

use mye_track::collector::TrackerConfig;
use mye_track::db::now_ms;
use mye_track::diff::{apply_hunks, DiffLimits, Hunk};
use mye_track::hash::sha256_hex;
use mye_track::startup::{open_project, ServiceState};
use mye_track::store::Paths;
use mye_track::watch::{Phase, Tracker};

const WAIT_LIMIT: Duration = Duration::from_secs(60);

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

fn wait_until(what: &str, mut cond: impl FnMut() -> bool) {
    let deadline = Instant::now() + WAIT_LIMIT;
    loop {
        if cond() {
            return;
        }
        assert!(Instant::now() < deadline, "待ちの上限を超えた: {what}");
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// テストごとの作業ツリー。成功したテストは終わりに消す (失敗したときは調べられるよう残す)。
struct Fixture {
    root: PathBuf,
    model: BTreeMap<String, Vec<u8>>,
}

impl Fixture {
    fn new(name: &str) -> Fixture {
        let root = std::env::temp_dir().join(format!("mye_track_v_{}_{}", name, std::process::id()));
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

    fn rename(&mut self, from: &str, to: &str) {
        fs::create_dir_all(self.path(to).parent().unwrap()).unwrap();
        fs::rename(self.path(from), self.path(to)).unwrap();
        let bytes = self.model.remove(from).unwrap();
        self.model.insert(to.to_string(), bytes);
    }

    fn rename_dir(&mut self, from: &str, to: &str) {
        fs::rename(self.path(from), self.path(to)).unwrap();
        let prefix = format!("{from}/");
        let moved: Vec<String> = self.model.keys().filter(|k| k.starts_with(&prefix)).cloned().collect();
        for key in moved {
            let bytes = self.model.remove(&key).unwrap();
            self.model.insert(format!("{to}/{}", &key[prefix.len()..]), bytes);
        }
    }

    fn start(&self, cfg: TrackerConfig) -> Tracker {
        let (store, report) = open_project(&self.root, now_ms());
        assert_eq!(report.state, Some(ServiceState::Recording));
        let tracker = Tracker::start(&self.root, store.unwrap(), cfg);
        let status = tracker.wait_ready(WAIT_LIMIT);
        assert_eq!(status.phase, Phase::Watching, "{status:?}");
        tracker
    }

    fn db(&self) -> Connection {
        let conn = Connection::open(Paths::for_project(&self.root).db).unwrap();
        conn.busy_timeout(Duration::from_secs(10)).unwrap();
        conn
    }

    fn count(&self, sql: &str) -> i64 {
        self.db().query_row(sql, [], |r| r.get(0)).unwrap()
    }

    fn revision_count(&self, path: &str) -> i64 {
        self.db()
            .query_row(
                "SELECT COUNT(*) FROM change_revisions r JOIN changes c ON c.change_id = r.change_id WHERE c.path = ?1",
                [path],
                |r| r.get(0),
            )
            .unwrap()
    }

    /// 番兵を書き、その Revision が出るまで待つ。それより前に書いたパスの処理は済んでいる。
    fn settle(&mut self, name: &str) {
        let rel = format!("zz_sentinel_{name}.txt");
        self.write(&rel, name.as_bytes());
        wait_until(&format!("番兵 {name}"), || self.revision_count(&rel) == 1);
    }

    fn blob(&self, hash: &str) -> Vec<u8> {
        fs::read(Paths::for_project(&self.root).blobs.join(&hash[..2]).join(&hash[2..])).unwrap()
    }

    /// パスの最後の Revision の (Before の内容, After の内容, Hunk)。
    fn last_revision(&self, path: &str) -> RevisionView {
        revision_views(&self.db(), path).pop().expect("Revision が無い")
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        if std::thread::panicking() {
            return;
        }
        let actual = tree(&self.root);
        let expected: BTreeMap<String, String> = self.model.iter().map(|(k, v)| (k.clone(), sha256_hex(v))).collect();
        assert_eq!(actual, expected, "作業ツリーがテストの書いた分と違う (追跡が書き込んだ?)");
        let _ = fs::remove_dir_all(&self.root);
    }
}

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

#[derive(Debug)]
struct RevisionView {
    change_id: i64,
    revision_id: i64,
    seq: i64,
    change_type: String,
    old_path: Option<String>,
    diff_mode: String,
    completeness: String,
    before_hash: Option<String>,
    after_hash: Option<String>,
    hunks: Vec<(i64, Hunk, i64)>,
}

fn revision_views(conn: &Connection, path: &str) -> Vec<RevisionView> {
    let mut stmt = conn
        .prepare(
            "SELECT r.change_id, r.revision_id, r.seq, r.change_type, r.old_path, r.diff_mode, r.completeness, sb.blob_hash, sa.blob_hash
               FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
               LEFT JOIN snapshots sb ON sb.snapshot_id = r.before_snapshot
               LEFT JOIN snapshots sa ON sa.snapshot_id = r.after_snapshot
              WHERE c.path = ?1 ORDER BY r.revision_id",
        )
        .unwrap();
    let mut views: Vec<RevisionView> = stmt
        .query_map([path], |r| {
            Ok(RevisionView {
                change_id: r.get(0)?,
                revision_id: r.get(1)?,
                seq: r.get(2)?,
                change_type: r.get(3)?,
                old_path: r.get(4)?,
                diff_mode: r.get(5)?,
                completeness: r.get(6)?,
                before_hash: r.get(7)?,
                after_hash: r.get(8)?,
                hunks: Vec::new(),
            })
        })
        .unwrap()
        .map(Result::unwrap)
        .collect();
    for view in &mut views {
        let mut stmt = conn
            .prepare(
                "SELECT hunk_id, ordinal, old_start, old_count, new_start, new_count, before_text, after_text,
                        context_pre, context_post, eol_only
                   FROM change_hunks WHERE revision_id = ?1 ORDER BY ordinal",
            )
            .unwrap();
        view.hunks = stmt
            .query_map([view.revision_id], |r| {
                Ok((
                    r.get::<_, i64>(0)?,
                    Hunk {
                        old_start: r.get::<_, i64>(2)? as usize,
                        old_count: r.get::<_, i64>(3)? as usize,
                        new_start: r.get::<_, i64>(4)? as usize,
                        new_count: r.get::<_, i64>(5)? as usize,
                        before_text: r.get(6)?,
                        after_text: r.get(7)?,
                        context_pre: r.get(8)?,
                        context_post: r.get(9)?,
                        eol_only: r.get::<_, i64>(10)? != 0,
                    },
                    r.get::<_, i64>(1)?,
                ))
            })
            .unwrap()
            .map(Result::unwrap)
            .collect();
    }
    views
}

fn lines(n: usize) -> String {
    (1..=n).map(|i| format!("line{i:02}\n")).collect()
}

#[test]
fn three_distant_edits_make_three_hunks_that_rebuild_the_after_content() {
    let mut fx = Fixture::new("three");
    let before = lines(40);
    fx.write("code.txt", before.as_bytes());
    let tracker = fx.start(cfg());

    let after = before.replace("line05\n", "FIVE\n").replace("line20\n", "TWENTY\n").replace("line35\n", "日本語🦀\n");
    fx.write("code.txt", after.as_bytes());
    wait_until("modified", || fx.revision_count("code.txt") == 1);

    let rev = fx.last_revision("code.txt");
    assert_eq!(rev.diff_mode, "lines");
    assert_eq!(rev.hunks.len(), 3);
    let starts: Vec<usize> = rev.hunks.iter().map(|(_, h, _)| h.old_start).collect();
    assert_eq!(starts, vec![5, 20, 35]);
    let hunks: Vec<Hunk> = rev.hunks.iter().map(|(_, h, _)| h.clone()).collect();
    let before_blob = String::from_utf8(fx.blob(rev.before_hash.as_ref().unwrap())).unwrap();
    let after_blob = String::from_utf8(fx.blob(rev.after_hash.as_ref().unwrap())).unwrap();
    assert_eq!(before_blob, before);
    assert_eq!(apply_hunks(&before_blob, &hunks).as_deref(), Ok(after_blob.as_str()));
    assert_eq!(after_blob, after);
    tracker.stop("test");
}

#[test]
fn same_path_twice_is_one_change_two_revisions_with_distinct_hunk_ids() {
    let mut fx = Fixture::new("twice");
    fx.write("a.txt", b"one\ntwo\nthree\n");
    let tracker = fx.start(cfg());

    fx.write("a.txt", b"one\nTWO\nthree\n");
    wait_until("first", || fx.revision_count("a.txt") == 1);
    fx.write("a.txt", b"one\nTWO\nTHREE\n");
    wait_until("second", || fx.revision_count("a.txt") == 2);

    let views = revision_views(&fx.db(), "a.txt");
    assert_eq!(views[0].change_id, views[1].change_id, "同じパスの変更は 1 つの Change");
    assert_eq!((views[0].seq, views[1].seq), (1, 2));
    assert_ne!(views[0].revision_id, views[1].revision_id);
    let ids: Vec<i64> = views.iter().flat_map(|v| v.hunks.iter().map(|(id, _, _)| *id)).collect();
    assert_eq!(ids.len(), 2);
    assert_ne!(ids[0], ids[1], "Revision が変われば Hunk ID も変わる");
    assert_eq!(fx.count("SELECT COUNT(*) FROM changes WHERE path = 'a.txt'"), 1);
    assert_eq!(fx.count("SELECT COUNT(*) FROM changes WHERE path = 'a.txt' AND state = 'active'"), 1);
    tracker.stop("test");
}

#[test]
fn recreating_a_deleted_path_supersedes_the_old_change() {
    let mut fx = Fixture::new("recreate");
    fx.write("gone.txt", b"first life\n");
    let tracker = fx.start(cfg());

    fx.remove("gone.txt");
    wait_until("deleted", || fx.revision_count("gone.txt") == 1);
    fx.write("gone.txt", b"second life\n");
    wait_until("recreated", || fx.revision_count("gone.txt") == 2);

    let views = revision_views(&fx.db(), "gone.txt");
    assert_eq!((views[0].change_type.as_str(), views[1].change_type.as_str()), ("deleted", "added"));
    assert_ne!(views[0].change_id, views[1].change_id);
    let state = |id: i64| -> String { fx.db().query_row("SELECT state FROM changes WHERE change_id = ?1", [id], |r| r.get(0)).unwrap() };
    assert_eq!(state(views[0].change_id), "superseded");
    assert_eq!(state(views[1].change_id), "active");
    // 削除は全行が消える 1 Hunk、作成は全行が加わる 1 Hunk
    assert_eq!(views[0].hunks.len(), 1);
    assert_eq!((views[0].hunks[0].1.old_count, views[0].hunks[0].1.new_count), (1, 0));
    assert_eq!((views[1].hunks[0].1.old_count, views[1].hunks[0].1.new_count), (0, 1));
    tracker.stop("test");
}

#[test]
fn crlf_to_lf_rewrite_is_a_single_eol_only_hunk() {
    let mut fx = Fixture::new("eol");
    fx.write("win.txt", b"a\r\nb\r\nc\r\n");
    let tracker = fx.start(cfg());

    fx.write("win.txt", b"a\nb\nc\n");
    wait_until("modified", || fx.revision_count("win.txt") == 1);
    let rev = fx.last_revision("win.txt");
    assert_eq!(rev.hunks.len(), 1);
    assert!(rev.hunks[0].1.eol_only);
    assert_eq!(rev.hunks[0].1.before_text, "a\r\nb\r\nc\r\n", "改行を正規化しない");
    assert_eq!(rev.hunks[0].1.after_text, "a\nb\nc\n");
    tracker.stop("test");
}

#[test]
fn input_over_the_limit_is_one_whole_hunk_marked_on_the_revision() {
    let mut fx = Fixture::new("limit");
    let old: String = (0..300).map(|i| format!("old {i}\n")).collect();
    let new: String = (0..300).map(|i| format!("new {i}\n")).collect();
    fx.write("big.txt", old.as_bytes());
    let tracker = fx.start(TrackerConfig {
        diff_limits: DiffLimits { max_edit_distance: 50, ..DiffLimits::default() },
        ..cfg()
    });

    let started = Instant::now();
    fx.write("big.txt", new.as_bytes());
    wait_until("modified", || fx.revision_count("big.txt") == 1);
    assert!(started.elapsed() < Duration::from_secs(30));
    let rev = fx.last_revision("big.txt");
    assert_eq!(rev.diff_mode, "whole");
    assert_eq!(rev.hunks.len(), 1);
    assert_eq!((rev.hunks[0].1.old_count, rev.hunks[0].1.new_count), (300, 300));
    let hunks: Vec<Hunk> = rev.hunks.iter().map(|(_, h, _)| h.clone()).collect();
    assert_eq!(apply_hunks(&old, &hunks).as_deref(), Ok(new.as_str()));
    tracker.stop("test");
}

#[test]
fn binary_excluded_and_unseen_before_have_no_hunks_and_say_why() {
    let mut fx = Fixture::new("nohunks");
    fx.write("keep.txt", b"keep\n");
    let tracker = fx.start(cfg());

    fx.write("img.bin", &[0xff, 0xfe, 0x00, 0x01]);
    fx.write("secret.pem", b"-----BEGIN-----\n");
    fx.write("plain.txt", b"plain\n");
    wait_until("plain", || fx.revision_count("plain.txt") == 1);
    wait_until("binary", || fx.revision_count("img.bin") == 1);
    wait_until("excluded", || fx.revision_count("secret.pem") == 1);
    let bin = fx.last_revision("img.bin");
    assert_eq!((bin.diff_mode.as_str(), bin.hunks.len()), ("binary", 0));
    let secret = fx.last_revision("secret.pem");
    assert_eq!((secret.diff_mode.as_str(), secret.hunks.len()), ("excluded", 0));
    let plain = fx.last_revision("plain.txt");
    assert_eq!((plain.diff_mode.as_str(), plain.hunks.len()), ("lines", 1));
    tracker.stop("test");
}

#[test]
fn same_directory_rename_supersedes_the_old_path_change_and_has_no_hunks() {
    let mut fx = Fixture::new("samedir");
    fx.write("a.txt", b"one\n");
    let tracker = fx.start(cfg());

    fx.write("a.txt", b"one\ntwo\n");
    wait_until("modified", || fx.revision_count("a.txt") == 1);
    fx.rename("a.txt", "b.txt");
    wait_until("renamed", || fx.revision_count("b.txt") == 1);

    let renamed = fx.last_revision("b.txt");
    assert_eq!((renamed.change_type.as_str(), renamed.old_path.as_deref()), ("renamed", Some("a.txt")));
    assert_eq!(renamed.hunks.len(), 0, "内容が同じリネームは Hunk が無い");
    assert_eq!(
        fx.count("SELECT COUNT(*) FROM changes WHERE path = 'a.txt' AND state = 'superseded'"),
        1,
        "元のパスの Change は superseded"
    );
    tracker.stop("test");
}

#[test]
fn moves_link_only_when_the_content_is_identical_and_the_candidate_is_unique() {
    let mut fx = Fixture::new("links");
    fx.write("src/same.txt", b"identical\n");
    fx.write("src/edited.txt", b"before edit\n");
    fx.write("dup1/x.txt", b"twin\n");
    fx.write("dup2/y.txt", b"twin\n");
    fx.write("dst/keep.txt", b"keep\n");
    let tracker = fx.start(cfg());

    // 内容が同じ 1 件: renamed
    fx.rename("src/same.txt", "dst/same.txt");
    wait_until("same", || fx.revision_count("dst/same.txt") == 1);
    assert_eq!(fx.last_revision("dst/same.txt").change_type, "renamed");

    // 1 バイト違う: 別々の削除と作成
    fx.remove("src/edited.txt");
    fx.write("dst/edited.txt", b"before edit!\n");
    wait_until("edited", || fx.revision_count("dst/edited.txt") == 1 && fx.revision_count("src/edited.txt") == 1);
    assert_eq!(fx.last_revision("dst/edited.txt").change_type, "added");
    assert_eq!(fx.last_revision("src/edited.txt").change_type, "deleted");

    // 同じ内容の削除が 2 件あり、作成は 1 件: つながない
    fx.remove("dup1/x.txt");
    fx.remove("dup2/y.txt");
    fx.write("dst/twin.txt", b"twin\n");
    wait_until("twins", || {
        fx.revision_count("dup1/x.txt") == 1 && fx.revision_count("dup2/y.txt") == 1 && fx.revision_count("dst/twin.txt") == 1
    });
    assert_eq!(fx.last_revision("dst/twin.txt").change_type, "added");
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions WHERE change_type = 'renamed'"), 1);
    tracker.stop("test");
}

#[test]
fn directory_rename_links_files_and_uses_paths_only_for_duplicate_content() {
    let mut fx = Fixture::new("dirrename");
    fx.write("old/a.txt", b"alpha\n");
    fx.write("old/twin1.txt", b"twin\n");
    fx.write("old/sub/twin1.txt", b"twin\n");
    fx.write("keep.txt", b"keep\n");
    let tracker = fx.start(cfg());

    fx.rename_dir("old", "new");
    for rel in ["new/a.txt", "new/twin1.txt", "new/sub/twin1.txt"] {
        wait_until(rel, || fx.revision_count(rel) == 1);
    }
    fx.settle("dir");
    assert_eq!(fx.last_revision("new/a.txt").change_type, "renamed");
    // 同じ内容が 3 本のうち 2 本あるが、末尾のパスが 1 対 1 で対応するので、パスで決まる
    let twin = fx.last_revision("new/twin1.txt");
    assert_eq!((twin.change_type.as_str(), twin.old_path.as_deref()), ("renamed", Some("old/twin1.txt")));
    let deep = fx.last_revision("new/sub/twin1.txt");
    assert_eq!((deep.change_type.as_str(), deep.old_path.as_deref()), ("renamed", Some("old/sub/twin1.txt")));
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions WHERE change_type = 'deleted'"), 0);
    tracker.stop("test");
}

#[test]
fn a_failing_hunk_write_leaves_no_revision_and_the_verify_scan_records_it_later() {
    let mut fx = Fixture::new("hunkfail");
    fx.write("f.txt", b"before\n");
    let tracker = fx.start(cfg());

    // Hunk の書き込みだけが失敗する状態を、別の接続のトリガで作る
    fx.db()
        .execute_batch("CREATE TRIGGER fail_hunk BEFORE INSERT ON change_hunks BEGIN SELECT RAISE(ABORT, 'injected hunk failure'); END;")
        .unwrap();
    fx.write("f.txt", b"after\n");
    wait_until("recorded as unrecorded", || tracker.status().unrecorded_since_ms.is_some());
    assert_eq!(fx.revision_count("f.txt"), 0, "Hunk を書けなかった Revision は確定しない");
    assert_eq!(fx.count("SELECT COUNT(*) FROM changes WHERE path = 'f.txt'"), 0, "Change も残らない");
    assert_eq!(fx.count("SELECT COUNT(*) FROM change_revisions"), 0);

    // 直ったら、検証の走査が実際の状態から拾い直す (gap)
    fx.db().execute_batch("DROP TRIGGER fail_hunk").unwrap();
    wait_until("recovered", || fx.revision_count("f.txt") == 1);
    let rev = fx.last_revision("f.txt");
    assert_eq!(rev.completeness, "gap");
    assert_eq!(rev.hunks.len(), 1);
    assert!(fx.count("SELECT COUNT(*) FROM coverage_gaps") >= 1);
    tracker.stop("test");
}

/// 表の全行を文字列にする (列の値をそのまま。順序は主キー)。
fn dump(conn: &Connection, sql: &str) -> Vec<String> {
    let mut stmt = conn.prepare(sql).unwrap();
    let columns = stmt.column_count();
    let rows = stmt
        .query_map([], |row| {
            let mut cells = Vec::new();
            for i in 0..columns {
                cells.push(match row.get_ref(i)? {
                    ValueRef::Null => "NULL".to_string(),
                    ValueRef::Integer(v) => v.to_string(),
                    ValueRef::Real(v) => v.to_string(),
                    ValueRef::Text(t) => format!("{:?}", String::from_utf8_lossy(t)),
                    ValueRef::Blob(b) => format!("blob{}", b.len()),
                });
            }
            Ok(cells.join("|"))
        })
        .unwrap();
    rows.map(Result::unwrap).collect()
}

fn derived_tables(conn: &Connection) -> BTreeMap<&'static str, Vec<String>> {
    let mut out = BTreeMap::new();
    out.insert("changes", dump(conn, "SELECT * FROM changes ORDER BY change_id"));
    out.insert("change_revisions", dump(conn, "SELECT * FROM change_revisions ORDER BY revision_id"));
    out.insert("change_hunks", dump(conn, "SELECT * FROM change_hunks ORDER BY hunk_id"));
    out.insert("snapshots", dump(conn, "SELECT * FROM snapshots ORDER BY snapshot_id"));
    out
}

/// 変更を一通り作って止め、DB の派生の表を返す。
fn record_a_varied_history(fx: &mut Fixture) -> BTreeMap<&'static str, Vec<String>> {
    fx.write("edit.txt", lines(40).as_bytes());
    fx.write("del.txt", b"to delete\n");
    fx.write("ren.txt", b"to rename\n");
    fx.write("crlf.txt", b"x\r\ny\r\n");
    let tracker = fx.start(cfg());

    let edited = lines(40).replace("line05\n", "FIVE\n").replace("line30\n", "THIRTY\n");
    fx.write("edit.txt", edited.as_bytes());
    wait_until("edit", || fx.revision_count("edit.txt") == 1);
    fx.write("edit.txt", format!("{edited}appended 日本語\n").as_bytes());
    wait_until("edit2", || fx.revision_count("edit.txt") == 2);
    fx.remove("del.txt");
    wait_until("del", || fx.revision_count("del.txt") == 1);
    fx.rename("ren.txt", "moved/ren2.txt");
    wait_until("ren", || fx.revision_count("moved/ren2.txt") == 1);
    fx.write("crlf.txt", b"x\ny\n");
    wait_until("crlf", || fx.revision_count("crlf.txt") == 1);
    fx.write("fresh.txt", b"brand new\n");
    wait_until("fresh", || fx.revision_count("fresh.txt") == 1);
    fx.write("blob.bin", &[0xff, 0x00, 0xfe]);
    wait_until("bin", || fx.revision_count("blob.bin") == 1);
    fx.write("secret.key", b"secret\n");
    wait_until("key", || fx.revision_count("secret.key") == 1);
    tracker.stop("test");
    derived_tables(&fx.db())
}

/// DB を壊して (SQLite のファイルでなくして) 開き直す。`broken_db` と `rebuilt` が入った報告を返す。
fn corrupt_and_reopen(fx: &Fixture) -> mye_track::startup::StartupReport {
    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert_eq!(report.state, Some(ServiceState::Recording), "{report:?}");
    drop(store);
    report
}

#[test]
fn a_broken_db_is_rebuilt_to_the_same_changes_revisions_and_hunks() {
    let mut fx = Fixture::new("rebuild");
    let before = record_a_varied_history(&mut fx);
    assert!(!before["change_hunks"].is_empty() && before["changes"].len() >= 7, "{before:#?}");

    let report = corrupt_and_reopen(&fx);
    assert!(report.broken_db.is_some(), "壊れた DB は退避される");
    let rebuilt = report.rebuilt.clone().expect("派生の表を作り直した");
    assert_eq!(rebuilt.missing_blobs, 0);
    assert!(report.failed_steps.is_empty(), "{:?}", report.failed_steps);

    let after = derived_tables(&fx.db());
    for table in ["changes", "change_revisions", "change_hunks", "snapshots"] {
        assert_eq!(before[table], after[table], "{table} が壊す前と違う");
    }
    // 作り直した DB で、もう一度開いても同じ (入れ直しが二重に入らない)
    drop(open_project(&fx.root, now_ms()).0);
    assert_eq!(after, derived_tables(&fx.db()));
}

#[test]
fn events_replayed_from_the_journal_also_rebuild_their_revision() {
    // トランザクションが確定する前に落ちた状態を模す: 最後の観測のイベントと派生の行を DB から消す。
    // ジャーナルには残っているので、次の起動で入れ直され、Revision と Hunk も揃う
    let mut fx = Fixture::new("replay_derive");
    fx.write("x.txt", b"1\n2\n3\n");
    let tracker = fx.start(cfg());
    fx.write("x.txt", b"1\nTWO\n3\n");
    wait_until("modified", || fx.revision_count("x.txt") == 1);
    tracker.stop("test");
    let before = derived_tables(&fx.db());

    {
        let conn = fx.db();
        let event_id: i64 = conn
            .query_row("SELECT MAX(event_id) FROM events WHERE kind = 'observed'", [], |r| r.get(0))
            .unwrap();
        conn.execute_batch(&format!(
            "DELETE FROM events WHERE event_id >= {event_id};
             DELETE FROM change_hunks;
             DELETE FROM change_revisions;
             DELETE FROM changes;
             DELETE FROM snapshots WHERE kind = 'disk';"
        ))
        .unwrap();
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert_eq!(report.state, Some(ServiceState::Recording), "{report:?}");
    assert!(report.broken_db.is_none());
    assert!(report.replayed_events >= 1);
    drop(store);
    assert_eq!(before, derived_tables(&fx.db()));
}

#[test]
fn a_missing_blob_keeps_its_revision_but_marks_it_missing_without_hunks() {
    let mut fx = Fixture::new("rebuild_missing");
    let before = record_a_varied_history(&mut fx);
    let target_hash = {
        let views = revision_views(&fx.db(), "fresh.txt");
        views[0].after_hash.clone().unwrap()
    };
    let paths = Paths::for_project(&fx.root);
    fs::remove_file(paths.blobs.join(&target_hash[..2]).join(&target_hash[2..])).unwrap();

    let report = corrupt_and_reopen(&fx);
    assert!(report.rebuilt.is_some());
    let after = derived_tables(&fx.db());
    assert_eq!(before["changes"], after["changes"]);
    assert_eq!(before["change_revisions"].len(), after["change_revisions"].len(), "Revision の行は戻る");
    let conn = fx.db();
    let state: String = conn
        .query_row("SELECT blob_state FROM snapshots WHERE blob_hash = ?1", [&target_hash], |r| r.get(0))
        .unwrap();
    assert_eq!(state, "missing");
    let rev = revision_views(&conn, "fresh.txt").pop().unwrap();
    assert_eq!((rev.diff_mode.as_str(), rev.hunks.len()), ("unavailable", 0));
    // 欠けた Revision 以外は、壊す前と同じ Hunk を持つ
    let other_hunks: i64 = conn
        .query_row(
            "SELECT COUNT(*) FROM change_hunks h JOIN change_revisions r ON r.revision_id = h.revision_id
               JOIN changes c ON c.change_id = r.change_id WHERE c.path != 'fresh.txt'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    let before_other = before["change_hunks"].len() as i64 - 1;
    assert_eq!(other_hunks, before_other);
}
