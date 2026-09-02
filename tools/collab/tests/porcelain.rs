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
