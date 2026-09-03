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

// ---- M66b: 非 ASCII パス / 監視 → 通知 ----
//
// ★非 ASCII の検査は**ここでしかできない**。collab_verify.bat は cmd 経由で
//   stdout を読むので、コンソール CP (CP932) で復号されて化ける = ASCII 限定。
//   「core.quotepath=false のおかげで entries.path が生 UTF-8 で載る」ことは
//   cargo test で証明する。

use std::path::{Path, PathBuf};
use std::process::Command;

/// 一時ディレクトリに空の git リポジトリを作る。**global/system の設定は遮断する**
/// (開発者の hook / gpgsign / quotepath に検査が引きずられないため)。
/// 空 config はリポジトリの**外**に置く — 中に置くと自分が untracked として出る
fn temp_repo(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("mye_collab_{}_{}", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let empty_config = std::env::temp_dir().join(format!("mye_collab_{}_{}.gitconfig", name, std::process::id()));
    std::fs::write(&empty_config, b"").unwrap();
    git(&dir, &empty_config, &["init", "-q", "-b", "main"]);
    dir
}

fn git(dir: &Path, empty_config: &Path, args: &[&str]) {
    let out = Command::new("git")
        .args(args)
        .current_dir(dir)
        .env("GIT_CONFIG_GLOBAL", empty_config)
        .env("GIT_CONFIG_NOSYSTEM", "1")
        .env("GIT_TERMINAL_PROMPT", "0")
        .output()
        .expect("git must be on PATH");
    assert!(out.status.success(), "git {args:?} failed: {}", String::from_utf8_lossy(&out.stderr));
}

fn config_path_for(dir: &Path) -> PathBuf {
    PathBuf::from(format!("{}.gitconfig", dir.to_string_lossy()))
}

#[test]
fn non_ascii_paths_come_back_as_raw_utf8() {
    let dir = temp_repo("utf8");
    std::fs::create_dir_all(dir.join("assets/textures")).unwrap();
    // ファイル名も中身も非 ASCII。git の既定 (core.quotepath=true) だと
    // "\346\227\245..." のような 8 進エスケープで返ってきて UI に化けて出る
    std::fs::write(dir.join("assets/textures/日本語.png"), "テクスチャ").unwrap();

    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "status" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "status failed: {r}");
    let entries = r["result"]["entries"].as_array().expect("entries must be an array");
    assert_eq!(entries.len(), 1, "未追跡ファイル 1 件のはず: {r}");
    assert_eq!(entries[0]["path"], "assets/textures/日本語.png");
    assert_eq!(entries[0]["index"], "?");
    assert_eq!(entries[0]["worktree"], "?");

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn refresh_reports_head_moves_and_swallows_identical_status() {
    let dir = temp_repo("head");
    let cfg = config_path_for(&dir);
    let ident = [
        "-c", "user.name=mye",
        "-c", "user.email=mye@example.com",
        // 開発者の commit.gpgsign=true を遮断済みでも念のため明示する
        // (署名を要求されるとテストが**固まる**)
        "-c", "commit.gpgsign=false",
    ];
    std::fs::write(dir.join("a.txt"), "1").unwrap();
    git(&dir, &cfg, &["add", "-A"]);
    let mut commit1: Vec<&str> = ident.to_vec();
    commit1.extend_from_slice(&["commit", "-q", "-m", "one"]);
    git(&dir, &cfg, &commit1);
    std::fs::write(dir.join("a.txt"), "2").unwrap();
    git(&dir, &cfg, &["add", "-A"]);
    let mut commit2: Vec<&str> = ident.to_vec();
    commit2.extend_from_slice(&["commit", "-q", "-m", "two"]);
    git(&dir, &cfg, &commit2);

    let mut state = State::new(dir.clone());
    // 1 回目は「基準を覚える」だけ。**初回に repo_changed を出さない**ことが要点
    // (出すと起動直後に必ず「外部で HEAD が移動しました」が出る)
    let first = mye_collab::ops::refresh_status(&mut state);
    assert_eq!(first.len(), 1, "初回は status_changed だけ: {first:?}");
    assert!(first[0].contains("\"event\":\"status_changed\""));

    // 2 回目は何も変わっていない = 通知ゼロ (窓が毎秒作り直されないための要)
    assert!(mye_collab::ops::refresh_status(&mut state).is_empty());

    // 外部で HEAD を戻した (= ターミナルで checkout されたのと同じ)
    git(&dir, &cfg, &["reset", "--hard", "-q", "HEAD~1"]);
    let moved = mye_collab::ops::refresh_status(&mut state);
    assert_eq!(moved.len(), 2, "repo_changed + status_changed: {moved:?}");
    assert!(moved[0].contains("\"event\":\"repo_changed\""), "{moved:?}");
    assert!(moved[1].contains("\"event\":\"status_changed\""), "{moved:?}");

    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn watching_service_emits_status_changed_when_a_file_appears() {
    let dir = temp_repo("watch");
    let svc = Service::with_watch(dir.to_string_lossy().to_string());
    // 先に status を 1 回取って基準を作る (これをしないと初回の通知が
    // 「空 → 空」で握り潰されるのか「空 → 1 件」なのか読み取れない)
    svc.request(json!({ "id": 1, "op": "status" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "status failed: {r}");

    std::thread::sleep(Duration::from_millis(200)); // 監視の登録待ち
    std::fs::write(dir.join("new.txt"), "hi").unwrap();

    let deadline = Instant::now() + Duration::from_secs(15);
    let mut got: Option<Value> = None;
    while Instant::now() < deadline {
        if let Some(line) = svc.poll() {
            let v: Value = serde_json::from_str(&line).unwrap();
            if v["event"] == "status_changed" {
                got = Some(v);
                break;
            }
        } else {
            std::thread::sleep(Duration::from_millis(20));
        }
    }
    let ev = got.expect("status_changed が 15 s 以内に来ない (監視が動いていない)");
    let entries = ev["status"]["entries"].as_array().unwrap();
    assert!(entries.iter().any(|e| e["path"] == "new.txt"), "{ev}");

    drop(svc); // 監視 → worker の順に止まること (固まったら Drop の順序が逆)
    let _ = std::fs::remove_dir_all(&dir);
}

// ---- M66c: git の失敗文 -> error.code の分類 ----
// ★実 git を走らせて撮らない理由: identity 未設定の fatal は
//   "unable to auto-detect email address (got 'user@HOSTNAME.(none)')" のように
//   **機体名を含む**。期待値に機体名が混ざるテストは他人の環境で赤くなる。
//   分類器は純関数なので、git が出す文言を手で与えて検査する方が強い。

use mye_collab::git::{classify_error, GitOutput};

fn failed(stderr: &str) -> GitOutput {
    GitOutput { status: 128, stdout: Vec::new(), stderr: stderr.as_bytes().to_vec() }
}

#[test]
fn missing_identity_is_classified() {
    // git 2.48.1 が実際に出す 2 文 (LC_ALL=C)
    let e = classify_error(&failed(
        "\n*** Please tell me who you are.\n\nfatal: unable to auto-detect email address (got 'u@h.(none)')",
    ));
    assert_eq!(e.code, code::IDENTITY_MISSING);
    let e2 = classify_error(&failed("fatal: empty ident name (for <u@h>) not allowed"));
    assert_eq!(e2.code, code::IDENTITY_MISSING);
}

#[test]
fn unrelated_failures_stay_unclassified_with_their_stderr() {
    // ★分類を広げすぎない。拾えなかったものは git_failed + stderr 全文で出す
    //   (誤分類は「見当違いの案内」になるので、未分類より害が大きい)
    let e = classify_error(&failed("fatal: pathspec 'nope' did not match any files"));
    assert_eq!(e.code, code::GIT_FAILED);
    assert!(e.detail.contains("did not match"), "detail に stderr が残ること");
}

#[test]
fn locked_index_and_overwrite_are_still_classified() {
    // M66a からの既存分類が M66c の追加で壊れていないこと
    assert_eq!(
        classify_error(&failed("fatal: Unable to create '.../index.lock': File exists.")).code,
        code::LOCKED_INDEX
    );
    assert_eq!(
        classify_error(&failed("error: Your local changes would be overwritten by merge.")).code,
        code::LOCAL_CHANGES_OVERWRITTEN
    );
}

// ---- M66d: revert / diff_names ----

/// identity 付きのコミット (署名を明示的に切る — 開発者の commit.gpgsign=true で固まらないため)
fn commit_all(dir: &Path, cfg: &Path, message: &str) {
    git(dir, cfg, &["add", "-A"]);
    git(
        dir,
        cfg,
        &[
            "-c",
            "user.name=mye",
            "-c",
            "user.email=mye@example.com",
            "-c",
            "commit.gpgsign=false",
            "commit",
            "-q",
            "-m",
            message,
        ],
    );
}

#[test]
fn revert_restores_tracked_and_deletes_untracked() {
    let dir = temp_repo("revert");
    let cfg = config_path_for(&dir);
    std::fs::write(dir.join("kept.txt"), "original").unwrap();
    commit_all(&dir, &cfg, "one");
    // 追跡済みを書き換え + 未追跡を 1 個置く
    std::fs::write(dir.join("kept.txt"), "edited").unwrap();
    std::fs::write(dir.join("fresh.txt"), "new").unwrap();

    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(
        json!({ "id": 1, "op": "revert", "args": { "paths": ["kept.txt", "fresh.txt"] } })
            .to_string(),
    );
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "revert failed: {r}");
    assert_eq!(r["result"]["reverted"], 2);
    assert_eq!(r["result"]["deleted"], 1, "未追跡は 1 件だけ消える");
    assert_eq!(
        std::fs::read_to_string(dir.join("kept.txt")).unwrap(),
        "original",
        "追跡済みは index の内容へ戻る"
    );
    assert!(!dir.join("fresh.txt").exists(), "未追跡ファイルは消える");
    // 書き込み系の応答は実行後の status を載せる (spec §4.1)
    let entries = r["result"]["status"]["entries"].as_array().unwrap();
    assert!(entries.is_empty(), "revert 後は清浄なはず: {r}");

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn revert_with_only_untracked_paths_does_not_call_checkout() {
    // ★checkout に未追跡パスを混ぜると pathspec エラーで**呼び出しごと**失敗する。
    //   「未追跡だけ選んだ」は普通の操作なので、ここが割れると revert が丸ごと使えない
    let dir = temp_repo("revert_untracked");
    let cfg = config_path_for(&dir);
    std::fs::write(dir.join("base.txt"), "b").unwrap();
    commit_all(&dir, &cfg, "one");
    std::fs::write(dir.join("only.txt"), "x").unwrap();

    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "revert", "args": { "paths": ["only.txt"] } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "revert failed: {r}");
    assert!(!dir.join("only.txt").exists());

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn revert_rejects_an_empty_path_list() {
    // 空を通すと `git checkout -- ` = リポジトリ全体が戻る (= 全編集の消失)
    let dir = temp_repo("revert_empty");
    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "revert", "args": { "paths": [] } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], false);
    assert_eq!(r["error"]["code"], code::BAD_REQUEST);

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn diff_names_reports_add_modify_delete_between_two_commits() {
    let dir = temp_repo("diffnames");
    let cfg = config_path_for(&dir);
    std::fs::create_dir_all(dir.join("assets")).unwrap();
    std::fs::write(dir.join("assets/a.png"), "1").unwrap();
    std::fs::write(dir.join("assets/gone.png"), "1").unwrap();
    commit_all(&dir, &cfg, "one");
    std::fs::write(dir.join("assets/a.png"), "2").unwrap();
    std::fs::remove_file(dir.join("assets/gone.png")).unwrap();
    std::fs::write(dir.join("assets/added.png"), "3").unwrap();
    commit_all(&dir, &cfg, "two");

    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(
        json!({ "id": 1, "op": "diff_names", "args": { "from": "HEAD~1", "to": "HEAD" } })
            .to_string(),
    );
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "diff_names failed: {r}");
    let names = r["result"]["names"].as_array().unwrap();
    let find = |p: &str| -> String {
        names
            .iter()
            .find(|n| n["path"] == p)
            .map(|n| n["status"].as_str().unwrap_or("").to_string())
            .unwrap_or_default()
    };
    assert_eq!(find("assets/a.png"), "M");
    assert_eq!(find("assets/added.png"), "A");
    assert_eq!(find("assets/gone.png"), "D");
    assert_eq!(names.len(), 3, "{r}");

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn diff_names_rejects_an_option_like_revision() {
    let dir = temp_repo("diffnames_bad");
    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(
        json!({ "id": 1, "op": "diff_names", "args": { "from": "--exec=calc" } }).to_string(),
    );
    let r = wait_line(&svc);
    assert_eq!(r["ok"], false);
    assert_eq!(r["error"]["code"], code::BAD_REQUEST);

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
}
