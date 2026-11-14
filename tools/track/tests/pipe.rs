//! pipe のサービスの結合テスト (M104f)。実際に `mye_track.exe serve` を起動して pipe で話す。
//!
//! 起動したプロセスは `ServiceProc` の Drop で必ず止める (失敗しても残さない)。
//! 待ちは固定の sleep ではなく、上限付きの `wait_until` で書く。

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use rusqlite::Connection;
use serde_json::{json, Value};

use mye_track::client::{read_service_json, Client};
use mye_track::pipe::{current_user_sid, open_client, pipe_name, read_dacl_sddl};
use mye_track::project::project_id;
use mye_track::startup::open_project;
use mye_track::store::Paths;

const WAIT_LIMIT: Duration = Duration::from_secs(60);

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

/// テストごとの空のプロジェクトルート。終わりに消す (失敗したときは調べられるよう残す)。
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
            // サービスの終了直後は DB のハンドルが残ることがあるので、少し待ってやり直す
            for _ in 0..50 {
                if fs::remove_dir_all(&self.0).is_ok() || !self.0.exists() {
                    return;
                }
                std::thread::sleep(Duration::from_millis(100));
            }
        }
    }
}

fn temp_project(name: &str) -> TempProject {
    let dir = std::env::temp_dir().join(format!("mye_track_pipe_{}_{}", name, std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    TempProject(dir)
}

/// 起動した `mye_track.exe serve`。Drop で、まだ動いていれば強制終了する。
struct ServiceProc {
    child: Child,
    root: PathBuf,
    stderr_path: PathBuf,
}

impl ServiceProc {
    fn spawn_with(root: &Path, project_arg: &Path, idle_ms: u64) -> ServiceProc {
        Self::spawn_full(root, project_arg, idle_ms, &[])
    }

    /// `serve` に追加の引数を渡して起動する。
    fn spawn_opts(root: &Path, extra: &[&str]) -> ServiceProc {
        Self::spawn_full(root, root, 120_000, extra)
    }

    fn spawn_full(root: &Path, project_arg: &Path, idle_ms: u64, extra: &[&str]) -> ServiceProc {
        static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let n = COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        let stderr_path = std::env::temp_dir().join(format!("mye_track_pipe_err_{}_{n}.txt", std::process::id()));
        let child = Command::new(env!("CARGO_BIN_EXE_mye_track"))
            .args(["serve", "--project"])
            .arg(project_arg)
            .args(["--idle-timeout-ms", &idle_ms.to_string()])
            .args(extra)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(fs::File::create(&stderr_path).unwrap())
            .spawn()
            .expect("mye_track.exe を起動できない");
        ServiceProc { child, root: root.to_path_buf(), stderr_path }
    }

    /// 後始末のため、アイドルの時間切れは長めにしておく (テストの途中で自分で終わらない)。
    fn spawn(root: &Path) -> ServiceProc {
        Self::spawn_with(root, root, 120_000)
    }

    fn pipe(&self) -> String {
        pipe_name(&project_id(&self.root))
    }

    fn token(&self) -> String {
        let mut token = None;
        wait_until("service.json", WAIT_LIMIT, || {
            token = read_service_json(&self.root).and_then(|v| v["token"].as_str().map(str::to_string));
            token.is_some()
        });
        token.unwrap()
    }

    /// hello まで済ませた接続を作る。
    fn connect(&self, role: &str) -> Client {
        let token = self.token();
        let mut client = Client::connect(&self.pipe(), WAIT_LIMIT).expect("pipe に繋がる");
        let reply = client
            .request(
                1,
                "hello",
                json!({ "proto": 1, "role": role, "token": token, "projectRoot": self.root.to_string_lossy() }),
            )
            .unwrap();
        assert_eq!(reply["ok"], true, "{reply}");
        client
    }

    /// ベースラインが終わり、監視に入るまで待つ。
    fn wait_watching(&self) {
        let mut client = self.connect("mcp");
        wait_until("phase = watching", WAIT_LIMIT, || {
            client.request(2, "status", json!({})).unwrap()["result"]["phase"] == "watching"
        });
    }

    fn wait_exit(&mut self, limit: Duration) -> std::process::ExitStatus {
        let deadline = Instant::now() + limit;
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                return status;
            }
            assert!(Instant::now() < deadline, "サービスが終わらない");
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    fn stderr_text(&self) -> String {
        fs::read_to_string(&self.stderr_path).unwrap_or_default()
    }

    /// pipe の `shutdown` で止め、終了コードを返す。
    fn shutdown(&mut self) -> i32 {
        let mut client = self.connect("editor");
        let reply = client.request(9, "shutdown", json!({})).unwrap();
        assert_eq!(reply["ok"], true, "{reply}");
        self.wait_exit(WAIT_LIMIT).code().unwrap_or(-1)
    }
}

impl Drop for ServiceProc {
    fn drop(&mut self) {
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
        let _ = fs::remove_file(&self.stderr_path);
    }
}

fn list_items(client: &mut Client, args: Value) -> Vec<Value> {
    let reply = client.request(3, "list", args).unwrap();
    assert_eq!(reply["ok"], true, "{reply}");
    reply["result"]["items"].as_array().unwrap().clone()
}

#[test]
fn second_instance_for_the_same_project_exits_while_another_project_runs() {
    let project = temp_project("single");
    let other = temp_project("single_other");
    let first = ServiceProc::spawn(&project);
    first.wait_watching();

    let mut second = ServiceProc::spawn(&project);
    let status = second.wait_exit(Duration::from_secs(20));
    assert_eq!(status.code(), Some(3), "{}", second.stderr_text());
    assert!(second.stderr_text().contains("既に動いている"), "{}", second.stderr_text());

    // 1 つ目は動き続け、記録も続く
    fs::write(project.join("a.txt"), b"one\n").unwrap();
    let mut client = first.connect("mcp");
    wait_until("a.txt の Revision", WAIT_LIMIT, || !list_items(&mut client, json!({ "path": "a.txt" })).is_empty());

    // 別のプロジェクトのサービスは同時に動ける
    let mut third = ServiceProc::spawn(&other);
    third.wait_watching();
    assert!(third.child.try_wait().unwrap().is_none());
    assert_eq!(third.shutdown(), 0);
}

#[test]
fn second_instance_via_an_alias_path_exits_before_touching_the_db() {
    let project = temp_project("alias");
    let alias_parent = temp_project("alias_link");
    let alias = alias_parent.join("junction");
    let made = Command::new("cmd").args(["/C", "mklink", "/J"]).arg(&alias).arg(&*project).output().unwrap();
    assert!(made.status.success(), "ジャンクションを作れない: {}", String::from_utf8_lossy(&made.stdout));

    let first = ServiceProc::spawn(&project);
    first.wait_watching();
    assert_ne!(project_id(&project), project_id(&alias), "別名で projectId が同じなら、このテストは service.lock を試せない");

    let mut second = ServiceProc::spawn_with(&alias, &alias, 120_000);
    let status = second.wait_exit(Duration::from_secs(20));
    let err = second.stderr_text();
    assert_eq!(status.code(), Some(3), "{err}");
    assert!(err.contains("service.lock"), "{err}");
    assert!(err.contains(&project.to_string_lossy().to_string()), "動いているサービスの projectRoot を出していない: {err}");
    // DB を開く前に終わっている (起動時の検査の報告 = 標準エラーの StartupReport が無い)
    assert!(!err.contains("StartupReport"), "{err}");

    fs::write(project.join("b.txt"), b"x\n").unwrap();
    let mut client = first.connect("mcp");
    wait_until("1 つ目の記録が続く", WAIT_LIMIT, || !list_items(&mut client, json!({ "path": "b.txt" })).is_empty());
    // ジャンクションはリンクだけを外す (行き先は消さない)
    fs::remove_dir(&alias).unwrap();
    assert!(project.join("b.txt").exists());
}

#[test]
fn history_survives_a_restart_and_the_down_time_is_a_gap() {
    let project = temp_project("restart");
    fs::write(project.join("keep.txt"), b"keep\n").unwrap();
    let mut first = ServiceProc::spawn(&project);
    first.wait_watching();

    fs::write(project.join("a.txt"), b"one\n").unwrap();
    let mut client = first.connect("mcp");
    wait_until("a.txt の Revision", WAIT_LIMIT, || !list_items(&mut client, json!({ "path": "a.txt" })).is_empty());
    let before = list_items(&mut client, json!({}));
    drop(client);
    assert_eq!(first.shutdown(), 0);

    // 止まっている間の変更
    fs::write(project.join("b.txt"), b"new while down\n").unwrap();
    fs::write(project.join("a.txt"), b"one\ntwo\n").unwrap();

    let second = ServiceProc::spawn(&project);
    let mut client = second.connect("mcp");
    wait_until("止まっていた間の変更", WAIT_LIMIT, || list_items(&mut client, json!({})).len() >= before.len() + 2);
    let after = list_items(&mut client, json!({}));

    // 止める前の履歴が同じ Revision のまま取れる
    let old = before.iter().find(|i| i["path"] == "a.txt").unwrap();
    let kept = after.iter().find(|i| i["revisionId"] == old["revisionId"]).expect("止める前の Revision が消えた");
    assert_eq!(kept["completeness"], "complete", "{kept}");
    assert_eq!(kept["changeType"], "added");
    let detail = client.request(4, "describe", json!({ "revisionId": old["revisionId"] })).unwrap();
    assert_eq!(detail["ok"], true, "{detail}");
    assert_eq!(detail["result"]["after"]["size"], 4);

    // 止まっていた間の変更は gap として区別できる
    let down_b = after.iter().find(|i| i["path"] == "b.txt").expect("b.txt");
    assert_eq!((down_b["completeness"].as_str(), down_b["changeType"].as_str()), (Some("gap"), Some("added")), "{down_b}");
    let down_a = after.iter().find(|i| i["path"] == "a.txt" && i["changeType"] == "modified").expect("a.txt の modified");
    assert_eq!(down_a["completeness"], "gap", "{down_a}");
    let status = client.request(5, "status", json!({})).unwrap();
    let gaps = status["result"]["gaps"]["recent"].as_array().unwrap();
    assert!(gaps.iter().any(|g| g["reason"] == "service_down"), "{status}");
    // 監視の本数が見える (このプロジェクトは .mye を持つのでルート 1 本 + 無視しない子 0 本以上)
    assert!(status["result"]["watches"]["count"].as_u64().unwrap() >= 1, "{status}");
    assert_eq!(status["result"]["watches"]["fallback"], false, "{status}");
}

#[test]
fn idle_service_stops_itself_and_leaves_service_stop() {
    let project = temp_project("idle");
    let mut service = ServiceProc::spawn_with(&project, &project, 1500);
    let client = service.connect("mcp");
    // 接続を持っている間は、時間切れの値を過ぎても終わらない
    std::thread::sleep(Duration::from_millis(2500));
    assert!(service.child.try_wait().unwrap().is_none(), "接続があるのに終わった: {}", service.stderr_text());
    drop(client);
    let status = service.wait_exit(Duration::from_secs(30));
    assert_eq!(status.code(), Some(0), "{}", service.stderr_text());

    let conn = Connection::open(Paths::for_project(&project).db).unwrap();
    let reason: String = conn
        .query_row("SELECT payload FROM events WHERE kind = 'service_stop' ORDER BY event_id DESC LIMIT 1", [], |r| r.get(0))
        .expect("service_stop が残っていない");
    let payload: Value = serde_json::from_str(&reason).unwrap();
    assert_eq!(payload["reason"], "idle_timeout");
    assert!(!Paths::for_project(&project).dir.join("service.json").exists(), "service.json が残った");
}

#[test]
fn pipe_is_owner_only_and_refuses_remote_clients() {
    let project = temp_project("acl");
    let service = ServiceProc::spawn(&project);
    service.wait_watching();
    let name = service.pipe();

    // 読み戻した DACL は、自分の SID だけの許可 1 件で、継承を切ってある (保護 = `D:P`)
    let file = open_client(&name).unwrap();
    let sddl = read_dacl_sddl(&file).unwrap();
    let sid = current_user_sid().unwrap();
    assert!(sddl.starts_with("D:P("), "{sddl}");
    assert_eq!(sddl.matches('(').count(), 1, "許可が 1 件ではない: {sddl}");
    assert!(sddl.starts_with("D:P(A;") && sddl.ends_with(&format!(";;;{sid})")), "{sddl}");
    drop(file);

    // `\\localhost\pipe\...` (リモート経由の形) は開けない。PIPE_REJECT_REMOTE_CLIENTS の有無は
    // 試作 1 で、フラグなしなら開けることと比べて確かめた (prototype-results.md)
    let remote = name.replacen(r"\\.\pipe", r"\\localhost\pipe", 1);
    let result = fs::OpenOptions::new().read(true).write(true).open(&remote);
    assert!(result.is_err(), "リモート形式で開けた: {remote}");
}

#[test]
fn several_connections_work_at_once_and_changes_can_be_described() {
    let project = temp_project("multi");
    let service = ServiceProc::spawn(&project);
    service.wait_watching();
    let mut editor = service.connect("editor");
    let mut mcp = service.connect("mcp");

    fs::write(project.join("note.txt"), "line1\nline2\nline3\n").unwrap();
    wait_until("note.txt の Revision", WAIT_LIMIT, || !list_items(&mut mcp, json!({ "path": "note.txt" })).is_empty());
    fs::write(project.join("note.txt"), "line1\nCHANGED\nline3\n").unwrap();
    wait_until("note.txt の 2 つ目", WAIT_LIMIT, || list_items(&mut editor, json!({ "path": "note.txt" })).len() == 2);

    let items = list_items(&mut editor, json!({ "path": "note.txt" }));
    // 新しい順
    assert_eq!(items[0]["changeType"], "modified");
    assert_eq!(items[1]["changeType"], "added");
    assert!(items[0].get("beforeText").is_none() && items[0].get("hunks").is_none(), "一覧に本文を入れた: {}", items[0]);
    let detail = mcp.request(7, "describe", json!({ "revisionId": items[0]["revisionId"] })).unwrap();
    assert_eq!(detail["ok"], true, "{detail}");
    let result = &detail["result"];
    assert_eq!(result["completeness"], "complete");
    assert_eq!(result["diffMode"], "lines");
    assert_eq!(result["restorable"], true);
    let hunks = result["hunks"].as_array().unwrap();
    assert_eq!(hunks.len(), 1, "{result}");
    assert_eq!((hunks[0]["oldStart"].as_i64(), hunks[0]["oldCount"].as_i64()), (Some(2), Some(1)));
    assert_eq!((hunks[0]["newStart"].as_i64(), hunks[0]["newCount"].as_i64()), (Some(2), Some(1)));
    assert!(hunks[0].get("beforeText").is_none(), "describe に Hunk の本文を入れた");
    assert_ne!(result["before"]["hash"], result["after"]["hash"]);

    // 存在しない Revision は not_found
    let missing = mcp.request(8, "describe", json!({ "revisionId": 999_999 })).unwrap();
    assert_eq!(missing["error"]["code"], "not_found");
}

#[test]
fn status_reports_failed_maintenance_steps_and_keeps_serving() {
    let project = temp_project("failed_steps_pipe");
    drop(open_project(&project, 1).0);
    let conn = Connection::open(Paths::for_project(&project).db).unwrap();
    conn.execute_batch("DROP TABLE sessions;").unwrap();
    drop(conn);

    let service = ServiceProc::spawn(&project);
    service.wait_watching();
    let mut client = service.connect("mcp");
    let status = client.request(2, "status", json!({})).unwrap();
    let db = &status["result"]["db"];
    assert_eq!(db["state"], "recording", "{status}");
    let steps = db["failedSteps"].as_array().unwrap();
    assert!(steps.iter().any(|s| s["step"] == "残った session の整理"), "{status}");
}

/// 通知を待つ。`matches` に合う最初の行を返し、合わない行 (別の通知) は読み捨てる。
fn expect_event(client: &mut Client, what: &str, matches: impl Fn(&Value) -> bool) -> Value {
    let deadline = Instant::now() + WAIT_LIMIT;
    while Instant::now() < deadline {
        if let Some(line) = client.wait_line(Duration::from_millis(500)).unwrap() {
            if matches(&line) {
                return line;
            }
        }
    }
    panic!("通知が来ない: {what}");
}

fn subscribe_all(client: &mut Client) {
    let reply = client
        .request(
            2,
            "subscribe",
            json!({ "events": ["change_recorded", "attribution_updated", "coverage_gap", "session_changed", "shutting_down"] }),
        )
        .unwrap();
    assert_eq!(reply["ok"], true, "{reply}");
}

#[test]
fn subscribe_is_editor_only_and_checks_its_arguments() {
    let project = temp_project("subscribe_args");
    let service = ServiceProc::spawn(&project);
    service.wait_watching();

    for role in ["mcp", "hook"] {
        let mut client = service.connect(role);
        let reply = client.request(2, "subscribe", json!({ "events": ["change_recorded"] })).unwrap();
        assert_eq!(reply["error"]["code"], "forbidden", "{role}: {reply}");
    }
    let mut editor = service.connect("editor");
    for (args, what) in [
        (json!({}), "events が無い"),
        (json!({ "events": [] }), "events が空"),
        (json!({ "events": ["nosuchevent"] }), "知らない通知"),
        (json!({ "events": ["resync"] }), "resync は選べない (必ず届く)"),
        (json!({ "events": "change_recorded" }), "events が配列でない"),
        (json!({ "events": ["change_recorded"], "extra": 1 }), "知らない引数"),
    ] {
        let reply = editor.request(2, "subscribe", args).unwrap();
        assert_eq!(reply["error"]["code"], "bad_args", "{what}: {reply}");
    }
    subscribe_all(&mut editor);
    // 2 回目は断る (接続は残り、通知も続く)
    let again = editor.request(3, "subscribe", json!({ "events": ["coverage_gap"] })).unwrap();
    assert_eq!(again["error"]["code"], "bad_args", "{again}");
    let status = editor.request(4, "status", json!({})).unwrap();
    assert_eq!(status["ok"], true, "{status}");
}

#[test]
fn subscribed_editor_receives_the_notifications() {
    let project = temp_project("notify");
    let mut service = ServiceProc::spawn(&project);
    service.wait_watching();
    let mut editor = service.connect("editor");
    subscribe_all(&mut editor);
    let mut mcp = service.connect("mcp");

    // session_changed
    let begun = mcp
        .request(2, "session_begin", json!({ "provider": "claude", "workspace": project.to_string_lossy(), "source": "mcp" }))
        .unwrap();
    assert_eq!(begun["ok"], true, "{begun}");
    let session_id = begun["result"]["sessionId"].as_str().unwrap().to_string();
    let event = expect_event(&mut editor, "session_changed running", |e| {
        e["event"] == "session_changed" && e["sessionId"] == session_id.as_str() && e["status"] == "running"
    });
    assert!(event.get("id").is_none(), "通知に id が付いている: {event}");

    // change_recorded
    fs::write(project.join("n.txt"), "one\n").unwrap();
    let recorded = expect_event(&mut editor, "change_recorded n.txt", |e| e["event"] == "change_recorded" && e["path"] == "n.txt");
    assert_eq!(recorded["changeType"], "added", "{recorded}");
    let revision_id = recorded["revisionId"].as_i64().unwrap();
    assert!(recorded["changeId"].as_i64().is_some() && recorded["attribution"].is_string(), "{recorded}");
    // 通知の revisionId は list の Revision と同じ
    let items = list_items(&mut mcp, json!({ "path": "n.txt" }));
    assert_eq!(items[0]["revisionId"].as_i64(), Some(revision_id));

    // attribution_updated: 知らせたあとの申告
    let reported = mcp.request(4, "report", json!({ "sessionId": session_id, "paths": ["n.txt"] })).unwrap();
    assert_eq!(reported["ok"], true, "{reported}");
    let updated = expect_event(&mut editor, "attribution_updated", |e| {
        e["event"] == "attribution_updated" && e["revisionId"].as_i64() == Some(revision_id)
    });
    assert!(updated["actor"].is_string() && updated["attribution"].is_string(), "{updated}");

    // session_changed (終了)
    let ended = mcp.request(5, "session_end", json!({ "sessionId": session_id })).unwrap();
    assert_eq!(ended["ok"], true, "{ended}");
    expect_event(&mut editor, "session_changed finished", |e| {
        e["event"] == "session_changed" && e["sessionId"] == session_id.as_str() && e["status"] == "finished"
    });

    // shutting_down: 終了の前に届く
    let mut stopper = service.connect("editor");
    let reply = stopper.request(9, "shutdown", json!({})).unwrap();
    assert_eq!(reply["ok"], true, "{reply}");
    let closing = expect_event(&mut editor, "shutting_down", |e| e["event"] == "shutting_down");
    assert_eq!(closing["reason"], "shutdown_op", "{closing}");
    service.wait_exit(WAIT_LIMIT);
}

#[test]
fn an_unread_subscriber_does_not_stop_recording_and_gets_one_resync() {
    let project = temp_project("notify_unread");
    let service = ServiceProc::spawn_opts(&project, &["--notify-queue", "4"]);
    service.wait_watching();
    let mut editor = service.connect("editor");
    let reply = editor.request(2, "subscribe", json!({ "events": ["change_recorded"] })).unwrap();
    assert_eq!(reply["ok"], true, "{reply}");

    // 読まないまま、pipe の緩衝 (64 KiB) を超える量の通知を出させる
    const FILES: usize = 500;
    let pad = "x".repeat(100);
    for n in 0..FILES {
        fs::write(project.join(format!("bulk_{n:04}_{pad}.txt")), "x\n").unwrap();
    }
    // 記録は通知の詰まりに関係なく進む
    let db = Paths::for_project(&project).db;
    wait_until("500 件の Revision", WAIT_LIMIT, || {
        Connection::open(&db)
            .and_then(|c| c.query_row("SELECT COUNT(*) FROM change_revisions", [], |r| r.get::<_, i64>(0)))
            .map(|n| n >= FILES as i64)
            .unwrap_or(false)
    });

    // 読み始めると、取りこぼしを知らせる resync が届く
    let mut resyncs = 0;
    let mut changes = 0;
    let deadline = Instant::now() + WAIT_LIMIT;
    while Instant::now() < deadline {
        match editor.wait_line(Duration::from_secs(2)).unwrap() {
            Some(line) if line["event"] == "resync" => resyncs += 1,
            Some(line) if line["event"] == "change_recorded" => changes += 1,
            Some(_) => {}
            None => break,
        }
    }
    assert!(resyncs >= 1, "resync が届かない (change_recorded は {changes} 件)");
    assert!(changes < FILES, "全部届いた (あふれていない): {changes}");
}
