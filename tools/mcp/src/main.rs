//====================================================================================
//                          main.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          mye_mcp の入口 (引数・root の解決・stdin/stdout のループ)
//====================================================================================

mod handler;
mod index;
mod notes;
mod rpc;
mod search;
mod stdio;
mod tools;
mod verify;

use std::io::BufReader;
use std::path::{Path, PathBuf};

const SOLUTION_FILE: &str = "MyEngine.sln";

// start から親へ辿って MyEngine.sln のあるディレクトリを探す
fn find_root_from(start: &Path) -> Option<PathBuf> {
    start.ancestors().find(|d| d.join(SOLUTION_FILE).is_file()).map(Path::to_path_buf)
}

// --root 省略時: exe の場所から上へ → 無ければカレントから上へ
fn find_root_default() -> Option<PathBuf> {
    if let Ok(exe) = std::env::current_exe() {
        if let Some(root) = exe.parent().and_then(find_root_from) {
            return Some(root);
        }
    }
    std::env::current_dir().ok().and_then(|cwd| find_root_from(&cwd))
}

// 1 件の要求で panic してもサーバを落とさず、その要求だけ -32603 で返して次へ進む。
// panic = "unwind" (Cargo.toml) はこの catch_unwind のための設定。panic の内容は既定のフックが stderr に出す
fn handle_guarded(ctx: &mut handler::Context, request: rpc::Request) -> Option<rpc::Response> {
    let id = request.id.clone();
    let method = request.method.clone();
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| handler::handle(ctx, request))) {
        Ok(response) => response,
        Err(_) => {
            eprintln!("[mye_mcp] panic while handling {method}; the server keeps running");
            id.map(|id| rpc::Response::error(id, rpc::INTERNAL_ERROR, "Internal error"))
        }
    }
}

fn main() {
    let mut root_arg: Option<PathBuf> = None;
    let mut index_arg: Option<PathBuf> = None;
    let mut project_arg: Option<PathBuf> = None;
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < args.len() {
        let flag = args[i].as_str();
        if !matches!(flag, "--root" | "--index" | "--project") {
            eprintln!("mye_mcp: unknown argument: {flag}");
            std::process::exit(2);
        }
        let Some(value) = args.get(i + 1) else {
            eprintln!("mye_mcp: {flag} requires a value");
            std::process::exit(2);
        };
        if flag == "--root" {
            root_arg = Some(PathBuf::from(value));
        } else if flag == "--index" {
            index_arg = Some(PathBuf::from(value));
        } else {
            project_arg = Some(PathBuf::from(value));
        }
        i += 2;
    }

    // 見つからなくても起動する。道具が isError で理由を返す (縮退)
    let root = root_arg.or_else(find_root_default);
    match &root {
        Some(r) => eprintln!("[mye_mcp] root={}", r.display()),
        None => eprintln!("[mye_mcp] root not found (MyEngine.sln); tools will report an error"),
    }
    let mut ctx = handler::Context { root, index: index_arg, project: project_arg, jobs: verify::JobTable::default() };

    let mut input = BufReader::new(std::io::stdin().lock());
    let mut out = std::io::stdout().lock();
    loop {
        let line = match stdio::read_line(&mut input) {
            Ok(Some(l)) => l,
            Ok(None) => break,
            Err(e) => {
                eprintln!("[mye_mcp] stdin read failed: {e}");
                break;
            }
        };
        let response = match line {
            Err(_) => Some(rpc::Response::error(
                serde_json::Value::Null,
                rpc::PARSE_ERROR,
                "Parse error: input is not valid UTF-8",
            )),
            Ok(text) => match rpc::parse_request(&text) {
                Err(error_response) => Some(error_response),
                Ok(request) => handle_guarded(&mut ctx, request),
            },
        };
        if let Some(r) = response {
            if stdio::write_line(&mut out, &r.to_line()).is_err() {
                break;
            }
        }
    }
}
