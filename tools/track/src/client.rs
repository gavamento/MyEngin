//! pipe のクライアント (ipc-contract.md §1、§6)。`mye_track client` と、試験・検証が使う。
//!
//! `client` サブコマンドは、標準入力の 1 行を要求として pipe へ送り、応答の 1 行を標準出力へ出す。
//! hello は自動では行わない (hello の失敗を検証できるように、行をそのまま流す)。
//! フックの CLI (sub-08) もこの接続の実装を共有する。

use std::fs::File;
use std::io::{self, BufRead, BufReader, Write};
use std::path::Path;
use std::time::{Duration, Instant};

use serde_json::Value;

use crate::pipe::{bytes_available, open_client, pipe_name};
use crate::project::project_id;
use crate::store::Paths;

/// `client` が pipe の出現を待つ既定の時間 (ipc-contract.md §6 の「最大 5 秒」)。
const DEFAULT_WAIT: Duration = Duration::from_secs(5);

/// pipe が見つからない (サービスが居ない)。
pub const EXIT_NO_SERVICE: u8 = 4;

/// 繋がった pipe。
pub struct Client {
    reader: BufReader<File>,
    writer: File,
}

impl Client {
    /// pipe を開く。無ければ `wait` の間 (短い間隔で) 待つ。0 なら 1 回だけ。
    pub fn connect(name: &str, wait: Duration) -> io::Result<Client> {
        let deadline = Instant::now() + wait;
        loop {
            match open_client(name) {
                Ok(file) => {
                    let writer = file.try_clone()?;
                    return Ok(Client { reader: BufReader::new(file), writer });
                }
                Err(e) if e.kind() == io::ErrorKind::NotFound && Instant::now() < deadline => {
                    std::thread::sleep(Duration::from_millis(50));
                }
                Err(e) => return Err(e),
            }
        }
    }

    /// 1 行を送り、応答の 1 行を受ける。切られていれば `None`。
    pub fn exchange(&mut self, line: &str) -> io::Result<Option<String>> {
        let mut text = line.as_bytes().to_vec();
        text.push(b'\n');
        if let Err(e) = self.writer.write_all(&text).and_then(|()| self.writer.flush()) {
            return match e.kind() {
                io::ErrorKind::BrokenPipe | io::ErrorKind::ConnectionReset => Ok(None),
                _ if e.raw_os_error() == Some(232) => Ok(None), // ERROR_NO_DATA: 切断中の pipe への書き込み
                _ => Err(e),
            };
        }
        let mut response = String::new();
        match self.reader.read_line(&mut response) {
            Ok(0) => Ok(None),
            Ok(_) => Ok(Some(response.trim_end_matches(['\n', '\r']).to_string())),
            Err(e) if matches!(e.kind(), io::ErrorKind::BrokenPipe | io::ErrorKind::ConnectionReset) => Ok(None),
            Err(e) => Err(e),
        }
    }

    /// 要求なしに届く 1 行 (通知) を `limit` まで待つ。来なければ `None`。相手が閉じていれば `Err`。
    pub fn wait_line(&mut self, limit: Duration) -> io::Result<Option<Value>> {
        let deadline = Instant::now() + limit;
        loop {
            if !self.reader.buffer().is_empty() || bytes_available(self.reader.get_ref())? > 0 {
                let mut text = String::new();
                if self.reader.read_line(&mut text)? == 0 {
                    return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "サービスが接続を切った"));
                }
                return serde_json::from_str(text.trim_end()).map(Some).map_err(io::Error::other);
            }
            if Instant::now() >= deadline {
                return Ok(None);
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    /// 要求を JSON で組んで送り、応答を JSON で返す。
    pub fn request(&mut self, id: u64, op: &str, args: Value) -> io::Result<Value> {
        let line = serde_json::json!({ "id": id, "op": op, "args": args }).to_string();
        match self.exchange(&line)? {
            Some(text) => serde_json::from_str(&text).map_err(io::Error::other),
            None => Err(io::Error::new(io::ErrorKind::UnexpectedEof, "サービスが接続を切った")),
        }
    }
}

/// `<project>\.mye\ai_changes\service.json` を読む (トークンなどを得る)。
pub fn read_service_json(root: &Path) -> Option<Value> {
    let bytes = std::fs::read(Paths::for_project(root).dir.join("service.json")).ok()?;
    serde_json::from_slice(&bytes).ok()
}

/// `mye_track client --project <root> | --pipe <name> [--wait-ms <n>]`。
pub fn run_cli(args: &[String]) -> u8 {
    let mut name: Option<String> = None;
    let mut wait = DEFAULT_WAIT;
    let mut iter = args.iter();
    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "--project" => name = iter.next().map(|root| pipe_name(&project_id(Path::new(root)))),
            "--pipe" => name = iter.next().cloned(),
            "--wait-ms" => match iter.next().and_then(|v| v.parse::<u64>().ok()) {
                Some(ms) => wait = Duration::from_millis(ms),
                None => {
                    eprintln!("client: --wait-ms は整数");
                    return 2;
                }
            },
            other => {
                eprintln!("client: 不明な引数 {other}");
                return 2;
            }
        }
    }
    let Some(name) = name else {
        eprintln!("client: --project <root> か --pipe <name> が要る");
        return 2;
    };
    let mut client = match Client::connect(&name, wait) {
        Ok(client) => client,
        Err(e) => {
            eprintln!("client: {name} に繋げない: {e}");
            return EXIT_NO_SERVICE;
        }
    };
    let stdout = io::stdout();
    let mut out = stdout.lock();
    for line in io::stdin().lock().lines() {
        let Ok(line) = line else { break };
        if line.trim().is_empty() {
            continue;
        }
        match client.exchange(&line) {
            Ok(Some(response)) => {
                if writeln!(out, "{response}").and_then(|()| out.flush()).is_err() {
                    return 0;
                }
            }
            Ok(None) => {
                let _ = writeln!(out, "{{\"client\":\"closed\"}}");
                return 0;
            }
            Err(e) => {
                eprintln!("client: {e}");
                return 1;
            }
        }
    }
    0
}
