//! Blob ストア (db-schema.md §4)。内容アドレス・原本そのまま・検証してから確定。

use std::fmt;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

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

pub struct BlobStore {
    root: PathBuf,
    exclude_patterns: Vec<String>,
    /// 書いた後の読み直しの口。テストが差し替えて検証失敗を作る。
    pub read_back: fn(&Path) -> io::Result<Vec<u8>>,
}

impl BlobStore {
    pub fn new(root: PathBuf, exclude_patterns: Vec<String>) -> Self {
        BlobStore { root, exclude_patterns, read_back: |p| std::fs::read(p) }
    }

    /// 既定の除外パターンで作る。
    pub fn with_defaults(root: PathBuf) -> Self {
        Self::new(root, DEFAULT_EXCLUDE_PATTERNS.iter().map(|s| s.to_string()).collect())
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
        let size = bytes.len() as u64;
        if self.is_excluded(rel_path) {
            return Ok(PutOutcome::Excluded { size });
        }
        let hash = sha256_hex(bytes);
        let final_path = self.path_for_hash(&hash);
        // 同じ名前のファイルがあっても、大きさが違えば途中で壊れた残りなので書き直す
        if let Ok(meta) = std::fs::metadata(&final_path) {
            if meta.len() == size {
                return Ok(PutOutcome::Stored { hash, size, existed: true });
            }
        }
        let dir = final_path.parent().expect("blob path has a parent");
        std::fs::create_dir_all(dir)?;
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

    fn write_verified(&self, temp: &Path, final_path: &Path, bytes: &[u8], hash: &str) -> Result<(), BlobError> {
        {
            use std::io::Write;
            let mut f = std::fs::File::create(temp)?;
            f.write_all(bytes)?;
            f.sync_all()?;
        }
        let actual = sha256_hex(&(self.read_back)(temp)?);
        if actual != hash {
            return Err(BlobError::VerifyFailed { expected: hash.to_string(), actual });
        }
        // Windows の rename は既存の最終名を置き換える (同じ内容の競合は無害)
        std::fs::rename(temp, final_path)?;
        Ok(())
    }
}

/// `*` (0 文字以上) と `?` (1 文字) だけの glob。ASCII の大文字小文字は区別しない。
fn glob_match_ci(pattern: &str, name: &str) -> bool {
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
