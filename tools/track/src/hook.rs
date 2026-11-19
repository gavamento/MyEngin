//! `mye_track.exe hook <pre|post|post-failure|denied|turn-end|session-start|session-end> [--project <root>]`
//! (M104i、attribution-rules.md §6.1)。
//!
//! Claude Code のフックとして呼ばれ、標準入力の JSON を追跡サービスの op (session_begin / op_begin / op_end /
//! session_turn_end / session_end) へ変換する。入力の形は docs\ai-change-tracking\hooks-setup.md。
//!
//! 守ること (AI の作業を止めない):
//! - 終了コードは常に 0。`run` は値を返さず、`main` が `ExitCode::SUCCESS` を返す (0 以外を作る経路が型に無い)。
//!   終了コード 2 は PreToolUse の道具を止める。
//! - 標準出力へ 1 バイトも書かない。標準出力の JSON は Claude Code が解釈するため。失敗の説明は標準エラーへ 1 行。
//! - 全体で `HOOK_BUDGET` を超えない。作業は別スレッドで行い、時間切れ・panic のどちらでも 0 で返す。
//! - 誤った場所にサービスを立てない。起動するのは `--project` で明示されたときだけ (`resolve_root`)。

use std::fs::OpenOptions;
use std::io::{self, Read, Write};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::sync::mpsc::channel;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::client::{read_service_json, Client};
use crate::launch::start_service;
use crate::ops_record::{normalize_path, MAX_HOST_TERMINAL_BYTES};
use crate::pipe::pipe_name;
use crate::project::project_id;
use crate::protocol::{MAX_LINE_BYTES, PROTO_VERSION};
use crate::store::Paths;

/// フック全体の上限。AI の編集を待たせる時間の上限でもある (ipc-contract.md §6)。
pub const HOOK_BUDGET: Duration = Duration::from_secs(5);

/// 標準入力として読む量の上限。これを超えるものは入力として読めないものと扱う。
const MAX_STDIN_BYTES: u64 = 64 * 1024 * 1024;

/// 設定されたファイルへ標準入力をそのまま追記する (本物の入力の記録用)。
/// プロジェクトの外へも書く唯一の例外なので、既定では無効で、設定したときだけ働く。
pub const ENV_DUMP: &str = "MYE_TRACK_HOOK_DUMP";

/// 設定されていれば panic する (終了コード 0 の試験用。製品の設定では使わない)。
pub const ENV_TEST_PANIC: &str = "MYE_TRACK_HOOK_TEST_PANIC";

/// MyEngine 統合ターミナルが子プロセスへ入れる ID (Editor の `TerminalVsEnv.h` の `kTerminalEnvTerminalId`)。
/// 書式は `<エディタ PID>-<起動 UNIX 秒>-<シェル起動連番>`。フックは中身を解釈せず、そのまま `hostTerminal` へ渡す。
pub const ENV_TERMINAL_ID: &str = "MYE_TERMINAL_ID";

/// bad_token のあと、新しい service.json の値を待つ上限。
const TOKEN_REFRESH_WAIT: Duration = Duration::from_millis(1000);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Sub {
    Pre,
    Post,
    /// PostToolUseFailure。道具が失敗した
    PostFailure,
    /// PermissionDenied (自動のモードの拒否)。手で拒否したときは来ない
    Denied,
    /// Stop。応答の終わり
    TurnEnd,
    SessionStart,
    SessionEnd,
}

impl Sub {
    fn parse(text: &str) -> Option<Sub> {
        match text {
            "pre" => Some(Sub::Pre),
            "post" => Some(Sub::Post),
            "post-failure" => Some(Sub::PostFailure),
            "denied" => Some(Sub::Denied),
            "turn-end" => Some(Sub::TurnEnd),
            "session-start" => Some(Sub::SessionStart),
            "session-end" => Some(Sub::SessionEnd),
            _ => None,
        }
    }

    /// 道具 (tool_use_id) に結び付く入力か。
    fn is_tool_event(self) -> bool {
        matches!(self, Sub::Pre | Sub::Post | Sub::PostFailure | Sub::Denied)
    }

    /// 入力にサービスの無い状態で、サービスを起動してよいか。pre と session-start だけ
    /// (post / session-end のために起動しても、終える相手の記録が無い)。
    fn may_start_service(self) -> bool {
        matches!(self, Sub::Pre | Sub::SessionStart)
    }
}

/// フックを実行する。必ず返る (時間切れ・panic のときも)。標準出力へは何も書かない。
pub fn run(args: &[String]) {
    // 既定の panic フックは標準エラーに複数行を書く。1 行にして、書けなくても続ける
    std::panic::set_hook(Box::new(|info| {
        let place = info.location().map(|l| format!("{}:{}", l.file(), l.line())).unwrap_or_default();
        note(&format!("内部の失敗 (panic) {place}"));
    }));
    let deadline = Instant::now() + HOOK_BUDGET;
    let owned = args.to_vec();
    let (done_tx, done_rx) = channel::<()>();
    let spawned = std::thread::Builder::new().name("hook".to_string()).spawn(move || {
        let outcome = catch_unwind(AssertUnwindSafe(|| execute(&owned, deadline)));
        if outcome.is_err() {
            note("内部の失敗のため何もせず終わる");
        }
        let _ = done_tx.send(());
    });
    if spawned.is_err() {
        note("作業スレッドを作れない");
        return;
    }
    // 作業が止まっていても (応答しない pipe、閉じない標準入力) ここで切り上げる。スレッドはプロセスの終了で消える
    if done_rx.recv_timeout(HOOK_BUDGET).is_err() {
        note("5 秒を超えたので途中でやめる");
    }
}

/// 標準エラーへ 1 行。書けなくても (閉じていても) 何も起こさない。
fn note(message: &str) {
    let _ = writeln!(io::stderr(), "mye_track hook: {message}");
}

fn remaining(deadline: Instant) -> Duration {
    deadline.saturating_duration_since(Instant::now())
}

struct Command {
    sub: Sub,
    project: Option<PathBuf>,
}

fn parse_command(args: &[String]) -> Result<Command, String> {
    let mut iter = args.iter();
    let sub = iter
        .next()
        .and_then(|text| Sub::parse(text))
        .ok_or("hook の次は pre / post / post-failure / denied / turn-end / session-start / session-end")?;
    let mut project = None;
    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "--project" => {
                let value = iter.next().ok_or("--project に値が要る")?;
                project = Some(std::path::absolute(value).unwrap_or_else(|_| PathBuf::from(value)));
            }
            other => return Err(format!("不明な引数 {other}")),
        }
    }
    Ok(Command { sub, project })
}

/// 標準入力を全部読む。上限を超えたら None (読めなかったものとして扱う)。
fn read_stdin() -> Option<Vec<u8>> {
    let mut bytes = Vec::new();
    io::stdin().lock().take(MAX_STDIN_BYTES + 1).read_to_end(&mut bytes).ok()?;
    (bytes.len() as u64 <= MAX_STDIN_BYTES).then_some(bytes)
}

/// `MYE_TRACK_HOOK_DUMP` があるときだけ、入力を 1 行にして追記する。失敗しても何も起こさない。
fn dump_input(raw: &[u8]) {
    let Some(path) = std::env::var_os(ENV_DUMP).filter(|v| !v.is_empty()) else { return };
    // JSON の空白としての改行だけを潰す (文字列の中の改行は JSON では \n と書かれる)
    let mut line: Vec<u8> = raw.iter().map(|&b| if b == b'\n' || b == b'\r' { b' ' } else { b }).collect();
    line.push(b'\n');
    let written = OpenOptions::new().create(true).append(true).open(&path).and_then(|mut file| file.write_all(&line));
    if let Err(e) = written {
        note(&format!("{ENV_DUMP} へ書けない: {e}"));
    }
}

/// フックの入力 (知らない項目は無視する)。
///
/// 公式の入力は項目が増えうるので、サービスの pipe (`bad_args`) と違って知らないキーを拒否しない。
struct Input {
    hook_event_name: Option<String>,
    session_id: Option<String>,
    cwd: Option<String>,
    tool_name: Option<String>,
    tool_use_id: Option<String>,
    tool_input: Value,
    tool_response: Value,
}

fn text_of(value: &Value, key: &str) -> Option<String> {
    value.get(key).and_then(Value::as_str).filter(|s| !s.is_empty()).map(str::to_string)
}

fn parse_input(raw: &[u8]) -> Option<Input> {
    let value: Value = serde_json::from_slice(raw).ok()?;
    value.as_object()?;
    Some(Input {
        hook_event_name: text_of(&value, "hook_event_name"),
        session_id: text_of(&value, "session_id"),
        cwd: text_of(&value, "cwd"),
        tool_name: text_of(&value, "tool_name"),
        tool_use_id: text_of(&value, "tool_use_id"),
        tool_input: value.get("tool_input").cloned().unwrap_or(Value::Null),
        tool_response: value.get("tool_response").cloned().unwrap_or(Value::Null),
    })
}

/// 編集の道具。`Write` と `Edit` だけを扱う。
#[derive(Clone, Copy, PartialEq, Eq)]
enum Tool {
    Write,
    Edit,
}

impl Tool {
    fn parse(name: &str) -> Option<Tool> {
        match name {
            "Write" => Some(Tool::Write),
            "Edit" => Some(Tool::Edit),
            _ => None,
        }
    }

    fn operation_type(self) -> &'static str {
        match self {
            Tool::Write => "write",
            Tool::Edit => "edit",
        }
    }

    /// 宣言した編集。必要な項目が無ければ None (宣言なしで op_begin する = Claimed 止まり)。
    fn declared_edit(self, tool_input: &Value) -> Option<Value> {
        match self {
            Tool::Write => {
                let content = tool_input.get("content")?.as_str()?;
                Some(json!({ "kind": "write", "content": content }))
            }
            Tool::Edit => {
                let old = tool_input.get("old_string")?.as_str()?;
                let new = tool_input.get("new_string")?.as_str()?;
                let replace_all = tool_input.get("replace_all").and_then(Value::as_bool).unwrap_or(false);
                Some(json!({ "kind": "replace", "oldString": old, "newString": new, "replaceAll": replace_all }))
            }
        }
    }
}

/// `tool_response` から失敗が分かれば `failed`、判定できないものは `ok`。
/// 本物の形は未確認 (D14 では Write の例が `{"filePath":..,"type":"create"}`)。分かる印だけを見る。
fn result_of(response: &Value) -> &'static str {
    let Some(object) = response.as_object() else { return "ok" };
    let flagged = |key: &str, expected: bool| object.get(key).and_then(Value::as_bool) == Some(expected);
    let has_error = object.get("error").is_some_and(|e| !e.is_null() && e.as_str() != Some(""));
    if flagged("success", false) || flagged("is_error", true) || flagged("isError", true) || has_error {
        "failed"
    } else {
        "ok"
    }
}

/// `op_end` に送る結果。`post` に PostToolUseFailure の入力が来た場合 (設定の誤り) も失敗として扱う。
fn end_result(sub: Sub, input: &Input) -> &'static str {
    match sub {
        Sub::Denied => "denied",
        Sub::PostFailure => "failed",
        _ if input.hook_event_name.as_deref() == Some("PostToolUseFailure") => "failed",
        _ => result_of(&input.tool_response),
    }
}

/// プロジェクトのルートと、サービスを起動してよいか。
///
/// 1. `--project` があればそれ (起動してよい)。
/// 2. 無ければ `start` (対象のファイル、無ければ cwd) から親へたどり、`.mye\ai_changes\service.json` がある最初の
///    ディレクトリ (サービスが動いたことのあるプロジェクト)。起動はしない。
/// 3. どちらも無ければ None。cwd はプロジェクトのルートとは限らず、そのまま使うとサブディレクトリを別の
///    プロジェクトとして追跡してしまう。
fn resolve_root(explicit: Option<&Path>, start: Option<&Path>) -> Option<(PathBuf, bool)> {
    if let Some(root) = explicit {
        return Some((root.to_path_buf(), true));
    }
    let found = start?.ancestors().find(|dir| Paths::for_project(dir).dir.join("service.json").is_file())?;
    Some((found.to_path_buf(), false))
}

/// 対象のファイルの絶対パス (相対なら cwd 基準)。
fn absolute_file(file: &str, cwd: Option<&str>) -> PathBuf {
    let path = Path::new(file);
    match cwd {
        Some(cwd) if path.is_relative() => Path::new(cwd).join(path),
        _ => path.to_path_buf(),
    }
}

fn execute(args: &[String], deadline: Instant) {
    let command = match parse_command(args) {
        Ok(command) => command,
        Err(e) => return note(&e),
    };
    if std::env::var_os(ENV_TEST_PANIC).is_some() {
        panic!("{ENV_TEST_PANIC} が設定されている");
    }
    let Some(raw) = read_stdin() else { return note("標準入力を読めない") };
    dump_input(&raw);
    let Some(input) = parse_input(&raw) else { return note("標準入力が JSON のオブジェクトではない") };

    let edit = command.sub.is_tool_event();
    let mut tool = None;
    let mut file = None;
    if edit {
        // 対応しない道具は静かに終わる (matcher で絞っていない設定でも動く)
        let Some(parsed) = input.tool_name.as_deref().and_then(Tool::parse) else { return };
        tool = Some(parsed);
        // 操作は tool_use_id で引くので、終わりの知らせはパスが無くても働く (プロジェクトは cwd から決める)
        match text_of(&input.tool_input, "file_path") {
            Some(path) => file = Some(absolute_file(&path, input.cwd.as_deref())),
            None if matches!(command.sub, Sub::Pre | Sub::Post) => return note("tool_input.file_path が無い"),
            None => {}
        }
    }
    let Some(session_id) = input.session_id.clone() else { return note("session_id が無い") };
    let tool_use_id = input.tool_use_id.clone();
    if edit && tool_use_id.is_none() {
        return note("tool_use_id が無い");
    }

    let start = file.clone().or_else(|| input.cwd.as_deref().map(PathBuf::from));
    let Some((root, explicit)) = resolve_root(command.project.as_deref(), start.as_deref()) else {
        return note("プロジェクトを決められない (--project が無く、親にサービスの跡も無い)");
    };
    let target = match &file {
        Some(path) => match normalize_path(&root, &path.to_string_lossy(), false) {
            Ok(_) => Some(path.to_string_lossy().into_owned()),
            Err(_) => return note("file_path がプロジェクトの外にある"),
        },
        None => None,
    };

    let allow_start = explicit && command.sub.may_start_service();
    let mut link = match Link::open(&root, allow_start, deadline) {
        Ok(link) => link,
        Err(e) => return note(&e),
    };
    let outcome = match command.sub {
        Sub::SessionStart => link.begin_session(&session_id).map(|_| ()),
        Sub::SessionEnd => link.call("session_end", json!({ "clientSessionId": session_id })).map(|_| ()),
        Sub::Pre => {
            let (Some(tool), Some(target), Some(op_id)) = (tool, target, tool_use_id) else { return };
            link.pre(&session_id, &op_id, tool, &target, &input.tool_input)
        }
        Sub::Post | Sub::PostFailure | Sub::Denied => {
            let Some(op_id) = tool_use_id else { return };
            let args = json!({ "clientSessionId": session_id, "clientOpId": op_id, "result": end_result(command.sub, &input) });
            link.call("op_end", args).map(|_| ())
        }
        Sub::TurnEnd => link.call("session_turn_end", json!({ "clientSessionId": session_id })).map(|_| ()),
    };
    if let Err(e) = outcome {
        note(&e);
    }
}

/// サービスへ繋がった接続 (hello 済み)。
struct Link {
    client: Client,
    root: PathBuf,
    next_id: u64,
}

impl Link {
    /// 繋いで hello する。サービスが居なければ、`allow_start` のときだけ起動して待つ (全体の `deadline` の内側)。
    fn open(root: &Path, allow_start: bool, deadline: Instant) -> Result<Link, String> {
        let name = pipe_name(&project_id(root));
        let mut started = false;
        let mut retried = false;
        loop {
            let mut client = match Client::connect(&name, Duration::ZERO) {
                Ok(client) => client,
                Err(e) if e.kind() == io::ErrorKind::NotFound => {
                    if !allow_start {
                        return Err("サービスは動いていない".to_string());
                    }
                    if started {
                        return Err("起動したサービスの pipe が時間内に現れない".to_string());
                    }
                    start_service(root)?;
                    started = true;
                    Client::connect(&name, remaining(deadline)).map_err(|e| format!("起動したサービスに繋がらない: {e}"))?
                }
                Err(e) => return Err(format!("サービスに繋がらない: {e}")),
            };
            let token = wait_token(root, deadline, None).ok_or("service.json のトークンを読めない")?;
            let hello = json!({
                "proto": PROTO_VERSION,
                "role": "hook",
                "token": token,
                "projectRoot": root.to_string_lossy(),
                "pid": std::process::id(),
            });
            let reply = client.request(1, "hello", hello).map_err(|e| format!("hello に失敗: {e}"))?;
            if reply["ok"] == true {
                if reply["result"]["projectId"].as_str() != Some(project_id(root).as_str()) {
                    return Err("別のプロジェクトのサービスに繋がった (projectId が違う)".to_string());
                }
                return Ok(Link { client, root: root.to_path_buf(), next_id: 2 });
            }
            let code = reply["error"]["code"].as_str().unwrap_or("?");
            // サービスの再起動で変わったトークンを読み直し、1 回だけやり直す
            if code == "bad_token" && !retried {
                retried = true;
                wait_token(root, deadline, Some(&token));
                continue;
            }
            return Err(format!("hello が拒否された: {code} {}", reply["error"]["detail"].as_str().unwrap_or("")));
        }
    }

    /// 1 件の op。失敗の応答は説明の文字列にする。
    fn call(&mut self, op: &str, args: Value) -> Result<Value, String> {
        let id = self.next_id;
        self.next_id += 1;
        let reply = self.client.request(id, op, args).map_err(|e| format!("{op} に失敗: {e}"))?;
        if reply["ok"] == true {
            return Ok(reply["result"].clone());
        }
        Err(format!(
            "{op} が拒否された: {} {}",
            reply["error"]["code"].as_str().unwrap_or("?"),
            reply["error"]["detail"].as_str().unwrap_or("")
        ))
    }

    fn begin_session(&mut self, session_id: &str) -> Result<Value, String> {
        let host_terminal = host_terminal_from_env(std::env::var_os(ENV_TERMINAL_ID));
        let args = session_begin_args(session_id, &self.root, host_terminal.as_deref());
        self.call("session_begin", args)
    }

    /// 編集の前: セッションを (無ければ) 作り、操作を始める。
    fn pre(&mut self, session_id: &str, op_id: &str, tool: Tool, target: &str, tool_input: &Value) -> Result<(), String> {
        self.begin_session(session_id)?;
        let mut args = json!({
            "clientSessionId": session_id,
            "clientOpId": op_id,
            "operationType": tool.operation_type(),
            "targets": [target],
        });
        if let Some(declared) = tool.declared_edit(tool_input) {
            args["declaredEdit"] = declared;
            // 1 行の上限を超える宣言は付けない。Claimed 止まりで記録する (欠落させない)
            if request_line_len(self.next_id, "op_begin", &args) > MAX_LINE_BYTES {
                args.as_object_mut().map(|o| o.remove("declaredEdit"));
            }
        }
        self.call("op_begin", args).map(|_| ())
    }
}

/// 環境変数 `MYE_TERMINAL_ID` の値から `hostTerminal` に載せる文字列を取る。
/// 無い・空・UTF-8 でない・サービスの上限 (`MAX_HOST_TERMINAL_BYTES`) を超える・制御文字を含むものは None。
/// 拒否されて session_begin ごと失敗するより、統合ターミナルの印だけを諦めて記録を続ける (AI の作業と変更の記録を止めない)。
pub fn host_terminal_from_env(value: Option<std::ffi::OsString>) -> Option<String> {
    let id = value?.into_string().ok()?;
    let usable = !id.is_empty() && id.len() <= MAX_HOST_TERMINAL_BYTES && !id.chars().any(char::is_control);
    usable.then_some(id)
}

/// `session_begin` の引数。`source` は常に "terminal" のまま (統合ターミナルかどうかは `hostTerminal` の有無で表す)。
pub fn session_begin_args(session_id: &str, root: &Path, host_terminal: Option<&str>) -> Value {
    let mut args = json!({
        "clientSessionId": session_id,
        "provider": "claude",
        "source": "terminal",
        "workspace": root.to_string_lossy(),
    });
    if let Some(id) = host_terminal {
        args["hostTerminal"] = json!(id);
    }
    args
}

/// 送る 1 行の長さ (改行を除く)。`Client::request` が組む形と同じ。
fn request_line_len(id: u64, op: &str, args: &Value) -> usize {
    json!({ "id": id, "op": op, "args": args }).to_string().len()
}

/// service.json のトークン。サービスは pipe を張った後に書くので、`deadline` まで待つ。`not` と同じ値は新しくないので待つ。
fn wait_token(root: &Path, deadline: Instant, not: Option<&str>) -> Option<String> {
    let until = match not {
        Some(_) => (Instant::now() + TOKEN_REFRESH_WAIT).min(deadline),
        None => deadline,
    };
    loop {
        let token = read_service_json(root).and_then(|v| v["token"].as_str().map(str::to_string));
        if let Some(token) = token.filter(|t| Some(t.as_str()) != not) {
            return Some(token);
        }
        if Instant::now() >= until {
            return None;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 失敗の印がある応答だけ_failed() {
        assert_eq!(result_of(&json!({"filePath": "a", "type": "create"})), "ok");
        assert_eq!(result_of(&json!("done")), "ok");
        assert_eq!(result_of(&Value::Null), "ok");
        assert_eq!(result_of(&json!({"success": false})), "failed");
        assert_eq!(result_of(&json!({"is_error": true})), "failed");
        assert_eq!(result_of(&json!({"error": "boom"})), "failed");
        assert_eq!(result_of(&json!({"error": null})), "ok");
        assert_eq!(result_of(&json!({"success": true})), "ok");
    }

    #[test]
    fn 宣言の形() {
        let edit = json!({"file_path": "a", "old_string": "x", "new_string": "y", "replace_all": true});
        assert_eq!(
            Tool::Edit.declared_edit(&edit).unwrap(),
            json!({"kind": "replace", "oldString": "x", "newString": "y", "replaceAll": true})
        );
        assert_eq!(Tool::Write.declared_edit(&json!({"content": "c"})).unwrap(), json!({"kind": "write", "content": "c"}));
        assert!(Tool::Write.declared_edit(&json!({})).is_none());
        assert!(Tool::Edit.declared_edit(&json!({"old_string": "x"})).is_none());
        // replace_all が無ければ偽
        let plain = json!({"old_string": "x", "new_string": "y"});
        assert_eq!(Tool::Edit.declared_edit(&plain).unwrap()["replaceAll"], false);
    }

    #[test]
    fn 統合ターミナルの_id_は条件を満たすときだけ_host_terminal_になる() {
        use std::ffi::OsString;
        let id = |s: &str| host_terminal_from_env(Some(OsString::from(s)));
        assert_eq!(id("4242-1763500000-3").as_deref(), Some("4242-1763500000-3"));
        assert_eq!(host_terminal_from_env(None), None, "環境変数が無い = 統合ターミナルの外");
        assert_eq!(id(""), None, "空");
        assert_eq!(id("a\nb"), None, "制御文字");
        assert_eq!(id(&"x".repeat(MAX_HOST_TERMINAL_BYTES)).map(|v| v.len()), Some(MAX_HOST_TERMINAL_BYTES));
        assert_eq!(id(&"x".repeat(MAX_HOST_TERMINAL_BYTES + 1)), None, "サービスが拒む長さは送らない");
        #[cfg(unix)]
        {
            use std::os::unix::ffi::OsStringExt;
            assert_eq!(host_terminal_from_env(Some(OsString::from_vec(vec![0xff, 0xfe]))), None, "UTF-8 でない");
        }
    }

    #[test]
    fn session_begin_の引数は_host_terminal_があるときだけ載り_source_は変わらない() {
        let root = Path::new("C:/proj");
        let plain = session_begin_args("s1", root, None);
        assert!(plain.get("hostTerminal").is_none(), "旧フックと同じ形: {plain}");
        assert_eq!(plain["source"], "terminal");
        let hosted = session_begin_args("s1", root, Some("10-20-3"));
        assert_eq!(hosted["hostTerminal"], "10-20-3");
        assert_eq!(hosted["source"], "terminal", "source は統合ターミナルでも terminal のまま");
        assert_eq!(hosted["clientSessionId"], "s1");
        assert_eq!(hosted["provider"], "claude");
    }

    #[test]
    fn 対応しない道具と引数() {
        assert!(Tool::parse("Bash").is_none());
        assert!(Tool::parse("MultiEdit").is_none());
        assert!(parse_command(&["pre".to_string()]).is_ok());
        assert!(parse_command(&["pre".to_string(), "--project".to_string()]).is_err());
        assert!(parse_command(&["post".to_string(), "--x".to_string()]).is_err());
        assert!(parse_command(&["nope".to_string()]).is_err());
        assert!(parse_command(&[]).is_err());
    }

    #[test]
    fn プロジェクトは_project_か親の_service_json_から決まる() {
        let base = std::env::temp_dir().join(format!("mye_track_hookunit_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let sub = base.join("a").join("b");
        std::fs::create_dir_all(&sub).unwrap();
        // 何も無ければ決まらない
        assert!(resolve_root(None, Some(&sub.join("f.txt"))).is_none());
        // --project は無条件に採る (起動してよい)
        assert_eq!(resolve_root(Some(&base), Some(&sub)), Some((base.clone(), true)));
        // 親にサービスの跡があれば、それ (起動してはいけない)
        let dir = Paths::for_project(&base).dir;
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("service.json"), "{}").unwrap();
        assert_eq!(resolve_root(None, Some(&sub.join("f.txt"))), Some((base.clone(), false)));
        // 内側にも跡があれば、近いほう
        let inner = Paths::for_project(&base.join("a")).dir;
        std::fs::create_dir_all(&inner).unwrap();
        std::fs::write(inner.join("service.json"), "{}").unwrap();
        assert_eq!(resolve_root(None, Some(&sub)), Some((base.join("a"), false)));
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn 巨大な宣言は行の上限を超える() {
        let big = "x".repeat(MAX_LINE_BYTES);
        let args = json!({"declaredEdit": {"kind": "write", "content": big}});
        assert!(request_line_len(2, "op_begin", &args) > MAX_LINE_BYTES);
        assert!(request_line_len(2, "op_begin", &json!({})) < 100);
    }
}
