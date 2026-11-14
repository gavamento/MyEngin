//! 監視の分け方 (architecture.md §4「監視を分ける」)。
//!
//! `notify` の Windows 版は `watch()` ごとに別のハンドルと 16 KiB のバッファを持ち、あふれを報告しない。
//! ルートを再帰 1 本で見ると、無視するディレクトリ (`bin` `obj` `target` `.git` など) の書き込みの洪水が
//! 追跡対象と同じバッファを取り合う。そこで、無視するディレクトリを含む所は非再帰で見て、
//! 無視するものを含まない子ディレクトリだけを再帰で見る。純粋にディレクトリの形から決まる (監視は張らない)。

use std::collections::BTreeMap;
use std::path::Path;

use crate::ignore::IgnoreRules;

/// 張る監視の本数の上限。超えたらルートの再帰 1 本へ戻す。
pub const MAX_WATCHES: usize = 256;

/// ルートからの相対 ('/' 区切り、ルートは "") → 再帰か。
pub struct WatchPlan {
    pub entries: BTreeMap<String, bool>,
    /// 分けた結果が上限を超え、ルートの再帰 1 本に戻した。
    pub fallback: bool,
}

struct Node {
    rel: String,
    children: Vec<Node>,
    /// 自分より下に無視するディレクトリがある。
    has_ignored: bool,
}

/// 無視しないディレクトリの木を作る。無視するディレクトリは辿らず、見つけたことだけ覚える。
fn scan(root: &Path, ignore: &mut IgnoreRules, rel: &str) -> Node {
    let dir = if rel.is_empty() { root.to_path_buf() } else { root.join(rel) };
    let mut names: Vec<String> = Vec::new();
    if let Ok(entries) = std::fs::read_dir(&dir) {
        for entry in entries.flatten() {
            // シンボリックリンクは辿らない (collector の走査と同じ)
            let Ok(file_type) = entry.file_type() else { continue };
            if file_type.is_dir() {
                if let Some(name) = entry.file_name().to_str() {
                    names.push(name.to_string());
                }
            }
        }
    }
    names.sort();
    let mut node = Node { rel: rel.to_string(), children: Vec::new(), has_ignored: false };
    for name in names {
        let child_rel = if rel.is_empty() { name } else { format!("{rel}/{name}") };
        if ignore.is_ignored(&child_rel, true) {
            node.has_ignored = true;
            continue;
        }
        let child = scan(root, ignore, &child_rel);
        node.has_ignored |= child.has_ignored;
        node.children.push(child);
    }
    node
}

fn emit(node: &Node, out: &mut BTreeMap<String, bool>) {
    // ルートは必ず自分の分を持つ (ルート直下のファイルと、最上位のディレクトリの出入りを見る)
    if !node.has_ignored {
        out.insert(node.rel.clone(), true);
        return;
    }
    out.insert(node.rel.clone(), false);
    for child in &node.children {
        emit(child, out);
    }
}

/// `root` の下の監視の分け方を決める。ディレクトリだけを歩く (ファイルは開かない)。
pub fn plan(root: &Path, max_watches: usize) -> WatchPlan {
    let mut ignore = IgnoreRules::new(root.to_path_buf());
    let tree = scan(root, &mut ignore, "");
    let mut entries = BTreeMap::new();
    emit(&tree, &mut entries);
    if entries.len() > max_watches {
        return WatchPlan { entries: BTreeMap::from([(String::new(), true)]), fallback: true };
    }
    WatchPlan { entries, fallback: false }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn tree(name: &str, dirs: &[&str]) -> std::path::PathBuf {
        let root = std::env::temp_dir().join(format!("mye_track_wp_{}_{name}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        for d in dirs {
            fs::create_dir_all(root.join(d)).unwrap();
        }
        root
    }

    fn entries(root: &Path, max: usize) -> Vec<(String, bool)> {
        plan(root, max).entries.into_iter().collect()
    }

    #[test]
    fn no_ignored_dir_means_one_recursive_watch_on_the_root() {
        let root = tree("clean", &["a/b", "c"]);
        assert_eq!(entries(&root, 256), vec![(String::new(), true)]);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn ignored_top_level_dir_splits_the_root() {
        let root = tree("top", &["bin/x", "src/a", "assets", ".mye"]);
        assert_eq!(
            entries(&root, 256),
            vec![(String::new(), false), ("assets".into(), true), ("src".into(), true)]
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn nested_ignored_dir_splits_only_its_ancestors() {
        let root = tree("nested", &["tools/track/src", "tools/track/target/debug", "tools/mcp/src", "src"]);
        assert_eq!(
            entries(&root, 256),
            vec![
                (String::new(), false),
                ("src".into(), true),
                ("tools".into(), false),
                ("tools/mcp".into(), true),
                ("tools/track".into(), false),
                ("tools/track/src".into(), true),
            ]
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn gitignore_directories_are_not_watched() {
        let root = tree("gitignore", &["Intermediate/x", "src"]);
        fs::write(root.join(".gitignore"), "Intermediate/\n").unwrap();
        assert_eq!(entries(&root, 256), vec![(String::new(), false), ("src".into(), true)]);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn over_the_cap_falls_back_to_one_recursive_watch() {
        let dirs: Vec<String> = (0..10).map(|i| format!("d{i}/target")).collect();
        let refs: Vec<&str> = dirs.iter().map(String::as_str).collect();
        let root = tree("cap", &refs);
        // d0〜d9 のそれぞれが split される (子は無視だけなので非再帰) 10 本 + ルート = 11 本
        assert_eq!(plan(&root, 11).entries.len(), 11);
        let over = plan(&root, 10);
        assert!(over.fallback);
        assert_eq!(over.entries.into_iter().collect::<Vec<_>>(), vec![(String::new(), true)]);
        let _ = fs::remove_dir_all(&root);
    }
}
