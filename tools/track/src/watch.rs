//! 追跡サービスの本体: ファイル監視スレッドと、その寿命 (architecture.md §3.1、§4)。
//!
//! `notify` のイベントをパスごとにデバウンスし、flush の時点で `Collector` が内容を見て観測にする。
//! 起動時はベースライン (最初の起動) か、停止していた間の突き合わせ (2 回目以降) を行う。

use std::collections::{BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, RecvTimeoutError, Sender};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use notify::event::{ModifyKind, RenameMode};
use notify::{Event, EventKind, RecommendedWatcher, RecursiveMode, Watcher};
use serde_json::json;

use crate::baseline::{load_from_db, LoadedState};
use crate::collector::{Collector, Deferred, DueItem, TrackerConfig};
use crate::db::now_ms;
use crate::ignore::under_always_ignored_dir;
use crate::observe::{record_gap, Source};
use crate::store::Store;
use crate::writer::Writer;

/// 届いた時刻 (監視のコールバックの中で付ける) 付きのイベント。処理が遅れても、書き込みの間隔を正しく測れる。
type Stamped = (Instant, notify::Result<Event>);

/// イベントが無いときに止まっているかを見に行く間隔の上限。
const MAX_WAIT: Duration = Duration::from_millis(50);

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Phase {
    Starting,
    Baselining,
    Watching,
    /// 監視を始められなかった (理由)。記録は残るが、変更は拾えない。
    Failed(String),
    Stopped,
}

/// `status` が返す値のもと (sub-05)。
#[derive(Clone, Debug)]
pub struct TrackerStatus {
    pub phase: Phase,
    /// 最初の起動のベースライン、または 2 回目以降の突き合わせの対象になったファイル数。
    pub baseline_files: usize,
    pub baseline_ms: u64,
    pub baseline_bytes: u64,
    pub first_run: bool,
    pub overflows: u32,
}

struct Shared {
    status: Mutex<TrackerStatus>,
    changed: Condvar,
}

impl Shared {
    fn update(&self, f: impl FnOnce(&mut TrackerStatus)) {
        f(&mut self.status.lock().unwrap());
        self.changed.notify_all();
    }
}

pub struct Tracker {
    writer: Arc<Writer>,
    shared: Arc<Shared>,
    stop: Arc<AtomicBool>,
    drop_events: Arc<AtomicBool>,
    inject: Sender<Stamped>,
    thread: Option<JoinHandle<()>>,
    stopped: bool,
}

impl Tracker {
    /// Store を Writer へ移し、監視スレッドを起動する。ベースラインはそのスレッドで取る (`wait_ready` で待てる)。
    pub fn start(root: &Path, store: Store, cfg: TrackerConfig) -> Tracker {
        let writer = Arc::new(Writer::spawn(store));
        let shared = Arc::new(Shared {
            status: Mutex::new(TrackerStatus {
                phase: Phase::Starting,
                baseline_files: 0,
                baseline_ms: 0,
                baseline_bytes: 0,
                first_run: false,
                overflows: 0,
            }),
            changed: Condvar::new(),
        });
        let stop = Arc::new(AtomicBool::new(false));
        let drop_events = Arc::new(AtomicBool::new(false));
        let (tx, rx) = channel::<Stamped>();

        let thread = {
            let mut collector = Collector::new(root.to_path_buf(), cfg.clone(), Arc::clone(&writer));
            let window = collector.new_window();
            let mut watch = WatchLoop {
                root: root.to_path_buf(),
                cfg: cfg.clone(),
                writer: Arc::clone(&writer),
                collector,
                window,
                window_last_event: Instant::now(),
                shared: Arc::clone(&shared),
                stop: Arc::clone(&stop),
                drop_events: Arc::clone(&drop_events),
                pending: HashMap::new(),
                last_from: None,
                last_good_ms: now_ms(),
                watcher: None,
            };
            let tx_watcher = tx.clone();
            std::thread::Builder::new()
                .name("mye-track-watch".into())
                .spawn(move || watch.run(tx_watcher, rx))
                .expect("watch thread")
        };
        Tracker { writer, shared, stop, drop_events, inject: tx, thread: Some(thread), stopped: false }
    }

    pub fn status(&self) -> TrackerStatus {
        self.shared.status.lock().unwrap().clone()
    }

    /// ベースライン (または停止中の突き合わせ) が終わり、監視に入る (または失敗する) まで待つ。
    pub fn wait_ready(&self, timeout: Duration) -> TrackerStatus {
        let deadline = Instant::now() + timeout;
        let mut guard = self.shared.status.lock().unwrap();
        while !matches!(guard.phase, Phase::Watching | Phase::Failed(_) | Phase::Stopped) {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            guard = self.shared.changed.wait_timeout(guard, left).unwrap().0;
        }
        guard.clone()
    }

    pub fn writer(&self) -> &Arc<Writer> {
        &self.writer
    }

    /// 試験用: `notify` のコールバックへ値を差し込む。`Err` や再走査フラグ付きのイベントで、
    /// 監視バッファのあふれの報告を模す (本物のあふれは notify 8 が Windows で報告しない。architecture.md §4)。
    #[doc(hidden)]
    pub fn inject_event(&self, item: notify::Result<Event>) {
        let _ = self.inject.send((Instant::now(), item));
    }

    /// 試験用: 届いたイベントを捨てる (取りこぼしを模す)。
    #[doc(hidden)]
    pub fn drop_incoming_events(&self, on: bool) {
        self.drop_events.store(on, Ordering::SeqCst);
    }

    /// 溜まっているデバウンス中の変更を処理してから止め、`service_stop` を残す。
    pub fn stop(mut self, reason: &str) {
        self.shutdown(reason);
    }

    fn shutdown(&mut self, reason: &str) {
        if self.stopped {
            return;
        }
        self.stopped = true;
        self.stop.store(true, Ordering::SeqCst);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
        let reason = reason.to_string();
        let _ = self.writer.call(move |store| {
            let at = now_ms();
            if let Err(e) = store.record_event(at, "service_stop", json!({ "reason": reason })) {
                eprintln!("[track] service_stop を残せない: {e}");
            }
        });
        self.shared.update(|s| s.phase = Phase::Stopped);
    }
}

impl Drop for Tracker {
    fn drop(&mut self) {
        self.shutdown("dropped");
    }
}

/// デバウンス中の 1 パス。
struct Pending {
    first: Instant,
    last_event: Instant,
    /// 書き込みの塊の数 (`burst_gap` より離れたイベントで区切る)。
    bursts: u32,
    /// 内容の変更を表す `Modify` の件数。1 回の書き込みは最大 2 件 (切り詰めと書き込み)。
    writes: u32,
    structural: bool,
    /// 待ち直す前のイベントで、複数回の書き込みと判断していた。
    carried_multiple: bool,
    /// 書き手が続いているかもしれない (または共有なしで開かれている) ので、この時刻にやり直す。
    retry_at: Option<Instant>,
    busy_since: Option<Instant>,
    rename_from: Option<String>,
    rename_to: Option<String>,
    window: String,
}

impl Pending {
    fn new(now: Instant, window: String) -> Self {
        Pending {
            window,
            first: now,
            last_event: now,
            bursts: 1,
            writes: 0,
            structural: false,
            carried_multiple: false,
            retry_at: None,
            busy_since: None,
            rename_from: None,
            rename_to: None,
        }
    }

    fn due(&self, now: Instant, cfg: &TrackerConfig) -> bool {
        if let Some(retry) = self.retry_at {
            return now >= retry;
        }
        now.duration_since(self.last_event) >= cfg.debounce || now.duration_since(self.first) >= cfg.max_hold
    }
}

struct WatchLoop {
    root: PathBuf,
    cfg: TrackerConfig,
    writer: Arc<Writer>,
    collector: Collector,
    shared: Arc<Shared>,
    stop: Arc<AtomicBool>,
    drop_events: Arc<AtomicBool>,
    pending: HashMap<String, Pending>,
    /// 直前のイベントが `Name(From)` だったときの旧パス。
    last_from: Option<String>,
    /// いまの観測の窓。前のイベントから `debounce` を超えて空いたら新しくする。
    window: String,
    window_last_event: Instant,
    /// 最後に取りこぼしていないと分かっている時刻 (あふれの区間の始まりに使う)。
    last_good_ms: i64,
    /// 監視スレッドの寿命の間だけ持つ。drop で OS の監視が止まる。
    watcher: Option<RecommendedWatcher>,
}

/// `root` からの相対パス ('/' 区切り、元の大文字小文字のまま)。外にある、またはルート自身なら None。
fn relative_to(root: &Path, path: &Path) -> Option<String> {
    let root_s = root.to_string_lossy().replace('\\', "/");
    let root_s = root_s.trim_end_matches('/');
    let path_s = path.to_string_lossy().replace('\\', "/");
    let head = path_s.get(..root_s.len())?;
    if !head.eq_ignore_ascii_case(root_s) || path_s.as_bytes().get(root_s.len()) != Some(&b'/') {
        return None;
    }
    let rel = path_s[root_s.len() + 1..].trim_end_matches('/');
    (!rel.is_empty()).then(|| rel.to_string())
}

impl WatchLoop {
    fn run(&mut self, tx_watcher: Sender<Stamped>, rx: std::sync::mpsc::Receiver<Stamped>) {
        if let Err(reason) = self.startup(tx_watcher) {
            self.shared.update(|s| s.phase = Phase::Failed(reason.clone()));
            eprintln!("[track] 監視を始められない: {reason}");
            return;
        }
        // 取りこぼさないよう、監視を始めた後にベースラインを取る。その間のイベントは rx に溜まっている
        let check_every = (self.cfg.debounce / 4).clamp(Duration::from_millis(1), Duration::from_millis(10));
        let mut last_check = Instant::now();
        while !self.stop.load(Ordering::SeqCst) {
            let timeout = if self.pending.is_empty() { MAX_WAIT } else { check_every.min(MAX_WAIT) };
            match rx.recv_timeout(timeout) {
                Ok((at, Ok(event))) => {
                    if !self.drop_events.load(Ordering::SeqCst) {
                        self.handle_event(event, at);
                    }
                }
                Ok((_, Err(e))) => self.handle_overflow(&format!("watcher error: {e}")),
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => break,
            }
            if last_check.elapsed() >= check_every {
                last_check = Instant::now();
                self.flush_due(false);
            }
        }
        // 止める前に、届いているイベントとデバウンス中の変更を処理する
        while let Ok((at, item)) = rx.try_recv() {
            if let Ok(event) = item {
                if !self.drop_events.load(Ordering::SeqCst) {
                    self.handle_event(event, at);
                }
            }
        }
        self.flush_due(true);
    }

    /// 起動の手順: 前回の状態を読む → `service_start` → 監視を始める → ベースライン / 突き合わせ。
    fn startup(&mut self, tx_watcher: Sender<Stamped>) -> Result<(), String> {
        self.shared.update(|s| s.phase = Phase::Baselining);
        let started = Instant::now();
        let loaded: LoadedState = self
            .writer
            .call(|store| match store.conn.as_ref().map(load_from_db) {
                Some(Ok(state)) => Some(state),
                Some(Err(e)) => {
                    eprintln!("[track] 前回の状態を読めない: {e}");
                    None
                }
                None => None,
            })
            .map_err(|e| e.to_string())?
            .unwrap_or(LoadedState { files: Default::default(), had_baseline: false, last_event_at: None });
        let start_at = now_ms();
        let first_run = !loaded.had_baseline;
        let root_text = self.root.to_string_lossy().into_owned();
        self.writer
            .call(move |store| {
                let payload = json!({
                    "project": root_text,
                    "pid": std::process::id(),
                    "version": env!("CARGO_PKG_VERSION"),
                    "first_run": first_run,
                });
                if let Err(e) = store.record_event(start_at, "service_start", payload) {
                    eprintln!("[track] service_start を残せない: {e}");
                }
            })
            .map_err(|e| e.to_string())?;

        let mut watcher: RecommendedWatcher =
            notify::recommended_watcher(move |res| {
                let _ = tx_watcher.send((Instant::now(), res));
            })
            .map_err(|e| format!("監視を作れない: {e}"))?;
        watcher.watch(&self.root, RecursiveMode::Recursive).map_err(|e| format!("監視を始められない: {e}"))?;
        self.watcher = Some(watcher);

        let stats = if loaded.had_baseline {
            self.collector.baseline.files = loaded.files;
            if let Some(last) = loaded.last_event_at {
                self.writer
                    .call(move |store| {
                        if let Err(e) = record_gap(store, "service_down", last, start_at.max(last), None) {
                            eprintln!("[track] service_down を残せない: {e}");
                        }
                    })
                    .map_err(|e| e.to_string())?;
            }
            self.collector.rescan(Source::StartupGap)
        } else {
            self.collector.scan_baseline()
        };
        let elapsed_ms = Collector::elapsed_since(started);
        let files_now = self.collector.baseline.files.len();
        let (scanned, bytes, changes) = (stats.files, stats.bytes, stats.changes);
        let done_at = now_ms();
        self.writer
            .call(move |store| {
                let payload = json!({
                    "files": scanned,
                    "tracked_files": files_now,
                    "bytes": bytes,
                    "changes": changes,
                    "elapsed_ms": elapsed_ms,
                    "first_run": first_run,
                });
                if let Err(e) = store.record_event(done_at, "baseline_complete", payload) {
                    eprintln!("[track] baseline_complete を残せない: {e}");
                }
            })
            .map_err(|e| e.to_string())?;
        self.last_good_ms = now_ms();
        self.shared.update(|s| {
            s.baseline_files = scanned;
            s.baseline_ms = elapsed_ms;
            s.baseline_bytes = bytes;
            s.first_run = first_run;
            s.phase = Phase::Watching;
        });
        Ok(())
    }

    /// 絶対パスをパスごとの待ち行列へ入れる。追跡の外 (ルート自身・固定の無視名の下) は None。
    fn touch(&mut self, path: &Path, structural: bool, write: bool, now: Instant) -> Option<String> {
        let rel = relative_to(&self.root, path)?;
        if under_always_ignored_dir(&rel) {
            return None;
        }
        let burst_gap = self.cfg.burst_gap;
        if now.saturating_duration_since(self.window_last_event) > self.cfg.debounce {
            self.window = self.collector.new_window();
        }
        self.window_last_event = now;
        let window = self.window.clone();
        let p = self.pending.entry(rel.clone()).or_insert_with(|| Pending::new(now, window));
        if now.saturating_duration_since(p.last_event) > burst_gap {
            p.bursts += 1;
        }
        p.last_event = now;
        p.writes += u32::from(write);
        p.structural |= structural;
        p.retry_at = None;
        Some(rel)
    }

    fn link_rename(&mut self, from: &str, to: &str) {
        if let Some(p) = self.pending.get_mut(from) {
            p.rename_to = Some(to.to_string());
        }
        if let Some(p) = self.pending.get_mut(to) {
            p.rename_from = Some(from.to_string());
        }
    }

    /// イベントの種別では変更かどうかを決めない。パスを待ち行列へ入れるだけ
    /// (リネームの `Name(From)` の直後の `Name(To)` だけは対にする)。
    fn handle_event(&mut self, event: Event, now: Instant) {
        if event.need_rescan() {
            self.handle_overflow("rescan flag");
            return;
        }
        self.last_good_ms = now_ms();
        let prev_from = self.last_from.take();
        match event.kind {
            EventKind::Create(_) | EventKind::Remove(_) => {
                for path in &event.paths {
                    self.touch(path, true, false, now);
                }
            }
            EventKind::Modify(ModifyKind::Name(RenameMode::From)) => {
                if let Some(path) = event.paths.first() {
                    self.last_from = self.touch(path, true, false, now);
                }
            }
            EventKind::Modify(ModifyKind::Name(RenameMode::To)) => {
                if let Some(path) = event.paths.first() {
                    let to = self.touch(path, true, false, now);
                    if let (Some(from), Some(to)) = (prev_from, to) {
                        self.link_rename(&from, &to);
                    }
                }
            }
            EventKind::Modify(ModifyKind::Name(RenameMode::Both)) => {
                if let [from, to] = event.paths.as_slice() {
                    let from = self.touch(from, true, false, now);
                    let to = self.touch(to, true, false, now);
                    if let (Some(from), Some(to)) = (from, to) {
                        self.link_rename(&from, &to);
                    }
                }
            }
            EventKind::Modify(ModifyKind::Name(_)) => {
                for path in &event.paths {
                    self.touch(path, true, false, now);
                }
            }
            EventKind::Modify(_) => {
                for path in &event.paths {
                    self.touch(path, false, true, now);
                }
            }
            EventKind::Access(_) | EventKind::Any | EventKind::Other => {}
        }
    }

    /// 監視のあふれ (または再走査の要求)。区間を `watcher_overflow` で残し、ベースラインを取り直す。
    /// あふれの間の変更は走査で見つかったものだけが `gap` の観測になる (中間の編集は復元しない)。
    fn handle_overflow(&mut self, reason: &str) {
        eprintln!("[track] 監視のあふれ: {reason}");
        let from = self.last_good_ms;
        let to = now_ms().max(from);
        self.pending.clear();
        self.last_from = None;
        let scope_reason = reason.to_string();
        let _ = self.writer.call(move |store| {
            // scope は影響したパス。全体なので NULL
            if let Err(e) = record_gap(store, "watcher_overflow", from, to, None) {
                eprintln!("[track] watcher_overflow を残せない ({scope_reason}): {e}");
            }
        });
        self.collector.rescan(Source::OverflowGap);
        self.last_good_ms = now_ms();
        self.shared.update(|s| s.overflows += 1);
    }

    fn flush_due(&mut self, force_all: bool) {
        let now = Instant::now();
        let due: Vec<String> = self
            .pending
            .iter()
            .filter(|(_, p)| force_all || p.due(now, &self.cfg))
            .map(|(k, _)| k.clone())
            .collect();
        if due.is_empty() {
            return;
        }
        // リネームの相手は一緒に処理する (別々のタイミングで流すと対が崩れる)
        let mut keys: BTreeSet<String> = due.into_iter().collect();
        for key in keys.clone() {
            if let Some(p) = self.pending.get(&key) {
                for partner in [&p.rename_from, &p.rename_to].into_iter().flatten() {
                    if self.pending.contains_key(partner) {
                        keys.insert(partner.clone());
                    }
                }
            }
        }
        let mut busy_since: HashMap<String, (Option<Instant>, String, bool)> = HashMap::new();
        let items: Vec<DueItem> = keys
            .into_iter()
            .filter_map(|key| {
                let p = self.pending.remove(&key)?;
                let multiple = p.carried_multiple || p.bursts > 1 || p.writes > 2;
                busy_since.insert(key.clone(), (p.busy_since, p.window.clone(), multiple));
                let waited_too_long = p.busy_since.is_some_and(|b| now.duration_since(b) >= self.cfg.settle_give_up);
                Some(DueItem {
                    rel: key,
                    structural: p.structural,
                    multiple_writes: multiple,
                    window: p.window,
                    rename_from: p.rename_from,
                    give_up: force_all || waited_too_long,
                })
            })
            .collect();
        let still: HashSet<String> = self.pending.keys().cloned().collect();
        let deferred = self.collector.flush(items, &still);
        self.last_good_ms = now_ms();
        let later = Instant::now();
        for Deferred { rel, delay } in deferred {
            let (since, window, multiple) =
                busy_since.get(&rel).cloned().unwrap_or((None, self.window.clone(), false));
            let since = since.unwrap_or(now);
            let mut p = Pending::new(now, window);
            p.carried_multiple = multiple;
            p.retry_at = Some(later + delay);
            p.busy_since = Some(since);
            match self.pending.get_mut(&rel) {
                // 待っている間に新しいイベントが来ていた: そちらの待ちを使い、判断だけ引き継ぐ
                Some(newer) => {
                    newer.carried_multiple |= multiple;
                    newer.busy_since = newer.busy_since.or(p.busy_since);
                }
                None => {
                    self.pending.insert(rel, p);
                }
            }
        }
    }
}
