//! Editor への通知 (`subscribe`、ipc-contract.md §4.5)。
//!
//! 記録の経路 (Writer、監視) には触れず、読み取り専用の接続で DB を定期的に引いて、増えた分を通知にする
//! (購読者がいる間だけ)。通知は購読ごとの上限つきのキューに積み、あふれたら古いものを捨てて `resync` を 1 つ出す。
//! 読まない購読者がいても、積むだけなので Writer も監視も止まらない。

use std::collections::{HashMap, HashSet, VecDeque};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use rusqlite::Connection;
use serde_json::{json, Value};

use crate::ops;

/// 購読ごとのキューの既定の長さ。
pub const DEFAULT_QUEUE_CAP: usize = 256;

/// DB を引く間隔 (購読者がいる間だけ)。通知の遅れの上限になる。
pub const POLL_INTERVAL: Duration = Duration::from_millis(200);

/// 1 回の引きで読む Revision・根拠の上限。多いときは次の回に回す。
const BATCH_LIMIT: i64 = 1000;

/// 1 つの購読。通知を積むのはポーラとサービスの終了の合図、取り出すのは接続のスレッドだけ。
pub struct Subscriber {
    events: HashSet<String>,
    state: Mutex<QueueState>,
}

struct QueueState {
    queue: VecDeque<Value>,
    cap: usize,
    /// キューがあふれて通知を捨てた (次の取り出しの先頭で `resync` を 1 つ出す)。
    lost: bool,
    /// 取り出した通知を接続のスレッドが書いている最中。
    sending: bool,
}

impl Subscriber {
    fn new(events: HashSet<String>, cap: usize) -> Subscriber {
        let state = QueueState { queue: VecDeque::new(), cap: cap.max(1), lost: false, sending: false };
        Subscriber { events, state: Mutex::new(state) }
    }

    /// 購読した名前の通知なら積む。あふれたら一番古いものを捨てる。
    pub fn push(&self, event: &Value) {
        let wanted = event["event"].as_str().is_some_and(|name| self.events.contains(name));
        if !wanted {
            return;
        }
        let mut state = self.state.lock().unwrap();
        if state.queue.len() >= state.cap {
            state.queue.pop_front();
            state.lost = true;
        }
        state.queue.push_back(event.clone());
    }

    /// 積まれている通知を全部取り出す。捨てた分があれば先頭に `resync` を 1 つ付ける。
    /// 取り出したら `finish_sending` を呼ぶこと (終了の合図が、書き終わりまで待てるように)。
    pub fn take(&self) -> Vec<Value> {
        let mut state = self.state.lock().unwrap();
        if state.queue.is_empty() && !state.lost {
            return Vec::new();
        }
        let mut out = Vec::with_capacity(state.queue.len() + 1);
        if std::mem::take(&mut state.lost) {
            out.push(json!({ "event": "resync", "reason": "queue_overflow" }));
        }
        out.extend(state.queue.drain(..));
        state.sending = true;
        out
    }

    pub fn finish_sending(&self) {
        self.state.lock().unwrap().sending = false;
    }

    /// 積まれたものが無く、書いている最中でもない。
    fn is_idle(&self) -> bool {
        let state = self.state.lock().unwrap();
        state.queue.is_empty() && !state.lost && !state.sending
    }
}

/// 引いた位置。ここより新しい分だけを通知にする。
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Cursors {
    revision: i64,
    evidence: i64,
    gap: i64,
    sessions: HashMap<String, String>,
}

impl Cursors {
    /// いまの DB の末尾。購読した時点より前の出来事は通知にしない (取り直しは list で行う)。
    pub fn capture(conn: &Connection) -> rusqlite::Result<Cursors> {
        let tx = conn.unchecked_transaction()?;
        let max = |sql: &str| tx.query_row(sql, [], |r| r.get::<_, i64>(0));
        let revision = max("SELECT COALESCE(MAX(revision_id), 0) FROM change_revisions")?;
        let evidence = max("SELECT COALESCE(MAX(evidence_id), 0) FROM attribution_evidence")?;
        let gap = max("SELECT COALESCE(MAX(gap_id), 0) FROM coverage_gaps")?;
        let sessions = read_sessions(&tx)?;
        Ok(Cursors { revision, evidence, gap, sessions })
    }
}

fn read_sessions(conn: &Connection) -> rusqlite::Result<HashMap<String, String>> {
    let mut stmt = conn.prepare("SELECT session_id, status FROM sessions")?;
    let rows = stmt.query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))?;
    rows.collect()
}

/// 前回の位置から増えた分を通知にして返し、位置を進める。1 つの読み取りトランザクションの中で引く (回をまたいで食い違わない)。
pub fn poll_once(conn: &Connection, cursors: &mut Cursors) -> rusqlite::Result<Vec<Value>> {
    let tx = conn.unchecked_transaction()?;
    let mut events: Vec<Value> = Vec::new();

    // 新しい Revision。この回で知らせるものは、この時点の帰属を持つ
    let revision_before = cursors.revision;
    {
        let mut stmt = tx.prepare(
            "SELECT r.revision_id, r.change_id, c.path, r.change_type, r.actor, r.attribution,
                    COALESCE((SELECT s.document_id FROM scene_changes s WHERE s.revision_id = r.revision_id LIMIT 1), c.document_id)
               FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
              WHERE r.revision_id > ?1 ORDER BY r.revision_id LIMIT ?2",
        )?;
        let rows = stmt.query_map([revision_before, BATCH_LIMIT], |r| {
            let mut event = json!({
                "event": "change_recorded",
                "revisionId": r.get::<_, i64>(0)?,
                "changeId": r.get::<_, i64>(1)?,
                "path": r.get::<_, String>(2)?,
                "changeType": r.get::<_, String>(3)?,
                "actor": r.get::<_, String>(4)?,
                "attribution": r.get::<_, String>(5)?,
            });
            // シーン / プレハブ / アクターのときだけ。Editor が開いているシーンの変更かをすぐ判定できる
            if let Some(document_id) = r.get::<_, Option<String>>(6)? {
                event["documentId"] = json!(document_id);
            }
            Ok((r.get::<_, i64>(0)?, event))
        })?;
        for row in rows {
            let (id, event) = row?;
            cursors.revision = id;
            events.push(event);
        }
    }

    // 知らせ済みの Revision に根拠が足された = 帰属が変わりうる。この回で初めて知らせる Revision の根拠は、上で反映済み
    let mut updated: Vec<i64> = Vec::new();
    {
        let mut stmt = tx.prepare(
            "SELECT evidence_id, revision_id FROM attribution_evidence WHERE evidence_id > ?1 ORDER BY evidence_id LIMIT ?2",
        )?;
        let rows = stmt.query_map([cursors.evidence, BATCH_LIMIT], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?)))?;
        for row in rows {
            let (evidence_id, revision_id) = row?;
            cursors.evidence = evidence_id;
            if revision_id <= revision_before && !updated.contains(&revision_id) {
                updated.push(revision_id);
            }
        }
    }
    for revision_id in updated {
        let found = tx.query_row(
            "SELECT actor, attribution FROM change_revisions WHERE revision_id = ?1",
            [revision_id],
            |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)),
        );
        if let Ok((actor, attribution)) = found {
            events.push(json!({
                "event": "attribution_updated",
                "revisionId": revision_id,
                "actor": actor,
                "attribution": attribution,
            }));
        }
    }

    {
        let mut stmt = tx.prepare(
            "SELECT gap_id, from_at, to_at, reason, scope FROM coverage_gaps WHERE gap_id > ?1 ORDER BY gap_id LIMIT ?2",
        )?;
        let rows = stmt.query_map([cursors.gap, BATCH_LIMIT], |r| {
            Ok((
                r.get::<_, i64>(0)?,
                json!({
                    "event": "coverage_gap",
                    "gapId": r.get::<_, i64>(0)?,
                    "fromAt": r.get::<_, i64>(1)?,
                    "toAt": r.get::<_, i64>(2)?,
                    "reason": r.get::<_, String>(3)?,
                    "scope": r.get::<_, Option<String>>(4)?,
                }),
            ))
        })?;
        for row in rows {
            let (id, event) = row?;
            cursors.gap = id;
            events.push(event);
        }
    }

    let sessions = read_sessions(&tx)?;
    let mut changed: Vec<(&String, &String)> =
        sessions.iter().filter(|(id, status)| cursors.sessions.get(*id) != Some(*status)).collect();
    changed.sort();
    for (session_id, status) in changed {
        events.push(json!({ "event": "session_changed", "sessionId": session_id, "status": status }));
    }
    cursors.sessions = sessions;
    Ok(events)
}

/// 購読の登録簿とポーラの状態。サービス 1 つに 1 つ。
pub struct NotifyHub {
    queue_cap: usize,
    subscribers: Mutex<Vec<Arc<Subscriber>>>,
    /// `Some` の間だけ、ポーラが位置を持っている (購読者が 0 になったら捨てる)。
    cursors: Mutex<Option<Cursors>>,
}

impl NotifyHub {
    pub fn new(queue_cap: usize) -> NotifyHub {
        NotifyHub { queue_cap, subscribers: Mutex::new(Vec::new()), cursors: Mutex::new(None) }
    }

    /// 購読を登録する。最初の購読者のときは `capture` で位置を決める (購読より前の出来事を通知にしない)。
    pub fn register(
        self: &Arc<Self>,
        events: HashSet<String>,
        capture: impl FnOnce() -> Option<Cursors>,
    ) -> Subscription {
        let sub = Arc::new(Subscriber::new(events, self.queue_cap));
        {
            let mut cursors = self.cursors.lock().unwrap();
            let mut subs = self.subscribers.lock().unwrap();
            if subs.is_empty() {
                *cursors = capture();
            }
            subs.push(Arc::clone(&sub));
        }
        Subscription { hub: Arc::clone(self), sub }
    }

    fn unregister(&self, sub: &Arc<Subscriber>) {
        // ロックの順は常に cursors → subscribers (register、poll と揃える)
        let mut cursors = self.cursors.lock().unwrap();
        let mut subs = self.subscribers.lock().unwrap();
        subs.retain(|s| !Arc::ptr_eq(s, sub));
        if subs.is_empty() {
            *cursors = None;
        }
    }

    pub fn subscriber_count(&self) -> usize {
        self.subscribers.lock().unwrap().len()
    }

    /// 全購読者へ通知を積む。待たない。
    pub fn broadcast(&self, event: &Value) {
        for sub in self.subscribers.lock().unwrap().iter() {
            sub.push(event);
        }
    }

    /// 積まれた通知が書き終わるまで、最大 `limit` 待つ (サービスの終了の前に `shutting_down` を届けるため)。
    pub fn wait_drained(&self, limit: Duration) {
        let deadline = Instant::now() + limit;
        while Instant::now() < deadline {
            if self.subscribers.lock().unwrap().iter().all(|s| s.is_idle()) {
                return;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    /// ポーラの 1 回分。購読者がいなければ何もしない。DB を引けなければ位置はそのままで次の回に回す。
    fn poll(&self, conn: &Connection) {
        let mut guard = self.cursors.lock().unwrap();
        let Some(cursors) = guard.as_mut() else {
            // 登録のときに位置を決められなかった (DB を引けなかった) 分を、ここで決め直す
            *guard = Cursors::capture(conn).ok();
            return;
        };
        let mut next = cursors.clone();
        match poll_once(conn, &mut next) {
            Ok(events) => {
                *cursors = next;
                for event in &events {
                    self.broadcast(event);
                }
            }
            Err(e) => eprintln!("[track] 通知のための DB の読み取りに失敗: {e}"),
        }
    }
}

/// 登録した購読。Drop で外れる。
pub struct Subscription {
    hub: Arc<NotifyHub>,
    sub: Arc<Subscriber>,
}

impl Subscription {
    pub fn take(&self) -> Vec<Value> {
        self.sub.take()
    }

    pub fn finish_sending(&self) {
        self.sub.finish_sending();
    }
}

impl Drop for Subscription {
    fn drop(&mut self) {
        self.hub.unregister(&self.sub);
    }
}

/// ポーラのスレッド。購読者がいる間だけ `POLL_INTERVAL` ごとに DB を引き、`stopping` が立ったら終わる。
pub fn spawn_poller(hub: Arc<NotifyHub>, db_path: PathBuf, stopping: Arc<AtomicBool>) -> std::io::Result<()> {
    std::thread::Builder::new().name("mye-track-notify".into()).spawn(move || {
        let mut reader: Option<Connection> = None;
        while !stopping.load(Ordering::SeqCst) {
            std::thread::sleep(POLL_INTERVAL);
            if hub.subscriber_count() == 0 {
                reader = None;
                continue;
            }
            if reader.is_none() {
                reader = ops::open_reader(&db_path).ok();
            }
            if let Some(conn) = reader.as_ref() {
                hub.poll(conn);
            }
        }
    })?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn event(name: &str, n: i64) -> Value {
        json!({ "event": name, "n": n })
    }

    #[test]
    fn queue_drops_oldest_and_emits_one_resync() {
        let sub = Subscriber::new(["change_recorded".to_string()].into_iter().collect(), 3);
        for n in 0..10 {
            sub.push(&event("change_recorded", n));
        }
        let out = sub.take();
        assert_eq!(out.len(), 4, "{out:?}");
        assert_eq!(out[0]["event"], "resync");
        assert_eq!(out.iter().skip(1).map(|e| e["n"].as_i64().unwrap()).collect::<Vec<_>>(), vec![7, 8, 9]);
        sub.finish_sending();
        // 取り出したあとは平常に戻る (resync は 1 回だけ)
        sub.push(&event("change_recorded", 10));
        let again = sub.take();
        assert_eq!(again.len(), 1);
        assert_eq!(again[0]["n"], 10);
    }

    #[test]
    fn unsubscribed_names_are_not_queued() {
        let sub = Subscriber::new(["coverage_gap".to_string()].into_iter().collect(), 3);
        sub.push(&event("change_recorded", 1));
        assert!(sub.take().is_empty());
        assert!(sub.is_idle());
    }

    #[test]
    fn broadcast_reaches_only_registered_subscribers() {
        let hub = Arc::new(NotifyHub::new(4));
        let a = hub.register(["shutting_down".to_string()].into_iter().collect(), || None);
        {
            let b = hub.register(["shutting_down".to_string()].into_iter().collect(), || None);
            assert_eq!(hub.subscriber_count(), 2);
            drop(b);
        }
        assert_eq!(hub.subscriber_count(), 1);
        hub.broadcast(&json!({ "event": "shutting_down", "reason": "x" }));
        assert_eq!(a.take().len(), 1);
        a.finish_sending();
        hub.wait_drained(Duration::from_millis(100));
    }
}

#[cfg(test)]
mod poll_tests {
    use super::*;
    use crate::startup::open_project;

    #[test]
    fn poll_once_reports_new_revisions_evidence_gaps_and_session_changes() {
        let dir = std::env::temp_dir().join(format!("mye_track_notify_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let (store, _report) = open_project(&dir, 1_000);
        let store = store.expect("DB を開けない");
        let conn = store.conn.as_ref().expect("DB 接続");

        let mut cursors = Cursors::capture(conn).unwrap();
        assert!(poll_once(conn, &mut cursors).unwrap().is_empty(), "何も無いのに通知が出た");

        conn.execute("INSERT INTO changes (path, kind, first_seen, state) VALUES ('a.txt', 'text', 1, 'active')", []).unwrap();
        let insert_revision = |seq: i64| {
            conn.execute(
                "INSERT INTO change_revisions (change_id, seq, change_type, observed_at, completeness, actor, attribution, diff_mode)
                 VALUES (1, ?1, 'added', 5, 'complete', 'unknown', 'unknown', 'lines')",
                [seq],
            )
            .unwrap();
            conn.last_insert_rowid()
        };
        let first = insert_revision(1);
        conn.execute(
            "INSERT INTO attribution_evidence (revision_id, kind, detail, recorded_at) VALUES (?1, 'observed', '{}', 6)",
            [first],
        )
        .unwrap();
        let events = poll_once(conn, &mut cursors).unwrap();
        // この回で知らせる Revision の根拠は、change_recorded に含まれる (attribution_updated を重ねない)
        assert_eq!(events.len(), 1, "{events:?}");
        assert_eq!(events[0]["event"], "change_recorded");
        assert_eq!((events[0]["revisionId"].as_i64(), events[0]["path"].as_str()), (Some(first), Some("a.txt")));

        // 知らせ済みの Revision に根拠が足された = attribution_updated
        conn.execute(
            "INSERT INTO attribution_evidence (revision_id, kind, detail, recorded_at) VALUES (?1, 'reported', '{}', 7)",
            [first],
        )
        .unwrap();
        conn.execute("UPDATE change_revisions SET actor = 'ai', attribution = 'claimed' WHERE revision_id = ?1", [first]).unwrap();
        let events = poll_once(conn, &mut cursors).unwrap();
        assert_eq!(events.len(), 1, "{events:?}");
        assert_eq!(events[0]["event"], "attribution_updated");
        assert_eq!((events[0]["actor"].as_str(), events[0]["attribution"].as_str()), (Some("ai"), Some("claimed")));

        conn.execute("INSERT INTO coverage_gaps (from_at, to_at, reason, scope) VALUES (10, 20, 'watcher_overflow', NULL)", []).unwrap();
        conn.execute(
            "INSERT INTO sessions (session_id, provider, source, workspace, started_at, status, coverage)
             VALUES ('s1', 'claude', 'mcp', 'w', 8, 'running', '{}')",
            [],
        )
        .unwrap();
        let events = poll_once(conn, &mut cursors).unwrap();
        let names: Vec<&str> = events.iter().filter_map(|e| e["event"].as_str()).collect();
        assert_eq!(names, vec!["coverage_gap", "session_changed"], "{events:?}");
        assert_eq!(events[0]["reason"], "watcher_overflow");
        assert_eq!((events[1]["sessionId"].as_str(), events[1]["status"].as_str()), (Some("s1"), Some("running")));

        conn.execute("UPDATE sessions SET status = 'finished' WHERE session_id = 's1'", []).unwrap();
        let events = poll_once(conn, &mut cursors).unwrap();
        assert_eq!(events.len(), 1, "{events:?}");
        assert_eq!(events[0]["status"], "finished");
        assert!(poll_once(conn, &mut cursors).unwrap().is_empty(), "同じ通知を二度出した");

        // 購読より前の出来事は、位置を決め直せば通知にならない
        let mut fresh = Cursors::capture(conn).unwrap();
        assert!(poll_once(conn, &mut fresh).unwrap().is_empty());
        drop(store);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
