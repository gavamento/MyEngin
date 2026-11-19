//! セッション・操作・帰属の結合テスト (M104g)。Tracker と Writer を同じプロセスで動かし、記録の op を直接呼ぶ
//! (`ops_record` の関数。pipe の入口と同じ検査を通る)。pipe 越しでしか確かめられないもの (接続元の PID、
//! 接続が無くても終わらない条件) だけ `mye_track.exe serve` を起動する。
//!
//! 待ちは固定の sleep ではなく上限付きの待ち合わせ。全テストの後処理で、テスト自身が書いた分を除いて
//! 作業ツリー (`.mye` を除く) が変わっていないことを検査する (spec §2 D17)。

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use rusqlite::types::ValueRef;
use rusqlite::Connection;
use serde_json::{json, Value};

use mye_track::client::{read_service_json, Client};
use mye_track::collector::TrackerConfig;
use mye_track::db::now_ms;
use mye_track::hash::sha256_hex;
use mye_track::ops;
use mye_track::ops_record::{self, normalize_path, Caller, RecordEnv, DEFAULT_FLUSH_BUDGET};
use mye_track::pipe::pipe_name;
use mye_track::process::{is_alive, process_start_ms};
use mye_track::project::project_id;
use mye_track::protocol::{code, role_allows, Args, OpError, Role};
use mye_track::session::{self, LifecycleConfig};
use mye_track::startup::{open_project, ServiceState};
use mye_track::store::Paths;
use mye_track::watch::{Phase, Tracker};
use mye_track::attribution::AttributionConfig;

const WAIT_LIMIT: Duration = Duration::from_secs(60);

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

/// 監視が速く反応する設定。`near_ms` は `time_near` の幅。
fn cfg(near_ms: i64) -> TrackerConfig {
    TrackerConfig {
        debounce: Duration::from_millis(150),
        max_hold: Duration::from_secs(1),
        busy_retry: Duration::from_millis(50),
        settle_wait: Duration::from_millis(100),
        settle_give_up: Duration::from_secs(10),
        verify_quiet: Duration::from_millis(250),
        verify_interval: Duration::from_millis(500),
        unrecorded_retry: Duration::from_millis(100),
        attribution: AttributionConfig { window_grace_ms: 2000, time_near_ms: near_ms },
        ..TrackerConfig::default()
    }
}

/// 監視がデバウンスで待つ間に、管理された操作の読み取りが先に走る設定 (人の変更が未記録のまま残る)。
fn slow_watch_cfg() -> TrackerConfig {
    TrackerConfig { debounce: Duration::from_secs(20), max_hold: Duration::from_secs(40), ..cfg(2000) }
}

const HOOK: Caller = Caller { role: Role::Hook, pid: None };
const EDITOR: Caller = Caller { role: Role::Editor, pid: None };

fn mcp() -> Caller {
    Caller { role: Role::Mcp, pid: Some(std::process::id()) }
}

fn args(value: Value) -> Args {
    Args::new(value.as_object().expect("オブジェクト").clone())
}

/// テストごとの作業ツリーと、その上で動く Tracker。
struct Fx {
    root: PathBuf,
    model: BTreeMap<String, Vec<u8>>,
    tracker: Option<Tracker>,
    env: Option<RecordEnv>,
    editors: Arc<AtomicUsize>,
}

impl Fx {
    fn new(name: &str) -> Fx {
        let root = std::env::temp_dir().join(format!("mye_track_s_{}_{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        Fx { root, model: BTreeMap::new(), tracker: None, env: None, editors: Arc::new(AtomicUsize::new(0)) }
    }

    fn write(&mut self, rel: &str, bytes: &[u8]) {
        let p = self.root.join(rel);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(&p, bytes).unwrap();
        self.model.insert(rel.to_string(), bytes.to_vec());
    }

    fn start(&mut self, cfg: TrackerConfig) {
        let (store, report) = open_project(&self.root, now_ms());
        assert_eq!(report.state, Some(ServiceState::Recording), "{report:?}");
        let tracker = Tracker::start(&self.root, store.unwrap(), cfg);
        let status = tracker.wait_ready(WAIT_LIMIT);
        assert_eq!(status.phase, Phase::Watching, "{status:?}");
        self.env = Some(RecordEnv {
            root: self.root.clone(),
            db_path: Paths::for_project(&self.root).db,
            writer: Arc::clone(tracker.writer()),
            flush: tracker.flush_handle(),
            editor_connections: Arc::clone(&self.editors),
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

    fn env(&self) -> &RecordEnv {
        self.env.as_ref().expect("start 済み")
    }

    fn db(&self) -> Connection {
        let conn = Connection::open(Paths::for_project(&self.root).db).unwrap();
        conn.busy_timeout(Duration::from_secs(10)).unwrap();
        conn
    }

    fn call(&self, op: &str, caller: Caller, a: Value) -> Result<Value, OpError> {
        let env = self.env();
        match op {
            "session_begin" => ops_record::session_begin(env, &caller, args(a)),
            "session_end" => ops_record::session_end(env, &caller, args(a)),
            "session_turn_end" => ops_record::session_turn_end(env, &caller, args(a)),
            "op_begin" => ops_record::op_begin(env, &caller, args(a)),
            "op_end" => ops_record::op_end(env, &caller, args(a)),
            "report" => ops_record::report(env, &caller, args(a)),
            "editor_saved" => ops_record::editor_saved(env, &caller, args(a)),
            "restore_record" => ops_record::restore_record(env, &caller, args(a)),
            "restore_expect" => ops_record::restore_expect(env, &caller, args(a)),
            "review_set" => ops_record::review_set(env, &caller, args(a)),
            other => panic!("知らない op {other}"),
        }
    }

    fn ok(&self, op: &str, caller: Caller, a: Value) -> Value {
        self.call(op, caller, a.clone()).unwrap_or_else(|e| panic!("{op} {a} が失敗: {} {}", e.code, e.detail))
    }

    /// hook の `session_begin`。`client` が clientSessionId。
    fn begin_hook(&self, client: &str) -> String {
        let r = self.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": client }));
        r["sessionId"].as_str().unwrap().to_string()
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

    /// パスの Revision を新しい順に。
    fn revisions(&self, path: &str) -> Vec<Rev> {
        let conn = self.db();
        let mut stmt = conn
            .prepare(
                "SELECT r.revision_id, r.actor, r.attribution, r.operation_id, r.completeness
                   FROM change_revisions r JOIN changes c ON c.change_id = r.change_id WHERE c.path = ?1 ORDER BY r.revision_id DESC",
            )
            .unwrap();
        let rows = stmt
            .query_map([path], |r| {
                Ok(Rev {
                    id: r.get(0)?,
                    actor: r.get(1)?,
                    attribution: r.get(2)?,
                    operation_id: r.get(3)?,
                    completeness: r.get(4)?,
                })
            })
            .unwrap();
        rows.map(Result::unwrap).collect()
    }

    fn last(&self, path: &str) -> Rev {
        self.revisions(path).into_iter().next().unwrap_or_else(|| panic!("{path} の Revision が無い"))
    }

    fn evidence(&self, revision_id: i64) -> Vec<String> {
        let conn = self.db();
        let mut stmt = conn.prepare("SELECT kind FROM attribution_evidence WHERE revision_id = ?1 ORDER BY evidence_id").unwrap();
        let rows = stmt.query_map([revision_id], |r| r.get::<_, String>(0)).unwrap();
        rows.map(Result::unwrap).collect()
    }

    fn describe(&self, revision_id: i64) -> Value {
        let conn = ops::open_reader(&Paths::for_project(&self.root).db).unwrap();
        ops::describe(&conn, args(json!({ "revisionId": revision_id }))).unwrap()
    }

    fn list_for_session(&self, session_id: &str) -> Vec<String> {
        let conn = ops::open_reader(&Paths::for_project(&self.root).db).unwrap();
        let list = ops::list(&conn, args(json!({ "sessionId": session_id, "limit": 200 }))).unwrap();
        list["items"].as_array().unwrap().iter().map(|i| i["path"].as_str().unwrap().to_string()).collect()
    }

    fn blob(&self, hash: &str) -> Vec<u8> {
        fs::read(Paths::for_project(&self.root).blobs.join(&hash[..2]).join(&hash[2..])).unwrap()
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

#[derive(Debug, Clone)]
struct Rev {
    id: i64,
    actor: String,
    attribution: String,
    operation_id: Option<String>,
    completeness: String,
}

fn verdict(r: &Rev) -> (&str, &str) {
    (r.actor.as_str(), r.attribution.as_str())
}

fn replace_edit(old: &str, new: &str) -> Value {
    json!({ "kind": "replace", "oldString": old, "newString": new, "replaceAll": false })
}

fn write_edit(content: &str) -> Value {
    json!({ "kind": "write", "content": content })
}

/// `op_begin` を送り、`operationId` を返す。
fn begin_op(fx: &Fx, caller: Caller, client: &str, path: &str, declared: Option<Value>, client_op: &str) -> Value {
    let mut a = json!({ "clientSessionId": client, "operationType": "edit", "targets": [path], "clientOpId": client_op });
    if let Some(d) = declared {
        a["declaredEdit"] = d;
    }
    fx.ok("op_begin", caller, a)
}

// ---- 受け入れ条件 2: 人の未コミットの変更を混ぜない -------------------------------------------

#[test]
fn a_change_made_before_the_session_is_not_a_candidate_of_that_session() {
    let mut fx = Fx::new("preexisting");
    fx.write("a.txt", b"A0\n");
    fx.write("b.txt", b"B0\n");
    fx.start(cfg(2000));

    // 人が A を変えて (記録される) から、AI のセッションが始まる
    fx.write("a.txt", b"A1 by a human\n");
    wait_until("a.txt の Revision", || fx.revision_count("a.txt") == 1);
    std::thread::sleep(Duration::from_millis(50));
    let session = fx.begin_hook("c-pre");

    begin_op(&fx, HOOK, "c-pre", "b.txt", Some(write_edit("B1 by the AI\n")), "t1");
    fx.write("b.txt", b"B1 by the AI\n");
    let end = fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-pre", "clientOpId": "t1", "result": "ok" }));
    assert_eq!(end["pending"], false, "{end}");

    let a = fx.last("a.txt");
    assert_eq!(verdict(&a), ("unknown", "unknown"), "セッションより前の変更が AI のものになった");
    assert!(fx.evidence(a.id).contains(&"preexisting".to_string()), "{:?}", fx.evidence(a.id));
    assert_eq!(fx.describe(a.id)["candidateSessions"], json!([]));
    assert_eq!(fx.list_for_session(&session), ["b.txt"], "A の Revision がセッションの絞り込みに出た");
    let done = fx.ok("session_end", HOOK, json!({ "sessionId": session }));
    assert_eq!(done["changeCount"], 1, "{done}");
    assert_eq!(done["status"], "finished");
}

// ---- 受け入れ条件 3、4: 宣言と一致すれば Verified、人が挟まれば Verified にならない ---------------

#[test]
fn a_change_matching_the_declared_edit_is_ai_verified_with_its_evidence() {
    let mut fx = Fx::new("verified");
    fx.write("x.txt", b"alpha\nbeta\ngamma\n");
    fx.start(cfg(2000));
    let session = fx.begin_hook("c-ver");

    let begin = begin_op(&fx, HOOK, "c-ver", "x.txt", Some(replace_edit("beta", "BETA")), "tool-1");
    assert_eq!(begin["targets"][0]["path"], "x.txt");
    assert_eq!(begin["targets"][0]["hash"], sha256_hex(b"alpha\nbeta\ngamma\n"));
    fx.write("x.txt", b"alpha\nBETA\ngamma\n");
    // pre と post は別のプロセス: operationId を持たず、clientSessionId + clientOpId で引く
    let end = fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-ver", "clientOpId": "tool-1", "result": "ok" }));
    assert_eq!(end["pending"], false, "{end}");
    let revisions = end["revisions"].as_array().unwrap();
    assert_eq!(revisions.len(), 1, "{end}");
    assert_eq!((revisions[0]["actor"].as_str(), revisions[0]["attribution"].as_str()), (Some("ai"), Some("verified")), "{end}");
    assert_eq!(revisions[0]["completeness"], "complete");

    let rev = fx.last("x.txt");
    assert_eq!(rev.id, revisions[0]["revisionId"].as_i64().unwrap());
    assert_eq!(rev.operation_id.as_deref(), begin["operationId"].as_str());
    let kinds = fx.evidence(rev.id);
    for expected in ["observed", "op_window", "before_match", "declared_edit_match"] {
        assert!(kinds.iter().any(|k| k == expected), "{expected} が根拠に無い: {kinds:?}");
    }
    let detail = fx.describe(rev.id);
    assert_eq!(detail["candidateSessions"], json!([session]));
    // Before / After が操作の前後とバイト一致
    assert_eq!(fx.blob(detail["before"]["hash"].as_str().unwrap()), b"alpha\nbeta\ngamma\n");
    assert_eq!(fx.blob(detail["after"]["hash"].as_str().unwrap()), b"alpha\nBETA\ngamma\n");
    assert_eq!(rev.completeness, "complete");
    // 宣言は Blob に入り、ジャーナルには内容が無い
    let declared_snapshots: i64 =
        fx.db().query_row("SELECT COUNT(*) FROM snapshots WHERE kind = 'declared_edit'", [], |r| r.get(0)).unwrap();
    assert_eq!(declared_snapshots, 1);
    let journal_text: String = fs::read_dir(Paths::for_project(&fx.root).journal)
        .unwrap()
        .flatten()
        .map(|e| fs::read_to_string(e.path()).unwrap_or_default())
        .collect();
    assert!(!journal_text.contains("BETA"), "宣言の中身がジャーナルに入った");
    // 操作の行も結び付いている
    let observed: String = fx
        .db()
        .query_row("SELECT observed_targets FROM operations WHERE operation_id = ?1", [begin["operationId"].as_str().unwrap()], |r| r.get(0))
        .unwrap();
    assert!(observed.contains("x.txt"), "{observed}");
}

#[test]
fn a_write_that_differs_from_the_declaration_is_claimed_not_verified() {
    let mut fx = Fx::new("claimed");
    fx.write("x.txt", b"alpha\nbeta\n");
    fx.start(slow_watch_cfg());
    fx.begin_hook("c-cl");
    begin_op(&fx, HOOK, "c-cl", "x.txt", Some(replace_edit("beta", "gamma")), "t");
    fx.write("x.txt", b"alpha\nSOMETHING ELSE\n");
    let end = fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-cl", "clientOpId": "t", "result": "ok" }));
    let rev = fx.last("x.txt");
    assert_eq!(verdict(&rev), ("ai", "claimed"), "{end}");
    assert!(!fx.evidence(rev.id).contains(&"declared_edit_match".to_string()));
}

#[test]
fn a_human_change_just_before_op_begin_becomes_its_own_unknown_revision() {
    let mut fx = Fx::new("before_flush");
    fx.write("y.txt", b"y0\n");
    fx.start(slow_watch_cfg());
    fx.begin_hook("c-bf");
    // 人が書いた直後 (監視はまだデバウンス中) に、AI の操作が始まる
    fx.write("y.txt", b"y by a human\n");
    begin_op(&fx, HOOK, "c-bf", "y.txt", Some(write_edit("y by the AI\n")), "t");
    fx.write("y.txt", b"y by the AI\n");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-bf", "clientOpId": "t", "result": "ok" }));

    let revisions = fx.revisions("y.txt");
    assert_eq!(revisions.len(), 2, "{revisions:?}");
    let human = &revisions[1];
    let ai = &revisions[0];
    assert_eq!(verdict(human), ("unknown", "unknown"), "操作の前の人の変更が AI の Revision に混ざった / 帰属された");
    assert_eq!(human.completeness, "complete");
    assert!(!fx.evidence(human.id).contains(&"op_window".to_string()), "人の変更が操作の窓に入った");
    assert_eq!(verdict(ai), ("ai", "verified"));
    // AI の Revision の Before は、人の変更の後の内容
    let detail = fx.describe(ai.id);
    assert_eq!(fx.blob(detail["before"]["hash"].as_str().unwrap()), b"y by a human\n");
}

#[test]
fn a_human_change_after_the_edit_but_before_op_end_breaks_verification() {
    let mut fx = Fx::new("human_after");
    fx.write("z.txt", b"one\ntwo\n");
    fx.start(slow_watch_cfg());
    fx.begin_hook("c-ha");
    begin_op(&fx, HOOK, "c-ha", "z.txt", Some(replace_edit("two", "TWO")), "t");
    fx.write("z.txt", b"one\nTWO\n");
    fx.write("z.txt", b"one\nTWO\nhuman line\n");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-ha", "clientOpId": "t", "result": "ok" }));
    let rev = fx.last("z.txt");
    assert_ne!(rev.attribution, "verified", "After が宣言と違うのに Verified になった");
    assert_eq!(verdict(&rev), ("ai", "claimed"));
}

// ---- spec D46 (sub-28): op_end の ok で閉じない操作の窓は結論に数えない ----------------------------------

fn op_result(fx: &Fx, operation_id: &str) -> Option<String> {
    fx.db().query_row("SELECT result FROM operations WHERE operation_id = ?1", [operation_id], |r| r.get(0)).unwrap()
}

/// 窓の根拠の行が (監査のために) 残っていて、結論には数えられていないこと。
fn assert_audited_but_not_counted(fx: &Fx, rev: &Rev) {
    let kinds = fx.evidence(rev.id);
    assert!(kinds.contains(&"op_window".to_string()) && kinds.contains(&"before_match".to_string()), "{kinds:?}");
    assert_ne!(rev.actor, "ai", "閉じられない操作の窓で人の編集が AI のものになった: {rev:?}");
    assert_eq!(rev.operation_id, None);
}

#[test]
fn a_human_edit_in_the_window_of_an_operation_that_never_ends_is_not_the_ais() {
    let mut fx = Fx::new("d46_a");
    fx.write("h.txt", b"h0\n");
    fx.start(cfg(50));
    let session = fx.begin_hook("c-46a");
    let op = begin_op(&fx, HOOK, "c-46a", "h.txt", Some(replace_edit("h0", "AI")), "t");
    fx.write("h.txt", b"written by a human\n");
    wait_until("h.txt", || fx.revision_count("h.txt") == 1);
    let rev = fx.last("h.txt");
    assert_audited_but_not_counted(&fx, &rev);
    assert!(fx.list_for_session(&session).is_empty(), "セッションの一覧に入った");
    let end = fx.ok("session_end", HOOK, json!({ "sessionId": session }));
    assert_eq!(end["changeCount"], 0);
    // session_end が開いていた操作を閉じ、閉じた後も数えない
    assert_eq!(op_result(&fx, op["operationId"].as_str().unwrap()).as_deref(), Some("abandoned"));
    assert_audited_but_not_counted(&fx, &fx.last("h.txt"));
}

#[test]
fn a_human_edit_after_session_end_is_not_the_ais_even_when_another_operation_finished_ok() {
    let mut fx = Fx::new("d46_b");
    fx.write("h.txt", b"h0\n");
    fx.write("o.txt", b"o0\n");
    fx.start(cfg(2000));
    let session = fx.begin_hook("c-46b");
    begin_op(&fx, HOOK, "c-46b", "h.txt", Some(replace_edit("h0", "AI")), "t1");
    begin_op(&fx, HOOK, "c-46b", "o.txt", Some(replace_edit("o0", "O1")), "t2");
    fx.write("o.txt", b"O1\n");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-46b", "clientOpId": "t2", "result": "ok" }));
    assert_eq!(verdict(&fx.last("o.txt")), ("ai", "verified"));
    let end = fx.ok("session_end", HOOK, json!({ "sessionId": session }));
    assert_eq!(end["changeCount"], 1, "o.txt だけ");
    fx.write("h.txt", b"written by a human after the session\n");
    wait_until("h.txt", || fx.revision_count("h.txt") == 1);
    let rev = fx.last("h.txt");
    assert_audited_but_not_counted(&fx, &rev);
    let again = fx.ok("session_end", HOOK, json!({ "sessionId": session }));
    assert_eq!(again["changeCount"], 1, "終わったセッションの changeCount に人の編集が入った");

    // 壊した DB を作り直しても、同じ結論・同じ根拠
    fx.stop();
    let before = attribution_tables(&fx.db());
    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert!(report.rebuilt.is_some() && report.failed_steps.is_empty(), "{report:?}");
    drop(store);
    let after = attribution_tables(&fx.db());
    for (table, rows) in &before {
        assert_eq!(rows, &after[table], "{table} が壊す前と違う");
    }
}

#[test]
fn an_op_end_that_arrives_after_the_observation_raises_the_change_and_notifies() {
    let mut fx = Fx::new("d46_late");
    fx.write("x.txt", b"alpha\nbeta\n");
    fx.start(cfg(50));
    fx.begin_hook("c-late");
    let op = begin_op(&fx, HOOK, "c-late", "x.txt", Some(replace_edit("beta", "BETA")), "t");
    fx.write("x.txt", b"alpha\nBETA\n");
    wait_until("x.txt", || fx.revision_count("x.txt") == 1);
    // post の前は数えない (宣言どおりでも)
    assert_ne!(fx.last("x.txt").actor, "ai");
    let reader = ops::open_reader(&Paths::for_project(&fx.root).db).unwrap();
    let mut cursors = mye_track::notify::Cursors::capture(&reader).unwrap();
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-late", "clientOpId": "t", "result": "ok" }));
    let rev = fx.last("x.txt");
    assert_eq!(verdict(&rev), ("ai", "verified"));
    assert_eq!(rev.operation_id.as_deref(), op["operationId"].as_str());
    let events = mye_track::notify::poll_once(&reader, &mut cursors).unwrap();
    let updated: Vec<&Value> = events.iter().filter(|e| e["event"] == "attribution_updated").collect();
    assert_eq!(updated.len(), 1, "{events:?}");
    assert_eq!((updated[0]["actor"].as_str(), updated[0]["attribution"].as_str()), (Some("ai"), Some("verified")));
}

#[test]
fn parallel_operations_are_both_verified_when_their_ends_arrive_in_order() {
    let mut fx = Fx::new("d46_par");
    fx.write("a.txt", b"a0\n");
    fx.write("b.txt", b"b0\n");
    fx.start(slow_watch_cfg());
    fx.begin_hook("c-par");
    begin_op(&fx, HOOK, "c-par", "a.txt", Some(replace_edit("a0", "A1")), "A");
    begin_op(&fx, HOOK, "c-par", "b.txt", Some(replace_edit("b0", "B1")), "B");
    fx.write("a.txt", b"A1\n");
    fx.write("b.txt", b"B1\n");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-par", "clientOpId": "A", "result": "ok" }));
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-par", "clientOpId": "B", "result": "ok" }));
    assert_eq!(verdict(&fx.last("a.txt")), ("ai", "verified"));
    assert_eq!(verdict(&fx.last("b.txt")), ("ai", "verified"));
}

#[test]
fn a_failed_or_denied_end_and_a_turn_end_close_the_window_without_counting_it() {
    let mut fx = Fx::new("d46_close");
    for name in ["f.txt", "d.txt", "t.txt", "o.txt"] {
        fx.write(name, b"0\n");
    }
    fx.start(cfg(50));
    fx.begin_hook("c-close");
    fx.begin_hook("c-other");
    let failed = begin_op(&fx, HOOK, "c-close", "f.txt", Some(replace_edit("0", "X")), "f");
    let denied = begin_op(&fx, HOOK, "c-close", "d.txt", Some(replace_edit("0", "X")), "d");
    let turn = begin_op(&fx, HOOK, "c-close", "t.txt", Some(replace_edit("0", "X")), "t");
    let other = begin_op(&fx, HOOK, "c-other", "o.txt", None, "o");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-close", "clientOpId": "f", "result": "failed" }));
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-close", "clientOpId": "d", "result": "denied" }));
    // 別のセッションのターンの終わりでは閉じない。自分のセッションのものだけ閉じる
    let closed = fx.ok("session_turn_end", HOOK, json!({ "clientSessionId": "c-close" }));
    assert_eq!(closed["closed"], 1);
    let none = fx.ok("session_turn_end", HOOK, json!({ "clientSessionId": "c-close" }));
    assert_eq!(none["closed"], 0, "開いている操作が無ければ何もしない");
    let id = |v: &Value| v["operationId"].as_str().unwrap().to_string();
    assert_eq!(op_result(&fx, &id(&failed)).as_deref(), Some("failed"));
    assert_eq!(op_result(&fx, &id(&denied)).as_deref(), Some("denied"));
    assert_eq!(op_result(&fx, &id(&turn)).as_deref(), Some("abandoned"));
    assert_eq!(op_result(&fx, &id(&other)), None, "別のセッションの操作に触れた");

    for name in ["f.txt", "d.txt", "t.txt"] {
        fx.write(name, b"X\n");
        wait_until(name, || fx.revision_count(name) == 1);
        let rev = fx.last(name);
        assert_audited_but_not_counted(&fx, &rev);
    }
    // 別のセッションの開いた操作も、ok で閉じるまでは数えない
    fx.write("o.txt", b"human\n");
    wait_until("o.txt", || fx.revision_count("o.txt") == 1);
    assert_ne!(fx.last("o.txt").actor, "ai");
}

#[test]
fn op_end_rejects_unknown_results_and_the_new_op_is_for_ai_side_roles_only() {
    let mut fx = Fx::new("d46_args");
    fx.write("a.txt", b"a\n");
    fx.start(cfg(50));
    fx.begin_hook("c-args");
    begin_op(&fx, HOOK, "c-args", "a.txt", None, "t");
    let bad = fx.call("op_end", HOOK, json!({ "clientSessionId": "c-args", "clientOpId": "t", "result": "refused" }));
    assert_eq!(bad.unwrap_err().code, code::BAD_ARGS);
    assert!(role_allows(Role::Hook, "session_turn_end") && role_allows(Role::Mcp, "session_turn_end"));
    assert!(!role_allows(Role::Editor, "session_turn_end"));
}

// ---- 受け入れ条件 5: 窓の重なり -----------------------------------------------------------------

#[test]
fn overlapping_operations_on_one_path_are_never_verified() {
    let mut fx = Fx::new("overlap");
    fx.write("x.txt", b"a\nb\nc\n");
    fx.write("w.txt", b"a\nb\nc\n");
    fx.start(slow_watch_cfg());
    let s1 = fx.begin_hook("c-1");
    let s2 = fx.begin_hook("c-2");
    assert_ne!(s1, s2);

    // 一方にだけ宣言が一致する
    let op1 = begin_op(&fx, HOOK, "c-1", "x.txt", Some(replace_edit("b", "B1")), "t1");
    begin_op(&fx, HOOK, "c-2", "x.txt", Some(replace_edit("b", "B2")), "t2");
    fx.write("x.txt", b"a\nB1\nc\n");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-1", "clientOpId": "t1", "result": "ok" }));
    let rev = fx.last("x.txt");
    assert_eq!(verdict(&rev), ("ai", "claimed"), "窓が重なっているのに verified / 絞れていない");
    assert_eq!(rev.operation_id.as_deref(), op1["operationId"].as_str());
    assert!(fx.evidence(rev.id).contains(&"overlap".to_string()));
    assert_eq!(fx.describe(rev.id)["candidateSessions"], json!([s1]));
    assert_eq!(fx.list_for_session(&s1), ["x.txt"]);
    assert!(fx.list_for_session(&s2).is_empty(), "候補から外れたセッションの絞り込みに出た");

    // どちらにも宣言が一致しない: 両方が候補で、ai / unknown
    begin_op(&fx, HOOK, "c-1", "w.txt", Some(replace_edit("b", "B1")), "u1");
    begin_op(&fx, HOOK, "c-2", "w.txt", Some(replace_edit("b", "B2")), "u2");
    fx.write("w.txt", b"a\nNEITHER\nc\n");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-2", "clientOpId": "u2", "result": "ok" }));
    // 開いたままの u1 は数えない (D46)。ok で閉じると両方が候補になる
    assert_eq!(fx.describe(fx.last("w.txt").id)["candidateSessions"], json!([s2]));
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-1", "clientOpId": "u1", "result": "ok" }));
    let rev = fx.last("w.txt");
    assert_eq!(verdict(&rev), ("ai", "unknown"));
    let mut expected = vec![s1.clone(), s2.clone()];
    expected.sort();
    assert_eq!(fx.describe(rev.id)["candidateSessions"], json!(expected));
    assert_eq!(rev.operation_id, None);
    assert!(fx.list_for_session(&s1).contains(&"w.txt".to_string()));
    assert!(fx.list_for_session(&s2).contains(&"w.txt".to_string()));
}

// ---- 受け入れ条件 6、7: 後から届く根拠、report の結果 -----------------------------------------------

#[test]
fn a_report_arriving_later_raises_unknown_to_claimed_and_keeps_the_old_evidence() {
    let mut fx = Fx::new("report_later");
    fx.write("f.txt", b"f0\n");
    fx.start(cfg(50));
    let session = fx
        .ok("session_begin", mcp(), json!({ "provider": "claude", "workspace": ".", "source": "mcp" }))["sessionId"]
        .as_str()
        .unwrap()
        .to_string();
    std::thread::sleep(Duration::from_millis(300));
    fx.write("f.txt", b"f1 by the AI\n");
    wait_until("f.txt の Revision", || fx.revision_count("f.txt") == 1);
    let rev = fx.last("f.txt");
    assert_eq!(verdict(&rev), ("unknown", "unknown"), "申告も窓も無く時刻も遠い変更は unknown");
    let before_evidence = fx.evidence(rev.id);
    assert_eq!(before_evidence, ["observed"]);

    let reply = fx.ok(
        "report",
        mcp(),
        json!({
            "sessionId": session, "paths": ["f.txt"],
            "beforeHashes": [sha256_hex(b"f0\n")], "afterHashes": [sha256_hex(b"f1 by the AI\n")],
        }),
    );
    assert_eq!(reply["results"][0]["result"], "matched", "{reply}");
    let rev = fx.last("f.txt");
    assert_eq!(verdict(&rev), ("ai", "claimed"), "申告だけでは verified にならない");
    let after_evidence = fx.evidence(rev.id);
    assert!(after_evidence.starts_with(&before_evidence), "根拠が消えた: {after_evidence:?}");
    assert!(after_evidence.contains(&"report_hash_match".to_string()), "{after_evidence:?}");
    assert_eq!(fx.list_for_session(&session), ["f.txt"]);
}

#[test]
fn report_results_are_matched_mismatch_or_not_observed_and_never_verified() {
    let mut fx = Fx::new("report_results");
    fx.write("m.txt", b"m0\n");
    fx.write("n.txt", b"n0\n");
    fx.write("quiet.txt", b"q\n");
    fx.start(cfg(50));
    let session = fx
        .ok("session_begin", mcp(), json!({ "provider": "codex", "workspace": ".", "source": "mcp" }))["sessionId"]
        .as_str()
        .unwrap()
        .to_string();
    std::thread::sleep(Duration::from_millis(200));
    fx.write("m.txt", b"m1\n");
    fx.write("n.txt", b"n1\n");

    // 申告の前に、申告されたパスを読む (監視を待たない)。ハッシュはオブジェクトの形でも渡せる
    let reply = fx.ok(
        "report",
        mcp(),
        json!({
            "sessionId": session, "paths": ["m.txt", "n.txt", "quiet.txt"],
            "beforeHashes": { "m.txt": sha256_hex(b"m0\n"), "n.txt": sha256_hex(b"not the before") },
            "afterHashes": { "m.txt": sha256_hex(b"m1\n"), "n.txt": sha256_hex(b"n1\n") },
        }),
    );
    let results: Vec<(&str, &str)> =
        reply["results"].as_array().unwrap().iter().map(|r| (r["path"].as_str().unwrap(), r["result"].as_str().unwrap())).collect();
    assert_eq!(results, [("m.txt", "matched"), ("n.txt", "mismatch"), ("quiet.txt", "not_observed")], "{reply}");
    // どれも verified にならない。照合できなかった申告も claimed (根拠に report_only が残る)
    assert_eq!(verdict(&fx.last("m.txt")), ("ai", "claimed"));
    let n = fx.last("n.txt");
    assert_eq!(verdict(&n), ("ai", "claimed"));
    assert!(fx.evidence(n.id).contains(&"report_only".to_string()));
    // ハッシュの形が悪ければ bad_args
    let bad = fx.call("report", mcp(), json!({ "sessionId": session, "paths": ["m.txt"], "beforeHashes": ["zz"] })).unwrap_err();
    assert_eq!(bad.code, code::BAD_ARGS);
}

// ---- 受け入れ条件 8: time_near と gap ---------------------------------------------------------------

#[test]
fn a_change_near_a_running_session_is_inferred_and_a_wider_width_cannot_raise_it() {
    for near_ms in [2000, 3_600_000] {
        let mut fx = Fx::new(&format!("near_{near_ms}"));
        fx.write("n.txt", b"n0\n");
        fx.start(cfg(near_ms));
        let session = fx.begin_hook("c-near");
        fx.write("n.txt", b"n1\n");
        wait_until("n.txt", || fx.revision_count("n.txt") == 1);
        let rev = fx.last("n.txt");
        assert_eq!(verdict(&rev), ("ai", "inferred"), "幅 {near_ms}: {:?}", fx.evidence(rev.id));
        assert!(fx.evidence(rev.id).contains(&"time_near".to_string()));
        assert_eq!(fx.list_for_session(&session), ["n.txt"]);
    }
}

#[test]
fn a_change_with_no_session_near_is_unknown() {
    let mut fx = Fx::new("far");
    fx.write("n.txt", b"n0\n");
    fx.start(cfg(50));
    let _ = fx.begin_hook("c-far");
    std::thread::sleep(Duration::from_millis(400));
    fx.write("n.txt", b"n1\n");
    wait_until("n.txt", || fx.revision_count("n.txt") == 1);
    assert_eq!(verdict(&fx.last("n.txt")), ("unknown", "unknown"));
}

#[test]
fn a_gap_revision_is_unknown_even_while_a_session_runs() {
    let mut fx = Fx::new("gap");
    fx.write("g.txt", b"g0\n");
    fx.start(cfg(2000));
    let _ = fx.begin_hook("c-gap");
    // 監視のイベントを取りこぼし、検証の走査が gap として拾う
    fx.tracker.as_ref().unwrap().drop_incoming_events(true);
    fx.write("g.txt", b"g1 missed by the watcher\n");
    wait_until("検証の走査が g.txt を拾う", || fx.revision_count("g.txt") == 1);
    let rev = fx.last("g.txt");
    assert_eq!(rev.completeness, "gap");
    assert_eq!(verdict(&rev), ("unknown", "unknown"), "取りこぼした間の変更を、時刻だけで AI のものにした");
    // 監視の取りこぼし (verify_gap) は preexisting にしない。時刻による根拠も作らない
    let kinds = fx.evidence(rev.id);
    assert_eq!(kinds, ["observed"], "{kinds:?}");
}

/// 取りこぼしを人為的に起こし、検証の走査が gap の Revision を作るまで待つ。
fn write_while_the_watcher_misses(fx: &mut Fx, path: &str, bytes: &[u8]) -> Rev {
    fx.tracker.as_ref().unwrap().drop_incoming_events(true);
    fx.write(path, bytes);
    wait_until("検証の走査が拾う", || fx.revision_count(path) == 1);
    fx.tracker.as_ref().unwrap().drop_incoming_events(false);
    let rev = fx.last(path);
    assert_eq!(rev.completeness, "gap");
    rev
}

#[test]
fn a_verify_gap_change_matching_the_declared_edit_is_still_verified() {
    let mut fx = Fx::new("vgap_verified");
    fx.write("g.txt", b"alpha
beta
");
    fx.start(cfg(2000));
    let session = fx.begin_hook("c-vg");
    let begin = begin_op(&fx, HOOK, "c-vg", "g.txt", Some(replace_edit("beta", "gamma")), "t");
    let rev = write_while_the_watcher_misses(&mut fx, "g.txt", b"alpha
gamma
");
    let end = fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-vg", "clientOpId": "t", "result": "ok" }));
    let rev_after = fx.last("g.txt");
    assert_eq!(rev.id, rev_after.id, "op_end の即時の読み取りが 2 つ目の Revision を作った: {end}");
    assert_eq!(verdict(&rev_after), ("ai", "verified"), "{:?}", fx.evidence(rev.id));
    assert_eq!(rev_after.operation_id.as_deref(), begin["operationId"].as_str());
    let kinds = fx.evidence(rev.id);
    assert!(kinds.contains(&"declared_edit_match".to_string()), "{kinds:?}");
    assert!(!kinds.contains(&"op_window".to_string()) && !kinds.contains(&"preexisting".to_string()), "{kinds:?}");
    assert_eq!(end["revisions"][0]["revisionId"].as_i64(), Some(rev.id), "{end}");
    assert_eq!(fx.list_for_session(&session), ["g.txt"]);
}

#[test]
fn a_verify_gap_change_that_differs_from_the_declaration_is_not_verified() {
    let mut fx = Fx::new("vgap_differs");
    fx.write("g.txt", b"alpha
beta
");
    fx.start(cfg(2000));
    fx.begin_hook("c-vd");
    begin_op(&fx, HOOK, "c-vd", "g.txt", Some(replace_edit("beta", "gamma")), "t");
    let rev = write_while_the_watcher_misses(&mut fx, "g.txt", b"alpha
SOMETHING ELSE
");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-vd", "clientOpId": "t", "result": "ok" }));
    // 窓だけでは根拠にしない (観測の時刻は走査の時刻)。内容が違うので unknown のまま
    assert_eq!(verdict(&fx.last("g.txt")), ("unknown", "unknown"), "{:?}", fx.evidence(rev.id));
}

#[test]
fn a_startup_gap_stays_unknown_even_when_it_matches_the_declared_edit() {
    let mut fx = Fx::new("startup_gap");
    fx.write("g.txt", b"alpha
beta
");
    fx.start(cfg(2000));
    fx.begin_hook("c-sg");
    begin_op(&fx, HOOK, "c-sg", "g.txt", Some(replace_edit("beta", "gamma")), "t");
    // サービスが止まっている間に、宣言どおりの編集が起きる
    fx.stop();
    fx.write("g.txt", b"alpha
gamma
");
    fx.start(cfg(2000));
    wait_until("起動時の再走査が拾う", || fx.revision_count("g.txt") == 1);
    let rev = fx.last("g.txt");
    assert_eq!(rev.completeness, "gap");
    assert_eq!(verdict(&rev), ("unknown", "unknown"), "止まっていた間の変更を内容の一致だけで AI のものにした");
    assert!(fx.evidence(rev.id).contains(&"preexisting".to_string()));
}

// ---- 受け入れ条件 9: PID の再利用 ------------------------------------------------------------------------

#[test]
fn the_same_pid_with_another_start_time_is_another_process() {
    let mut fx = Fx::new("pid");
    fx.start(cfg(2000));
    let own_start = process_start_ms(std::process::id()).expect("自分の開始時刻");
    let begin = |client: &str| {
        fx.ok("session_begin", mcp(), json!({ "provider": "claude", "workspace": ".", "source": "mcp", "clientSessionId": client}))["sessionId"]
            .as_str()
            .unwrap()
            .to_string()
    };
    let alive = begin("alive");
    let reused = begin("reused");
    let (pid, start): (i64, i64) = fx
        .db()
        .query_row("SELECT process_id, process_start FROM sessions WHERE session_id = ?1", [&alive], |r| Ok((r.get(0)?, r.get(1)?)))
        .unwrap();
    assert_eq!(pid, i64::from(std::process::id()));
    assert!((start - own_start).abs() < 2000, "開始時刻が記録されていない: {start} / {own_start}");

    // 同じ PID で、開始時刻が違う (PID が使い回された)。表を直接書き換えて、記録のときの値を模す
    fx.db().execute("UPDATE sessions SET process_start = process_start - 86400000 WHERE session_id = ?1", [&reused]).unwrap();
    let report = fx
        .env()
        .writer
        .call(|store| session::tick(store, now_ms(), &LifecycleConfig::default(), &is_alive).unwrap())
        .unwrap();
    assert_eq!((report.interrupted, report.abandoned), (1, 0));
    let status = |id: &str| -> String { fx.db().query_row("SELECT status FROM sessions WHERE session_id = ?1", [id], |r| r.get(0)).unwrap() };
    assert_eq!(status(&alive), "running", "生きているプロセスのセッションを止めた");
    assert_eq!(status(&reused), "interrupted", "PID が同じだけで同じプロセスと見なした");
}

// ---- 受け入れ条件 10: 複数セッションの区別と再接続 -----------------------------------------------------------

#[test]
fn sessions_are_distinguished_and_the_same_client_session_id_resumes_one() {
    let mut fx = Fx::new("distinct");
    fx.start(cfg(2000));
    let one = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal" }));
    let two = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal" }));
    assert_ne!(one["sessionId"], two["sessionId"]);
    assert_eq!(one["resumed"], false);
    let id = one["sessionId"].as_str().unwrap();
    // UUID v4 の形
    let b: Vec<char> = id.chars().collect();
    assert_eq!((id.len(), b[8], b[13], b[14], b[18], b[23]), (36, '-', '-', '4', '-', '-'), "{id}");
    assert!(one["trackingCapabilities"].as_array().unwrap().iter().any(|k| k == "declared_edit_match"));
    assert!(one["baseCommit"].is_null(), "git でないディレクトリ: {one}");

    let first = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "same" }));
    let again = fx.ok("session_begin", mcp(), json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "same" }));
    assert_eq!(first["sessionId"], again["sessionId"]);
    assert_eq!((first["resumed"].as_bool(), again["resumed"].as_bool()), (Some(false), Some(true)));
    let sessions: i64 = fx.db().query_row("SELECT COUNT(*) FROM sessions WHERE client_session_id = 'same'", [], |r| r.get(0)).unwrap();
    assert_eq!(sessions, 1);

    // 終了した後の同じ clientSessionId は、新しいセッション
    fx.ok("session_end", HOOK, json!({ "clientSessionId": "same" }));
    let third = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "same" }));
    assert_ne!(third["sessionId"], first["sessionId"]);
    assert_eq!(third["resumed"], false);
}

// ---- 統合ターミナルの紐付け (M106f、スキーマ版 4): hostTerminal ------------------------------------------

fn host_terminal_of(fx: &Fx, session_id: &str) -> Option<String> {
    fx.db().query_row("SELECT host_terminal FROM sessions WHERE session_id = ?1", [session_id], |r| r.get(0)).unwrap()
}

#[test]
fn a_host_terminal_id_is_stored_and_a_missing_one_is_null() {
    let mut fx = Fx::new("host_terminal");
    fx.start(cfg(2000));
    let hosted = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "h1", "hostTerminal": "4242-1763500000-3" }));
    let old_hook = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "h2" }));
    let null_field = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "h3", "hostTerminal": null }));
    let empty = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "h4", "hostTerminal": "" }));
    let id_of = |r: &Value| r["sessionId"].as_str().unwrap().to_string();
    assert_eq!(host_terminal_of(&fx, &id_of(&hosted)).as_deref(), Some("4242-1763500000-3"));
    // フィールドが無い (旧フック)・null・空は、どれも NULL (欠落 = 統合ターミナルの外)
    for r in [&old_hook, &null_field, &empty] {
        assert_eq!(host_terminal_of(&fx, &id_of(r)), None, "{r}");
    }
    // source は統合ターミナルでも "terminal" のまま
    let source: String = fx.db().query_row("SELECT source FROM sessions WHERE client_session_id = 'h1'", [], |r| r.get(0)).unwrap();
    assert_eq!(source, "terminal");
    // 同じ clientSessionId の再開は新しい行を作らず、保存済みの値を変えない
    let again = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "h1", "hostTerminal": "9-9-9" }));
    assert_eq!((again["resumed"].as_bool(), id_of(&again)), (Some(true), id_of(&hosted)));
    assert_eq!(host_terminal_of(&fx, &id_of(&hosted)).as_deref(), Some("4242-1763500000-3"));

    // `sessions` の項目に hostTerminal が出る (無ければ null)
    let listed = ops::sessions(&fx.db(), args(json!({}))).unwrap();
    let items = listed["items"].as_array().unwrap();
    let find = |r: &Value| items.iter().find(|i| i["sessionId"] == r["sessionId"]).unwrap().clone();
    assert_eq!(find(&hosted)["hostTerminal"], "4242-1763500000-3");
    assert!(find(&old_hook)["hostTerminal"].is_null(), "{}", find(&old_hook));
    assert_eq!(find(&hosted)["source"], "terminal");

    // 上限を超える ID と文字列でない値は bad_args。セッションは作られない
    let before: i64 = fx.db().query_row("SELECT COUNT(*) FROM sessions", [], |r| r.get(0)).unwrap();
    for bad in [json!("x".repeat(ops_record::MAX_HOST_TERMINAL_BYTES + 1)), json!(12), json!(["a"])] {
        let e = fx.call("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "hostTerminal": bad })).unwrap_err();
        assert_eq!(e.code, code::BAD_ARGS, "{bad}");
    }
    let after: i64 = fx.db().query_row("SELECT COUNT(*) FROM sessions", [], |r| r.get(0)).unwrap();
    assert_eq!(before, after);
}

#[test]
fn a_host_terminal_id_survives_a_rebuild_from_the_journal() {
    let mut fx = Fx::new("host_terminal_rebuild");
    fx.start(cfg(2000));
    fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "rb-h", "hostTerminal": "7-8-9" }));
    fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "rb-n" }));
    fx.stop();
    drop(open_project(&fx.root, now_ms()).0);
    let before = attribution_tables(&fx.db())["sessions"].clone();
    assert!(before.iter().any(|r| r.contains("7-8-9")), "{before:#?}");

    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert!(report.rebuilt.is_some() && report.failed_steps.is_empty(), "{report:?}");
    drop(store);
    assert_eq!(attribution_tables(&fx.db())["sessions"], before, "作り直しで host_terminal が変わった");
}

// ---- 受け入れ条件 11: worktree ----------------------------------------------------------------------------

/// 4 つの条件のうち何を外すか。
#[derive(Clone, Copy, PartialEq)]
enum Break {
    None,
    NotIsolated,
    NotLinked,
    SecondSession,
    EditorConnected,
}

fn worktree_verdict(name: &str, brk: Break) -> ((String, String), Vec<String>) {
    let mut fx = Fx::new(name);
    fx.write("src.txt", b"s0\n");
    if brk == Break::NotLinked {
        // 普通のプロジェクト: `.git` はディレクトリ
        fs::create_dir_all(fx.root.join(".git")).unwrap();
    } else {
        // リンクされた作業ツリー: `.git` はファイルで `gitdir:` の行を持つ
        fx.write(".git", format!("gitdir: {}\n", fx.root.join("main_git").join("worktrees").join("wt").display()).as_bytes());
    }
    if brk == Break::EditorConnected {
        fx.editors.store(1, Ordering::SeqCst);
    }
    fx.start(cfg(2000));
    let isolated = brk != Break::NotIsolated;
    let begin = |client: &str| {
        fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": client, "isolated": isolated }))
    };
    begin("w1");
    if brk == Break::SecondSession {
        begin("w2");
    }
    // 申告も操作も無い変更 (専用の作業ツリーの中の変更)
    fx.write("src.txt", b"s1\n");
    wait_until("src.txt", || fx.revision_count("src.txt") == 1);
    let rev = fx.last("src.txt");
    let verdict = (rev.actor.clone(), rev.attribution.clone());
    let kinds = fx.evidence(rev.id);
    fx.editors.store(0, Ordering::SeqCst);
    (verdict, kinds)
}

#[test]
fn a_dedicated_worktree_session_is_verified_only_when_all_four_conditions_hold() {
    let (verdict, kinds) = worktree_verdict("wt_all", Break::None);
    assert_eq!(verdict, ("ai".to_string(), "verified".to_string()), "{kinds:?}");
    assert!(kinds.contains(&"worktree".to_string()));
    for (name, brk) in [
        ("wt_no_iso", Break::NotIsolated),
        ("wt_not_linked", Break::NotLinked),
        ("wt_two", Break::SecondSession),
        ("wt_editor", Break::EditorConnected),
    ] {
        let (verdict, kinds) = worktree_verdict(name, brk);
        assert_ne!(verdict.1, "verified", "{name}: 条件が欠けているのに verified: {kinds:?}");
        assert!(!kinds.contains(&"worktree".to_string()), "{name}: {kinds:?}");
    }
}

// ---- 受け入れ条件 14: 作り直し -----------------------------------------------------------------------------

fn dump(conn: &Connection, sql: &str) -> Vec<String> {
    let mut stmt = conn.prepare(sql).unwrap();
    let n = stmt.column_count();
    let rows = stmt
        .query_map([], |r| {
            let mut cells = Vec::new();
            for i in 0..n {
                cells.push(match r.get_ref(i)? {
                    ValueRef::Null => "NULL".to_string(),
                    ValueRef::Integer(v) => v.to_string(),
                    ValueRef::Real(v) => v.to_string(),
                    ValueRef::Text(t) => String::from_utf8_lossy(t).into_owned(),
                    ValueRef::Blob(b) => format!("blob{}", b.len()),
                });
            }
            Ok(cells.join("|"))
        })
        .unwrap();
    rows.map(Result::unwrap).collect()
}

fn attribution_tables(conn: &Connection) -> BTreeMap<&'static str, Vec<String>> {
    let mut out = BTreeMap::new();
    out.insert("sessions", dump(conn, "SELECT * FROM sessions ORDER BY session_id"));
    out.insert("operations", dump(conn, "SELECT * FROM operations ORDER BY operation_id"));
    out.insert("attribution_evidence", dump(conn, "SELECT * FROM attribution_evidence ORDER BY evidence_id"));
    out.insert("change_revisions", dump(conn, "SELECT * FROM change_revisions ORDER BY revision_id"));
    out.insert("snapshots", dump(conn, "SELECT * FROM snapshots ORDER BY snapshot_id"));
    out
}

#[test]
fn a_broken_db_is_rebuilt_to_the_same_sessions_operations_and_evidence() {
    let mut fx = Fx::new("rebuild");
    fx.write("x.txt", b"alpha\nbeta\n");
    fx.write("r.txt", b"r0\n");
    fx.start(cfg(2000));
    let s1 = fx.begin_hook("c-rb1");
    let s2 = fx.ok("session_begin", mcp(), json!({ "provider": "gemini", "workspace": ".", "source": "mcp" }))["sessionId"].as_str().unwrap().to_string();
    begin_op(&fx, HOOK, "c-rb1", "x.txt", Some(replace_edit("beta", "gamma")), "t1");
    fx.write("x.txt", b"alpha\ngamma\n");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-rb1", "clientOpId": "t1", "result": "ok" }));
    let hashes = json!({ "beforeHashes": [sha256_hex(b"r0\n")], "afterHashes": [sha256_hex(b"r1\n")] });
    fx.write("r.txt", b"r1\n");
    wait_until("r.txt", || fx.revision_count("r.txt") == 1);
    let mut report = json!({ "sessionId": s2, "paths": ["r.txt"] });
    report["beforeHashes"] = hashes["beforeHashes"].clone();
    report["afterHashes"] = hashes["afterHashes"].clone();
    fx.ok("report", mcp(), report);
    begin_op(&fx, HOOK, "c-rb1", "r.txt", None, "t2");
    fx.ok("session_end", HOOK, json!({ "sessionId": s1 }));
    fx.stop();

    // 一度開き直して、残った操作・セッションの整理 (これもイベント) を済ませる。その状態が基準
    drop(open_project(&fx.root, now_ms()).0);
    let before = attribution_tables(&fx.db());
    assert!(before["attribution_evidence"].len() >= 6 && before["operations"].len() == 3, "{before:#?}");
    assert!(before["operations"].iter().any(|r| r.contains("abandoned")), "{:#?}", before["operations"]);

    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert!(report.broken_db.is_some() && report.rebuilt.is_some(), "{report:?}");
    assert!(report.failed_steps.is_empty(), "{:?}", report.failed_steps);
    drop(store);
    let after = attribution_tables(&fx.db());
    for (table, rows) in &before {
        assert_eq!(rows, &after[table], "{table} が壊す前と違う");
    }
}

/// ---- editor_saved (M104k2): Editor の保存は「Editor の利用者」として確定する --------------------------------------

fn saved(fx: &Fx, path: &str, bytes: &[u8]) -> Value {
    fx.ok("editor_saved", EDITOR, json!({ "path": path, "hash": sha256_hex(bytes) }))
}

fn count_evidence(conn: &Connection, kind: &str) -> i64 {
    conn.query_row("SELECT COUNT(*) FROM attribution_evidence WHERE kind = ?1", [kind], |r| r.get(0)).unwrap()
}

#[test]
fn a_save_notice_after_the_observation_makes_the_change_the_editor_users() {
    let mut fx = Fx::new("saved_after");
    fx.write("scene.json", b"{}\n");
    fx.start(cfg(2000));
    fx.write("scene.json", b"{\"a\":1}\n");
    wait_until("scene.json の Revision", || fx.revision_count("scene.json") == 1);
    assert_eq!(verdict(&fx.last("scene.json")), ("unknown", "unknown"));
    let reply = saved(&fx, "scene.json", b"{\"a\":1}\n");
    assert_eq!(reply["pending"], false, "{reply}");
    let rev = fx.last("scene.json");
    assert_eq!(verdict(&rev), ("editor_user", "verified"), "{:?}", fx.evidence(rev.id));
    assert!(fx.evidence(rev.id).contains(&"editor_saved".to_string()));
    assert_eq!(fx.describe(rev.id)["actor"], "editor_user");
}

#[test]
fn a_save_notice_before_the_observation_gives_the_same_result() {
    let mut fx = Fx::new("saved_before");
    fx.write("a.json", b"a0\n");
    fx.start(cfg(2000));
    // 知らせが先 (ファイルはまだ書かれていない)。観測が後から届いて、保持していた知らせと照合される
    saved(&fx, "a.json", b"a1 by the editor\n");
    assert_eq!(fx.revision_count("a.json"), 0);
    fx.write("a.json", b"a1 by the editor\n");
    wait_until("a.json の Revision", || fx.revision_count("a.json") == 1);
    let rev = fx.last("a.json");
    assert_eq!(verdict(&rev), ("editor_user", "verified"), "{:?}", fx.evidence(rev.id));
    assert_eq!(fx.evidence(rev.id), ["observed", "editor_saved"]);
}

#[test]
fn a_save_notice_with_another_hash_or_another_path_does_not_match() {
    let mut fx = Fx::new("saved_mismatch");
    fx.write("a.json", b"a0\n");
    fx.write("b.json", b"b0\n");
    fx.start(cfg(2000));
    saved(&fx, "a.json", b"not what is written\n");
    saved(&fx, "b.json", b"b1\n"); // 別のパスの知らせ
    fx.write("a.json", b"a1\n");
    wait_until("a.json", || fx.revision_count("a.json") == 1);
    assert_eq!(verdict(&fx.last("a.json")), ("unknown", "unknown"), "内容が違う知らせで Editor のものにした");
    // 知らせたパスでも、あとから別の内容に書き換わったら照合しない
    fx.write("b.json", b"b2 written by someone else\n");
    wait_until("b.json", || fx.revision_count("b.json") == 1);
    assert_eq!(verdict(&fx.last("b.json")), ("unknown", "unknown"));
}

#[test]
fn a_save_notice_older_than_the_window_is_not_kept() {
    let mut fx = Fx::new("saved_old");
    fx.write("a.json", b"a0\n");
    fx.start(cfg(2000));
    let old = now_ms() - 2 * mye_track::attribution::EDITOR_SAVED_WINDOW_MS - 5_000;
    let hash = sha256_hex(b"a1\n");
    let rx = fx
        .env()
        .writer
        .submit_bounded(1024, move |store| {
            session::record_and_apply(store, old, "editor_saved", json!({ "path": "a.json", "hash": hash }))
        })
        .unwrap();
    rx.recv().unwrap().unwrap();
    fx.write("a.json", b"a1\n");
    wait_until("a.json", || fx.revision_count("a.json") == 1);
    assert_eq!(verdict(&fx.last("a.json")), ("unknown", "unknown"), "窓より古い知らせが照合された");
}

#[test]
fn the_editor_wins_over_an_ai_declared_edit_that_produced_the_same_content() {
    let mut fx = Fx::new("saved_vs_ai");
    fx.write("x.txt", b"alpha\nbeta\n");
    fx.start(cfg(2000));
    let session = fx.begin_hook("c-ed");
    begin_op(&fx, HOOK, "c-ed", "x.txt", Some(replace_edit("beta", "BETA")), "t1");
    fx.write("x.txt", b"alpha\nBETA\n");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-ed", "clientOpId": "t1", "result": "ok" }));
    let rev = fx.last("x.txt");
    assert_eq!(verdict(&rev), ("ai", "verified"), "前提: AI の宣言と一致している");
    assert_eq!(fx.list_for_session(&session), ["x.txt"]);

    // 同じ内容を Editor が保存したと知らせる: attr §3 の表の先頭の行が勝つ
    saved(&fx, "x.txt", b"alpha\nBETA\n");
    let rev = fx.last("x.txt");
    assert_eq!(verdict(&rev), ("editor_user", "verified"), "{:?}", fx.evidence(rev.id));
    assert!(rev.operation_id.is_none());
    let kinds = fx.evidence(rev.id);
    assert!(
        kinds.contains(&"declared_edit_match".to_string()) && kinds.contains(&"editor_saved".to_string()),
        "根拠は消さない: {kinds:?}"
    );
    assert!(fx.list_for_session(&session).is_empty(), "Editor の変更が AI のセッションの一覧に残った");
    assert_eq!(fx.ok("session_end", HOOK, json!({ "sessionId": session }))["changeCount"], 0);
}

#[test]
fn editor_saved_rejects_bad_arguments_and_only_the_editor_may_call_it() {
    let mut fx = Fx::new("saved_args");
    fx.write("a.txt", b"a\n");
    fx.start(cfg(2000));
    let good = sha256_hex(b"a\n");
    for (name, a) in [
        ("hash が短い", json!({ "path": "a.txt", "hash": "abc" })),
        ("hash が 16 進でない", json!({ "path": "a.txt", "hash": "z".repeat(64) })),
        ("path が無い", json!({ "hash": good })),
        ("外のパス", json!({ "path": "../outside.txt", "hash": good })),
        ("知らない引数", json!({ "path": "a.txt", "hash": good, "extra": 1 })),
    ] {
        let e = fx.call("editor_saved", EDITOR, a).unwrap_err();
        assert_eq!(e.code, code::BAD_ARGS, "{name}: {}", e.detail);
    }
    for role in [Role::Mcp, Role::Hook] {
        assert!(!role_allows(role, "editor_saved"), "{role:?} が editor_saved を呼べる");
    }
    assert!(role_allows(Role::Editor, "editor_saved"));
    // 大文字の 16 進も受ける (小文字にして照合する)
    assert!(fx.call("editor_saved", EDITOR, json!({ "path": "a.txt", "hash": good.to_ascii_uppercase() })).is_ok());
}

#[test]
fn list_takes_one_attribution_or_several_and_rejects_unknown_ones() {
    let mut fx = Fx::new("list_attr");
    fx.write("a.txt", b"a0\n");
    fx.write("b.txt", b"b0\n");
    fx.start(cfg(2000));
    fx.write("a.txt", b"a1\n");
    fx.write("b.txt", b"b1\n");
    wait_until("2 件", || fx.revision_count("a.txt") == 1 && fx.revision_count("b.txt") == 1);
    saved(&fx, "b.txt", b"b1\n");
    let conn = ops::open_reader(&Paths::for_project(&fx.root).db).unwrap();
    let paths = |a: Value| -> Vec<String> {
        let list = ops::list(&conn, args(json!({ "attribution": a }))).unwrap();
        let mut out: Vec<String> =
            list["items"].as_array().unwrap().iter().map(|i| i["path"].as_str().unwrap().to_string()).collect();
        out.sort();
        out
    };
    assert_eq!(paths(json!("unknown")), ["a.txt"]);
    assert_eq!(paths(json!("verified")), ["b.txt"]);
    assert_eq!(paths(json!(["unknown", "inferred"])), ["a.txt"]);
    assert_eq!(paths(json!(["unknown", "verified"])), ["a.txt", "b.txt"]);
    assert_eq!(ops::list(&conn, args(json!({ "attribution": ["unknown", "bogus"] }))).unwrap_err().code, code::BAD_ARGS);
    assert_eq!(ops::list(&conn, args(json!({ "attribution": 5 }))).unwrap_err().code, code::BAD_ARGS);
    assert_eq!(ops::list(&conn, args(json!({ "attribution": [1] }))).unwrap_err().code, code::BAD_ARGS);
}

#[test]
fn a_broken_db_is_rebuilt_with_the_editor_saved_evidence_in_either_order() {
    let mut fx = Fx::new("saved_rebuild");
    fx.write("late.txt", b"l0\n");
    fx.write("early.txt", b"e0\n");
    fx.write("ai.txt", b"alpha\nbeta\n");
    fx.start(cfg(2000));
    let _s = fx.begin_hook("c-sr");
    // 知らせが後 / 先 / AI の宣言と同じ内容 (Editor が勝つ)
    fx.write("late.txt", b"l1\n");
    wait_until("late.txt", || fx.revision_count("late.txt") == 1);
    saved(&fx, "late.txt", b"l1\n");
    saved(&fx, "early.txt", b"e1\n");
    fx.write("early.txt", b"e1\n");
    wait_until("early.txt", || fx.revision_count("early.txt") == 1);
    begin_op(&fx, HOOK, "c-sr", "ai.txt", Some(replace_edit("beta", "BETA")), "t1");
    fx.write("ai.txt", b"alpha\nBETA\n");
    fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-sr", "clientOpId": "t1", "result": "ok" }));
    saved(&fx, "ai.txt", b"alpha\nBETA\n");
    for p in ["late.txt", "early.txt", "ai.txt"] {
        assert_eq!(verdict(&fx.last(p)), ("editor_user", "verified"), "{p}: 前提");
    }
    fx.stop();

    drop(open_project(&fx.root, now_ms()).0);
    let before = attribution_tables(&fx.db());
    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert!(report.broken_db.is_some() && report.rebuilt.is_some(), "{report:?}");
    assert!(report.failed_steps.is_empty(), "{:?}", report.failed_steps);
    drop(store);
    let after = attribution_tables(&fx.db());
    for (table, rows) in &before {
        assert_eq!(rows, &after[table], "{table} が壊す前と違う");
    }
    assert_eq!(count_evidence(&fx.db(), "editor_saved"), 3, "editor_saved の根拠が戻っていない");
}

// ---- 受け入れ条件 15: 寿命 ----------------------------------------------------------------------------------

#[test]
fn an_operation_without_op_end_is_abandoned_and_a_silent_hook_session_is_interrupted_then_resumed() {
    let mut fx = Fx::new("lifecycle");
    fx.write("o.txt", b"o0\n");
    fx.start(cfg(2000));
    let session = fx.begin_hook("c-life");
    let op = begin_op(&fx, HOOK, "c-life", "o.txt", None, "t");
    // 窓の中で変更が起きる
    fx.write("o.txt", b"o1\n");
    wait_until("o.txt", || fx.revision_count("o.txt") == 1);
    std::thread::sleep(Duration::from_millis(300));

    let quick = LifecycleConfig { hook_idle_ms: 200, op_timeout_ms: 200, check_interval: Duration::from_secs(1) };
    let report = fx.env().writer.call(move |store| session::tick(store, now_ms(), &quick, &is_alive).unwrap()).unwrap();
    assert_eq!((report.interrupted, report.abandoned), (1, 1));
    let (result, finished): (String, Option<i64>) = fx
        .db()
        .query_row("SELECT result, finished_at FROM operations WHERE operation_id = ?1", [op["operationId"].as_str().unwrap()], |r| Ok((r.get(0)?, r.get(1)?)))
        .unwrap();
    assert_eq!(result, "abandoned");
    assert!(finished.is_some());
    // 打ち切りの前に窓へ入った Revision の根拠は残る
    assert!(fx.evidence(fx.last("o.txt").id).contains(&"op_window".to_string()));
    let status: String = fx.db().query_row("SELECT status FROM sessions WHERE session_id = ?1", [&session], |r| r.get(0)).unwrap();
    assert_eq!(status, "interrupted");

    // 同じ clientSessionId の session_begin で running に戻る (新しいセッションにはならない)
    let back = fx.ok("session_begin", HOOK, json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "c-life" }));
    assert_eq!((back["sessionId"].as_str(), back["resumed"].as_bool(), back["status"].as_str()), (Some(session.as_str()), Some(true), Some("running")));
    // 無活動の間に打ち切られても、op_begin が来れば戻る
    fx.env().writer.call(move |store| session::tick(store, now_ms() + 3_600_000, &LifecycleConfig::default(), &is_alive).unwrap()).unwrap();
    let status: String = fx.db().query_row("SELECT status FROM sessions WHERE session_id = ?1", [&session], |r| r.get(0)).unwrap();
    assert_eq!(status, "interrupted");
    begin_op(&fx, HOOK, "c-life", "o.txt", None, "t3");
    let status: String = fx.db().query_row("SELECT status FROM sessions WHERE session_id = ?1", [&session], |r| r.get(0)).unwrap();
    assert_eq!(status, "running");
}

// ---- 受け入れ条件 16: 記録の op は AI を待たせない -------------------------------------------------------------

#[test]
fn op_end_returns_pending_in_about_two_seconds_while_another_process_holds_the_file() {
    use std::io::Write;
    use std::os::windows::fs::OpenOptionsExt;

    let mut fx = Fx::new("pending");
    fx.write("p.txt", b"p0\n");
    fx.start(cfg(2000));
    fx.begin_hook("c-pend");
    begin_op(&fx, HOOK, "c-pend", "p.txt", Some(write_edit("p1\n")), "t");
    // 別のプロセスが共有なしで開いて、書き続けている状態を模す
    let mut holder = fs::OpenOptions::new().write(true).truncate(true).share_mode(0).open(fx.root.join("p.txt")).unwrap();
    holder.write_all(b"p1\n").unwrap();
    fx.model.insert("p.txt".to_string(), b"p1\n".to_vec());

    let started = Instant::now();
    let end = fx.ok("op_end", HOOK, json!({ "clientSessionId": "c-pend", "clientOpId": "t", "result": "ok" }));
    let took = started.elapsed();
    assert_eq!(end["pending"], true, "{end}");
    assert!(took >= Duration::from_millis(1500) && took < Duration::from_secs(5), "2 秒ほどで返るはず: {took:?}");
    assert_eq!(end["revisions"], json!([]));
    drop(holder);
    // 開放の後は、監視の経路でこの変更が記録される (帰属は後から付く)
    wait_until("p.txt の Revision", || fx.revision_count("p.txt") == 1);
}

#[test]
fn recording_ops_answer_busy_when_the_writer_queue_is_over_its_limit() {
    let mut fx = Fx::new("busy");
    fx.start(cfg(2000));
    let mut env = fx.env().clone();
    env.queue_limit = 0;
    let err = ops_record::session_begin(&env, &HOOK, args(json!({ "provider": "claude", "workspace": ".", "source": "terminal" }))).unwrap_err();
    assert_eq!(err.code, code::BUSY, "{} {}", err.code, err.detail);
}

// ---- パスの正規化と役割 ----------------------------------------------------------------------------------------

#[test]
fn paths_are_normalized_in_the_service_and_must_stay_inside_the_project() {
    let root = PathBuf::from(r"C:\Work\Proj");
    let ok = |p: &str| normalize_path(&root, p, false).unwrap();
    assert_eq!(ok("a/b.txt"), "a/b.txt");
    assert_eq!(ok(r"a\b.txt"), "a/b.txt");
    assert_eq!(ok("./a//b.txt"), "a/b.txt");
    assert_eq!(ok("a/../b.txt"), "b.txt");
    assert_eq!(ok(r"C:\Work\Proj\a\b.txt"), "a/b.txt");
    assert_eq!(ok(r"c:/work/proj/A.TXT"), "A.TXT", "ルートの大文字小文字は区別しない (パスの綴りは保つ)");
    assert_eq!(ok(r"\\?\C:\Work\Proj\a.txt"), "a.txt");
    for bad in [
        "../x.txt", "a/../../x.txt", r"C:\Work\Other\x.txt", r"C:\Work\ProjX\x.txt", r"D:\Work\Proj\x.txt", r"\\server\share\x.txt",
        "", "a.txt:stream", "a?.txt", "C:x.txt",
    ] {
        let e = normalize_path(&root, bad, false).unwrap_err();
        assert_eq!(e.code, code::BAD_ARGS, "{bad}");
    }
    assert_eq!(normalize_path(&root, ".", true).unwrap(), "");
    assert_eq!(normalize_path(&root, ".", false).unwrap_err().code, code::BAD_ARGS);
    assert_eq!(normalize_path(&root, r"C:\Work\Proj", true).unwrap(), "");
}

#[test]
fn targets_outside_the_project_are_rejected_by_the_ops() {
    let mut fx = Fx::new("outside");
    fx.start(cfg(2000));
    fx.begin_hook("c-out");
    let e = fx.call("op_begin", HOOK, json!({ "clientSessionId": "c-out", "operationType": "edit", "targets": ["../escape.txt"] })).unwrap_err();
    assert_eq!(e.code, code::BAD_ARGS);
    let e = fx.call("session_begin", HOOK, json!({ "provider": "claude", "workspace": "..", "source": "terminal" })).unwrap_err();
    assert_eq!(e.code, code::BAD_ARGS);
    let e = fx.call("op_begin", HOOK, json!({ "sessionId": "nope", "operationType": "edit", "targets": ["a.txt"] })).unwrap_err();
    assert_eq!(e.code, code::NOT_FOUND);
    let e = fx.call("op_begin", HOOK, json!({ "clientSessionId": "c-out", "operationType": "edit", "targets": ["a.txt"], "extra": 1 })).unwrap_err();
    assert_eq!(e.code, code::BAD_ARGS, "知らない引数は bad_args");
}

#[test]
fn roles_gate_the_recording_ops() {
    for op in ["session_begin", "session_end", "op_begin", "op_end"] {
        assert!(role_allows(Role::Mcp, op) && role_allows(Role::Hook, op), "{op}");
        assert!(!role_allows(Role::Editor, op), "{op} を editor が呼べる");
    }
    assert!(role_allows(Role::Mcp, "report"));
    assert!(!role_allows(Role::Hook, "report"));
    assert!(!role_allows(Role::Editor, "report"));
}

// ---- pipe 越し ----------------------------------------------------------------------------------------------------

struct ServiceProc {
    child: Child,
    root: PathBuf,
}

impl ServiceProc {
    fn spawn(root: &Path, idle_ms: u64) -> ServiceProc {
        let child = Command::new(env!("CARGO_BIN_EXE_mye_track"))
            .args(["serve", "--project"])
            .arg(root)
            .args(["--idle-timeout-ms", &idle_ms.to_string()])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("mye_track.exe を起動できない");
        ServiceProc { child, root: root.to_path_buf() }
    }

    fn connect(&self, role: &str, claimed_pid: Option<u32>) -> Client {
        let mut token = None;
        wait_until("service.json", || {
            token = read_service_json(&self.root).and_then(|v| v["token"].as_str().map(str::to_string));
            token.is_some()
        });
        let mut client = Client::connect(&pipe_name(&project_id(&self.root)), WAIT_LIMIT).expect("pipe に繋がる");
        let mut hello = json!({ "proto": 1, "role": role, "token": token.unwrap(), "projectRoot": self.root.to_string_lossy() });
        if let Some(pid) = claimed_pid {
            hello["pid"] = json!(pid);
        }
        let reply = client.request(1, "hello", hello).unwrap();
        assert_eq!(reply["ok"], true, "{reply}");
        client
    }

    fn wait_watching(&self) {
        let mut client = self.connect("mcp", None);
        wait_until("phase = watching", || client.request(2, "status", json!({})).unwrap()["result"]["phase"] == "watching");
    }
}

impl Drop for ServiceProc {
    fn drop(&mut self) {
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

struct TempRoot(PathBuf);

impl Drop for TempRoot {
    fn drop(&mut self) {
        if !std::thread::panicking() {
            for _ in 0..50 {
                if fs::remove_dir_all(&self.0).is_ok() || !self.0.exists() {
                    return;
                }
                std::thread::sleep(Duration::from_millis(100));
            }
        }
    }
}

fn temp_root(name: &str) -> TempRoot {
    let dir = std::env::temp_dir().join(format!("mye_track_sp_{}_{}", name, std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    TempRoot(dir)
}

#[test]
fn an_mcp_session_takes_the_pid_from_the_pipe_not_from_the_claim() {
    let project = temp_root("pid");
    let service = ServiceProc::spawn(&project.0, 120_000);
    service.wait_watching();
    // hello で別の pid を申告しても、pipe から得た値 (= このテストのプロセス) が採られる
    let mut mcp = service.connect("mcp", Some(4));
    let reply = mcp
        .request(3, "session_begin", json!({ "provider": "claude", "workspace": ".", "source": "mcp" }))
        .unwrap();
    assert_eq!(reply["ok"], true, "{reply}");
    let conn = Connection::open(Paths::for_project(&project.0).db).unwrap();
    let (pid, start): (i64, Option<i64>) = conn
        .query_row("SELECT process_id, process_start FROM sessions WHERE session_id = ?1", [reply["result"]["sessionId"].as_str().unwrap()], |r| Ok((r.get(0)?, r.get(1)?)))
        .unwrap();
    assert_eq!(pid, i64::from(std::process::id()), "申告の pid が採られた");
    assert!(start.is_some());
    // フックの接続のセッションは、短命なのでプロセスを持たない
    let mut hook = service.connect("hook", None);
    let reply = hook
        .request(3, "session_begin", json!({ "provider": "claude", "workspace": ".", "source": "terminal" }))
        .unwrap();
    let (pid, start): (Option<i64>, Option<i64>) = conn
        .query_row("SELECT process_id, process_start FROM sessions WHERE session_id = ?1", [reply["result"]["sessionId"].as_str().unwrap()], |r| Ok((r.get(0)?, r.get(1)?)))
        .unwrap();
    assert_eq!((pid, start), (None, None));
    // Editor は記録の op を呼べない
    let mut editor = service.connect("editor", None);
    let denied = editor.request(4, "session_begin", json!({ "provider": "claude", "workspace": ".", "source": "terminal" })).unwrap();
    assert_eq!(denied["error"]["code"], "forbidden");
    let denied = hook.request(5, "report", json!({ "sessionId": "x", "paths": ["a"] })).unwrap();
    assert_eq!(denied["error"]["code"], "forbidden");
}

#[test]
fn editor_saved_over_the_pipe_is_for_the_editor_only_and_marks_the_change() {
    let root = temp_root("saved_pipe");
    fs::write(root.0.join("a.txt"), b"a0\n").unwrap();
    let service = ServiceProc::spawn(&root.0, 60_000);
    service.wait_watching();
    fs::write(root.0.join("a.txt"), b"a1 saved in the editor\n").unwrap();
    let args = json!({ "path": "a.txt", "hash": sha256_hex(b"a1 saved in the editor\n") });

    // AI 側の役割は、Editor の保存を名乗れない
    for role in ["mcp", "hook"] {
        let mut other = service.connect(role, None);
        let denied = other.request(2, "editor_saved", args.clone()).unwrap();
        assert_eq!(denied["error"]["code"], "forbidden", "{role}: {denied}");
    }
    let mut editor = service.connect("editor", None);
    let reply = editor.request(2, "editor_saved", args).unwrap();
    assert_eq!(reply["ok"], true, "{reply}");
    assert_eq!(reply["result"]["path"], "a.txt");

    // 一覧の行為者が Editor の利用者になる (観測が先でも後でも。ここは書いた直後に知らせた)
    let mut actor = String::new();
    wait_until("a.txt が editor_user になる", || {
        let list = editor.request(3, "list", json!({ "path": "a.txt" })).unwrap();
        actor = list["result"]["items"][0]["actor"].as_str().unwrap_or("").to_string();
        actor == "editor_user"
    });
    let list = editor.request(4, "list", json!({ "attribution": ["unknown", "inferred"] })).unwrap();
    assert!(list["result"]["items"].as_array().unwrap().is_empty(), "{list}");
}

#[test]
fn the_service_does_not_idle_out_while_a_session_is_running() {
    let project = temp_root("idle_session");
    let mut service = ServiceProc::spawn(&project.0, 1500);
    service.wait_watching();
    let mut hook = service.connect("hook", None);
    let begin = hook.request(2, "session_begin", json!({ "provider": "claude", "workspace": ".", "source": "terminal", "clientSessionId": "idle" })).unwrap();
    assert_eq!(begin["ok"], true, "{begin}");
    drop(hook);
    // 接続は 0 だが、running のセッションがある間は時間切れの値を過ぎても終わらない
    std::thread::sleep(Duration::from_millis(4500));
    assert!(service.child.try_wait().unwrap().is_none(), "running のセッションがあるのにサービスが終わった");
    // セッションを終えれば、無接続の時間切れで終わる
    let mut hook = service.connect("hook", None);
    let end = hook.request(2, "session_end", json!({ "clientSessionId": "idle" })).unwrap();
    assert_eq!(end["ok"], true, "{end}");
    drop(hook);
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        if let Some(status) = service.child.try_wait().unwrap() {
            assert_eq!(status.code(), Some(0));
            break;
        }
        assert!(Instant::now() < deadline, "セッションが無くなったのに終わらない");
        std::thread::sleep(Duration::from_millis(50));
    }
}


// ---- 復元の記録 (M104p2): restore_expect / restore_record -------------------------------------------------------

fn expect(fx: &Fx, id: &str, path: &str, bytes: Option<&[u8]>) -> Value {
    let hash = bytes.map(sha256_hex);
    fx.ok("restore_expect", EDITOR, json!({ "restoreId": id, "paths": [path], "hashes": [hash] }))
}

fn restore_state(conn: &Connection, id: &str) -> (String, Option<i64>) {
    conn.query_row("SELECT state, finished_at FROM restore_operations WHERE restore_id = ?1", [id], |r| Ok((r.get(0)?, r.get(1)?))).unwrap()
}

fn change_state(conn: &Connection, path: &str) -> Vec<String> {
    let mut stmt = conn.prepare("SELECT state FROM changes WHERE path = ?1 ORDER BY change_id").unwrap();
    let rows = stmt.query_map([path], |r| r.get::<_, String>(0)).unwrap();
    rows.map(Result::unwrap).collect()
}

#[test]
fn an_expected_restore_makes_the_change_the_restores_whether_it_is_observed_first_or_second() {
    let mut fx = Fx::new("restore_expect");
    fx.write("before.txt", b"b0\n");
    fx.write("after.txt", b"a0\n");
    fx.write("gone.txt", b"g0\n");
    fx.start(cfg(2000));
    // 予告が先 (書く前に届く。Editor の実際の順序)
    expect(&fx, "r1", "before.txt", Some(b"b1\n"));
    fx.write("before.txt", b"b1\n");
    wait_until("before.txt", || fx.revision_count("before.txt") == 1);
    let rev = fx.last("before.txt");
    assert_eq!(verdict(&rev), ("restore", "verified"), "{:?}", fx.evidence(rev.id));
    assert_eq!(fx.evidence(rev.id), ["observed", "restore_expected"]);
    // 観測が先
    fx.write("after.txt", b"a1\n");
    wait_until("after.txt", || fx.revision_count("after.txt") == 1);
    assert_eq!(verdict(&fx.last("after.txt")), ("unknown", "unknown"));
    expect(&fx, "r2", "after.txt", Some(b"a1\n"));
    assert_eq!(verdict(&fx.last("after.txt")), ("restore", "verified"), "{:?}", fx.evidence(fx.last("after.txt").id));
    // 削除の予告 (内容は null)
    expect(&fx, "r3", "gone.txt", None);
    fs::remove_file(fx.root.join("gone.txt")).unwrap();
    fx.model.remove("gone.txt");
    wait_until("gone.txt", || fx.revision_count("gone.txt") == 1);
    assert_eq!(verdict(&fx.last("gone.txt")), ("restore", "verified"));
}

#[test]
fn an_expected_restore_with_another_content_or_path_does_not_match_and_the_editor_save_keeps_priority() {
    let mut fx = Fx::new("restore_expect_miss");
    fx.write("a.txt", b"a0\n");
    fx.write("b.txt", b"b0\n");
    fx.write("c.txt", b"c0\n");
    fx.start(cfg(2000));
    expect(&fx, "r1", "a.txt", Some(b"something else\n"));
    expect(&fx, "r1", "b.txt", Some(b"b1\n")); // 別のパスの予告
    fx.write("a.txt", b"a1\n");
    wait_until("a.txt", || fx.revision_count("a.txt") == 1);
    assert_eq!(verdict(&fx.last("a.txt")), ("unknown", "unknown"), "内容が違う予告で restore にした");
    fx.write("b.txt", b"b2 by someone\n");
    wait_until("b.txt", || fx.revision_count("b.txt") == 1);
    assert_eq!(verdict(&fx.last("b.txt")), ("unknown", "unknown"));
    // 同じ内容に editor_saved と restore_expect の両方: attr §3 の表の先頭 (editor_saved) が勝つ
    expect(&fx, "r2", "c.txt", Some(b"c1\n"));
    saved(&fx, "c.txt", b"c1\n");
    fx.write("c.txt", b"c1\n");
    wait_until("c.txt", || fx.revision_count("c.txt") == 1);
    let rev = fx.last("c.txt");
    assert_eq!(verdict(&rev), ("editor_user", "verified"), "{:?}", fx.evidence(rev.id));
}

#[test]
fn a_committed_restore_marks_the_targets_and_leaves_the_review_alone() {
    let mut fx = Fx::new("restore_record");
    let lines = |a: &str, b: &str| -> Vec<u8> {
        let mut text = String::new();
        for i in 1..=24 {
            text.push_str(&match i {
                2 => format!("{a}\n"),
                22 => format!("{b}\n"),
                _ => format!("line{i}\n"),
            });
        }
        text.into_bytes()
    };
    fx.write("r.txt", &lines("line2", "line22"));
    fx.start(cfg(2000));
    fx.write("r.txt", &lines("AI2", "AI22"));
    wait_until("r.txt", || fx.revision_count("r.txt") == 1);
    let rev = fx.last("r.txt");
    let conn = fx.db();
    let hunks: Vec<i64> = {
        let mut stmt = conn.prepare("SELECT hunk_id FROM change_hunks WHERE revision_id = ?1 ORDER BY ordinal").unwrap();
        let rows = stmt.query_map([rev.id], |r| r.get(0)).unwrap();
        rows.map(Result::unwrap).collect()
    };
    assert_eq!(hunks.len(), 2, "前提: Hunk が 2 つ");
    fx.ok("review_set", EDITOR, json!({ "targets": [{ "revisionId": rev.id, "scope": { "kind": "revision" } }], "state": "approved" }));
    let reviews_before = dump(&conn, "SELECT * FROM reviews ORDER BY review_id");
    let pre_hash = sha256_hex(&lines("AI2", "AI22"));
    let record = |state: &str, targets: Value| {
        let mut a = json!({
            "restoreId": "rr-1", "state": state, "targets": targets,
            "preHashes": { "r.txt": { "hash": pre_hash, "size": 10 } },
        });
        if state == "committed" {
            a["postHashes"] = json!({ "r.txt": sha256_hex(&lines("line2", "AI22")) });
        }
        fx.ok("restore_record", EDITOR, a)
    };
    let hunk_one = json!([{ "revisionId": rev.id, "scope": "hunks", "hunkIds": [hunks[0]], "paths": ["r.txt"] }]);
    record("applying", hunk_one.clone());
    assert_eq!(restore_state(&fx.db(), "rr-1"), ("applying".to_string(), None));
    let snaps = dump(&fx.db(), "SELECT blob_hash, path, kind, blob_state FROM snapshots WHERE kind = 'pre_restore'");
    assert_eq!(snaps.len(), 1, "applying で適用前の内容を pre_restore のスナップショットにする: {snaps:?}");
    assert!(snaps[0].contains("present"), "{snaps:?}"); // 観測された内容なので Blob は既にある
    assert_eq!(change_state(&fx.db(), "r.txt"), ["active"], "committed までは状態を変えない");
    record("committed", hunk_one);
    let (state, finished) = restore_state(&fx.db(), "rr-1");
    assert_eq!(state, "committed");
    assert!(finished.is_some());
    assert_eq!(change_state(&fx.db(), "r.txt"), ["partially_restored"]);
    let hunk_states: Vec<String> = {
        let db = fx.db();
        let mut stmt = db.prepare("SELECT state FROM change_hunks WHERE revision_id = ?1 ORDER BY ordinal").unwrap();
        let rows = stmt.query_map([rev.id], |r| r.get(0)).unwrap();
        rows.map(Result::unwrap).collect()
    };
    assert_eq!(hunk_states, ["restored", "active"]);
    // 残りの Hunk も戻れば、変更全体が restored
    let hunk_two = json!([{ "revisionId": rev.id, "scope": "hunks", "hunkIds": [hunks[1]], "paths": ["r.txt"] }]);
    fx.ok("restore_record", EDITOR, json!({ "restoreId": "rr-2", "state": "committed", "targets": hunk_two }));
    assert_eq!(change_state(&fx.db(), "r.txt"), ["restored"]);
    // 巻き戻した復元は対象の状態を変えない
    fx.ok(
        "restore_record",
        EDITOR,
        json!({ "restoreId": "rr-3", "state": "rolled_back", "error": "x", "targets": [{ "revisionId": rev.id, "scope": "file", "paths": ["r.txt"] }] }),
    );
    assert_eq!(restore_state(&fx.db(), "rr-3").0, "rolled_back");
    // レビューの記録は 1 行も変わらない (要求 §10.5)
    assert_eq!(dump(&fx.db(), "SELECT * FROM reviews ORDER BY review_id"), reviews_before);
    // 履歴の op
    let reader = ops::open_reader(&Paths::for_project(&fx.root).db).unwrap();
    let list = ops::restore_list(&reader, args(json!({}))).unwrap();
    let items = list["items"].as_array().unwrap();
    assert_eq!(items.len(), 3, "{list}");
    assert_eq!(items.iter().filter(|i| i["state"] == "committed").count(), 2);
    assert_eq!(ops::restore_list(&reader, args(json!({ "limit": 0 }))).unwrap_err().code, code::BAD_ARGS);
}

#[test]
fn restore_ops_reject_bad_arguments_and_only_the_editor_may_call_them() {
    let mut fx = Fx::new("restore_args");
    fx.write("a.txt", b"a\n");
    fx.start(cfg(2000));
    let good = sha256_hex(b"a\n");
    let target = json!([{ "revisionId": 1, "scope": "file", "paths": ["a.txt"] }]);
    for (name, a) in [
        ("state が知らない値", json!({ "restoreId": "x", "state": "weird", "targets": target })),
        ("restoreId が不正", json!({ "restoreId": "a/b", "state": "applying", "targets": target })),
        ("targets が空", json!({ "restoreId": "x", "state": "applying", "targets": [] })),
        ("scope が不正", json!({ "restoreId": "x", "state": "applying", "targets": [{ "revisionId": 1, "scope": "all" }] })),
        ("外のパス", json!({ "restoreId": "x", "state": "applying", "targets": [{ "revisionId": 1, "scope": "file", "paths": ["../o.txt"] }] })),
        ("preHashes の値が不正", json!({ "restoreId": "x", "state": "applying", "targets": target, "preHashes": { "a.txt": "zz" } })),
        ("知らない引数", json!({ "restoreId": "x", "state": "applying", "targets": target, "extra": 1 })),
    ] {
        let e = fx.call("restore_record", EDITOR, a).unwrap_err();
        assert_eq!(e.code, code::BAD_ARGS, "{name}: {}", e.detail);
    }
    for (name, a) in [
        ("hash が短い", json!({ "restoreId": "x", "paths": ["a.txt"], "hashes": ["abc"] })),
        ("長さが違う", json!({ "restoreId": "x", "paths": ["a.txt"], "hashes": [] })),
        ("外のパス", json!({ "restoreId": "x", "paths": ["../o.txt"], "hashes": [good] })),
        ("paths が無い", json!({ "restoreId": "x", "hashes": [good] })),
    ] {
        let e = fx.call("restore_expect", EDITOR, a).unwrap_err();
        assert_eq!(e.code, code::BAD_ARGS, "{name}: {}", e.detail);
    }
    for role in [Role::Mcp, Role::Hook] {
        for op in ["restore_record", "restore_expect", "restore_list"] {
            assert!(!role_allows(role, op), "{role:?} が {op} を呼べる");
        }
    }
    assert!(fx.call("restore_expect", EDITOR, json!({ "restoreId": "x", "paths": ["a.txt"], "hashes": [good.to_ascii_uppercase()] })).is_ok());
}

#[test]
fn a_broken_db_is_rebuilt_with_the_restore_records_and_expectations() {
    let mut fx = Fx::new("restore_rebuild");
    fx.write("a.txt", b"a0\n");
    fx.write("b.txt", b"b0\n");
    fx.start(cfg(2000));
    // 予告が先 / 観測が先、の両方と、committed までの記録
    expect(&fx, "rb1", "a.txt", Some(b"a1\n"));
    fx.write("a.txt", b"a1\n");
    wait_until("a.txt", || fx.revision_count("a.txt") == 1);
    fx.write("b.txt", b"b1\n");
    wait_until("b.txt", || fx.revision_count("b.txt") == 1);
    expect(&fx, "rb2", "b.txt", Some(b"b1\n"));
    let rev = fx.last("a.txt");
    let targets = json!([{ "revisionId": rev.id, "scope": "file", "paths": ["a.txt"] }]);
    fx.ok(
        "restore_record",
        EDITOR,
        json!({ "restoreId": "rb1", "state": "applying", "targets": targets, "preHashes": { "a.txt": { "hash": sha256_hex(b"a1\n"), "size": 3 } } }),
    );
    fx.ok(
        "restore_record",
        EDITOR,
        json!({ "restoreId": "rb1", "state": "committed", "targets": targets, "postHashes": { "a.txt": sha256_hex(b"a0\n") } }),
    );
    for p in ["a.txt", "b.txt"] {
        assert_eq!(verdict(&fx.last(p)), ("restore", "verified"), "{p}: 前提");
    }
    fx.stop();

    drop(open_project(&fx.root, now_ms()).0);
    let snapshot = |conn: &Connection| {
        let mut t = attribution_tables(conn);
        t.insert("restore_operations", dump(conn, "SELECT * FROM restore_operations ORDER BY restore_id"));
        t.insert("changes", dump(conn, "SELECT path, state FROM changes ORDER BY change_id"));
        t.insert("change_hunks", dump(conn, "SELECT revision_id, ordinal, state FROM change_hunks ORDER BY hunk_id"));
        t
    };
    let before = snapshot(&fx.db());
    assert_eq!(before["restore_operations"].len(), 1, "{before:#?}");
    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert!(report.broken_db.is_some() && report.rebuilt.is_some(), "{report:?}");
    assert!(report.failed_steps.is_empty(), "{:?}", report.failed_steps);
    drop(store);
    let after = snapshot(&fx.db());
    for (table, rows) in &before {
        assert_eq!(rows, &after[table], "{table} が壊す前と違う");
    }
    assert_eq!(count_evidence(&fx.db(), "restore_expected"), 2);
}

// ---- ライブのシーンの復元の記録 (M104q): restore_record の scope = scene ---------------------------------------

fn scene_row_states(conn: &Connection) -> Vec<String> {
    dump(conn, "SELECT scene_change_id, change_type, state FROM scene_changes ORDER BY scene_change_id")
}

#[test]
fn a_committed_scene_restore_marks_only_the_restored_scene_rows_and_survives_a_rebuild() {
    let mut fx = Fx::new("scene_restore");
    let scene = |text: &str, name: &str| {
        format!("{{\"engine\":\"MyEngine\",\"entities\":[{{\"childIndex\":0,\"components\":{{\"Note\":{{\"text\":\"{text}\"}}}},\"fileId\":1,\"name\":\"{name}\"}}],\"nextFileId\":2,\"sceneName\":\"s\",\"version\":4}}")
    };
    fx.write("a.scene.json.meta", b"{\"guid\":\"0123456789abcdef\",\"type\":\"scene\",\"version\":1}");
    fx.write("a.scene.json", scene("x", "A").as_bytes());
    fx.start(cfg(2000));
    fx.write("a.scene.json", scene("y", "A2").as_bytes());
    wait_until("a.scene.json", || fx.revision_count("a.scene.json") == 1);
    let rev = fx.last("a.scene.json");
    let ids: Vec<i64> = {
        let conn = fx.db();
        let mut stmt = conn.prepare("SELECT scene_change_id FROM scene_changes WHERE revision_id = ?1 ORDER BY scene_change_id").unwrap();
        let rows = stmt.query_map([rev.id], |r| r.get(0)).unwrap();
        rows.map(Result::unwrap).collect()
    };
    assert_eq!(ids.len(), 2, "前提: フィールドの変更と名前の変更の 2 行: {ids:?}");
    fx.ok("review_set", EDITOR, json!({ "targets": [{ "revisionId": rev.id, "scope": { "kind": "revision" } }], "state": "approved" }));
    let reviews_before = dump(&fx.db(), "SELECT * FROM reviews ORDER BY review_id");
    let changes_before = change_state(&fx.db(), "a.scene.json");
    let target = |restored: &[i64]| json!([{ "revisionId": rev.id, "scope": "scene", "sceneChangeIds": restored, "paths": ["a.scene.json"] }]);
    // 巻き戻した記録 (rolled_back) と、まだ committed でない記録は、行の状態を変えない
    fx.ok("restore_record", EDITOR, json!({ "restoreId": "sr-1", "state": "rolled_back", "error": "verify_failed", "targets": target(&[ids[0]]) }));
    assert!(scene_row_states(&fx.db()).iter().all(|row| row.contains("active")), "{:?}", scene_row_states(&fx.db()));
    fx.ok("restore_record", EDITOR, json!({ "restoreId": "sr-2", "state": "committed", "targets": target(&[ids[0]]) }));
    let states = scene_row_states(&fx.db());
    assert!(states[0].contains("restored") && states[1].contains("active"), "戻した行だけが restored: {states:?}");
    assert_eq!(change_state(&fx.db(), "a.scene.json"), changes_before, "変更 (ファイル) の状態は変えない: ファイルは戻していない");
    assert_eq!(dump(&fx.db(), "SELECT * FROM reviews ORDER BY review_id"), reviews_before, "レビューの記録は変わらない");
    {
        // (読み取りの接続は、DB を壊す前に閉じる)
        let reader = ops::open_reader(&Paths::for_project(&fx.root).db).unwrap();
        // 強調の表 (scene_overlay) は active の行だけを返す
        let overlay = mye_track::scene_overlay::scene_overlay(&reader, args(json!({ "documentId": "0123456789abcdef", "excludeApproved": false }))).unwrap();
        let entity = &overlay["entities"].as_array().unwrap()[0];
        assert_eq!(entity["components"].as_array().unwrap().len(), 0, "戻した Note.text は強調から消える: {overlay}");
        assert_eq!(entity["entityFields"].as_array().unwrap().len(), 1, "戻していない name は残る: {overlay}");
        // 履歴は scope = scene と sceneChangeIds を返し、ファイル・Hunk の記録の形は変えない
        let list = ops::restore_list(&reader, args(json!({}))).unwrap();
        let first = &list["items"].as_array().unwrap().iter().find(|i| i["restoreId"] == "sr-2").unwrap()["targets"][0];
        assert_eq!(first["scope"], "scene");
        assert_eq!(first["sceneChangeIds"], json!([ids[0]]));
    }
    // 引数の検査
    for bad in [json!("x"), json!([1.5]), json!([[]])] {
        let e = fx.call("restore_record", EDITOR, json!({ "restoreId": "sr-3", "state": "committed", "targets": [{ "revisionId": rev.id, "scope": "scene", "sceneChangeIds": bad }] })).unwrap_err();
        assert_eq!(e.code, code::BAD_ARGS);
    }
    assert_eq!(fx.call("restore_record", EDITOR, json!({ "restoreId": "sr-3", "state": "committed", "targets": [{ "revisionId": rev.id, "scope": "scenes" }] })).unwrap_err().code, code::BAD_ARGS);
    fx.stop();

    // DB を作り直しても、行の状態と履歴は同じ
    drop(open_project(&fx.root, now_ms()).0);
    let before = scene_row_states(&fx.db());
    let history_before = dump(&fx.db(), "SELECT restore_id, state, targets FROM restore_operations ORDER BY restore_id");
    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert!(report.broken_db.is_some() && report.rebuilt.is_some(), "{report:?}");
    assert!(report.failed_steps.is_empty(), "{:?}", report.failed_steps);
    drop(store);
    assert_eq!(scene_row_states(&fx.db()), before, "作り直したあとも restored の行が同じ");
    assert_eq!(dump(&fx.db(), "SELECT restore_id, state, targets FROM restore_operations ORDER BY restore_id"), history_before);
}
