//! フック CLI (`mye_track.exe hook ...`) の結合テスト (M104i)。実際に exe を別プロセスとして起動し、
//! 終了コード・標準出力・所要時間を測る。サービスも `mye_track.exe serve` を起動する。
//!
//! 起動したプロセスは Drop で必ず止める。フックが起動したサービスは、最後に shutdown する。
//! 待ちは固定の sleep ではなく、上限付きの待ち合わせ。

use std::fs;
use std::io::{Read, Write};
use std::os::windows::io::FromRawHandle;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use mye_track::client::{read_service_json, Client};
use mye_track::pipe::pipe_name;
use mye_track::process::is_alive;
use mye_track::project::project_id;
use mye_track::store::Paths;

use windows_sys::Win32::Storage::FileSystem::PIPE_ACCESS_DUPLEX;
use windows_sys::Win32::System::Pipes::{ConnectNamedPipe, CreateNamedPipeW};

const WAIT_LIMIT: Duration = Duration::from_secs(60);
/// フックの全体の上限 (`HOOK_BUDGET` = 5 秒) に、プロセスの起動と終了の余裕を足した値。
const HOOK_LIMIT: Duration = Duration::from_secs(8);
const MISSING_EXE: &str = r"C:\mye_track_no_such_dir\mye_track.exe";

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
    let dir = std::env::temp_dir().join(format!("mye_track_hook_{}_{}", name, std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    TempRoot(dir)
}

fn connect(root: &Path, role: &str) -> Client {
    let mut token = None;
    wait_until("service.json", || {
        token = read_service_json(root).and_then(|v| v["token"].as_str().map(str::to_string));
        token.is_some()
    });
    let mut client = Client::connect(&pipe_name(&project_id(root)), WAIT_LIMIT).expect("pipe に繋がる");
    let hello = json!({ "proto": 1, "role": role, "token": token.unwrap(), "projectRoot": root.to_string_lossy() });
    let reply = client.request(1, "hello", hello).unwrap();
    assert_eq!(reply["ok"], true, "{reply}");
    client
}

fn wait_watching(root: &Path) {
    let mut client = connect(root, "mcp");
    wait_until("phase = watching", || client.request(2, "status", json!({})).unwrap()["result"]["phase"] == "watching");
}

/// 試験が起動したサービス。Drop で止める。
struct ServiceProc {
    child: Child,
}

impl ServiceProc {
    fn spawn(root: &Path) -> ServiceProc {
        let child = Command::new(env!("CARGO_BIN_EXE_mye_track"))
            .args(["serve", "--project"])
            .arg(root)
            .args(["--idle-timeout-ms", "120000"])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("mye_track.exe を起動できない");
        let service = ServiceProc { child };
        wait_watching(root);
        service
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

/// フックが起動したサービスを止める (shutdown して、プロセスの終了を待つ)。
fn stop_service_started_by_hook(root: &Path) {
    let pid = read_service_json(root).and_then(|v| v["pid"].as_u64()).map(|p| p as u32);
    let mut editor = connect(root, "editor");
    let _ = editor.request(2, "shutdown", json!({}));
    if let Some(pid) = pid {
        wait_until("フックが起動したサービスの終了", || !is_alive(pid, None));
    }
}

struct HookRun {
    code: Option<i32>,
    stdout: Vec<u8>,
    stderr: String,
    elapsed: Duration,
}

impl HookRun {
    /// 終了コード 0、標準出力が空、上限の時間内。
    fn assert_quiet(&self, what: &str) {
        assert_eq!(self.code, Some(0), "{what}: 終了コード (stderr: {})", self.stderr);
        assert!(self.stdout.is_empty(), "{what}: 標準出力に {} バイト出た", self.stdout.len());
        assert!(self.elapsed < HOOK_LIMIT, "{what}: 所要時間 {:?}", self.elapsed);
    }
}

fn hook_command(sub: &str, project: Option<&Path>) -> Command {
    let mut command = Command::new(env!("CARGO_BIN_EXE_mye_track"));
    command.args(["hook", sub]);
    if let Some(project) = project {
        command.arg("--project").arg(project);
    }
    command
}

fn run_command(mut command: Command, stdin: &[u8]) -> HookRun {
    let started = Instant::now();
    let mut child = command.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().expect("hook を起動できない");
    // 大きい入力を書いている間に子が終わることがある (壊れた pipe は無視する)
    let mut stdin_pipe = child.stdin.take().unwrap();
    let input = stdin.to_vec();
    let writer = std::thread::spawn(move || {
        let _ = stdin_pipe.write_all(&input);
    });
    let output = child.wait_with_output().expect("hook を待てない");
    let _ = writer.join();
    HookRun {
        code: output.status.code(),
        stdout: output.stdout,
        stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
        elapsed: started.elapsed(),
    }
}

fn run_hook(sub: &str, project: Option<&Path>, stdin: &[u8]) -> HookRun {
    run_command(hook_command(sub, project), stdin)
}

fn run_hook_with_env(sub: &str, project: Option<&Path>, stdin: &[u8], envs: &[(&str, &str)]) -> HookRun {
    let mut command = hook_command(sub, project);
    for (key, value) in envs {
        command.env(key, value);
    }
    run_command(command, stdin)
}

fn json_bytes(value: &Value) -> Vec<u8> {
    value.to_string().into_bytes()
}

fn pre_edit(root: &Path, cwd: &Path, rel: &str, old: &str, new: &str, replace_all: bool, tool_use_id: &str) -> Value {
    json!({
        "session_id": "sess-1", "cwd": cwd.to_string_lossy(), "hook_event_name": "PreToolUse", "tool_name": "Edit",
        "tool_use_id": tool_use_id,
        "tool_input": { "file_path": root.join(rel).to_string_lossy(), "old_string": old, "new_string": new, "replace_all": replace_all },
    })
}

fn pre_write(root: &Path, rel: &str, content: &str, tool_use_id: &str) -> Value {
    json!({
        "session_id": "sess-1", "cwd": root.to_string_lossy(), "hook_event_name": "PreToolUse", "tool_name": "Write",
        "tool_use_id": tool_use_id,
        "tool_input": { "file_path": root.join(rel).to_string_lossy(), "content": content },
    })
}

/// pre の入力から post の入力を作る (`tool_response` を足す)。
fn post_of(pre: &Value, response: Value) -> Value {
    let mut post = pre.clone();
    post["hook_event_name"] = json!("PostToolUse");
    post["tool_response"] = response;
    post
}

/// `path` の最新の Revision の (actor, attribution)。無ければ記録されるまで待つ。
fn verdict_of(root: &Path, path: &str) -> (String, String) {
    let mut mcp = connect(root, "mcp");
    let mut found = None;
    wait_until(&format!("{path} の Revision"), || {
        let reply = mcp.request(3, "list", json!({ "path": path, "limit": 1 })).unwrap();
        found = reply["result"]["items"].get(0).cloned();
        found.is_some()
    });
    let item = found.unwrap();
    (item["actor"].as_str().unwrap().to_string(), item["attribution"].as_str().unwrap().to_string())
}

/// 編集の前後をフックで知らせる。`edit` が編集の中身 (ファイルへ書く)。
fn hooked_edit(root: &Path, pre: &Value, edit: impl FnOnce()) {
    let run = run_hook("pre", Some(root), &json_bytes(pre));
    run.assert_quiet("pre");
    edit();
    let run = run_hook("post", Some(root), &json_bytes(&post_of(pre, json!({ "filePath": "x", "type": "update" }))));
    run.assert_quiet("post");
}

// ---- 受け入れ条件 1、2: 宣言と一致すれば Verified ----------------------------------------------------------

#[test]
fn edit_and_write_hooks_make_declared_changes_verified() {
    let project = temp_root("verified");
    let root = &project.0;
    fs::write(root.join("e1.txt"), "a\nb\nc\n").unwrap();
    fs::write(root.join("e2.txt"), "x\nx\ny\n").unwrap();
    fs::write(root.join("w2.txt"), "old\n").unwrap();
    let _service = ServiceProc::spawn(root);

    // Edit (replace_all 偽)
    let pre = pre_edit(root, root, "e1.txt", "b", "B", false, "t-e1");
    hooked_edit(root, &pre, || fs::write(root.join("e1.txt"), "a\nB\nc\n").unwrap());
    assert_eq!(verdict_of(root, "e1.txt"), ("ai".into(), "verified".into()));

    // Edit (replace_all 真): 同じ文字列が 2 か所
    let pre = pre_edit(root, root, "e2.txt", "x", "X", true, "t-e2");
    hooked_edit(root, &pre, || fs::write(root.join("e2.txt"), "X\nX\ny\n").unwrap());
    assert_eq!(verdict_of(root, "e2.txt"), ("ai".into(), "verified".into()));

    // Write (新規)
    let pre = pre_write(root, "w1.txt", "brand new\n", "t-w1");
    hooked_edit(root, &pre, || fs::write(root.join("w1.txt"), "brand new\n").unwrap());
    assert_eq!(verdict_of(root, "w1.txt"), ("ai".into(), "verified".into()));

    // Write (上書き)
    let pre = pre_write(root, "w2.txt", "replaced\n", "t-w2");
    hooked_edit(root, &pre, || fs::write(root.join("w2.txt"), "replaced\n").unwrap());
    assert_eq!(verdict_of(root, "w2.txt"), ("ai".into(), "verified".into()));

    // セッションに 4 件が出る (clientSessionId = session_id)
    let mut mcp = connect(root, "mcp");
    let sessions = mcp.request(3, "sessions", json!({})).unwrap();
    let items = sessions["result"]["items"].as_array().unwrap();
    assert_eq!(items.len(), 1, "{sessions}");
    assert_eq!(items[0]["clientSessionId"], "sess-1");
    assert_eq!(items[0]["provider"], "claude");
    assert_eq!(items[0]["source"], "terminal");
    let session_id = items[0]["sessionId"].as_str().unwrap().to_string();
    let listed = mcp.request(4, "list", json!({ "sessionId": session_id })).unwrap();
    let mut paths: Vec<&str> = listed["result"]["items"].as_array().unwrap().iter().map(|i| i["path"].as_str().unwrap()).collect();
    paths.sort();
    assert_eq!(paths, ["e1.txt", "e2.txt", "w1.txt", "w2.txt"]);

    // session-end で終了する
    run_hook("session-end", Some(root), &json_bytes(&json!({ "session_id": "sess-1" }))).assert_quiet("session-end");
    let sessions = mcp.request(5, "sessions", json!({})).unwrap();
    assert_eq!(sessions["result"]["items"][0]["status"], "finished");
}

#[test]
fn a_human_change_or_a_missing_old_string_keeps_the_change_from_being_verified() {
    let project = temp_root("not_verified");
    let root = &project.0;
    fs::write(root.join("h.txt"), "one\ntwo\n").unwrap();
    fs::write(root.join("m.txt"), "alpha\nbeta\n").unwrap();
    let _service = ServiceProc::spawn(root);

    // pre と post の間に、人が同じファイルへ別の変更を足す
    let pre = pre_edit(root, root, "h.txt", "two", "TWO", false, "t-h");
    hooked_edit(root, &pre, || {
        fs::write(root.join("h.txt"), "one\nTWO\n").unwrap();
        fs::write(root.join("h.txt"), "one\nTWO\nhuman\n").unwrap();
    });
    let (actor, attribution) = verdict_of(root, "h.txt");
    assert_ne!(attribution, "verified", "人の変更を足したのに verified");
    assert_eq!(actor, "ai");

    // pre の old_string が実際の内容に無い (宣言を当てられない)
    let pre = pre_edit(root, root, "m.txt", "no such text", "x", false, "t-m");
    hooked_edit(root, &pre, || fs::write(root.join("m.txt"), "alpha\nBETA\n").unwrap());
    let (_, attribution) = verdict_of(root, "m.txt");
    assert_ne!(attribution, "verified", "宣言を当てられないのに verified");
}

/// 設定の例 (hooks-setup.md) は `--project` に `/` 区切りのパスを書く。同じプロジェクトとして扱われる。
#[test]
fn a_project_path_with_forward_slashes_reaches_the_same_service() {
    let project = temp_root("slashes");
    let root = &project.0;
    fs::write(root.join("s.txt"), "s0\n").unwrap();
    let _service = ServiceProc::spawn(root);
    let slashed = PathBuf::from(root.to_string_lossy().replace('\\', "/"));
    let pre = pre_edit(root, root, "s.txt", "s0", "s1", false, "t-s");
    run_hook("pre", Some(&slashed), &json_bytes(&pre)).assert_quiet("pre (/ 区切り)");
    fs::write(root.join("s.txt"), "s1\n").unwrap();
    run_hook("post", Some(&slashed), &json_bytes(&post_of(&pre, json!({})))).assert_quiet("post (/ 区切り)");
    assert_eq!(verdict_of(root, "s.txt"), ("ai".into(), "verified".into()));
}

// ---- 受け入れ条件 7: 1 MiB を超える content ----------------------------------------------------------------

#[test]
fn a_write_over_one_mib_is_recorded_claimed_without_the_declaration() {
    let project = temp_root("big");
    let root = &project.0;
    let _service = ServiceProc::spawn(root);
    let content = "x".repeat(1024 * 1024 + 100);
    let pre = pre_write(root, "big.txt", &content, "t-big");
    let run = run_hook("pre", Some(root), &json_bytes(&pre));
    run.assert_quiet("巨大な Write の pre");
    fs::write(root.join("big.txt"), &content).unwrap();
    let post = post_of(&pre, json!({ "type": "create" }));
    run_hook("post", Some(root), &json_bytes(&post)).assert_quiet("巨大な Write の post");
    // 宣言なし: 操作は結び付くが Verified にはならない (Claimed)
    assert_eq!(verdict_of(root, "big.txt"), ("ai".into(), "claimed".into()));
}

// ---- 受け入れ条件 3、4: どの失敗でも終了コード 0・標準出力が空・5 秒以内 -------------------------------------

#[test]
fn every_failure_exits_zero_with_empty_stdout_in_time() {
    let project = temp_root("failures");
    let root = &project.0;
    let outside = temp_root("failures_outside");
    let good_pre = pre_write(root, "a.txt", "a\n", "t-1");
    let good_bytes = json_bytes(&good_pre);
    let envs = [("MYE_TRACK_EXE", MISSING_EXE)];

    // サービス不在 (--project なし): 何も起動せず、.mye も作らない
    run_hook("pre", None, &good_bytes).assert_quiet("サービス不在");
    // MYE_TRACK_EXE が存在しない + --project あり (起動を試みて失敗する)
    run_hook_with_env("pre", Some(root), &good_bytes, &envs).assert_quiet("exe が無い");
    // 壊れた JSON / 空の標準入力 / JSON だが配列 / 巨大でない文字化け
    run_hook_with_env("pre", Some(root), b"{not json", &envs).assert_quiet("壊れた JSON");
    run_hook_with_env("pre", Some(root), b"", &envs).assert_quiet("空の標準入力");
    run_hook_with_env("pre", Some(root), b"[1,2]", &envs).assert_quiet("配列");
    run_hook_with_env("pre", Some(root), &[0xff, 0xfe, 0x00], &envs).assert_quiet("バイナリ");
    // tool_name が Bash (matcher で絞っていなくても静か)
    let mut bash = good_pre.clone();
    bash["tool_name"] = json!("Bash");
    let run = run_hook_with_env("pre", Some(root), &json_bytes(&bash), &envs);
    run.assert_quiet("Bash");
    assert!(run.stderr.is_empty(), "対応しない道具は標準エラーにも出さない: {}", run.stderr);
    // 項目の欠落
    for key in ["session_id", "tool_use_id"] {
        let mut missing = good_pre.clone();
        missing.as_object_mut().unwrap().remove(key);
        run_hook_with_env("pre", Some(root), &json_bytes(&missing), &envs).assert_quiet(key);
    }
    let mut not_string = good_pre.clone();
    not_string["session_id"] = json!(7);
    run_hook_with_env("pre", Some(root), &json_bytes(&not_string), &envs).assert_quiet("session_id が数値");
    let mut no_path = good_pre.clone();
    no_path["tool_input"] = json!({});
    run_hook_with_env("pre", Some(root), &json_bytes(&no_path), &envs).assert_quiet("file_path が無い");
    // プロジェクトの外の file_path (--project があっても何も送らず、起動もしない)
    let outside_pre = pre_write(&outside.0, "o.txt", "o\n", "t-o");
    let run = run_hook_with_env("pre", Some(root), &json_bytes(&outside_pre), &[]);
    run.assert_quiet("プロジェクトの外");
    // 他のサブコマンドも、サービスが居なければ静かに終わる (post / session-end は起動もしない)
    run_hook("post", Some(root), &json_bytes(&post_of(&good_pre, json!({})))).assert_quiet("post (サービス不在)");
    run_hook("session-end", Some(root), &json_bytes(&json!({ "session_id": "s" }))).assert_quiet("session-end (サービス不在)");
    run_hook("session-start", None, &json_bytes(&json!({ "session_id": "s", "cwd": root.to_string_lossy() }))).assert_quiet("session-start");
    // 不明なサブコマンド・引数
    run_command(
        {
            let mut c = Command::new(env!("CARGO_BIN_EXE_mye_track"));
            c.args(["hook", "nope"]);
            c
        },
        b"{}",
    )
    .assert_quiet("不明なサブコマンド");
    run_command(
        {
            let mut c = Command::new(env!("CARGO_BIN_EXE_mye_track"));
            c.args(["hook"]);
            c
        },
        b"{}",
    )
    .assert_quiet("サブコマンドが無い");

    // ここまでのどれも、サービスも .mye も作っていない
    assert!(!root.join(".mye").exists(), "失敗の経路が .mye を作った");
    assert!(!outside.0.join(".mye").exists());
}

#[test]
fn a_pipe_that_never_answers_ends_within_the_budget() {
    let project = temp_root("silent");
    let root = &project.0;
    // 本物のサービスのように service.json (トークン) を置き、pipe は張るが hello に答えない偽のサーバ
    let paths = Paths::for_project(root);
    fs::create_dir_all(&paths.dir).unwrap();
    fs::write(paths.dir.join("service.json"), r#"{"token":"00000000000000000000000000000000","pid":1}"#).unwrap();
    let name: Vec<u16> = pipe_name(&project_id(root)).encode_utf16().chain(std::iter::once(0)).collect();
    // SAFETY: 名前は NUL 終端。得たハンドルはスレッドが File として所有する
    let handle = unsafe { CreateNamedPipeW(name.as_ptr(), PIPE_ACCESS_DUPLEX, 0, 1, 4096, 4096, 0, std::ptr::null()) };
    assert!(!handle.is_null() && handle as isize != -1, "pipe を作れない");
    let raw = handle as usize;
    let server = std::thread::spawn(move || {
        let handle = raw as windows_sys::Win32::Foundation::HANDLE;
        // SAFETY: 繋がるまで待つだけ
        unsafe { ConnectNamedPipe(handle, std::ptr::null_mut()) };
        // SAFETY: ハンドルはこのスレッドだけが持つ
        let mut file = unsafe { fs::File::from_raw_handle(handle) };
        let mut buf = [0u8; 4096];
        let _ = file.read(&mut buf);
        // 答えない。フックが諦めて終わるまで開いておく
        std::thread::sleep(Duration::from_secs(8));
    });
    let pre = pre_write(root, "a.txt", "a\n", "t-1");
    let run = run_hook("pre", None, &json_bytes(&pre));
    run.assert_quiet("応答しない pipe");
    assert!(run.elapsed >= Duration::from_secs(4), "時間切れを待たずに終わった: {:?} (hello を送っていない?)", run.elapsed);
    let _ = server.join();
}

#[test]
fn a_stdin_that_never_closes_still_ends_within_the_budget() {
    let started = Instant::now();
    let mut command = hook_command("pre", None);
    let mut child = command.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().unwrap();
    let _keep_open = child.stdin.take().unwrap();
    let output = child.wait_with_output().unwrap();
    assert_eq!(output.status.code(), Some(0));
    assert!(output.stdout.is_empty());
    assert!(started.elapsed() < HOOK_LIMIT, "{:?}", started.elapsed());
}

#[test]
fn a_panic_still_exits_zero_with_empty_stdout() {
    let project = temp_root("panic");
    let pre = pre_write(&project.0, "a.txt", "a\n", "t-1");
    let run = run_hook_with_env("pre", Some(&project.0), &json_bytes(&pre), &[("MYE_TRACK_HOOK_TEST_PANIC", "1"), ("MYE_TRACK_EXE", MISSING_EXE)]);
    run.assert_quiet("panic");
    assert!(run.stderr.contains("panic"), "panic の説明が標準エラーに無い: {}", run.stderr);
    // panic フックは 1 行に抑える (既定の複数行の出力にしない)
    assert!(run.stderr.lines().count() <= 2, "{}", run.stderr);
    assert!(!project.0.join(".mye").exists());
}

#[test]
fn the_hook_paths_never_exit_with_a_nonzero_code() {
    // run は値を返さず、main の hook の枝は ExitCode::SUCCESS を返す。hook.rs / launch.rs に exit を書かない
    let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    for file in ["hook.rs", "launch.rs"] {
        let text = fs::read_to_string(src.join(file)).unwrap();
        for forbidden in ["process::exit", "ExitCode", "abort("] {
            assert!(!text.lines().filter(|l| !l.trim_start().starts_with("//")).any(|l| l.contains(forbidden)), "{file} に {forbidden}");
        }
    }
    let main = fs::read_to_string(src.join("main.rs")).unwrap();
    let branch = main.split("Some(\"hook\") =>").nth(1).unwrap().split("Some(\"bench\")").next().unwrap();
    assert!(branch.contains("ExitCode::SUCCESS") && !branch.contains("ExitCode::from"), "{branch}");
}

// ---- 受け入れ条件 5: プロジェクトの決め方 ----------------------------------------------------------------

#[test]
fn a_hook_without_project_finds_the_service_of_a_parent_directory() {
    let project = temp_root("parent");
    let root = &project.0;
    let sub = root.join("deep").join("er");
    fs::create_dir_all(&sub).unwrap();
    fs::write(sub.join("f.txt"), "f0\n").unwrap();
    let _service = ServiceProc::spawn(root);

    // cwd はサブディレクトリ、--project なし
    let pre = pre_edit(root, &sub, "deep/er/f.txt", "f0", "f1", false, "t-sub");
    let run = run_hook("pre", None, &json_bytes(&pre));
    run.assert_quiet("pre (サブディレクトリ)");
    fs::write(sub.join("f.txt"), "f1\n").unwrap();
    run_hook("post", None, &json_bytes(&post_of(&pre, json!({})))).assert_quiet("post (サブディレクトリ)");
    assert_eq!(verdict_of(root, "deep/er/f.txt"), ("ai".into(), "verified".into()));
    // サブディレクトリを別のプロジェクトとして扱っていない
    assert!(!sub.join(".mye").exists() && !root.join("deep").join(".mye").exists());

    // 対象の無い session-start も cwd から親をたどる
    run_hook("session-start", None, &json_bytes(&json!({ "session_id": "sess-2", "cwd": sub.to_string_lossy() }))).assert_quiet("session-start");
    let mut mcp = connect(root, "mcp");
    let sessions = mcp.request(3, "sessions", json!({})).unwrap();
    assert!(sessions["result"]["items"].as_array().unwrap().iter().any(|s| s["clientSessionId"] == "sess-2"), "{sessions}");
}

#[test]
fn a_hook_without_project_starts_nothing_where_no_service_has_run() {
    let project = temp_root("nowhere");
    let root = &project.0;
    let sub = root.join("sub");
    fs::create_dir_all(&sub).unwrap();
    let pre = pre_write(root, "sub/f.txt", "f\n", "t-1");
    let run = run_hook("pre", None, &json_bytes(&pre));
    run.assert_quiet("サービスの跡が無い場所");
    // 少し待っても何も立ち上がらない・何も作られない
    std::thread::sleep(Duration::from_millis(500));
    assert!(!root.join(".mye").exists() && !sub.join(".mye").exists(), "--project なしで .mye が作られた");
    let pipe = pipe_name(&project_id(root));
    assert!(Client::connect(&pipe, Duration::ZERO).is_err(), "サービスが起動された");
}

#[test]
fn a_hook_with_project_starts_the_service_and_the_next_operations_are_recorded() {
    let project = temp_root("cold");
    let root = &project.0;
    fs::write(root.join("c.txt"), "c0\n").unwrap();
    // 1 回目: サービスは居ない。--project があるので起動する (この操作は記録されなくてよい)
    let pre = pre_edit(root, root, "c.txt", "c0", "c1", false, "t-1");
    let run = run_hook("pre", Some(root), &json_bytes(&pre));
    run.assert_quiet("cold start の pre");
    // 後始末のため、起動されたことを確かめて、最後に必ず止める
    let outcome = std::panic::catch_unwind(|| {
        wait_watching(root);
        fs::write(root.join("c.txt"), "c1\n").unwrap();
        run_hook("post", Some(root), &json_bytes(&post_of(&pre, json!({})))).assert_quiet("cold start の post");
        // 2 回目: サービスは動いている。次の操作から記録される
        let second = pre_edit(root, root, "c.txt", "c1", "c2", false, "t-2");
        hooked_edit(root, &second, || fs::write(root.join("c.txt"), "c2\n").unwrap());
        assert_eq!(verdict_of(root, "c.txt"), ("ai".into(), "verified".into()));
    });
    stop_service_started_by_hook(root);
    if let Err(panic) = outcome {
        std::panic::resume_unwind(panic);
    }
}

// ---- 受け入れ条件 6: 起動したサービスがフックの標準入出力を掴まない ------------------------------------

#[test]
fn the_service_started_by_a_hook_does_not_hold_the_hooks_stdout() {
    let project = temp_root("handles");
    let root = &project.0;
    let pre = pre_write(root, "a.txt", "a\n", "t-1");
    let mut command = hook_command("pre", Some(root));
    let mut child = command.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().unwrap();
    child.stdin.take().unwrap().write_all(&json_bytes(&pre)).unwrap();
    let mut stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel::<usize>();
    std::thread::spawn(move || {
        let mut bytes = Vec::new();
        let _ = stdout.read_to_end(&mut bytes);
        let _ = tx.send(bytes.len());
    });
    std::thread::spawn(move || {
        let mut bytes = Vec::new();
        let _ = stderr.read_to_end(&mut bytes);
    });
    let status = child.wait().unwrap();
    let outcome = std::panic::catch_unwind(|| {
        assert_eq!(status.code(), Some(0));
        // フックが終わったら、読み側はすぐに EOF を受ける (サービスが掴んでいればここで止まる)
        let length = rx.recv_timeout(Duration::from_secs(5)).expect("フックが終わっても標準出力の EOF が来ない (サービスが掴んでいる)");
        assert_eq!(length, 0);
        assert!(read_service_json(root).is_some(), "サービスが起動していない");
    });
    stop_service_started_by_hook(root);
    if let Err(panic) = outcome {
        std::panic::resume_unwind(panic);
    }
}

// ---- 受け入れ条件 8: MYE_TRACK_HOOK_DUMP -----------------------------------------------------------------

#[test]
fn the_dump_variable_appends_the_stdin_and_nothing_is_written_without_it() {
    let project = temp_root("dump");
    let dump_dir = temp_root("dump_out");
    let dump = dump_dir.0.join("hook_dump.jsonl");
    let envs = [("MYE_TRACK_EXE", MISSING_EXE)];
    // 複数行に整形された入力でも 1 行で追記される
    let input = "{\n  \"session_id\": \"s\",\n  \"tool_name\": \"Bash\"\n}\n";

    let run = run_hook_with_env("pre", Some(&project.0), input.as_bytes(), &envs);
    run.assert_quiet("DUMP 無し");
    assert!(!dump.exists(), "設定していないのに書かれた");

    let with_dump = [envs[0], ("MYE_TRACK_HOOK_DUMP", dump.to_str().unwrap())];
    run_hook_with_env("pre", Some(&project.0), input.as_bytes(), &with_dump).assert_quiet("DUMP 1 回目");
    // 壊れた JSON もそのまま記録する (本物の入力を調べるため)
    run_hook_with_env("post", Some(&project.0), b"{broken", &with_dump).assert_quiet("DUMP 2 回目");
    let text = fs::read_to_string(&dump).unwrap();
    let lines: Vec<&str> = text.lines().collect();
    assert_eq!(lines.len(), 2, "{text}");
    let first: Value = serde_json::from_str(lines[0]).unwrap();
    assert_eq!(first, serde_json::from_str::<Value>(input).unwrap());
    assert_eq!(lines[1], "{broken");

    // 追記先が書けなくても 0 (ディレクトリを指す)
    let bad = [envs[0], ("MYE_TRACK_HOOK_DUMP", dump_dir.0.to_str().unwrap())];
    let run = run_hook_with_env("pre", Some(&project.0), input.as_bytes(), &bad);
    run.assert_quiet("追記先が書けない");
    assert!(run.stderr.contains("MYE_TRACK_HOOK_DUMP"), "{}", run.stderr);
}
