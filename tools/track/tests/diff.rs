//! 行 diff・Hunk・リネームのまとめの単体テスト (M104e)。DB・ファイルには触らない。

use std::time::{Duration, Instant, SystemTime};

use mye_track::baseline::{ContentKind, FileState};
use mye_track::diff::{apply_hunks, diff_lines, unapply_hunks, DiffLimits, Hunk, CONTEXT_LINES};
use mye_track::observe::{AfterContent, ChangeType, Completeness, Observation, Source};
use mye_track::rename::merge_renames;

fn diff(before: &str, after: &str) -> Vec<Hunk> {
    diff_lines(before, after, &DiffLimits::default()).hunks
}

/// Hunk の列が、Before / After を正確に再構成でき、座標の約束 (diff.rs の冒頭) を守っている。
fn check_hunks(before: &str, after: &str, hunks: &[Hunk]) {
    assert_eq!(apply_hunks(before, hunks).as_deref(), Ok(after), "Before に当てた結果が After と違う");
    assert_eq!(unapply_hunks(after, hunks).as_deref(), Ok(before), "After に逆適用した結果が Before と違う");
    let old: Vec<&str> = before.split_inclusive('\n').collect();
    let mut shift: isize = 0;
    let mut cursor = 0usize;
    for (i, h) in hunks.iter().enumerate() {
        assert_eq!(h.before_text.split_inclusive('\n').count(), h.old_count, "hunk {i} の old_count");
        assert_eq!(h.after_text.split_inclusive('\n').count(), h.new_count, "hunk {i} の new_count");
        assert!(h.old_start - 1 >= cursor, "hunk {i} が前の Hunk と重なる");
        assert_eq!((h.old_start as isize + shift) as usize, h.new_start, "hunk {i} の new_start");
        assert!(h.context_pre.split_inclusive('\n').count() <= CONTEXT_LINES);
        assert!(h.context_post.split_inclusive('\n').count() <= CONTEXT_LINES);
        let pre_from = (h.old_start - 1).saturating_sub(CONTEXT_LINES);
        assert_eq!(h.context_pre, old[pre_from..h.old_start - 1].concat(), "hunk {i} の前の文脈");
        let end = h.old_start - 1 + h.old_count;
        assert_eq!(h.context_post, old[end..(end + CONTEXT_LINES).min(old.len())].concat(), "hunk {i} の後ろの文脈");
        assert_ne!(h.before_text, h.after_text, "変更の無い Hunk");
        shift += h.new_count as isize - h.old_count as isize;
        cursor = end;
    }
}

fn numbered(n: usize) -> Vec<String> {
    (1..=n).map(|i| format!("line{i:02}\n")).collect()
}

#[test]
fn three_distant_changes_make_three_hunks_with_exact_ranges_and_context() {
    let mut after = numbered(40);
    for i in [5, 20, 35] {
        after[i - 1] = format!("CHANGED{i}\n");
    }
    let (before, after) = (numbered(40).concat(), after.concat());
    let hunks = diff(&before, &after);
    check_hunks(&before, &after, &hunks);
    assert_eq!(hunks.len(), 3);
    for (h, line) in hunks.iter().zip([5usize, 20, 35]) {
        assert_eq!((h.old_start, h.old_count, h.new_start, h.new_count), (line, 1, line, 1));
        assert_eq!(h.before_text, format!("line{line:02}\n"));
        assert_eq!(h.after_text, format!("CHANGED{line}\n"));
        assert_eq!(h.context_pre, format!("line{:02}\nline{:02}\nline{:02}\n", line - 3, line - 2, line - 1));
        assert_eq!(h.context_post, format!("line{:02}\nline{:02}\nline{:02}\n", line + 1, line + 2, line + 3));
        assert!(!h.eol_only);
    }
}

#[test]
fn context_is_shorter_at_the_edges_of_the_file() {
    let mut after = numbered(10);
    after[0] = "FIRST\n".into();
    after[9] = "LAST\n".into();
    let (before, after) = (numbered(10).concat(), after.concat());
    let hunks = diff(&before, &after);
    check_hunks(&before, &after, &hunks);
    assert_eq!(hunks.len(), 2);
    assert_eq!(hunks[0].context_pre, "");
    assert_eq!(hunks[0].context_post, "line02\nline03\nline04\n");
    assert_eq!(hunks[1].context_pre, "line07\nline08\nline09\n");
    assert_eq!(hunks[1].context_post, "");
}

#[test]
fn changes_closer_than_twice_the_context_share_a_hunk() {
    // 間の同じ行が 6 行以下なら 1 つ、7 行なら 2 つ
    for (gap, hunks_expected) in [(0usize, 1usize), (6, 1), (7, 2)] {
        let mut after = numbered(30);
        after[9] = "A\n".into();
        after[9 + gap + 1] = "B\n".into();
        let (before, after) = (numbered(30).concat(), after.concat());
        let hunks = diff(&before, &after);
        check_hunks(&before, &after, &hunks);
        assert_eq!(hunks.len(), hunks_expected, "gap {gap}");
    }
}

#[test]
fn boundary_inputs_round_trip() {
    let cases: &[(&str, &str)] = &[
        ("", ""),
        ("", "a\nb\n"),
        ("a\nb\n", ""),
        ("one", "two"),
        ("one\n", "one"),
        ("one", "one\n"),
        ("a\nb\nc", "a\nb\nc\n"),
        ("a\r\nb\nc\r\n", "a\nb\r\nc\r\n"),
        ("a\r\nb\r\n", "a\nb\n"),
        ("a\nb\nc\nd\n", "w\nx\ny\nz\n"),
        ("keep1\nkeep2\nold\n", "keep1\nkeep2\nnew\n"),
        ("old\nkeep1\nkeep2\n", "new\nkeep1\nkeep2\n"),
        ("日本語\n絵文字🦀\nend\n", "日本語\n絵文字🦀🦀\nend\n"),
        ("x\r", "x\r\n"),
        ("\n\n\n", "\n\n"),
        ("a\n\n\nb\n", "a\n\nb\n"),
    ];
    for (before, after) in cases {
        let hunks = diff(before, after);
        check_hunks(before, after, &hunks);
        assert_eq!(hunks.is_empty(), before == after, "{before:?} → {after:?}");
    }
}

#[test]
fn empty_to_content_and_back_are_single_hunks() {
    let hunks = diff("", "a\nb\n");
    assert_eq!(hunks.len(), 1);
    assert_eq!((hunks[0].old_start, hunks[0].old_count, hunks[0].new_start, hunks[0].new_count), (1, 0, 1, 2));
    assert_eq!(hunks[0].before_text, "");
    let hunks = diff("a\nb\n", "");
    assert_eq!(hunks.len(), 1);
    assert_eq!((hunks[0].old_start, hunks[0].old_count, hunks[0].new_start, hunks[0].new_count), (1, 2, 1, 0));
    assert_eq!(hunks[0].after_text, "");
}

#[test]
fn missing_final_newline_alone_is_an_eol_only_hunk() {
    let hunks = diff("a\nb", "a\nb\n");
    assert_eq!(hunks.len(), 1);
    assert_eq!((hunks[0].old_start, hunks[0].old_count), (2, 1));
    assert!(hunks[0].eol_only);
}

#[test]
fn crlf_to_lf_only_is_a_single_eol_only_hunk() {
    let before = "a\r\nb\r\nc\r\n";
    let after = "a\nb\nc\n";
    let hunks = diff(before, after);
    check_hunks(before, after, &hunks);
    assert_eq!(hunks.len(), 1);
    assert!(hunks[0].eol_only, "全行が改行だけの違いなので eol_only");
    assert_eq!((hunks[0].old_count, hunks[0].new_count), (3, 3));
}

#[test]
fn crlf_to_lf_stays_cheap_for_a_big_file() {
    // 本文が同じなので、行の置き換えとして Myers に掛けない (全体 1 Hunk に落ちない)
    let before: String = (0..30_000).map(|i| format!("line {i}\r\n")).collect();
    let after: String = (0..30_000).map(|i| format!("line {i}\n")).collect();
    let started = Instant::now();
    let result = diff_lines(&before, &after, &DiffLimits::default());
    assert!(!result.whole_file);
    assert_eq!(result.hunks.len(), 1);
    assert!(result.hunks[0].eol_only);
    assert!(started.elapsed() < Duration::from_secs(10), "{:?}", started.elapsed());
}

#[test]
fn body_change_mixed_with_eol_change_is_not_eol_only() {
    let before = "a\r\nb\r\nc\r\n";
    let after = "a\nB\nc\n";
    let hunks = diff(before, after);
    check_hunks(before, after, &hunks);
    assert_eq!(hunks.len(), 1);
    assert!(!hunks[0].eol_only);
}

#[test]
fn distant_eol_only_and_body_hunks_are_classified_separately() {
    let mut before: Vec<String> = numbered(40);
    before[0] = "line01\r\n".into();
    let mut after = numbered(40);
    after[39] = "BODY\n".into();
    let (before, after) = (before.concat(), after.concat());
    let hunks = diff(&before, &after);
    check_hunks(&before, &after, &hunks);
    assert_eq!(hunks.len(), 2);
    assert!(hunks[0].eol_only);
    assert!(!hunks[1].eol_only);
}

/// 固定 seed の PRNG (splitmix64)。テストの再現性のため、外部の乱数に頼らない。
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

const TOKENS: &[&str] = &["alpha", "beta", "gamma", "", "日本語の行", "絵文字🦀🎉", "x", "  indented", "}", "{", "tab\there"];

fn random_text(rng: &mut Rng) -> Vec<(String, &'static str)> {
    let lines = rng.below(40);
    (0..lines)
        .map(|_| {
            let token = TOKENS[rng.below(TOKENS.len() as u64) as usize].to_string();
            let eol = if rng.below(5) == 0 { "\r\n" } else { "\n" };
            (token, eol)
        })
        .collect()
}

fn render(lines: &[(String, &'static str)], last_has_eol: bool) -> String {
    let mut out = String::new();
    for (i, (body, eol)) in lines.iter().enumerate() {
        out.push_str(body);
        if i + 1 < lines.len() || last_has_eol {
            out.push_str(eol);
        }
    }
    out
}

fn mutate(rng: &mut Rng, lines: &[(String, &'static str)]) -> Vec<(String, &'static str)> {
    let mut out = Vec::new();
    for line in lines {
        match rng.below(20) {
            0..=1 => {}                                                 // 削除
            2..=3 => out.push((format!("{}!", line.0), line.1)),        // 本文の変更
            4 => out.push((line.0.clone(), if line.1 == "\n" { "\r\n" } else { "\n" })), // 改行だけ
            5..=6 => {
                out.push((TOKENS[rng.below(TOKENS.len() as u64) as usize].to_string(), "\n")); // 挿入
                out.push(line.clone());
            }
            _ => out.push(line.clone()),
        }
    }
    out
}

#[test]
fn property_hunks_rebuild_both_sides_byte_for_byte() {
    let mut rng = Rng(0x5EED_0000_0000_0104);
    for case in 0..4000 {
        let base = random_text(&mut rng);
        let changed = mutate(&mut rng, &base);
        let before = render(&base, rng.below(4) != 0);
        let after = render(&changed, rng.below(4) != 0);
        let hunks = diff(&before, &after);
        let context = format!("case {case}: {before:?} → {after:?}");
        assert_eq!(apply_hunks(&before, &hunks).as_deref(), Ok(after.as_str()), "{context}");
        assert_eq!(unapply_hunks(&after, &hunks).as_deref(), Ok(before.as_str()), "{context}");
        check_hunks(&before, &after, &hunks);
    }
}

#[test]
fn hunks_that_do_not_match_are_rejected() {
    let hunks = diff("a\nb\nc\n", "a\nX\nc\n");
    assert!(apply_hunks("a\nb\nc\n", &hunks).is_ok());
    assert!(apply_hunks("a\nY\nc\n", &hunks).is_err(), "本文が違う入力へは当てない");
    assert!(apply_hunks("z\nb\nc\n", &hunks).is_err(), "文脈が違う入力へは当てない");
    assert!(unapply_hunks("a\nb\nc\n", &hunks).is_err());
}

#[test]
fn input_over_the_limit_falls_back_to_one_whole_hunk_quickly() {
    // 共通する行が 1 行も無い大きな入力 (Myers の最悪)。既定の上限で時間内に返る
    let before: String = (0..100_000).map(|i| format!("old line {i}\n")).collect();
    let after: String = (0..100_000).map(|i| format!("new line {i}\n")).collect();
    let started = Instant::now();
    let result = diff_lines(&before, &after, &DiffLimits::default());
    let elapsed = started.elapsed();
    assert!(result.whole_file);
    assert_eq!(result.hunks.len(), 1);
    assert_eq!((result.hunks[0].old_start, result.hunks[0].old_count), (1, 100_000));
    assert_eq!((result.hunks[0].new_start, result.hunks[0].new_count), (1, 100_000));
    assert_eq!(apply_hunks(&before, &result.hunks).as_deref(), Ok(after.as_str()));
    assert_eq!(unapply_hunks(&after, &result.hunks).as_deref(), Ok(before.as_str()));
    // デバッグビルドの上限付きの余裕。実測は architecture.md §5
    assert!(elapsed < Duration::from_secs(20), "{elapsed:?}");
}

#[test]
fn small_limits_force_the_fallback_and_large_ones_do_not() {
    let before: String = (0..200).map(|i| format!("l{i}\n")).collect();
    let after: String = (0..200).map(|i| if i % 3 == 0 { format!("x{i}\n") } else { format!("l{i}\n") }).collect();
    let tiny = DiffLimits { max_edit_distance: 10, ..DiffLimits::default() };
    assert!(diff_lines(&before, &after, &tiny).whole_file);
    let few_lines = DiffLimits { max_lines: 50, ..DiffLimits::default() };
    assert!(diff_lines(&before, &after, &few_lines).whole_file);
    let few_bytes = DiffLimits { max_bytes: 100, ..DiffLimits::default() };
    assert!(diff_lines(&before, &after, &few_bytes).whole_file);
    let little_work = DiffLimits { max_work: 100, ..DiffLimits::default() };
    assert!(diff_lines(&before, &after, &little_work).whole_file);
    assert!(!diff_lines(&before, &after, &DiffLimits::default()).whole_file);
}

// ---- リネームのまとめ ----

fn state(hash: &str) -> FileState {
    FileState { hash: Some(hash.into()), size: 1, snapshot_id: None, blob_hash: Some(hash.into()), kind: Some(ContentKind::Text), modified: None }
}

fn deleted(path: &str, hash: &str, window: &str) -> Observation {
    Observation {
        path: path.into(),
        change_type: ChangeType::Deleted,
        old_path: None,
        before: Some(state(hash)),
        after: None,
        completeness: Completeness::Complete,
        source: Source::Watch,
        window: window.into(),
        observed_at: 1,
    }
}

fn added(path: &str, hash: &str, window: &str) -> Observation {
    Observation {
        path: path.into(),
        change_type: ChangeType::Added,
        old_path: None,
        before: None,
        after: Some(AfterContent { bytes: vec![b'x'], hash: hash.into(), kind: ContentKind::Text, modified: None::<SystemTime> }),
        completeness: Completeness::Complete,
        source: Source::Watch,
        window: window.into(),
        observed_at: 2,
    }
}

fn summary(observations: &[Observation]) -> Vec<String> {
    observations
        .iter()
        .map(|o| match &o.old_path {
            Some(old) => format!("{} {old}->{}", o.change_type.as_str(), o.path),
            None => format!("{} {}", o.change_type.as_str(), o.path),
        })
        .collect()
}

#[test]
fn delete_plus_add_with_same_hash_in_one_window_becomes_one_rename() {
    let merged = merge_renames(vec![deleted("from/m.txt", "h1", "w1"), added("to/m2.txt", "h1", "w1"), added("other.txt", "h9", "w1")]);
    assert_eq!(summary(&merged), vec!["renamed from/m.txt->to/m2.txt", "added other.txt"]);
    assert_eq!(merged[0].before.as_ref().unwrap().hash.as_deref(), Some("h1"));
    assert_eq!(merged[0].after.as_ref().unwrap().hash, "h1");
}

#[test]
fn different_content_or_window_is_not_linked() {
    let merged = merge_renames(vec![deleted("a.txt", "h1", "w1"), added("b.txt", "h2", "w1")]);
    assert_eq!(summary(&merged), vec!["deleted a.txt", "added b.txt"], "内容が違えば別々");
    let merged = merge_renames(vec![deleted("a.txt", "h1", "w1"), added("b.txt", "h1", "w2")]);
    assert_eq!(summary(&merged), vec!["deleted a.txt", "added b.txt"], "窓が違えば別々");
}

#[test]
fn two_candidates_with_the_same_hash_are_not_linked() {
    // 削除 1 件に作成 2 件 (名前の手がかりも無い)
    let merged = merge_renames(vec![deleted("a.txt", "h1", "w1"), added("x/b.txt", "h1", "w1"), added("y/c.txt", "h1", "w1")]);
    assert_eq!(summary(&merged).len(), 3);
    assert!(merged.iter().all(|o| o.change_type != ChangeType::Renamed));
    // 削除 2 件に作成 1 件
    let merged = merge_renames(vec![deleted("a.txt", "h1", "w1"), deleted("b.txt", "h1", "w1"), added("c.txt", "h1", "w1")]);
    assert!(merged.iter().all(|o| o.change_type != ChangeType::Renamed));
}

#[test]
fn directory_rename_pairs_each_file() {
    let merged = merge_renames(vec![
        deleted("d1/a.txt", "ha", "w"),
        deleted("d1/b.txt", "hb", "w"),
        deleted("d1/sub/deep.txt", "hd", "w"),
        added("d2/a.txt", "ha", "w"),
        added("d2/b.txt", "hb", "w"),
        added("d2/sub/deep.txt", "hd", "w"),
    ]);
    assert_eq!(
        summary(&merged),
        vec!["renamed d1/a.txt->d2/a.txt", "renamed d1/b.txt->d2/b.txt", "renamed d1/sub/deep.txt->d2/sub/deep.txt"]
    );
}

#[test]
fn directory_rename_with_duplicate_content_pairs_by_path_only_when_unique() {
    // 同じ内容の 2 ファイル: ハッシュでは決まらないが、末尾のパスが 1 対 1 で対応する
    let merged = merge_renames(vec![
        deleted("d1/a.txt", "same", "w"),
        deleted("d1/b.txt", "same", "w"),
        added("d2/a.txt", "same", "w"),
        added("d2/b.txt", "same", "w"),
    ]);
    assert_eq!(summary(&merged), vec!["renamed d1/a.txt->d2/a.txt", "renamed d1/b.txt->d2/b.txt"]);

    // 対応するパスが無い: つながない
    let merged = merge_renames(vec![
        deleted("d1/a.txt", "same", "w"),
        deleted("d1/b.txt", "same", "w"),
        added("d2/c.txt", "same", "w"),
        added("d2/d.txt", "same", "w"),
    ]);
    assert!(merged.iter().all(|o| o.change_type != ChangeType::Renamed));

    // 末尾のパスが同じ候補が 2 つ: 決められない
    let merged = merge_renames(vec![
        deleted("p/x.txt", "same", "w"),
        deleted("q/x.txt", "same", "w"),
        added("r/x.txt", "same", "w"),
    ]);
    assert!(merged.iter().all(|o| o.change_type != ChangeType::Renamed));
}

#[test]
fn directory_mapping_from_a_unique_file_resolves_same_named_duplicates() {
    // `twin.txt` が 2 本 (同じ名前・同じ内容) あり、末尾のパスでは決まらない。
    // 内容が唯一の `a.txt` が `old/` → `new/` の対応を教えるので、パスで決まる
    let merged = merge_renames(vec![
        deleted("old/a.txt", "ha", "w"),
        deleted("old/twin.txt", "same", "w"),
        deleted("old/sub/twin.txt", "same", "w"),
        added("new/a.txt", "ha", "w"),
        added("new/twin.txt", "same", "w"),
        added("new/sub/twin.txt", "same", "w"),
    ]);
    assert_eq!(
        summary(&merged),
        vec!["renamed old/a.txt->new/a.txt", "renamed old/twin.txt->new/twin.txt", "renamed old/sub/twin.txt->new/sub/twin.txt"]
    );

    // 対応を教えるファイルが無ければ、つながない
    let merged = merge_renames(vec![
        deleted("old/twin.txt", "same", "w"),
        deleted("old/sub/twin.txt", "same", "w"),
        added("new/twin.txt", "same", "w"),
        added("new/sub/twin.txt", "same", "w"),
    ]);
    assert!(merged.iter().all(|o| o.change_type != ChangeType::Renamed));
}

#[test]
fn merged_rename_keeps_the_weaker_completeness_and_gap_source() {
    let mut gone = deleted("a.txt", "h", "w");
    gone.source = Source::StartupGap;
    gone.completeness = Completeness::Gap;
    let merged = merge_renames(vec![gone, added("b.txt", "h", "w")]);
    assert_eq!(merged.len(), 1);
    assert_eq!(merged[0].completeness, Completeness::Gap);
    assert_eq!(merged[0].source, Source::StartupGap);
}
