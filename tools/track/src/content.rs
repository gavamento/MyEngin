//! ファイル内容の読み取り (architecture.md §4 の「書き込み中の見分け方」)。
//!
//! 追跡が他のプロセスの保存を失敗させてはならない (要求 §1.3)。内容は全共有
//! (`FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`) で開いて読み、前後のサイズと更新時刻が
//! 同じことを確かめる。違えば読み直し、上限を超えたらその時点の内容を `coalesced` として返す。
//!
//! 「`FILE_SHARE_READ` だけで開けるか探る」方式は採らない。開いている一瞬にも、他のプロセスの
//! 書き込み open・rename (`ERROR_ACCESS_DENIED`)・delete が失敗する (tests/watch.rs の
//! `writers_never_fail_while_the_service_reads` を探り付きで回すと 4 分の 1 が失敗した)。
//! 書き手が続いているかどうかは、サイズが続けて同じかで見る (collector.rs の `settle`)。

use std::fs::{File, OpenOptions};
use std::io::{self, Read};
use std::os::windows::fs::OpenOptionsExt;
use std::path::Path;
use std::time::{Duration, SystemTime};

use windows_sys::Win32::Foundation::{ERROR_LOCK_VIOLATION, ERROR_SHARING_VIOLATION};
use windows_sys::Win32::Storage::FileSystem::{FILE_SHARE_DELETE, FILE_SHARE_READ, FILE_SHARE_WRITE};

/// 読み直しの間隔。書き手の 1 回の書き込みが終わるのを待つだけの短さ。
const REREAD_PAUSE: Duration = Duration::from_millis(5);

const SHARE_ALL: u32 = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

#[derive(Debug)]
pub enum ReadOutcome {
    /// 無い (パスの途中が欠けている場合も含む)。
    Missing,
    /// ファイルではない (ディレクトリ、シンボリックリンク)。
    NotFile,
    /// 他のプロセスが共有なしで開いていて、読めない。後でやり直す。
    Busy,
    /// `coalesced` = 読み取りが安定せず、途中の内容の可能性がある。
    /// `modified` = 読み終えた時点の更新時刻 (ベースラインの検証の走査が、読まずに済ませる判断に使う)。
    Content { bytes: Vec<u8>, coalesced: bool, modified: Option<SystemTime> },
    Failed(io::Error),
}

#[derive(Debug)]
pub enum LenOutcome {
    Missing,
    NotFile,
    Busy,
    Len(u64),
    Failed(io::Error),
}

#[derive(Debug)]
pub enum StatOutcome {
    Missing,
    NotFile,
    Busy,
    Stat { len: u64, modified: Option<SystemTime> },
    Failed(io::Error),
}

fn is_sharing_violation(e: &io::Error) -> bool {
    matches!(e.raw_os_error(), Some(c) if c == ERROR_SHARING_VIOLATION as i32 || c == ERROR_LOCK_VIOLATION as i32)
}

/// パスが無い、またはパスの途中がディレクトリではない。
fn is_missing(e: &io::Error) -> bool {
    // 3 = ERROR_PATH_NOT_FOUND、267 = ERROR_DIRECTORY (途中の成分がファイル)
    e.kind() == io::ErrorKind::NotFound || matches!(e.raw_os_error(), Some(3) | Some(267))
}

/// 通常のファイルか。ディレクトリ・シンボリックリンクは `NotFile`。
fn check_regular(path: &Path) -> Option<ReadOutcome> {
    match std::fs::symlink_metadata(path) {
        Ok(meta) if meta.is_dir() || meta.file_type().is_symlink() => Some(ReadOutcome::NotFile),
        Ok(_) => None,
        Err(e) if is_missing(&e) => Some(ReadOutcome::Missing),
        Err(e) => Some(ReadOutcome::Failed(e)),
    }
}

fn open_shared(path: &Path) -> Result<File, ReadOutcome> {
    OpenOptions::new().read(true).share_mode(SHARE_ALL).open(path).map_err(|e| {
        if is_missing(&e) {
            ReadOutcome::Missing
        } else if is_sharing_violation(&e) {
            ReadOutcome::Busy
        } else {
            ReadOutcome::Failed(e)
        }
    })
}

/// 開いているハンドルの現在の大きさと更新時刻 (パスからの問い合わせと違い、書き手が書いた分が見える)。
pub fn peek_stat(path: &Path) -> StatOutcome {
    match check_regular(path) {
        Some(ReadOutcome::NotFile) => return StatOutcome::NotFile,
        Some(ReadOutcome::Missing) => return StatOutcome::Missing,
        Some(ReadOutcome::Failed(e)) => return StatOutcome::Failed(e),
        _ => {}
    }
    match open_shared(path) {
        Ok(file) => match file.metadata() {
            Ok(m) => StatOutcome::Stat { len: m.len(), modified: m.modified().ok() },
            Err(e) => StatOutcome::Failed(e),
        },
        Err(ReadOutcome::Missing) => StatOutcome::Missing,
        Err(ReadOutcome::Busy) => StatOutcome::Busy,
        Err(ReadOutcome::Failed(e)) => StatOutcome::Failed(e),
        Err(_) => unreachable!("open_shared は Missing / Busy / Failed だけ返す"),
    }
}

/// 開いているハンドルの現在の大きさ。
pub fn peek_len(path: &Path) -> LenOutcome {
    match peek_stat(path) {
        StatOutcome::Missing => LenOutcome::Missing,
        StatOutcome::NotFile => LenOutcome::NotFile,
        StatOutcome::Busy => LenOutcome::Busy,
        StatOutcome::Stat { len, .. } => LenOutcome::Len(len),
        StatOutcome::Failed(e) => LenOutcome::Failed(e),
    }
}

/// 内容を読む。前後のサイズと更新時刻が同じなら安定とみなし、違えば `stable_attempts` 回まで読み直す。
pub fn read_stable(path: &Path, stable_attempts: u32) -> ReadOutcome {
    if let Some(outcome) = check_regular(path) {
        return outcome;
    }
    let mut last: Vec<u8> = Vec::new();
    let mut last_modified: Option<SystemTime> = None;
    for attempt in 0..stable_attempts.max(1) {
        if attempt > 0 {
            std::thread::sleep(REREAD_PAUSE);
        }
        let mut file = match open_shared(path) {
            Ok(f) => f,
            Err(outcome) => return outcome,
        };
        let before = match file.metadata() {
            Ok(m) => m,
            Err(e) => return ReadOutcome::Failed(e),
        };
        let mut bytes = Vec::with_capacity(before.len() as usize);
        if let Err(e) = file.read_to_end(&mut bytes) {
            return ReadOutcome::Failed(e);
        }
        let after = match file.metadata() {
            Ok(m) => m,
            Err(e) => return ReadOutcome::Failed(e),
        };
        let stable = before.len() == after.len()
            && before.modified().ok() == after.modified().ok()
            && bytes.len() as u64 == after.len();
        if stable {
            return ReadOutcome::Content { bytes, coalesced: false, modified: after.modified().ok() };
        }
        last = bytes;
        last_modified = after.modified().ok();
    }
    ReadOutcome::Content { bytes: last, coalesced: true, modified: last_modified }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("mye_track_content_{}_{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn reads_missing_dir_and_file() {
        let dir = temp("basic");
        std::fs::write(dir.join("a.txt"), b"hello").unwrap();
        assert!(matches!(read_stable(&dir.join("a.txt"), 3), ReadOutcome::Content { coalesced: false, .. }));
        assert!(matches!(read_stable(&dir.join("none.txt"), 3), ReadOutcome::Missing));
        assert!(matches!(read_stable(&dir.join("a.txt").join("x"), 3), ReadOutcome::Missing));
        assert!(matches!(read_stable(&dir, 3), ReadOutcome::NotFile));
        assert!(matches!(peek_len(&dir.join("a.txt")), LenOutcome::Len(5)));
        assert!(matches!(peek_len(&dir.join("none.txt")), LenOutcome::Missing));
    }

    #[test]
    fn open_writer_does_not_block_reading_and_peek_sees_growth() {
        use std::io::Write;
        let dir = temp("writer");
        let path = dir.join("w.txt");
        let mut writer = File::create(&path).unwrap();
        writer.write_all(b"partial").unwrap();
        match read_stable(&path, 3) {
            ReadOutcome::Content { bytes, .. } => assert_eq!(bytes, b"partial"),
            other => panic!("{other:?}"),
        }
        assert!(matches!(peek_len(&path), LenOutcome::Len(7)));
        writer.write_all(b" more").unwrap();
        // パスからの問い合わせはキャッシュで古いことがあるが、ハンドル経由なら書き込み済みの大きさが見える
        assert!(matches!(peek_len(&path), LenOutcome::Len(12)));
    }

    #[test]
    fn exclusive_writer_is_busy() {
        let dir = temp("exclusive");
        let path = dir.join("e.txt");
        std::fs::write(&path, b"x").unwrap();
        let held = OpenOptions::new().read(true).write(true).share_mode(0).open(&path).unwrap();
        assert!(matches!(read_stable(&path, 3), ReadOutcome::Busy));
        assert!(matches!(peek_len(&path), LenOutcome::Busy));
        drop(held);
        assert!(matches!(read_stable(&path, 3), ReadOutcome::Content { .. }));
    }
}
