//! 追跡サービスの本体: ファイル監視スレッドと、その寿命 (architecture.md §3.1、§4)。
//!
//! `notify` のイベントをパスごとにデバウンスし、flush の時点で `Collector` が内容を見て観測にする。
//! 起動時はベースライン (最初の起動) か、停止していた間の突き合わせ (2 回目以降) を行う。

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use notify::event::{ModifyKind, RenameMode};
use notify::{Event, EventKind, RecommendedWatcher, RecursiveMode, Watcher};
use serde_json::json;

use crate::baseline::{load_from_db, LoadedState};
use crate::collector::{Collector, Deferred, DueItem, ManagedFlush, ScanProgress, ScanTimings, TrackerConfig};
use crate::db::now_ms;
use crate::debounce::{verify_due, window_expired, Pending};
use crate::ignore::{under_always_ignored_dir, ALWAYS_IGNORED_DIR_NAMES};
use crate::observe::{record_gap, Source};
use crate::store::Store;
use crate::watch_plan::{plan as plan_watches, MAX_WATCHES};
use crate::writer::Writer;

/// 管理された操作が、監視スレッドへ「このパスを今すぐ読んで」と頼む要求。
struct FlushCmd {
    paths: Vec<String>,
    source: Source,
    reply: Sender<ManagedFlush>,
}

/// 即時の読み取りの結果。
#[derive(Clone, Debug)]
pub struct FlushOutcome {
    /// 全パスを予算の時間内に読めた (false = 帰属は後から付く)。
    pub done: bool,
    /// 読み取りに使った観測の時刻のうち最後のもの。何も読めなければ None。
    pub observed_at: Option<i64>,
}

/// 監視スレッドへ即時の読み取りを頼む取っ手 (複製できる)。
#[derive(Clone)]
pub struct FlushHandle {
    tx: Arc<Mutex<Sender<FlushCmd>>>,
}

/// 読めなかったパスをやり直す間隔。
const FLUSH_RETRY: Duration = Duration::from_millis(100);

impl FlushHandle {
    /// `paths` を今すぐ観測にする。読めないパスは `budget` の間やり直し、間に合わなければ `done = false`。
    /// 監視スレッドがベースライン・再走査の最中でも、予算の時間で返る (呼び出しを塞がない)。
    pub fn flush(&self, paths: Vec<String>, source: Source, budget: Duration) -> FlushOutcome {
        let deadline = Instant::now() + budget;
        let mut remaining = paths;
        let mut observed_at = None;
        while !remaining.is_empty() {
            let (reply, replies) = channel::<ManagedFlush>();
            let sent = self.tx.lock().unwrap().send(FlushCmd { paths: remaining.clone(), source, reply }).is_ok();
            if !sent {
                return FlushOutcome { done: false, observed_at };
            }
            match replies.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
                Ok(done) => {
                    observed_at = observed_at.max(Some(done.observed_at));
                    remaining = done.unreadable;
                }
                Err(_) => return FlushOutcome { done: false, observed_at },
            }
            if !remaining.is_empty() {
                if Instant::now() + FLUSH_RETRY >= deadline {
                    return FlushOutcome { done: false, observed_at };
                }
                std::thread::sleep(FLUSH_RETRY);
            }
        }
        FlushOutcome { done: true, observed_at }
    }
}

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
    /// ベースラインの所要時間の内訳。2 回目以降の突き合わせでは 0 のまま。
    pub baseline_timings: ScanTimings,
    pub first_run: bool,
    pub overflows: u32,
    /// 走査 (ベースライン・突き合わせ) の進み具合。走査中でなければ最後の値のまま。
    pub scan_done: usize,
    pub scan_total: usize,
    /// 検証の走査を行った回数と、取りこぼしを見つけた回数。
    pub verify_runs: u32,
    pub verify_gaps: u32,
    /// 記録に失敗した観測の区間の始まりと理由 (書けるようになって `coverage_gaps` に残したら None に戻る)。
    pub unrecorded_since_ms: Option<i64>,
    pub unrecorded_reason: Option<&'static str>,
    /// いま追跡しているファイルの数 (ベースラインの管理下にあるもの)。
    pub tracked_files: usize,
    /// 直近に記録した観測 1 件あたりの Writer への往復 (マイクロ秒)。0 = まだ記録していない。
    pub last_record_us: u64,
    /// 直近の検証の走査の前半だけの所要時間 (ミリ秒)。0 = まだ走らせていない。
    pub verify_scan_ms: u64,
    /// いま張っている OS の監視の本数。
    pub watch_count: usize,
    /// 監視を分けた結果が上限を超え、ルートの再帰 1 本へ戻している。
    pub watch_fallback: bool,
}

/// `status` を読むだけの取っ手。Tracker の所有権を持たずに複製できる (接続のスレッドが持つ)。
#[derive(Clone)]
pub struct StatusReader {
    shared: Arc<Shared>,
    progress: Arc<ScanProgress>,
}

impl StatusReader {
    pub fn status(&self) -> TrackerStatus {
        let mut status = self.shared.status.lock().unwrap().clone();
        status.scan_done = self.progress.done.load(Ordering::Relaxed);
        status.scan_total = self.progress.total.load(Ordering::Relaxed);
        status.last_record_us = self.progress.last_record_us.load(Ordering::Relaxed);
        status.verify_scan_ms = self.progress.last_verify_scan_ms.load(Ordering::Relaxed);
        status
    }
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
    dropped_paths: Arc<Mutex<Vec<PathBuf>>>,
    event_delay_us: Arc<AtomicU64>,
    inject: Sender<Stamped>,
    progress: Arc<ScanProgress>,
    flush: FlushHandle,
    thread: Option<JoinHandle<()>>,
    stopped: bool,
}

impl Tracker {
    /// Store を Writer へ移し、監視スレッドを起動する。ベースラインはそのスレッドで取る (`wait_ready` で待てる)。
    pub fn start(root: &Path, mut store: Store, cfg: TrackerConfig) -> Tracker {
        store.attribution = cfg.attribution.clone();
        let blob_reader = store.blobs.reader();
        let writer = Arc::new(Writer::spawn(store));
        let shared = Arc::new(Shared {
            status: Mutex::new(TrackerStatus {
                phase: Phase::Starting,
                baseline_files: 0,
                baseline_ms: 0,
                baseline_bytes: 0,
                baseline_timings: ScanTimings::default(),
                first_run: false,
                overflows: 0,
                scan_done: 0,
                scan_total: 0,
                verify_runs: 0,
                verify_gaps: 0,
                unrecorded_since_ms: None,
                unrecorded_reason: None,
                tracked_files: 0,
                last_record_us: 0,
                verify_scan_ms: 0,
                watch_count: 0,
                watch_fallback: false,
            }),
            changed: Condvar::new(),
        });
        let stop = Arc::new(AtomicBool::new(false));
        let drop_events = Arc::new(AtomicBool::new(false));
        let dropped_paths = Arc::new(Mutex::new(Vec::new()));
        let event_delay_us = Arc::new(AtomicU64::new(0));
        let (tx, rx) = channel::<Stamped>();
        let progress = Arc::new(ScanProgress::default());
        let (flush_tx, flush_rx) = channel::<FlushCmd>();

        let thread = {
            let mut collector = Collector::new(root.to_path_buf(), cfg.clone(), Arc::clone(&writer), Arc::clone(&stop), Arc::clone(&progress), blob_reader);
            let window = collector.new_window();
            let mut watch = WatchLoop {
                root: root.to_path_buf(),
                cfg: cfg.clone(),
                writer: Arc::clone(&writer),
                collector,
                flush_rx,
                window,
                window_last_event: Instant::now(),
                shared: Arc::clone(&shared),
                stop: Arc::clone(&stop),
                drop_events: Arc::clone(&drop_events),
                dropped_paths: Arc::clone(&dropped_paths),
                event_delay_us: Arc::clone(&event_delay_us),
                pending: HashMap::new(),
                last_from: None,
                last_good_ms: now_ms(),
                watcher: None,
                last_check: Instant::now(),
                last_event: Instant::now(),
                last_verify: Instant::now(),
                last_verify_ms: now_ms(),
                last_retry: Instant::now(),
                events_since_verify: 0,
                verify_wanted: false,
                published_unrecorded: None,
                published_tracked: 0,
                watched: BTreeMap::new(),
                replan_wanted: false,
                last_plan: Instant::now(),
                plan_cost: Duration::ZERO,
            };
            let tx_watcher = tx.clone();
            std::thread::Builder::new()
                .name("mye-track-watch".into())
                .spawn(move || watch.run(tx_watcher, rx))
                .expect("watch thread")
        };
        let flush = FlushHandle { tx: Arc::new(Mutex::new(flush_tx)) };
        Tracker { writer, shared, stop, drop_events, dropped_paths, event_delay_us, inject: tx, progress, flush, thread: Some(thread), stopped: false }
    }

    pub fn status(&self) -> TrackerStatus {
        self.status_reader().status()
    }

    /// Tracker を持たずに `status` を読める取っ手 (pipe の接続が使う)。
    pub fn status_reader(&self) -> StatusReader {
        StatusReader { shared: Arc::clone(&self.shared), progress: Arc::clone(&self.progress) }
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

    /// 管理された操作が、パスを今すぐ読んでもらうための取っ手。
    pub fn flush_handle(&self) -> FlushHandle {
        self.flush.clone()
    }

    /// 試験用: `notify` のコールバックへ値を差し込む。`Err` や再走査フラグ付きのイベントで、
    /// 監視バッファのあふれの報告を模す (本物のあふれは notify 8 が Windows で報告しない。architecture.md §4)。
    #[doc(hidden)]
    pub fn inject_event(&self, item: notify::Result<Event>) {
        let _ = self.inject.send((Instant::now(), item));
    }

    /// 試験用: イベント 1 件の処理に毎回この時間を足す (処理待ちのイベントが溜まる状態を、決定的に作る)。
    #[doc(hidden)]
    pub fn delay_event_handling(&self, per_event: Duration) {
        self.event_delay_us.store(per_event.as_micros() as u64, Ordering::SeqCst);
    }

    /// 試験用: 届いたイベントを捨てる (取りこぼしを模す)。
    #[doc(hidden)]
    pub fn drop_incoming_events(&self, on: bool) {
        self.drop_events.store(on, Ordering::SeqCst);
    }

    /// 試験用: `drop_incoming_events` で捨てたイベントのパス (届いた順)。最後の操作のパスが現れれば、
    /// それより前の操作のイベントは (OS が起きた順に届けるので) 捨て終わっている。固定の待ちの代わりに使う。
    #[doc(hidden)]
    pub fn dropped_event_paths(&self) -> Vec<PathBuf> {
        self.dropped_paths.lock().unwrap().clone()
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

struct WatchLoop {
    root: PathBuf,
    cfg: TrackerConfig,
    writer: Arc<Writer>,
    collector: Collector,
    flush_rx: Receiver<FlushCmd>,
    shared: Arc<Shared>,
    stop: Arc<AtomicBool>,
    drop_events: Arc<AtomicBool>,
    dropped_paths: Arc<Mutex<Vec<PathBuf>>>,
    event_delay_us: Arc<AtomicU64>,
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
    last_check: Instant,
    /// 最後にイベントを処理した時刻 (検証の走査の「静かな時間」)。
    last_event: Instant,
    last_verify: Instant,
    /// 前回の検証の走査 (無ければ起動) の時刻。取りこぼしの区間の始まり。
    last_verify_ms: i64,
    last_retry: Instant,
    events_since_verify: usize,
    /// 記録に失敗した区間を書けた直後など、間隔を待たずに検証の走査を行う。
    verify_wanted: bool,
    published_unrecorded: Option<(i64, &'static str)>,
    published_tracked: usize,
    /// 張っている OS の監視 (ルートからの相対 → 再帰か)。`watch_plan` の計画に合わせて付け替える。
    watched: BTreeMap<String, bool>,
    /// 監視の付け替えが要るかもしれない (ディレクトリの出入り、`.gitignore` の変更)。
    replan_wanted: bool,
    last_plan: Instant,
    /// 直近の計画づくり (ディレクトリの走査) の所要時間。次の計画までの間隔の下限に使う。
    plan_cost: Duration,
}

/// 監視の付け替えの最短の間隔。ディレクトリが大量に作られる間、毎回は走査しない。
const REPLAN_MIN_INTERVAL: Duration = Duration::from_millis(200);
/// 計画づくりに掛かった時間のこの倍数は次の計画まで空ける (走査が監視スレッドを占有しすぎないように)。
const REPLAN_COST_FACTOR: u32 = 8;

/// `rel` が `base` の下 (または同じ)。`base` が "" ならルート (全部の下)。
fn is_under_rel(rel: &str, base: &str) -> bool {
    base.is_empty() || rel == base || rel.strip_prefix(base).is_some_and(|rest| rest.starts_with('/'))
}

/// `root` + 相対パス ('/' 区切り。"" はルート自身)。
fn join_rel(root: &Path, rel: &str) -> PathBuf {
    if rel.is_empty() {
        root.to_path_buf()
    } else {
        root.join(rel)
    }
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
    fn run(&mut self, tx_watcher: Sender<Stamped>, rx: Receiver<Stamped>) {
        match self.startup(tx_watcher) {
            Err(reason) => {
                self.shared.update(|s| s.phase = Phase::Failed(reason.clone()));
                eprintln!("[track] 監視を始められない: {reason}");
                return;
            }
            // ベースラインを取り終える前に止められた。取り直しは次の起動でやる
            Ok(false) => return,
            Ok(true) => {}
        }
        // 取りこぼさないよう、監視を始めた後にベースラインを取る。その間のイベントは rx に溜まっている
        while !self.stop.load(Ordering::SeqCst) {
            self.step(&rx);
            self.maintenance(&rx);
        }
        // 止める前に、届いているイベントとデバウンス中の変更を処理する
        while let Ok((at, item)) = rx.try_recv() {
            if let Ok(event) = item {
                self.handle_event(event, at);
            }
        }
        self.flush_due(true);
        self.collector.release_held();
    }

    fn check_every(&self) -> Duration {
        (self.cfg.debounce / 4).clamp(Duration::from_millis(1), Duration::from_millis(10))
    }

    /// イベントを 1 件待って処理し、期限が来たパスを flush する。
    fn step(&mut self, rx: &Receiver<Stamped>) {
        let check_every = self.check_every();
        let timeout = if self.pending.is_empty() { MAX_WAIT } else { check_every.min(MAX_WAIT) };
        match rx.recv_timeout(timeout) {
            Ok((at, Ok(event))) => self.handle_event(event, at),
            Ok((_, Err(e))) => self.handle_overflow(&format!("watcher error: {e}")),
            Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => self.stop.store(true, Ordering::SeqCst),
        }
        if self.last_check.elapsed() >= check_every {
            self.last_check = Instant::now();
            self.flush_due(false);
        }
        self.serve_flush_commands(rx);
    }

    /// 管理された操作からの即時の読み取りの要求に答える。届いているイベントを先に取り込み、
    /// 操作の前に起きた変更が、操作の変更と混ざらないようにする。
    fn serve_flush_commands(&mut self, rx: &Receiver<Stamped>) {
        while let Ok(cmd) = self.flush_rx.try_recv() {
            self.drain_events(rx);
            let done = self.collector.observe_managed(cmd.paths, cmd.source);
            let _ = cmd.reply.send(done);
        }
    }

    /// 記録に失敗した区間の再試行と、取りこぼしを見つける検証の走査 (architecture.md §4)。
    /// 走査はデバウンス中のパスが無く、イベントが静かなときだけ行う (処理待ちの変更を `gap` にしないため)。
    fn maintenance(&mut self, rx: &Receiver<Stamped>) {
        let now = Instant::now();
        self.replan_if_due();
        let tracked = self.collector.baseline.files.len();
        if tracked != self.published_tracked {
            self.published_tracked = tracked;
            self.shared.update(|s| s.tracked_files = tracked);
        }
        let unrecorded = self.collector.unrecorded().map(|u| (u.since_ms, u.reason));
        if unrecorded != self.published_unrecorded {
            self.published_unrecorded = unrecorded;
            self.shared.update(|s| {
                s.unrecorded_since_ms = unrecorded.map(|u| u.0);
                s.unrecorded_reason = unrecorded.map(|u| u.1);
            });
        }
        if unrecorded.is_some() {
            if now.duration_since(self.last_retry) >= self.cfg.unrecorded_retry {
                self.last_retry = now;
                // 書けるようになった: 失敗した間の変更を、実際の状態との突き合わせで拾う
                self.verify_wanted |= self.collector.retry_unrecorded();
            }
            return;
        }
        // 処理待ちのイベント (長い flush の間に溜まったもの) を先に取り込む。取り込まないまま走査すると、
        // 待ちのイベントの変更を取りこぼしと取り違える
        self.drain_events(rx);
        let quiet_for = Instant::now().duration_since(self.last_event);
        let since_last_verify = now.duration_since(self.last_verify);
        if verify_due(self.verify_wanted, self.events_since_verify, self.pending.is_empty(), quiet_for, since_last_verify, &self.cfg) {
            self.verify(rx);
        }
    }

    /// キューに届いているイベントを全部処理する (待たない)。
    fn drain_events(&mut self, rx: &Receiver<Stamped>) {
        while let Ok((at, item)) = rx.try_recv() {
            match item {
                Ok(event) => self.handle_event(event, at),
                Err(e) => self.handle_overflow(&format!("watcher error: {e}")),
            }
        }
    }

    /// 検証の走査。ベースラインと実際の中身を突き合わせ、違うものを `gap` の観測にする。
    /// 違いを見つけてから、そのパスのイベントが遅れて届く時間だけ待ち、届いたものは通常の経路へ任せる。
    fn verify(&mut self, rx: &Receiver<Stamped>) {
        let from = self.last_verify_ms;
        self.verify_wanted = false;
        self.events_since_verify = 0;
        let (mut candidates, completed) = self.collector.detect_changed();
        let settle = self.cfg.debounce + self.cfg.settle_wait * 2;
        let until = Instant::now() + settle;
        while Instant::now() < until && !self.stop.load(Ordering::SeqCst) {
            self.step(rx);
        }
        self.drain_events(rx);
        candidates.retain(|rel| !self.pending.contains_key(rel));
        let changes = if completed && !self.stop.load(Ordering::SeqCst) {
            self.collector.observe_paths(candidates, Source::VerifyGap)
        } else {
            0
        };
        let to = now_ms().max(from);
        if changes > 0 {
            eprintln!("[track] 取りこぼしを検証の走査で見つけた: {changes} 件");
            let _ = self.writer.call(move |store| {
                if let Err(e) = record_gap(store, "watcher_overflow", from, to, None) {
                    eprintln!("[track] watcher_overflow を残せない (検証の走査): {e}");
                }
            });
        }
        self.last_verify = Instant::now();
        self.last_verify_ms = to;
        self.last_good_ms = to;
        self.shared.update(|s| {
            s.verify_runs += 1;
            s.verify_gaps += u32::from(changes > 0);
        });
    }

    /// 起動の手順: 前回の状態を読む → `service_start` → 監視を始める → ベースライン / 突き合わせ。
    /// 戻り値の bool は、走査を取り終えたか (止める合図で打ち切られたら false)。
    fn startup(&mut self, tx_watcher: Sender<Stamped>) -> Result<bool, String> {
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

        let watcher: RecommendedWatcher =
            notify::recommended_watcher(move |res| {
                let _ = tx_watcher.send((Instant::now(), res));
            })
            .map_err(|e| format!("監視を作れない: {e}"))?;
        self.watcher = Some(watcher);
        self.apply_watch_plan(false)?;

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
        if !stats.completed {
            return Ok(false);
        }
        let elapsed_ms = Collector::elapsed_since(started);
        let files_now = self.collector.baseline.files.len();
        let (scanned, bytes, changes, timings) = (stats.files, stats.bytes, stats.changes, stats.timings);
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
            s.baseline_timings = timings;
            s.first_run = first_run;
            s.tracked_files = files_now;
            s.phase = Phase::Watching;
        });
        self.published_tracked = files_now;
        self.last_verify = Instant::now();
        self.last_verify_ms = now_ms();
        Ok(true)
    }

    /// 絶対パスをパスごとの待ち行列へ入れる。追跡の外 (ルート自身・固定の無視名の下) は None。
    fn touch(&mut self, path: &Path, structural: bool, write: bool, now: Instant) -> Option<String> {
        let rel = relative_to(&self.root, path)?;
        if under_always_ignored_dir(&rel) {
            return None;
        }
        if self.may_change_watch_plan(&rel, path, structural) {
            self.replan_wanted = true;
        }
        if window_expired(now, self.window_last_event, self.cfg.debounce) {
            self.window = self.collector.new_window();
        }
        self.window_last_event = now;
        let window = self.window.clone();
        let burst_gap = self.cfg.burst_gap;
        self.pending
            .entry(rel.clone())
            .or_insert_with(|| Pending::new(now, window))
            .touch(now, burst_gap, structural, write);
        Some(rel)
    }

    /// このイベントで、監視の分け方が変わりうるか。`.gitignore` は書き込みでも対象。そのほかは構造の変更
    /// (ディレクトリの作成・名前変更・削除、無視する名前) だけ (ファイルの作成の洪水では、安い問い合わせで False になる)。
    fn may_change_watch_plan(&self, rel: &str, path: &Path, structural: bool) -> bool {
        let name = rel.rsplit('/').next().unwrap_or(rel);
        if name.eq_ignore_ascii_case(".gitignore") {
            return true;
        }
        structural
            && (ALWAYS_IGNORED_DIR_NAMES.iter().any(|n| name.eq_ignore_ascii_case(n))
                || self.watched.contains_key(rel)
                || std::fs::symlink_metadata(path).map(|m| m.is_dir()).unwrap_or(false))
    }

    /// 監視の分け方を作り直す時刻が来ていれば、付け替える。
    fn replan_if_due(&mut self) {
        if !self.replan_wanted {
            return;
        }
        let wait = REPLAN_MIN_INTERVAL.max(self.plan_cost * REPLAN_COST_FACTOR);
        if self.last_plan.elapsed() < wait {
            return;
        }
        self.replan_wanted = false;
        // ルートの監視は起動で張れている。付け替えの失敗は、その子の監視が無いだけ (検証の走査が拾う)
        if let Err(e) = self.apply_watch_plan(true) {
            eprintln!("[track] 監視の付け替えに失敗: {e}");
        }
    }

    /// 計画に合わせて OS の監視を足す・外す。`replan` が真なら、新しく見るようになったディレクトリの中を
    /// 突き合わせに回す (足す前に起きた変更は監視に見えないため。起動時はベースラインが受け持つ)。
    /// 見え方が変わる間に変更を落とさないよう、足すのを先にし、外すのを最後にする。
    /// ルート自身の監視に失敗したら Err (起動なら監視を始められない)。
    fn apply_watch_plan(&mut self, replan: bool) -> Result<(), String> {
        let started = Instant::now();
        let plan = plan_watches(&self.root, MAX_WATCHES);
        let Some(watcher) = self.watcher.as_mut() else { return Err("監視を作れていない".into()) };
        let before = self.watched.clone();
        let mut uncovered: Vec<PathBuf> = Vec::new();
        let mut remounted = false;
        let watch_one = |watcher: &mut RecommendedWatcher, rel: &str, recursive: bool, watched: &mut BTreeMap<String, bool>| {
            let path = join_rel(&self.root, rel);
            let mode = if recursive { RecursiveMode::Recursive } else { RecursiveMode::NonRecursive };
            match watcher.watch(&path, mode) {
                Ok(()) => {
                    watched.insert(rel.to_string(), recursive);
                    Ok(path)
                }
                Err(e) if rel.is_empty() => Err(format!("監視を始められない: {e}")),
                Err(e) => {
                    eprintln!("[track] 監視を張れない: {rel}: {e}");
                    Ok(PathBuf::new())
                }
            }
        };
        // 1. 新しく見るパスを足す
        for (rel, recursive) in &plan.entries {
            if before.contains_key(rel) {
                continue;
            }
            let path = watch_one(watcher, rel, *recursive, &mut self.watched)?;
            // 前から再帰の監視の下にあったパスは、足す前の変更も既に見えていた
            let covered = before.iter().any(|(k, rec)| *rec && is_under_rel(rel, k));
            if !path.as_os_str().is_empty() && !covered {
                uncovered.push(path);
            }
        }
        // 2. 再帰か非再帰かが変わるパスは付け直す (張り直す一瞬の変更は検証の走査が拾う)
        for (rel, recursive) in &plan.entries {
            if before.get(rel).is_some_and(|old| old != recursive) {
                self.watched.remove(rel);
                let _ = watcher.unwatch(&join_rel(&self.root, rel));
                watch_one(watcher, rel, *recursive, &mut self.watched)?;
                remounted = true;
            }
        }
        // 3. 計画に無くなったパスを外す
        for rel in before.keys().filter(|rel| !plan.entries.contains_key(*rel)) {
            self.watched.remove(rel);
            let _ = watcher.unwatch(&join_rel(&self.root, rel));
        }
        let (count, fallback) = (self.watched.len(), plan.fallback);
        self.shared.update(|s| {
            s.watch_count = count;
            s.watch_fallback = fallback;
        });
        self.plan_cost = started.elapsed();
        self.last_plan = Instant::now();
        if replan {
            let now = Instant::now();
            for path in uncovered {
                self.touch(&path, true, false, now);
            }
            // 足したディレクトリ自身の touch で、計画のやり直しを呼び直さない
            self.replan_wanted = false;
            self.verify_wanted |= remounted;
        }
        Ok(())
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
        if self.drop_events.load(Ordering::SeqCst) {
            self.dropped_paths.lock().unwrap().extend(event.paths);
            return;
        }
        let delay = self.event_delay_us.load(Ordering::Relaxed);
        if delay > 0 {
            std::thread::sleep(Duration::from_micros(delay));
        }
        if event.need_rescan() {
            self.handle_overflow("rescan flag");
            return;
        }
        self.last_good_ms = now_ms();
        self.last_event = Instant::now();
        self.events_since_verify += 1;
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
        self.last_verify = Instant::now();
        self.last_verify_ms = self.last_good_ms;
        self.events_since_verify = 0;
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
                let multiple = p.is_multiple();
                busy_since.insert(key.clone(), (p.busy_since, p.window.clone(), multiple));
                let waited_too_long = p.waited_too_long(now, self.cfg.settle_give_up);
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
        let still_windows: HashSet<String> = self.pending.values().map(|p| p.window.clone()).collect();
        let deferred = self.collector.flush(items, &still, &still_windows, now);
        self.last_good_ms = now_ms();
        let later = Instant::now();
        for Deferred { rel, delay, .. } in deferred {
            let (since, window, multiple) =
                busy_since.get(&rel).cloned().unwrap_or((None, self.window.clone(), false));
            let since = since.unwrap_or(now);
            match self.pending.get_mut(&rel) {
                // 待っている間に新しいイベントが来ていた: そちらの待ちを使い、判断だけ引き継ぐ
                Some(newer) => newer.carry_over(multiple, since),
                None => {
                    self.pending.insert(rel, Pending::deferred(now, window, multiple, since, later + delay));
                }
            }
        }
    }
}
