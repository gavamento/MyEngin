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

// ---- M66e: branches / branch_create / checkout ----

#[test]
fn branches_lists_locals_and_marks_the_current_one() {
    let dir = temp_repo("branches");
    let cfg = config_path_for(&dir);
    std::fs::write(dir.join("a.txt"), "1").unwrap();
    commit_all(&dir, &cfg, "one");
    git(&dir, &cfg, &["branch", "feature"]);

    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "branches" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "branches failed: {r}");
    assert_eq!(r["result"]["current"], "main");
    let locals = r["result"]["locals"].as_array().unwrap();
    assert_eq!(locals.len(), 2, "{r}");
    let names: Vec<&str> = locals.iter().map(|b| b["name"].as_str().unwrap()).collect();
    assert!(names.contains(&"main") && names.contains(&"feature"), "{r}");
    assert!(r["result"]["remotes"].as_array().unwrap().is_empty());

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn branch_create_then_checkout_reports_what_changed_on_disk() {
    let dir = temp_repo("checkout");
    let cfg = config_path_for(&dir);
    std::fs::create_dir_all(dir.join("assets")).unwrap();
    std::fs::write(dir.join("assets/keep.txt"), "1").unwrap();
    commit_all(&dir, &cfg, "one");

    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "branch_create", "args": { "name": "feature" } }).to_string());
    assert_eq!(wait_line(&svc)["ok"], true);
    // feature 側にだけテクスチャを 1 枚足す
    git(&dir, &cfg, &["checkout", "-q", "feature"]);
    std::fs::write(dir.join("assets/only_feature.png"), "png").unwrap();
    commit_all(&dir, &cfg, "two");
    git(&dir, &cfg, &["checkout", "-q", "main"]);

    svc.request(json!({ "id": 2, "op": "checkout", "args": { "name": "feature" } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "checkout failed: {r}");
    assert_eq!(r["result"]["branch"], "feature");
    // ★段階分類の唯一の入力。空だと「何も変わらなかった」と読まれる
    let names = r["result"]["names"].as_array().unwrap();
    assert_eq!(names.len(), 1, "{r}");
    assert_eq!(names[0]["path"], "assets/only_feature.png");
    assert_eq!(names[0]["status"], "A");
    assert_eq!(r["result"]["status"]["branch"], "feature", "応答に実行後の status が載る");
    assert!(dir.join("assets/only_feature.png").exists());

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn checkout_with_conflicting_local_changes_lists_the_files() {
    // spec §4.1 (S7): git に任せて、拒否されたら**対象ファイル一覧**を返す。
    // ここが空だとモーダルに「何を破棄すればいいか」が出せない
    let dir = temp_repo("overwrite");
    let cfg = config_path_for(&dir);
    std::fs::create_dir_all(dir.join("assets")).unwrap();
    std::fs::write(dir.join("assets/shared.txt"), "main").unwrap();
    commit_all(&dir, &cfg, "one");
    git(&dir, &cfg, &["checkout", "-q", "-b", "feature"]);
    std::fs::write(dir.join("assets/shared.txt"), "feature").unwrap();
    commit_all(&dir, &cfg, "two");
    git(&dir, &cfg, &["checkout", "-q", "main"]);
    // 未コミットのローカル変更を作る (保存しただけ = 普通の状態)
    std::fs::write(dir.join("assets/shared.txt"), "local edit").unwrap();

    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "checkout", "args": { "name": "feature" } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], false, "git は拒否するはず: {r}");
    assert_eq!(r["error"]["code"], code::LOCAL_CHANGES_OVERWRITTEN);
    let paths = r["error"]["paths"].as_array().expect("paths が要る");
    assert_eq!(paths.len(), 1, "{r}");
    assert_eq!(paths[0], "assets/shared.txt");
    // detail は git の案内文ではなく**こちらの固定文**(版で割れないため)
    assert!(!r["error"]["detail"].as_str().unwrap().contains("stash"), "{r}");
    assert_eq!(
        std::fs::read_to_string(dir.join("assets/shared.txt")).unwrap(),
        "local edit",
        "拒否されたら working tree は 1 バイトも動かない"
    );

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn checkout_and_branch_create_reject_option_like_names() {
    // ★checkout には `--` を置けない (`git checkout -- x` はパスの復元 = 別操作)。
    //   オプション化を止める砦は引数検証しか無い
    let dir = temp_repo("badname");
    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "checkout", "args": { "name": "--orphan" } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], false);
    assert_eq!(r["error"]["code"], code::BAD_REQUEST);
    svc.request(json!({ "id": 2, "op": "branch_create", "args": { "name": "" } }).to_string());
    assert_eq!(wait_line(&svc)["error"]["code"], code::BAD_REQUEST);
    svc.request(json!({ "id": 3, "op": "checkout" }).to_string());
    assert_eq!(wait_line(&svc)["error"]["code"], code::BAD_REQUEST, "name は必須");

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
}

// ---- M66f: fetch / pull / push / remote_state と定期 fetch ----
//
// ★ここが「2 クローンの往復」を実際に走らせる唯一の場所…ではない
//   (collab_verify の 07_remote.ndjson も同じ形を通す) が、**通知**
//   (remote_changed) はここでしか検査できない — CLI は event 行を出さない契約なので。

/// bare な origin を作り、`dir` を push して追跡を張る。戻り値 = origin のパス。
///
/// ★`-b main` を必ず付ける。省くと bare の HEAD が master になり、clone した側が
///   **main ではなく master に乗る** = 2 つのクローンが永久にすれ違う
///   (実測でこれを踏んで「分岐しないはずの分岐テスト」が緑になった)
fn attach_origin(dir: &Path, cfg: &Path, name: &str) -> PathBuf {
    let origin = std::env::temp_dir()
        .join(format!("mye_collab_{}_{}_origin.git", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&origin);
    let tmp = std::env::temp_dir();
    git(&tmp, cfg, &["init", "-q", "--bare", "-b", "main", &origin.to_string_lossy()]);
    git(dir, cfg, &["remote", "add", "origin", &origin.to_string_lossy()]);
    git(dir, cfg, &["push", "-q", "-u", "origin", "main"]);
    origin
}

/// origin をもう 1 つの作業ツリーへ clone する (= 同僚の機体)
fn clone_of(origin: &Path, cfg: &Path, name: &str) -> PathBuf {
    let dst = std::env::temp_dir().join(format!("mye_collab_{}_{}_peer", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&dst);
    let tmp = std::env::temp_dir();
    git(&tmp, cfg, &["clone", "-q", &origin.to_string_lossy(), &dst.to_string_lossy()]);
    dst
}

/// サービスが叩く git はテストの env を継がない (= 開発者の global config を見る)。
/// マージコミットで identity を聞かれたり gpgsign で固まったりしないよう、
/// **リポジトリ設定**で塞ぐ (repo local は global に勝つ)
fn pin_identity(dir: &Path, cfg: &Path) {
    git(dir, cfg, &["config", "user.name", "mye"]);
    git(dir, cfg, &["config", "user.email", "mye@example.com"]);
    git(dir, cfg, &["config", "commit.gpgsign", "false"]);
}

#[test]
fn fetch_then_pull_brings_the_peer_commit_and_names_it() {
    let dir = temp_repo("remote");
    let cfg = config_path_for(&dir);
    pin_identity(&dir, &cfg);
    std::fs::create_dir_all(dir.join("assets")).unwrap();
    std::fs::write(dir.join("assets/a.txt"), "one").unwrap();
    commit_all(&dir, &cfg, "one");
    let origin = attach_origin(&dir, &cfg, "remote");
    let peer = clone_of(&origin, &cfg, "remote");

    // 同僚が 1 本コミットして push する
    std::fs::write(peer.join("assets/b.txt"), "peer").unwrap();
    commit_all(&peer, &cfg, "peer-commit");
    git(&peer, &cfg, &["push", "-q"]);

    let svc = Service::new(dir.to_string_lossy().to_string());

    // fetch する前は「遅れ 0」— まだ知らないのだから知らないままであること
    svc.request(json!({ "id": 1, "op": "remote_state" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "remote_state failed: {r}");
    assert_eq!(r["result"]["upstream"], "origin/main");
    assert_eq!(r["result"]["hasRemote"], true);
    assert_eq!(r["result"]["behind"], 0, "fetch 前に behind が立ったら見えないはずの物を見ている");

    svc.request(json!({ "id": 2, "op": "fetch" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "fetch failed: {r}");
    assert_eq!(r["result"]["remote"]["behind"], 1, "{r}");
    let commits = r["result"]["remote"]["commits"].as_array().unwrap();
    assert_eq!(commits.len(), 1, "{r}");
    assert_eq!(commits[0]["subject"], "peer-commit");
    // 帯に出す 3 つ (誰が / いつ / 何を) がそろっていること
    assert!(!commits[0]["author"].as_str().unwrap_or("").is_empty(), "{r}");
    assert!(!commits[0]["date"].as_str().unwrap_or("").is_empty(), "{r}");
    // fetch は working tree を 1 バイトも動かさない
    assert!(!dir.join("assets/b.txt").exists(), "fetch がファイルを降らせている");

    svc.request(json!({ "id": 3, "op": "pull" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "pull failed: {r}");
    // ★names が段階分類の唯一の入力。空だと「何も変わらなかった」と読まれ、
    //   降ってきたファイルが古いまま使われる
    let names = r["result"]["names"].as_array().expect("names が要る");
    assert_eq!(names.len(), 1, "{r}");
    assert_eq!(names[0]["path"], "assets/b.txt");
    assert_eq!(names[0]["status"], "A");
    assert_eq!(r["result"]["remote"]["behind"], 0, "{r}");
    assert!(dir.join("assets/b.txt").exists(), "pull がファイルを降らせていない");

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&peer);
    let _ = std::fs::remove_dir_all(&origin);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn diverged_push_is_non_fast_forward_and_merge_pull_resolves_it() {
    let dir = temp_repo("diverge");
    let cfg = config_path_for(&dir);
    pin_identity(&dir, &cfg);
    std::fs::create_dir_all(dir.join("assets")).unwrap();
    std::fs::write(dir.join("assets/a.txt"), "one").unwrap();
    commit_all(&dir, &cfg, "one");
    let origin = attach_origin(&dir, &cfg, "diverge");
    let peer = clone_of(&origin, &cfg, "diverge");

    // 同僚と自分が**別々のファイル**を触って分岐する (中身が衝突しない = 自動マージできる)
    std::fs::write(peer.join("assets/peer.txt"), "peer").unwrap();
    commit_all(&peer, &cfg, "peer-commit");
    git(&peer, &cfg, &["push", "-q"]);
    std::fs::write(dir.join("assets/mine.txt"), "mine").unwrap();
    commit_all(&dir, &cfg, "my-commit");

    let svc = Service::new(dir.to_string_lossy().to_string());

    // 1) push は拒否される。**ここを git_failed に落とすと UI が「先に pull」を出せない**
    svc.request(json!({ "id": 1, "op": "push" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], false, "分岐しているのに push が通った: {r}");
    assert_eq!(r["error"]["code"], code::NON_FAST_FORWARD, "{r}");

    // 2) 既定の pull (--ff-only) も拒否される (fatal: Not possible to fast-forward)
    svc.request(json!({ "id": 2, "op": "pull" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], false, "{r}");
    assert_eq!(r["error"]["code"], code::NON_FAST_FORWARD, "{r}");
    assert!(!dir.join("assets/peer.txt").exists(), "拒否された pull がファイルを降らせている");

    // 3) allowMerge=true でマージが作られ、相手のファイルが降ってくる
    svc.request(json!({ "id": 3, "op": "pull", "args": { "allowMerge": true } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "merge pull failed: {r}");
    let names = r["result"]["names"].as_array().unwrap();
    assert!(names.iter().any(|n| n["path"] == "assets/peer.txt"), "{r}");
    assert!(dir.join("assets/peer.txt").exists());

    // 4) マージ後の push は通り、ahead が 0 に戻る
    svc.request(json!({ "id": 4, "op": "push" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "push after merge failed: {r}");
    assert_eq!(r["result"]["remote"]["ahead"], 0, "{r}");
    assert_eq!(r["result"]["remote"]["behind"], 0, "{r}");

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&peer);
    let _ = std::fs::remove_dir_all(&origin);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn pull_with_conflicting_content_is_reported_as_conflict() {
    // ★競合の解決 UI は sub-07 だが、**分類**はここで確定させる。
    //   git は競合を stdout に書く (stderr ではない) ので、classify_error 任せだと
    //   git_failed に化ける = 「マージ途中で止まっているのに理由が分からない」
    let dir = temp_repo("pullconf");
    let cfg = config_path_for(&dir);
    pin_identity(&dir, &cfg);
    std::fs::create_dir_all(dir.join("assets")).unwrap();
    std::fs::write(dir.join("assets/a.txt"), "base").unwrap();
    commit_all(&dir, &cfg, "one");
    let origin = attach_origin(&dir, &cfg, "pullconf");
    let peer = clone_of(&origin, &cfg, "pullconf");

    std::fs::write(peer.join("assets/a.txt"), "peer side").unwrap();
    commit_all(&peer, &cfg, "peer-edit");
    git(&peer, &cfg, &["push", "-q"]);
    std::fs::write(dir.join("assets/a.txt"), "my side").unwrap();
    commit_all(&dir, &cfg, "my-edit");

    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "pull", "args": { "allowMerge": true } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["ok"], false, "同じ行を両側で書き換えたのに通った: {r}");
    assert_eq!(r["error"]["code"], code::CONFLICT, "{r}");
    // detail は固定文 (git の案内文は版で変わる)
    assert_eq!(r["error"]["detail"], "the merge produced conflicts");

    // 後片付け: 競合を残したまま次のテストへ持ち越さない (sub-07 の abort 相当)
    git(&dir, &cfg, &["merge", "--abort"]);
    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&peer);
    let _ = std::fs::remove_dir_all(&origin);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn background_fetch_notifies_once_per_error_code() {
    // spec §4.1「背景 fetch の失敗は**同じ code が続く間は 1 回だけ**」。
    // オフラインのまま作業する人に 5 分ごとのトーストを投げつけないための唯一の仕掛け
    let dir = temp_repo("bgerr");
    let cfg = config_path_for(&dir);
    std::fs::write(dir.join("a.txt"), "one").unwrap();
    commit_all(&dir, &cfg, "one");
    // 存在しないローカルパスを origin にする = fetch が必ず失敗する
    let nowhere = std::env::temp_dir().join("mye_collab_no_such_origin.git");
    git(&dir, &cfg, &["remote", "add", "origin", &nowhere.to_string_lossy()]);

    let mut state = State::new(dir.to_string_lossy().to_string());
    let first = mye_collab::ops::background_fetch(&mut state);
    assert_eq!(first.len(), 1, "最初の失敗は 1 回知らせる: {first:?}");
    assert!(first[0].contains("\"event\":\"remote_changed\""), "{first:?}");
    assert!(first[0].contains("\"error\""), "{first:?}");
    let second = mye_collab::ops::background_fetch(&mut state);
    assert!(second.is_empty(), "同じ code の 2 回目は黙ること: {second:?}");

    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn background_fetch_reports_the_peer_commit_once() {
    let dir = temp_repo("bgok");
    let cfg = config_path_for(&dir);
    pin_identity(&dir, &cfg);
    std::fs::write(dir.join("a.txt"), "one").unwrap();
    commit_all(&dir, &cfg, "one");
    let origin = attach_origin(&dir, &cfg, "bgok");
    let peer = clone_of(&origin, &cfg, "bgok");

    let mut state = State::new(dir.to_string_lossy().to_string());
    // 1 回目で基準を作る (何も変わっていないので remote_changed は出ない…とは限らない —
    // last_remote が空なので初回は必ず 1 本出る。そこを基準にする)
    let _ = mye_collab::ops::background_fetch(&mut state);
    let quiet = mye_collab::ops::background_fetch(&mut state);
    assert!(
        !quiet.iter().any(|l| l.contains("remote_changed")),
        "何も変わっていないのに通知が出ている: {quiet:?}"
    );

    std::fs::write(peer.join("b.txt"), "peer").unwrap();
    commit_all(&peer, &cfg, "peer-commit");
    git(&peer, &cfg, &["push", "-q"]);

    let lines = mye_collab::ops::background_fetch(&mut state);
    let ev = lines
        .iter()
        .find(|l| l.contains("\"event\":\"remote_changed\""))
        .expect("同僚の push が remote_changed にならない");
    let v: Value = serde_json::from_str(ev).unwrap();
    assert_eq!(v["remote"]["behind"], 1, "{ev}");
    assert_eq!(v["remote"]["commits"][0]["subject"], "peer-commit", "{ev}");

    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&peer);
    let _ = std::fs::remove_dir_all(&origin);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn the_worker_timer_fetches_without_a_second_thread() {
    // spec §4.0「Rust 側に join できないスレッドを増やさない」= 定期 fetch は
    // worker の recv_timeout で回る。ここではその**配線**を実走で確かめる:
    // hello{autoFetch:true, fetchIntervalMin:0} → タイマーが 1 秒後に fetch する
    let dir = temp_repo("timer");
    let cfg = config_path_for(&dir);
    pin_identity(&dir, &cfg);
    std::fs::write(dir.join("a.txt"), "one").unwrap();
    commit_all(&dir, &cfg, "one");
    let origin = attach_origin(&dir, &cfg, "timer");
    let peer = clone_of(&origin, &cfg, "timer");
    std::fs::write(peer.join("b.txt"), "peer").unwrap();
    commit_all(&peer, &cfg, "peer-commit");
    git(&peer, &cfg, &["push", "-q"]);

    let svc = Service::with_timer(dir.to_string_lossy().to_string());
    svc.request(
        json!({ "id": 1, "op": "hello", "args": { "autoFetch": true, "fetchIntervalMin": 0 } })
            .to_string(),
    );

    let deadline = Instant::now() + Duration::from_secs(30);
    let mut got: Option<Value> = None;
    while Instant::now() < deadline {
        if let Some(line) = svc.poll() {
            let v: Value = serde_json::from_str(&line).unwrap();
            if v["event"] == "remote_changed" && v.get("remote").is_some() {
                got = Some(v);
                break;
            }
        } else {
            std::thread::sleep(Duration::from_millis(20));
        }
    }
    let ev = got.expect("タイマーが 30 s 以内に fetch していない (recv_timeout の配線)");
    assert_eq!(ev["remote"]["behind"], 1, "{ev}");

    drop(svc); // タイマーを回していても join できること (固まったら Drop が壊れている)
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&peer);
    let _ = std::fs::remove_dir_all(&origin);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn auto_fetch_off_keeps_the_timer_silent() {
    // 受け入れ条件 4 の「scmAutoFetch=false で fetch が止まる」の機械的な証明。
    // ★実機では「トーストが出ないこと」でしか見えないので、ここで押さえておく
    let dir = temp_repo("timeroff");
    let cfg = config_path_for(&dir);
    pin_identity(&dir, &cfg);
    std::fs::write(dir.join("a.txt"), "one").unwrap();
    commit_all(&dir, &cfg, "one");
    let origin = attach_origin(&dir, &cfg, "timeroff");
    let peer = clone_of(&origin, &cfg, "timeroff");
    std::fs::write(peer.join("b.txt"), "peer").unwrap();
    commit_all(&peer, &cfg, "peer-commit");
    git(&peer, &cfg, &["push", "-q"]);

    let svc = Service::with_timer(dir.to_string_lossy().to_string());
    svc.request(
        json!({ "id": 1, "op": "hello", "args": { "autoFetch": false, "fetchIntervalMin": 0 } })
            .to_string(),
    );
    let r = wait_line(&svc);
    assert_eq!(r["ok"], true, "hello failed: {r}");

    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        if let Some(line) = svc.poll() {
            assert!(
                !line.contains("remote_changed"),
                "autoFetch=false なのに定期 fetch が走っている: {line}"
            );
        } else {
            std::thread::sleep(Duration::from_millis(50));
        }
    }
    // 5 秒経っても origin/main は取り込まれていないこと (fetch していない証拠)
    let refs = dir.join(".git/refs/remotes/origin/main");
    let before = std::fs::read_to_string(&refs).unwrap_or_default();
    svc.request(json!({ "id": 2, "op": "remote_state" }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["result"]["behind"], 0, "fetch していないなら behind は 0 のまま: {r}");
    assert_eq!(std::fs::read_to_string(&refs).unwrap_or_default(), before);

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&peer);
    let _ = std::fs::remove_dir_all(&origin);
    let _ = std::fs::remove_file(&cfg);
}

#[test]
fn the_timer_is_not_starved_by_a_steady_stream_of_requests() {
    // ★実機で踏んだ回帰。定期 fetch の期限確認を `recv_timeout` の **timeout の枝だけ**に
    //   置くと、TICK より短い間隔でメッセージが届き続ける限りタイマーが永久に飢える。
    //   監視スレッドは status が触った `.git\index` を拾って Refresh を投げ返すので、
    //   これは理論上の話ではない (実機で 2 分半 fetch が 1 回も走らなかった)。
    //   ここでは「200 ms ごとに要求を投げ続けても定期 fetch が走る」ことを見る
    let dir = temp_repo("starve");
    let cfg = config_path_for(&dir);
    pin_identity(&dir, &cfg);
    std::fs::write(dir.join("a.txt"), "one").unwrap();
    commit_all(&dir, &cfg, "one");
    let origin = attach_origin(&dir, &cfg, "starve");
    let peer = clone_of(&origin, &cfg, "starve");
    std::fs::write(peer.join("b.txt"), "peer").unwrap();
    commit_all(&peer, &cfg, "peer-commit");
    git(&peer, &cfg, &["push", "-q"]);

    let svc = Service::with_timer(dir.to_string_lossy().to_string());
    svc.request(
        json!({ "id": 1, "op": "hello", "args": { "autoFetch": true, "fetchIntervalMin": 0 } })
            .to_string(),
    );

    let deadline = Instant::now() + Duration::from_secs(30);
    let mut id = 2;
    let mut fetched = false;
    while Instant::now() < deadline && !fetched {
        // 途切れなく要求を積む = worker の recv が待たされない状態を作る
        svc.request(json!({ "id": id, "op": "status" }).to_string());
        id += 1;
        std::thread::sleep(Duration::from_millis(200));
        while let Some(line) = svc.poll() {
            if line.contains("\"event\":\"remote_changed\"") && line.contains("\"remote\"") {
                fetched = true;
            }
        }
    }
    assert!(fetched, "要求が続いている間もタイマーが定期 fetch を走らせること");

    drop(svc);
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&peer);
    let _ = std::fs::remove_dir_all(&origin);
    let _ = std::fs::remove_file(&cfg);
}

// ---- M66g: 競合 (conflicts / resolve / merge_abort / continue) ----

/// 「同じ行を両側で変更」+「片側が消したファイルをもう片側が変更」の 2 種を作り、
/// 競合したまま止まっているリポジトリを返す (本体 / origin / 同僚)。
///
/// ★2 種目 (modify/delete) を必ず混ぜる。ours / theirs の分岐は
///   「相手の版が存在するか」で変わるので、content 競合だけだと
///   `git checkout --theirs` が落ちる経路が永久に未検査になる
fn conflicted_repo(name: &str) -> (PathBuf, PathBuf, PathBuf, PathBuf) {
    let dir = temp_repo(name);
    let cfg = config_path_for(&dir);
    pin_identity(&dir, &cfg);
    std::fs::create_dir_all(dir.join("assets")).unwrap();
    std::fs::write(dir.join("assets/shared.txt"), "base").unwrap();
    std::fs::write(dir.join("assets/del.txt"), "base").unwrap();
    commit_all(&dir, &cfg, "one");
    let origin = attach_origin(&dir, &cfg, name);
    let peer = clone_of(&origin, &cfg, name);

    // 同僚: shared を書き換え、del を消し、無関係な 1 本を足す
    std::fs::write(peer.join("assets/shared.txt"), "peer side").unwrap();
    std::fs::remove_file(peer.join("assets/del.txt")).unwrap();
    std::fs::write(peer.join("assets/clean.txt"), "peer only").unwrap();
    commit_all(&peer, &cfg, "peer-edit");
    git(&peer, &cfg, &["push", "-q"]);

    // こちら: shared と del の両方を書き換える
    std::fs::write(dir.join("assets/shared.txt"), "my side").unwrap();
    std::fs::write(dir.join("assets/del.txt"), "my side").unwrap();
    commit_all(&dir, &cfg, "my-edit");
    (dir, cfg, origin, peer)
}

fn cleanup(paths: &[&Path], cfg: &Path) {
    for p in paths {
        let _ = std::fs::remove_dir_all(p);
    }
    let _ = std::fs::remove_file(cfg);
}

#[test]
fn conflicts_lists_unmerged_files_and_the_cleanly_merged_ones() {
    let (dir, cfg, origin, peer) = conflicted_repo("conflist");
    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "pull", "args": { "allowMerge": true } }).to_string());
    let r = wait_line(&svc);
    assert_eq!(r["error"]["code"], code::CONFLICT, "{r}");
    // ★競合したファイルは応答に載る (git の案内文ではなく status の u レコード由来)
    let paths: Vec<String> = r["error"]["paths"]
        .as_array()
        .expect("conflict は paths を載せる")
        .iter()
        .map(|v| v.as_str().unwrap().to_string())
        .collect();
    assert!(paths.contains(&"assets/shared.txt".to_string()), "{paths:?}");
    assert!(paths.contains(&"assets/del.txt".to_string()), "{paths:?}");

    svc.request(json!({ "id": 2, "op": "conflicts" }).to_string());
    let c = wait_line(&svc);
    assert_eq!(c["ok"], true, "{c}");
    assert_eq!(c["result"]["mergeInProgress"], true);
    let list = c["result"]["conflicts"].as_array().unwrap();
    assert_eq!(list.len(), 2, "{c}");
    let shared = list.iter().find(|e| e["path"] == "assets/shared.txt").unwrap();
    assert_eq!(shared["kind"], "both_modified");
    assert_eq!(shared["ours"], true);
    assert_eq!(shared["theirs"], true);
    let del = list.iter().find(|e| e["path"] == "assets/del.txt").unwrap();
    assert_eq!(del["kind"], "deleted_by_them");
    assert_eq!(del["theirs"], false, "相手の版が無い = theirs は「消す」の意味");
    // 競合しなかったファイルは **もうディスクに書かれている** ので merged に載る
    let merged = c["result"]["merged"].as_array().unwrap();
    assert_eq!(merged.len(), 1, "{c}");
    assert_eq!(merged[0]["path"], "assets/clean.txt");
    assert_eq!(merged[0]["status"], "A");

    // status にもマージ中が載る (ゲートが毎回読む口)
    svc.request(json!({ "id": 3, "op": "status" }).to_string());
    let s = wait_line(&svc);
    assert_eq!(s["result"]["mergeInProgress"], true, "{s}");

    git(&dir, &cfg, &["merge", "--abort"]);
    drop(svc);
    cleanup(&[&dir, &peer, &origin], &cfg);
}

#[test]
fn resolve_then_continue_closes_the_merge() {
    let (dir, cfg, origin, peer) = conflicted_repo("resolve");
    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "pull", "args": { "allowMerge": true } }).to_string());
    assert_eq!(wait_line(&svc)["error"]["code"], code::CONFLICT);

    // 全件解決する前の continue は **merge_in_progress + 残り** で返る
    svc.request(json!({ "id": 2, "op": "continue" }).to_string());
    let early = wait_line(&svc);
    assert_eq!(early["error"]["code"], code::MERGE_IN_PROGRESS, "{early}");
    assert_eq!(early["error"]["paths"].as_array().unwrap().len(), 2, "{early}");

    // ours = 自分の版を採る
    svc.request(
        json!({ "id": 3, "op": "resolve",
                "args": { "paths": ["assets/shared.txt"], "side": "ours" } })
            .to_string(),
    );
    let r3 = wait_line(&svc);
    assert_eq!(r3["ok"], true, "{r3}");
    assert_eq!(r3["result"]["resolved"], 1);
    assert_eq!(
        std::fs::read_to_string(dir.join("assets/shared.txt")).unwrap(),
        "my side",
        "ours はマーカーを消して自分の版に戻す"
    );

    // theirs = 相手の版が無い (消された) ので「消す」
    svc.request(
        json!({ "id": 4, "op": "resolve",
                "args": { "paths": ["assets/del.txt"], "side": "theirs" } })
            .to_string(),
    );
    let r4 = wait_line(&svc);
    assert_eq!(r4["ok"], true, "{r4}");
    assert!(!dir.join("assets/del.txt").exists(), "theirs = 相手に合わせて消える");

    svc.request(json!({ "id": 5, "op": "continue" }).to_string());
    let done = wait_line(&svc);
    assert_eq!(done["ok"], true, "{done}");
    assert_eq!(done["result"]["status"]["mergeInProgress"], false, "{done}");
    // names は「マージ前の HEAD → マージコミット」= 相手から入ったもの
    let names: Vec<(String, String)> = done["result"]["names"]
        .as_array()
        .unwrap()
        .iter()
        .map(|n| {
            (n["path"].as_str().unwrap().to_string(), n["status"].as_str().unwrap().to_string())
        })
        .collect();
    assert!(names.contains(&("assets/clean.txt".to_string(), "A".to_string())), "{names:?}");
    assert!(names.contains(&("assets/del.txt".to_string(), "D".to_string())), "{names:?}");
    // マージコミットが 1 本増え、working tree は清浄
    assert!(done["result"]["status"]["entries"].as_array().unwrap().is_empty(), "{done}");
    assert_eq!(done["result"]["status"]["ahead"], 2, "{done}");

    drop(svc);
    cleanup(&[&dir, &peer, &origin], &cfg);
}

#[test]
fn merge_abort_puts_the_working_tree_back() {
    let (dir, cfg, origin, peer) = conflicted_repo("abort");
    let svc = Service::new(dir.to_string_lossy().to_string());
    svc.request(json!({ "id": 1, "op": "pull", "args": { "allowMerge": true } }).to_string());
    assert_eq!(wait_line(&svc)["error"]["code"], code::CONFLICT);
    assert!(dir.join("assets/clean.txt").exists(), "競合しなかった方は既に降っている");

    svc.request(json!({ "id": 2, "op": "merge_abort" }).to_string());
    let a = wait_line(&svc);
    assert_eq!(a["ok"], true, "{a}");
    assert_eq!(a["result"]["status"]["mergeInProgress"], false, "{a}");
    // ★names は **ディスクの在り方の変化**。中止は HEAD を動かさないので、
    //   コミット間の diff で作ると必ず空になる
    let names: Vec<(String, String)> = a["result"]["names"]
        .as_array()
        .unwrap()
        .iter()
        .map(|n| {
            (n["path"].as_str().unwrap().to_string(), n["status"].as_str().unwrap().to_string())
        })
        .collect();
    assert!(names.contains(&("assets/clean.txt".to_string(), "D".to_string())), "{names:?}");
    assert!(names.contains(&("assets/shared.txt".to_string(), "M".to_string())), "{names:?}");
    assert!(!dir.join("assets/clean.txt").exists(), "相手のファイルは消えて元に戻る");
    assert_eq!(
        std::fs::read_to_string(dir.join("assets/shared.txt")).unwrap(),
        "my side",
        "マーカーは消え、pull 前の内容に戻る"
    );
    assert!(a["result"]["status"]["entries"].as_array().unwrap().is_empty(), "{a}");

    // マージ中でなければ中止も継続も受け付けない (押せてしまうと状態が読めなくなる)
    svc.request(json!({ "id": 3, "op": "merge_abort" }).to_string());
    assert_eq!(wait_line(&svc)["error"]["code"], code::BAD_REQUEST);
    svc.request(json!({ "id": 4, "op": "continue" }).to_string());
    assert_eq!(wait_line(&svc)["error"]["code"], code::BAD_REQUEST);
    // 競合していないパスへの resolve も同じ (「解決したつもり」を作らない)
    svc.request(
        json!({ "id": 5, "op": "resolve",
                "args": { "paths": ["assets/shared.txt"], "side": "ours" } })
            .to_string(),
    );
    assert_eq!(wait_line(&svc)["error"]["code"], code::BAD_REQUEST);

    drop(svc);
    cleanup(&[&dir, &peer, &origin], &cfg);
}
