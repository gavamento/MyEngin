//! 追跡サービスの常駐 (`mye_track serve`): 単一インスタンスの確保、pipe のサーバ、接続ごとの処理、寿命。
//!
//! 起動の順序: mutex → `service.lock` (別名で来た 2 つ目を DB に触る前に止める) → pipe の最初の 1 本
//! → `service.json` → DB を開く → Tracker。終了の合図は、pipe の `shutdown`、接続 0 のままの時間切れ、
//! Ctrl+C / コンソールを閉じる、のどれか (標準入力の EOF では終わらない)。
//! 設計: docs/ai-change-tracking/architecture.md §3、ipc-contract.md。

use std::fs::{File, OpenOptions};
use std::io::{self, BufRead, BufReader, Write};
use std::os::windows::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use rusqlite::Connection;
use serde_json::{json, Value};
use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, ERROR_ALREADY_EXISTS, HANDLE};
use windows_sys::Win32::Security::Cryptography::{BCryptGenRandom, BCRYPT_USE_SYSTEM_PREFERRED_RNG};
use windows_sys::Win32::System::Threading::CreateMutexW;

use crate::collector::TrackerConfig;
use crate::db::now_ms;
use crate::ops;
use crate::pipe::{self, PipeInstance, PipeListener};
use crate::process::process_start_ms;
use crate::project::{project_id, project_id_source};
use crate::protocol::{
    clip, code, error_response, ok_response, parse_request, role_allows, Args, OpError, Request, Role, IMPLEMENTED_OPS,
    MAX_LINE_BYTES, PROTO_VERSION,
};
use crate::startup::{open_project, ServiceState, StartupReport};
use crate::store::Paths;
use crate::watch::{Phase, StatusReader, Tracker, TrackerStatus};
use crate::writer::{Writer, WriterError};

/// 同じプロジェクトのサービスが既に居る (mutex、`service.lock`、pipe 名のどれかで分かった)。
pub const EXIT_ALREADY_RUNNING: u8 = 3;

/// 接続が 0 になってからサービスが自分で終わるまでの既定。
pub const DEFAULT_IDLE_TIMEOUT: Duration = Duration::from_secs(10 * 60);

/// Writer のキューがこれを超えたら、記録を伴う要求は `busy` で断る。
pub const WRITER_QUEUE_LIMIT: usize = 1024;

/// `ERROR_SHARING_VIOLATION`。`service.lock` を別のプロセスが持っている。
const SHARING_VIOLATION: i32 = 32;

pub struct ServeOptions {
    pub idle_timeout: Duration,
    pub tracker: TrackerConfig,
}

impl Default for ServeOptions {
    fn default() -> Self {
        ServeOptions { idle_timeout: DEFAULT_IDLE_TIMEOUT, tracker: TrackerConfig::default() }
    }
}

/// `CloseHandle` を呼ぶだけの持ち主 (mutex 用)。
struct HandleGuard(HANDLE);

impl Drop for HandleGuard {
    fn drop(&mut self) {
        // SAFETY: CreateMutexW が返した、所有権のあるハンドルを 1 回だけ閉じる
        unsafe {
            CloseHandle(self.0);
        }
    }
}

/// 名前付き mutex を取る。既に在れば `Ok(None)`。
fn acquire_mutex(name: &str) -> io::Result<Option<HandleGuard>> {
    let wide: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
    // SAFETY: wide は NUL 終端。GetLastError は直後に読む
    unsafe {
        let handle = CreateMutexW(std::ptr::null(), 0, wide.as_ptr());
        if handle.is_null() {
            return Err(io::Error::last_os_error());
        }
        let guard = HandleGuard(handle);
        Ok((GetLastError() != ERROR_ALREADY_EXISTS).then_some(guard))
    }
}

/// OS の乱数源から 16 バイトを取り、16 進にする (sim の決定性とは無関係。`rand` crate は足さない)。
fn random_token() -> io::Result<String> {
    let mut bytes = [0u8; 16];
    // SAFETY: bytes は 16 バイトの出力先
    let status = unsafe {
        BCryptGenRandom(std::ptr::null_mut(), bytes.as_mut_ptr(), bytes.len() as u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)
    };
    if status != 0 {
        return Err(io::Error::other(format!("BCryptGenRandom が失敗: 0x{status:08x}")));
    }
    Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
}

/// `service.json` (PID、開始時刻、pipe 名、起動ごとのトークン)。一時ファイルへ書いてから置き換える。
fn write_service_json(path: &Path, ctx: &Context) -> io::Result<()> {
    let body = json!({
        "protoVersion": PROTO_VERSION,
        "pid": std::process::id(),
        "startedAt": ctx.started_at_ms,
        "pipe": ctx.pipe_name,
        "token": ctx.token,
        "projectId": ctx.project_id,
        "projectRoot": ctx.root_text,
    });
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, serde_json::to_vec_pretty(&body).map_err(io::Error::other)?)?;
    std::fs::rename(&tmp, path)
}

/// 2 つ目が起動を断られたとき、先に居るサービスの projectRoot を `service.json` から読んで知らせる。
fn describe_existing_service(dir: &Path) -> String {
    std::fs::read(dir.join("service.json"))
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        .and_then(|v| v["projectRoot"].as_str().map(str::to_string))
        .unwrap_or_else(|| "(service.json を読めない)".to_string())
}

/// 接続のスレッドが共有する、サービスの状態。
struct Context {
    project_id: String,
    root_text: String,
    pipe_name: String,
    token: String,
    started_at_ms: i64,
    started: Instant,
    db_path: PathBuf,
    /// DB へ書ける状態か (`false` = `db_degraded`。取得の op は `db_degraded` で断る)。
    db_recording: bool,
    db_summary: Value,
    status: StatusReader,
    writer: Arc<Writer>,
    stop_tx: Sender<&'static str>,
    connections: AtomicUsize,
    idle_since: Mutex<Instant>,
    stopping: AtomicBool,
}

impl Context {
    /// 接続が 0 のまま続いている時間 (接続があれば 0)。
    fn idle_for(&self) -> Duration {
        if self.connections.load(Ordering::SeqCst) > 0 {
            return Duration::ZERO;
        }
        self.idle_since.lock().unwrap().elapsed()
    }
}

/// 接続 1 本の間だけ接続数を数える。
struct ConnectionGuard(Arc<Context>);

impl ConnectionGuard {
    fn enter(ctx: &Arc<Context>) -> ConnectionGuard {
        ctx.connections.fetch_add(1, Ordering::SeqCst);
        ConnectionGuard(Arc::clone(ctx))
    }
}

impl Drop for ConnectionGuard {
    fn drop(&mut self) {
        if self.0.connections.fetch_sub(1, Ordering::SeqCst) == 1 {
            *self.0.idle_since.lock().unwrap() = Instant::now();
        }
    }
}

/// `StartupReport` を `status` の `db` 欄の形にする。
fn summarize_startup(report: &StartupReport) -> Value {
    let (state, reason) = match &report.state {
        Some(ServiceState::Recording) => ("recording", None),
        Some(ServiceState::DbDegraded(r)) => ("db_degraded", Some(r.clone())),
        Some(ServiceState::Unavailable(r)) => ("unavailable", Some(r.clone())),
        None => ("unknown", None),
    };
    let failed: Vec<Value> =
        report.failed_steps.iter().map(|f| json!({ "step": f.step, "reason": f.reason })).collect();
    json!({
        "state": state,
        "reason": reason,
        "failedSteps": failed,
        "brokenDb": report.broken_db.as_ref().map(|p| p.to_string_lossy().into_owned()),
        "replayedEvents": report.replayed_events,
        "unreadableJournalLines": report.unreadable_journal_lines,
        "blobsMarkedMissing": report.blobs_marked_missing,
        "blobsCorrupted": report.blobs_corrupted,
        "orphanBlobs": report.orphan_blobs,
        "tempBlobsRemoved": report.temp_blobs_removed,
        "sessionsInterrupted": report.sessions_interrupted,
        "operationsAbandoned": report.operations_abandoned,
    })
}

fn phase_name(phase: &Phase) -> (&'static str, Option<String>) {
    match phase {
        Phase::Starting => ("starting", None),
        Phase::Baselining => ("baselining", None),
        Phase::Watching => ("watching", None),
        Phase::Failed(reason) => ("failed", Some(reason.clone())),
        Phase::Stopped => ("stopped", None),
    }
}

/// 取得の結果に付ける「いまの記録の網羅」。ベースライン中は一覧が不完全だと分かる。
fn coverage_json(status: &TrackerStatus) -> Value {
    let (phase, _) = phase_name(&status.phase);
    json!({
        "phase": phase,
        "baselining": matches!(status.phase, Phase::Starting | Phase::Baselining),
        "baselineScanned": status.scan_done,
        "baselineTotal": status.scan_total,
        "unrecorded": status.unrecorded_since_ms.is_some(),
    })
}

/// 接続の中の状態。
struct ConnState {
    role: Option<Role>,
    reader: Option<Connection>,
}

/// 1 行の読み取り結果。
enum Line {
    Data(Vec<u8>),
    TooLarge,
    Eof,
}

/// LF までを 1 行として読む。上限を超える行は中身を捨てて LF まで進み (`TooLarge`)、次の行から読み直せるようにする。
fn read_line<R: BufRead>(reader: &mut R, max: usize) -> io::Result<Line> {
    let mut line: Vec<u8> = Vec::new();
    let mut too_large = false;
    loop {
        let available = reader.fill_buf()?;
        if available.is_empty() {
            // 末尾に LF が無い最後の行も 1 行として扱う
            return Ok(if too_large {
                Line::TooLarge
            } else if line.is_empty() {
                Line::Eof
            } else {
                Line::Data(line)
            });
        }
        let newline = available.iter().position(|&b| b == b'\n');
        let take = newline.unwrap_or(available.len());
        if !too_large {
            if line.len() + take > max {
                too_large = true;
                line = Vec::new();
            } else {
                line.extend_from_slice(&available[..take]);
            }
        }
        reader.consume(take + usize::from(newline.is_some()));
        if newline.is_some() {
            if too_large {
                return Ok(Line::TooLarge);
            }
            if line.last() == Some(&b'\r') {
                line.pop();
            }
            return Ok(Line::Data(line));
        }
    }
}

fn send(mut out: &File, value: &Value) -> io::Result<()> {
    let mut text = serde_json::to_vec(value).map_err(io::Error::other)?;
    text.push(b'\n');
    out.write_all(&text)?;
    out.flush()
}

/// 要求の処理結果。`close` が真なら、応答を返したあと接続を切る。`stop` が真なら、応答を返したあとサービスを止める。
struct Outcome {
    response: Value,
    close: bool,
    stop: bool,
}

impl Outcome {
    fn ok(id: u64, result: Value) -> Outcome {
        Outcome { response: ok_response(id, result), close: false, stop: false }
    }

    fn err(id: u64, e: &OpError) -> Outcome {
        Outcome { response: error_response(Some(id), e), close: false, stop: false }
    }

    fn err_and_close(id: u64, e: &OpError) -> Outcome {
        Outcome { response: error_response(Some(id), e), close: true, stop: false }
    }
}

fn serve_connection(ctx: &Context, instance: PipeInstance) {
    let client_pid = instance.client_pid();
    let file = instance.file();
    let mut reader = BufReader::with_capacity(64 * 1024, file);
    let mut conn = ConnState { role: None, reader: None };
    loop {
        let line = match read_line(&mut reader, MAX_LINE_BYTES) {
            Ok(Line::Data(line)) => line,
            Ok(Line::TooLarge) => {
                let e = OpError::new(code::TOO_LARGE, format!("1 行は {MAX_LINE_BYTES} バイトまで"));
                if send(file, &error_response(None, &e)).is_err() {
                    return;
                }
                continue;
            }
            Ok(Line::Eof) | Err(_) => return,
        };
        if line.iter().all(u8::is_ascii_whitespace) {
            continue;
        }
        let outcome = match parse_request(&line) {
            Err((id, e)) => Outcome { response: error_response(id, &e), close: false, stop: false },
            Ok(request) => handle_request(ctx, &mut conn, &request, client_pid),
        };
        let sent = send(file, &outcome.response);
        // 応答を書き終えてから止める (止める側が先に進んで、応答を切らないように)
        if outcome.stop {
            let _ = ctx.stop_tx.send("shutdown_op");
        }
        if sent.is_err() || outcome.close {
            return;
        }
    }
}

fn handle_request(ctx: &Context, conn: &mut ConnState, request: &Request, client_pid: Option<u32>) -> Outcome {
    let id = request.id;
    let Some(role) = conn.role else {
        if request.op != "hello" {
            return Outcome::err_and_close(id, &OpError::new(code::NEED_HELLO, "最初の要求は hello"));
        }
        return match hello(ctx, request, client_pid) {
            Ok((role, result)) => {
                conn.role = Some(role);
                Outcome::ok(id, result)
            }
            Err(e) => Outcome::err_and_close(id, &e),
        };
    };
    if request.op == "hello" {
        return Outcome::err(id, &OpError::bad_args("hello は接続ごとに 1 回だけ"));
    }
    if !IMPLEMENTED_OPS.contains(&request.op.as_str()) {
        // 巨大な op 名をそのまま返さない (応答も 1 行の上限に収める)
        return Outcome::err(id, &OpError::new(code::UNKNOWN_OP, format!("知らない op: {}", clip(&request.op))));
    }
    if !role_allows(role, &request.op) {
        let detail = format!("役割 {} は {} を呼べない", role.as_str(), request.op);
        return Outcome::err(id, &OpError::new(code::FORBIDDEN, detail));
    }
    let args = Args::new(request.args.clone());
    let result = match request.op.as_str() {
        "status" => args.finish().map(|()| status_result(ctx, conn)),
        "list" => with_reader(ctx, conn, |db| ops::list(db, args)).map(|mut v| {
            v["coverage"] = coverage_json(&ctx.status.status());
            v
        }),
        "describe" => with_reader(ctx, conn, |db| ops::describe(db, args)),
        "sessions" => with_reader(ctx, conn, |db| ops::sessions(db, args)),
        "shutdown" => args.finish().map(|()| json!({})),
        other => Err(OpError::new(code::INTERNAL, format!("op の配線が無い: {other}"))),
    };
    match result {
        Ok(value) => {
            let shutdown = request.op == "shutdown";
            Outcome { response: ok_response(id, value), close: shutdown, stop: shutdown }
        }
        Err(e) => Outcome::err(id, &e),
    }
}

/// 読み取り専用の接続 (接続ごとに 1 本、最初に必要になったとき開く) で DB を引く。
fn with_reader(
    ctx: &Context,
    conn: &mut ConnState,
    query: impl FnOnce(&Connection) -> Result<Value, OpError>,
) -> Result<Value, OpError> {
    if !ctx.db_recording {
        return Err(OpError::new(code::DB_DEGRADED, "DB へ書けない状態 (status の db を見る)"));
    }
    if conn.reader.is_none() {
        conn.reader = Some(
            ops::open_reader(&ctx.db_path)
                .map_err(|e| OpError::new(code::DB_DEGRADED, format!("DB を読み取りで開けない: {e}")))?,
        );
    }
    query(conn.reader.as_ref().expect("opened above"))
}

/// `hello` の検査: 形 → 版 → 役割 → トークン → プロジェクト。通ったら接続元を証跡に残す。
fn hello(ctx: &Context, request: &Request, client_pid: Option<u32>) -> Result<(Role, Value), OpError> {
    let mut args = Args::new(request.args.clone());
    let proto = args.required_int("proto")?;
    let role_text = args.required_string("role")?;
    let token = args.required_string("token")?;
    let project_root = args.required_string("projectRoot")?;
    let claimed_pid = args.int("pid")?;
    args.finish()?;
    if proto != i64::from(PROTO_VERSION) {
        return Err(OpError::new(
            code::PROTO_MISMATCH,
            format!("サービスの版は {PROTO_VERSION}、クライアントは {proto}"),
        ));
    }
    let role = Role::parse(&role_text).ok_or_else(|| OpError::bad_args("role は mcp / editor / hook のどれか"))?;
    if token != ctx.token {
        return Err(OpError::new(code::BAD_TOKEN, "トークンが違う (service.json を読み直す)"));
    }
    // クライアントが言ったパスで DB を開き直さない。projectId が合うかだけを見る
    if project_id(Path::new(&project_root)) != ctx.project_id {
        return Err(OpError::new(code::WRONG_PROJECT, "別のプロジェクトのサービスに繋がっている"));
    }
    // 申告した pid ではなく、pipe から得た値を採る。開始時刻と組で証跡に残す
    let pid = client_pid.or_else(|| claimed_pid.and_then(|p| u32::try_from(p).ok()));
    let evidence = json!({
        "role": role.as_str(),
        "pid": pid,
        "claimedPid": claimed_pid,
        "processStartedAt": pid.and_then(process_start_ms),
    });
    match ctx.writer.submit_bounded(WRITER_QUEUE_LIMIT, move |store| {
        if let Err(e) = store.record_event(now_ms(), "client_hello", evidence) {
            eprintln!("[track] client_hello を残せない: {e}");
        }
    }) {
        Ok(_) => {}
        Err(WriterError::Busy) => return Err(OpError::new(code::BUSY, "Writer のキューが詰まっている")),
        Err(e) => return Err(OpError::new(code::INTERNAL, e.to_string())),
    }
    let status = ctx.status.status();
    Ok((
        role,
        json!({
            "proto": PROTO_VERSION,
            "projectId": ctx.project_id,
            "role": role.as_str(),
            "serviceStartedAt": ctx.started_at_ms,
            "connectionPid": pid,
            "coverage": coverage_json(&status),
        }),
    ))
}

fn status_result(ctx: &Context, conn: &mut ConnState) -> Value {
    let s = ctx.status.status();
    let (phase, failure) = phase_name(&s.phase);
    let mut result = json!({
        "proto": PROTO_VERSION,
        "projectId": ctx.project_id,
        "pid": std::process::id(),
        "startedAt": ctx.started_at_ms,
        "uptimeMs": ctx.started.elapsed().as_millis() as u64,
        "phase": phase,
        "failure": failure,
        "trackedFiles": s.tracked_files,
        "baseline": {
            "firstRun": s.first_run,
            "files": s.baseline_files,
            "bytes": s.baseline_bytes,
            "elapsedMs": s.baseline_ms,
            "scanDone": s.scan_done,
            "scanTotal": s.scan_total,
            "walkMs": s.baseline_timings.walk_ms,
            "readMs": s.baseline_timings.read_ms,
            "hashMs": s.baseline_timings.hash_ms,
            "commitMs": s.baseline_timings.commit_ms,
        },
        "verify": { "runs": s.verify_runs, "gaps": s.verify_gaps, "lastScanMs": s.verify_scan_ms },
        "recent": { "recordUs": s.last_record_us },
        "overflows": s.overflows,
        "unrecorded": s.unrecorded_since_ms.map(|since| json!({ "sinceMs": since, "reason": s.unrecorded_reason })),
        "db": ctx.db_summary,
        "blobs": Value::Null,
        "gaps": Value::Null,
    });
    // DB から引く値は、読めなければ null のまま (status 自体は答える)
    if let Ok(db_part) = with_reader(ctx, conn, ops::status_from_db) {
        result["blobs"] = db_part["blobs"].clone();
        result["gaps"] = db_part["gaps"].clone();
    }
    result
}

/// 受付ループで 1 本の接続を待つ間に、次の pipe を作る。失敗は短く待って繰り返す (止める合図まで)。
fn create_next_instance(ctx: &Context, listener: &mut PipeListener) -> Option<PipeInstance> {
    loop {
        match listener.create_instance() {
            Ok(instance) => return Some(instance),
            Err(e) => {
                eprintln!("[track] pipe を作れない: {e}");
                if ctx.stopping.load(Ordering::SeqCst) {
                    return None;
                }
                std::thread::sleep(Duration::from_millis(100));
            }
        }
    }
}

fn accept_loop(ctx: Arc<Context>, mut listener: PipeListener, first: PipeInstance) {
    let mut instance = first;
    loop {
        let connected = instance.connect();
        if ctx.stopping.load(Ordering::SeqCst) {
            return;
        }
        if let Err(e) = connected {
            eprintln!("[track] pipe の接続の受け付けに失敗: {e}");
            match create_next_instance(&ctx, &mut listener) {
                Some(next) => instance = next,
                None => return,
            }
            continue;
        }
        // 接続を引き渡す前に次のインスタンスを作る (空きが無い隙間を短くする)
        let guard = ConnectionGuard::enter(&ctx);
        let Some(next) = create_next_instance(&ctx, &mut listener) else { return };
        let handler_ctx = Arc::clone(&ctx);
        let served = std::mem::replace(&mut instance, next);
        let spawned = std::thread::Builder::new().name("mye-track-conn".into()).spawn(move || {
            let _guard = guard;
            serve_connection(&handler_ctx, served);
        });
        if let Err(e) = spawned {
            eprintln!("[track] 接続のスレッドを作れない: {e}");
        }
    }
}

/// 受付ループの `ConnectNamedPipe` を起こすため、自分に 1 回繋ぐ。
fn wake_accept_loop(pipe_name: &str) {
    let _ = pipe::open_client(pipe_name);
}

/// サービスを動かし、終了コードを返す。`stop_rx` に届いた理由で終わる (`stop_tx` はその送り口)。
pub fn run(
    root: &Path,
    options: ServeOptions,
    stop_tx: Sender<&'static str>,
    stop_rx: Receiver<&'static str>,
) -> u8 {
    let id = project_id(root);
    let paths = Paths::for_project(root);

    // 二重起動の守り (1): 名前付き mutex。同じ projectId のサービスが居れば即終了
    let _mutex = match acquire_mutex(&format!("Local\\mye-track-{id}")) {
        Ok(Some(guard)) => guard,
        Ok(None) => {
            eprintln!("[track] 同じプロジェクトのサービスが既に動いている (mutex): {}", project_id_source(root));
            return EXIT_ALREADY_RUNNING;
        }
        Err(e) => {
            eprintln!("[track] mutex を作れない: {e}");
            return 1;
        }
    };
    if let Err(e) = std::fs::create_dir_all(&paths.dir) {
        eprintln!("[track] {} を作れない: {e}", paths.dir.display());
        return 1;
    }
    // 二重起動の守り (2): 同じディレクトリを別の名前 (subst、ジャンクション、8.3 名) で指すと projectId が変わり、
    // mutex も pipe の名前も効かない。DB のあるディレクトリの service.lock を共有なしで持ち続ける。DB に触る前に確かめる
    let lock_path = paths.dir.join("service.lock");
    let _lock = match OpenOptions::new().read(true).write(true).create(true).truncate(false).share_mode(0).open(&lock_path)
    {
        Ok(file) => file,
        Err(e) if e.raw_os_error() == Some(SHARING_VIOLATION) => {
            eprintln!(
                "[track] 同じ DB に別のサービスが居る (service.lock)。動いているサービスの projectRoot: {}",
                describe_existing_service(&paths.dir)
            );
            return EXIT_ALREADY_RUNNING;
        }
        Err(e) => {
            eprintln!("[track] service.lock を開けない: {e}");
            return 1;
        }
    };

    let token = match random_token() {
        Ok(token) => token,
        Err(e) => {
            eprintln!("[track] トークンを作れない: {e}");
            return 1;
        }
    };
    // 二重起動の守り (3): pipe の最初の 1 本は同名があると失敗する
    let pipe_name = pipe::pipe_name(&id);
    let mut listener = match PipeListener::new(&pipe_name) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("[track] pipe の準備に失敗: {e}");
            return 1;
        }
    };
    let first = match listener.create_instance() {
        Ok(instance) => instance,
        Err(e) if pipe::is_access_denied(&e) => {
            eprintln!("[track] 同名の pipe が既にある: {pipe_name}");
            return EXIT_ALREADY_RUNNING;
        }
        Err(e) => {
            eprintln!("[track] pipe を作れない: {e}");
            return 1;
        }
    };

    let started_at_ms = now_ms();
    let (store, report) = open_project(root, started_at_ms);
    eprintln!("projectId: {id}");
    eprintln!("{report:#?}");
    // 記録の置き場所が無ければ追跡できない。db_degraded はジャーナルだけで動き続ける
    let Some(store) = store else { return 1 };
    let degraded = !matches!(report.state, Some(ServiceState::Recording));

    let tracker = Tracker::start(root, store, options.tracker);
    let ctx = Arc::new(Context {
        project_id: id,
        root_text: root.to_string_lossy().into_owned(),
        pipe_name: pipe_name.clone(),
        token,
        started_at_ms,
        started: Instant::now(),
        db_path: paths.db.clone(),
        db_recording: !degraded,
        db_summary: summarize_startup(&report),
        status: tracker.status_reader(),
        writer: Arc::clone(tracker.writer()),
        stop_tx,
        connections: AtomicUsize::new(0),
        idle_since: Mutex::new(Instant::now()),
        stopping: AtomicBool::new(false),
    });
    let service_json = paths.dir.join("service.json");
    if let Err(e) = write_service_json(&service_json, &ctx) {
        eprintln!("[track] service.json を書けない: {e}");
        tracker.stop("startup_failed");
        return 1;
    }
    let accept_ctx = Arc::clone(&ctx);
    let accept = std::thread::Builder::new()
        .name("mye-track-accept".into())
        .spawn(move || accept_loop(accept_ctx, listener, first));
    if let Err(e) = accept {
        eprintln!("[track] 受付のスレッドを作れない: {e}");
        tracker.stop("startup_failed");
        let _ = std::fs::remove_file(&service_json);
        return 1;
    }

    let reason = loop {
        match stop_rx.recv_timeout(Duration::from_millis(100)) {
            Ok(reason) => break reason,
            Err(RecvTimeoutError::Timeout) => {
                if ctx.idle_for() >= options.idle_timeout {
                    break "idle_timeout";
                }
            }
            Err(RecvTimeoutError::Disconnected) => break "channel_closed",
        }
    };
    eprintln!("[track] 終了: {reason}");
    ctx.stopping.store(true, Ordering::SeqCst);
    wake_accept_loop(&pipe_name);
    let failed = matches!(ctx.status.status().phase, Phase::Failed(_));
    tracker.stop(reason);
    let _ = std::fs::remove_file(&service_json);
    u8::from(failed || degraded)
}
