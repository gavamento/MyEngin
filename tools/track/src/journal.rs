//! イベントジャーナル (db-schema.md §5)。DB へ入れる前に追記する 1 行 1 JSON のログ。
//!
//! 1 ファイルは 16 MiB まで。Blob の中身は書かない (呼び出し側がハッシュだけを payload に入れる)。

use std::fmt;
use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// 1 ファイルの上限の既定。
pub const DEFAULT_MAX_FILE_BYTES: u64 = 16 * 1024 * 1024;

/// ジャーナルの 1 行。
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct JournalEntry {
    /// UTC ミリ秒。
    pub at: i64,
    pub kind: String,
    pub payload: serde_json::Value,
}

/// ジャーナル内の位置。ファイル番号と 1 始まりの行番号。`events.journal_pos` に文字列で入る。
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct JournalPos {
    pub file: u32,
    pub line: u64,
}

impl fmt::Display for JournalPos {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:06}.ndjson:{}", self.file, self.line)
    }
}

impl JournalPos {
    /// `000001.ndjson:12` を読む。
    pub fn parse(s: &str) -> Option<JournalPos> {
        let (name, line) = s.split_once(':')?;
        let file = file_number(name)?;
        Some(JournalPos { file, line: line.parse().ok()? })
    }
}

fn file_name(number: u32) -> String {
    format!("{number:06}.ndjson")
}

/// `NNNNNN.ndjson` からファイル番号を取る。他の名前は None。
fn file_number(name: &str) -> Option<u32> {
    let stem = name.strip_suffix(".ndjson")?;
    if stem.len() == 6 && stem.bytes().all(|b| b.is_ascii_digit()) {
        stem.parse().ok()
    } else {
        None
    }
}

/// ディレクトリ内のジャーナルファイルを番号順に返す。
fn list_files(dir: &Path) -> io::Result<Vec<(u32, PathBuf)>> {
    let mut files = Vec::new();
    for entry in std::fs::read_dir(dir)? {
        let entry = entry?;
        if let Some(number) = entry.file_name().to_str().and_then(file_number) {
            files.push((number, entry.path()));
        }
    }
    files.sort();
    Ok(files)
}

/// 本文を行に分ける。最後の改行より後ろの断片 (書きかけ) も 1 行として数える。
fn split_lines(bytes: &[u8]) -> Vec<&[u8]> {
    let mut lines: Vec<&[u8]> = bytes.split(|&b| b == b'\n').collect();
    if lines.last().is_some_and(|l| l.is_empty()) {
        lines.pop();
    }
    lines
}

pub struct Journal {
    dir: PathBuf,
    max_file_bytes: u64,
    file_no: u32,
    /// 現在のファイルに書き込み済みの行数 (書きかけの断片を含む)。
    line_no: u64,
    size: u64,
    /// 現在のファイルが改行で終わっていない (前回の書きかけが残っている)。
    needs_newline: bool,
    file: Option<File>,
}

impl Journal {
    /// ディレクトリを開く。既存のファイルがあれば最後の番号の続きから書く。
    pub fn open(dir: &Path, max_file_bytes: u64) -> io::Result<Journal> {
        let (file_no, line_no, size, needs_newline) = match list_files(dir)?.pop() {
            Some((number, path)) => {
                let bytes = std::fs::read(&path)?;
                (number, split_lines(&bytes).len() as u64, bytes.len() as u64, !bytes.is_empty() && !bytes.ends_with(b"\n"))
            }
            None => (1, 0, 0, false),
        };
        Ok(Journal { dir: dir.to_path_buf(), max_file_bytes, file_no, line_no, size, needs_newline, file: None })
    }

    /// 1 行追記して位置を返す。上限を超えるなら次の番号のファイルへ進む。
    pub fn append(&mut self, entry: &JournalEntry) -> io::Result<JournalPos> {
        let mut line = serde_json::to_string(entry).map_err(io::Error::other)?;
        line.push('\n');
        let prefix = usize::from(self.needs_newline) as u64;
        if self.size > 0 && self.size + prefix + line.len() as u64 > self.max_file_bytes {
            self.file_no += 1;
            self.line_no = 0;
            self.size = 0;
            self.needs_newline = false;
            self.file = None;
        }
        if self.file.is_none() {
            let path = self.dir.join(file_name(self.file_no));
            self.file = Some(OpenOptions::new().create(true).append(true).open(path)?);
        }
        let file = self.file.as_mut().expect("opened above");
        let mut buf = Vec::with_capacity(line.len() + 1);
        if self.needs_newline {
            // 前回の書きかけの行を終わらせる。その行は読むとき壊れた 1 行として扱われる
            buf.push(b'\n');
        }
        buf.extend_from_slice(line.as_bytes());
        file.write_all(&buf)?;
        file.flush()?;
        self.needs_newline = false;
        self.size += buf.len() as u64;
        self.line_no += 1;
        Ok(JournalPos { file: self.file_no, line: self.line_no })
    }
}

/// `read_after` の 1 行分。`entry` が None なら壊れた行 (JSON として読めない)。
pub struct ReadLine {
    pub pos: JournalPos,
    pub entry: Option<JournalEntry>,
}

/// `after` より後ろの行を番号順に読む。`after` が None なら先頭から。
pub fn read_after(dir: &Path, after: Option<JournalPos>) -> io::Result<Vec<ReadLine>> {
    let mut out = Vec::new();
    for (number, path) in list_files(dir)? {
        if after.is_some_and(|a| number < a.file) {
            continue;
        }
        let bytes = std::fs::read(&path)?;
        for (i, raw) in split_lines(&bytes).into_iter().enumerate() {
            let pos = JournalPos { file: number, line: i as u64 + 1 };
            if after.is_some_and(|a| pos <= a) {
                continue;
            }
            let entry = serde_json::from_slice::<JournalEntry>(raw).ok();
            out.push(ReadLine { pos, entry });
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pos_round_trip() {
        let p = JournalPos { file: 3, line: 42 };
        assert_eq!(p.to_string(), "000003.ndjson:42");
        assert_eq!(JournalPos::parse("000003.ndjson:42"), Some(p));
        assert_eq!(JournalPos::parse("3.ndjson:42"), None);
        assert_eq!(JournalPos::parse("garbage"), None);
    }

    #[test]
    fn split_lines_counts_partial_tail() {
        assert_eq!(split_lines(b"a\nb\n").len(), 2);
        assert_eq!(split_lines(b"a\nb").len(), 2);
        assert_eq!(split_lines(b"").len(), 0);
    }
}
