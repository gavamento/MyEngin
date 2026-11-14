//! mye_track.exe — 追跡サービスの入口。
//!
//!   mye_track.exe --version
//!   mye_track.exe serve --project <root> [--idle-timeout-ms <n>]
//!   mye_track.exe client --project <root> | --pipe <name> [--wait-ms <n>]
//!   mye_track.exe hook <pre|post|session-start|session-end> [--project <root>]   (Claude Code のフック。常に終了コード 0)
//!   mye_track.exe bench ...   (計測用。docs/ai-change-tracking/performance.md)
//!
//! `serve` は pipe を張って常駐する (mye_track::server)。終了の合図は pipe の `shutdown`、
//! 接続 0 のままの時間切れ、Ctrl+C / Ctrl+Break / コンソールを閉じること。標準入力の EOF では終わらない。
//! 終了コード: 0 = 正常、1 = 失敗 (または degraded で終了)、2 = 引数の誤り、3 = 同じプロジェクトのサービスが既に居る。
//! `client` は標準入力の 1 行を要求として pipe へ送り、応答を標準出力へ出す (検証用。終了コード 4 = サービスが居ない)。

use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::mpsc::{channel, Sender};
use std::sync::OnceLock;
use std::time::Duration;

use windows_sys::Win32::System::Console::{SetConsoleCtrlHandler, CTRL_BREAK_EVENT, CTRL_C_EVENT, CTRL_CLOSE_EVENT};

use mye_track::collector::TrackerConfig;
use mye_track::server::{ServeOptions, DEFAULT_IDLE_TIMEOUT};

/// 終了の合図の送り先。Ctrl ハンドラは別スレッドから呼ばれるので静的に持つ。
static STOP_SIGNAL: OnceLock<Sender<&'static str>> = OnceLock::new();

unsafe extern "system" fn on_ctrl(ctrl_type: u32) -> i32 {
    if matches!(ctrl_type, CTRL_C_EVENT | CTRL_BREAK_EVENT | CTRL_CLOSE_EVENT) {
        if let Some(tx) = STOP_SIGNAL.get() {
            let _ = tx.send("signal");
        }
        return 1;
    }
    0
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("--version") => {
            println!("mye_track {}", env!("CARGO_PKG_VERSION"));
            ExitCode::SUCCESS
        }
        Some("serve") => serve(&args[1..]),
        Some("client") => ExitCode::from(mye_track::client::run_cli(&args[1..])),
        Some("hook") => {
            // フックは Claude Code の道具の実行を左右する。終了コードは常に 0 (hook.rs)
            mye_track::hook::run(&args[1..]);
            ExitCode::SUCCESS
        }
        Some("bench") => ExitCode::from(mye_track::bench::run(&args[1..])),
        _ => {
            eprintln!(
                "usage: mye_track --version | mye_track serve --project <root> | mye_track client --project <root> | mye_track hook ... | mye_track bench ..."
            );
            ExitCode::from(2)
        }
    }
}

fn serve(args: &[String]) -> ExitCode {
    let mut project: Option<PathBuf> = None;
    let mut idle_timeout = DEFAULT_IDLE_TIMEOUT;
    let mut iter = args.iter();
    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "--project" => project = iter.next().map(PathBuf::from),
            "--idle-timeout-ms" => match iter.next().and_then(|v| v.parse::<u64>().ok()) {
                Some(ms) => idle_timeout = Duration::from_millis(ms),
                None => {
                    eprintln!("serve: --idle-timeout-ms は整数");
                    return ExitCode::from(2);
                }
            },
            other => {
                eprintln!("serve: 不明な引数 {other}");
                return ExitCode::from(2);
            }
        }
    }
    let Some(root) = project else {
        eprintln!("serve: --project <root> が要る");
        return ExitCode::from(2);
    };

    let (stop_tx, stop_rx) = channel::<&'static str>();
    let _ = STOP_SIGNAL.set(stop_tx.clone());
    // SAFETY: on_ctrl は引数を読むだけで、静的な Sender へ送る
    unsafe {
        SetConsoleCtrlHandler(Some(on_ctrl), 1);
    }
    let options = ServeOptions { idle_timeout, tracker: TrackerConfig::default(), ..ServeOptions::default() };
    ExitCode::from(mye_track::server::run(&root, options, stop_tx, stop_rx))
}
