//! セッション (`sessions`) の派生の表への反映と、寿命 (中断・放棄) の管理。
//!
//! セッションと操作の op は `record_and_apply` で `events` へ記録し、同じ関数で派生の表へ反映する。
//! `rebuild_derived` も `apply_event` を通るので、作り直した DB は元の DB と同じになる (spec D26)。
//! 寿命の判定 (プロセスの生死、無活動、操作の打ち切り) もイベント (`session_interrupted` / `op_abandoned`) にする。

use std::time::Duration;

use rusqlite::{params, Connection, OptionalExtension};
use serde_json::{json, Value};

use crate::attribution::{self, AttributionConfig};
use crate::blob::BlobStore;
use crate::operation;
use crate::process::is_alive;
use crate::store::{Store, StoreError};

/// セッションの寿命の設定。既定は sub-06 の決定値で、試験は短くできる。
#[derive(Clone, Debug)]
pub struct LifecycleConfig {
    /// プロセスを持たないセッション (フック) が、最後の活動からこれだけ無活動なら `interrupted`。
    pub hook_idle_ms: i64,
    /// `op_end` が来ない操作を `abandoned` にするまで。
    pub op_timeout_ms: i64,
    /// 寿命を確かめる間隔。
    pub check_interval: Duration,
}

impl Default for LifecycleConfig {
    fn default() -> Self {
        LifecycleConfig {
            hook_idle_ms: 30 * 60 * 1000,
            op_timeout_ms: 10 * 60 * 1000,
            check_interval: Duration::from_secs(1),
        }
    }
}

/// セッションの最後の活動時刻を `coverage` に残す (無活動の判定に使う)。
pub fn touch(conn: &Connection, session_id: &str, at: i64) -> rusqlite::Result<()> {
    conn.execute(
        "UPDATE sessions SET coverage = json_set(CASE WHEN json_valid(coverage) THEN coverage ELSE '{}' END, '$.lastActivityAt', ?1)
          WHERE session_id = ?2",
        params![at, session_id],
    )?;
    Ok(())
}

/// セッション・操作のイベントを派生の表へ反映する。対象外の kind は何もしない。`pos` はそのイベントのジャーナルの位置。
pub fn apply_event(
    conn: &Connection,
    blobs: &BlobStore,
    cfg: &AttributionConfig,
    kind: &str,
    at: i64,
    payload: &Value,
    pos: &str,
) -> rusqlite::Result<()> {
    match kind {
        "session_begin" => session_begin(conn, cfg, at, payload, pos),
        "session_resumed" => session_resumed(conn, at, payload),
        "session_end" => {
            if let Some(id) = payload["session_id"].as_str() {
                conn.execute(
                    "UPDATE sessions SET status = 'finished', ended_at = ?1 WHERE session_id = ?2 AND status <> 'finished'",
                    params![at, id],
                )?;
                touch(conn, id, at)?;
            }
            Ok(())
        }
        "session_interrupted" => {
            if let Some(id) = payload["session_id"].as_str() {
                conn.execute("UPDATE sessions SET status = 'interrupted' WHERE session_id = ?1 AND status = 'running'", [id])?;
            }
            Ok(())
        }
        "review_set" => crate::review::apply_event(conn, at, payload),
        "editor_saved" => match (payload["path"].as_str(), payload["hash"].as_str()) {
            (Some(path), Some(hash)) => attribution::apply_editor_saved(conn, at, path, hash),
            _ => Ok(()),
        },
        _ => operation::apply(conn, blobs, cfg, kind, at, payload).map(|_| ()),
    }
}

/// この kind のイベントを `apply_event` が反映するか。
pub fn is_session_event(kind: &str) -> bool {
    matches!(
        kind,
        "session_begin"
            | "session_resumed"
            | "session_end"
            | "session_interrupted"
            | "op_begin"
            | "op_end"
            | "op_abandoned"
            | "reported"
            | "editor_saved"
            | "review_set"
    )
}

fn session_begin(conn: &Connection, cfg: &AttributionConfig, at: i64, p: &Value, pos: &str) -> rusqlite::Result<()> {
    let (Some(id), Some(provider), Some(source), Some(workspace)) = (
        p["session_id"].as_str(),
        p["provider"].as_str(),
        p["source"].as_str(),
        p["workspace"].as_str(),
    ) else {
        return Ok(());
    };
    // セッションの基準は、開始の時刻とジャーナルの位置だけ (全ファイルのハッシュの写しは持たない。spec D30)
    let coverage = json!({
        "startedAt": at,
        "journalPos": pos,
        "lastActivityAt": at,
        "isolated": p["isolated"].as_bool().unwrap_or(false),
        "worktreeEligible": p["worktree_eligible"].as_bool().unwrap_or(false),
        "workspaceRel": p["workspace_rel"],
        "capabilities": p["capabilities"],
    });
    conn.execute(
        "INSERT OR IGNORE INTO sessions
           (session_id, provider, source, client_session_id, process_id, process_start, workspace, base_commit,
            started_at, ended_at, status, coverage)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, NULL, 'running', ?10)",
        params![
            id,
            provider,
            source,
            p["client_session_id"].as_str(),
            p["process_id"].as_i64(),
            p["process_start"].as_i64(),
            workspace,
            p["base_commit"].as_str(),
            at,
            coverage.to_string(),
        ],
    )?;
    // 開始のすぐ前の変更は、このセッションより前からあった変更として根拠に残す
    attribution::reevaluate_range(conn, cfg, at, at - cfg.time_near_ms, at, None)
}

fn session_resumed(conn: &Connection, at: i64, p: &Value) -> rusqlite::Result<()> {
    let Some(id) = p["session_id"].as_str() else { return Ok(()) };
    conn.execute(
        "UPDATE sessions SET status = 'running', ended_at = NULL,
                process_id = COALESCE(?1, process_id), process_start = COALESCE(?2, process_start)
          WHERE session_id = ?3 AND status <> 'finished'",
        params![p["process_id"].as_i64(), p["process_start"].as_i64(), id],
    )?;
    touch(conn, id, at)
}

/// イベントを記録し (ジャーナルが先)、同じ内容を派生の表へ反映する。反映は 1 つの塊で、失敗したら何も残さない。
/// 失敗してもイベントは残るので、次の作り直しで反映される。
pub fn record_and_apply(store: &mut Store, at: i64, kind: &str, payload: Value) -> Result<(), StoreError> {
    let recorded = store.record_event(at, kind, payload.clone())?;
    let Some(conn) = store.conn.as_ref() else {
        return Err(StoreError::Degraded("DB 接続が無い".to_string()));
    };
    conn.execute_batch("SAVEPOINT record_and_apply")?;
    match apply_event(conn, &store.blobs, &store.attribution, kind, at, &payload, &recorded.pos.to_string()) {
        Ok(()) => {
            conn.execute_batch("RELEASE record_and_apply")?;
            Ok(())
        }
        Err(e) => {
            let _ = conn.execute_batch("ROLLBACK TO record_and_apply; RELEASE record_and_apply");
            Err(e.into())
        }
    }
}

/// そのセッションが候補に入っている Revision を選ぶ条件 (`change_revisions r` に対して、`?{n}` がセッション ID)。
/// 結論が 1 つの操作に絞れていればその操作で、複数候補・操作なし (申告・時刻・作業ツリー) は根拠のセッションで見る。
/// Editor の保存・復元と結論が出た変更は、AI のセッションの変更に入れない (根拠が残っていても)。
pub fn session_candidate_sql(n: usize) -> String {
    format!(
        "(r.operation_id IN (SELECT operation_id FROM operations WHERE session_id = ?{n})
          OR (r.operation_id IS NULL AND r.actor NOT IN ('editor_user', 'restore') AND EXISTS (SELECT 1 FROM attribution_evidence e
               WHERE e.revision_id = r.revision_id AND e.session_id = ?{n}
                 AND e.kind NOT IN ('preexisting', 'observed', 'overlap'))))"
    )
}

/// そのセッションが候補に入っている Revision の数。
pub fn change_count(conn: &Connection, session_id: &str) -> rusqlite::Result<i64> {
    conn.query_row(
        &format!("SELECT COUNT(*) FROM change_revisions r WHERE {}", session_candidate_sql(1)),
        [session_id],
        |r| r.get(0),
    )
}

/// セッションの探し方。
pub enum SessionKey<'a> {
    Id(&'a str),
    /// クライアント側の ID。同じ ID のうち、終了していない最新のもの (無ければ最新のもの)。
    Client(&'a str),
}

/// 見つかったセッションの要約。
pub struct SessionRow {
    pub session_id: String,
    pub status: String,
    pub started_at: i64,
    pub provider: String,
    pub workspace: String,
}

pub fn find_session(conn: &Connection, key: SessionKey<'_>) -> rusqlite::Result<Option<SessionRow>> {
    let map = |r: &rusqlite::Row<'_>| {
        Ok(SessionRow { session_id: r.get(0)?, status: r.get(1)?, started_at: r.get(2)?, provider: r.get(3)?, workspace: r.get(4)? })
    };
    match key {
        SessionKey::Id(id) => conn
            .query_row("SELECT session_id, status, started_at, provider, workspace FROM sessions WHERE session_id = ?1", [id], map)
            .optional(),
        SessionKey::Client(client) => conn
            .query_row(
                "SELECT session_id, status, started_at, provider, workspace FROM sessions WHERE client_session_id = ?1
                  ORDER BY (status <> 'finished') DESC, started_at DESC, session_id DESC LIMIT 1",
                [client],
                map,
            )
            .optional(),
    }
}

/// 寿命の確認の結果。
#[derive(Debug, Default, PartialEq, Eq)]
pub struct TickReport {
    pub interrupted: usize,
    pub abandoned: usize,
}

/// プロセスの生死の判定 (PID と開始時刻の組)。試験が差し替える。
pub type AliveFn<'a> = &'a dyn Fn(u32, Option<i64>) -> bool;

/// 寿命を確かめる (Writer の中で動く)。プロセスの居ないセッションと、無活動のフックのセッションを `interrupted` に、
/// 打ち切りの時間を過ぎた操作を `abandoned` にする。接続が切れただけでは何も変えない。
pub fn tick(store: &mut Store, now: i64, cfg: &LifecycleConfig, alive: AliveFn<'_>) -> Result<TickReport, StoreError> {
    let mut report = TickReport::default();
    let Some(conn) = store.conn.as_ref() else { return Ok(report) };
    let running: Vec<(String, Option<i64>, Option<i64>, i64)> = {
        let mut stmt = conn.prepare_cached(
            "SELECT session_id, process_id, process_start,
                    COALESCE(json_extract(CASE WHEN json_valid(coverage) THEN coverage ELSE '{}' END, '$.lastActivityAt'), started_at)
               FROM sessions WHERE status = 'running' ORDER BY session_id",
        )?;
        let rows = stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    let mut interrupts: Vec<(String, &'static str)> = Vec::new();
    for (id, pid, start, last) in running {
        match pid {
            Some(pid) => {
                if !u32::try_from(pid).is_ok_and(|p| alive(p, start)) {
                    interrupts.push((id, "process_exited"));
                }
            }
            None => {
                if last + cfg.hook_idle_ms < now {
                    interrupts.push((id, "idle"));
                }
            }
        }
    }
    let stale_ops: Vec<String> = {
        let mut stmt = conn.prepare_cached(
            "SELECT operation_id FROM operations WHERE finished_at IS NULL AND result IS NULL AND started_at + ?1 < ?2
              ORDER BY started_at, operation_id",
        )?;
        let rows = stmt.query_map(params![cfg.op_timeout_ms, now], |r| r.get(0))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    for (id, reason) in interrupts {
        record_and_apply(store, now, "session_interrupted", json!({ "session_id": id, "reason": reason }))?;
        report.interrupted += 1;
    }
    for id in stale_ops {
        record_and_apply(store, now, "op_abandoned", json!({ "operation_id": id, "reason": "timeout" }))?;
        report.abandoned += 1;
    }
    Ok(report)
}

/// 起動時の整理 (db-schema.md §6 の 5、6): 前の起動から残った running のセッションのうちプロセスの居ないもの
/// (プロセスを持たないものは確かめられないので中断側)、終わっていない操作を、イベントにして反映する。
pub fn startup_cleanup(store: &mut Store, now: i64) -> Result<TickReport, StoreError> {
    let mut report = TickReport::default();
    let Some(conn) = store.conn.as_ref() else { return Ok(report) };
    let running: Vec<(String, Option<i64>, Option<i64>)> = {
        let mut stmt = conn.prepare("SELECT session_id, process_id, process_start FROM sessions WHERE status = 'running' ORDER BY session_id")?;
        let rows = stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    let dead: Vec<String> = running
        .into_iter()
        .filter(|(_, pid, start)| !pid.is_some_and(|p| u32::try_from(p).is_ok_and(|p| is_alive(p, *start))))
        .map(|(id, _, _)| id)
        .collect();
    let open_ops: Vec<String> = {
        let mut stmt = conn.prepare("SELECT operation_id FROM operations WHERE finished_at IS NULL AND result IS NULL ORDER BY started_at, operation_id")?;
        let rows = stmt.query_map([], |r| r.get(0))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    for id in dead {
        record_and_apply(store, now, "session_interrupted", json!({ "session_id": id, "reason": "startup" }))?;
        report.interrupted += 1;
    }
    for id in open_ops {
        record_and_apply(store, now, "op_abandoned", json!({ "operation_id": id, "reason": "startup" }))?;
        report.abandoned += 1;
    }
    Ok(report)
}
