//! projectId と、そのもとになるパスの正規化 (architecture.md §3)。
//!
//! 正規化は C++ 側 `mye::NormalizePathKey` (src\Engine\Platform\PathUtil.cpp) と同じ結果にする:
//! 絶対化 (GetFullPathNameW。`.` `..` と重複する区切りの整理を含む) → '/' を '\' に →
//! ASCII の大文字だけ小文字に。末尾の区切りは入力にあれば残る。
//! projectId は `normalize_path_key` の結果から末尾の区切りを全部落としたものをハッシュする
//! (`project_id_source`)。`C:\Proj\MyGame` と `C:\Proj\MyGame\` が別の ID になると、mutex と pipe の
//! 名前が割れて同じ DB に書き手が 2 つできるため。ドライブのルート (`c:\`) だけは落とさない。
//! 非 ASCII を小文字にしないのは、プロジェクトの C++ が "C" ロケールの `towlower` を使い、
//! その結果が ASCII のみの変換になるため (tests\project_id_cases.tsv は C++ の実測)。

use std::os::windows::ffi::OsStrExt;
use std::path::Path;

use crate::hash::sha256_hex;

/// projectId の桁数 (SHA-256 の 16 進の先頭)。
const PROJECT_ID_LEN: usize = 16;

/// `NormalizePathKey` と同じ規則でパスを正規化する。
pub fn normalize_path_key(path: &Path) -> String {
    let absolute = std::path::absolute(path).unwrap_or_else(|_| path.to_path_buf());
    let units: Vec<u16> = absolute
        .as_os_str()
        .encode_wide()
        .map(|c| match c {
            0x2F => 0x5C,
            0x41..=0x5A => c + 0x20,
            _ => c,
        })
        .collect();
    String::from_utf16_lossy(&units)
}

/// projectId のもとになる文字列: `normalize_path_key` の結果から末尾の '\' を全部落としたもの。
/// ドライブのルート (`c:\`) は落とさない。
pub fn project_id_source(root: &Path) -> String {
    let key = normalize_path_key(root);
    let is_drive_root = key.len() == 3 && key.ends_with(":\\");
    if is_drive_root {
        return key;
    }
    let trimmed = key.trim_end_matches('\\');
    // 全部が区切り (`\` など) のときは空にせず、そのまま使う
    if trimmed.is_empty() { key } else { trimmed.to_string() }
}

/// `project_id_source` の UTF-8 を SHA-256 にかけた、先頭 16 桁。pipe 名・mutex 名・DB で共通。
pub fn project_id(root: &Path) -> String {
    sha256_hex(project_id_source(root).as_bytes())[..PROJECT_ID_LEN].to_string()
}
