// 監視フィルタ (M66b) と、実際に notify が起きるかの往復。
//
// ★フィルタは**純関数**なので大半はここで済む。実 FS を触るのは 1 本だけ
//   (ReadDirectoryChangesW が本当に届くか = 記憶ではなく観測で確かめたい部分)。

use std::path::Path;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use mye_collab::watch::{is_interesting, relative_key, spawn};

#[test]
fn git_internals_are_filtered_to_the_five_interesting_paths() {
    // 拾うもの: HEAD / index / refs/** / MERGE_HEAD / rebase-*
    assert!(is_interesting(".git/HEAD"));
    assert!(is_interesting(".git/index"));
    assert!(is_interesting(".git/MERGE_HEAD"));
    assert!(is_interesting(".git/refs/heads/main"));
    assert!(is_interesting(".git/refs/remotes/origin/main"));
    assert!(is_interesting(".git/rebase-merge/done"));
    assert!(is_interesting(".git/rebase-apply/next"));

    // 捨てるもの: git が動くたびに数百件書き換わる場所。
    // ★index.lock を拾うと「status を走らせる → git が index.lock を作る →
    //   また status を走らせる」の自励振動になる
    assert!(!is_interesting(".git/index.lock"));
    assert!(!is_interesting(".git/objects/ab/cdef0123"));
    assert!(!is_interesting(".git/logs/HEAD"));
    assert!(!is_interesting(".git/FETCH_HEAD"));
    assert!(!is_interesting(".git/ORIG_HEAD"));
    assert!(!is_interesting(".git"));
}

#[test]
fn generated_directories_are_ignored_but_similar_names_are_not() {
    assert!(!is_interesting("cache/collab_verify/x"));
    assert!(!is_interesting(".mye/editor_settings.json"));
    assert!(!is_interesting("dist/game.exe"));
    assert!(!is_interesting("target/release/mye_collab.dll"));

    // 名前が似ているだけの**追跡対象**を巻き込まないこと
    assert!(is_interesting(".github/workflows/ci.yml"), ".git/ の前方一致で落ちてはいけない");
    assert!(is_interesting("caches/note.txt"));
    assert!(is_interesting("assets/target_dummy.png"));
    assert!(is_interesting("assets/textures/test.png"));
    assert!(is_interesting("project.mye.json"));
}

#[test]
fn empty_and_root_relative_paths_are_not_interesting() {
    assert!(!is_interesting(""));
    assert!(!is_interesting("/"));
}

#[test]
fn non_ascii_paths_survive_the_filter() {
    // core.quotepath=false で git が返す生 UTF-8 のまま判定できること
    assert!(is_interesting("assets/textures/日本語.png"));
    assert!(!is_interesting("cache/日本語.png"));
}

#[test]
fn relative_key_is_case_insensitive_and_separator_agnostic() {
    let root = Path::new("C:\\HAL\\MyEngin\\cache\\fixture");
    assert_eq!(
        relative_key(root, Path::new("c:\\hal\\myengin\\cache\\fixture\\assets\\a.png")).as_deref(),
        Some("assets/a.png"),
        "ドライブ文字の綴り違いで判定を落とさない"
    );
    assert_eq!(relative_key(root, Path::new("C:\\HAL\\MyEngin\\cache\\fixture")).as_deref(), Some(""));
    // root の外は None (呼び出し側が「拾う」に倒す)
    assert_eq!(relative_key(root, Path::new("C:\\Windows\\notepad.exe")), None);
}

#[test]
fn watcher_fires_for_a_real_file_write_and_stops_on_drop() {
    let dir = std::env::temp_dir().join(format!("mye_collab_watch_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(dir.join("assets")).unwrap();
    std::fs::create_dir_all(dir.join("cache")).unwrap();

    let hits = Arc::new(AtomicUsize::new(0));
    let hits_cb = Arc::clone(&hits);
    let handle = spawn(&dir, move || {
        hits_cb.fetch_add(1, Ordering::SeqCst);
    })
    .expect("watcher must start on an existing directory");

    // 監視の登録が終わるまで待たずに書くと最初の 1 件を落とすことがある
    std::thread::sleep(Duration::from_millis(200));
    std::fs::write(dir.join("assets/a.txt"), b"hello").unwrap();

    let deadline = Instant::now() + Duration::from_secs(10);
    while hits.load(Ordering::SeqCst) == 0 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(20));
    }
    assert_eq!(hits.load(Ordering::SeqCst), 1, "デバウンス後にちょうど 1 回");

    // 無視対象への書き込みでは起きないこと (ここが緩むと status が走りっぱなしになる)
    std::fs::write(dir.join("cache/b.txt"), b"noise").unwrap();
    std::thread::sleep(Duration::from_millis(900)); // DEBOUNCE (300 ms) の 3 倍待つ
    assert_eq!(hits.load(Ordering::SeqCst), 1, "cache\\ への書き込みは拾わない");

    drop(handle); // 停止 + join。ここで固まったら stop フラグが見られていない
    let before = hits.load(Ordering::SeqCst);
    std::fs::write(dir.join("assets/c.txt"), b"after stop").unwrap();
    std::thread::sleep(Duration::from_millis(600));
    assert_eq!(hits.load(Ordering::SeqCst), before, "drop 後は二度と起きない");

    let _ = std::fs::remove_dir_all(&dir);
}
