//! プロセスの生死 (db-schema.md §6 の 5)。PID は使い回されるので、開始時刻と組で見る。

use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, ERROR_ACCESS_DENIED, FILETIME, HANDLE};
use windows_sys::Win32::System::Threading::{
    GetExitCodeProcess, GetProcessTimes, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION,
};

/// GetExitCodeProcess が「まだ動いている」ときに返す値 (STILL_ACTIVE)。
const STILL_ACTIVE_CODE: u32 = 259;

/// FILETIME (1601-01-01 から 100ns) と Unix epoch (1970-01-01) の差 (ミリ秒)。
const FILETIME_TO_UNIX_MS: i64 = 11_644_473_600_000;

/// 開始時刻の比較で許す誤差。記録する側と読む側で丸めが違っても同じプロセスとして扱う。
const START_TOLERANCE_MS: i64 = 2000;

fn filetime_to_unix_ms(ft: &FILETIME) -> i64 {
    let ticks = ((ft.dwHighDateTime as u64) << 32) | ft.dwLowDateTime as u64;
    (ticks / 10_000) as i64 - FILETIME_TO_UNIX_MS
}

/// プロセスの開始時刻 (UTC ミリ秒)。開けない / 既に無いときは None。
pub fn process_start_ms(pid: u32) -> Option<i64> {
    // SAFETY: OpenProcess の戻りを CloseHandle で必ず閉じる。FILETIME は出力先として初期化済み
    unsafe {
        let handle: HANDLE = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if handle.is_null() {
            return None;
        }
        let zero = FILETIME { dwLowDateTime: 0, dwHighDateTime: 0 };
        let (mut created, mut exited, mut kernel, mut user) = (zero, zero, zero, zero);
        let ok = GetProcessTimes(handle, &mut created, &mut exited, &mut kernel, &mut user);
        CloseHandle(handle);
        (ok != 0).then(|| filetime_to_unix_ms(&created))
    }
}

/// `pid` のプロセスが、記録時の開始時刻 `start_ms` のまま生きているか。
/// 開始時刻が分からない (None) ときは PID だけで判断する。
pub fn is_alive(pid: u32, start_ms: Option<i64>) -> bool {
    // SAFETY: 上と同じ。取得したハンドルは全経路で閉じる
    unsafe {
        let handle: HANDLE = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if handle.is_null() {
            // 権限が足りないだけなら存在はしている。それ以外 (INVALID_PARAMETER) は居ない
            return GetLastError() == ERROR_ACCESS_DENIED;
        }
        let mut code = 0u32;
        let alive = GetExitCodeProcess(handle, &mut code) != 0 && code == STILL_ACTIVE_CODE;
        CloseHandle(handle);
        if !alive {
            return false;
        }
    }
    match (start_ms, process_start_ms(pid)) {
        (Some(recorded), Some(actual)) => (recorded - actual).abs() <= START_TOLERANCE_MS,
        _ => true,
    }
}
