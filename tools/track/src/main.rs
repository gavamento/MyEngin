//! mye_track.exe — 追跡サービスの入口。
//!
//!   mye_track.exe --version
//!   mye_track.exe serve --project <root>
//!
//! `serve` は DB を開いて起動時の検査を行い、ファイル監視を続ける常駐になる。
//! 終了の合図は標準入力の EOF か Ctrl+C / Ctrl+Break / コンソールを閉じること。
//! (pipe 経由の終了と、無接続の時間切れは M104f。)

use std::io::Read;
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::mpsc::{channel, Sender};
use std::sync::OnceLock;
use std::time::Duration;

use windows_sys::Win32::System::Console::{SetConsoleCtrlHandler, CTRL_BREAK_EVENT, CTRL_C_EVENT, CTRL_CLOSE_EVENT};

use mye_track::db::now_ms;
use mye_track::project::project_id;
use mye_track::collector::TrackerConfig;
use mye_track::startup::{open_project, ServiceState};
use mye_track::watch::{Phase, Tracker};

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
        _ => {
            eprintln!("usage: mye_track --version | mye_track serve --project <root>");
            ExitCode::from(2)
        }
    }
}

fn serve(args: &[String]) -> ExitCode {
    let mut project: Option<PathBuf> = None;
    let mut iter = args.iter();
    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "--project" => project = iter.next().map(PathBuf::from),
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

    let (store, report) = open_project(&root, now_ms());
    eprintln!("projectId: {}", project_id(&root));
    eprintln!("{report:#?}");
    // 記録の置き場所が無ければ追跡できない。db_degraded はジャーナルだけで動き続ける
    let Some(store) = store else { return ExitCode::from(1) };
    let degraded = !matches!(report.state, Some(ServiceState::Recording));

    let (stop_tx, stop_rx) = channel::<&'static str>();
    let _ = STOP_SIGNAL.set(stop_tx.clone());
    // SAFETY: on_ctrl は引数を読むだけで、静的な Sender へ送る
    unsafe {
        SetConsoleCtrlHandler(Some(on_ctrl), 1);
    }
    // 標準入力の EOF = 起動した側が終わった。読めないとき (閉じたハンドル) は合図にしない
    std::thread::spawn(move || {
        let mut buf = [0u8; 256];
        let mut stdin = std::io::stdin();
        loop {
            match stdin.read(&mut buf) {
                Ok(0) => {
                    let _ = stop_tx.send("stdin_eof");
                    return;
                }
                Ok(_) => {}
                Err(_) => return,
            }
        }
    });

    let tracker = Tracker::start(&root, store, TrackerConfig::default());
    let status = tracker.wait_ready(Duration::from_secs(3600));
    eprintln!("{status:#?}");
    let failed = matches!(status.phase, Phase::Failed(_));
    let reason = stop_rx.recv().unwrap_or("channel_closed");
    tracker.stop(reason);
    if failed || degraded {
        ExitCode::from(1)
    } else {
        ExitCode::SUCCESS
    }
}
