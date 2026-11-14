//! mye_track.exe — 追跡サービスの入口。
//!
//!   mye_track.exe --version
//!   mye_track.exe serve --project <root>
//!
//! 現段階の `serve` は DB を開いて起動時の検査を行い、結果を標準エラーへ出して終わる
//! (ファイル監視は M104d、pipe は M104f)。

use std::path::PathBuf;
use std::process::ExitCode;

use mye_track::db::now_ms;
use mye_track::project::project_id;
use mye_track::startup::{open_project, ServiceState};

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

    let (_store, report) = open_project(&root, now_ms());
    eprintln!("projectId: {}", project_id(&root));
    eprintln!("{report:#?}");
    // 記録できる状態なら 0。db_degraded / Unavailable は 1 (プロセスは落とさず、報告して終わる)
    match report.state {
        Some(ServiceState::Recording) => ExitCode::SUCCESS,
        _ => ExitCode::from(1),
    }
}
