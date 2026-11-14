//! リネームのまとめ (architecture.md §5)。同じ窓の「削除 + 同じ内容ハッシュの作成」を 1 件の `renamed` にする。
//!
//! 別のディレクトリへの移動は監視に `Remove` + `Create` でしか届かない (試作 3) ので、これが主たる手段になる。
//! 内容が違えば、推測でつながずに別々の削除と作成のまま残す。

use std::collections::{BTreeMap, BTreeSet};

use crate::observe::{ChangeType, Completeness, Observation};

/// 同じ窓・同じ内容ハッシュの削除と作成を `renamed` にまとめる。観測の順序は保つ
/// (まとめた観測は作成の位置に置く)。
///
/// 1. 削除 1 件と作成 1 件なら、パスは問わずつなぐ (名前も変えた移動を拾う)。
/// 2. どちらかが 2 件以上 (同じ内容のファイルが複数) なら候補が一意に決まらない。末尾のパスが同じ
///    (`dir1/x` と `dir2/x`) 対が互いに 1 件ずつだけのものを、つなぐ。
/// 3. 1・2 でつないだ対から、ディレクトリの対応 (`dir1/` → `dir2/`) を作る。それでも残った削除は、
///    対応で移した先のパスに同じ内容の作成がちょうど 1 件あるときだけつなぐ。
/// どれにも当たらないものは別々の削除と作成のまま。
pub fn merge_renames(observations: Vec<Observation>) -> Vec<Observation> {
    // 窓と内容ハッシュごとの (削除の位置, 作成の位置)。BTreeMap = 走査順を決定的にする
    let mut groups: BTreeMap<(&str, &str), (Vec<usize>, Vec<usize>)> = BTreeMap::new();
    for (index, o) in observations.iter().enumerate() {
        match o.change_type {
            ChangeType::Deleted => {
                if let Some(hash) = o.before.as_ref().and_then(|s| s.hash.as_deref()) {
                    groups.entry((o.window.as_str(), hash)).or_default().0.push(index);
                }
            }
            ChangeType::Added => {
                if let Some(after) = &o.after {
                    groups.entry((o.window.as_str(), after.hash.as_str())).or_default().1.push(index);
                }
            }
            ChangeType::Modified | ChangeType::Renamed => {}
        }
    }

    // 作成の位置 → 対になる削除の位置
    let mut pairs: BTreeMap<usize, usize> = BTreeMap::new();
    let mut directory_moves: BTreeMap<&str, BTreeSet<(String, String)>> = BTreeMap::new();
    let mut ambiguous: Vec<(&str, &Vec<usize>, &Vec<usize>)> = Vec::new();
    for (&(window, _), (deleted, added)) in &groups {
        if deleted.is_empty() || added.is_empty() {
            continue;
        }
        let mut linked_any = false;
        if deleted.len() == 1 && added.len() == 1 {
            pairs.insert(added[0], deleted[0]);
            linked_any = true;
        } else {
            for &d in deleted {
                let mut candidates = added.iter().filter(|&&a| shares_tail(&observations[d].path, &observations[a].path));
                let (Some(&a), None) = (candidates.next(), candidates.next()) else { continue };
                let back = deleted.iter().filter(|&&other| shares_tail(&observations[other].path, &observations[a].path)).count();
                if back == 1 {
                    pairs.insert(a, d);
                    linked_any = true;
                }
            }
        }
        if linked_any {
            for (&a, &d) in pairs.iter().filter(|(a, _)| added.contains(a)) {
                if let Some(mapping) = directory_move(&observations[d].path, &observations[a].path) {
                    directory_moves.entry(window).or_default().insert(mapping);
                }
            }
        }
        if deleted.len() > 1 || added.len() > 1 {
            ambiguous.push((window, deleted, added));
        }
    }
    // 3: ディレクトリの対応で、残った削除の行き先を決める
    for (window, deleted, added) in ambiguous {
        let Some(moves) = directory_moves.get(window) else { continue };
        for &d in deleted {
            if pairs.values().any(|&paired| paired == d) {
                continue;
            }
            let predicted: BTreeSet<String> =
                moves.iter().filter_map(|(from, to)| move_path(&observations[d].path, from, to)).collect();
            let mut hits = added.iter().filter(|&&a| !pairs.contains_key(&a) && predicted.contains(&observations[a].path));
            if let (Some(&a), None) = (hits.next(), hits.next()) {
                pairs.insert(a, d);
            }
        }
    }
    if pairs.is_empty() {
        return observations;
    }

    let mut slots: Vec<Option<Observation>> = observations.into_iter().map(Some).collect();
    let mut merged: BTreeMap<usize, Observation> = BTreeMap::new();
    for (&a, &d) in &pairs {
        let deleted = slots[d].take().expect("each deletion is paired once");
        let added = slots[a].take().expect("each creation is paired once");
        merged.insert(a, merge_pair(deleted, added));
    }
    let mut out = Vec::with_capacity(slots.len());
    for (index, slot) in slots.into_iter().enumerate() {
        match slot {
            Some(o) => out.push(o),
            // まとめた観測は作成の位置へ。削除の位置は空になる
            None => out.extend(merged.remove(&index)),
        }
    }
    out
}

/// 末尾のパス要素が同じで、パスは違う。
fn shares_tail(a: &str, b: &str) -> bool {
    a != b && a.rsplit('/').next() == b.rsplit('/').next()
}

/// 末尾の共通のパス要素を除いた手前 (ディレクトリ側) の対。`d1/x` と `d2/x` なら (`d1`, `d2`)。
/// 共通の末尾が無い、またはどちらかの手前が空 (ルート) なら None。
fn directory_move(from: &str, to: &str) -> Option<(String, String)> {
    let (a, b): (Vec<&str>, Vec<&str>) = (from.split('/').collect(), to.split('/').collect());
    let common = a.iter().rev().zip(b.iter().rev()).take_while(|(x, y)| x == y).count();
    if common == 0 || common >= a.len() || common >= b.len() {
        return None;
    }
    Some((a[..a.len() - common].join("/"), b[..b.len() - common].join("/")))
}

/// `path` が `from/` の下なら、`to/` の下へ移したパス。
fn move_path(path: &str, from: &str, to: &str) -> Option<String> {
    path.strip_prefix(from).and_then(|rest| rest.strip_prefix('/')).map(|rest| format!("{to}/{rest}"))
}

fn merge_pair(deleted: Observation, added: Observation) -> Observation {
    let source = if deleted.source.is_gap() { deleted.source } else { added.source };
    Observation {
        source,
        path: added.path,
        change_type: ChangeType::Renamed,
        old_path: Some(deleted.path),
        before: deleted.before,
        after: added.after,
        completeness: weaker(deleted.completeness, added.completeness),
        window: added.window,
        observed_at: added.observed_at.max(deleted.observed_at),
    }
}

/// 2 つの観測のうち、中間状態を見ていない度合いが大きい方 (gap > coalesced > no_before > complete)。
fn weaker(a: Completeness, b: Completeness) -> Completeness {
    let rank = |c: Completeness| match c {
        Completeness::Complete => 0,
        Completeness::NoBefore => 1,
        Completeness::Coalesced => 2,
        Completeness::Gap => 3,
    };
    if rank(a) >= rank(b) { a } else { b }
}
