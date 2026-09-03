// porcelain v2 (-z) の解析テスト。
// **fixture はバイト列を手で組む** — 実 git を呼ぶと「この環境の git が出す形」しか
// 検査できず、リネーム (2 レコード) や競合 (u レコード) のような**作るのが面倒な形**が
// 永久に未検査のまま残る。形の正本は git-status.txt (v2.11.0) の表。

use mye_collab::porcelain::parse_status_v2;

/// NUL 区切りのレコード列を組む (末尾も NUL — git の -z 出力と同じ)
fn nul(records: &[&str]) -> Vec<u8> {
    let mut out = Vec::new();
    for r in records {
        out.extend_from_slice(r.as_bytes());
        out.push(0);
    }
    out
}

const HH: &str = "1111111111111111111111111111111111111111";
const HI: &str = "2222222222222222222222222222222222222222";

fn full_fixture() -> Vec<u8> {
    nul(&[
        "# branch.oid 3333333333333333333333333333333333333333",
        "# branch.head feature/collab",
        "# branch.upstream origin/feature/collab",
        "# branch.ab +2 -1",
        // M: worktree だけ変更
        &format!("1 .M N... 100644 100644 100644 {HH} {HI} assets/textures/test.png"),
        // A: index に追加 (パスに空白を含める = splitn の境界検査)
        &format!("1 A. N... 000000 100644 100644 0000000000000000000000000000000000000000 {HI} assets/new file.txt"),
        // D: index から削除
        &format!("1 D. N... 100644 000000 000000 {HH} 0000000000000000000000000000000000000000 assets/gone.json"),
        // R: リネーム。-z では旧パスが**次のレコード**として来る
        &format!("2 R. N... 100644 100644 100644 {HH} {HI} R100 assets/scenes/new.scene.json"),
        "assets/scenes/old.scene.json",
        // u: 未マージ (both modified)
        &format!("u UU N... 100644 100644 100644 100644 {HH} {HI} 3333333333333333333333333333333333333333 assets/conflict.json"),
        // ?: 未追跡
        "? assets/untracked.txt",
        // !: 無視 (--ignored を付けたときだけ来る。来ても落ちないこと)
        "! cache/build.log",
    ])
}

#[test]
fn parses_branch_headers() {
    let info = parse_status_v2(&full_fixture());
    assert_eq!(info.oid, "3333333333333333333333333333333333333333");
    assert_eq!(info.branch, "feature/collab");
    assert_eq!(info.upstream, "origin/feature/collab");
    assert_eq!(info.ahead, 2);
    assert_eq!(info.behind, 1);
}

#[test]
fn parses_six_entry_kinds() {
    let info = parse_status_v2(&full_fixture());
    assert_eq!(info.entries.len(), 7, "M / A / D / R / u / ? / ! の 7 件");

    let m = &info.entries[0];
    assert_eq!(m.path, "assets/textures/test.png");
    assert_eq!((m.index, m.worktree), ('.', 'M'));
    assert!(!m.conflict);
    assert_eq!(m.old_path, None);

    let a = &info.entries[1];
    assert_eq!(a.path, "assets/new file.txt", "パス中の空白が切れない");
    assert_eq!((a.index, a.worktree), ('A', '.'));

    let d = &info.entries[2];
    assert_eq!(d.path, "assets/gone.json");
    assert_eq!((d.index, d.worktree), ('D', '.'));

    let r = &info.entries[3];
    assert_eq!(r.path, "assets/scenes/new.scene.json");
    assert_eq!(r.old_path.as_deref(), Some("assets/scenes/old.scene.json"));
    assert_eq!((r.index, r.worktree), ('R', '.'));

    let u = &info.entries[4];
    assert_eq!(u.path, "assets/conflict.json");
    assert!(u.conflict, "u レコードは conflict=true");
    assert_eq!((u.index, u.worktree), ('U', 'U'));

    let q = &info.entries[5];
    assert_eq!(q.path, "assets/untracked.txt");
    assert_eq!((q.index, q.worktree), ('?', '?'));

    let ign = &info.entries[6];
    assert_eq!(ign.path, "cache/build.log");
    assert_eq!((ign.index, ign.worktree), ('!', '!'));
}

#[test]
fn rename_does_not_swallow_the_next_entry() {
    // リネームの旧パスを消費し損ねると、次のエントリが旧パスとして食われて
    // **1 件消える**。実際に踏むと「リネームした直後だけファイルが 1 個消える」
    let raw = nul(&[
        &format!("2 R. N... 100644 100644 100644 {HH} {HI} R100 b.txt"),
        "a.txt",
        "? c.txt",
    ]);
    let info = parse_status_v2(&raw);
    assert_eq!(info.entries.len(), 2);
    assert_eq!(info.entries[0].old_path.as_deref(), Some("a.txt"));
    assert_eq!(info.entries[1].path, "c.txt");
}

#[test]
fn unborn_branch_has_empty_oid_and_zero_ab() {
    // git init 直後 (コミット 0 件)。upstream が無いので branch.ab ヘッダ自体が出ない
    let raw = nul(&["# branch.oid (initial)", "# branch.head main", "? README.md"]);
    let info = parse_status_v2(&raw);
    assert_eq!(info.oid, "");
    assert_eq!(info.branch, "main");
    assert_eq!(info.upstream, "");
    assert_eq!((info.ahead, info.behind), (0, 0));
    assert_eq!(info.entries.len(), 1);
}

#[test]
fn clean_worktree_has_no_entries() {
    let raw = nul(&["# branch.oid 3333333333333333333333333333333333333333", "# branch.head main"]);
    let info = parse_status_v2(&raw);
    assert!(info.entries.is_empty());
}

#[test]
fn empty_input_does_not_panic() {
    assert!(parse_status_v2(b"").entries.is_empty());
    assert!(parse_status_v2(b"\0\0").entries.is_empty());
    // 種別だけで本体が無い壊れたレコード (途中で切れた出力) も落ちないこと
    assert!(parse_status_v2(b"1 .M\0? \0").entries.len() <= 1);
}

// ---- M66c: git log / diff の解析 ----
// fixture は**バイト列を手で組む** (parse_status_v2 と同じ方針)。実 git を呼ぶと
// 「この環境の git が返した並び」しか検査できず、端数レコードや非 UTF-8 の
// author 名のような**作るのが面倒な形**が永久に未検査で残る。

use mye_collab::porcelain::{clip_text, parse_log_z};

/// git log -z の実測形 (各レコードが NUL で終端される)
fn log_fixture() -> Vec<u8> {
    let mut out = Vec::new();
    for rec in [
        ("1111111111111111111111111111111111111111", "Alice", "2026-09-03T00:18:55+09:00", "M66c: stage and commit"),
        ("2222222222222222222222222222222222222222", "Bob Builder", "2026-09-02T12:00:00+09:00", "fixture: initial"),
    ] {
        out.extend_from_slice(rec.0.as_bytes());
        out.push(0);
        out.extend_from_slice(rec.1.as_bytes());
        out.push(0);
        out.extend_from_slice(rec.2.as_bytes());
        out.push(0);
        out.extend_from_slice(rec.3.as_bytes());
        out.push(0);
    }
    out
}

#[test]
fn parses_log_records() {
    let entries = parse_log_z(&log_fixture());
    assert_eq!(entries.len(), 2, "終端 NUL の後ろの空要素をコミットに数えない");
    assert_eq!(entries[0].sha, "1111111111111111111111111111111111111111");
    assert_eq!(entries[0].author, "Alice");
    assert_eq!(entries[0].date, "2026-09-03T00:18:55+09:00");
    assert_eq!(entries[0].subject, "M66c: stage and commit");
    // author に空白が入っても 4 フィールドの切り出しは NUL 基準なので壊れない
    assert_eq!(entries[1].author, "Bob Builder");
    assert_eq!(entries[1].subject, "fixture: initial");
}

#[test]
fn log_is_empty_for_empty_output() {
    assert!(parse_log_z(b"").is_empty(), "commit 0 件 = 空配列 (エラーではない)");
    assert!(parse_log_z(b"\0").is_empty(), "NUL だけの出力も空");
}

#[test]
fn log_drops_a_partial_trailing_record() {
    // 途中で切れた出力 (3 フィールドしか無い) から半端なコミットを作らないこと
    let mut raw = log_fixture();
    raw.extend_from_slice(b"3333333333333333333333333333333333333333\0Carol\0");
    let entries = parse_log_z(&raw);
    assert_eq!(entries.len(), 2, "端数レコードは捨てる");
}

#[test]
fn log_keeps_an_empty_subject() {
    // subject が空のコミット (git commit --allow-empty-message) でも 1 件として数える
    let raw = b"4444444444444444444444444444444444444444\0Dave\02026-09-03T00:00:00+09:00\0\0";
    let entries = parse_log_z(raw);
    assert_eq!(entries.len(), 1);
    assert_eq!(entries[0].subject, "");
}

#[test]
fn clip_text_leaves_short_text_alone() {
    let (text, truncated) = clip_text("diff --git a/x b/x", 1024);
    assert_eq!(text, "diff --git a/x b/x");
    assert!(!truncated);
}

#[test]
fn clip_text_never_splits_a_multibyte_char() {
    // "日" は 3 バイト。上限 4 は 2 文字目の途中 = 文字境界まで戻して 1 文字で切る
    let (text, truncated) = clip_text("日本語", 4);
    assert!(truncated);
    assert_eq!(text, "日", "UTF-8 の途中で切ると String が panic する");
    // 上限 0 でも panic しない (境界まで戻すと 0 になる)
    let (empty, truncated0) = clip_text("日本語", 0);
    assert!(truncated0);
    assert_eq!(empty, "");
}

// ---- M66d: --name-status -z ----

#[test]
fn name_status_parses_plain_records() {
    let raw = b"M\0assets/a.png\0A\0assets/b.png\0D\0assets/c.png\0";
    let out = mye_collab::porcelain::parse_name_status_z(raw);
    assert_eq!(out.len(), 3);
    assert_eq!(out[0].status, 'M');
    assert_eq!(out[0].path, "assets/a.png");
    assert_eq!(out[2].status, 'D');
    assert!(out[0].old_path.is_none());
}

#[test]
fn name_status_consumes_three_records_for_a_rename() {
    // ★R だけ 3 レコード。2 レコードと決め打つと**以降が全部ずれる**
    let raw = b"R100\0old/x.png\0new/x.png\0M\0assets/z.png\0";
    let out = mye_collab::porcelain::parse_name_status_z(raw);
    assert_eq!(out.len(), 2, "{out:?}");
    assert_eq!(out[0].status, 'R');
    assert_eq!(out[0].old_path.as_deref(), Some("old/x.png"));
    assert_eq!(out[0].path, "new/x.png");
    assert_eq!(out[1].status, 'M', "リネームの後ろがずれていないこと");
    assert_eq!(out[1].path, "assets/z.png");
}

#[test]
fn name_status_ignores_a_truncated_tail() {
    // 途中で切れた出力で panic しないこと (worker ごと dead 化する)
    let out = mye_collab::porcelain::parse_name_status_z(b"M\0");
    assert!(out.is_empty());
    assert!(mye_collab::porcelain::parse_name_status_z(b"").is_empty());
}

// ---- M66e: for-each-ref / would be overwritten / ls-tree -z ----

#[test]
fn for_each_ref_splits_five_fields() {
    // `-z` が無いのでレコードは LF、フィールドだけ NUL
    let raw = b"refs/heads/main\0main\0aaa\0origin/main\0*\n\
                refs/heads/feature\0feature\0bbb\0\0 \n\
                refs/remotes/origin/main\0origin/main\0aaa\0\0 \n";
    let out = mye_collab::porcelain::parse_for_each_ref(raw);
    assert_eq!(out.len(), 3, "{out:?}");
    assert!(out[0].current, "%(HEAD) が * の行だけが current");
    assert_eq!(out[0].upstream, "origin/main");
    assert!(!out[1].current);
    assert_eq!(out[1].upstream, "");
    assert!(out[2].refname.starts_with("refs/remotes/"),
            "ローカル / リモートの判別は短縮名ではなく完全な ref 名で行う");
}

#[test]
fn for_each_ref_drops_short_records() {
    // フィールドが欠けた行は捨てる (捨てないと空名のブランチが一覧に出る)
    let raw = b"refs/heads/main\0main\n\n";
    assert!(mye_collab::porcelain::parse_for_each_ref(raw).is_empty());
}

#[test]
fn overwritten_paths_takes_only_the_indented_block() {
    // ★案内文の文言は git の版で変わる。当てにしているのは
    //   「見出し行 → タブ字下げの並び」という形だけ
    let stderr = "error: Your local changes to the following files would be overwritten by \
                  checkout:\n\tassets/a.png\n\tassets/scenes/main.scene.json\n\
                  Please commit your changes or stash them before you switch branches.\nAborting";
    let out = mye_collab::porcelain::parse_overwritten_paths(stderr);
    assert_eq!(out, vec!["assets/a.png".to_string(),
                         "assets/scenes/main.scene.json".to_string()]);
}

#[test]
fn overwritten_paths_handles_the_untracked_wording_and_crlf() {
    let stderr = "error: The following untracked working tree files would be overwritten by \
                  checkout:\r\n\tassets/new.png\r\nPlease move or remove them.\r\nAborting";
    assert_eq!(mye_collab::porcelain::parse_overwritten_paths(stderr),
               vec!["assets/new.png".to_string()]);
}

#[test]
fn overwritten_paths_is_empty_for_other_errors() {
    // 関係の無い stderr から**でっち上げない** (でっち上げると「これを破棄すれば
    // 直る」という嘘の案内がモーダルに出る)
    assert!(mye_collab::porcelain::parse_overwritten_paths(
        "error: pathspec 'nope' did not match any file(s) known to git").is_empty());
    assert!(mye_collab::porcelain::parse_overwritten_paths("").is_empty());
}

#[test]
fn z_paths_drops_the_trailing_empty_record() {
    let out = mye_collab::porcelain::parse_z_paths(b"a/b.png\0c.txt\0");
    assert_eq!(out, vec!["a/b.png".to_string(), "c.txt".to_string()]);
    assert!(mye_collab::porcelain::parse_z_paths(b"").is_empty());
}
