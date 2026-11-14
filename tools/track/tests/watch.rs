//! ファイル監視・ベースライン・欠落区間の結合テスト (M104d)。
//!
//! 「イベントが来るまで待つ」は固定の sleep ではなく、上限付きの待ち合わせ (`wait_until`) で書く。
//! 「何も起きないこと」の確認だけは、後から書いた番兵ファイルの Revision が出るのを待つ
//! (番兵のデバウンスが先のパスより後に切れるので、先のパスの処理は済んでいる)。
//! 全テストの後処理で、テスト自身が書いた分を除いて作業ツリー (`.mye` を除く) が変わっていないことを検査する (spec §2 D17)。

use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use rusqlite::Connection;
use serde_json::Value;

use mye_track::collector::TrackerConfig;
use mye_track::db::now_ms;
use mye_track::hash::sha256_hex;
use mye_track::startup::{open_project, ServiceState};
use mye_track::store::Paths;
use mye_track::watch::{Phase, Tracker};

/// 待ちの上限。遅い CI でも足りるよう長めにする (通る場合は待たない)。
const WAIT_LIMIT: Duration = Duration::from_secs(60);

fn fast_config() -> TrackerConfig {
    TrackerConfig {
        debounce: Duration::from_millis(150),
        max_hold: Duration::from_secs(1),
        busy_retry: Duration::from_millis(50),
        settle_wait: Duration::from_millis(100),
        settle_give_up: Duration::from_secs(10),
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
    /// テストが書いた結果として期待するツリー (相対パス → 内容)。
    model: BTreeMap<String, Vec<u8>>,
}

impl Fixture {
    fn new(name: &str) -> Fixture {
        let root = std::env::temp_dir().join(format!("mye_track_w_{}_{}", name, std::process::id()));
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
        fs::rename(self.path(from), self.path(to)).unwrap();
        let bytes = self.model.remove(from).unwrap();
        self.model.insert(to.to_string(), bytes);
    }

    /// ディレクトリごとの名前変更 (`std::fs::rename`)。
    fn rename_dir(&mut self, from: &str, to: &str) {
        fs::rename(self.path(from), self.path(to)).unwrap();
        let prefix = format!("{from}/");
        let moved: Vec<String> = self.model.keys().filter(|k| k.starts_with(&prefix)).cloned().collect();
        for key in moved {
            let bytes = self.model.remove(&key).unwrap();
            self.model.insert(format!("{to}/{}", &key[prefix.len()..]), bytes);
        }
    }

    fn remove_dir(&mut self, rel: &str) {
        fs::remove_dir_all(self.path(rel)).unwrap();
        let prefix = format!("{rel}/");
        self.model.retain(|k, _| !k.starts_with(&prefix));
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

    fn revisions(&self, path: &str) -> Vec<Rev> {
        revisions(&self.db(), path)
    }

    fn revision_count(&self, path: &str) -> usize {
        self.revisions(path).len()
    }

    fn events(&self, kind: &str) -> Vec<Value> {
        events(&self.db(), kind)
    }

    /// 番兵を書き、その Revision が出るまで待つ。それより前に書いたパスの処理は済んでいる。
    fn settle(&mut self, name: &str) {
        let rel = format!("zz_sentinel_{name}.txt");
        self.write(&rel, name.as_bytes());
        wait_until(&format!("番兵 {name}"), WAIT_LIMIT, || self.revision_count(&rel) == 1);
    }

    fn blob(&self, hash: &str) -> Vec<u8> {
        fs::read(Paths::for_project(&self.root).blobs.join(&hash[..2]).join(&hash[2..])).unwrap()
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
    old_path: Option<String>,
    before: Option<String>,
    after: Option<String>,
    actor: String,
    attribution: String,
    kind: String,
}

fn revisions(conn: &Connection, path: &str) -> Vec<Rev> {
    let mut stmt = conn
        .prepare(
            "SELECT r.change_type, r.completeness, r.old_path, sb.blob_hash, sa.blob_hash, r.actor, r.attribution, c.kind
               FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
               LEFT JOIN snapshots sb ON sb.snapshot_id = r.before_snapshot
               LEFT JOIN snapshots sa ON sa.snapshot_id = r.after_snapshot
              WHERE c.path = ?1 ORDER BY r.revision_id",
        )
        .unwrap();
    let rows = stmt
        .query_map([path], |r| {
            Ok(Rev {
                change_type: r.get(0)?,
                completeness: r.get(1)?,
                old_path: r.get(2)?,
                before: r.get(3)?,
                after: r.get(4)?,
                actor: r.get(5)?,
                attribution: r.get(6)?,
                kind: r.get(7)?,
            })
        })
        .unwrap();
    rows.map(Result::unwrap).collect()
}

fn events(conn: &Connection, kind: &str) -> Vec<Value> {
    let mut stmt = conn.prepare("SELECT payload FROM events WHERE kind = ?1 ORDER BY event_id").unwrap();
    let rows = stmt.query_map([kind], |r| r.get::<_, String>(0)).unwrap();
    rows.map(|p| serde_json::from_str(&p.unwrap()).unwrap()).collect()
}

fn scalar(conn: &Connection, sql: &str) -> i64 {
    conn.query_row(sql, [], |r| r.get(0)).unwrap()
}

fn hash_of(bytes: &[u8]) -> String {
    sha256_hex(bytes)
}

#[test]
fn create_modify_rename_delete_each_make_one_revision() {
    let mut fx = Fixture::new("crud");
    fx.write("keep.txt", b"keep");
    let tracker = fx.start(fast_config());

    // 作成
    fx.write("a.txt", b"first");
    wait_until("added", WAIT_LIMIT, || fx.revision_count("a.txt") == 1);
    let revs = fx.revisions("a.txt");
    assert_eq!(revs[0].change_type, "added");
    assert_eq!(revs[0].completeness, "complete");
    assert_eq!(revs[0].before, None);
    assert_eq!(fx.blob(revs[0].after.as_ref().unwrap()), b"first");
    assert_eq!((revs[0].actor.as_str(), revs[0].attribution.as_str()), ("unknown", "unknown"));

    // 変更
    fx.write("a.txt", b"second version");
    wait_until("modified", WAIT_LIMIT, || fx.revision_count("a.txt") == 2);
    let revs = fx.revisions("a.txt");
    assert_eq!(revs[1].change_type, "modified");
    assert_eq!(revs[1].completeness, "complete");
    assert_eq!(fx.blob(revs[1].before.as_ref().unwrap()), b"first");
    assert_eq!(fx.blob(revs[1].after.as_ref().unwrap()), b"second version");

    // 同じディレクトリ内のリネーム
    fx.rename("a.txt", "b.txt");
    wait_until("renamed", WAIT_LIMIT, || fx.revision_count("b.txt") == 1);
    let renamed = fx.revisions("b.txt");
    assert_eq!(renamed[0].change_type, "renamed");
    assert_eq!(renamed[0].old_path.as_deref(), Some("a.txt"));
    assert_eq!(fx.blob(renamed[0].before.as_ref().unwrap()), b"second version");
    assert_eq!(fx.blob(renamed[0].after.as_ref().unwrap()), b"second version");

    // 削除
    fx.remove("b.txt");
    wait_until("deleted", WAIT_LIMIT, || fx.revision_count("b.txt") == 2);
    let revs = fx.revisions("b.txt");
    assert_eq!(revs[1].change_type, "deleted");
    assert_eq!(revs[1].after, None);
    assert_eq!(fx.blob(revs[1].before.as_ref().unwrap()), b"second version");

    fx.settle("crud");
    // 旧パスには、削除やリネームの Revision が重複して付いていない
    assert_eq!(fx.revision_count("a.txt"), 2);
    assert_eq!(fx.revision_count("keep.txt"), 0);

    // 観測のイベントに、派生の表を作り直せるだけの情報がある (spec D26)
    let observed = fx.events("observed");
    let rename_event = observed.iter().find(|e| e["change_type"] == "renamed").expect("renamed event");
    assert_eq!(rename_event["path"], "b.txt");
    assert_eq!(rename_event["old_path"], "a.txt");
    assert_eq!(rename_event["before"]["hash"], hash_of(b"second version"));
    assert_eq!(rename_event["before"]["size"], 14);
    assert_eq!(rename_event["after"]["hash"], hash_of(b"second version"));
    assert_eq!(rename_event["completeness"], "complete");
    assert!(rename_event["observed_at"].as_i64().unwrap() > 0);
    tracker.stop("test");
}

#[test]
fn atomic_save_is_one_change_on_the_target_path() {
    let mut fx = Fixture::new("atomic");
    fx.write("x.json", b"{\"v\":1}");
    let tracker = fx.start(fast_config());

    // WriteFileReplacing と同じ: 一時ファイルへ書いて、既存へ rename
    let tmp = fx.path("x.json.123.1.0.tmp");
    fs::write(&tmp, b"{\"v\":2}").unwrap();
    fs::rename(&tmp, fx.path("x.json")).unwrap();
    fx.model.insert("x.json".to_string(), b"{\"v\":2}".to_vec());

    wait_until("modified", WAIT_LIMIT, || fx.revision_count("x.json") == 1);
    fx.settle("atomic");
    let revs = fx.revisions("x.json");
    assert_eq!(revs.len(), 1);
    assert_eq!(revs[0].change_type, "modified");
    assert_eq!(fx.blob(revs[0].before.as_ref().unwrap()), b"{\"v\":1}");
    assert_eq!(fx.blob(revs[0].after.as_ref().unwrap()), b"{\"v\":2}");
    // 一時ファイルは作成と削除のどちらも別の変更として残らない
    let tmp_changes = scalar(&fx.db(), "SELECT COUNT(*) FROM changes WHERE path LIKE '%.tmp'");
    assert_eq!(tmp_changes, 0);

    // 新しい名前へ置く保存も、その名前の 1 件の追加
    let tmp = fx.path("y.json.123.2.0.tmp");
    fs::write(&tmp, b"y").unwrap();
    fs::rename(&tmp, fx.path("y.json")).unwrap();
    fx.model.insert("y.json".to_string(), b"y".to_vec());
    wait_until("added y", WAIT_LIMIT, || fx.revision_count("y.json") == 1);
    fx.settle("atomic2");
    assert_eq!(fx.revisions("y.json")[0].change_type, "added");
    assert_eq!(scalar(&fx.db(), "SELECT COUNT(*) FROM changes WHERE path LIKE '%.tmp'"), 0);
    tracker.stop("test");
}

#[test]
fn writes_within_debounce_make_one_coalesced_revision() {
    let mut fx = Fixture::new("coalesce");
    fx.write("c.txt", b"v0");
    // 3 回の書き込み (合わせて約 60 ms) が 1 つのデバウンスに入る。CPU が詰まっても割れないよう長めにする
    let tracker = fx.start(TrackerConfig { debounce: Duration::from_millis(600), max_hold: Duration::from_secs(5), ..fast_config() });

    for v in ["v1", "v2", "v3"] {
        fx.write("c.txt", v.as_bytes());
        std::thread::sleep(Duration::from_millis(30));
    }
    wait_until("revision", WAIT_LIMIT, || fx.revision_count("c.txt") >= 1);
    fx.settle("coalesce");
    let revs = fx.revisions("c.txt");
    assert_eq!(revs.len(), 1, "{revs:?}");
    assert_eq!(revs[0].completeness, "coalesced");
    assert_eq!(fx.blob(revs[0].before.as_ref().unwrap()), b"v0");
    assert_eq!(fx.blob(revs[0].after.as_ref().unwrap()), b"v3");
    tracker.stop("test");
}

#[test]
fn single_save_is_complete_not_coalesced() {
    let mut fx = Fixture::new("complete");
    fx.write("s.txt", b"before");
    let tracker = fx.start(fast_config());
    fx.write("s.txt", b"after");
    wait_until("revision", WAIT_LIMIT, || fx.revision_count("s.txt") == 1);
    assert_eq!(fx.revisions("s.txt")[0].completeness, "complete");
    tracker.stop("test");
}

#[test]
fn changes_while_service_was_down_are_gap_unknown() {
    let mut fx = Fixture::new("down");
    fx.write("a.txt", b"a0");
    fx.write("c.txt", b"c0");
    fx.write("same.txt", b"same");
    let tracker = fx.start(fast_config());
    assert!(tracker.status().first_run);
    assert_eq!(tracker.status().baseline_files, 3);
    tracker.stop("test");
    {
        let db = fx.db();
        assert_eq!(scalar(&db, "SELECT COUNT(*) FROM snapshots WHERE kind = 'baseline'"), 3);
        assert_eq!(scalar(&db, "SELECT COUNT(*) FROM change_revisions"), 0, "ベースラインは変更ではない");
    }

    // 止まっている間: a は 2 回変える (1 件になる)、b を足す、c を消す、same は触らない (同じ内容で書き直す)
    fx.write("a.txt", b"a1");
    fx.write("a.txt", b"a2");
    fx.write("b.txt", b"b0");
    fx.remove("c.txt");
    fx.write("same.txt", b"same");

    let tracker = fx.start(fast_config());
    assert!(!tracker.status().first_run);
    let a = fx.revisions("a.txt");
    assert_eq!(a.len(), 1);
    assert_eq!((a[0].change_type.as_str(), a[0].completeness.as_str()), ("modified", "gap"));
    assert_eq!((a[0].actor.as_str(), a[0].attribution.as_str()), ("unknown", "unknown"));
    assert_eq!(fx.blob(a[0].before.as_ref().unwrap()), b"a0");
    assert_eq!(fx.blob(a[0].after.as_ref().unwrap()), b"a2");
    let b = fx.revisions("b.txt");
    assert_eq!((b.len(), b[0].change_type.as_str(), b[0].completeness.as_str()), (1, "added", "gap"));
    let c = fx.revisions("c.txt");
    assert_eq!((c.len(), c[0].change_type.as_str(), c[0].completeness.as_str()), (1, "deleted", "gap"));
    assert_eq!(fx.revision_count("same.txt"), 0);

    let db = fx.db();
    assert_eq!(scalar(&db, "SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'service_down'"), 1);
    let (from, to): (i64, i64) = db
        .query_row("SELECT from_at, to_at FROM coverage_gaps WHERE reason = 'service_down'", [], |r| {
            Ok((r.get(0)?, r.get(1)?))
        })
        .unwrap();
    assert!(from <= to);
    assert_eq!(fx.events("service_start").len(), 2);
    assert_eq!(fx.events("service_stop").len(), 1);
    assert_eq!(fx.events("baseline_complete").len(), 2);
    assert!(fx.events("service_stop")[0]["reason"] == "test");

    // 2 回目の起動の後の通常の変更は、状態を引き継いでいる (Before = 起動時に確認した内容)
    fx.write("a.txt", b"a3");
    wait_until("after restart", WAIT_LIMIT, || fx.revision_count("a.txt") == 2);
    let a = fx.revisions("a.txt");
    assert_eq!(a[1].completeness, "complete");
    assert_eq!(fx.blob(a[1].before.as_ref().unwrap()), b"a2");

    // 削除したままのファイルは、3 回目の起動で「また削除された」扱いにならない
    tracker.stop("test");
    let tracker = fx.start(fast_config());
    assert_eq!(fx.revision_count("c.txt"), 1);
    assert_eq!(fx.revision_count("b.txt"), 1);
    tracker.stop("test");
}

#[test]
fn ignored_paths_are_not_recorded() {
    let mut fx = Fixture::new("ignore");
    fx.write(".gitignore", b"*.log\ngen/\n");
    fx.write("src/main.txt", b"main");
    let tracker = fx.start(fast_config());

    fx.write(".git/HEAD", b"x");
    fx.write("target/out.bin", b"x");
    fx.write("tools/x/target/deep.bin", b"x");
    fx.write("bin/a.exe", b"x");
    fx.write("obj/a.o", b"x");
    fx.write("cache/c.dat", b"x");
    fx.write("dist/d.zip", b"x");
    fx.write("a.log", b"x");
    fx.write("src/b.log", b"x");
    fx.write("gen/gen.cpp", b"x");
    // `.mye\ai_changes\` の中は追跡サービス自身が書く場所。ここへ書いても拾わない
    fs::write(fx.path(".mye").join("note.txt"), b"x").unwrap();
    fx.write("visible.txt", b"visible");
    wait_until("visible", WAIT_LIMIT, || fx.revision_count("visible.txt") == 1);
    fx.settle("ignore");

    let paths: Vec<String> = {
        let db = fx.db();
        let mut stmt = db.prepare("SELECT path FROM changes ORDER BY path").unwrap();
        stmt.query_map([], |r| r.get(0)).unwrap().map(Result::unwrap).collect()
    };
    assert_eq!(paths, ["visible.txt", "zz_sentinel_ignore.txt"]);
    // ベースラインにも無視したパスは入っていない
    let snap_paths: Vec<String> = {
        let db = fx.db();
        let mut stmt = db.prepare("SELECT DISTINCT path FROM snapshots ORDER BY path").unwrap();
        stmt.query_map([], |r| r.get(0)).unwrap().map(Result::unwrap).collect()
    };
    assert_eq!(snap_paths, [".gitignore", "src/main.txt", "visible.txt", "zz_sentinel_ignore.txt"]);

    // `.gitignore` を書き換えると、次の判定から新しい規則になる
    fx.write(".gitignore", b"*.log\ngen/\nsecret.txt\n");
    wait_until(".gitignore", WAIT_LIMIT, || fx.revision_count(".gitignore") == 1);
    fx.write("secret.txt", b"s");
    fx.write("other.txt", b"o");
    wait_until("other", WAIT_LIMIT, || fx.revision_count("other.txt") == 1);
    fx.settle("ignore2");
    assert_eq!(fx.revision_count("secret.txt"), 0);
    tracker.stop("test");
}

#[test]
fn binary_big_and_excluded_files() {
    let mut fx = Fixture::new("kinds");
    let tracker = fx.start(TrackerConfig { max_text_bytes: 1024, ..fast_config() });

    fx.write("text.txt", "日本語のテキスト".as_bytes());
    fx.write("blob.dat", &[0xff, 0xfe, 0x00, 0x80, 0x81]);
    fx.write("big.txt", &vec![b'a'; 2000]);
    fx.write(".env", b"SECRET_TOKEN=abc123");
    fx.write("keys/server.pem", b"PRIVATE KEY MATERIAL");
    fx.write("keep.txt", b"ok");
    for rel in ["text.txt", "blob.dat", "big.txt", ".env", "keys/server.pem", "keep.txt"] {
        wait_until(rel, WAIT_LIMIT, || fx.revision_count(rel) == 1);
    }
    fx.settle("kinds");

    assert_eq!(fx.revisions("text.txt")[0].kind, "text");
    assert_eq!(fx.revisions("blob.dat")[0].kind, "binary");
    assert_eq!(fx.revisions("big.txt")[0].kind, "binary");
    assert!(fx.revisions("blob.dat")[0].after.is_some(), "binary でも原本は残る");

    // 除外パターン: 変わったことだけ。内容も、内容のハッシュも残さない
    for rel in [".env", "keys/server.pem"] {
        let revs = fx.revisions(rel);
        assert_eq!(revs[0].change_type, "added");
        assert_eq!(revs[0].after, None);
    }
    let db = fx.db();
    assert_eq!(
        scalar(&db, "SELECT COUNT(*) FROM snapshots WHERE path IN ('.env','keys/server.pem') AND blob_state = 'excluded' AND blob_hash IS NULL"),
        2
    );
    let env_event = fx.events("observed").into_iter().find(|e| e["path"] == ".env").unwrap();
    assert_eq!(env_event["after"]["excluded"], true);
    assert!(env_event["after"]["hash"].is_null());
    assert_eq!(env_event["after"]["size"], 19);

    // Blob のどこにも秘密の中身が無い (ジャーナルにも無い)
    let paths = Paths::for_project(&fx.root);
    for dir in [paths.blobs.clone(), paths.journal.clone()] {
        let mut stack = vec![dir];
        while let Some(d) = stack.pop() {
            for e in fs::read_dir(d).unwrap().flatten() {
                if e.file_type().unwrap().is_dir() {
                    stack.push(e.path());
                } else {
                    let bytes = fs::read(e.path()).unwrap();
                    let text = String::from_utf8_lossy(&bytes);
                    assert!(!text.contains("SECRET_TOKEN") && !text.contains("PRIVATE KEY"), "{:?}", e.path());
                }
            }
        }
    }

    // 除外パスの変更も 1 件の「変わったこと」になる。同じ内容の書き直しは変更にならない (メモリ上のハッシュ)
    fx.write(".env", b"SECRET_TOKEN=abc123");
    fx.write(".env", b"SECRET_TOKEN=def456");
    wait_until(".env modified", WAIT_LIMIT, || fx.revision_count(".env") == 2);
    assert_eq!(fx.revisions(".env")[1].change_type, "modified");
    tracker.stop("test");
}

#[test]
fn same_content_save_makes_no_revision() {
    let mut fx = Fixture::new("samecontent");
    fx.write("h.txt", b"hello");
    let tracker = fx.start(fast_config());

    fx.write("h.txt", b"hello");
    let tmp = fx.path("h.txt.9.9.9.tmp");
    fs::write(&tmp, b"hello").unwrap();
    fs::rename(&tmp, fx.path("h.txt")).unwrap();
    fx.settle("same");
    assert_eq!(fx.revision_count("h.txt"), 0);
    tracker.stop("test");
}

#[test]
fn cross_directory_move_becomes_one_rename() {
    let mut fx = Fixture::new("move");
    fx.write("from/m.txt", b"moving");
    fx.write("to/keep.txt", b"keep");
    let tracker = fx.start(fast_config());

    // 監視には削除 + 作成でしか届かない。同じ窓の同じ内容なので 1 件の renamed にまとまる (M104e)
    fx.rename("from/m.txt", "to/m.txt");
    wait_until("renamed", WAIT_LIMIT, || fx.revision_count("to/m.txt") == 1);
    fx.settle("move");
    let revs = fx.revisions("to/m.txt");
    assert_eq!(revs[0].change_type, "renamed");
    assert_eq!(revs[0].old_path.as_deref(), Some("from/m.txt"));
    assert_eq!(fx.revision_count("from/m.txt"), 0, "元のパスに削除の Revision は残らない");
    let observed = fx.events("observed");
    assert_eq!(observed.iter().filter(|e| e["path"] == "from/m.txt").count(), 0);
    tracker.stop("test");
}

#[test]
fn directory_operations_are_all_recorded() {
    let mut fx = Fixture::new("dirs");
    fx.write("d1/a.txt", b"a");
    fx.write("d1/b.txt", b"b");
    fx.write("d1/sub/deep.txt", b"deep");
    let tracker = fx.start(fast_config());

    // 名前変更: 中のファイルのイベントは来ない。削除と作成が、内容の同じ renamed 3 件にまとまる (M104e)
    fx.rename_dir("d1", "d2");
    for (old, new) in [("d1/a.txt", "d2/a.txt"), ("d1/b.txt", "d2/b.txt"), ("d1/sub/deep.txt", "d2/sub/deep.txt")] {
        wait_until(new, WAIT_LIMIT, || fx.revision_count(new) == 1);
        let revs = fx.revisions(new);
        assert_eq!(revs[0].change_type, "renamed");
        assert_eq!(revs[0].old_path.as_deref(), Some(old));
        assert_eq!(fx.revision_count(old), 0);
    }

    // 外で作ったツリーを移し入れる
    let outside = std::env::temp_dir().join(format!("mye_track_w_dirs_outside_{}", std::process::id()));
    let _ = fs::remove_dir_all(&outside);
    fs::create_dir_all(outside.join("tree/inner")).unwrap();
    for (rel, body) in [("tree/t1.txt", "1"), ("tree/t2.txt", "2"), ("tree/inner/t3.txt", "3"), ("tree/inner/t4.txt", "4")] {
        fs::write(outside.join(rel), body).unwrap();
    }
    fs::rename(outside.join("tree"), fx.path("arrived")).unwrap();
    for (rel, body) in [("arrived/t1.txt", "1"), ("arrived/t2.txt", "2"), ("arrived/inner/t3.txt", "3"), ("arrived/inner/t4.txt", "4")] {
        fx.model.insert(rel.to_string(), body.as_bytes().to_vec());
        wait_until(rel, WAIT_LIMIT, || fx.revision_count(rel) == 1);
        let revs = fx.revisions(rel);
        assert_eq!(revs[0].change_type, "added");
        assert_eq!(fx.blob(revs[0].after.as_ref().unwrap()), body.as_bytes());
    }

    // 監視の外へ移し出す
    fs::rename(fx.path("arrived"), outside.join("gone")).unwrap();
    fx.model.retain(|k, _| !k.starts_with("arrived/"));
    for rel in ["arrived/t1.txt", "arrived/t2.txt", "arrived/inner/t3.txt", "arrived/inner/t4.txt"] {
        wait_until(rel, WAIT_LIMIT, || fx.revision_count(rel) == 2);
        assert_eq!(fx.revisions(rel)[1].change_type, "deleted");
    }
    let _ = fs::remove_dir_all(&outside);

    // ツリーごとの削除
    fx.remove_dir("d2");
    for rel in ["d2/a.txt", "d2/b.txt", "d2/sub/deep.txt"] {
        wait_until(rel, WAIT_LIMIT, || fx.revision_count(rel) == 2);
        assert_eq!(fx.revisions(rel)[1].change_type, "deleted");
    }
    fx.settle("dirs");
    tracker.stop("test");
}

#[test]
fn thousand_files_are_all_recorded_or_overflow_is_reported() {
    let mut fx = Fixture::new("bulk");
    let tracker = fx.start(fast_config());
    let started = Instant::now();

    const N: usize = 1000;
    for i in 0..N {
        fx.write(&format!("bulk/f{i:04}.txt"), format!("v1 {i}").as_bytes());
    }
    let overflowed = |fx: &Fixture| scalar(&fx.db(), "SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'") > 0;
    wait_until("1000 added", WAIT_LIMIT, || {
        scalar(&fx.db(), "SELECT COUNT(*) FROM change_revisions") >= N as i64 || overflowed(&fx)
    });
    // 後から Revision が増えることがあるので、少し落ち着くまで待つ (番兵)
    fx.settle("bulk1");

    for i in 0..N {
        fx.write(&format!("bulk/f{i:04}.txt"), format!("v2 {i}").as_bytes());
    }
    fx.settle("bulk2");
    wait_until("1000 modified", WAIT_LIMIT, || {
        scalar(&fx.db(), "SELECT COUNT(*) FROM change_revisions WHERE change_type = 'modified'") >= N as i64
            || overflowed(&fx)
    });

    if !overflowed(&fx) {
        let db = fx.db();
        for i in 0..N {
            let revs = revisions(&db, &format!("bulk/f{i:04}.txt"));
            assert_eq!(revs.len(), 2, "f{i:04}: {revs:?}");
            assert_eq!(fx.blob(revs[1].after.as_ref().unwrap()), format!("v2 {i}").as_bytes());
        }
    } else {
        // あふれたなら、取りこぼしの区間が残っている。その後の記録は gap として全件が追いつく
        let covered = scalar(&fx.db(), "SELECT COUNT(DISTINCT c.path) FROM changes c WHERE c.path LIKE 'bulk/%'");
        assert_eq!(covered, N as i64);
    }
    assert!(started.elapsed() < Duration::from_secs(120));
    tracker.stop("test");
}

#[test]
fn injected_watcher_error_rebaselines_and_records_gap_changes() {
    let mut fx = Fixture::new("overflow");
    fx.write("x.txt", b"x0");
    fx.write("z.txt", b"z0");
    let tracker = fx.start(fast_config());

    // 取りこぼしを模す: この間のイベントは捨てる
    tracker.drop_incoming_events(true);
    fx.write("x.txt", b"x1");
    fx.write("y.txt", b"y0");
    fx.remove("z.txt");
    std::thread::sleep(Duration::from_millis(100));
    // 本物のあふれ (notify 8 は Windows で報告しない) の代わりに、報告そのものを差し込む
    tracker.inject_event(Err(notify::Error::generic("injected overflow")));
    wait_until("gap", WAIT_LIMIT, || {
        scalar(&fx.db(), "SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'") == 1
            && fx.revision_count("x.txt") == 1
            && fx.revision_count("y.txt") == 1
            && fx.revision_count("z.txt") == 1
    });
    tracker.drop_incoming_events(false);

    for (path, change_type) in [("x.txt", "modified"), ("y.txt", "added"), ("z.txt", "deleted")] {
        let revs = fx.revisions(path);
        assert_eq!(revs.len(), 1);
        assert_eq!((revs[0].change_type.as_str(), revs[0].completeness.as_str()), (change_type, "gap"));
        assert_eq!((revs[0].actor.as_str(), revs[0].attribution.as_str()), ("unknown", "unknown"));
    }
    assert_eq!(tracker.status().overflows, 1);
    let gap_event = &fx.events("coverage_gap")[0];
    assert_eq!(gap_event["reason"], "watcher_overflow");
    assert!(gap_event["from_at"].as_i64().unwrap() <= gap_event["to_at"].as_i64().unwrap());

    // ベースラインは取り直されている: 次の通常の変更の Before はあふれの後の内容
    fx.write("x.txt", b"x2");
    wait_until("x after", WAIT_LIMIT, || fx.revision_count("x.txt") == 2);
    let revs = fx.revisions("x.txt");
    assert_eq!(revs[1].completeness, "complete");
    assert_eq!(fx.blob(revs[1].before.as_ref().unwrap()), b"x1");
    // 変わっていないファイルは、再走査で変更にならない
    fx.settle("overflow");
    assert_eq!(fx.revision_count("x.txt"), 2);

    // 再走査を求めるフラグ付きのイベントも、同じ扱い
    let mut event = notify::Event::new(notify::EventKind::Other);
    event = event.set_flag(notify::event::Flag::Rescan);
    tracker.inject_event(Ok(event));
    wait_until("rescan flag", WAIT_LIMIT, || {
        scalar(&fx.db(), "SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'") == 2
    });
    tracker.stop("test");
}

#[test]
fn file_held_open_for_a_second_is_not_recorded_mid_write() {
    let mut fx = Fixture::new("stream");
    // 書き手が 50 ms ごとに書く。ほかのテストで CPU が詰まって書き手が止まっても見分けられるよう、
    // サイズが 250 ms 間隔で 3 回続けて同じになるまで読まない
    let tracker = fx.start(TrackerConfig { settle_samples: 3, settle_wait: Duration::from_millis(250), ..fast_config() });

    // 開いたままのハンドルへ 64 KiB ずつ 50 ms おきに 20 回 (1 秒)
    let path = fx.path("stream.bin");
    let chunk = vec![b'z'; 64 * 1024];
    let mut file = fs::File::create(&path).unwrap();
    for _ in 0..20 {
        file.write_all(&chunk).unwrap();
        file.flush().unwrap();
        std::thread::sleep(Duration::from_millis(50));
    }
    drop(file);
    let final_content = vec![b'z'; 64 * 1024 * 20];
    fx.model.insert("stream.bin".to_string(), final_content.clone());

    let final_hash = hash_of(&final_content);
    wait_until("final revision", WAIT_LIMIT, || {
        fx.revisions("stream.bin").last().is_some_and(|r| r.after.as_deref() == Some(final_hash.as_str()))
    });
    fx.settle("stream");
    let revs = fx.revisions("stream.bin");
    // 途中の内容は「完全な Revision」として残らない。残るなら coalesced
    for r in &revs {
        assert!(
            r.completeness == "coalesced" || r.after.as_deref() == Some(final_hash.as_str()),
            "途中の内容が complete で残った: {revs:?}"
        );
    }
    assert_eq!(revs.len(), 1, "{revs:?}");
    // 書き手が続いているのを見ていたので、複数回の書き込みをまとめた Revision
    assert_eq!(revs[0].completeness, "coalesced");
    tracker.stop("test");
}

#[test]
fn waiting_for_a_writer_gives_up_as_coalesced_and_the_final_content_follows() {
    let mut fx = Fixture::new("giveup");
    let tracker =
        fx.start(TrackerConfig { settle_give_up: Duration::from_millis(500), settle_samples: 3, ..fast_config() });

    // 1.5 秒のあいだ 30 ms ごとに追記し続ける。待ちの上限 (0.5 秒) を超えたら、その時点の内容を読む
    let path = fx.path("long.bin");
    let mut file = fs::File::create(&path).unwrap();
    let started = Instant::now();
    let mut written = Vec::new();
    while started.elapsed() < Duration::from_millis(1500) {
        file.write_all(b"0123456789abcdef").unwrap();
        written.extend_from_slice(b"0123456789abcdef");
        file.flush().unwrap();
        std::thread::sleep(Duration::from_millis(30));
    }
    drop(file);
    fx.model.insert("long.bin".to_string(), written.clone());

    let final_hash = hash_of(&written);
    wait_until("final", WAIT_LIMIT, || {
        fx.revisions("long.bin").last().is_some_and(|r| r.after.as_deref() == Some(final_hash.as_str()))
    });
    let revs = fx.revisions("long.bin");
    assert!(revs.len() >= 2, "待ちを打ち切った Revision と最終の Revision: {revs:?}");
    assert_eq!(revs[0].completeness, "coalesced");
    assert_ne!(revs[0].after.as_deref(), Some(final_hash.as_str()));
    tracker.stop("test");
}

#[test]
fn file_unreadable_at_baseline_is_no_before_when_it_changes() {
    use std::os::windows::fs::OpenOptionsExt;
    let mut fx = Fixture::new("nobefore");
    fx.write("locked.txt", b"locked");
    // 共有なしで開いたまま起動する: 内容を読めない
    let exclusive = fs::OpenOptions::new().read(true).share_mode(0).open(fx.path("locked.txt")).unwrap();
    let tracker = fx.start(fast_config());
    drop(exclusive);

    fx.write("locked.txt", b"now readable");
    wait_until("revision", WAIT_LIMIT, || fx.revision_count("locked.txt") == 1);
    let revs = fx.revisions("locked.txt");
    assert_eq!((revs[0].change_type.as_str(), revs[0].completeness.as_str()), ("modified", "no_before"));
    assert_eq!(revs[0].before, None);
    tracker.stop("test");
}

#[test]
fn startup_removes_stale_temp_blobs_only() {
    let fx = Fixture::new("tempblob");
    let (store, _) = open_project(&fx.root, now_ms());
    drop(store);
    let blobs = Paths::for_project(&fx.root).blobs;
    fs::create_dir_all(blobs.join("ab")).unwrap();
    fs::write(blobs.join("ab").join(".tmp-1234-0"), b"half written").unwrap();
    fs::write(blobs.join("ab").join("c".repeat(62)), b"a real blob").unwrap();

    let (store, report) = open_project(&fx.root, now_ms());
    drop(store);
    assert_eq!(report.temp_blobs_removed, 1);
    assert!(!blobs.join("ab").join(".tmp-1234-0").exists());
    assert!(blobs.join("ab").join("c".repeat(62)).exists());
}

/// 書き手 (別スレッド) が、追跡の読み取りのせいで失敗しない (spec §2 D21)。
/// デバウンスを極端に短くして、追跡側の読み取りを書き手の操作と重ね続ける。
#[test]
fn writers_never_fail_while_the_service_reads() {
    let mut fx = Fixture::new("writers");
    fx.write("w.txt", b"initial");
    let tracker = fx.start(TrackerConfig {
        debounce: Duration::from_millis(1),
        max_hold: Duration::from_millis(5),
        busy_retry: Duration::from_millis(2),
        settle_wait: Duration::from_millis(1),
        ..TrackerConfig::default()
    });

    let target = fx.path("w.txt");
    let failures = Arc::new(AtomicUsize::new(0));
    let attempts = Arc::new(AtomicUsize::new(0));
    let first_failure = Arc::new(std::sync::Mutex::new(String::new()));
    let writer = {
        let (target, failures, attempts, first_failure) =
            (target.clone(), Arc::clone(&failures), Arc::clone(&attempts), Arc::clone(&first_failure));
        std::thread::spawn(move || {
            let tmp = target.with_extension("txt.tmp");
            let record = |what: &str, r: std::io::Result<()>| {
                attempts.fetch_add(1, Ordering::SeqCst);
                if let Err(e) = r {
                    if failures.fetch_add(1, Ordering::SeqCst) == 0 {
                        *first_failure.lock().unwrap() = format!("{what}: {e} (os error {:?})", e.raw_os_error());
                    }
                }
            };
            for i in 0..1000 {
                let started = Instant::now();
                match i % 3 {
                    0 => {
                        record("write tmp", fs::write(&tmp, format!("tmp {i}")));
                        record("rename", fs::rename(&tmp, &target));
                    }
                    1 => record("fs::write", fs::write(&target, format!("direct {i}"))),
                    _ => {
                        record("remove", fs::remove_file(&target));
                        record("recreate", fs::write(&target, format!("recreated {i}")));
                    }
                }
                // 1 ms 間隔
                while started.elapsed() < Duration::from_millis(1) {
                    std::thread::yield_now();
                }
            }
        })
    };
    writer.join().unwrap();
    let final_text = fs::read(&target).unwrap();
    fx.model.insert("w.txt".to_string(), final_text);
    let _ = fs::remove_file(fx.path("w.txt.tmp"));
    eprintln!(
        "書き手の操作 {} 回、失敗 {} 件、Revision {} 件",
        attempts.load(Ordering::SeqCst),
        failures.load(Ordering::SeqCst),
        scalar(&fx.db(), "SELECT COUNT(*) FROM change_revisions")
    );
    assert_eq!(failures.load(Ordering::SeqCst), 0, "書き手が失敗した: {}", first_failure.lock().unwrap());
    tracker.stop("test");
}

/// 実行ファイルとして起動する。標準入力の EOF では止まらず (`DETACHED_PROCESS` で起動されると標準入力が無効で、
/// すぐ EOF になる)、pipe の `shutdown` で止まって `service_stop` を残す。
#[test]
fn serve_ignores_stdin_eof_and_stops_on_pipe_shutdown() {
    use std::process::{Command, Stdio};

    /// 失敗しても子プロセスを残さない。
    struct Child(std::process::Child);
    impl Drop for Child {
        fn drop(&mut self) {
            if self.0.try_wait().ok().flatten().is_none() {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }
    }

    let mut fx = Fixture::new("serve");
    fx.write("a.txt", b"a");
    let mut child = Child(
        Command::new(env!("CARGO_BIN_EXE_mye_track"))
            .args(["serve", "--project"])
            .arg(&fx.root)
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap(),
    );
    wait_until("baseline_complete", WAIT_LIMIT, || {
        Connection::open(Paths::for_project(&fx.root).db)
            .and_then(|c| c.query_row("SELECT COUNT(*) FROM events WHERE kind = 'baseline_complete'", [], |r| r.get::<_, i64>(0)))
            .is_ok_and(|n| n == 1)
    });
    // 標準入力を閉じても常駐し続ける
    drop(child.0.stdin.take());
    std::thread::sleep(Duration::from_millis(1500));
    assert!(child.0.try_wait().unwrap().is_none(), "標準入力の EOF で終わった");
    // 監視も動いている
    fx.write("b.txt", b"b");
    wait_until("b added", WAIT_LIMIT, || fx.revision_count("b.txt") == 1);

    let token = mye_track::client::read_service_json(&fx.root).unwrap()["token"].as_str().unwrap().to_string();
    let mut client = mye_track::client::Client::connect(&mye_track::pipe::pipe_name(&mye_track::project::project_id(&fx.root)), WAIT_LIMIT).unwrap();
    let hello = client
        .request(1, "hello", serde_json::json!({ "proto": 1, "role": "editor", "token": token, "projectRoot": fx.root.to_string_lossy() }))
        .unwrap();
    assert_eq!(hello["ok"], true, "{hello}");
    assert_eq!(client.request(2, "shutdown", serde_json::json!({})).unwrap()["ok"], true);
    wait_until("exit", WAIT_LIMIT, || child.0.try_wait().unwrap().is_some());
    assert!(child.0.wait().unwrap().success());
    let stops = fx.events("service_stop");
    assert_eq!(stops.len(), 1);
    assert_eq!(stops[0]["reason"], "shutdown_op");
}

/// 受け入れ条件 15: CPU を使い切る負荷の下でも、1 回の保存は complete。しきい値より十分長い間隔の 3 回書きは coalesced。
#[test]
fn single_saves_stay_complete_under_cpu_load() {
    let mut fx = Fixture::new("load");
    let tracker = fx.start(TrackerConfig { debounce: Duration::from_millis(1500), max_hold: Duration::from_secs(8), ..fast_config() });
    let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let cores = std::thread::available_parallelism().map_or(4, |n| n.get());
    let burners: Vec<_> = (0..cores)
        .map(|_| {
            let stop = Arc::clone(&stop);
            std::thread::spawn(move || {
                let mut x = 1u64;
                while !stop.load(Ordering::Relaxed) {
                    x = x.wrapping_mul(6364136223846793005).wrapping_add(1);
                    std::hint::black_box(x);
                }
            })
        })
        .collect();
    for i in 0..200 {
        fx.write(&format!("plain/p{i:03}.txt"), format!("v{i}").as_bytes());
    }
    for i in 0..200 {
        let rel = format!("atomic/a{i:03}.txt");
        let p = fx.path(&rel);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        let tmp = fx.path(&format!("atomic/a{i:03}.txt.1.1.tmp"));
        fs::write(&tmp, format!("v{i}")).unwrap();
        fs::rename(&tmp, &p).unwrap();
        fx.model.insert(rel, format!("v{i}").into_bytes());
    }
    // しきい値 (200 ms) より十分長い 400 ms 間隔、デバウンス (1.5 s) より短い合計
    fx.write("triple.txt", b"t0");
    for v in ["t1", "t2", "t3"] {
        std::thread::sleep(Duration::from_millis(400));
        fx.write("triple.txt", v.as_bytes());
    }
    wait_until("all recorded", Duration::from_secs(120), || {
        scalar(&fx.db(), "SELECT COUNT(*) FROM change_revisions") >= 401
    });
    stop.store(true, Ordering::Relaxed);
    for b in burners {
        b.join().unwrap();
    }
    fx.settle("load");
    let db = fx.db();
    assert_eq!(scalar(&db, "SELECT COUNT(*) FROM change_revisions WHERE completeness = 'coalesced' AND change_id IN (SELECT change_id FROM changes WHERE path NOT LIKE 'triple%')"), 0);
    assert_eq!(scalar(&db, "SELECT COUNT(*) FROM change_revisions WHERE completeness = 'complete'"), 401);
    assert_eq!(revisions(&db, "triple.txt")[0].completeness, "coalesced");
    tracker.stop("test");
}

/// 無視するディレクトリを含む所は非再帰、含まない子は再帰で見る (M104g2)。本数は `status` に出る。
#[test]
fn watches_are_split_around_ignored_directories() {
    let mut fx = Fixture::new("split");
    fx.write("bin/x.exe", b"x");
    fx.write("assets/a.txt", b"a");
    fx.write("src/s.txt", b"s");
    fx.write("tools/p/src/p.txt", b"p");
    fx.write("tools/p/target/t.bin", b"t");
    fx.write("tools/q/src/q.txt", b"q");
    let tracker = fx.start(fast_config());
    // ルート (.mye と bin を含む) / assets / src / tools (target を含む親は非再帰) / tools/p / tools/p/src / tools/q (再帰)
    let status = tracker.status();
    assert_eq!(status.watch_count, 7, "{status:?}");
    assert!(!status.watch_fallback);

    // 無視する場所は記録されず、追跡する場所は全部記録される
    fx.write("bin/y.exe", b"y");
    fx.write("tools/p/target/u.bin", b"u");
    let tracked = ["assets/a.txt", "src/s.txt", "tools/p/src/p.txt", "tools/q/src/q.txt", "root.txt", "tools/p/tools.txt", "tools/top.txt"];
    for rel in tracked {
        fx.write(rel, b"changed");
    }
    for rel in tracked {
        wait_until(rel, WAIT_LIMIT, || fx.revision_count(rel) >= 1);
    }
    fx.settle("split");
    assert_eq!(scalar(&fx.db(), "SELECT COUNT(*) FROM changes WHERE path LIKE 'bin/%' OR path LIKE '%/target/%'"), 0);
    tracker.stop("test");
}

/// ルート直下のファイルの作成・変更・リネーム・削除が、ルートの非再帰の監視で記録される。
#[test]
fn root_level_files_are_recorded_by_the_non_recursive_root_watch() {
    let mut fx = Fixture::new("rootfiles");
    fx.write("bin/x.exe", b"x");
    fx.write("src/keep.txt", b"keep");
    let tracker = fx.start(fast_config());
    assert_eq!(tracker.status().watch_count, 2, "ルート (非再帰) と src");

    fx.write("r.txt", b"1");
    wait_until("added", WAIT_LIMIT, || fx.revision_count("r.txt") == 1);
    fx.write("r.txt", b"2 changed");
    wait_until("modified", WAIT_LIMIT, || fx.revision_count("r.txt") == 2);
    fx.rename("r.txt", "r2.txt");
    wait_until("renamed", WAIT_LIMIT, || fx.revision_count("r2.txt") == 1);
    assert_eq!(fx.revisions("r2.txt")[0].change_type, "renamed");
    fx.remove("r2.txt");
    wait_until("deleted", WAIT_LIMIT, || fx.revision_count("r2.txt") == 2);
    assert_eq!(fx.revisions("r2.txt")[1].change_type, "deleted");
    tracker.stop("test");
}

/// 最上位のディレクトリを後から作る・名前を変える・消す、のあとも、中のファイルの変更が全部記録される。
/// 作った直後に書いたファイル (監視を張る前) も、突き合わせで拾われる。
#[test]
fn top_level_directories_created_renamed_and_removed_keep_being_watched() {
    let mut fx = Fixture::new("toplevel");
    fx.write("bin/x.exe", b"x");
    fx.write("src/a.txt", b"a");
    let tracker = fx.start(fast_config());
    let base = tracker.status().watch_count;

    // 作る: 作った直後から書く (監視を張る前の書き込みも拾う)
    for i in 0..30 {
        fx.write(&format!("fresh/f{i:02}.txt"), format!("v{i}").as_bytes());
    }
    for i in 0..30 {
        let rel = format!("fresh/f{i:02}.txt");
        wait_until(&rel, WAIT_LIMIT, || fx.revision_count(&rel) == 1);
    }
    wait_until("fresh を監視", WAIT_LIMIT, || tracker.status().watch_count == base + 1);
    fx.write("fresh/late.txt", b"late");
    wait_until("late", WAIT_LIMIT, || fx.revision_count("fresh/late.txt") == 1);

    // 名前を変える: 新しい名前の下の変更が記録される (古い名前の監視は外れる)
    fx.rename_dir("fresh", "renamed");
    wait_until("renamed 配下", WAIT_LIMIT, || fx.revision_count("renamed/f00.txt") == 1);
    wait_until("監視の付け替え", WAIT_LIMIT, || tracker.status().watch_count == base + 1);
    fx.write("renamed/after_rename.txt", b"ar");
    wait_until("after_rename", WAIT_LIMIT, || fx.revision_count("renamed/after_rename.txt") == 1);

    // 消す: 監視が外れ、同じ名前で作り直しても記録される
    fx.remove_dir("renamed");
    wait_until("消えた", WAIT_LIMIT, || fx.revision_count("renamed/f00.txt") == 2);
    wait_until("監視が外れる", WAIT_LIMIT, || tracker.status().watch_count == base);
    fx.write("renamed/again.txt", b"again");
    wait_until("again", WAIT_LIMIT, || fx.revision_count("renamed/again.txt") == 1);
    fx.write("renamed/again2.txt", b"again2");
    wait_until("again2", WAIT_LIMIT, || fx.revision_count("renamed/again2.txt") == 1);
    fx.settle("toplevel");
    tracker.stop("test");
}

/// `.gitignore` の書き換えで、無視する・しないが入れ替わったディレクトリの監視を付け替える。
#[test]
fn gitignore_changes_move_the_watches() {
    let mut fx = Fixture::new("gitignore_watch");
    fx.write(".gitignore", b"gen/\n");
    fx.write("gen/old.txt", b"old");
    fx.write("src/a.txt", b"a");
    let tracker = fx.start(fast_config());
    let ignored_count = tracker.status().watch_count; // ルート (.mye と gen を含む) + src
    assert_eq!(ignored_count, 2);

    fx.write("gen/ignored.txt", b"x");
    fx.settle("g1");
    assert_eq!(fx.revision_count("gen/ignored.txt"), 0);

    // 無視をやめる: 既にあったファイルは突き合わせで、これから書くファイルは新しい監視で記録される
    fx.write(".gitignore", b"# none\n");
    wait_until("gen を監視", WAIT_LIMIT, || tracker.status().watch_count == ignored_count + 1);
    wait_until("gen/old.txt", WAIT_LIMIT, || fx.revision_count("gen/old.txt") == 1);
    fx.write("gen/new.txt", b"new");
    wait_until("gen/new.txt", WAIT_LIMIT, || fx.revision_count("gen/new.txt") == 1);

    // 無視へ戻す: 監視が分かれ直し、中の変更は記録されない
    fx.write(".gitignore", b"gen/\n");
    wait_until("gen を外す", WAIT_LIMIT, || tracker.status().watch_count == ignored_count);
    fx.write("gen/again.txt", b"again");
    fx.write("src/b.txt", b"b");
    wait_until("src/b", WAIT_LIMIT, || fx.revision_count("src/b.txt") == 1);
    fx.settle("g2");
    assert_eq!(fx.revision_count("gen/again.txt"), 0);
    tracker.stop("test");
}

/// 無視するディレクトリへの書き込みの洪水の最中でも、追跡対象の保存が全部通常の Revision になる
/// (洪水のイベントが追跡対象と同じ OS のバッファを取り合わない)。実測 A の縮小版 (`mye_track bench flood`)。
#[test]
fn saves_during_a_flood_into_an_ignored_directory_are_never_gaps() {
    let mut fx = Fixture::new("flood");
    const SAVES: usize = 100;
    for i in 0..SAVES {
        fx.write(&format!("src/s{i:03}.txt"), b"v1");
    }
    fx.write("bin/seed.txt", b"x");
    let tracker = fx.start(TrackerConfig {
        verify_quiet: Duration::from_millis(200),
        verify_interval: Duration::from_millis(300),
        ..fast_config()
    });
    let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let cores = std::thread::available_parallelism().map_or(4, |n| n.get());
    let burners: Vec<_> = (0..cores)
        .map(|_| {
            let stop = Arc::clone(&stop);
            std::thread::spawn(move || {
                let mut x = 1u64;
                while !stop.load(Ordering::Relaxed) {
                    x = x.wrapping_mul(6364136223846793005).wrapping_add(1);
                    std::hint::black_box(x);
                }
            })
        })
        .collect();
    let flooders: Vec<_> = (0..8)
        .map(|t| {
            let dir = fx.path(&format!("bin/t{t}"));
            std::thread::spawn(move || {
                fs::create_dir_all(&dir).unwrap();
                for i in 0..1500 {
                    fs::write(dir.join(format!("long_flood_file_name_{t}_{i:05}_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.bin")), b"x").unwrap();
                }
            })
        })
        .collect();
    for i in 0..SAVES {
        fs::write(fx.path(&format!("src/s{i:03}.txt")), format!("v2 {i}")).unwrap();
        fx.model.insert(format!("src/s{i:03}.txt"), format!("v2 {i}").into_bytes());
        std::thread::sleep(Duration::from_millis(20));
    }
    for f in flooders {
        f.join().unwrap();
    }
    wait_until("all recorded", Duration::from_secs(120), || {
        scalar(&fx.db(), "SELECT COUNT(DISTINCT change_id) FROM changes WHERE path LIKE 'src/%'") >= SAVES as i64
    });
    stop.store(true, Ordering::Relaxed);
    for b in burners {
        b.join().unwrap();
    }
    let runs = tracker.status().verify_runs;
    wait_until("scans", WAIT_LIMIT, || tracker.status().verify_runs >= runs + 2);
    let db = fx.db();
    assert_eq!(scalar(&db, "SELECT COUNT(*) FROM change_revisions WHERE completeness = 'gap'"), 0, "洪水が追跡対象の取りこぼしになった");
    assert_eq!(scalar(&db, "SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'"), 0);
    assert_eq!(scalar(&db, "SELECT COUNT(*) FROM changes WHERE path LIKE 'bin/%'"), 0);
    tracker.stop("test");
    fs::remove_dir_all(fx.path("bin")).unwrap();
    fx.model.retain(|k, _| !k.starts_with("bin/"));
}
