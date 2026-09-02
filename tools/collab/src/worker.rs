// worker スレッド 1 本 + 出力キュー。
//
// なぜ 1 本か: git は index.lock を握る = 並列に走らせると自分同士で locked_index を
// 踏む。FIFO で直列化しておけば「UI から見て実行中の op は高々 1 個」も同時に守れる。
//
// C++ 側にはスレッドを作らせない (spec §4.0)。Editor は毎フレーム poll を
// NULL まで drain するだけ = ロックも条件変数も C++ に持ち込まない。

use std::collections::VecDeque;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

use serde_json::Value;

use crate::ops::{self, Dispatcher, State};
use crate::protocol::{self, code, ErrorBody, Request, Response};

enum Msg {
    Request(String),
    Shutdown,
}

pub struct Service {
    tx: Option<Sender<Msg>>,
    out: Arc<Mutex<VecDeque<String>>>,
    dead: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

impl Service {
    pub fn new(root: String) -> Self {
        Self::with_dispatcher(root, ops::dispatch)
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
                    }
                }
            })
            .ok();
        Service { tx: Some(tx), out, dead, handle }
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

fn panic_text(payload: &Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "panic".to_string()
    }
}
