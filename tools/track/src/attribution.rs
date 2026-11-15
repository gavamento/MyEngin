//! 変更の帰属 (attribution-rules.md)。根拠の種類、結論の関数 `conclude`、DB の根拠からの計算し直し。
//!
//! 結論は根拠の集合だけから決まる純粋な関数 (`conclude`)。根拠は消さず、増えるたびに結論を計算し直す。
//! 根拠を作る入力はすべてイベントから導ける派生の表 (`sessions` / `operations` / `change_revisions`) だけで、
//! 通常の記録も `rebuild_derived` も同じ関数を通る (spec D26)。時計は読まず、イベントの時刻 `at` を使う。

use std::collections::{BTreeSet, HashSet};

use rusqlite::{params, Connection, OptionalExtension};
use serde_json::{json, Value};

/// `op_end` の後も、この間に観測された変更は操作の窓に入れる (監視のイベントは `op_end` より遅れうる)。
pub const DEFAULT_WINDOW_GRACE_MS: i64 = 2000;
/// `time_near` の幅。広げても `inferred` より上にはならない。
pub const DEFAULT_TIME_NEAR_MS: i64 = 2000;
/// `report` より後に観測された変更も、この間は申告の対象にする (監視のデバウンスで遅れる)。
pub const REPORT_LOOKAHEAD_MS: i64 = 10_000;
/// `report` より前の変更を申告の対象にする遡り幅。
pub const REPORT_BACKLOG_MS: i64 = 10 * 60 * 1000;
/// `editor_saved` の知らせと、Revision の観測時刻の許容差 (前後とも)。知らせが先に届いても、観測が先でも成り立つ。
/// 監視のデバウンスと pipe の遅れより十分に長く、別の変更を巻き込むほどは長くしない。
pub const EDITOR_SAVED_WINDOW_MS: i64 = 10_000;
/// `restore_expect` の予告は、書き込みの前に届く。書き終わって観測されるまでの猶予 (大きいファイルの書き込みと監視のデバウンス) と、
/// 観測が予告より少し先になりうる幅。内容とパスが同じ変更だけに効くので、`editor_saved` より長くてよい。
pub const RESTORE_EXPECT_BEFORE_MS: i64 = 120_000;
pub const RESTORE_EXPECT_AFTER_MS: i64 = 10_000;

/// 帰属の計算の設定。試験が短くできる。
#[derive(Clone, Debug)]
pub struct AttributionConfig {
    pub window_grace_ms: i64,
    pub time_near_ms: i64,
}

impl Default for AttributionConfig {
    fn default() -> Self {
        AttributionConfig { window_grace_ms: DEFAULT_WINDOW_GRACE_MS, time_near_ms: DEFAULT_TIME_NEAR_MS }
    }
}

/// 根拠の種類 (attribution-rules.md §2 の 12 種)。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Kind {
    Observed,
    OpWindow,
    BeforeMatch,
    DeclaredEditMatch,
    ReportHashMatch,
    ReportOnly,
    Worktree,
    TimeNear,
    EditorSaved,
    RestoreExpected,
    Overlap,
    Preexisting,
}

impl Kind {
    pub fn as_str(self) -> &'static str {
        match self {
            Kind::Observed => "observed",
            Kind::OpWindow => "op_window",
            Kind::BeforeMatch => "before_match",
            Kind::DeclaredEditMatch => "declared_edit_match",
            Kind::ReportHashMatch => "report_hash_match",
            Kind::ReportOnly => "report_only",
            Kind::Worktree => "worktree",
            Kind::TimeNear => "time_near",
            Kind::EditorSaved => "editor_saved",
            Kind::RestoreExpected => "restore_expected",
            Kind::Overlap => "overlap",
            Kind::Preexisting => "preexisting",
        }
    }

    pub fn parse(text: &str) -> Option<Kind> {
        Some(match text {
            "observed" => Kind::Observed,
            "op_window" => Kind::OpWindow,
            "before_match" => Kind::BeforeMatch,
            "declared_edit_match" => Kind::DeclaredEditMatch,
            "report_hash_match" => Kind::ReportHashMatch,
            "report_only" => Kind::ReportOnly,
            "worktree" => Kind::Worktree,
            "time_near" => Kind::TimeNear,
            "editor_saved" => Kind::EditorSaved,
            "restore_expected" => Kind::RestoreExpected,
            "overlap" => Kind::Overlap,
            "preexisting" => Kind::Preexisting,
            _ => return None,
        })
    }
}

/// 根拠 1 件。`session` は候補のセッション (どのセッションにも属さない根拠は None)。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Evidence {
    pub kind: Kind,
    pub session: Option<String>,
    pub operation: Option<String>,
}

impl Evidence {
    pub fn new(kind: Kind, session: Option<&str>, operation: Option<&str>) -> Evidence {
        Evidence { kind, session: session.map(str::to_string), operation: operation.map(str::to_string) }
    }
}

/// 結論。`candidates` は候補のセッション (actor が ai のときだけ入る)。`operation` は候補が 1 つのときの操作。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Conclusion {
    pub actor: &'static str,
    pub attribution: &'static str,
    pub candidates: Vec<String>,
    pub operation: Option<String>,
}

impl Conclusion {
    fn plain(actor: &'static str, attribution: &'static str) -> Conclusion {
        Conclusion { actor, attribution, candidates: Vec::new(), operation: None }
    }
}

/// 根拠を持つセッションの一覧 (重複なし、整列済み)。
fn sessions_with(evidence: &[&Evidence], kind: Kind) -> Vec<String> {
    let set: BTreeSet<String> =
        evidence.iter().filter(|e| e.kind == kind).filter_map(|e| e.session.clone()).collect();
    set.into_iter().collect()
}

/// 候補が 1 つのセッションに絞れたとき、結び付ける操作 (強い根拠の操作を優先する)。
fn operation_for(evidence: &[&Evidence], session: &str) -> Option<String> {
    const PRIORITY: [Kind; 3] = [Kind::DeclaredEditMatch, Kind::ReportHashMatch, Kind::OpWindow];
    for kind in PRIORITY.into_iter().chain([Kind::ReportOnly]) {
        let found = evidence
            .iter()
            .find(|e| e.kind == kind && e.session.as_deref() == Some(session) && e.operation.is_some());
        if let Some(e) = found {
            return e.operation.clone();
        }
    }
    None
}

/// 根拠の集合から結論を決める。attribution-rules.md §3 の表を上から順に当て、§5 の規則を含む。
pub fn conclude(all: &[Evidence]) -> Conclusion {
    let has = |kind: Kind| all.iter().any(|e| e.kind == kind);
    if has(Kind::EditorSaved) {
        return Conclusion::plain("editor_user", "verified");
    }
    if has(Kind::RestoreExpected) {
        return Conclusion::plain("restore", "verified");
    }
    // セッションに属さない preexisting (止まっていた間・取りこぼし) は、ほかの根拠より強い
    if all.iter().any(|e| e.kind == Kind::Preexisting && e.session.is_none()) {
        return Conclusion::plain("unknown", "unknown");
    }
    // 自分の開始より前の変更だと分かっているセッションは、候補から外す
    let excluded: HashSet<&str> =
        all.iter().filter(|e| e.kind == Kind::Preexisting).filter_map(|e| e.session.as_deref()).collect();
    let ev: Vec<&Evidence> =
        all.iter().filter(|e| e.session.as_deref().is_none_or(|s| !excluded.contains(s))).collect();

    let declared = sessions_with(&ev, Kind::DeclaredEditMatch);
    let report_match = sessions_with(&ev, Kind::ReportHashMatch);
    let window = sessions_with(&ev, Kind::OpWindow);
    let overlap = ev.iter().any(|e| e.kind == Kind::Overlap);

    let ai = |attribution: &'static str, candidates: Vec<String>| -> Conclusion {
        let operation = match candidates.as_slice() {
            [only] => operation_for(&ev, only),
            _ => None,
        };
        Conclusion { actor: "ai", attribution, candidates, operation }
    };
    let union = |lists: &[&Vec<String>]| -> Vec<String> {
        let set: BTreeSet<String> = lists.iter().flat_map(|l| l.iter().cloned()).collect();
        set.into_iter().collect()
    };

    if overlap {
        // 窓が重なっている間は Verified にしない (§5)。内容の照合で 1 つに絞れるときだけ候補を絞る
        if declared.len() == 1 {
            return ai("claimed", declared);
        }
        if declared.is_empty() && report_match.len() == 1 {
            return ai("claimed", report_match);
        }
        let candidates = union(&[&window, &declared, &report_match]);
        return if candidates.is_empty() { Conclusion::plain("unknown", "unknown") } else { ai("unknown", candidates) };
    }

    // 4: 宣言した編集そのものの結果と一致した
    match declared.len() {
        0 => {}
        1 => return ai("verified", declared),
        _ => return ai("unknown", declared),
    }
    // 5: 専用の作業ツリーのセッションが 1 つだけ
    let worktree = sessions_with(&ev, Kind::Worktree);
    if worktree.len() == 1 {
        return ai("verified", worktree);
    }
    // 6: 申告のハッシュ一致、または窓 + Before の一致
    let window_and_before: Vec<String> = sessions_with(&ev, Kind::BeforeMatch)
        .into_iter()
        .filter(|s| window.contains(s))
        .collect();
    let claimed = union(&[&report_match, &window_and_before]);
    if !claimed.is_empty() {
        let attribution = if claimed.len() == 1 { "claimed" } else { "unknown" };
        return ai(attribution, claimed);
    }
    // 7: 申告はあるが照合できなかった
    let report_only = sessions_with(&ev, Kind::ReportOnly);
    if !report_only.is_empty() {
        let attribution = if report_only.len() == 1 { "claimed" } else { "unknown" };
        return ai(attribution, report_only);
    }
    // 8: 時刻が近いだけ
    let near = sessions_with(&ev, Kind::TimeNear);
    if !near.is_empty() {
        return ai("inferred", near);
    }
    Conclusion::plain("unknown", "unknown")
}

/// 内容のハッシュの状態。`Unknown` は照合に使えない (除外パターンのファイル、Before を見ていない変更)。
#[derive(Clone, Debug, PartialEq, Eq)]
enum HashState {
    Absent,
    Hash(String),
    Unknown,
}

impl HashState {
    /// `op_begin` で取ったハッシュ (無ければ None) と同じ内容か。
    fn equals(&self, taken: Option<&str>) -> bool {
        match (self, taken) {
            (HashState::Absent, None) => true,
            (HashState::Hash(a), Some(b)) => a == b,
            _ => false,
        }
    }
}

fn snapshot_hash(conn: &Connection, snapshot: Option<i64>) -> rusqlite::Result<HashState> {
    let Some(id) = snapshot else { return Ok(HashState::Absent) };
    let hash: Option<Option<String>> = conn
        .query_row("SELECT blob_hash FROM snapshots WHERE snapshot_id = ?1", [id], |r| r.get(0))
        .optional()?;
    Ok(match hash {
        Some(Some(h)) => HashState::Hash(h),
        _ => HashState::Unknown,
    })
}

struct RevisionRow {
    id: i64,
    path: String,
    old_path: Option<String>,
    observed_at: i64,
    completeness: String,
    before: HashState,
    after: HashState,
}

fn load_revision(conn: &Connection, revision_id: i64) -> rusqlite::Result<Option<RevisionRow>> {
    let row: Option<(String, Option<String>, i64, String, Option<i64>, Option<i64>)> = conn
        .query_row(
            "SELECT c.path, r.old_path, r.observed_at, r.completeness, r.before_snapshot, r.after_snapshot
               FROM change_revisions r JOIN changes c ON c.change_id = r.change_id WHERE r.revision_id = ?1",
            [revision_id],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?, r.get(5)?)),
        )
        .optional()?;
    let Some((path, old_path, observed_at, completeness, before, after)) = row else { return Ok(None) };
    let before = if completeness == "no_before" { HashState::Unknown } else { snapshot_hash(conn, before)? };
    let after = snapshot_hash(conn, after)?;
    Ok(Some(RevisionRow { id: revision_id, path, old_path, observed_at, completeness, before, after }))
}

/// 操作の対象パスと、`op_begin` で取ったハッシュ (無ければ None)。
type Targets = Vec<(String, Option<String>)>;

fn parse_targets(value: &Value) -> Targets {
    value["targets"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|t| Some((t["path"].as_str()?.to_string(), t["hash"].as_str().map(str::to_string))))
        .collect()
}

struct OpRow {
    id: String,
    session: String,
    /// `op_end` の ok で閉じた操作か。窓の根拠を数えてよいのはこれだけ (spec D46)
    counted: bool,
    targets: Targets,
    declared_after: Option<String>,
    declared_path: Option<String>,
}

fn op_row(id: String, session: String, result: Option<String>, raw: &str) -> OpRow {
    let json: Value = serde_json::from_str(raw).unwrap_or(Value::Null);
    OpRow {
        id,
        session,
        counted: result.as_deref() == Some("ok"),
        targets: parse_targets(&json),
        declared_after: json["declaredAfterHash"].as_str().map(str::to_string),
        declared_path: json["declaredPath"].as_str().map(str::to_string),
    }
}

/// 根拠を足す。同じ (種類, セッション, 操作) は 1 度だけ。
struct Sink<'a> {
    conn: &'a Connection,
    revision_id: i64,
    at: i64,
    existing: HashSet<(Kind, Option<String>, Option<String>)>,
}

impl Sink<'_> {
    fn add(&mut self, kind: Kind, session: Option<&str>, operation: Option<&str>, detail: Value) -> rusqlite::Result<()> {
        let key = (kind, session.map(str::to_string), operation.map(str::to_string));
        if !self.existing.insert(key) {
            return Ok(());
        }
        self.conn.execute(
            "INSERT INTO attribution_evidence (revision_id, session_id, operation_id, kind, detail, recorded_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![self.revision_id, session, operation, kind.as_str(), detail.to_string(), self.at],
        )?;
        Ok(())
    }

    fn has_any(&self, kinds: &[Kind]) -> bool {
        self.existing.iter().any(|(k, _, _)| kinds.contains(k))
    }
}

/// 操作の窓に基づく根拠の種類。その操作が `op_end` の ok で閉じたときだけ結論に数える (spec D46)。
pub const WINDOW_KINDS_SQL: &str = "'op_window', 'before_match', 'declared_edit_match'";

/// 結論に数える根拠だけを読む。窓の根拠は、操作が ok で閉じていなければ数えない
/// (開いたまま・失敗・拒否・打ち切りの操作の窓で起きた人の編集を AI のものにしない)。行そのものは監査のために残る。
fn load_evidence(conn: &Connection, revision_id: i64) -> rusqlite::Result<Vec<Evidence>> {
    let mut stmt = conn.prepare_cached(&format!(
        "SELECT e.kind, e.session_id, e.operation_id FROM attribution_evidence e
           LEFT JOIN operations o ON o.operation_id = e.operation_id
          WHERE e.revision_id = ?1 AND (e.kind NOT IN ({WINDOW_KINDS_SQL}) OR o.result = 'ok')
          ORDER BY e.evidence_id"
    ))?;
    let rows = stmt.query_map([revision_id], |r| {
        Ok((r.get::<_, String>(0)?, r.get::<_, Option<String>>(1)?, r.get::<_, Option<String>>(2)?))
    })?;
    let mut out = Vec::new();
    for row in rows {
        let (kind, session, operation) = row?;
        if let Some(kind) = Kind::parse(&kind) {
            out.push(Evidence { kind, session, operation });
        }
    }
    Ok(out)
}

/// いまの根拠からの結論 (`describe` が候補のセッションを出すために使う)。
pub fn current_conclusion(conn: &Connection, revision_id: i64) -> rusqlite::Result<Conclusion> {
    Ok(conclude(&load_evidence(conn, revision_id)?))
}

/// 通常の記録と作り直しの両方が、Revision を作った直後に呼ぶ。`source` は観測の出どころ (`observed` の payload)。
/// `events_through` は、`editor_saved` の知らせとして数える最後の `event_id` (None = 全部)。作り直しは全イベントを
/// 先に入れてあるので、いま処理しているイベントまでに絞らないと、記録したときと根拠の入る順が変わる。
pub fn evaluate_new_revision(
    conn: &Connection,
    cfg: &AttributionConfig,
    revision_id: i64,
    at: i64,
    source: &str,
    events_through: Option<i64>,
) -> rusqlite::Result<()> {
    evaluate(conn, cfg, revision_id, at, Some(source), events_through)
}

/// 根拠が増えうる出来事 (セッション・操作・申告) の後に、Revision の結論を計算し直す。
pub fn reevaluate(conn: &Connection, cfg: &AttributionConfig, revision_id: i64, at: i64) -> rusqlite::Result<()> {
    evaluate(conn, cfg, revision_id, at, None, None)
}

/// `observed_at` が [lo, hi] の Revision (`paths` があればそのパスだけ) を計算し直す。
pub fn reevaluate_range(
    conn: &Connection,
    cfg: &AttributionConfig,
    at: i64,
    lo: i64,
    hi: i64,
    paths: Option<&[String]>,
) -> rusqlite::Result<()> {
    let ids: Vec<(i64, String, Option<String>)> = {
        let mut stmt = conn.prepare_cached(
            "SELECT r.revision_id, c.path, r.old_path FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
              WHERE r.observed_at BETWEEN ?1 AND ?2 ORDER BY r.revision_id",
        )?;
        let rows = stmt.query_map([lo, hi], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    for (id, path, old_path) in ids {
        if let Some(paths) = paths {
            if !paths.iter().any(|p| p.eq_ignore_ascii_case(&path) || old_path.as_deref().is_some_and(|o| o.eq_ignore_ascii_case(p))) {
                continue;
            }
        }
        reevaluate(conn, cfg, id, at)?;
    }
    Ok(())
}

fn evaluate(
    conn: &Connection,
    cfg: &AttributionConfig,
    revision_id: i64,
    at: i64,
    source: Option<&str>,
    events_through: Option<i64>,
) -> rusqlite::Result<()> {
    let fresh = source.is_some();
    // セッションが 1 つも無い (普通の記録の大半) 新しい Revision は、帰属の結論が既定 (unknown / unknown) のまま。
    // 根拠の読み直しと結論の UPDATE を省く (一括の変更で監視が CPU を食わないように)
    let no_sessions = fresh && conn.query_row("SELECT NOT EXISTS (SELECT 1 FROM sessions)", [], |r| r.get::<_, bool>(0))?;
    let Some(rev) = load_revision(conn, revision_id)? else { return Ok(()) };
    let mut sink = Sink { conn, revision_id, at, existing: HashSet::new() };
    let mut source_text = source.map(str::to_string);
    if !fresh {
        let mut stmt = conn.prepare_cached("SELECT kind, session_id, operation_id, detail FROM attribution_evidence WHERE revision_id = ?1")?;
        let rows = stmt.query_map([revision_id], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, Option<String>>(1)?, r.get::<_, Option<String>>(2)?, r.get::<_, String>(3)?))
        })?;
        for row in rows {
            let (kind, session, operation, detail) = row?;
            let Some(kind) = Kind::parse(&kind) else { continue };
            if kind == Kind::Observed {
                source_text = serde_json::from_str::<Value>(&detail).ok().and_then(|v| v["source"].as_str().map(str::to_string));
            }
            sink.existing.insert((kind, session, operation));
        }
    }
    let managed_begin = source_text.as_deref() == Some("managed_begin");
    sink.add(
        Kind::Observed,
        None,
        None,
        json!({
            "observedAt": rev.observed_at,
            "completeness": rev.completeness,
            "source": source_text,
            "beforeHash": hash_json(&rev.before),
            "afterHash": hash_json(&rev.after),
        }),
    )?;
    let verify_gap = rev.completeness == "gap" && source_text.as_deref() == Some("verify_gap");
    // 先に届いていた Editor の保存の知らせ。後から届く知らせは `apply_editor_saved` が足す
    if fresh {
        if let Some(notice_at) = editor_saved_notice(conn, &rev, events_through.unwrap_or(i64::MAX))? {
            sink.add(Kind::EditorSaved, None, None, editor_saved_detail(&rev, notice_at))?;
        }
        // 復元の予告 (restore_expect)。Editor が書く前に知らせてくる
        if let Some(notice_at) = restore_expect_notice(conn, &rev, events_through.unwrap_or(i64::MAX))? {
            sink.add(Kind::RestoreExpected, None, None, restore_expect_detail(&rev, notice_at))?;
        }
    }
    if rev.completeness == "gap" && !verify_gap {
        // 止まっていた間 (起動時の再走査)・監視のあふれの変更は、その間にセッションが動いていても見ていない (§4)
        sink.add(Kind::Preexisting, None, None, json!({ "reason": "gap" }))?;
    } else {
        // 動いている間の取りこぼしを検証の走査が拾った変更 (`verify_gap`) は、観測の時刻が走査の時刻で
        // 書かれた時刻ではないので、時刻による根拠を作らず、内容による根拠だけを数える (spec D35)
        session_evidence(conn, cfg, &rev, managed_begin, verify_gap, &mut sink)?;
    }
    if no_sessions && rev.completeness != "gap" && !sink.has_any(&[Kind::EditorSaved, Kind::RestoreExpected]) {
        return Ok(());
    }
    recompute_conclusion(conn, revision_id)
}

/// 根拠の集合から結論を計算し直して Revision へ書く。
fn recompute_conclusion(conn: &Connection, revision_id: i64) -> rusqlite::Result<()> {
    let evidence = load_evidence(conn, revision_id)?;
    let conclusion = conclude(&evidence);
    conn.execute(
        "UPDATE change_revisions SET actor = ?1, attribution = ?2, operation_id = ?3 WHERE revision_id = ?4",
        params![conclusion.actor, conclusion.attribution, conclusion.operation, revision_id],
    )?;
    Ok(())
}

fn editor_saved_detail(rev: &RevisionRow, notice_at: i64) -> Value {
    json!({ "path": rev.path, "hash": hash_json(&rev.after), "noticeAt": notice_at })
}

/// この Revision の After と同じ内容の `editor_saved` の知らせ (event_id が `through` 以下、観測時刻の前後
/// `EDITOR_SAVED_WINDOW_MS` 以内)。あればその知らせの時刻。`gap` の Revision は観測の時刻が書いた時刻ではないので対象外。
fn editor_saved_notice(conn: &Connection, rev: &RevisionRow, through: i64) -> rusqlite::Result<Option<i64>> {
    let HashState::Hash(after) = &rev.after else { return Ok(None) };
    if rev.completeness == "gap" {
        return Ok(None);
    }
    let (lo, hi) = (rev.observed_at - EDITOR_SAVED_WINDOW_MS, rev.observed_at + EDITOR_SAVED_WINDOW_MS);
    conn.query_row(
        "SELECT at FROM events
          WHERE kind = 'editor_saved' AND at BETWEEN ?1 AND ?2 AND event_id <= ?3
            AND json_extract(payload, '$.hash') = ?4 AND json_extract(payload, '$.path') = ?5 COLLATE NOCASE
          ORDER BY event_id LIMIT 1",
        params![lo, hi, through, after, rev.path],
        |r| r.get(0),
    )
    .optional()
}

/// `editor_saved` のイベントを派生の表へ反映する: 観測が先に届いていた Revision に根拠を足して結論を計算し直す。
/// 観測が後から届く場合は、新しい Revision の評価 (`editor_saved_notice`) が同じ根拠を足す。
pub fn apply_editor_saved(conn: &Connection, at: i64, path: &str, hash: &str) -> rusqlite::Result<()> {
    let ids: Vec<i64> = {
        let mut stmt = conn.prepare_cached(
            "SELECT r.revision_id FROM change_revisions r
               JOIN changes c ON c.change_id = r.change_id
               JOIN snapshots s ON s.snapshot_id = r.after_snapshot
              WHERE r.observed_at BETWEEN ?1 AND ?2 AND c.path = ?3 COLLATE NOCASE AND s.blob_hash = ?4
                AND r.completeness <> 'gap'
                AND NOT EXISTS (SELECT 1 FROM attribution_evidence e WHERE e.revision_id = r.revision_id AND e.kind = 'editor_saved')
              ORDER BY r.revision_id",
        )?;
        let rows = stmt.query_map(params![at - EDITOR_SAVED_WINDOW_MS, at + EDITOR_SAVED_WINDOW_MS, path, hash], |r| r.get(0))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    for id in ids {
        conn.execute(
            "INSERT INTO attribution_evidence (revision_id, session_id, operation_id, kind, detail, recorded_at)
             VALUES (?1, NULL, NULL, 'editor_saved', ?2, ?3)",
            params![id, json!({ "path": path, "hash": hash, "noticeAt": at }).to_string(), at],
        )?;
        recompute_conclusion(conn, id)?;
    }
    Ok(())
}

fn restore_expect_detail(rev: &RevisionRow, notice_at: i64) -> Value {
    json!({ "path": rev.path, "hash": hash_json(&rev.after), "noticeAt": notice_at })
}

/// この Revision の After (無ければ削除) と同じ内容の `restore_expect` の予告 (event_id が `through` 以下)。あればその時刻。
/// 予告は書き込みの前に届くので、観測時刻より最大 `RESTORE_EXPECT_BEFORE_MS` 前までを数える。`gap` の Revision は対象外。
fn restore_expect_notice(conn: &Connection, rev: &RevisionRow, through: i64) -> rusqlite::Result<Option<i64>> {
    if rev.completeness == "gap" {
        return Ok(None);
    }
    let (lo, hi) = (rev.observed_at - RESTORE_EXPECT_BEFORE_MS, rev.observed_at + RESTORE_EXPECT_AFTER_MS);
    let sql = "SELECT at FROM events
                WHERE kind = 'restore_expect' AND at BETWEEN ?1 AND ?2 AND event_id <= ?3
                  AND json_extract(payload, '$.path') = ?4 COLLATE NOCASE
                  AND json_extract(payload, '$.hash') IS ?5
                ORDER BY event_id LIMIT 1";
    let hash: Option<&str> = match &rev.after {
        HashState::Hash(h) => Some(h.as_str()),
        HashState::Absent => None,
        HashState::Unknown => return Ok(None),
    };
    conn.query_row(sql, params![lo, hi, through, rev.path, hash], |r| r.get(0)).optional()
}

/// `restore_expect` のイベントを派生の表へ反映する: 予告のあとに観測された (観測が先に届いていた) Revision に根拠を足す。
/// `hash` が None の予告は「このパスは消える」。予告が後から届いても、先に届いても同じ結果になる。
pub fn apply_restore_expect(conn: &Connection, at: i64, path: &str, hash: Option<&str>) -> rusqlite::Result<()> {
    let ids: Vec<i64> = {
        let mut stmt = conn.prepare_cached(
            "SELECT r.revision_id FROM change_revisions r
               JOIN changes c ON c.change_id = r.change_id
               LEFT JOIN snapshots s ON s.snapshot_id = r.after_snapshot
              WHERE r.observed_at BETWEEN ?1 AND ?2 AND c.path = ?3 COLLATE NOCASE
                AND ((?4 IS NULL AND r.after_snapshot IS NULL) OR (?4 IS NOT NULL AND s.blob_hash = ?4))
                AND r.completeness <> 'gap'
                AND NOT EXISTS (SELECT 1 FROM attribution_evidence e WHERE e.revision_id = r.revision_id AND e.kind = 'restore_expected')
              ORDER BY r.revision_id",
        )?;
        let rows = stmt.query_map(params![at - RESTORE_EXPECT_AFTER_MS, at + RESTORE_EXPECT_BEFORE_MS, path, hash], |r| r.get(0))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    for id in ids {
        conn.execute(
            "INSERT INTO attribution_evidence (revision_id, session_id, operation_id, kind, detail, recorded_at)
             VALUES (?1, NULL, NULL, 'restore_expected', ?2, ?3)",
            params![id, json!({ "path": path, "hash": hash, "noticeAt": at }).to_string(), at],
        )?;
        recompute_conclusion(conn, id)?;
    }
    Ok(())
}

fn hash_json(state: &HashState) -> Value {
    match state {
        HashState::Hash(h) => json!(h),
        _ => Value::Null,
    }
}

/// セッション・操作・申告に基づく根拠。
fn session_evidence(
    conn: &Connection,
    cfg: &AttributionConfig,
    rev: &RevisionRow,
    managed_begin: bool,
    content_only: bool,
    sink: &mut Sink<'_>,
) -> rusqlite::Result<()> {
    let session_count: i64 = conn.query_row("SELECT COUNT(*) FROM sessions", [], |r| r.get(0))?;
    if session_count == 0 {
        return Ok(());
    }
    let obs = rev.observed_at;
    let before_path = rev.old_path.as_deref().unwrap_or(&rev.path);
    // Windows のパスは大文字小文字を区別しない (クライアントの綴りと監視が見たパスが違ってもよい)
    let touches = |path: &str| path.eq_ignore_ascii_case(&rev.path) || rev.old_path.as_deref().is_some_and(|o| o.eq_ignore_ascii_case(path));

    // 操作の窓 (report は窓を持たない)
    let ops: Vec<OpRow> = {
        let mut stmt = conn.prepare_cached(
            "SELECT operation_id, session_id, reported_targets, result FROM operations
              WHERE operation_type <> 'report' AND started_at <= ?1
                AND (?3 OR finished_at IS NULL OR finished_at + ?2 >= ?1)
                AND NOT (finished_at IS NULL AND result IS NOT NULL)
              ORDER BY started_at, operation_id",
        )?;
        let rows = stmt.query_map(params![obs, cfg.window_grace_ms, content_only], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?, r.get::<_, String>(2)?, r.get::<_, Option<String>>(3)?))
        })?;
        let mut out = Vec::new();
        for row in rows {
            let (id, session, raw, result) = row?;
            out.push(op_row(id, session, result, &raw));
        }
        out
    };
    let mut window_sessions: BTreeSet<String> = BTreeSet::new();
    let mut counted_window = false;
    for op in ops.iter().filter(|o| o.targets.iter().any(|(p, _)| touches(p))) {
        counted_window |= op.counted;
        if !content_only {
            window_sessions.insert(op.session.clone());
            sink.add(Kind::OpWindow, Some(&op.session), Some(&op.id), json!({ "path": rev.path }))?;
        }
        let taken = op.targets.iter().find(|(p, _)| p.eq_ignore_ascii_case(before_path)).and_then(|(_, h)| h.as_deref());
        let before_match = rev.before.equals(taken);
        if before_match {
            sink.add(Kind::BeforeMatch, Some(&op.session), Some(&op.id), json!({ "hash": taken }))?;
        }
        if before_match
            && op.declared_path.as_deref().is_some_and(|d| d.eq_ignore_ascii_case(&rev.path))
            && matches!((&op.declared_after, &rev.after), (Some(h), HashState::Hash(a)) if h == a)
        {
            sink.add(Kind::DeclaredEditMatch, Some(&op.session), Some(&op.id), json!({ "hash": op.declared_after }))?;
        }
    }
    if window_sessions.len() >= 2 {
        let sessions: Vec<&String> = window_sessions.iter().collect();
        sink.add(Kind::Overlap, None, None, json!({ "sessions": sessions }))?;
    }

    report_evidence(conn, rev, content_only, sink)?;
    if content_only {
        return Ok(());
    }

    // 作業ツリーと時刻の近さは、running のセッションだけを見る
    let running: Vec<(String, i64, String, String)> = {
        let mut stmt = conn.prepare_cached(
            "SELECT session_id, started_at, workspace, coverage FROM sessions WHERE status = 'running' ORDER BY session_id",
        )?;
        let rows = stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    for (session, started_at, workspace, coverage) in &running {
        if *started_at > obs {
            continue;
        }
        let cov: Value = serde_json::from_str(coverage).unwrap_or(Value::Null);
        let under = cov["workspaceRel"]
            .as_str()
            .is_some_and(|w| w.is_empty() || rev.path == w || rev.path.starts_with(&format!("{w}/")));
        let same_workspace = running.iter().filter(|(_, _, w, _)| w == workspace).count();
        if cov["worktreeEligible"].as_bool() == Some(true) && under && same_workspace == 1 {
            sink.add(Kind::Worktree, Some(session), None, json!({ "workspace": workspace }))?;
        }
    }
    // 結論に数える窓 (ok で閉じた操作) がある変更は、時刻の近さを見ない。開いたまま・失敗した操作の窓は数えないので見る
    if managed_begin || counted_window || sink.has_any(&[Kind::ReportHashMatch, Kind::ReportOnly]) {
        return Ok(());
    }
    let (lo, hi) = (obs - cfg.time_near_ms, obs + cfg.time_near_ms);
    let near: Vec<String> = {
        let mut stmt = conn.prepare_cached(
            "SELECT s.session_id FROM sessions s
              WHERE s.status = 'running' AND s.started_at <= ?1
                AND (s.started_at BETWEEN ?2 AND ?3
                     OR EXISTS (SELECT 1 FROM operations o WHERE o.session_id = s.session_id
                                  AND (o.started_at BETWEEN ?2 AND ?3 OR o.finished_at BETWEEN ?2 AND ?3)))
              ORDER BY s.session_id",
        )?;
        let rows = stmt.query_map(params![obs, lo, hi], |r| r.get(0))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    for session in near {
        sink.add(Kind::TimeNear, Some(&session), None, json!({ "widthMs": cfg.time_near_ms }))?;
    }
    // セッションの開始のすぐ前の変更は、そのセッションより前からあった変更
    for (session, started_at, _, _) in &running {
        if *started_at > obs && *started_at - obs <= cfg.time_near_ms {
            sink.add(Kind::Preexisting, Some(session), None, json!({ "sessionStartedAt": started_at }))?;
        }
    }
    Ok(())
}

/// `report` に基づく根拠。申告のハッシュが Before / After の両方と一致すれば `report_hash_match`、
/// そうでなければ (申告の時点で最後の変更に限って) `report_only`。セッションの開始より前の変更は `preexisting`。
fn report_evidence(conn: &Connection, rev: &RevisionRow, hash_only: bool, sink: &mut Sink<'_>) -> rusqlite::Result<()> {
    let obs = rev.observed_at;
    let mut stmt = conn.prepare_cached(
        "SELECT o.operation_id, o.session_id, o.started_at, o.reported_targets, s.started_at
           FROM operations o JOIN sessions s ON s.session_id = o.session_id
          WHERE o.operation_type = 'report' AND o.started_at >= ?1 AND o.started_at <= ?2
          ORDER BY o.started_at, o.operation_id",
    )?;
    let rows = stmt.query_map(params![obs - REPORT_LOOKAHEAD_MS, obs + REPORT_BACKLOG_MS], |r| {
        Ok((
            r.get::<_, String>(0)?,
            r.get::<_, String>(1)?,
            r.get::<_, i64>(2)?,
            r.get::<_, String>(3)?,
            r.get::<_, i64>(4)?,
        ))
    })?;
    let reports: Vec<(String, String, i64, String, i64)> = rows.collect::<rusqlite::Result<_>>()?;
    for (op_id, session, report_at, raw, session_started) in reports {
        let json: Value = serde_json::from_str(&raw).unwrap_or(Value::Null);
        let Some(entry) = json["report"]["paths"]
            .as_array()
            .into_iter()
            .flatten()
            .find(|e| e["path"].as_str().is_some_and(|p| p.eq_ignore_ascii_case(&rev.path)))
        else {
            continue;
        };
        if obs < session_started {
            sink.add(Kind::Preexisting, Some(&session), Some(&op_id), json!({ "reason": "before_session" }))?;
            continue;
        }
        let before_given = entry["beforeHash"].as_str();
        let after_given = entry["afterHash"].as_str();
        let matches_hash = match (before_given, after_given) {
            (Some(b), Some(a)) => {
                matches!(&rev.before, HashState::Hash(x) if x == b) && matches!(&rev.after, HashState::Hash(x) if x == a)
            }
            _ => false,
        };
        if matches_hash {
            sink.add(Kind::ReportHashMatch, Some(&session), Some(&op_id), json!({ "path": rev.path }))?;
            continue;
        }
        if hash_only {
            continue;
        }
        // 申告の時点で、そのパスの最後の変更だけを対象にする (前の変更まで AI のものにしない)
        let later: i64 = conn.query_row(
            "SELECT COUNT(*) FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
              WHERE c.path = ?1 COLLATE NOCASE AND r.revision_id > ?2 AND r.observed_at <= ?3",
            params![rev.path, rev.id, report_at],
            |r| r.get(0),
        )?;
        if later == 0 {
            let reason = if before_given.is_some() || after_given.is_some() { "hash_mismatch" } else { "no_hash" };
            sink.add(Kind::ReportOnly, Some(&session), Some(&op_id), json!({ "reason": reason }))?;
        }
    }
    Ok(())
}
