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
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

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
    /// 監視なし。CLI と単体テストが使う
    pub fn new(root: String) -> Self {
        Self::with_dispatcher(root, ops::dispatch)
    }

    /// 監視あり。**mye_collab_create (DLL) だけが使う**。
    /// 監視が張れなくても Service は生きる (窓の「更新」で status は取れる)
    pub fn with_watch(root: String) -> Self {
        let mut svc = Self::with_dispatcher(root.clone(), ops::dispatch);
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
        let out: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let dead = Arc::new(AtomicBool::new(false));
        let (tx, rx) = channel::<Msg>();
        let out_w = Arc::clone(&out);
        let dead_w = Arc::clone(&dead);
        let handle = std::thread::Builder::new()
            .name("mye_collab_worker".to_string())
            .spawn(move || {
                let mut state = State::new(root);
                while let Ok(msg) = rx.recv() {
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
