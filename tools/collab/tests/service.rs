// 要求 → 応答の往復と、panic 隔離の検査。
//
// ★panic の検査が要点 — in-process 化 (spec §4.0) で失ったメモリ隔離の代わりに、
//   「worker が panic してもエディタは生き続け、以後の要求には service_dead が返る」
//   だけが残された防波堤になっている。ここが壊れると**エディタごと落ちる**。

use std::time::{Duration, Instant};

use serde_json::{json, Value};

use mye_collab::ops::State;
use mye_collab::protocol::{code, ErrorBody, PROTO_VERSION};
use mye_collab::worker::Service;

fn wait_line(svc: &Service) -> Value {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        if let Some(line) = svc.poll() {
            return serde_json::from_str(&line).expect("service must emit valid JSON");
        }
        assert!(Instant::now() < deadline, "no response within 10s");
        std::thread::sleep(Duration::from_millis(2));
    }
}

/// テスト用 dispatcher: args をそのまま返す / "boom" で panic する
fn echo_dispatcher(_state: &mut State, op: &str, args: &Value) -> Result<Value, ErrorBody> {
    match op {
        "echo" => Ok(args.clone()),
        "boom" => panic!("intentional test panic"),
        "fail" => Err(ErrorBody::new(code::NOT_REPO, "nope")),
        other => Err(ErrorBody::new(code::BAD_REQUEST, format!("unknown op: {other}"))),
    }
}

#[test]
fn proto_version_is_one() {
    // C++ の kCollabProtoVersion と対。値を変えるときは
    // src\Editor\SourceControl\CollabProtocol.h と**同時に**変えること
    assert_eq!(PROTO_VERSION, 1);
}

#[test]
fn request_response_roundtrip() {
    let svc = Service::with_dispatcher(".".to_string(), echo_dispatcher);
    svc.request(json!({ "id": 7, "op": "echo", "args": { "hi": 1 } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["id"], 7);
    assert_eq!(r["ok"], true);
    assert_eq!(r["result"]["hi"], 1);
    assert!(r.get("error").is_none(), "ok の応答に error を載せない");
}

#[test]
fn error_response_carries_code() {
    let svc = Service::with_dispatcher(".".to_string(), echo_dispatcher);
    svc.request(json!({ "id": 1, "op": "fail" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], false);
    assert_eq!(r["error"]["code"], code::NOT_REPO);
    assert!(r.get("result").is_none());
}

#[test]
fn malformed_json_gets_bad_request_not_silence() {
    // 黙って捨てると C++ 側のコールバックが永久に残る (窓が「実行中」で固まる)
    let svc = Service::with_dispatcher(".".to_string(), echo_dispatcher);
    svc.request("{\"id\": 42, \"op\": ".to_string());
    let r = wait_line(&svc);
    assert_eq!(r["id"], 42, "壊れた JSON でも id だけは拾う");
    assert_eq!(r["error"]["code"], code::BAD_REQUEST);
}

#[test]
fn unknown_op_is_bad_request() {
    // 本物の dispatcher (git を呼ばない経路)
    let svc = Service::new(".".to_string());
    svc.request(json!({ "id": 3, "op": "no_such_op" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["error"]["code"], code::BAD_REQUEST);
}

#[test]
fn panic_is_isolated_and_marks_service_dead() {
    let svc = Service::with_dispatcher(".".to_string(), echo_dispatcher);
    svc.request(json!({ "id": 1, "op": "boom" }).to_string());

    // 1) service_error 通知が 1 回
    let ev = wait_line(&svc);
    assert_eq!(ev["event"], "service_error");
    assert_eq!(ev["code"], code::INTERNAL_PANIC);

    // 2) 実行中だった要求にも応答が返る (返さないと UI が固まる)
    let r = wait_line(&svc);
    assert_eq!(r["id"], 1);
    assert_eq!(r["error"]["code"], code::INTERNAL_PANIC);

    // 3) 以後の要求は service_dead
    assert!(svc.is_dead());
    svc.request(json!({ "id": 2, "op": "echo" }).to_string());
    let r2 = wait_line(&svc);
    assert_eq!(r2["id"], 2);
    assert_eq!(r2["error"]["code"], code::SERVICE_DEAD);
}

#[test]
fn hello_reports_git_version() {
    // 実 git を 1 回だけ叩く (CI ランナーにも開発機にも git はある = checkout に使われる)。
    // リポジトリでなくても hello は通る契約なので cwd は何でもよい
    let svc = Service::new(".".to_string());
    svc.request(json!({ "id": 1, "op": "hello", "args": { "fetchIntervalMin": 5, "autoFetch": false } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "hello failed: {r}");
    let ver = r["result"]["gitVersion"].as_str().unwrap_or("");
    assert!(!ver.is_empty(), "gitVersion is empty");
    assert!(ver.starts_with(char::is_numeric), "unexpected version: {ver}");
}

#[test]
fn status_outside_a_repo_is_not_repo() {
    // 一時ディレクトリ (git 管理外) で status → not_repo。
    // ここを「空の status」で返すと UI が**清浄なリポジトリ**と誤表示する
    let dir = std::env::temp_dir().join("mye_collab_not_a_repo");
    std::fs::create_dir_all(&dir).unwrap();
    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "status" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], false);
    assert_eq!(r["error"]["code"], code::NOT_REPO);
}
