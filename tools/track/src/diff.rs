//! 行 diff (Myers 法) と Hunk (architecture.md §5、db-schema.md §3 の `change_hunks`)。
//!
//! 純粋な計算だけを持つ (DB・ファイルに触らない)。改行は LF / CRLF / 末尾なしをそのまま保持し、
//! Hunk の本文から Before と After をバイト単位で再構成できる (`apply_hunks` / `unapply_hunks`)。
//!
//! 座標: `old_start` / `new_start` は 1 始まりの行番号で、変更範囲の最初の行を指す。
//! 行が 0 件の側 (純粋な挿入・削除) は、その行が入る位置 (次の行の番号) を指す。

use std::collections::HashMap;
use std::fmt;

/// 文脈の行数 (前後それぞれ)。
pub const CONTEXT_LINES: usize = 3;

/// Myers 法の計算量の上限。超えたら「ファイル全体を 1 Hunk」に落とす (`LineDiff::whole_file`)。
/// 値の根拠: docs/ai-change-tracking/architecture.md §5。
#[derive(Clone, Debug)]
pub struct DiffLimits {
    /// 前後の共通部分を除いた Before と After の行数の合計。
    pub max_lines: usize,
    /// 編集距離 (削除と挿入の行数の合計)。トレースの記憶量が (値 + 1)² に比例する。
    pub max_edit_distance: usize,
    /// 対角線の走査と一致行の前進の合計回数。実行時間の上限で、時計は見ない (結果を決定的にする)。
    pub max_work: u64,
    /// Before と After の合計バイト数。
    pub max_bytes: usize,
}

impl Default for DiffLimits {
    fn default() -> Self {
        DiffLimits { max_lines: 2_000_000, max_edit_distance: 3_000, max_work: 30_000_000, max_bytes: 16 * 1024 * 1024 }
    }
}

/// 1 つの Hunk。`before_text` / `after_text` / `context_*` は改行込みのまま。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Hunk {
    pub old_start: usize,
    pub old_count: usize,
    pub new_start: usize,
    pub new_count: usize,
    pub before_text: String,
    pub after_text: String,
    pub context_pre: String,
    pub context_post: String,
    /// 全行が改行の違いだけ (本文は同じ)。
    pub eol_only: bool,
}

/// `diff_lines` の結果。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LineDiff {
    pub hunks: Vec<Hunk>,
    /// 上限を超えて、ファイル全体を 1 Hunk にした。
    pub whole_file: bool,
}

/// Hunk を当てられなかった。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApplyError {
    pub hunk_index: usize,
    pub reason: &'static str,
}

impl fmt::Display for ApplyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Hunk {} を当てられない: {}", self.hunk_index, self.reason)
    }
}

impl std::error::Error for ApplyError {}

/// 改行込みで行に分ける。区切りは LF だけ (CRLF の CR は行の末尾に残る)。
fn split_lines(text: &str) -> Vec<&str> {
    text.split_inclusive('\n').collect()
}

/// 行末の LF / CRLF を除いた本文。単独の CR は本文として扱う。
fn strip_eol(line: &str) -> &str {
    line.strip_suffix("\r\n").or_else(|| line.strip_suffix('\n')).unwrap_or(line)
}

/// 変更のあった行の範囲 (0 始まり、半開区間の長さ表現)。
#[derive(Clone, Copy, Debug)]
struct Region {
    old_start: usize,
    old_len: usize,
    new_start: usize,
    new_len: usize,
}

impl Region {
    fn old_end(&self) -> usize {
        self.old_start + self.old_len
    }
    fn new_end(&self) -> usize {
        self.new_start + self.new_len
    }
}

/// Before と After の行 diff。
pub fn diff_lines(before: &str, after: &str, limits: &DiffLimits) -> LineDiff {
    if before == after {
        return LineDiff { hunks: Vec::new(), whole_file: false };
    }
    let old = split_lines(before);
    let new = split_lines(after);
    let old_keys: Vec<&str> = old.iter().map(|l| strip_eol(l)).collect();
    let new_keys: Vec<&str> = new.iter().map(|l| strip_eol(l)).collect();
    if before.len() + after.len() > limits.max_bytes {
        return whole_file_diff(&old, &new);
    }
    let Some(matches) = align(&old_keys, &new_keys, limits) else {
        return whole_file_diff(&old, &new);
    };
    let regions = regions_from_matches(&old, &new, &matches);
    LineDiff { hunks: build_hunks(&old, &new, &regions), whole_file: false }
}

/// 本文 (改行を除いた行) が一致する行の対 (Before の行, After の行) を昇順に返す。
/// 上限を超えたら None。
fn align(old_keys: &[&str], new_keys: &[&str], limits: &DiffLimits) -> Option<Vec<(usize, usize)>> {
    let (n, m) = (old_keys.len(), new_keys.len());
    let mut prefix = 0;
    while prefix < n && prefix < m && old_keys[prefix] == new_keys[prefix] {
        prefix += 1;
    }
    let mut suffix = 0;
    while suffix < n - prefix && suffix < m - prefix && old_keys[n - 1 - suffix] == new_keys[m - 1 - suffix] {
        suffix += 1;
    }
    let mut matches: Vec<(usize, usize)> = (0..prefix).map(|i| (i, i)).collect();
    let (a, b) = (&old_keys[prefix..n - suffix], &new_keys[prefix..m - suffix]);
    if !a.is_empty() && !b.is_empty() {
        for (i, j) in myers(a, b, limits)? {
            matches.push((prefix + i, prefix + j));
        }
    }
    matches.extend((0..suffix).map(|i| (n - suffix + i, m - suffix + i)));
    Some(matches)
}

/// Myers の O(ND) 法。一致する行の対 (a の添字, b の添字) を昇順に返す。上限を超えたら None。
fn myers(a: &[&str], b: &[&str], limits: &DiffLimits) -> Option<Vec<(usize, usize)>> {
    let (n, m) = (a.len() as isize, b.len() as isize);
    if a.len() + b.len() > limits.max_lines {
        return None;
    }
    // 行を整数に置き換えて比較を速くする。番号の割り当て順は結果に影響しない (一致の判定にしか使わない)
    let mut table: HashMap<&str, u32> = HashMap::new();
    let ai: Vec<u32> = a.iter().map(|l| intern_line(&mut table, l)).collect();
    let bi: Vec<u32> = b.iter().map(|l| intern_line(&mut table, l)).collect();

    let dmax = (n + m).min(limits.max_edit_distance as isize);
    let off = dmax + 1;
    let mut v = vec![0isize; (2 * dmax + 3) as usize];
    // trace[d] = d 巡目の開始時点の v[-d..=d]
    let mut trace: Vec<Vec<i32>> = Vec::new();
    let mut work: u64 = 0;
    for d in 0..=dmax {
        trace.push(v[(off - d) as usize..=(off + d) as usize].iter().map(|&x| x as i32).collect());
        let mut k = -d;
        while k <= d {
            let idx = (k + off) as usize;
            let mut x = if k == -d || (k != d && v[idx - 1] < v[idx + 1]) { v[idx + 1] } else { v[idx - 1] + 1 };
            let mut y = x - k;
            work += 1;
            while x < n && y < m && ai[x as usize] == bi[y as usize] {
                x += 1;
                y += 1;
                work += 1;
            }
            v[idx] = x;
            if x >= n && y >= m {
                return Some(backtrack(&trace, n, m, d));
            }
            k += 2;
        }
        if work > limits.max_work {
            return None;
        }
    }
    None
}

fn intern_line<'a>(table: &mut HashMap<&'a str, u32>, line: &'a str) -> u32 {
    let next = table.len() as u32;
    *table.entry(line).or_insert(next)
}

/// 末尾 (n, m) から d 巡目まで戻り、対角線 (一致行) だけを集めて昇順にする。
fn backtrack(trace: &[Vec<i32>], n: isize, m: isize, last_d: isize) -> Vec<(usize, usize)> {
    let (mut x, mut y) = (n, m);
    let mut out: Vec<(usize, usize)> = Vec::new();
    for d in (0..=last_d).rev() {
        let (prev_x, prev_y) = if d == 0 {
            (0, 0)
        } else {
            let vd = &trace[d as usize];
            let at = |k: isize| vd[(k + d) as usize] as isize;
            let k = x - y;
            let prev_k = if k == -d || (k != d && at(k - 1) < at(k + 1)) { k + 1 } else { k - 1 };
            let prev_x = at(prev_k);
            (prev_x, prev_x - prev_k)
        };
        while x > prev_x && y > prev_y {
            x -= 1;
            y -= 1;
            out.push((x as usize, y as usize));
        }
        x = prev_x;
        y = prev_y;
    }
    out.reverse();
    out
}

/// 一致行の対から、変更のあった範囲を作る。本文が同じで改行だけ違う対も 1 行の範囲にする。
/// 隣り合う範囲は 1 つにつなぐ。
fn regions_from_matches(old: &[&str], new: &[&str], matches: &[(usize, usize)]) -> Vec<Region> {
    let mut regions: Vec<Region> = Vec::new();
    let (mut oi, mut nj) = (0usize, 0usize);
    let sentinel = [(old.len(), new.len())];
    for (pos, &(i, j)) in matches.iter().chain(sentinel.iter()).enumerate() {
        if i > oi || j > nj {
            push_region(&mut regions, Region { old_start: oi, old_len: i - oi, new_start: nj, new_len: j - nj });
        }
        if pos < matches.len() {
            if old[i] != new[j] {
                push_region(&mut regions, Region { old_start: i, old_len: 1, new_start: j, new_len: 1 });
            }
            oi = i + 1;
            nj = j + 1;
        }
    }
    regions
}

fn push_region(regions: &mut Vec<Region>, next: Region) {
    if let Some(last) = regions.last_mut() {
        if last.old_end() == next.old_start && last.new_end() == next.new_start {
            last.old_len += next.old_len;
            last.new_len += next.new_len;
            return;
        }
    }
    regions.push(next);
}

/// 範囲を Hunk にまとめる。間の同じ行が `2 * CONTEXT_LINES` 以下なら、文脈が重なるので 1 つの Hunk にする。
fn build_hunks(old: &[&str], new: &[&str], regions: &[Region]) -> Vec<Hunk> {
    let mut hunks = Vec::new();
    let mut group_start = 0;
    while group_start < regions.len() {
        let mut group_end = group_start;
        while group_end + 1 < regions.len()
            && regions[group_end + 1].old_start - regions[group_end].old_end() <= 2 * CONTEXT_LINES
        {
            group_end += 1;
        }
        let (first, last) = (regions[group_start], regions[group_end]);
        hunks.push(make_hunk(old, new, first.old_start, last.old_end(), first.new_start, last.new_end()));
        group_start = group_end + 1;
    }
    hunks
}

/// 範囲 (半開区間) から Hunk を作る。文脈は Before 側から取る (範囲の外は両側で同じ行)。
fn make_hunk(old: &[&str], new: &[&str], old_from: usize, old_to: usize, new_from: usize, new_to: usize) -> Hunk {
    let pre_from = old_from.saturating_sub(CONTEXT_LINES);
    let post_to = (old_to + CONTEXT_LINES).min(old.len());
    let before_lines = &old[old_from..old_to];
    let after_lines = &new[new_from..new_to];
    let eol_only = !before_lines.is_empty()
        && before_lines.len() == after_lines.len()
        && before_lines.iter().zip(after_lines).all(|(b, a)| strip_eol(b) == strip_eol(a));
    Hunk {
        old_start: old_from + 1,
        old_count: old_to - old_from,
        new_start: new_from + 1,
        new_count: new_to - new_from,
        before_text: before_lines.concat(),
        after_text: after_lines.concat(),
        context_pre: old[pre_from..old_from].concat(),
        context_post: old[old_to..post_to].concat(),
        eol_only,
    }
}

/// 上限を超えたときの結果: 全体を 1 Hunk にする。
fn whole_file_diff(old: &[&str], new: &[&str]) -> LineDiff {
    // 範囲が全体なので文脈は空になる
    LineDiff { hunks: vec![make_hunk(old, new, 0, old.len(), 0, new.len())], whole_file: true }
}

/// Hunk の列を Before に当てて After を得る。本文と文脈が合わなければ Err (黙って壊さない)。
pub fn apply_hunks(before: &str, hunks: &[Hunk]) -> Result<String, ApplyError> {
    replay(before, hunks, Direction::Forward)
}

/// Hunk の列を After に逆向きに当てて Before を得る。
pub fn unapply_hunks(after: &str, hunks: &[Hunk]) -> Result<String, ApplyError> {
    replay(after, hunks, Direction::Backward)
}

#[derive(Clone, Copy)]
enum Direction {
    Forward,
    Backward,
}

fn replay(input: &str, hunks: &[Hunk], direction: Direction) -> Result<String, ApplyError> {
    let lines = split_lines(input);
    let mut out = String::with_capacity(input.len());
    let mut cursor = 0usize;
    for (index, hunk) in hunks.iter().enumerate() {
        let fail = |reason: &'static str| ApplyError { hunk_index: index, reason };
        let (start, count, expected, replacement) = match direction {
            Direction::Forward => (hunk.old_start, hunk.old_count, &hunk.before_text, &hunk.after_text),
            Direction::Backward => (hunk.new_start, hunk.new_count, &hunk.after_text, &hunk.before_text),
        };
        let start = start.checked_sub(1).ok_or_else(|| fail("行番号が 0"))?;
        if start < cursor || start + count > lines.len() {
            return Err(fail("範囲が入力の外、または前の Hunk と重なる"));
        }
        let pre_from = start.saturating_sub(CONTEXT_LINES);
        let post_to = (start + count + CONTEXT_LINES).min(lines.len());
        if lines[pre_from..start].concat() != hunk.context_pre {
            return Err(fail("前の文脈が合わない"));
        }
        if lines[start + count..post_to].concat() != hunk.context_post {
            return Err(fail("後ろの文脈が合わない"));
        }
        if lines[start..start + count].concat() != *expected {
            return Err(fail("本文が合わない"));
        }
        out.push_str(&lines[cursor..start].concat());
        out.push_str(replacement);
        cursor = start + count;
    }
    out.push_str(&lines[cursor..].concat());
    Ok(out)
}
