//! サービス (`mye_track.exe serve`) の切り離し起動。フックの CLI (`hook.rs`) が使う。
//!
//! 作法は tools\mcp\src\track_client.rs の `start_service` と同じ (あちらは crate に依存しない約束なので、
//! 実装は別に持つ。変えるときは両方を直す)。

use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use windows_sys::Win32::Foundation::{
    GetHandleInformation, SetHandleInformation, ERROR_ACCESS_DENIED, HANDLE, HANDLE_FLAG_INHERIT, INVALID_HANDLE_VALUE,
};
use windows_sys::Win32::System::Console::{GetStdHandle, STD_ERROR_HANDLE, STD_INPUT_HANDLE, STD_OUTPUT_HANDLE};

/// サービスの exe を上書きする環境変数。設定されていればそれだけを使う (無ければ他を探さず失敗する)。
pub const ENV_TRACK_EXE: &str = "MYE_TRACK_EXE";

const CREATE_BREAKAWAY_FROM_JOB: u32 = 0x0100_0000;
const DETACHED_PROCESS: u32 = 0x0000_0008;

/// 起動する exe。`MYE_TRACK_EXE` があればそれ、無ければ自分自身 (フックは `mye_track.exe` 自身なので隣を探さない)。
pub fn find_track_exe() -> Result<PathBuf, String> {
    if let Some(value) = std::env::var_os(ENV_TRACK_EXE).filter(|v| !v.is_empty()) {
        let path = PathBuf::from(value);
        return if path.is_file() {
            Ok(path)
        } else {
            Err(format!("{ENV_TRACK_EXE} が存在しないファイルを指している: {}", path.display()))
        };
    }
    std::env::current_exe().map_err(|e| format!("自分自身の場所を得られない: {e}"))
}

/// 標準入出力のハンドルを継承不可にしておく間だけ生きる。Rust の `Command` は bInheritHandles = TRUE で起動するので、
/// 子の標準入出力を null にしても、親の継承可能なハンドルは子へ複製される。サービスがフックの標準出力を掴むと、
/// フックが終わっても Claude Code がその pipe の EOF を待ち続ける。
struct StdHandlesHidden(Vec<(HANDLE, u32)>);

impl StdHandlesHidden {
    fn new() -> Self {
        let mut saved = Vec::new();
        for which in [STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE] {
            // SAFETY: GetStdHandle は値を返すだけ。Get/SetHandleInformation は有効なハンドル (null と
            // INVALID_HANDLE_VALUE を除く) にだけ呼ぶ
            unsafe {
                let handle = GetStdHandle(which);
                if handle.is_null() || handle == INVALID_HANDLE_VALUE {
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

/// `serve --project <root>` を切り離して起動する。子は待たない。終了コード 3 (既に居る) は見えなくてよい
/// (呼び出し側の接続のやり直しで分かる)。
pub fn start_service(root: &Path) -> Result<(), String> {
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
    // 呼び出し元の Job が BREAKAWAY を許さないと、起動が ERROR_ACCESS_DENIED になる。外してやり直す
    let started = match spawn(CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS) {
        Err(e) if e.raw_os_error() == Some(ERROR_ACCESS_DENIED as i32) => spawn(DETACHED_PROCESS),
        other => other,
    };
    match started {
        Ok(child) => {
            drop(child);
            Ok(())
        }
        Err(e) => Err(format!("{} を起動できない: {e}", exe.display())),
    }
}
