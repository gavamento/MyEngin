// mye_collab_cli — エディタ抜きで同じサービスを叩く NDJSON ドライバ。
//
// これが存在する理由 (spec §4.0「決定 1 の根拠のうち生き残るもの」):
// Source Control の回帰を D3D もウィンドウも無い場所で取るため。collab_verify.bat は
// 一時リポジトリへこの exe を通して要求を流し、出力を期待 NDJSON と突き合わせる。
//
// 使い方:
//   mye_collab_cli --root <dir>  < scenario.ndjson  > actual.ndjson
// 1 行 = 1 要求 (`{"id":1,"op":"status"}`)。空行と `#` 始まりの行は無視する
// (`#` 行は collab_verify.ps1 側のディレクティブ = ここには届かないのが正常)。
//
// ★1 要求ずつ「応答が返るまで待つ」ことで出力順を決定的にしている。
//   まとめて送ると worker の完了順は同じでも、poll のタイミング次第で
//   通知が要求の間に割り込む位置が揺れる = 期待ファイルが不安定になる。

use std::io::{BufRead, Write};
use std::time::{Duration, Instant};

use mye_collab::worker::Service;

/// 1 要求あたりの上限。ネットワーク op (M66f) でも 30 s で諦める
const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
/// EOF 後に通知を拾うための猶予
const DRAIN_GRACE: Duration = Duration::from_millis(200);
const POLL_SLEEP: Duration = Duration::from_millis(2);

fn main() {
    let mut root = String::from(".");
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--root" => {
                if i + 1 >= args.len() {
                    eprintln!("--root requires a directory");
                    std::process::exit(2);
                }
                root = args[i + 1].clone();
                i += 2;
            }
            other => {
                eprintln!("unknown argument: {other}");
                std::process::exit(2);
            }
        }
    }

    let svc = Service::new(root);
    let stdin = std::io::stdin();
    let stdout = std::io::stdout();
    let mut out = stdout.lock();

    for line in stdin.lock().lines() {
        let line = match line {
            Ok(l) => l,
            Err(e) => {
                eprintln!("stdin read failed: {e}");
                break;
            }
        };
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        let want_id = serde_json::from_str::<serde_json::Value>(trimmed)
            .ok()
            .and_then(|v| v.get("id").and_then(|i| i.as_u64()))
            .unwrap_or(0);
        svc.request(trimmed.to_string());

        let deadline = Instant::now() + REQUEST_TIMEOUT;
        loop {
            match svc.poll() {
                Some(msg) => {
                    let done = response_id(&msg) == Some(want_id);
                    let _ = writeln!(out, "{msg}");
                    let _ = out.flush();
                    if done {
                        break;
                    }
                }
                None => {
                    if Instant::now() >= deadline {
                        let _ = writeln!(out, "{{\"id\":{want_id},\"ok\":false,\"error\":{{\"code\":\"timeout\",\"detail\":\"no response\"}}}}");
                        let _ = out.flush();
                        break;
                    }
                    std::thread::sleep(POLL_SLEEP);
                }
            }
        }
    }

    // 取りこぼした通知を吐いてから終了 (poll が空のまま猶予を使い切ったら抜ける)
    let deadline = Instant::now() + DRAIN_GRACE;
    while Instant::now() < deadline {
        match svc.poll() {
            Some(msg) => {
                let _ = writeln!(out, "{msg}");
            }
            None => std::thread::sleep(POLL_SLEEP),
        }
    }
    let _ = out.flush();
}

/// 応答行なら id を返す (通知行は None)
fn response_id(line: &str) -> Option<u64> {
    serde_json::from_str::<serde_json::Value>(line)
        .ok()
        .and_then(|v| v.get("id").and_then(|i| i.as_u64()))
}
