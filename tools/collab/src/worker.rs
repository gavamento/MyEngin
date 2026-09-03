// worker スレッド 1 本 + 出力キュー。
//
// なぜ 1 本か: git は index.lock を握る = 並列に走らせると自分同士で locked_index を
// 踏む。FIFO で直列化しておけば「UI から見て実行中の op は高々 1 個」も同時に守れる。
//
// C++ 側にはスレッドを作らせない (spec §4.0)。Editor は毎フレーム poll を
// NULL まで drain するだけ = ロックも条件変数も C++ に持ち込まない。

use std::collections::VecDeque;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use serde_json::Value;

use crate::ops::{self, Dispatcher, State};
use crate::protocol::{self, code, ErrorBody, Request, Response};
use crate::watch::WatchHandle;

enum Msg {
    Request(String),
    /// 監視スレッド発。status を取り直し、前回と違えば通知を積む (M66b)
    Refresh,
    Shutdown,
}

/// 定期 fetch の時刻合わせのために worker が目を覚ます間隔 (M66f)。
///
/// ★**新しいスレッドを立てない**ための仕掛け (spec §4.0)。notify 8.2.0 の
///   `ReadDirectoryChangesWatcher::drop` が内部スレッドを join しないせいで
///   `FreeLibrary` を撤去した経緯があり、「join できないスレッド」をこれ以上
///   増やすとアンロード時の即死が戻ってくる。既にある worker の `recv` を
///   `recv_timeout` に変えるだけなら、スレッドは 1 本のままで済む。
/// ★1 秒にしているのは「間隔が分単位である以上、それより細かい精度に意味が無い」から。
///   起き抜けにやるのは Instant の比較 1 回だけなので、常駐コストは無視できる
const TICK: Duration = Duration::from_secs(1);

pub struct Service {
    tx: Option<Sender<Msg>>,
    out: Arc<Mutex<VecDeque<String>>>,
    dead: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
    /// None = 監視なし。**CLI (script モード) は必ず None** — 監視を立てると
    /// 出力に event 行がタイミング次第で混ざり、期待 NDJSON が非決定になる
    /// (spec §4.4)
    watch: Option<WatchHandle>,
}

impl Service {
    /// 監視なし・タイマーなし。CLI と単体テストが使う。
    /// ★CLI (script モード) がこちらを使うのは spec §4.4 の要求 —
    ///   event 行が出ると期待 NDJSON が非決定になる
    pub fn new(root: String) -> Self {
        Self::with_dispatcher(root, ops::dispatch)
    }

    /// 定期 fetch のタイマーだけを回す (監視スレッドは立てない)。**テスト専用の入口** —
    /// 本番 (DLL) は with_watch。監視を外せるので「タイマーが本当に fetch したか」を
    /// ファイル変更由来の通知と混ぜずに観測できる
    pub fn with_timer(root: String) -> Self {
        Self::build(root, ops::dispatch, true)
    }

    /// 監視あり + 定期 fetch のタイマーあり。**mye_collab_create (DLL) だけが使う**。
    /// 監視が張れなくても Service は生きる (窓の「更新」で status は取れる)
    pub fn with_watch(root: String) -> Self {
        let mut svc = Self::build(root.clone(), ops::dispatch, true);
        if let Some(tx) = svc.tx.clone() {
            svc.watch = crate::watch::spawn(&PathBuf::from(root), move || {
                // 送信失敗 = worker が既に落ちている。監視スレッドは次の
                // stop チェックで抜けるので、ここでは黙って捨てる
                let _ = tx.send(Msg::Refresh);
            });
        }
        svc
    }

    /// テストが panic する dispatcher を差し込むための入口。
    /// **本番経路と同じ worker を使う**ことに意味がある (panic 隔離の検査は
    /// 「本物の worker が dead 化するか」でしか意味を持たない)
    pub fn with_dispatcher(root: String, dispatcher: Dispatcher) -> Self {
        Self::build(root, dispatcher, false)
    }

    /// worker を 1 本起こす。`background` = 定期 fetch のタイマーを回すか
    /// (spec §4.4: CLI は false = `recv` で寝たまま起きない)
    fn build(root: String, dispatcher: Dispatcher, background: bool) -> Self {
        let out: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let dead = Arc::new(AtomicBool::new(false));
        let (tx, rx) = channel::<Msg>();
        let out_w = Arc::clone(&out);
        let dead_w = Arc::clone(&dead);
        let handle = std::thread::Builder::new()
            .name("mye_collab_worker".to_string())
            .spawn(move || {
                let mut state = State::new(root);
                loop {
                    // ★background のときだけ `recv_timeout`。CLI で使うと
                    //   「何も来ていないのに 1 秒ごとに起きる」だけの無駄になるうえ、
                    //   タイマー由来の通知が期待 NDJSON に混ざりうる
                    let msg = if background {
                        // ★期限の確認は**待つ前**に、毎周回やる。timeout の枝にだけ
                        //   置くと、メッセージが 1 秒より短い間隔で届き続ける限り
                        //   タイマーが永久に飢える。監視スレッドは
                        //   `.git\index` の更新 (status を走らせると自分で触る) で
                        //   デバウンス間隔ごとに Refresh を投げうるので、これは
                        //   理論上の話ではない — 実機で 2 分半 fetch が 1 回も
                        //   走らないのを観測して直した
                        handle_tick(&mut state, &out_w, &dead_w);
                        match rx.recv_timeout(TICK) {
                            Ok(m) => m,
                            Err(RecvTimeoutError::Timeout) => continue, // 次の周回で期限を見る
                            Err(RecvTimeoutError::Disconnected) => break,
                        }
                    } else {
                        match rx.recv() {
                            Ok(m) => m,
                            Err(_) => break,
                        }
                    };
                    match msg {
                        Msg::Shutdown => break,
                        Msg::Request(line) => {
                            handle_line(&mut state, &line, &out_w, &dead_w, dispatcher);
                        }
                        Msg::Refresh => {
                            handle_refresh(&mut state, &out_w, &dead_w);
                        }
                    }
                }
            })
            .ok();
        Service { tx: Some(tx), out, dead, handle, watch: None }
    }

    /// 非同期。応答は poll で届く
    pub fn request(&self, line: String) {
        if self.dead.load(Ordering::SeqCst) {
            // worker が panic 済み。**待たせない**ことが最優先 — C++ 側は id 待ちの
            // コールバックを抱えているので、返さないと永久に「実行中」のままになる
            let id = extract_id(&line);
            self.push(Response::err(id, ErrorBody::new(code::SERVICE_DEAD, "collab worker is dead")).to_line());
            return;
        }
        if let Some(tx) = &self.tx {
            if tx.send(Msg::Request(line)).is_err() {
                self.dead.store(true, Ordering::SeqCst);
            }
        }
    }

    pub fn poll(&self) -> Option<String> {
        self.out.lock().ok()?.pop_front()
    }

    fn push(&self, line: String) {
        if let Ok(mut q) = self.out.lock() {
            q.push_back(line);
        }
    }

    pub fn is_dead(&self) -> bool {
        self.dead.load(Ordering::SeqCst)
    }
}

impl Drop for Service {
    fn drop(&mut self) {
        // ★監視を**先に**止める。逆順にすると、閉じた worker のチャネルへ
        //   Refresh を送りつける競合が残る (送信は握り潰すので害は無いが、
        //   監視スレッドが生きたまま FreeLibrary されると即死する)
        self.watch = None;
        // ★join を飛ばして FreeLibrary すると、走っている worker のコードごと
        //   アンロードされてエディタが落ちる。destroy → FreeLibrary の順は
        //   C++ 側 (CollabClient::Shutdown) の契約でもある
        if let Some(tx) = self.tx.take() {
            let _ = tx.send(Msg::Shutdown);
            drop(tx);
        }
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
    }
}

fn push_line(out: &Arc<Mutex<VecDeque<String>>>, line: String) {
    if let Ok(mut q) = out.lock() {
        q.push_back(line);
    }
}

/// 壊れた JSON でも id だけは拾う (拾えなければ 0)。
///
/// ★serde の全文パースが通らない行 (途中で切れた JSON など) でも id を救うために
///   手書きの走査へ落ちる。id を失うと C++ 側の待ちコールバックが**永久に残り**、
///   窓が「実行中」のまま二度と操作できなくなる — 誤った id を返す方がまだ回復可能
fn extract_id(line: &str) -> u64 {
    if let Some(v) = serde_json::from_str::<Value>(line)
        .ok()
        .and_then(|v| v.get("id").and_then(|i| i.as_u64()))
    {
        return v;
    }
    let pos = match line.find("\"id\"") {
        Some(p) => p + 4,
        None => return 0,
    };
    let rest = line[pos..].trim_start();
    let rest = match rest.strip_prefix(':') {
        Some(r) => r.trim_start(),
        None => return 0,
    };
    let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
    digits.parse().unwrap_or(0)
}

fn handle_line(
    state: &mut State,
    line: &str,
    out: &Arc<Mutex<VecDeque<String>>>,
    dead: &Arc<AtomicBool>,
    dispatcher: Dispatcher,
) {
    let req: Request = match serde_json::from_str(line) {
        Ok(r) => r,
        Err(e) => {
            push_line(
                out,
                Response::err(extract_id(line), ErrorBody::new(code::BAD_REQUEST, e.to_string())).to_line(),
            );
            return;
        }
    };
    let result = catch_unwind(AssertUnwindSafe(|| dispatcher(state, &req.op, &req.args)));
    match result {
        Ok(Ok(value)) => push_line(out, Response::ok(req.id, value).to_line()),
        Ok(Err(err)) => push_line(out, Response::err(req.id, err).to_line()),
        Err(payload) => {
            let detail = panic_text(&payload);
            // 通知は 1 回だけ (dead は二度と false に戻らない)
            if !dead.swap(true, Ordering::SeqCst) {
                push_line(out, protocol::service_error_line(code::INTERNAL_PANIC, &detail));
            }
            push_line(out, Response::err(req.id, ErrorBody::new(code::INTERNAL_PANIC, detail)).to_line());
        }
    }
}

/// タイマーの 1 目盛り (M66f)。**定期 fetch の期限が来ていなければ何もしない**。
///
/// ★fetch を走らせるのは worker スレッド自身 = git の直列実行に自然に乗る。
///   別スレッドから git を叩くと index.lock を自分同士で踏む (worker が 1 本である
///   理由そのもの)。「ユーザーが押した pull の最中に背景 fetch が割り込む」も
///   構造的に起こらない。
/// ★次回の予定は fetch が**終わってから**組む。走る前に組むと、ネットワークが遅くて
///   fetch に間隔以上かかる環境で背中合わせに走り続ける
fn handle_tick(state: &mut State, out: &Arc<Mutex<VecDeque<String>>>, dead: &Arc<AtomicBool>) {
    if !state.auto_fetch {
        return;
    }
    match state.next_fetch_at {
        Some(at) if Instant::now() >= at => {}
        _ => return,
    }
    match catch_unwind(AssertUnwindSafe(|| ops::background_fetch(state))) {
        Ok(lines) => {
            for line in lines {
                push_line(out, line);
            }
            state.next_fetch_at = Some(Instant::now() + state.fetch_interval());
        }
        Err(payload) => {
            // 要求経路・監視経路と同じ扱い。ここだけ握り潰すと
            // 「定期 fetch のせいで静かに死んだサービス」ができる
            let detail = panic_text(&payload);
            if !dead.swap(true, Ordering::SeqCst) {
                push_line(out, protocol::service_error_line(code::INTERNAL_PANIC, &detail));
            }
        }
    }
}

/// 監視スレッド発の取り直し。**応答 (id) は無い**ので、通知行だけを積む。
/// panic は要求経路と同じ扱い (service_error 1 回 + dead 化) — ここだけ
/// 握り潰すと「監視のせいで静かに死んだサービス」ができる
fn handle_refresh(state: &mut State, out: &Arc<Mutex<VecDeque<String>>>, dead: &Arc<AtomicBool>) {
    match catch_unwind(AssertUnwindSafe(|| ops::refresh_status(state))) {
        Ok(lines) => {
            for line in lines {
                push_line(out, line);
            }
        }
        Err(payload) => {
            let detail = panic_text(&payload);
            if !dead.swap(true, Ordering::SeqCst) {
                push_line(out, protocol::service_error_line(code::INTERNAL_PANIC, &detail));
            }
        }
    }
}

fn panic_text(payload: &Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "panic".to_string()
    }
}
