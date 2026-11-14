//! 追跡しないパスの判定 (architecture.md §4)。git は起動しない。
//!
//! 固定の名前 (`.git` `.mye` `cache` `dist` `target` `bin` `obj`) は深さを問わず、ディレクトリとして無視する。
//! `.gitignore` はルートと各ディレクトリのものを読む。対応する書き方:
//!   空行 / `#` の行、`!` による否定、末尾の `/` (ディレクトリだけ)、先頭または途中の `/` (その
//!   `.gitignore` のあるディレクトリからの相対)、`*` `?`、`**` (0 個以上のディレクトリ)。
//! 対応しない書き方 (その行は読み飛ばす = 無視されず追跡される側に倒れる):
//!   `[abc]` の文字クラス、`\` のエスケープ。`.git/info/exclude`、`core.excludesFile`、
//!   親が無視されたディレクトリの中の再包含 (`!`) も見ない。大文字小文字は区別しない (Windows)。

use std::collections::HashMap;
use std::path::PathBuf;

use crate::blob::glob_match_ci;

/// 深さを問わず無視するディレクトリ名。
pub const ALWAYS_IGNORED_DIR_NAMES: &[&str] = &[".git", ".mye", "cache", "dist", "target", "bin", "obj"];

/// 親のディレクトリ (最後の成分を除く) に固定の無視名がある。イベントの段で捨てる安価な判定。
pub fn under_always_ignored_dir(rel: &str) -> bool {
    let mut comps: Vec<&str> = rel.split('/').collect();
    comps.pop();
    comps.iter().any(|c| ALWAYS_IGNORED_DIR_NAMES.iter().any(|n| c.eq_ignore_ascii_case(n)))
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct Rule {
    negate: bool,
    dir_only: bool,
    /// 途中か先頭に `/` がある。`.gitignore` のあるディレクトリからの相対で全体を比べる。
    anchored: bool,
    segments: Vec<String>,
}

fn parse_line(line: &str) -> Option<Rule> {
    let line = line.trim_end_matches(['\r', '\n']).trim_end_matches(' ');
    if line.is_empty() || line.starts_with('#') || line.contains('[') || line.contains('\\') {
        return None;
    }
    let (negate, rest) = match line.strip_prefix('!') {
        Some(r) => (true, r),
        None => (false, line),
    };
    let (dir_only, rest) = match rest.strip_suffix('/') {
        Some(r) => (true, r),
        None => (false, rest),
    };
    let anchored = rest.contains('/');
    let segments: Vec<String> = rest.split('/').filter(|s| !s.is_empty()).map(str::to_string).collect();
    if segments.is_empty() {
        return None;
    }
    Some(Rule { negate, dir_only, anchored, segments })
}

fn parse_gitignore(text: &str) -> Vec<Rule> {
    text.trim_start_matches('\u{feff}').lines().filter_map(parse_line).collect()
}

fn match_segments(segs: &[String], comps: &[&str]) -> bool {
    match segs.split_first() {
        None => comps.is_empty(),
        Some((seg, rest)) if seg == "**" => {
            if rest.is_empty() {
                // 末尾の `**` は中身 (1 成分以上) に当たる
                return !comps.is_empty();
            }
            (0..=comps.len()).any(|skip| match_segments(rest, &comps[skip..]))
        }
        Some((seg, rest)) => {
            !comps.is_empty() && glob_match_ci(seg, comps[0]) && match_segments(rest, &comps[1..])
        }
    }
}

impl Rule {
    /// `comps` は `.gitignore` のあるディレクトリからの相対 (最後がそのパス自身)。
    fn matches(&self, comps: &[&str], is_dir: bool) -> bool {
        if self.dir_only && !is_dir {
            return false;
        }
        if self.anchored {
            match_segments(&self.segments, comps)
        } else {
            comps.last().is_some_and(|name| glob_match_ci(&self.segments[0], name))
        }
    }
}

/// 無視規則の読み込み済みキャッシュ。1 つのスレッドが持つ。
pub struct IgnoreRules {
    root: PathBuf,
    /// `.gitignore` のあるディレクトリ (ルートからの相対。ルートは "") → その規則。
    loaded: HashMap<String, Vec<Rule>>,
}

impl IgnoreRules {
    pub fn new(root: PathBuf) -> Self {
        IgnoreRules { root, loaded: HashMap::new() }
    }

    /// `base` ディレクトリの `.gitignore` を次の判定で読み直す。
    pub fn reload(&mut self, base: &str) {
        self.loaded.remove(base);
    }

    fn rules_for(&mut self, base: &str) -> &Vec<Rule> {
        if !self.loaded.contains_key(base) {
            let path = self.root.join(base).join(".gitignore");
            let rules = std::fs::read(&path)
                .map(|bytes| parse_gitignore(&String::from_utf8_lossy(&bytes)))
                .unwrap_or_default();
            self.loaded.insert(base.to_string(), rules);
        }
        &self.loaded[base]
    }

    /// `rel` ('/' 区切り、ルートからの相対) が追跡対象外か。祖先のどれかが無視されていれば真。
    pub fn is_ignored(&mut self, rel: &str, is_dir: bool) -> bool {
        let comps: Vec<&str> = rel.split('/').collect();
        (1..=comps.len()).any(|i| self.ignored_exact(&comps[..i], i < comps.len() || is_dir))
    }

    fn ignored_exact(&mut self, comps: &[&str], is_dir: bool) -> bool {
        if is_dir {
            let name = comps[comps.len() - 1];
            if ALWAYS_IGNORED_DIR_NAMES.iter().any(|n| name.eq_ignore_ascii_case(n)) {
                return true;
            }
        }
        let mut ignored = false;
        for depth in 0..comps.len() {
            let base = comps[..depth].join("/");
            let rules = self.rules_for(&base);
            for rule in rules {
                if rule.matches(&comps[depth..], is_dir) {
                    ignored = !rule.negate;
                }
            }
        }
        ignored
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rules(text: &str) -> IgnoreRules {
        let mut r = IgnoreRules::new(PathBuf::from("unused-root"));
        r.loaded.insert(String::new(), parse_gitignore(text));
        r
    }

    #[test]
    fn fixed_names_apply_at_any_depth_to_directories() {
        let mut r = rules("");
        assert!(r.is_ignored(".git/HEAD", false));
        assert!(r.is_ignored("tools/track/target/debug/x.exe", false));
        assert!(r.is_ignored("src/Scripting/obj", true));
        assert!(!r.is_ignored("src/Scripting/obj", false), "ファイルの名前までは無視しない");
        assert!(!r.is_ignored("assets/a.txt", false));
        assert!(under_always_ignored_dir("a/bin/b.txt"));
        assert!(!under_always_ignored_dir("a/b/bin"));
    }

    #[test]
    fn gitignore_basics() {
        let mut r = rules("*.log\n/build/\nlogs/\n!keep.log\n# comment\n\ndocs/**/tmp\nsrc/gen_*.cpp\n");
        assert!(r.is_ignored("a.log", false));
        assert!(r.is_ignored("x/y/a.log", false));
        assert!(!r.is_ignored("keep.log", false), "後ろの否定が勝つ");
        assert!(r.is_ignored("build/out.txt", false));
        assert!(!r.is_ignored("x/build/out.txt", false), "先頭の / はルートだけ");
        assert!(r.is_ignored("a/logs/x.txt", false), "末尾の / はディレクトリ");
        assert!(!r.is_ignored("logs", false), "同名のファイルは対象外");
        assert!(r.is_ignored("docs/a/b/tmp", false));
        assert!(r.is_ignored("docs/tmp", false), "** は 0 個にも当たる");
        assert!(r.is_ignored("src/gen_a.cpp", false));
        assert!(!r.is_ignored("other/src/gen_a.cpp", false), "途中に / があれば基準から相対");
        assert!(!r.is_ignored("src/a.cpp", false));
    }

    #[test]
    fn unsupported_lines_are_skipped_not_applied() {
        assert!(parse_line("[ab].txt").is_none());
        assert!(parse_line("a\\ b").is_none());
        assert!(parse_line("   ").is_none());
    }

    #[test]
    fn nested_gitignore_is_relative_to_its_directory() {
        let mut r = rules("");
        r.loaded.insert("sub".to_string(), parse_gitignore("/only_here.txt\n*.tmp\n"));
        assert!(r.is_ignored("sub/only_here.txt", false));
        assert!(!r.is_ignored("only_here.txt", false));
        assert!(!r.is_ignored("sub/deeper/only_here.txt", false));
        assert!(r.is_ignored("sub/deeper/a.tmp", false));
        assert!(!r.is_ignored("other/a.tmp", false));
    }
}
