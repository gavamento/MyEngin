//! Blob ストア (db-schema.md §4)。内容アドレス・原本そのまま・検証してから確定。

use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

use crate::hash::sha256_hex;

/// 原本を残さないファイル名のパターン (要求 §14.2)。ファイル名 (ディレクトリを除く) に当てる。
pub const DEFAULT_EXCLUDE_PATTERNS: &[&str] = &[
    ".env",
    ".env.*",
    "*.pem",
    "*.key",
    "*.pfx",
    "*.p12",
    "id_rsa*",
    "credentials*.json",
];

/// Blob の総量の上限の既定 (db-schema.md §7)。回収の実行は sub-09 以降で、ここでは値だけ持つ。
pub const DEFAULT_MAX_TOTAL_BYTES: u64 = 2 * 1024 * 1024 * 1024;

/// Revision の保存期間の既定 (日)。回収の実行は sub-09 以降。
pub const DEFAULT_RETENTION_DAYS: u32 = 90;

/// `put` の結果。
#[derive(Debug, PartialEq, Eq)]
pub enum PutOutcome {
    /// 保存した (`existed` = 同じ内容が既にあり、書かなかった)。
    Stored { hash: String, size: u64, existed: bool },
    /// 除外パターンに当たった。内容は保存しない。
    Excluded { size: u64 },
}

#[derive(Debug)]
pub enum BlobError {
    Io(io::Error),
    /// 書いた後に読み直したら内容が合わない。最終名へは確定していない。
    VerifyFailed { expected: String, actual: String },
}

impl fmt::Display for BlobError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BlobError::Io(e) => write!(f, "blob I/O: {e}"),
            BlobError::VerifyFailed { expected, actual } => {
                write!(f, "blob の検証に失敗 (期待 {expected} / 実際 {actual})")
            }
        }
    }
}

impl std::error::Error for BlobError {}

impl From<io::Error> for BlobError {
    fn from(e: io::Error) -> Self {
        BlobError::Io(e)
    }
}

/// 一時ファイル名を衝突させない連番。
static TEMP_SEQ: AtomicU64 = AtomicU64::new(0);

/// `put` の所要時間の内訳 (ナノ秒の累計)。計測用 (`mye_track bench`) で、記録の判断には使わない。
#[derive(Default)]
pub struct BlobStats {
    pub puts: AtomicU64,
    /// 同じ内容が既にあって書かなかった数。
    pub existed: AtomicU64,
    pub hash_ns: AtomicU64,
    pub existence_check_ns: AtomicU64,
    pub ensure_dir_ns: AtomicU64,
    pub create_write_ns: AtomicU64,
    pub verify_ns: AtomicU64,
    pub rename_ns: AtomicU64,
}

fn add_elapsed(counter: &AtomicU64, since: Instant) {
    counter.fetch_add(since.elapsed().as_nanos() as u64, Ordering::Relaxed);
}

/// ハンドルを先頭へ戻して全部読む (既定の読み直し)。
pub fn read_from_start(file: &mut File) -> io::Result<Vec<u8>> {
    file.seek(SeekFrom::Start(0))?;
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes)?;
    Ok(bytes)
}

pub struct BlobStore {
    root: PathBuf,
    exclude_patterns: Vec<String>,
    pub stats: BlobStats,
    /// 書いた後の読み直しの口 (書いたときのハンドルを渡す)。テストが差し替えて検証失敗を作る。
    pub read_back: fn(&mut File) -> io::Result<Vec<u8>>,
}

impl BlobStore {
    pub fn new(root: PathBuf, exclude_patterns: Vec<String>) -> Self {
        BlobStore { root, exclude_patterns, stats: BlobStats::default(), read_back: read_from_start }
    }

    /// 既定の除外パターンで作る。
    pub fn with_defaults(root: PathBuf) -> Self {
        Self::new(root, DEFAULT_EXCLUDE_PATTERNS.iter().map(|s| s.to_string()).collect())
    }

    /// 同じ置き場所と除外パターンを持つ読み取り用のハンドル (Diff ワーカーが別スレッドで読むため)。統計は別。
    pub fn reader(&self) -> BlobStore {
        BlobStore::new(self.root.clone(), self.exclude_patterns.clone())
    }

    /// Blob の中身を読む。名前 (ハッシュ) と中身が合わなければ `InvalidData` (壊れた Blob を内容として返さない)。
    pub fn read(&self, hash: &str) -> io::Result<Vec<u8>> {
        let bytes = std::fs::read(self.path_for_hash(hash))?;
        if sha256_hex(&bytes) != hash {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "blob の中身がハッシュと違う"));
        }
        Ok(bytes)
    }

    /// ハッシュに対応する最終パス: `<root>\<先頭 2 桁>\<残り>`。
    pub fn path_for_hash(&self, hash: &str) -> PathBuf {
        self.root.join(&hash[..2]).join(&hash[2..])
    }

    /// `rel_path` (ルートからの相対パス) が除外パターンに当たるか。大文字小文字は区別しない。
    pub fn is_excluded(&self, rel_path: &str) -> bool {
        let name = rel_path.rsplit(['/', '\\']).next().unwrap_or(rel_path);
        self.exclude_patterns.iter().any(|p| glob_match_ci(p, name))
    }

    /// 内容を保存する。除外パスなら保存しない。書いた後に読み直して検証し、
    /// 合ったときだけ最終名へ rename する (一時ファイルは失敗時に消す)。
    pub fn put(&self, rel_path: &str, bytes: &[u8]) -> Result<PutOutcome, BlobError> {
        let started = Instant::now();
        let hash = sha256_hex(bytes);
        add_elapsed(&self.stats.hash_ns, started);
        self.put_hashed(rel_path, bytes, &hash)
    }

    /// 呼び出し側が内容のハッシュを持っているときの `put` (二度目の計算を省く)。
    /// `hash` が内容と違えば、読み直しの検証で失敗する (最終名へは確定しない)。
    pub fn put_hashed(&self, rel_path: &str, bytes: &[u8], hash: &str) -> Result<PutOutcome, BlobError> {
        let size = bytes.len() as u64;
        if self.is_excluded(rel_path) {
            return Ok(PutOutcome::Excluded { size });
        }
        self.stats.puts.fetch_add(1, Ordering::Relaxed);
        let hash = hash.to_string();
        let final_path = self.path_for_hash(&hash);
        // 同じ名前のファイルがあっても、大きさが違えば途中で壊れた残りなので書き直す
        let checked = Instant::now();
        let found = std::fs::metadata(&final_path);
        add_elapsed(&self.stats.existence_check_ns, checked);
        if let Ok(meta) = found {
            if meta.len() == size {
                self.stats.existed.fetch_add(1, Ordering::Relaxed);
                return Ok(PutOutcome::Stored { hash, size, existed: true });
            }
        }
        let dir = final_path.parent().expect("blob path has a parent");
        let started = Instant::now();
        std::fs::create_dir_all(dir)?;
        add_elapsed(&self.stats.ensure_dir_ns, started);
        let temp_path = dir.join(format!(
            ".tmp-{}-{}",
            std::process::id(),
            TEMP_SEQ.fetch_add(1, Ordering::Relaxed)
        ));
        let result = self.write_verified(&temp_path, &final_path, bytes, &hash);
        if result.is_err() {
            let _ = std::fs::remove_file(&temp_path);
        }
        result?;
        Ok(PutOutcome::Stored { hash, size, existed: false })
    }

    // `sync_all` はしない (1 件ごとの sync_all が一括記録の所要時間の過半を占める)。
    // 代わりに起動時、直近に書いた Blob の中身を確かめる (startup.rs の verify_recent_blobs)。
    // 読み直しは書いたハンドルで行う: 閉じて開き直すと、書いたばかりのファイルを OS のスキャンが
    // 開き直しの時点で検査し、読み直しが書き込みより遅くなる。
    fn write_verified(&self, temp: &Path, final_path: &Path, bytes: &[u8], hash: &str) -> Result<(), BlobError> {
        let started = Instant::now();
        let mut file = File::options().read(true).write(true).create(true).truncate(true).open(temp)?;
        file.write_all(bytes)?;
        add_elapsed(&self.stats.create_write_ns, started);
        let started = Instant::now();
        let actual = sha256_hex(&(self.read_back)(&mut file)?);
        add_elapsed(&self.stats.verify_ns, started);
        drop(file);
        if actual != hash {
            return Err(BlobError::VerifyFailed { expected: hash.to_string(), actual });
        }
        // Windows の rename は既存の最終名を置き換える (同じ内容の競合は無害)
        let started = Instant::now();
        std::fs::rename(temp, final_path)?;
        add_elapsed(&self.stats.rename_ns, started);
        Ok(())
    }
}

/// `*` (0 文字以上) と `?` (1 文字) だけの glob。ASCII の大文字小文字は区別しない。
pub(crate) fn glob_match_ci(pattern: &str, name: &str) -> bool {
    let p: Vec<char> = pattern.to_ascii_lowercase().chars().collect();
    let n: Vec<char> = name.to_ascii_lowercase().chars().collect();
    let (mut pi, mut ni) = (0, 0);
    let (mut star, mut star_n) = (None, 0);
    while ni < n.len() {
        if pi < p.len() && (p[pi] == '?' || p[pi] == n[ni]) {
            pi += 1;
            ni += 1;
        } else if pi < p.len() && p[pi] == '*' {
            star = Some(pi);
            star_n = ni;
            pi += 1;
        } else if let Some(s) = star {
            pi = s + 1;
            star_n += 1;
            ni = star_n;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn glob_basics() {
        assert!(glob_match_ci(".env", ".env"));
        assert!(glob_match_ci(".env", ".ENV"));
        assert!(!glob_match_ci(".env", "x.env"));
        assert!(glob_match_ci(".env.*", ".env.local"));
        assert!(!glob_match_ci(".env.*", ".env"));
        assert!(glob_match_ci("*.pem", "a.b.pem"));
        assert!(glob_match_ci("id_rsa*", "id_rsa.pub"));
        assert!(glob_match_ci("credentials*.json", "credentials.json"));
        assert!(glob_match_ci("credentials*.json", "credentials-prod.json"));
        assert!(!glob_match_ci("credentials*.json", "credentials.jsonx"));
        assert!(glob_match_ci("a?c", "abc"));
        assert!(!glob_match_ci("a?c", "ac"));
    }

    #[test]
    fn exclusion_uses_file_name_only() {
        let store = BlobStore::with_defaults(PathBuf::from("unused"));
        assert!(store.is_excluded("config/.env"));
        assert!(store.is_excluded("config\\Secrets.KEY"));
        assert!(!store.is_excluded(".env/readme.txt"));
        assert!(!store.is_excluded("assets/scene.scene"));
    }
}
