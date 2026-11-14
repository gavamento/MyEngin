//! `.git` を読むだけで、HEAD のコミットとリンクされた作業ツリーかどうかを調べる。
//!
//! git を起動しない。ここで得た値は表示と `worktree` 根拠の構造確認にだけ使い、
//! 「どの変更が AI のものか」の判定には使わない (attribution-rules.md §4)。

use std::path::{Path, PathBuf};

/// `.git` の指す先。`git_dir` は HEAD のあるディレクトリ、`common_dir` は参照 (refs / packed-refs) の本体。
struct GitDirs {
    git_dir: PathBuf,
    common_dir: PathBuf,
    /// `.git` がファイル (`gitdir:` の行を持つ) だった。
    linked: bool,
}

/// `gitdir: <path>` の行を読む。
fn read_gitdir_line(path: &Path) -> Option<PathBuf> {
    let text = std::fs::read_to_string(path).ok()?;
    let rest = text.lines().find_map(|line| line.trim().strip_prefix("gitdir:"))?;
    Some(PathBuf::from(rest.trim()))
}

fn locate(root: &Path) -> Option<GitDirs> {
    let dot_git = root.join(".git");
    let meta = std::fs::metadata(&dot_git).ok()?;
    if meta.is_dir() {
        return Some(GitDirs { git_dir: dot_git.clone(), common_dir: dot_git, linked: false });
    }
    let target = read_gitdir_line(&dot_git)?;
    let git_dir = if target.is_absolute() { target } else { root.join(target) };
    // 作業ツリーの gitdir は `commondir` (相対パス) で本体を指す。無ければ自分が本体
    let common_dir = std::fs::read_to_string(git_dir.join("commondir"))
        .ok()
        .map(|text| {
            let p = PathBuf::from(text.trim());
            if p.is_absolute() { p } else { git_dir.join(p) }
        })
        .unwrap_or_else(|| git_dir.clone());
    // リンクされた作業ツリーの gitdir は `<本体>\.git\worktrees\<名前>`。サブモジュールの `.git` ファイルも
    // `gitdir:` の行を持つが、`modules` の下を指すので作業ツリーとは見なさない
    let linked = git_dir.parent().and_then(Path::file_name).is_some_and(|n| n.eq_ignore_ascii_case("worktrees"));
    Some(GitDirs { git_dir, common_dir, linked })
}

/// プロジェクトのルートが、git のリンクされた作業ツリー (`.git` がファイルで、`gitdir:` の指す先が `worktrees` の下) か。
pub fn is_linked_worktree(root: &Path) -> bool {
    locate(root).is_some_and(|d| d.linked)
}

fn is_object_id(text: &str) -> bool {
    matches!(text.len(), 40 | 64) && text.bytes().all(|b| b.is_ascii_hexdigit())
}

fn lookup_packed(dir: &Path, reference: &str) -> Option<String> {
    let text = std::fs::read_to_string(dir.join("packed-refs")).ok()?;
    text.lines().find_map(|line| {
        let (id, name) = line.split_once(' ')?;
        (name.trim() == reference && is_object_id(id)).then(|| id.to_string())
    })
}

/// 開始時点の HEAD のコミット。参照 → ゆるい参照ファイル → `packed-refs` の順に読む。detached HEAD はそのままのハッシュ。
/// git でない、または読めなければ None。
pub fn base_commit(root: &Path) -> Option<String> {
    let dirs = locate(root)?;
    let head = std::fs::read_to_string(dirs.git_dir.join("HEAD")).ok()?;
    let head = head.trim();
    let Some(reference) = head.strip_prefix("ref:") else {
        return is_object_id(head).then(|| head.to_string());
    };
    let reference = reference.trim();
    for dir in [&dirs.git_dir, &dirs.common_dir] {
        if let Ok(text) = std::fs::read_to_string(dir.join(reference)) {
            let id = text.trim();
            if is_object_id(id) {
                return Some(id.to_string());
            }
        }
    }
    lookup_packed(&dirs.common_dir, reference).or_else(|| lookup_packed(&dirs.git_dir, reference))
}
