//! 記録を伴う op (`session_begin` / `session_end` / `op_begin` / `op_end` / `report` と、Editor の `editor_saved`、
//! ipc-contract.md §4.2、§4.4)。
//!
//! 要求の検査 → 管理された操作は監視を待たずにパスを自分で読む (spec D31) → Writer へ 1 件の塊として渡す、の順。
//! Writer のキューがあふれていれば `busy`。パスはサービス側で正規化し、プロジェクトの外を指すものは `bad_args`
//! (クライアントの言うパスを信用しない。要求 §3.3)。

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use rusqlite::{Connection, OptionalExtension};
use serde_json::{json, Map, Value};

use crate::content::{read_stable, ReadOutcome};
use crate::db::now_ms;
use crate::declared_edit::DeclaredEdit;
use crate::gitref;
use crate::hash::sha256_hex;
use crate::observe::Source;
use crate::operation::{self, OPERATION_TYPES};
use crate::ops::open_reader;
use crate::process::process_start_ms;
use crate::protocol::{code, Args, OpError, Role};
use crate::restore;
use crate::random::uuid_v4;
use crate::review;
use crate::session::{self, find_session, record_and_apply, SessionKey, SessionRow};
use crate::store::{Store, StoreError};
use crate::watch::FlushHandle;
use crate::writer::{Writer, WriterError};

/// `provider` と `source` の値 (ipc-contract.md §4.2)。
const PROVIDERS: &[&str] = &["claude", "codex", "gemini", "other"];
const SOURCES: &[&str] = &["terminal", "mcp", "external"];

/// 1 回の要求が指せるパスの数の上限 (巨大な配列でサービスを塞がない)。
const MAX_TARGETS: usize = 256;
/// 1 つのパスの長さの上限。
const MAX_PATH_CHARS: usize = 1024;
/// 管理された操作の読み取りを待つ時間の既定 (ipc-contract.md §6 の 5 秒の内側)。
pub const DEFAULT_FLUSH_BUDGET: Duration = Duration::from_secs(2);
/// 読み取りの安定を確かめる回数 (`read_stable`)。
const STABLE_ATTEMPTS: u32 = 5;

/// 記録の op が使うサービスの状態。
#[derive(Clone)]
pub struct RecordEnv {
    pub root: PathBuf,
    pub db_path: PathBuf,
    pub writer: Arc<Writer>,
    pub flush: FlushHandle,
    /// 接続している Editor の数。`worktree` の根拠 (d) の確認に使う。
    pub editor_connections: Arc<AtomicUsize>,
    /// Writer のキューがこれを超えたら `busy`。
    pub queue_limit: usize,
    /// 管理された操作の読み取りを待つ時間 (全体)。
    pub flush_budget: Duration,
}

/// 要求した接続。`pid` は pipe から得た値 (申告ではない)。
#[derive(Clone, Copy, Debug)]
pub struct Caller {
    pub role: Role,
    pub pid: Option<u32>,
}

fn store_err(e: StoreError) -> OpError {
    match e {
        StoreError::Degraded(reason) => OpError::new(code::DB_DEGRADED, format!("DB へ書けない状態: {reason}")),
        other => OpError::new(code::INTERNAL, format!("記録に失敗: {other}")),
    }
}

fn db_err(e: rusqlite::Error) -> OpError {
    OpError::new(code::INTERNAL, format!("DB の読み取りに失敗: {e}"))
}

fn random_id() -> Result<String, OpError> {
    uuid_v4().map_err(|e| OpError::new(code::INTERNAL, format!("識別子を作れない: {e}")))
}

/// Writer のジョブとして実行し、結果を待つ。キューがあふれていれば `busy`。
fn run_in_writer<R: Send + 'static>(
    env: &RecordEnv,
    job: impl FnOnce(&mut Store) -> Result<R, OpError> + Send + 'static,
) -> Result<R, OpError> {
    let rx = env.writer.submit_bounded(env.queue_limit, job).map_err(|e| match e {
        WriterError::Busy => OpError::new(code::BUSY, "Writer のキューが詰まっている"),
        other => OpError::new(code::INTERNAL, other.to_string()),
    })?;
    rx.recv().map_err(|_| OpError::new(code::INTERNAL, "Writer のジョブが失敗した"))?
}

// ---- パス ------------------------------------------------------------------------------

/// パスを '/' 区切りの成分に分け、`.` と `..` を字面で解決する。ルートより上へ出るなら None。
fn components(text: &str) -> Option<Vec<String>> {
    let mut stack: Vec<String> = Vec::new();
    for part in text.replace('\\', "/").split('/') {
        match part {
            "" | "." => {}
            ".." => {
                stack.pop()?;
            }
            other => stack.push(other.to_string()),
        }
    }
    Some(stack)
}

fn strip_verbatim(text: &str) -> &str {
    text.strip_prefix(r"\\?\").or_else(|| text.strip_prefix("//?/")).unwrap_or(text)
}

/// クライアントが言ったパスを、プロジェクトのルートからの相対パス ('/' 区切り) にする。
/// 相対パスはプロジェクトのルート基準 (`workspace` 基準ではない)。絶対パスも受ける。外を指すなら `bad_args`。
/// 字面の正規化だけで、シンボリックリンクは辿らない。`allow_root` が真ならルート自身 (空文字) も許す。
pub fn normalize_path(root: &Path, input: &str, allow_root: bool) -> Result<String, OpError> {
    let outside = || OpError::bad_args(format!("パスがプロジェクトの外を指している: {}", crate::protocol::clip(input)));
    if input.is_empty() || input.contains('\0') || input.chars().count() > MAX_PATH_CHARS {
        return Err(OpError::bad_args("パスが空、または長すぎる"));
    }
    let text = strip_verbatim(input);
    let bytes = text.as_bytes();
    let absolute = text.starts_with(['/', '\\']) || (bytes.len() >= 2 && bytes[1] == b':');
    let parts = components(text).ok_or_else(outside)?;
    let relative: Vec<String> = if absolute {
        let root_text = root.to_string_lossy().into_owned();
        let root_parts = components(strip_verbatim(&root_text)).ok_or_else(outside)?;
        if parts.len() < root_parts.len() || !parts.iter().zip(&root_parts).all(|(a, b)| a.eq_ignore_ascii_case(b)) {
            return Err(outside());
        }
        parts[root_parts.len()..].to_vec()
    } else {
        parts
    };
    // ':' (代替データストリーム、ドライブ指定) と Windows で使えない文字は受けない
    if relative.iter().any(|c| c.contains([':', '<', '>', '"', '|', '?', '*'])) {
        return Err(OpError::bad_args(format!("パスに使えない文字がある: {}", crate::protocol::clip(input))));
    }
    if relative.is_empty() && !allow_root {
        return Err(OpError::bad_args("パスがプロジェクトのルート自身を指している"));
    }
    Ok(relative.join("/"))
}

fn normalize_list(root: &Path, key: &str, values: Vec<String>) -> Result<Vec<String>, OpError> {
    if values.len() > MAX_TARGETS {
        return Err(OpError::bad_args(format!("{key} は {MAX_TARGETS} 件まで")));
    }
    let mut out: Vec<String> = Vec::new();
    for v in values {
        let rel = normalize_path(root, &v, false)?;
        if !out.iter().any(|p| p.eq_ignore_ascii_case(&rel)) {
            out.push(rel);
        }
    }
    Ok(out)
}

fn read_target(root: &Path, rel: &str) -> (Option<String>, Option<Vec<u8>>, bool) {
    match read_stable(&root.join(rel), STABLE_ATTEMPTS) {
        ReadOutcome::Content { bytes, .. } => (Some(sha256_hex(&bytes)), Some(bytes), true),
        ReadOutcome::Missing | ReadOutcome::NotFile => (None, None, true),
        ReadOutcome::Busy | ReadOutcome::Failed(_) => (None, None, false),
    }
}

// ---- セッションの引き方 -----------------------------------------------------------------

struct SessionArg {
    id: Option<String>,
    client: Option<String>,
}

impl SessionArg {
    fn take(args: &mut Args) -> Result<SessionArg, OpError> {
        let arg = SessionArg { id: args.string("sessionId")?, client: args.string("clientSessionId")? };
        if arg.id.is_none() && arg.client.is_none() {
            return Err(OpError::bad_args("sessionId か clientSessionId が要る"));
        }
        Ok(arg)
    }

    fn resolve(&self, conn: &Connection) -> Result<SessionRow, OpError> {
        let key = match (&self.id, &self.client) {
            (Some(id), _) => SessionKey::Id(id),
            (None, Some(client)) => SessionKey::Client(client),
            (None, None) => return Err(OpError::bad_args("sessionId か clientSessionId が要る")),
        };
        find_session(conn, key)
            .map_err(db_err)?
            .ok_or_else(|| OpError::new(code::NOT_FOUND, "そのセッションは無い"))
    }
}

fn conn_of(store: &Store) -> Result<&Connection, OpError> {
    store.conn.as_ref().ok_or_else(|| OpError::new(code::DB_DEGRADED, "DB へ書けない状態"))
}

/// 終了したセッションは操作を受けない。中断されていれば再開のイベントを残す (接続元の情報があれば更新する)。
fn ensure_running(store: &mut Store, row: &SessionRow, at: i64, caller: &Caller) -> Result<(), OpError> {
    match row.status.as_str() {
        "finished" => Err(OpError::bad_args("セッションは終了している")),
        "interrupted" => {
            record_and_apply(store, at, "session_resumed", process_payload(&row.session_id, caller)).map_err(store_err)
        }
        _ => Ok(()),
    }
}

/// MCP の接続は、pipe から得た PID と開始時刻をセッションに持つ。フックは短命なので持たない。
fn process_payload(session_id: &str, caller: &Caller) -> Value {
    let (pid, start) = process_of(caller);
    json!({ "session_id": session_id, "process_id": pid, "process_start": start })
}

fn process_of(caller: &Caller) -> (Option<u32>, Option<i64>) {
    match (caller.role, caller.pid) {
        (Role::Mcp, Some(pid)) => (Some(pid), process_start_ms(pid)),
        _ => (None, None),
    }
}

// ---- session_begin / session_end ------------------------------------------------------------

/// 引き出せる根拠の種類 (このセッションで出せるもの)。
fn capabilities(role: Role, worktree: bool) -> Vec<&'static str> {
    let mut kinds = vec!["observed", "op_window", "before_match", "declared_edit_match", "time_near", "preexisting", "overlap"];
    if role == Role::Mcp {
        kinds.extend(["report_hash_match", "report_only"]);
    }
    if worktree {
        kinds.push("worktree");
    }
    kinds
}

pub fn session_begin(env: &RecordEnv, caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let provider = args.choice("provider", PROVIDERS)?.ok_or_else(|| OpError::bad_args("provider が要る"))?;
    let workspace_arg = args.required_string("workspace")?;
    let source = args.choice("source", SOURCES)?.ok_or_else(|| OpError::bad_args("source が要る"))?;
    let client_session_id = args.string("clientSessionId")?;
    let isolated = args.bool("isolated")?.unwrap_or(false);
    args.finish()?;
    let workspace_rel = normalize_path(&env.root, &workspace_arg, true)?;
    let workspace = if workspace_rel.is_empty() { env.root.clone() } else { env.root.join(&workspace_rel) };
    let workspace_text = workspace.to_string_lossy().into_owned();
    let (process_id, process_start) = process_of(caller);
    // `worktree` の根拠の条件のうち、開始時に決まるもの: (a) 申告 (b) 構造 (d) Editor が居ない。(c) は変更ごとに確かめる
    let linked = gitref::is_linked_worktree(&env.root);
    let worktree_eligible = isolated && linked && env.editor_connections.load(Ordering::SeqCst) == 0;
    let base_commit = gitref::base_commit(&env.root);
    let session_id = random_id()?;
    let role = caller.role;
    let caller = *caller;
    let at = now_ms();
    run_in_writer(env, move |store| {
        if let Some(client) = &client_session_id {
            let existing = find_session(conn_of(store)?, SessionKey::Client(client)).map_err(db_err)?;
            if let Some(row) = existing.filter(|r| r.status != "finished") {
                // 同じ clientSessionId の running / interrupted は、新しく作らずそれを返す
                let reconnect = row.status == "interrupted" || process_id.is_some();
                if reconnect {
                    record_and_apply(store, at, "session_resumed", process_payload(&row.session_id, &caller)).map_err(store_err)?;
                }
                return Ok(session_reply(store, &row.session_id, true, role, worktree_eligible));
            }
        }
        let capabilities = capabilities(role, worktree_eligible);
        let payload = json!({
            "session_id": session_id,
            "provider": provider,
            "source": source,
            "client_session_id": client_session_id,
            "workspace": workspace_text,
            "workspace_rel": workspace_rel,
            "base_commit": base_commit,
            "process_id": process_id,
            "process_start": process_start,
            "isolated": isolated,
            "worktree_eligible": worktree_eligible,
            "capabilities": capabilities,
        });
        record_and_apply(store, at, "session_begin", payload).map_err(store_err)?;
        Ok(session_reply(store, &session_id, false, role, worktree_eligible))
    })
}

/// `session_begin` の結果。保存された行から作る (再開のときも同じ形)。
fn session_reply(store: &Store, session_id: &str, resumed: bool, role: Role, worktree_eligible: bool) -> Value {
    let row: Option<(String, Option<String>, String)> = store.conn.as_ref().and_then(|c| {
        c.query_row("SELECT status, base_commit, coverage FROM sessions WHERE session_id = ?1", [session_id], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?))
        })
        .optional()
        .ok()
        .flatten()
    });
    let (status, base_commit, coverage) = row.unwrap_or_else(|| ("running".to_string(), None, "{}".to_string()));
    let coverage: Value = serde_json::from_str(&coverage).unwrap_or(Value::Null);
    let eligible = coverage["worktreeEligible"].as_bool().unwrap_or(worktree_eligible);
    json!({
        "sessionId": session_id,
        "status": status,
        "resumed": resumed,
        "trackingCapabilities": capabilities(role, eligible),
        "baseCommit": base_commit,
    })
}

pub fn session_end(env: &RecordEnv, _caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let key = SessionArg::take(&mut args)?;
    args.finish()?;
    let at = now_ms();
    run_in_writer(env, move |store| {
        let row = key.resolve(conn_of(store)?)?;
        if row.status != "finished" {
            record_and_apply(store, at, "session_end", json!({ "session_id": row.session_id })).map_err(store_err)?;
        }
        let count = session::change_count(conn_of(store)?, &row.session_id).map_err(db_err)?;
        Ok(json!({ "sessionId": row.session_id, "status": "finished", "changeCount": count }))
    })
}

/// 応答 (ターン) の終わり。そのセッションの開いている操作を閉じる。拒否された操作には終わりの知らせが来ないので、
/// 開いたままの窓を残さないための契機 (spec D46)。開いている操作が無ければ何も記録しない。
pub fn session_turn_end(env: &RecordEnv, _caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let key = SessionArg::take(&mut args)?;
    args.finish()?;
    let at = now_ms();
    run_in_writer(env, move |store| {
        let row = key.resolve(conn_of(store)?)?;
        let open = if row.status == "finished" { 0 } else { operation::open_op_ids(conn_of(store)?, &row.session_id).map_err(db_err)?.len() };
        if open > 0 {
            record_and_apply(store, at, "session_turn_end", json!({ "session_id": row.session_id })).map_err(store_err)?;
        }
        Ok(json!({ "sessionId": row.session_id, "closed": open }))
    })
}

// ---- op_begin / op_end ------------------------------------------------------------------------

pub fn op_begin(env: &RecordEnv, caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let key = SessionArg::take(&mut args)?;
    let operation_type = args
        .choice("operationType", OPERATION_TYPES)?
        .ok_or_else(|| OpError::bad_args("operationType が要る"))?;
    let targets = normalize_list(
        &env.root,
        "targets",
        args.string_list("targets")?.ok_or_else(|| OpError::bad_args("targets が要る"))?,
    )?;
    let declared = match args.value("declaredEdit") {
        None => None,
        Some(v) => Some(DeclaredEdit::from_json(&v).map_err(OpError::bad_args)?),
    };
    let client_op_id = args.string("clientOpId")?;
    args.finish()?;
    if targets.is_empty() {
        return Err(OpError::bad_args("targets は 1 件以上"));
    }
    if declared.is_some() && operation_type == "report" {
        return Err(OpError::bad_args("report に declaredEdit は付けられない"));
    }

    // 操作の前の、未記録の変更を先に独立した Revision にする。その後の内容が、この操作の Before になる
    let flushed = env.flush.flush(targets.clone(), Source::ManagedBegin, env.flush_budget);
    let mut begin_flushed = flushed.done;
    let mut taken: Vec<(String, Option<String>)> = Vec::new();
    let mut first_bytes: Option<Vec<u8>> = None;
    for (index, rel) in targets.iter().enumerate() {
        let (hash, bytes, readable) = read_target(&env.root, rel);
        begin_flushed &= readable;
        if index == 0 {
            first_bytes = bytes;
        }
        taken.push((rel.clone(), hash));
    }
    // 宣言した編集を Before に当てた結果。当てられなければ根拠を作らない (Verified にならないだけ)
    let declared_path = targets[0].clone();
    let declared_after_hash = declared.as_ref().and_then(|d| d.apply(first_bytes.as_deref())).map(|after| sha256_hex(&after));
    let operation_id = random_id()?;
    // この操作の窓は、Before を読んだ後に始まる。flush が作った Revision (人の変更) を窓に入れない
    let at = now_ms().max(flushed.observed_at.map_or(0, |t| t + 1));
    let caller = *caller;
    let target_json: Vec<Value> = taken.iter().map(|(p, h)| json!({ "path": p, "hash": h })).collect();
    run_in_writer(env, move |store| {
        let row = key.resolve(conn_of(store)?)?;
        ensure_running(store, &row, at, &caller)?;
        // 宣言は Blob に入れてハッシュだけをイベントに書く (ジャーナルに内容を書かない)。除外パターンのパスは保存しない
        let mut declared_edit = Value::Null;
        let mut declared_hash = declared_after_hash;
        if let Some(edit) = &declared {
            if store.blobs.is_excluded(&declared_path) {
                declared_hash = None;
            } else {
                let body = edit.to_json().to_string().into_bytes();
                match store.blobs.put(&declared_path, &body) {
                    Ok(crate::blob::PutOutcome::Stored { hash, size, .. }) => {
                        declared_edit = json!({ "blob_hash": hash, "size": size });
                    }
                    Ok(crate::blob::PutOutcome::Excluded { .. }) => declared_hash = None,
                    Err(e) => return Err(store_err(StoreError::Blob(e))),
                }
            }
        }
        let payload = json!({
            "operation_id": operation_id,
            "session_id": row.session_id,
            "operation_type": operation_type,
            "targets": target_json,
            "client_op_id": client_op_id,
            "declared_edit": declared_edit,
            "declared_after_hash": declared_hash,
            "declared_path": declared_hash.as_ref().map(|_| declared_path.clone()),
            "begin_flushed": begin_flushed,
        });
        record_and_apply(store, at, "op_begin", payload).map_err(store_err)?;
        Ok(json!({
            "operationId": operation_id,
            "sessionId": row.session_id,
            "targets": target_json,
            "flushed": begin_flushed,
        }))
    })
}

struct OpTarget {
    operation_id: String,
    session_id: String,
    targets: Vec<String>,
    finished: bool,
}

/// `operationId`、または `clientSessionId` + `clientOpId` で操作を引く (フックは pre と post が別のプロセス)。
fn find_operation(conn: &Connection, id: Option<&str>, client_session: Option<&str>, client_op: Option<&str>) -> Result<OpTarget, OpError> {
    let map = |r: &rusqlite::Row<'_>| -> rusqlite::Result<(String, String, String, Option<i64>, Option<String>)> {
        Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?))
    };
    let found = match (id, client_session, client_op) {
        (Some(id), _, _) => conn
            .query_row(
                "SELECT operation_id, session_id, reported_targets, finished_at, result FROM operations WHERE operation_id = ?1",
                [id],
                map,
            )
            .optional(),
        (None, Some(session), Some(op)) => conn
            .query_row(
                "SELECT o.operation_id, o.session_id, o.reported_targets, o.finished_at, o.result
                   FROM operations o JOIN sessions s ON s.session_id = o.session_id
                  WHERE s.client_session_id = ?1 AND json_extract(CASE WHEN json_valid(o.reported_targets) THEN o.reported_targets ELSE '{}' END, '$.clientOpId') = ?2
                  ORDER BY o.started_at DESC, o.operation_id DESC LIMIT 1",
                [session, op],
                map,
            )
            .optional(),
        _ => return Err(OpError::bad_args("operationId か、clientSessionId と clientOpId が要る")),
    }
    .map_err(db_err)?;
    let (operation_id, session_id, raw, finished_at, result) =
        found.ok_or_else(|| OpError::new(code::NOT_FOUND, "その操作は無い"))?;
    let json: Value = serde_json::from_str(&raw).unwrap_or(Value::Null);
    let targets = json["targets"].as_array().into_iter().flatten().filter_map(|t| t["path"].as_str().map(str::to_string)).collect();
    Ok(OpTarget { operation_id, session_id, targets, finished: finished_at.is_some() || result.is_some() })
}

pub fn op_end(env: &RecordEnv, _caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let operation_id = args.string("operationId")?;
    let client_session = args.string("clientSessionId")?;
    let client_op = args.string("clientOpId")?;
    let result = args.choice("result", &["ok", "failed", "denied"])?.ok_or_else(|| OpError::bad_args("result が要る"))?;
    args.finish()?;
    let found = {
        let reader = open_reader(&env.db_path).map_err(|e| OpError::new(code::DB_DEGRADED, format!("DB を読み取りで開けない: {e}")))?;
        find_operation(&reader, operation_id.as_deref(), client_session.as_deref(), client_op.as_deref())?
    };
    // 終わった操作の後ろは待たずにすぐ読む。デバウンスを待たないので、Before / After が操作の前後と一致する
    let flushed = if found.finished {
        crate::watch::FlushOutcome { done: true, observed_at: None }
    } else {
        env.flush.flush(found.targets.clone(), Source::Managed, env.flush_budget)
    };
    let at = now_ms();
    let pending = !flushed.done;
    run_in_writer(env, move |store| {
        if !found.finished {
            record_and_apply(store, at, "op_end", json!({ "operation_id": found.operation_id, "result": result })).map_err(store_err)?;
        }
        let conn = conn_of(store)?;
        let bound = operation::bound_revisions(conn, &found.operation_id).map_err(db_err)?;
        let others = operation::unbound_revisions_in_window(conn, &found.operation_id).map_err(db_err)?;
        let op_result: Option<String> = conn
            .query_row("SELECT result FROM operations WHERE operation_id = ?1", [&found.operation_id], |r| r.get(0))
            .map_err(db_err)?;
        Ok(json!({
            "operationId": found.operation_id,
            "sessionId": found.session_id,
            "result": op_result,
            "revisions": bound,
            "otherChanges": others,
            "pending": pending,
            "alreadyFinished": found.finished,
        }))
    })
}

// ---- report ------------------------------------------------------------------------------------

/// `beforeHashes` / `afterHashes`: `paths` と同じ並びの配列、またはパスをキーにしたオブジェクト。
fn hash_arg(root: &Path, key: &str, value: Option<Value>, paths: &[String]) -> Result<Vec<Option<String>>, OpError> {
    let hash = |v: &Value| -> Result<Option<String>, OpError> {
        match v {
            Value::Null => Ok(None),
            Value::String(s) if s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit()) => Ok(Some(s.to_ascii_lowercase())),
            _ => Err(OpError::bad_args(format!("{key} は 64 桁の 16 進のハッシュか null"))),
        }
    };
    match value {
        None => Ok(vec![None; paths.len()]),
        Some(Value::Array(items)) => {
            if items.len() != paths.len() {
                return Err(OpError::bad_args(format!("{key} は paths と同じ数")));
            }
            items.iter().map(hash).collect()
        }
        Some(Value::Object(map)) => {
            let mut by_path: Map<String, Value> = Map::new();
            for (k, v) in map {
                by_path.insert(normalize_path(root, &k, false)?.to_ascii_lowercase(), v);
            }
            paths.iter().map(|p| by_path.get(&p.to_ascii_lowercase()).map_or(Ok(None), hash)).collect()
        }
        Some(_) => Err(OpError::bad_args(format!("{key} は配列かオブジェクトでなければならない"))),
    }
}

pub fn report(env: &RecordEnv, caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let key = SessionArg::take(&mut args)?;
    let linked_operation = args.string("operationId")?;
    let original = args.string_list("paths")?.ok_or_else(|| OpError::bad_args("paths が要る"))?;
    let paths = normalize_list(&env.root, "paths", original)?;
    let before = hash_arg(&env.root, "beforeHashes", args.value("beforeHashes"), &paths)?;
    let after = hash_arg(&env.root, "afterHashes", args.value("afterHashes"), &paths)?;
    args.finish()?;
    if paths.is_empty() {
        return Err(OpError::bad_args("paths は 1 件以上"));
    }
    // 照合の前に、申告されたパスの今の状態を観測にする
    let flushed = env.flush.flush(paths.clone(), Source::Managed, env.flush_budget);
    let report_id = random_id()?;
    let at = now_ms();
    let pending = !flushed.done;
    let caller = *caller;
    run_in_writer(env, move |store| {
        let row = key.resolve(conn_of(store)?)?;
        ensure_running(store, &row, at, &caller)?;
        if let Some(op) = &linked_operation {
            let owner: Option<String> = conn_of(store)?
                .query_row("SELECT session_id FROM operations WHERE operation_id = ?1", [op], |r| r.get(0))
                .optional()
                .map_err(db_err)?;
            if owner.as_deref() != Some(row.session_id.as_str()) {
                return Err(OpError::bad_args("operationId がそのセッションの操作ではない"));
            }
        }
        let entries: Vec<Value> = paths
            .iter()
            .enumerate()
            .map(|(i, p)| json!({ "path": p, "before_hash": before[i], "after_hash": after[i] }))
            .collect();
        let payload = json!({
            "operation_id": report_id,
            "session_id": row.session_id,
            "linked_operation_id": linked_operation,
            "paths": entries,
        });
        record_and_apply(store, at, "reported", payload).map_err(store_err)?;
        let results = operation::report_results(conn_of(store)?, &report_id, &row.session_id, &paths).map_err(db_err)?;
        Ok(json!({ "operationId": report_id, "sessionId": row.session_id, "results": results, "pending": pending }))
    })
}

// ---- editor_saved -------------------------------------------------------------------------------

/// Editor が「このパスをこの内容で保存した」と知らせる。パスはすぐ読んで観測にし (デバウンスを待たない)、
/// 知らせをイベントとして記録する。同じ内容の観測があれば、その Revision の行為者が Editor の利用者になる
/// (観測が知らせより先でも後でも同じ。照合は `attribution::apply_editor_saved` と新しい Revision の評価)。
pub fn editor_saved(env: &RecordEnv, _caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let path_arg = args.required_string("path")?;
    let hash_arg = args.required_string("hash")?;
    args.finish()?;
    let rel = normalize_path(&env.root, &path_arg, false)?;
    if hash_arg.len() != 64 || !hash_arg.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(OpError::bad_args("hash は 64 桁の 16 進のハッシュ"));
    }
    let hash = hash_arg.to_ascii_lowercase();
    let flushed = env.flush.flush(vec![rel.clone()], Source::Managed, env.flush_budget);
    let pending = !flushed.done;
    let at = now_ms();
    run_in_writer(env, move |store| {
        record_and_apply(store, at, "editor_saved", json!({ "path": rel, "hash": hash })).map_err(store_err)?;
        Ok(json!({ "path": rel, "pending": pending }))
    })
}

// ---- restore_record / restore_expect (M104p2) ----------------------------------------------------

/// 復元の対象 (`targets`) を検査して、記録する形 (snake_case) にする。パスはサービス側で正規化する。
fn parse_restore_targets(env: &RecordEnv, value: Value) -> Result<Vec<Value>, OpError> {
    let Value::Array(items) = value else { return Err(OpError::bad_args("targets は配列でなければならない")) };
    if items.is_empty() || items.len() > restore::MAX_TARGETS {
        return Err(OpError::bad_args(format!("targets は 1〜{} 件", restore::MAX_TARGETS)));
    }
    let mut out = Vec::with_capacity(items.len());
    for item in items {
        let Value::Object(map) = item else { return Err(OpError::bad_args("targets の要素はオブジェクトでなければならない")) };
        if let Some(extra) = map.keys().find(|k| !matches!(k.as_str(), "revisionId" | "scope" | "hunkIds" | "paths" | "sceneChangeIds")) {
            return Err(OpError::bad_args(format!("targets の知らない項目: {}", crate::protocol::clip(extra))));
        }
        let revision_id = map
            .get("revisionId")
            .and_then(Value::as_i64)
            .ok_or_else(|| OpError::bad_args("targets[].revisionId は整数でなければならない"))?;
        let scope = map.get("scope").and_then(Value::as_str).filter(|s| matches!(*s, "file" | "hunks" | "scene"));
        let Some(scope) = scope else { return Err(OpError::bad_args("targets[].scope は file / hunks / scene のどれか")) };
        let hunk_ids: Vec<i64> = match map.get("hunkIds") {
            None | Some(Value::Null) => Vec::new(),
            Some(Value::Array(ids)) => ids
                .iter()
                .map(|v| v.as_i64().ok_or_else(|| OpError::bad_args("targets[].hunkIds は整数の配列")))
                .collect::<Result<_, _>>()?,
            Some(_) => return Err(OpError::bad_args("targets[].hunkIds は整数の配列")),
        };
        // ライブのシーンの復元 (scope = scene): 戻した構造の変更の行
        let scene_change_ids: Vec<i64> = match map.get("sceneChangeIds") {
            None | Some(Value::Null) => Vec::new(),
            Some(Value::Array(ids)) if ids.len() <= restore::MAX_SCENE_IDS => ids
                .iter()
                .map(|v| v.as_i64().ok_or_else(|| OpError::bad_args("targets[].sceneChangeIds は整数の配列")))
                .collect::<Result<_, _>>()?,
            Some(_) => return Err(OpError::bad_args("targets[].sceneChangeIds は整数の配列 (上限あり)")),
        };
        let mut paths: Vec<String> = Vec::new();
        match map.get("paths") {
            None | Some(Value::Null) => {}
            Some(Value::Array(list)) if list.len() <= restore::MAX_PATHS => {
                for p in list {
                    let text = p.as_str().ok_or_else(|| OpError::bad_args("targets[].paths は文字列の配列"))?;
                    paths.push(normalize_path(&env.root, text, false)?);
                }
            }
            Some(_) => return Err(OpError::bad_args("targets[].paths は文字列の配列 (上限あり)")),
        }
        let mut entry = json!({ "revision_id": revision_id, "scope": scope, "hunk_ids": hunk_ids, "paths": paths });
        // 既存の記録 (file / hunks) の形を変えない: scene のときだけ付ける
        if scope == "scene" {
            entry["scene_change_ids"] = json!(scene_change_ids);
        }
        out.push(entry);
    }
    Ok(out)
}

/// `{ パス: null | { hash, size } }` (適用前) または `{ パス: null | hash }` (適用後) を、パスを正規化した形にする。
fn parse_restore_hashes(env: &RecordEnv, key: &str, value: Option<Value>, with_size: bool) -> Result<Option<Value>, OpError> {
    let Some(value) = value else { return Ok(None) };
    let Value::Object(map) = value else { return Err(OpError::bad_args(format!("{key} はオブジェクトでなければならない"))) };
    if map.len() > restore::MAX_PATHS {
        return Err(OpError::bad_args(format!("{key} が多すぎる")));
    }
    let bad = || OpError::bad_args(format!("{key} の値は null か {}", if with_size { "{ hash, size }" } else { "64 桁の 16 進のハッシュ" }));
    let mut out = Map::new();
    for (path, entry) in map {
        let rel = normalize_path(&env.root, &path, false)?;
        let normalized = match entry {
            Value::Null => Value::Null,
            Value::String(hash) if !with_size => {
                if !restore::is_blob_hash(&hash.to_ascii_lowercase()) {
                    return Err(bad());
                }
                Value::String(hash.to_ascii_lowercase())
            }
            Value::Object(o) if with_size => {
                let hash = o.get("hash").and_then(Value::as_str).map(str::to_ascii_lowercase).filter(|h| restore::is_blob_hash(h)).ok_or_else(bad)?;
                let size = o.get("size").and_then(Value::as_i64).filter(|n| *n >= 0).ok_or_else(bad)?;
                json!({ "hash": hash, "size": size })
            }
            _ => return Err(bad()),
        };
        out.insert(rel, normalized);
    }
    Ok(Some(Value::Object(out)))
}

/// 復元の進行を履歴へ記録する。**記録するだけ**でファイルは書かない。`committed` のとき対象の状態を `restored` / `partially_restored` にする
/// (レビューの記録には触れない)。同じ `restoreId` の記録は、状態・結果を更新する。
pub fn restore_record(env: &RecordEnv, _caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let restore_id = args.required_string("restoreId")?;
    let state = args.choice("state", restore::STATES)?.ok_or_else(|| OpError::bad_args("state が要る"))?;
    let targets = args.value("targets").ok_or_else(|| OpError::bad_args("targets が要る"))?;
    let pre = args.value("preHashes");
    let post = args.value("postHashes");
    let error = args.string("error")?;
    args.finish()?;
    if !restore::is_restore_id(&restore_id) {
        return Err(OpError::bad_args("restoreId は英数字と - の 64 文字まで"));
    }
    if error.as_ref().is_some_and(|e| e.chars().count() > restore::MAX_ERROR_CHARS) {
        return Err(OpError::bad_args(format!("error は {} 文字まで", restore::MAX_ERROR_CHARS)));
    }
    let targets = parse_restore_targets(env, targets)?;
    let pre = parse_restore_hashes(env, "preHashes", pre, true)?;
    let post = parse_restore_hashes(env, "postHashes", post, false)?;
    let at = now_ms();
    run_in_writer(env, move |store| {
        let payload = json!({
            "restore_id": restore_id,
            "state": state,
            "targets": targets,
            "pre_hashes": pre,
            "post_hashes": post,
            "error": error,
        });
        record_and_apply(store, at, "restore_record", payload).map_err(store_err)?;
        Ok(json!({ "restoreId": restore_id, "state": state }))
    })
}

/// これから Editor が書く内容の予告。パスごとに `restore_expect` のイベントを記録する。内容 (ハッシュ。`null` は削除) とパスが同じ
/// 変更が観測されたら、その Revision の行為者が `restore` になる (観測が予告より先でも後でも同じ。照合は
/// `attribution::apply_restore_expect` と新しい Revision の評価)。予告は書き込みの前に届くので、ここでは何も読まない。
pub fn restore_expect(env: &RecordEnv, _caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let restore_id = args.required_string("restoreId")?;
    let paths = args.value("paths").ok_or_else(|| OpError::bad_args("paths が要る"))?;
    let hashes = args.value("hashes").ok_or_else(|| OpError::bad_args("hashes が要る"))?;
    args.finish()?;
    if !restore::is_restore_id(&restore_id) {
        return Err(OpError::bad_args("restoreId は英数字と - の 64 文字まで"));
    }
    let (Value::Array(paths), Value::Array(hashes)) = (paths, hashes) else {
        return Err(OpError::bad_args("paths と hashes は配列でなければならない"));
    };
    if paths.is_empty() || paths.len() > restore::MAX_PATHS || paths.len() != hashes.len() {
        return Err(OpError::bad_args(format!("paths と hashes は同じ長さで 1〜{} 件", restore::MAX_PATHS)));
    }
    let mut entries: Vec<(String, Option<String>)> = Vec::with_capacity(paths.len());
    for (path, hash) in paths.iter().zip(hashes.iter()) {
        let text = path.as_str().ok_or_else(|| OpError::bad_args("paths の要素は文字列でなければならない"))?;
        let rel = normalize_path(&env.root, text, false)?;
        let hash = match hash {
            Value::Null => None,
            Value::String(h) if restore::is_blob_hash(&h.to_ascii_lowercase()) => Some(h.to_ascii_lowercase()),
            _ => return Err(OpError::bad_args("hashes の要素は null か 64 桁の 16 進のハッシュ")),
        };
        entries.push((rel, hash));
    }
    let at = now_ms();
    run_in_writer(env, move |store| {
        for (path, hash) in &entries {
            record_and_apply(store, at, "restore_expect", json!({ "restore_id": restore_id, "path": path, "hash": hash })).map_err(store_err)?;
        }
        Ok(json!({ "restoreId": restore_id, "paths": entries.len() }))
    })
}

// ---- review_set ---------------------------------------------------------------------------------

/// `scope` を読む。`ref` は `kind` ごとの形で、正規の書き方 (数字の先頭の 0 なし) だけを受ける。
fn parse_scope(scope: &Value) -> Result<(String, String), OpError> {
    let Value::Object(map) = scope else { return Err(OpError::bad_args("scope はオブジェクトでなければならない")) };
    if let Some(extra) = map.keys().find(|k| !matches!(k.as_str(), "kind" | "ref")) {
        return Err(OpError::bad_args(format!("scope の知らない項目: {}", crate::protocol::clip(extra))));
    }
    let kind = map.get("kind").and_then(Value::as_str).ok_or_else(|| OpError::bad_args("scope.kind が要る"))?;
    if !review::SCOPE_KINDS.contains(&kind) {
        return Err(OpError::bad_args(format!("scope.kind は {} のどれか", review::SCOPE_KINDS.join(" / "))));
    }
    let scope_ref = match map.get("ref") {
        None | Some(Value::Null) => None,
        Some(Value::String(text)) => Some(text.as_str()),
        Some(_) => return Err(OpError::bad_args("scope.ref は文字列でなければならない")),
    };
    let bad = || OpError::bad_args(format!("scope.ref の形が {kind} と合わない"));
    let canonical = match kind {
        "revision" => {
            if scope_ref.is_some() {
                return Err(OpError::bad_args("revision の scope に ref は付けない"));
            }
            String::new()
        }
        "hunk" | "entity" => {
            let n: i64 = scope_ref.ok_or_else(bad)?.parse().map_err(|_| bad())?;
            n.to_string()
        }
        "component" => {
            let (file, component) = scope_ref.ok_or_else(bad)?.split_once('/').ok_or_else(bad)?;
            let file: i64 = file.parse().map_err(|_| bad())?;
            if component.is_empty() || component.contains('/') {
                return Err(bad());
            }
            review::component_ref(file, component)
        }
        _ => {
            let mut parts = scope_ref.ok_or_else(bad)?.splitn(3, '/');
            let (Some(file), Some(component), Some(field)) = (parts.next(), parts.next(), parts.next()) else {
                return Err(bad());
            };
            let file: i64 = file.parse().map_err(|_| bad())?;
            if field.is_empty() {
                return Err(bad());
            }
            review::field_ref(file, (!component.is_empty()).then_some(component), field)
        }
    };
    if scope_ref.is_some_and(|given| given != canonical) {
        return Err(bad());
    }
    Ok((kind.to_string(), canonical))
}

/// 対象がその Revision に実在するかを DB で確かめる。別の Revision の Hunk ID や存在しない範囲への記録を作らない。
fn check_target(conn: &Connection, target: &review::Target) -> Result<(), OpError> {
    let not_found = |what: &str| OpError::new(code::NOT_FOUND, format!("revisionId {} に {what} は無い", target.revision_id));
    let exists = |sql: &str, params: &[&dyn rusqlite::ToSql]| -> Result<bool, OpError> {
        conn.query_row(sql, params, |r| r.get::<_, bool>(0)).map_err(db_err)
    };
    let revision = target.revision_id;
    match target.kind.as_str() {
        "revision" => Ok(()),
        "hunk" => {
            let hunk_id: i64 = target.scope_ref.parse().map_err(|_| OpError::bad_args("hunk の ref は数字"))?;
            if !exists("SELECT EXISTS (SELECT 1 FROM change_hunks WHERE hunk_id = ?1 AND revision_id = ?2)", &[&hunk_id, &revision])? {
                return Err(not_found("その Hunk"));
            }
            // 構造の変更を持つ Revision の葉は構造の変更で、Hunk への記録は状態に効かない (黙って無意味な行を作らない)
            if exists("SELECT EXISTS (SELECT 1 FROM scene_changes WHERE revision_id = ?1)", &[&revision])? {
                return Err(OpError::bad_args("この Revision の葉は構造の変更なので、Hunk には記録できない"));
            }
            Ok(())
        }
        "entity" => {
            let file: i64 = target.scope_ref.parse().map_err(|_| OpError::bad_args("entity の ref は fileId"))?;
            exists("SELECT EXISTS (SELECT 1 FROM scene_changes WHERE revision_id = ?1 AND file_id = ?2)", &[&revision, &file])?
                .then_some(())
                .ok_or_else(|| not_found("そのエンティティ"))
        }
        "component" => {
            let (file, component) = target.scope_ref.split_once('/').unwrap_or(("", ""));
            let file: i64 = file.parse().map_err(|_| OpError::bad_args("component の ref の形が違う"))?;
            exists(
                "SELECT EXISTS (SELECT 1 FROM scene_changes WHERE revision_id = ?1 AND file_id = ?2 AND component = ?3)",
                &[&revision, &file, &component],
            )?
            .then_some(())
            .ok_or_else(|| not_found("そのコンポーネント"))
        }
        _ => {
            let mut parts = target.scope_ref.splitn(3, '/');
            let (file, component, field) = (parts.next().unwrap_or(""), parts.next().unwrap_or(""), parts.next().unwrap_or(""));
            let file: i64 = file.parse().map_err(|_| OpError::bad_args("field の ref の形が違う"))?;
            let component: Option<&str> = (!component.is_empty()).then_some(component);
            exists(
                "SELECT EXISTS (SELECT 1 FROM scene_changes WHERE revision_id = ?1 AND file_id = ?2 AND field = ?3
                                 AND ((?4 IS NULL AND component IS NULL) OR component = ?4))",
                &[&revision, &file, &field, &component],
            )?
            .then_some(())
            .ok_or_else(|| not_found("そのフィールド"))
        }
    }
}

/// Editor の利用者の判断 (承認・却下・要確認・再開) を記録する。複数の対象を 1 つのイベントとして記録する。
/// 何も実行しない (git・ファイル・Revision の復元状態に触れない)。`reviews` へ行を足すだけで、過去の行は変えない。
pub fn review_set(env: &RecordEnv, _caller: &Caller, mut args: Args) -> Result<Value, OpError> {
    let targets_value = args.value("targets").ok_or_else(|| OpError::bad_args("targets が要る"))?;
    let state = args.choice("state", review::INPUT_STATES)?.ok_or_else(|| OpError::bad_args("state が要る"))?;
    let note = args.string("note")?;
    args.finish()?;
    let Value::Array(items) = targets_value else { return Err(OpError::bad_args("targets は配列でなければならない")) };
    if items.is_empty() {
        return Err(OpError::bad_args("targets は 1 件以上"));
    }
    if items.len() > review::MAX_TARGETS_PER_CALL {
        return Err(OpError::bad_args(format!("targets は {} 件まで", review::MAX_TARGETS_PER_CALL)));
    }
    if note.as_ref().is_some_and(|n| n.chars().count() > review::MAX_NOTE_CHARS) {
        return Err(OpError::bad_args(format!("note は {} 文字まで", review::MAX_NOTE_CHARS)));
    }
    let mut targets: Vec<review::Target> = Vec::with_capacity(items.len());
    for item in &items {
        let Value::Object(map) = item else { return Err(OpError::bad_args("targets の要素はオブジェクトでなければならない")) };
        if let Some(extra) = map.keys().find(|k| !matches!(k.as_str(), "revisionId" | "scope")) {
            return Err(OpError::bad_args(format!("targets の知らない項目: {}", crate::protocol::clip(extra))));
        }
        let revision_id = map
            .get("revisionId")
            .and_then(Value::as_i64)
            .ok_or_else(|| OpError::bad_args("targets[].revisionId は整数でなければならない"))?;
        let (kind, scope_ref) = parse_scope(map.get("scope").ok_or_else(|| OpError::bad_args("targets[].scope が要る"))?)?;
        let target = review::Target { revision_id, kind, scope_ref };
        if !targets.contains(&target) {
            targets.push(target);
        }
    }
    let at = now_ms();
    run_in_writer(env, move |store| {
        let conn = conn_of(store)?;
        let mut checked: Vec<i64> = Vec::new();
        for target in &targets {
            if !checked.contains(&target.revision_id) {
                let actor: Option<String> = conn
                    .query_row("SELECT actor FROM change_revisions WHERE revision_id = ?1", [target.revision_id], |r| r.get(0))
                    .optional()
                    .map_err(db_err)?;
                let Some(actor) = actor else {
                    return Err(OpError::new(code::NOT_FOUND, format!("revisionId {} は無い", target.revision_id)));
                };
                if !review::is_reviewable_actor(&actor) {
                    return Err(OpError::bad_args(format!(
                        "revisionId {} の行為者は {actor} で、レビューの対象ではない",
                        target.revision_id
                    )));
                }
                checked.push(target.revision_id);
            }
            check_target(conn, target)?;
        }
        let payload_targets: Vec<Value> = targets
            .iter()
            .map(|t| json!({ "revision_id": t.revision_id, "kind": t.kind, "ref": t.scope_ref }))
            .collect();
        let payload = json!({ "targets": payload_targets, "state": state, "note": note });
        record_and_apply(store, at, "review_set", payload).map_err(store_err)?;
        let conn = conn_of(store)?;
        let mut states: Vec<Value> = Vec::new();
        for revision_id in &checked {
            let aggregate = review::revision_state(conn, *revision_id).map_err(db_err)?;
            states.push(json!({
                "revisionId": revision_id,
                "review": aggregate.map(|(state, _)| state.as_str()),
            }));
        }
        Ok(json!({ "recorded": targets.len(), "revisions": states }))
    })
}
