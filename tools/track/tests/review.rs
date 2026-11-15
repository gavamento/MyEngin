//! レビュー (`review_set`、今の状態・集約・取得・通知・作り直し) の結合テスト (M104o)。
//!
//! Tracker と Writer を同じプロセスで動かし、`ops_record::review_set` と取得の op を直接呼ぶ (pipe の入口と同じ検査を通る)。
//! 今の状態の決め方と集約の表の純粋な関数は `src\review.rs` の単体テスト。ここは「実際に記録した Revision に対して」の確認。
//! 全テストの後処理で、テスト自身が書いた分を除いて作業ツリー (`.mye` を除く) が変わっていないことを検査する。

use std::collections::{BTreeMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicUsize;
use std::sync::Arc;
use std::time::{Duration, Instant};

use rusqlite::types::ValueRef;
use rusqlite::Connection;
use serde_json::{json, Value};

use mye_track::attribution::AttributionConfig;
use mye_track::collector::TrackerConfig;
use mye_track::db::now_ms;
use mye_track::hash::sha256_hex;
use mye_track::notify::{poll_once, Cursors};
use mye_track::ops;
use mye_track::ops_record::{self, Caller, RecordEnv, DEFAULT_FLUSH_BUDGET};
use mye_track::protocol::{code, Args, OpError, Role};
use mye_track::scene_overlay;
use mye_track::startup::{open_project, ServiceState};
use mye_track::store::Paths;
use mye_track::watch::{Phase, Tracker};

const WAIT_LIMIT: Duration = Duration::from_secs(60);
const EDITOR: Caller = Caller { role: Role::Editor, pid: None };
const HOOK: Caller = Caller { role: Role::Hook, pid: None };

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
        attribution: AttributionConfig { window_grace_ms: 2000, time_near_ms: 2000 },
        ..TrackerConfig::default()
    }
}

fn args(value: Value) -> Args {
    Args::new(value.as_object().expect("オブジェクト").clone())
}

fn fixture(name: &str) -> String {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("..").join("tests").join("track").join("fixture").join(name);
    fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn meta(guid: &str) -> String {
    format!("{{\n  \"guid\": \"{guid}\",\n  \"type\": \"scene\",\n  \"version\": 1\n}}")
}

/// 30 行のテキスト。`edits` の (行番号, 新しい中身) を置き換える。離れた行は別の Hunk になる。
fn lines(edits: &[(usize, &str)]) -> String {
    (0..30)
        .map(|i| match edits.iter().find(|(n, _)| *n == i) {
            Some((_, text)) => format!("{text}\n"),
            None => format!("L{i:02}\n"),
        })
        .collect()
}

struct Fx {
    root: PathBuf,
    model: BTreeMap<String, Vec<u8>>,
    tracker: Option<Tracker>,
    env: Option<RecordEnv>,
}

impl Fx {
    fn new(name: &str) -> Fx {
        let root = std::env::temp_dir().join(format!("mye_track_rv_{}_{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        Fx { root, model: BTreeMap::new(), tracker: None, env: None }
    }

    fn write(&mut self, rel: &str, bytes: &[u8]) {
        let p = self.root.join(rel);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(&p, bytes).unwrap();
        self.model.insert(rel.to_string(), bytes.to_vec());
    }

    fn start(&mut self) {
        let (store, report) = open_project(&self.root, now_ms());
        assert_eq!(report.state, Some(ServiceState::Recording), "{report:?}");
        let tracker = Tracker::start(&self.root, store.unwrap(), cfg());
        let status = tracker.wait_ready(WAIT_LIMIT);
        assert_eq!(status.phase, Phase::Watching, "{status:?}");
        self.env = Some(RecordEnv {
            root: self.root.clone(),
            db_path: Paths::for_project(&self.root).db,
            writer: Arc::clone(tracker.writer()),
            flush: tracker.flush_handle(),
            editor_connections: Arc::new(AtomicUsize::new(0)),
            queue_limit: 1024,
            flush_budget: DEFAULT_FLUSH_BUDGET,
        });
        self.tracker = Some(tracker);
    }

    fn stop(&mut self) {
        self.env = None;
        if let Some(t) = self.tracker.take() {
            t.stop("test");
        }
    }

    fn db(&self) -> Connection {
        let conn = Connection::open(Paths::for_project(&self.root).db).unwrap();
        conn.busy_timeout(Duration::from_secs(10)).unwrap();
        conn
    }

    fn reader(&self) -> Connection {
        ops::open_reader(&Paths::for_project(&self.root).db).unwrap()
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

    /// ファイルを書き換え、そのパスの Revision が `n` 件になるまで待つ。最新の `revisionId` を返す。
    fn change(&mut self, rel: &str, bytes: &[u8], n: i64) -> i64 {
        self.write(rel, bytes);
        wait_until(&format!("{rel} の Revision {n} 件"), || self.revision_count(rel) >= n);
        self.last(rel)
    }

    fn last(&self, path: &str) -> i64 {
        self.db()
            .query_row(
                "SELECT MAX(r.revision_id) FROM change_revisions r JOIN changes c ON c.change_id = r.change_id WHERE c.path = ?1",
                [path],
                |r| r.get(0),
            )
            .unwrap()
    }

    fn review(&self, targets: Value, state: &str) -> Result<Value, OpError> {
        ops_record::review_set(self.env.as_ref().expect("start 済み"), &EDITOR, args(json!({ "targets": targets, "state": state })))
    }

    fn review_ok(&self, targets: Value, state: &str) -> Value {
        self.review(targets.clone(), state).unwrap_or_else(|e| panic!("review_set {targets} {state}: {} {}", e.code, e.detail))
    }

    /// Revision 全体へ。
    fn review_revision(&self, revision_id: i64, state: &str) -> Value {
        self.review_ok(json!([{ "revisionId": revision_id, "scope": { "kind": "revision" } }]), state)
    }

    fn review_hunk(&self, revision_id: i64, hunk_id: i64, state: &str) -> Value {
        self.review_ok(json!([{ "revisionId": revision_id, "scope": { "kind": "hunk", "ref": hunk_id.to_string() } }]), state)
    }

    fn describe(&self, revision_id: i64) -> Value {
        ops::describe(&self.reader(), args(json!({ "revisionId": revision_id }))).unwrap()
    }

    fn hunk_ids(&self, revision_id: i64) -> Vec<i64> {
        let conn = self.db();
        let mut stmt = conn.prepare("SELECT hunk_id FROM change_hunks WHERE revision_id = ?1 ORDER BY ordinal").unwrap();
        let rows = stmt.query_map([revision_id], |r| r.get(0)).unwrap();
        rows.map(Result::unwrap).collect()
    }

    /// 葉ごとの状態 (`describe` の `reviewLeaves` の並びのまま)。
    fn leaf_states(&self, revision_id: i64) -> Vec<String> {
        self.describe(revision_id)["reviewLeaves"]
            .as_array()
            .unwrap()
            .iter()
            .map(|l| l["state"].as_str().unwrap().to_string())
            .collect()
    }

    fn review_state(&self, revision_id: i64) -> Value {
        self.describe(revision_id)["review"].clone()
    }

    fn review_rows(&self) -> Vec<String> {
        dump(&self.db(), "SELECT * FROM reviews ORDER BY review_id")
    }
}

impl Drop for Fx {
    fn drop(&mut self) {
        self.stop();
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

/// 2 Hunk の変更を 1 回記録した text ファイルの Revision (最初の変更)。
fn text_fixture(fx: &mut Fx) -> i64 {
    fx.write("t.txt", lines(&[]).as_bytes());
    fx.start();
    fx.change("t.txt", lines(&[(2, "X02"), (25, "X25")]).as_bytes(), 1)
}

// ---- 受け入れ条件 1: 別の Revision へ引き継がれない -----------------------------------------------

#[test]
fn an_approval_stays_with_its_revision_and_a_later_revision_starts_pending() {
    let mut fx = Fx::new("carry");
    let first = text_fixture(&mut fx);
    let first_hunks = fx.hunk_ids(first);
    assert_eq!(first_hunks.len(), 2);
    fx.review_revision(first, "approved");
    assert_eq!(fx.review_state(first), "approved");
    let rows_before = fx.review_rows();

    let second = fx.change("t.txt", lines(&[(2, "X02"), (25, "X25"), (12, "Y12")]).as_bytes(), 2);
    assert_ne!(first, second);
    assert_eq!(fx.review_state(second), "pending", "新しい Revision は Pending から");
    assert_eq!(fx.leaf_states(second), vec!["pending"]);
    assert_eq!(fx.review_state(first), "approved", "前の Revision の承認は動かない");
    assert_eq!(fx.review_rows(), rows_before, "後の変更で reviews は変わらない");

    // 前の Revision の Hunk ID で後の Revision へ記録しようとしても作らない
    let e = fx.review(json!([{ "revisionId": second, "scope": { "kind": "hunk", "ref": first_hunks[0].to_string() } }]), "approved").unwrap_err();
    assert_eq!(e.code, code::NOT_FOUND, "{e:?}");
    assert_eq!(fx.review_rows(), rows_before, "失敗した要求で行が増えていない");
    // 存在しない Revision
    let e = fx.review(json!([{ "revisionId": 999_999, "scope": { "kind": "revision" } }]), "approved").unwrap_err();
    assert_eq!(e.code, code::NOT_FOUND);
}

// ---- 受け入れ条件 2: 追記だけ ---------------------------------------------------------------

#[test]
fn reviews_are_append_only_and_reopen_returns_to_pending() {
    let mut fx = Fx::new("append");
    let revision = text_fixture(&mut fx);
    let hunk = fx.hunk_ids(revision)[0];

    fx.review_hunk(revision, hunk, "approved");
    let after_one = fx.review_rows();
    assert_eq!(after_one.len(), 1);
    fx.review_hunk(revision, hunk, "reopened");
    let after_two = fx.review_rows();
    assert_eq!(after_two.len(), 2);
    assert_eq!(fx.leaf_states(revision), vec!["pending", "pending"], "再開の直後は pending");
    fx.review_hunk(revision, hunk, "rejected");
    let after_three = fx.review_rows();
    assert_eq!(after_three.len(), 3);
    // 前の行のどの列も変わっていない
    assert_eq!(after_two[0], after_one[0]);
    assert_eq!(&after_three[..2], &after_two[..]);
    assert_eq!(fx.leaf_states(revision), vec!["rejected", "pending"]);
    // 履歴は古い順
    let history = fx.describe(revision)["reviewHistory"].clone();
    let states: Vec<&str> = history.as_array().unwrap().iter().map(|h| h["state"].as_str().unwrap()).collect();
    assert_eq!(states, vec!["approved", "reopened", "rejected"]);
}

// ---- 受け入れ条件 3: 今の状態は新しさで決まる -------------------------------------------------

#[test]
fn the_newest_row_decides_and_the_revision_aggregates_from_its_leaves() {
    let mut fx = Fx::new("newest");
    let revision = text_fixture(&mut fx);
    let hunks = fx.hunk_ids(revision);

    fx.review_revision(revision, "approved");
    fx.review_hunk(revision, hunks[0], "rejected");
    assert_eq!(fx.leaf_states(revision), vec!["rejected", "approved"]);
    assert_eq!(fx.review_state(revision), "partially_approved");
    let counts = fx.describe(revision)["reviewCounts"].clone();
    assert_eq!((counts["approved"].as_u64(), counts["rejected"].as_u64()), (Some(1), Some(1)));
    // もう一度 Revision 全体を承認すると全部 approved (範囲の広さでなく新しさ)
    fx.review_revision(revision, "approved");
    assert_eq!(fx.leaf_states(revision), vec!["approved", "approved"]);
    assert_eq!(fx.review_state(revision), "approved");
    // 要確認が混ざると needs_review
    fx.review_hunk(revision, hunks[1], "needs_review");
    assert_eq!(fx.review_state(revision), "needs_review");
    // hunks の各要素にも今の状態が付く
    let listed = ops::hunks(&fx.reader(), args(json!({ "revisionId": revision }))).unwrap();
    let per_hunk: Vec<&str> = listed["items"].as_array().unwrap().iter().map(|h| h["review"].as_str().unwrap()).collect();
    assert_eq!(per_hunk, vec!["approved", "needs_review"]);
}

// ---- 受け入れ条件 4: 葉の無い Revision、対象外の行為者 -----------------------------------------

#[test]
fn a_binary_revision_is_its_own_leaf_and_hunk_scopes_do_not_apply() {
    let mut fx = Fx::new("binary");
    fx.write("b.bin", &[0xffu8, 0xfe, 2, 3, 0, 5]);
    fx.start();
    let revision = fx.change("b.bin", &[0xffu8, 0xfe, 9, 9, 0, 5, 6], 1);
    assert!(fx.hunk_ids(revision).is_empty());
    assert_eq!(fx.leaf_states(revision), vec!["pending"]);
    let e = fx.review(json!([{ "revisionId": revision, "scope": { "kind": "hunk", "ref": "1" } }]), "approved").unwrap_err();
    assert_eq!(e.code, code::NOT_FOUND);
    let e = fx.review(json!([{ "revisionId": revision, "scope": { "kind": "entity", "ref": "1" } }]), "approved").unwrap_err();
    assert_eq!(e.code, code::NOT_FOUND, "構造の変更の無い Revision に範囲は無い");
    fx.review_revision(revision, "approved");
    assert_eq!(fx.review_state(revision), "approved");
    assert_eq!(fx.describe(revision)["reviewLeaves"][0]["kind"], "revision");
}

#[test]
fn editor_user_revisions_are_not_reviewable_and_not_counted() {
    let mut fx = Fx::new("actor");
    fx.write("e.txt", b"one\n");
    fx.write("o.txt", b"other\n");
    fx.start();
    let ai_like = fx.change("o.txt", b"other changed\n", 1);
    // Editor の保存 = 行為者 editor_user
    fx.write("e.txt", b"two\n");
    let notice = ops_record::editor_saved(
        fx.env.as_ref().unwrap(),
        &EDITOR,
        args(json!({ "path": "e.txt", "hash": sha256_hex(b"two\n") })),
    );
    assert!(notice.is_ok(), "{notice:?}");
    wait_until("e.txt の Revision", || fx.revision_count("e.txt") >= 1);
    let editor_revision = fx.last("e.txt");
    wait_until("editor_user の結論", || {
        fx.db()
            .query_row("SELECT actor FROM change_revisions WHERE revision_id = ?1", [editor_revision], |r| r.get::<_, String>(0))
            .unwrap()
            == "editor_user"
    });
    let e = fx.review(json!([{ "revisionId": editor_revision, "scope": { "kind": "revision" } }]), "approved").unwrap_err();
    assert_eq!(e.code, code::BAD_ARGS, "{e:?}");
    let described = fx.describe(editor_revision);
    assert_eq!((described["reviewable"].clone(), described["review"].clone()), (json!(false), Value::Null));
    // review フィルタに出ない
    let pending = ops::list(&fx.reader(), args(json!({ "review": "pending" }))).unwrap();
    let ids: Vec<i64> = pending["items"].as_array().unwrap().iter().map(|i| i["revisionId"].as_i64().unwrap()).collect();
    assert!(ids.contains(&ai_like) && !ids.contains(&editor_revision), "{ids:?}");
    assert_eq!(
        ops::list(&fx.reader(), args(json!({ "path": "e.txt" }))).unwrap()["items"][0]["review"],
        Value::Null
    );
}

// ---- 受け入れ条件 5: 承認・却下は何も変えない -------------------------------------------------

#[test]
fn reviewing_changes_nothing_but_the_reviews_table() {
    let mut fx = Fx::new("nothing");
    // 擬似の git の索引 (中身は何でもよい。バイト列が変わらないことだけを見る)
    fx.write(".git/HEAD", b"ref: refs/heads/main\n");
    fx.write(".git/index", &[b'D', b'I', b'R', b'C', 0, 0, 0, 2, 7, 7, 7]);
    let revision = text_fixture(&mut fx);
    let hunks = fx.hunk_ids(revision);

    let snapshot = |fx: &Fx| {
        let conn = fx.db();
        let mut tables = Vec::new();
        for table in ["changes", "change_revisions", "change_hunks", "scene_changes", "snapshots", "restore_operations"] {
            tables.push(dump(&conn, &format!("SELECT * FROM {table} ORDER BY 1")));
        }
        (tree(&fx.root), tables)
    };
    for state in ["approved", "rejected", "needs_review", "reopened"] {
        let before = snapshot(&fx);
        fx.review_hunk(revision, hunks[0], state);
        fx.review_revision(revision, state);
        let after = snapshot(&fx);
        assert_eq!(before.0, after.0, "{state}: 作業ツリーのパスとハッシュ");
        assert_eq!(before.1, after.1, "{state}: 復元の状態や Revision の表が変わった");
    }
    assert_eq!(fs::read(fx.root.join(".git").join("index")).unwrap(), vec![b'D', b'I', b'R', b'C', 0, 0, 0, 2, 7, 7, 7]);
    // 却下は変更を消さない: ファイルは書き換え後のまま
    assert_eq!(fs::read_to_string(fx.root.join("t.txt")).unwrap(), lines(&[(2, "X02"), (25, "X25")]));
}

// ---- 受け入れ条件 6: 役割 --------------------------------------------------------------------

#[test]
fn only_the_editor_role_may_call_review_set() {
    use mye_track::protocol::{role_allows, IMPLEMENTED_OPS};
    assert!(IMPLEMENTED_OPS.contains(&"review_set"));
    assert!(role_allows(Role::Editor, "review_set"));
    assert!(!role_allows(Role::Mcp, "review_set") && !role_allows(Role::Hook, "review_set"));
}

// ---- 範囲の検査と引数 -------------------------------------------------------------------------

#[test]
fn bad_arguments_are_rejected_and_leave_no_rows() {
    let mut fx = Fx::new("badargs");
    let revision = text_fixture(&mut fx);
    let hunk = fx.hunk_ids(revision)[0].to_string();
    let ok_target = json!({ "revisionId": revision, "scope": { "kind": "hunk", "ref": hunk } });
    let env = fx.env.as_ref().unwrap();
    let call = |a: Value| ops_record::review_set(env, &HOOK, args(a)).unwrap_err().code;

    assert_eq!(call(json!({ "targets": [ok_target], "state": "pending" })), code::BAD_ARGS, "pending は指定できない");
    assert_eq!(call(json!({ "targets": [ok_target], "state": "nope" })), code::BAD_ARGS);
    assert_eq!(call(json!({ "targets": [], "state": "approved" })), code::BAD_ARGS);
    assert_eq!(call(json!({ "state": "approved" })), code::BAD_ARGS);
    assert_eq!(call(json!({ "targets": [ok_target], "state": "approved", "extra": 1 })), code::BAD_ARGS);
    assert_eq!(call(json!({ "targets": [ok_target], "state": "approved", "note": "x".repeat(4001) })), code::BAD_ARGS);
    assert_eq!(call(json!({ "targets": [{ "revisionId": revision, "scope": { "kind": "revision", "ref": "1" } }], "state": "approved" })), code::BAD_ARGS);
    assert_eq!(call(json!({ "targets": [{ "revisionId": revision, "scope": { "kind": "hunk", "ref": "007" } }], "state": "approved" })), code::BAD_ARGS);
    assert_eq!(call(json!({ "targets": [{ "revisionId": revision, "scope": { "kind": "weird" } }], "state": "approved" })), code::BAD_ARGS);
    let many: Vec<Value> = (0..1001).map(|_| json!({ "revisionId": revision, "scope": { "kind": "revision" } })).collect();
    assert_eq!(call(json!({ "targets": many, "state": "approved" })), code::BAD_ARGS, "1000 件まで");
    // 1 件でも不正なら、他の対象も記録しない
    let mixed = json!([{ "revisionId": revision, "scope": { "kind": "revision" } }, { "revisionId": revision, "scope": { "kind": "hunk", "ref": "999999" } }]);
    assert_eq!(call(json!({ "targets": mixed, "state": "approved" })), code::NOT_FOUND);
    assert!(fx.review_rows().is_empty());
    // メモは履歴に残る
    ops_record::review_set(fx.env.as_ref().unwrap(), &EDITOR, args(json!({ "targets": [ok_target], "state": "needs_review", "note": "あとで見る" }))).unwrap();
    assert_eq!(fx.describe(revision)["reviewHistory"][0]["note"], "あとで見る");
}

#[test]
fn one_call_with_many_targets_is_one_event() {
    let mut fx = Fx::new("batch");
    let revision = text_fixture(&mut fx);
    let hunks = fx.hunk_ids(revision);
    let targets: Vec<Value> =
        hunks.iter().map(|h| json!({ "revisionId": revision, "scope": { "kind": "hunk", "ref": h.to_string() } })).collect();
    let done = fx.review_ok(json!(targets), "approved");
    assert_eq!(done["recorded"], 2);
    let events: i64 = fx.db().query_row("SELECT COUNT(*) FROM events WHERE kind = 'review_set'", [], |r| r.get(0)).unwrap();
    assert_eq!(events, 1);
    assert_eq!(fx.review_rows().len(), 2);
    assert_eq!(fx.review_state(revision), "approved");
}

// ---- 取得: list の review フィルタ、sessions の集約、通知 ------------------------------------------

#[test]
fn list_filters_by_the_aggregate_and_sessions_carry_their_aggregate() {
    let mut fx = Fx::new("list");
    fx.write("a.txt", lines(&[]).as_bytes());
    fx.write("b.txt", lines(&[]).as_bytes());
    fx.write("c.txt", lines(&[]).as_bytes());
    fx.start();
    let a = fx.change("a.txt", lines(&[(2, "X02"), (25, "X25")]).as_bytes(), 1);
    let b = fx.change("b.txt", lines(&[(2, "X02")]).as_bytes(), 1);
    let c = fx.change("c.txt", lines(&[(2, "X02")]).as_bytes(), 1);
    fx.review_hunk(a, fx.hunk_ids(a)[0], "approved");
    fx.review_revision(b, "approved");
    fx.review_revision(c, "rejected");

    let ids = |review: &str| -> Vec<i64> {
        let list = ops::list(&fx.reader(), args(json!({ "review": review }))).unwrap();
        let mut ids: Vec<i64> = list["items"].as_array().unwrap().iter().map(|i| i["revisionId"].as_i64().unwrap()).collect();
        ids.sort();
        ids
    };
    assert_eq!(ids("partially_approved"), vec![a]);
    assert_eq!(ids("approved"), vec![b]);
    assert_eq!(ids("rejected"), vec![c]);
    assert!(ids("pending").iter().all(|id| ![a, b, c].contains(id)));
    assert!(ops::list(&fx.reader(), args(json!({ "review": "reopened" }))).is_err(), "集約の状態以外は受けない");
    // ページ送り: 1 件ずつ絞り込みながら全部取れる
    let first = ops::list(&fx.reader(), args(json!({ "review": "approved", "limit": 1 }))).unwrap();
    assert_eq!(first["items"].as_array().unwrap().len(), 1);

    // セッション: 宣言つきの AI の変更を承認すると、セッションの集約に出る
    let session = ops_record::session_begin(
        fx.env.as_ref().unwrap(),
        &HOOK,
        args(json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "c-rv" })),
    )
    .unwrap()["sessionId"]
        .as_str()
        .unwrap()
        .to_string();
    ops_record::op_begin(
        fx.env.as_ref().unwrap(),
        &HOOK,
        args(json!({ "clientSessionId": "c-rv", "operationType": "edit", "targets": ["d.txt"], "clientOpId": "t1",
                     "declaredEdit": { "kind": "write", "content": "new file\n" } })),
    )
    .unwrap();
    fx.write("d.txt", b"new file\n");
    ops_record::op_end(fx.env.as_ref().unwrap(), &HOOK, args(json!({ "clientSessionId": "c-rv", "clientOpId": "t1", "result": "ok" }))).unwrap();
    wait_until("d.txt の Revision", || fx.revision_count("d.txt") >= 1);
    let d = fx.last("d.txt");
    let listed = ops::sessions(&fx.reader(), args(json!({}))).unwrap();
    let item = listed["items"].as_array().unwrap().iter().find(|s| s["sessionId"] == session.as_str()).unwrap().clone();
    assert_eq!(item["review"]["state"], "pending", "{item}");
    assert_eq!(item["review"]["counts"]["pending"], 1);
    fx.review_revision(d, "approved");
    let listed = ops::sessions(&fx.reader(), args(json!({}))).unwrap();
    let item = listed["items"].as_array().unwrap().iter().find(|s| s["sessionId"] == session.as_str()).unwrap().clone();
    assert_eq!(item["review"]["state"], "approved", "{item}");
    assert_eq!(item["review"]["counts"]["approved"], 1);
    // 操作・変更の集約は describe に付く
    let described = fx.describe(d);
    assert_eq!(described["changeReview"]["state"], "approved");
    assert_eq!(described["operationReview"]["state"], "approved");
    // 操作の Revision だけを引ける (Editor の「操作全体をまとめてレビュー」の対象集め)。別の操作・操作の無い変更は入らない
    let operation_id = described["operationId"].as_str().expect("宣言つきの変更は操作に属する").to_string();
    let of_operation = ops::list(&fx.reader(), args(json!({ "operationId": operation_id }))).unwrap();
    let operation_revisions: Vec<i64> = of_operation["items"].as_array().unwrap().iter().map(|i| i["revisionId"].as_i64().unwrap()).collect();
    assert_eq!(operation_revisions, vec![d], "operationId で絞ると、その操作の Revision だけ");
    let none = ops::list(&fx.reader(), args(json!({ "operationId": "00000000-0000-4000-8000-000000000000" }))).unwrap();
    assert!(none["items"].as_array().unwrap().is_empty(), "知らない操作は 0 件");
}

#[test]
fn review_changed_names_the_revisions_once_per_poll() {
    let mut fx = Fx::new("notify");
    let revision = text_fixture(&mut fx);
    let reader = fx.reader();
    let mut cursors = Cursors::capture(&reader).unwrap();
    assert!(poll_once(&reader, &mut cursors).unwrap().is_empty());
    let hunks = fx.hunk_ids(revision);
    fx.review_hunk(revision, hunks[0], "approved");
    fx.review_hunk(revision, hunks[1], "rejected");
    let events = poll_once(&reader, &mut cursors).unwrap();
    let reviews: Vec<&Value> = events.iter().filter(|e| e["event"] == "review_changed").collect();
    assert_eq!(reviews.len(), 1, "{events:?}");
    assert_eq!(reviews[0]["revisionIds"], json!([revision]));
    assert!(poll_once(&reader, &mut cursors).unwrap().is_empty(), "同じ記録を 2 回知らせない");
    assert!(mye_track::protocol::EVENT_NAMES.contains(&"review_changed"));
}

// ---- シーン: 範囲、文書の区別、強調 -------------------------------------------------------------

fn two_scene_documents(fx: &mut Fx) -> (i64, i64) {
    let small = fixture("scene_small.scene.json");
    let edited = small.replacen("\"nearZ\": 0.10000000149011612", "\"nearZ\": 0.2", 1).replacen("6.0,\n            -14.0", "7.5,\n            -14.0", 1);
    assert_ne!(small, edited);
    fx.write("a.scene.json.meta", meta("aaaaaaaaaaaaaaaa").as_bytes());
    fx.write("a.scene.json", small.as_bytes());
    fx.write("b.scene.json.meta", meta("bbbbbbbbbbbbbbbb").as_bytes());
    fx.write("b.scene.json", small.as_bytes());
    fx.start();
    let a = fx.change("a.scene.json", edited.as_bytes(), 1);
    let b = fx.change("b.scene.json", edited.as_bytes(), 1);
    (a, b)
}

/// Revision の構造の変更の (scene_change_id, fileId, component, field)。
fn scene_rows(fx: &Fx, revision: i64) -> Vec<(i64, Option<i64>, Option<String>, Option<String>)> {
    let conn = fx.db();
    let mut stmt = conn
        .prepare("SELECT scene_change_id, file_id, component, field FROM scene_changes WHERE revision_id = ?1 ORDER BY scene_change_id")
        .unwrap();
    let rows = stmt.query_map([revision], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?))).unwrap();
    rows.map(Result::unwrap).collect()
}

#[test]
fn scene_scopes_are_per_revision_and_nest_entity_component_field() {
    let mut fx = Fx::new("scene");
    let (a, b) = two_scene_documents(&mut fx);
    let rows = scene_rows(&fx, a);
    assert!(rows.len() >= 2, "{rows:?}");
    assert_eq!(scene_rows(&fx, b).iter().map(|r| (r.1, r.2.clone(), r.3.clone())).collect::<Vec<_>>(), rows.iter().map(|r| (r.1, r.2.clone(), r.3.clone())).collect::<Vec<_>>(), "同じ編集");
    let target = rows.iter().find(|r| r.2.is_some() && r.3.is_some() && r.1.is_some()).expect("フィールドの変更");
    let (file_id, component, field) = (target.1.unwrap(), target.2.clone().unwrap(), target.3.clone().unwrap());
    let field_ref = format!("{file_id}/{component}/{field}");
    let leaf_index = rows.iter().position(|r| r.0 == target.0).unwrap();

    // Revision 全体を承認 → 1 つのフィールドを却下 → そのフィールドだけ rejected
    fx.review_revision(a, "approved");
    fx.review_ok(json!([{ "revisionId": a, "scope": { "kind": "field", "ref": field_ref } }]), "rejected");
    let states = fx.leaf_states(a);
    assert_eq!(states[leaf_index], "rejected");
    assert!(states.iter().enumerate().all(|(i, s)| i == leaf_index || s == "approved"), "{states:?}");
    assert_eq!(fx.review_state(a), "partially_approved");
    // その後 Entity を承認 → Entity の中は全部 approved (新しさで決まる)
    fx.review_ok(json!([{ "revisionId": a, "scope": { "kind": "entity", "ref": file_id.to_string() } }]), "approved");
    assert_eq!(fx.leaf_states(a)[leaf_index], "approved");
    // Component の範囲もそのコンポーネントのフィールドへ効く
    fx.review_ok(json!([{ "revisionId": a, "scope": { "kind": "component", "ref": format!("{file_id}/{component}") } }]), "needs_review");
    assert_eq!(fx.leaf_states(a)[leaf_index], "needs_review");
    assert_eq!(fx.review_state(a), "needs_review");
    // scene_diff の各要素にも今の状態
    let diff = ops::scene_diff(&fx.reader(), args(json!({ "revisionId": a }))).unwrap();
    let item = diff["items"].as_array().unwrap().iter().find(|i| i["sceneChangeId"] == target.0).unwrap().clone();
    assert_eq!(item["review"], "needs_review");

    // 別のシーン (別の Revision) の同じ fileId・同じフィールドには効かない
    assert_eq!(fx.review_state(b), "pending");
    assert!(fx.leaf_states(b).iter().all(|s| s == "pending"));
    fx.review_ok(json!([{ "revisionId": b, "scope": { "kind": "field", "ref": field_ref } }]), "approved");
    assert_eq!(fx.leaf_states(a)[leaf_index], "needs_review", "b への記録が a を動かした");
    assert_eq!(fx.leaf_states(b)[leaf_index], "approved");
    // 存在しない範囲、Hunk (構造の変更を持つ Revision の葉ではない) は記録を作らない
    let rows_before = fx.review_rows();
    let e = fx.review(json!([{ "revisionId": b, "scope": { "kind": "entity", "ref": "987654" } }]), "approved").unwrap_err();
    assert_eq!(e.code, code::NOT_FOUND);
    let e = fx.review(json!([{ "revisionId": b, "scope": { "kind": "field", "ref": format!("{file_id}/{component}/noSuchField") } }]), "approved").unwrap_err();
    assert_eq!(e.code, code::NOT_FOUND);
    let hunk = fx.hunk_ids(b).first().copied().unwrap_or(1);
    let e = fx.review(json!([{ "revisionId": b, "scope": { "kind": "hunk", "ref": hunk.to_string() } }]), "approved").unwrap_err();
    assert!(e.code == code::BAD_ARGS || e.code == code::NOT_FOUND, "{e:?}");
    assert_eq!(fx.review_rows(), rows_before);
}

#[test]
fn scene_overlay_drops_approved_changes_but_keeps_rejected_and_needs_review() {
    let mut fx = Fx::new("overlay");
    let (a, _) = two_scene_documents(&mut fx);
    let document: String = fx
        .db()
        .query_row("SELECT document_id FROM scene_changes WHERE revision_id = ?1 LIMIT 1", [a], |r| r.get(0))
        .unwrap();
    let overlay = |extra: Value| -> Value {
        let mut a = json!({ "documentId": document });
        for (k, v) in extra.as_object().unwrap() {
            a[k] = v.clone();
        }
        scene_overlay::scene_overlay(&fx.reader(), args(a)).unwrap()
    };
    let entity_count = |v: &Value| v["entities"].as_array().unwrap().len() + v["documentChanges"].as_array().map_or(0, Vec::len);
    let all = entity_count(&overlay(json!({})));
    assert!(all > 0);

    fx.review_revision(a, "approved");
    let hidden = overlay(json!({}));
    assert_eq!(entity_count(&hidden), 0, "承認済みは強調から外れる: {hidden}");
    assert!(hidden["approvedHidden"].as_u64().unwrap() > 0);
    assert_eq!(entity_count(&overlay(json!({ "excludeApproved": false }))), all, "引数で承認済みも返せる");

    fx.review_revision(a, "rejected");
    assert_eq!(entity_count(&overlay(json!({}))), all, "却下は強調したまま");
    fx.review_revision(a, "needs_review");
    assert_eq!(entity_count(&overlay(json!({}))), all, "要確認は強調したまま");
    fx.review_revision(a, "approved");
    fx.review_revision(a, "reopened");
    assert_eq!(entity_count(&overlay(json!({}))), all, "再開すれば強調が戻る");
}

// ---- 受け入れ条件 9: 作り直し -------------------------------------------------------------------

#[test]
fn a_broken_db_is_rebuilt_to_the_same_reviews_and_states() {
    let mut fx = Fx::new("rebuild");
    fx.write("t.txt", lines(&[]).as_bytes());
    let (a, b) = two_scene_documents(&mut fx);
    let t = fx.change("t.txt", lines(&[(2, "X02"), (25, "X25")]).as_bytes(), 1);
    let hunks = fx.hunk_ids(t);
    fx.review_revision(a, "approved");
    fx.review_ok(json!([{ "revisionId": b, "scope": { "kind": "revision" } }, { "revisionId": t, "scope": { "kind": "hunk", "ref": hunks[0].to_string() } }]), "rejected");
    fx.review_hunk(t, hunks[1], "needs_review");
    ops_record::review_set(
        fx.env.as_ref().unwrap(),
        &EDITOR,
        args(json!({ "targets": [{ "revisionId": b, "scope": { "kind": "revision" } }], "state": "reopened", "note": "メモ" })),
    )
    .unwrap();
    let states = |fx: &Fx| -> Vec<(Value, Vec<String>)> { [a, b, t].iter().map(|r| (fx.review_state(*r), fx.leaf_states(*r))).collect() };
    let rows_before = fx.review_rows();
    let states_before = states(&fx);
    assert_eq!(rows_before.len(), 5);
    fx.stop();

    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert_eq!(report.state, Some(ServiceState::Recording), "{report:?}");
    assert!(report.broken_db.is_some() && report.rebuilt.is_some(), "{report:?}");
    drop(store);

    assert_eq!(fx.review_rows(), rows_before, "reviews が壊す前と違う");
    assert_eq!(states(&fx), states_before);
    let distinct: HashSet<String> = fx.review_rows().into_iter().collect();
    assert_eq!(distinct.len(), 5);
}
