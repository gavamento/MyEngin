//! 帰属の結論 (`conclude`)、宣言された編集の適用、`.git` の読み取りの試験 (M104g)。
//! 表の 9 行のそれぞれと、上の行が下の行に勝つことを、入力だけで確かめる (DB も監視も要らない)。

use std::fs;
use std::path::PathBuf;

use mye_track::attribution::{conclude, Conclusion, Evidence, Kind};
use mye_track::declared_edit::DeclaredEdit;
use mye_track::gitref::{base_commit, is_linked_worktree};
use serde_json::json;

const S1: &str = "session-1";
const S2: &str = "session-2";
const OP1: &str = "op-1";
const OP2: &str = "op-2";

fn ev(kind: Kind, session: Option<&str>, op: Option<&str>) -> Evidence {
    Evidence::new(kind, session, op)
}

fn verdict(evidence: &[Evidence]) -> (&'static str, &'static str, Vec<String>) {
    let Conclusion { actor, attribution, candidates, .. } = conclude(evidence);
    (actor, attribution, candidates)
}

fn ai(attribution: &'static str, candidates: &[&str]) -> (&'static str, &'static str, Vec<String>) {
    ("ai", attribution, candidates.iter().map(|s| s.to_string()).collect())
}

const UNKNOWN: &str = "unknown";

// 表の 9 行: 1 editor_saved / 2 restore_expected / 3 preexisting / 4 declared_edit_match / 5 worktree /
// 6 report_hash_match または op_window + before_match / 7 report_only / 8 time_near だけ / 9 どれも無い

#[test]
fn row_1_editor_saved_is_the_editor_user_verified() {
    let e = [ev(Kind::EditorSaved, None, None)];
    assert_eq!(verdict(&e), ("editor_user", "verified", vec![]));
}

#[test]
fn row_2_restore_expected_is_restore_verified() {
    let e = [ev(Kind::RestoreExpected, None, None)];
    assert_eq!(verdict(&e), ("restore", "verified", vec![]));
}

#[test]
fn row_3_preexisting_is_unknown_even_with_a_session_running() {
    let e = [ev(Kind::Preexisting, None, None), ev(Kind::TimeNear, Some(S1), None)];
    assert_eq!(verdict(&e), (UNKNOWN, UNKNOWN, vec![]));
}

#[test]
fn row_4_a_declared_edit_match_is_ai_verified() {
    let e = [
        ev(Kind::OpWindow, Some(S1), Some(OP1)),
        ev(Kind::BeforeMatch, Some(S1), Some(OP1)),
        ev(Kind::DeclaredEditMatch, Some(S1), Some(OP1)),
    ];
    let c = conclude(&e);
    assert_eq!((c.actor, c.attribution), ("ai", "verified"));
    assert_eq!(c.candidates, [S1]);
    assert_eq!(c.operation.as_deref(), Some(OP1), "候補が 1 つなら、その操作に結び付く");
}

#[test]
fn row_5_a_single_worktree_session_is_ai_verified() {
    let e = [ev(Kind::Worktree, Some(S1), None)];
    assert_eq!(verdict(&e), ai("verified", &[S1]));
    // 作業ツリーを使うセッションが 2 つなら Verified にしない
    let two = [ev(Kind::Worktree, Some(S1), None), ev(Kind::Worktree, Some(S2), None)];
    assert_ne!(verdict(&two).1, "verified");
}

#[test]
fn row_6_report_hash_match_or_window_with_before_match_is_claimed() {
    let report = [ev(Kind::ReportHashMatch, Some(S1), Some(OP1))];
    assert_eq!(verdict(&report), ai("claimed", &[S1]));
    let window = [ev(Kind::OpWindow, Some(S1), Some(OP1)), ev(Kind::BeforeMatch, Some(S1), Some(OP1))];
    assert_eq!(verdict(&window), ai("claimed", &[S1]));
    // 窓だけ (Before が合わない = 人が挟まった) では claimed にならない
    let window_only = [ev(Kind::OpWindow, Some(S1), Some(OP1))];
    assert_eq!(verdict(&window_only), (UNKNOWN, UNKNOWN, vec![]));
    // 窓と Before の一致が別々のセッションなら、どちらも満たしていない
    let split = [ev(Kind::OpWindow, Some(S1), Some(OP1)), ev(Kind::BeforeMatch, Some(S2), Some(OP2))];
    assert_eq!(verdict(&split), (UNKNOWN, UNKNOWN, vec![]));
}

#[test]
fn row_7_report_only_is_claimed() {
    let e = [ev(Kind::ReportOnly, Some(S1), Some(OP1))];
    assert_eq!(verdict(&e), ai("claimed", &[S1]));
}

#[test]
fn row_8_time_near_alone_is_inferred() {
    let e = [ev(Kind::TimeNear, Some(S1), None)];
    assert_eq!(verdict(&e), ai("inferred", &[S1]));
}

#[test]
fn row_9_nothing_is_unknown() {
    assert_eq!(verdict(&[]), (UNKNOWN, UNKNOWN, vec![]));
    // 観測だけでは帰属しない
    assert_eq!(verdict(&[ev(Kind::Observed, None, None)]), (UNKNOWN, UNKNOWN, vec![]));
}

#[test]
fn an_upper_row_wins_when_two_rows_hold_at_once() {
    let declared = ev(Kind::DeclaredEditMatch, Some(S1), Some(OP1));
    let window = ev(Kind::OpWindow, Some(S1), Some(OP1));
    let before = ev(Kind::BeforeMatch, Some(S1), Some(OP1));
    let report = ev(Kind::ReportHashMatch, Some(S1), Some(OP1));
    let report_only = ev(Kind::ReportOnly, Some(S1), Some(OP1));
    let near = ev(Kind::TimeNear, Some(S1), None);

    // 1 が 2〜9 に勝つ / 2 が 3〜9 に勝つ / 3 が 4〜9 に勝つ
    let all_lower = [declared.clone(), ev(Kind::Worktree, Some(S1), None), report.clone(), report_only.clone(), near.clone()];
    let with = |top: Evidence| -> Vec<Evidence> { std::iter::once(top).chain(all_lower.clone()).collect() };
    assert_eq!(verdict(&with(ev(Kind::EditorSaved, None, None))).0, "editor_user");
    assert_eq!(verdict(&with(ev(Kind::RestoreExpected, None, None))).0, "restore");
    let editor_over_restore = [ev(Kind::EditorSaved, None, None), ev(Kind::RestoreExpected, None, None)];
    assert_eq!(verdict(&editor_over_restore).0, "editor_user");
    assert_eq!(verdict(&with(ev(Kind::Preexisting, None, None))), (UNKNOWN, UNKNOWN, vec![]));
    // 4 (declared) が 5 (worktree) より優先: 候補はどちらのセッションか
    let declared_over_worktree = [ev(Kind::Worktree, Some(S2), None), declared.clone()];
    assert_eq!(verdict(&declared_over_worktree), ai("verified", &[S1]));
    // 5 (worktree) が 6 (claimed) より優先
    let worktree_over_claimed = [window.clone(), before.clone(), report.clone(), ev(Kind::Worktree, Some(S2), None)];
    assert_eq!(verdict(&worktree_over_claimed), ai("verified", &[S2]));
    // 6 が 7 と 8 に勝つ / 7 が 8 に勝つ
    assert_eq!(verdict(&[report.clone(), report_only.clone(), near.clone()]).1, "claimed");
    assert_eq!(verdict(&[report_only, near.clone()]).1, "claimed");
    assert_eq!(verdict(&[window, before, near]).1, "claimed");
}

#[test]
fn a_session_known_to_predate_the_change_is_not_a_candidate() {
    // preexisting がセッションに付いているとき、そのセッションの根拠は使わない (他のセッションの根拠は残る)
    let e = [ev(Kind::Preexisting, Some(S1), None), ev(Kind::TimeNear, Some(S1), None), ev(Kind::TimeNear, Some(S2), None)];
    assert_eq!(verdict(&e), ai("inferred", &[S2]));
    let only = [ev(Kind::Preexisting, Some(S1), None), ev(Kind::ReportOnly, Some(S1), None)];
    assert_eq!(verdict(&only), (UNKNOWN, UNKNOWN, vec![]));
}

#[test]
fn overlapping_windows_never_verify_and_keep_both_candidates() {
    let overlap = ev(Kind::Overlap, None, None);
    let w1 = ev(Kind::OpWindow, Some(S1), Some(OP1));
    let w2 = ev(Kind::OpWindow, Some(S2), Some(OP2));
    let b1 = ev(Kind::BeforeMatch, Some(S1), Some(OP1));
    let b2 = ev(Kind::BeforeMatch, Some(S2), Some(OP2));
    let d1 = ev(Kind::DeclaredEditMatch, Some(S1), Some(OP1));

    // 宣言の照合が一方にだけ成り立つ: 候補はそのセッション 1 つ、確かさは claimed
    let one_declared = [overlap.clone(), w1.clone(), w2.clone(), b1.clone(), b2.clone(), d1.clone()];
    let c = conclude(&one_declared);
    assert_eq!((c.actor, c.attribution), ("ai", "claimed"));
    assert_eq!(c.candidates, [S1]);
    assert_eq!(c.operation.as_deref(), Some(OP1));

    // どちらにも成り立たない: 両方が候補、actor は ai、attribution は unknown
    let neither = [overlap.clone(), w1.clone(), w2.clone(), b1.clone(), b2.clone()];
    assert_eq!(verdict(&neither), ai("unknown", &[S1, S2]));
    assert_eq!(conclude(&neither).operation, None, "候補が 2 つなら操作に結び付けない");

    // 両方に成り立つ: 絞れない
    let d2 = ev(Kind::DeclaredEditMatch, Some(S2), Some(OP2));
    let both = [overlap.clone(), w1.clone(), w2, d1, d2];
    assert_eq!(verdict(&both), ai("unknown", &[S1, S2]));

    // 申告のハッシュ一致が一方にだけあれば絞れるが claimed
    let report = [overlap, w1, ev(Kind::OpWindow, Some(S2), Some(OP2)), ev(Kind::ReportHashMatch, Some(S2), Some(OP2))];
    assert_eq!(verdict(&report), ai("claimed", &[S2]));
}

#[test]
fn several_candidates_without_overlap_are_not_claimed_for_either() {
    let e = [ev(Kind::ReportHashMatch, Some(S1), None), ev(Kind::ReportHashMatch, Some(S2), None)];
    assert_eq!(verdict(&e), ai("unknown", &[S1, S2]));
}

// ---- 宣言された編集 -----------------------------------------------------------------------

fn replace(old: &str, new: &str, all: bool) -> DeclaredEdit {
    DeclaredEdit::Replace { old: old.to_string(), new: new.to_string(), replace_all: all }
}

#[test]
fn declared_replace_follows_the_counting_rules() {
    let before = b"a b a c\n";
    // 1 件: 置換
    assert_eq!(replace("c", "C", false).apply(Some(before)).unwrap(), b"a b a C\n");
    // 0 件: 当てられない
    assert_eq!(replace("zzz", "x", false).apply(Some(before)), None);
    assert_eq!(replace("zzz", "x", true).apply(Some(before)), None);
    // 2 件以上で replaceAll が偽: 当てられない。真なら全部
    assert_eq!(replace("a", "A", false).apply(Some(before)), None);
    assert_eq!(replace("a", "A", true).apply(Some(before)).unwrap(), b"A b A c\n");
    // 空の oldString、Before が無い、UTF-8 でない Before: 当てられない
    assert_eq!(replace("", "x", true).apply(Some(before)), None);
    assert_eq!(replace("a", "x", false).apply(None), None);
    assert_eq!(replace("a", "x", false).apply(Some(&[0xff, 0xfe])), None);
}

#[test]
fn declared_replace_keeps_line_endings_and_unicode_as_bytes() {
    let before = "一行目\r\n二行目\r\n".as_bytes();
    let after = replace("二行目", "二🦀目", false).apply(Some(before)).unwrap();
    assert_eq!(after, "一行目\r\n二🦀目\r\n".as_bytes(), "CRLF を変換しない");
    // 改行を含む oldString は、字面どおりに数える (LF と CRLF は別)
    assert_eq!(replace("一行目\n", "x", false).apply(Some(before)), None);
}

#[test]
fn declared_write_is_the_content_itself_and_round_trips_through_json() {
    let write = DeclaredEdit::Write { content: "new\n".to_string() };
    assert_eq!(write.apply(None).unwrap(), b"new\n");
    assert_eq!(write.apply(Some(b"old")).unwrap(), b"new\n");
    for edit in [write, replace("a", "b", true)] {
        assert_eq!(DeclaredEdit::from_json(&edit.to_json()).unwrap(), edit);
    }
    assert!(DeclaredEdit::from_json(&json!({ "kind": "replace", "oldString": "a" })).is_err());
    assert!(DeclaredEdit::from_json(&json!({ "kind": "explode" })).is_err());
}

// ---- .git の読み取り ----------------------------------------------------------------------

struct TempDir(PathBuf);

impl TempDir {
    fn new(name: &str) -> TempDir {
        let dir = std::env::temp_dir().join(format!("mye_track_git_{}_{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        TempDir(dir)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

const ID_A: &str = "0123456789abcdef0123456789abcdef01234567";
const ID_B: &str = "fedcba9876543210fedcba9876543210fedcba98";

#[test]
fn base_commit_reads_a_loose_ref() {
    let t = TempDir::new("loose");
    fs::create_dir_all(t.0.join(".git/refs/heads")).unwrap();
    fs::write(t.0.join(".git/HEAD"), "ref: refs/heads/main\n").unwrap();
    fs::write(t.0.join(".git/refs/heads/main"), format!("{ID_A}\n")).unwrap();
    assert_eq!(base_commit(&t.0).as_deref(), Some(ID_A));
    assert!(!is_linked_worktree(&t.0));
}

#[test]
fn base_commit_falls_back_to_packed_refs() {
    let t = TempDir::new("packed");
    fs::create_dir_all(t.0.join(".git")).unwrap();
    fs::write(t.0.join(".git/HEAD"), "ref: refs/heads/dev\n").unwrap();
    fs::write(t.0.join(".git/packed-refs"), format!("# pack-refs\n{ID_B} refs/heads/other\n{ID_A} refs/heads/dev\n^{ID_B}\n")).unwrap();
    assert_eq!(base_commit(&t.0).as_deref(), Some(ID_A));
}

#[test]
fn base_commit_of_a_detached_head_is_the_hash_itself() {
    let t = TempDir::new("detached");
    fs::create_dir_all(t.0.join(".git")).unwrap();
    fs::write(t.0.join(".git/HEAD"), format!("{ID_B}\n")).unwrap();
    assert_eq!(base_commit(&t.0).as_deref(), Some(ID_B));
}

#[test]
fn base_commit_of_a_non_git_directory_is_none() {
    let t = TempDir::new("nogit");
    assert_eq!(base_commit(&t.0), None);
    assert!(!is_linked_worktree(&t.0));
    // HEAD が壊れていても落ちない
    fs::create_dir_all(t.0.join(".git")).unwrap();
    fs::write(t.0.join(".git/HEAD"), "garbage").unwrap();
    assert_eq!(base_commit(&t.0), None);
}

#[test]
fn base_commit_follows_gitdir_and_commondir_of_a_linked_worktree() {
    let t = TempDir::new("linked");
    // 本体 <t>\main\.git、作業ツリー <t>\wt (.git = ファイル)
    let main_git = t.0.join("main/.git");
    let wt_git = main_git.join("worktrees/wt");
    fs::create_dir_all(main_git.join("refs/heads")).unwrap();
    fs::create_dir_all(&wt_git).unwrap();
    fs::create_dir_all(t.0.join("wt")).unwrap();
    fs::write(main_git.join("refs/heads/feature"), format!("{ID_A}\n")).unwrap();
    fs::write(wt_git.join("HEAD"), "ref: refs/heads/feature\n").unwrap();
    fs::write(wt_git.join("commondir"), "../..\n").unwrap();
    fs::write(t.0.join("wt/.git"), format!("gitdir: {}\n", wt_git.display())).unwrap();
    let root = t.0.join("wt");
    assert!(is_linked_worktree(&root));
    assert_eq!(base_commit(&root).as_deref(), Some(ID_A));
    // 相対パスの gitdir も読める
    fs::write(root.join(".git"), "gitdir: ../main/.git/worktrees/wt\n").unwrap();
    assert_eq!(base_commit(&root).as_deref(), Some(ID_A));
}

#[test]
fn a_submodule_git_file_is_not_a_linked_worktree() {
    let t = TempDir::new("submodule");
    // サブモジュール: `.git` はファイルで gitdir: を持つが、`modules` の下を指す
    fs::create_dir_all(t.0.join("super/.git/modules/sub")).unwrap();
    fs::create_dir_all(t.0.join("super/sub")).unwrap();
    fs::write(t.0.join("super/sub/.git"), "gitdir: ../.git/modules/sub\n").unwrap();
    assert!(!is_linked_worktree(&t.0.join("super/sub")));
}

#[test]
fn no_git_process_is_started_by_the_attribution_code() {
    // 試験の間に git のプロセスが起動されない: コードに `Command` が無いことを字面で確かめる
    let src = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("src");
    for name in ["gitref.rs", "session.rs", "operation.rs", "attribution.rs", "declared_edit.rs", "ops_record.rs"] {
        let text = fs::read_to_string(src.join(name)).unwrap();
        for line in text.lines().filter(|l| !l.trim_start().starts_with("//")) {
            assert!(!line.contains("Command"), "{name} がプロセスを起動する: {line}");
        }
    }
}
