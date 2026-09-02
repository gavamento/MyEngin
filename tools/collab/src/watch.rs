// ファイル監視 (M66b)。toplevel を再帰監視して、意味のある変更だけを 300 ms
// デバウンスしてから worker に status の取り直しを頼む。
//
// なぜフィルタが要るか:
//   * `.git\` の中は git が動くたびに **数百件**書き換わる (objects/ / logs/ /
//     index.lock / FETCH_HEAD …)。素通しすると status が走りっぱなしになり、
//     その status がまた index を触って自分で自分を起こす。
//   * `cache\` `.mye\` `dist\` はエディタ自身が毎フレーム書く可能性がある置き場
//     (gitignore 済み)。`target\` は Rust のビルド生成物。
//   ★除外は**パス名だけ**で決める。`git check-ignore` は呼ばない — 呼ぶと
//     「変更を見つけるために git を起動する」ことになり、デバウンスの意味が消える。
//
// なぜ「拾えないより拾いすぎ」に倒すか: 拾いすぎたときの実害は status が 1 回
// 余分に走るだけ (結果が同じなら worker 側が通知を握り潰す)。拾い損ねると
// **一覧が永久に古いまま**になり、ユーザーには壊れているようにしか見えない。

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, RecvTimeoutError};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use notify::{RecommendedWatcher, RecursiveMode, Watcher};

/// 最後のイベントからこれだけ静かになったら status を取り直す
pub const DEBOUNCE: Duration = Duration::from_millis(300);
/// 最初のイベントからこれを超えたら、まだイベントが続いていても 1 回流す。
/// ★これが無いと「100 ms ごとに何かを書き続ける常駐プロセス」がいる環境で
///   デバウンスが永久にリセットされ、一覧が二度と更新されない
pub const MAX_HOLD: Duration = Duration::from_secs(2);
/// stop フラグを見に行く間隔 (Service の破棄で待たされる上限でもある)
const TICK: Duration = Duration::from_millis(50);

/// トップレベル相対のパス ('/' 区切り、大文字小文字は問わない) が
/// status の取り直しに値するか。**純関数** — テストはここを叩く
pub fn is_interesting(rel: &str) -> bool {
    let trimmed = rel.trim_start_matches('/').trim_end_matches('/');
    if trimmed.is_empty() {
        return false; // toplevel 自身のタイムスタンプ更新
    }
    let lower = trimmed.to_ascii_lowercase();
    if lower == ".git" {
        return false;
    }
    if let Some(inside) = lower.strip_prefix(".git/") {
        // HEAD が動いた / index が変わった / 参照が動いた / マージ・リベース中になった。
        // それ以外 (objects, logs, index.lock, FETCH_HEAD, ORIG_HEAD …) は無視
        return inside == "head"
            || inside == "index"
            || inside == "merge_head"
            || inside.starts_with("refs/")
            || inside.starts_with("rebase-merge/")
            // rebase-apply = am 型のリベース。repo_check がこちらも「リベース中」と
            // 見なしている以上、監視側だけ落とすと状態が食い違う
            || inside.starts_with("rebase-apply/");
    }
    let top = lower.split('/').next().unwrap_or("");
    // テンプレ .gitignore の 3 行 (/.mye/ /cache/ /dist/) + Rust のビルド出力
    !matches!(top, ".mye" | "cache" | "dist" | "target")
}

/// 絶対パスを toplevel 相対 ('/' 区切り) に落とす。
/// root の外にある / 綴りが違って判定できないときは None
/// (呼び出し側は None を「拾う」に倒す)。
pub fn relative_key(root: &Path, path: &Path) -> Option<String> {
    // ★std::path::strip_prefix は**大文字小文字を区別する**。Windows の通知は
    //   ディレクトリの登録名をそのまま返すので普段は一致するが、ドライブ文字の
    //   綴り違い (c:\ と C:\) で外れうる。ここは文字列比較で吸収する
    let root_key = root.to_string_lossy().replace('\\', "/").to_ascii_lowercase();
    let root_key = root_key.trim_end_matches('/');
    let path_key = path.to_string_lossy().replace('\\', "/").to_ascii_lowercase();
    let rest = path_key.strip_prefix(root_key)?;
    Some(rest.trim_start_matches('/').to_string())
}

/// 監視スレッドの寿命。drop で停止して join する
/// (Service より先に落とさないと、止まった worker へ通知を送りつけることになる)
pub struct WatchHandle {
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl Drop for WatchHandle {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

/// root を再帰監視する。**失敗しても None を返すだけ** — 監視が無くても
/// 窓の「更新」ボタンで status は取れるので、エディタを止める理由にはならない
pub fn spawn<F>(root: &Path, on_change: F) -> Option<WatchHandle>
where
    F: Fn() + Send + 'static,
{
    let root: PathBuf = root.to_path_buf();
    if !root.is_dir() {
        return None;
    }
    let stop = Arc::new(AtomicBool::new(false));
    let stop_thread = Arc::clone(&stop);
    let thread = std::thread::Builder::new()
        .name("mye_collab_watch".to_string())
        .spawn(move || run(&root, &stop_thread, on_change))
        .ok()?;
    Some(WatchHandle { stop, thread: Some(thread) })
}

fn run<F: Fn()>(root: &Path, stop: &AtomicBool, on_change: F) {
    let (tx, rx) = channel();
    let mut watcher: RecommendedWatcher = match notify::recommended_watcher(tx) {
        Ok(w) => w,
        Err(_) => return,
    };
    if watcher.watch(root, RecursiveMode::Recursive).is_err() {
        return;
    }
    let mut first: Option<Instant> = None; // 溜め始めた時刻 (MAX_HOLD の基準)
    let mut last: Option<Instant> = None;  // 最後に拾った時刻 (DEBOUNCE の基準)
    while !stop.load(Ordering::SeqCst) {
        match rx.recv_timeout(TICK) {
            Ok(Ok(ev)) => {
                let hit = ev.paths.iter().any(|p| match relative_key(root, p) {
                    Some(rel) => is_interesting(&rel),
                    None => true, // 判定できない = 拾う側に倒す
                });
                if hit {
                    let now = Instant::now();
                    if first.is_none() {
                        first = Some(now);
                    }
                    last = Some(now);
                }
            }
            // notify 側のエラー (オーバーフロー等) で監視をやめない。
            // 取りこぼしても次の変更で必ず起き直る
            Ok(Err(_)) => {}
            Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => break,
        }
        if let (Some(f), Some(l)) = (first, last) {
            if l.elapsed() >= DEBOUNCE || f.elapsed() >= MAX_HOLD {
                first = None;
                last = None;
                on_change();
            }
        }
    }
    // watcher を明示的に落としてからスレッドを抜ける (Drop 任せでも同じだが、
    // 「監視の停止がここで完了する」ことを読み手に見せる)
    drop(watcher);
}
