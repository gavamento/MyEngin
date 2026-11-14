//====================================================================================
//                          track_client.rs
//  MyEngin/ 秋田蓮音                                                       11/15/2026
//                                          追跡サービス (mye_track.exe) の pipe クライアント (M104h)
//====================================================================================
//
// 正本: docs\ai-change-tracking\ipc-contract.md。tools\track の crate には依存しない (ADR-032 決定 1)。
// 道具の呼び出しごとに接続する。読み取りは別スレッドで行い、時間切れでも MCP 全体を固めない。

use serde_json::{json, Value};
use std::fs::{File, OpenOptions};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::os::windows::ffi::OsStrExt;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, Instant};

// tools\track\src\protocol.rs の PROTO_VERSION と同じ値 (check_rules 規則 9 が突き合わせる)
pub const PROTO_VERSION: u32 = 1;

// 1 行の上限 (改行を除く。ipc-contract.md §1)
const MAX_LINE_BYTES: usize = 1024 * 1024;
const PROJECT_ID_LEN: usize = 16;
const ENV_TRACK_EXE: &str = "MYE_TRACK_EXE";
const TRACK_EXE_NAME: &str = "mye_track.exe";
// mye_mcp.exe (tools\mcp\target\release) から見た、tools\track\target\release の mye_track.exe
const TRACK_EXE_DEV_RELATIVE: &str = r"..\..\..\track\target\release\mye_track.exe";

const ERROR_FILE_NOT_FOUND: i32 = 2;
const ERROR_ACCESS_DENIED: i32 = 5;
const ERROR_PIPE_BUSY: i32 = 231;
const CREATE_BREAKAWAY_FROM_JOB: u32 = 0x0100_0000;
const DETACHED_PROCESS: u32 = 0x0000_0008;
const POLL_INTERVAL: Duration = Duration::from_millis(50);
// 古い service.json を掴んで bad_token になったあと、新しい値が書かれるのを待つ上限
const TOKEN_REFRESH_WAIT: Duration = Duration::from_millis(1000);

// 時間切れの取り決め (ipc-contract.md §6)
#[derive(Clone, Debug)]
pub struct Settings {
    pub connect: Duration,
    pub hello: Duration,
    pub record: Duration,
    pub read: Duration,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            connect: Duration::from_secs(5),
            hello: Duration::from_secs(5),
            record: Duration::from_secs(5),
            read: Duration::from_secs(30),
        }
    }
}

// サービスのエラー、または接続の失敗。code はサービスのコードか、このファイルが付ける
// service_unavailable / timeout / protocol / too_large
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TrackError {
    pub code: String,
    pub detail: String,
}

impl TrackError {
    fn new(code: &str, detail: impl Into<String>) -> Self {
        TrackError { code: code.to_string(), detail: detail.into() }
    }
}

// 記録を伴う op は AI の編集を待たせないよう短く、取得の op は長く待つ
fn op_timeout(settings: &Settings, op: &str) -> Duration {
    match op {
        "session_begin" | "session_end" | "op_begin" | "op_end" | "report" => settings.record,
        _ => settings.read,
    }
}

// 1 つのプロジェクトのサービスへ 1 件の要求を送る。サービスが居なければ起動する。
pub fn call(project: &Path, op: &str, args: Value) -> Result<Value, TrackError> {
    call_with(&Settings::default(), project, op, args)
}

fn call_with(settings: &Settings, project: &Path, op: &str, args: Value) -> Result<Value, TrackError> {
    let root = std::path::absolute(project).unwrap_or_else(|_| project.to_path_buf());
    let id = project_id(&root);
    let pipe = format!(r"\\.\pipe\mye-track-{id}");
    let deadline = Instant::now() + settings.connect;
    let mut retried_token: Option<String> = None;
    loop {
        let file = connect(&pipe, &root, deadline)?;
        let token = read_token(&root, deadline)?;
        match exchange(file, &id, &root, &token, op, &args, settings) {
            // サービスの再起動で変わったトークンを読み直し、1 回だけやり直す
            Err(e) if e.code == "bad_token" && retried_token.is_none() => {
                retried_token = Some(token.clone());
                wait_token_change(&root, &token);
            }
            other => return other,
        }
    }
}

// hello と op を 1 本の接続で行う。呼び出しの後、接続は捨てる。
fn exchange(
    file: File,
    expected_id: &str,
    root: &Path,
    token: &str,
    op: &str,
    args: &Value,
    settings: &Settings,
) -> Result<Value, TrackError> {
    let mut conn = Connection::new(file);
    let hello = json!({
        "proto": PROTO_VERSION,
        "role": "mcp",
        "token": token,
        "projectRoot": root.to_string_lossy(),
        "pid": std::process::id(),
    });
    let reply = conn.request("hello", hello, settings.hello)?;
    // 自分の計算と違うサービスへ繋がっていたら使わない (別名のパスで起動された古いサービスなど)
    if reply.get("projectId").and_then(Value::as_str) != Some(expected_id) {
        return Err(TrackError::new("wrong_project", "the service reported a different projectId than this MCP server computed"));
    }
    conn.request(op, args.clone(), op_timeout(settings, op))
}

// 要求を送って応答を受ける接続。読み書きは作業スレッドに任せ、呼び出し側は時間切れで待つのをやめられる。
// 時間切れの接続は作業スレッドが読み取りで止まったまま残る (プロセスの終了で消える)。次の呼び出しは別の接続を張る。
struct Connection {
    jobs: mpsc::Sender<String>,
    results: mpsc::Receiver<io::Result<String>>,
    next_id: u64,
}

impl Connection {
    fn new(file: File) -> Connection {
        let (jobs, job_rx) = mpsc::channel::<String>();
        let (result_tx, results) = mpsc::channel();
        std::thread::spawn(move || worker(file, job_rx, result_tx));
        Connection { jobs, results, next_id: 1 }
    }

    fn request(&mut self, op: &str, args: Value, timeout: Duration) -> Result<Value, TrackError> {
        let id = self.next_id;
        self.next_id += 1;
        let line = json!({"id": id, "op": op, "args": args}).to_string();
        if line.len() > MAX_LINE_BYTES {
            return Err(TrackError::new("too_large", format!("the request line exceeds {MAX_LINE_BYTES} bytes")));
        }
        self.jobs.send(line).map_err(|_| TrackError::new("service_unavailable", "the connection to the tracking service is closed"))?;
        let text = match self.results.recv_timeout(timeout) {
            Ok(Ok(text)) => text,
            Ok(Err(e)) if e.kind() == io::ErrorKind::InvalidData => return Err(TrackError::new("too_large", e.to_string())),
            Ok(Err(e)) => return Err(TrackError::new("service_unavailable", format!("the tracking service closed the connection ({op}): {e}"))),
            Err(mpsc::RecvTimeoutError::Timeout) => {
                return Err(TrackError::new("timeout", format!("no response to {op} within {} ms", timeout.as_millis())))
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                return Err(TrackError::new("service_unavailable", "the connection to the tracking service ended"))
            }
        };
        let value: Value = serde_json::from_str(&text).map_err(|e| TrackError::new("protocol", format!("unreadable response: {e}")))?;
        if value.get("id").and_then(Value::as_u64) != Some(id) {
            return Err(TrackError::new("protocol", "the response id does not match the request"));
        }
        if value.get("ok").and_then(Value::as_bool) == Some(true) {
            return Ok(value.get("result").cloned().unwrap_or(Value::Null));
        }
        let error = value.get("error");
        let code = error.and_then(|e| e.get("code")).and_then(Value::as_str).unwrap_or("protocol");
        let detail = error.and_then(|e| e.get("detail")).and_then(Value::as_str).unwrap_or("");
        Err(TrackError::new(code, detail))
    }
}

fn worker(file: File, jobs: mpsc::Receiver<String>, results: mpsc::Sender<io::Result<String>>) {
    let mut reader = BufReader::new(file);
    while let Ok(line) = jobs.recv() {
        let mut text = line.into_bytes();
        text.push(b'\n');
        let mut writer: &File = reader.get_ref();
        let outcome = writer.write_all(&text).and_then(|()| writer.flush()).and_then(|()| read_line_limited(&mut reader));
        let failed = outcome.is_err();
        if results.send(outcome).is_err() || failed {
            return;
        }
    }
}

// 改行までを読む。1 MiB を超える行は InvalidData (続きは読まない)
fn read_line_limited(reader: &mut BufReader<File>) -> io::Result<String> {
    let mut buf = Vec::new();
    let n = reader.by_ref().take(MAX_LINE_BYTES as u64 + 2).read_until(b'\n', &mut buf)?;
    if n == 0 {
        return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "end of stream"));
    }
    if buf.last() == Some(&b'\n') {
        buf.pop();
    }
    if buf.last() == Some(&b'\r') {
        buf.pop();
    }
    if buf.len() > MAX_LINE_BYTES {
        return Err(io::Error::new(io::ErrorKind::InvalidData, format!("the response line exceeds {MAX_LINE_BYTES} bytes")));
    }
    String::from_utf8(buf).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
}

// pipe を開く。ERROR_PIPE_BUSY は待って開き直し、pipe が無ければサービスを 1 回だけ起動して待つ
fn connect(pipe: &str, root: &Path, deadline: Instant) -> Result<File, TrackError> {
    let mut started = false;
    loop {
        match OpenOptions::new().read(true).write(true).open(pipe) {
            Ok(file) => return Ok(file),
            Err(e) => match e.raw_os_error() {
                Some(ERROR_PIPE_BUSY) => {}
                Some(ERROR_FILE_NOT_FOUND) => {
                    if !started {
                        start_service(root)?;
                        started = true;
                    }
                }
                _ => return Err(TrackError::new("service_unavailable", format!("cannot open {pipe}: {e}"))),
            },
        }
        if Instant::now() >= deadline {
            return Err(TrackError::new(
                "service_unavailable",
                "the tracking service did not accept a connection in time (tracking continues without the MCP tools)",
            ));
        }
        std::thread::sleep(POLL_INTERVAL);
    }
}

// <project>\.mye\ai_changes\service.json のトークン。サービスは pipe を張った後に書くので、少し待つ
fn read_token(root: &Path, deadline: Instant) -> Result<String, TrackError> {
    loop {
        if let Some(token) = token_now(root) {
            return Ok(token);
        }
        if Instant::now() >= deadline {
            return Err(TrackError::new("service_unavailable", "service.json was not found; the tracking service is not ready"));
        }
        std::thread::sleep(POLL_INTERVAL);
    }
}

fn token_now(root: &Path) -> Option<String> {
    let path = root.join(".mye").join("ai_changes").join("service.json");
    let bytes = std::fs::read(path).ok()?;
    let doc: Value = serde_json::from_slice(&bytes).ok()?;
    doc.get("token").and_then(Value::as_str).map(str::to_string)
}

fn wait_token_change(root: &Path, old: &str) {
    let until = Instant::now() + TOKEN_REFRESH_WAIT;
    while Instant::now() < until && token_now(root).as_deref() == Some(old) {
        std::thread::sleep(POLL_INTERVAL);
    }
}

// mye_track.exe の場所。MYE_TRACK_EXE があればそれだけを使う (無ければ他を探さず失敗する)
fn find_track_exe() -> Result<PathBuf, TrackError> {
    if let Some(value) = std::env::var_os(ENV_TRACK_EXE).filter(|v| !v.is_empty()) {
        let path = PathBuf::from(value);
        return if path.is_file() {
            Ok(path)
        } else {
            Err(TrackError::new("service_unavailable", format!("{ENV_TRACK_EXE} points to a missing file: {}", path.display())))
        };
    }
    let exe_dir = std::env::current_exe().ok().and_then(|exe| exe.parent().map(Path::to_path_buf));
    if let Some(dir) = exe_dir {
        for candidate in [dir.join(TRACK_EXE_NAME), dir.join(TRACK_EXE_DEV_RELATIVE)] {
            if candidate.is_file() {
                return Ok(candidate);
            }
        }
    }
    Err(TrackError::new(
        "service_unavailable",
        format!("{TRACK_EXE_NAME} was not found (set {ENV_TRACK_EXE}, or run tools\\build_track.bat)"),
    ))
}

// 標準入出力のハンドルを継承不可にしておく間だけ生きる。Rust の Command は bInheritHandles = TRUE で起動するので、
// 子の標準入出力を null にしても、親の継承可能なハンドルは子に複製される。サービスがそれを掴むと、
// クライアントから MCP の stdout が閉じて見えない (EOF が来ない)。
struct StdHandlesHidden(Vec<(*mut core::ffi::c_void, u32)>);

impl StdHandlesHidden {
    fn new() -> Self {
        let mut saved = Vec::new();
        for which in STD_HANDLE_IDS {
            // SAFETY: GetStdHandle は値を返すだけ。GetHandleInformation / SetHandleInformation は
            // 有効なハンドルにだけ呼ぶ (INVALID_HANDLE_VALUE と null は除く)
            unsafe {
                let handle = GetStdHandle(which);
                if handle.is_null() || handle as isize == -1 {
                    continue;
                }
                let mut flags = 0u32;
                if GetHandleInformation(handle, &mut flags) != 0 && flags & HANDLE_FLAG_INHERIT != 0 {
                    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
                    saved.push((handle, flags));
                }
            }
        }
        StdHandlesHidden(saved)
    }
}

impl Drop for StdHandlesHidden {
    fn drop(&mut self) {
        for &(handle, flags) in &self.0 {
            // SAFETY: new() で継承可能だった有効なハンドルを元に戻すだけ
            unsafe {
                SetHandleInformation(handle, HANDLE_FLAG_INHERIT, flags & HANDLE_FLAG_INHERIT);
            }
        }
    }
}

extern "system" {
    fn GetStdHandle(which: u32) -> *mut core::ffi::c_void;
    fn GetHandleInformation(handle: *mut core::ffi::c_void, flags: *mut u32) -> i32;
    fn SetHandleInformation(handle: *mut core::ffi::c_void, mask: u32, flags: u32) -> i32;
}

const HANDLE_FLAG_INHERIT: u32 = 1;
// STD_INPUT_HANDLE / STD_OUTPUT_HANDLE / STD_ERROR_HANDLE (-10 / -11 / -12)
const STD_HANDLE_IDS: [u32; 3] = [-10i32 as u32, -11i32 as u32, -12i32 as u32];

// サービスを切り離して起動する。標準入出力は子へ渡さない (MCP の stdio を子が掴むと、クライアントから接続が閉じて見えない)。
// 子は待たない。終了コード 3 (既に居る) は見えなくてよい (接続のやり直しで分かる)。
fn start_service(root: &Path) -> Result<(), TrackError> {
    let exe = find_track_exe()?;
    let _hidden = StdHandlesHidden::new();
    let spawn = |flags: u32| {
        Command::new(&exe)
            .arg("serve")
            .arg("--project")
            .arg(root)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .creation_flags(flags)
            .spawn()
    };
    // クライアントの Job が BREAKAWAY を許さないと、起動が ERROR_ACCESS_DENIED になる。外してやり直す
    let started = match spawn(CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS) {
        Err(e) if e.raw_os_error() == Some(ERROR_ACCESS_DENIED) => spawn(DETACHED_PROCESS),
        other => other,
    };
    match started {
        Ok(child) => {
            drop(child);
            Ok(())
        }
        Err(e) => Err(TrackError::new("service_unavailable", format!("cannot start {}: {e}", exe.display()))),
    }
}

// projectId (spec D25): 正規化 -> 末尾の区切りを落とす (ドライブのルートは除く) -> UTF-8 の SHA-256 の先頭 16 桁。
// 正規化は tools\track\src\project.rs と同じ: 絶対化、'/' を '\'、ASCII の大文字だけ小文字に
pub fn project_id(root: &Path) -> String {
    let absolute = std::path::absolute(root).unwrap_or_else(|_| root.to_path_buf());
    let units: Vec<u16> = absolute
        .as_os_str()
        .encode_wide()
        .map(|c| match c {
            0x2F => 0x5C,
            0x41..=0x5A => c + 0x20,
            _ => c,
        })
        .collect();
    let key = String::from_utf16_lossy(&units);
    let is_drive_root = key.len() == 3 && key.ends_with(":\\");
    let trimmed = key.trim_end_matches('\\');
    let source = if is_drive_root || trimmed.is_empty() { key.as_str() } else { trimmed };
    to_hex(&sha256(source.as_bytes()))[..PROJECT_ID_LEN].to_string()
}

fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

const SHA256_K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

// SHA-256 (FIPS 180-4)。依存を増やさないための自前実装
fn sha256(data: &[u8]) -> [u8; 32] {
    let mut state: [u32; 8] =
        [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    let mut padded = data.to_vec();
    padded.push(0x80);
    while padded.len() % 64 != 56 {
        padded.push(0);
    }
    padded.extend_from_slice(&((data.len() as u64) * 8).to_be_bytes());
    for block in padded.chunks_exact(64) {
        let mut w = [0u32; 64];
        for (i, word) in block.chunks_exact(4).enumerate() {
            w[i] = u32::from_be_bytes([word[0], word[1], word[2], word[3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = state;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ (!e & g);
            let t1 = h.wrapping_add(s1).wrapping_add(ch).wrapping_add(SHA256_K[i]).wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        for (slot, add) in state.iter_mut().zip([a, b, c, d, e, f, g, h]) {
            *slot = slot.wrapping_add(add);
        }
    }
    let mut out = [0u8; 32];
    for (i, word) in state.iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&word.to_be_bytes());
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::windows::io::{FromRawHandle, RawHandle};
    use std::sync::atomic::{AtomicU32, Ordering};

    // 試験用の偽サーバを立てるための kernel32 の宣言 (std が既にリンクしている)。製品のコードでは使わない
    extern "system" {
        fn CreateNamedPipeW(
            name: *const u16,
            open_mode: u32,
            pipe_mode: u32,
            max_instances: u32,
            out_buffer: u32,
            in_buffer: u32,
            default_timeout: u32,
            security: *const core::ffi::c_void,
        ) -> RawHandle;
        fn ConnectNamedPipe(handle: RawHandle, overlapped: *mut core::ffi::c_void) -> i32;
    }

    const PIPE_ACCESS_DUPLEX: u32 = 3;
    const INVALID_HANDLE_VALUE: RawHandle = -1isize as RawHandle;

    #[test]
    fn sha256_既知のベクタ() {
        assert_eq!(to_hex(&sha256(b"")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        assert_eq!(to_hex(&sha256(b"abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert_eq!(
            to_hex(&sha256(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        );
        // ブロック境界 (55 / 56 / 63 / 64 / 65 バイト) は長さの書き込み位置が変わる
        assert_eq!(
            to_hex(&sha256(&[b'a'; 1_000_000])),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
        );
        assert_eq!(
            to_hex(&sha256(&[b'a'; 55])),
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"
        );
        assert_eq!(
            to_hex(&sha256(&[b'a'; 56])),
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"
        );
        assert_eq!(
            to_hex(&sha256(&[b'a'; 64])),
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"
        );
    }

    #[test]
    fn project_id_は_track_の固定表と一致する() {
        let table = include_str!("../../track/tests/project_id_cases.tsv");
        let mut rows = 0;
        for line in table.lines().filter(|l| !l.starts_with('#') && !l.trim().is_empty()) {
            let cols: Vec<&str> = line.split('\t').collect();
            assert_eq!(cols.len(), 4, "表の行は 4 列: {line}");
            assert_eq!(project_id(Path::new(cols[0])), cols[3], "projectId: {}", cols[0]);
            rows += 1;
        }
        assert!(rows >= 10, "表が空に近い");
    }

    fn unique_pipe(tag: &str) -> String {
        static COUNTER: AtomicU32 = AtomicU32::new(0);
        format!(r"\\.\pipe\mye-mcp-test-{}-{tag}-{}", std::process::id(), COUNTER.fetch_add(1, Ordering::SeqCst))
    }

    // 1 本だけ受け付ける偽のサーバ。reply が Some なら hello へ答える (id 1 の ok)。None なら読むだけで答えない
    fn fake_server(name: &str, answer_hello: bool, answer_op: bool) -> std::thread::JoinHandle<()> {
        let wide: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
        // SAFETY: 名前は NUL 終端。得たハンドルは File が所有する
        let handle = unsafe { CreateNamedPipeW(wide.as_ptr(), PIPE_ACCESS_DUPLEX, 0, 1, 4096, 4096, 0, core::ptr::null()) };
        assert_ne!(handle, INVALID_HANDLE_VALUE, "pipe を作れない");
        let raw = handle as usize;
        std::thread::spawn(move || {
            let handle = raw as RawHandle;
            // SAFETY: クライアントが繋がるまで待つだけ
            unsafe { ConnectNamedPipe(handle, core::ptr::null_mut()) };
            // SAFETY: ハンドルはこのスレッドだけが持つ
            let file = unsafe { File::from_raw_handle(handle) };
            let mut reader = BufReader::new(&file);
            let mut writer = &file;
            let mut line = String::new();
            if reader.read_line(&mut line).unwrap_or(0) == 0 {
                return;
            }
            if answer_hello {
                let reply = json!({"id": 1, "ok": true, "result": {"proto": 1, "projectId": "0123456789abcdef"}});
                let _ = writeln!(writer, "{reply}");
                line.clear();
                if reader.read_line(&mut line).unwrap_or(0) == 0 {
                    return;
                }
                if answer_op {
                    let reply = json!({"id": 2, "ok": true, "result": {"echo": "done"}});
                    let _ = writeln!(writer, "{reply}");
                    return;
                }
            }
            // 答えない。クライアントが時間切れで諦めた後に閉じる
            std::thread::sleep(Duration::from_secs(3));
        })
    }

    fn short_settings() -> Settings {
        Settings {
            connect: Duration::from_millis(500),
            hello: Duration::from_millis(300),
            record: Duration::from_millis(300),
            read: Duration::from_millis(300),
        }
    }

    #[test]
    fn hello_に答えないサービスは時間切れで失敗し_次の接続は通る() {
        let silent = unique_pipe("silent");
        let _silent_thread = fake_server(&silent, false, false);
        let file = OpenOptions::new().read(true).write(true).open(&silent).unwrap();
        let started = Instant::now();
        let err = exchange(file, "0123456789abcdef", Path::new(r"C:\x"), "t", "list", &json!({}), &short_settings()).unwrap_err();
        assert_eq!(err.code, "timeout");
        assert!(started.elapsed() < Duration::from_secs(2), "時間切れが遅い: {:?}", started.elapsed());

        // 前の時間切れが次の呼び出しを止めない
        let good = unique_pipe("good");
        let good_thread = fake_server(&good, true, true);
        let file = OpenOptions::new().read(true).write(true).open(&good).unwrap();
        let result = exchange(file, "0123456789abcdef", Path::new(r"C:\x"), "t", "list", &json!({}), &short_settings()).unwrap();
        assert_eq!(result["echo"], "done");
        good_thread.join().unwrap();
    }

    #[test]
    fn op_に答えないサービスは時間切れで失敗する() {
        let name = unique_pipe("noop");
        let _thread = fake_server(&name, true, false);
        let file = OpenOptions::new().read(true).write(true).open(&name).unwrap();
        let started = Instant::now();
        let err = exchange(file, "0123456789abcdef", Path::new(r"C:\x"), "t", "report", &json!({}), &short_settings()).unwrap_err();
        assert_eq!(err.code, "timeout");
        assert!(err.detail.contains("report"));
        assert!(started.elapsed() < Duration::from_secs(2));
    }

    #[test]
    fn hello_の_projectid_が違えば_wrong_project() {
        let name = unique_pipe("wrong");
        let thread = fake_server(&name, true, true);
        let file = OpenOptions::new().read(true).write(true).open(&name).unwrap();
        let err = exchange(file, "ffffffffffffffff", Path::new(r"C:\x"), "t", "list", &json!({}), &short_settings()).unwrap_err();
        assert_eq!(err.code, "wrong_project");
        drop(thread);
    }

    #[test]
    fn 記録の_op_だけ短い時間切れを使う() {
        let s = Settings::default();
        assert_eq!(op_timeout(&s, "report"), s.record);
        assert_eq!(op_timeout(&s, "session_begin"), s.record);
        assert_eq!(op_timeout(&s, "list"), s.read);
        assert_eq!(op_timeout(&s, "describe"), s.read);
    }
}
