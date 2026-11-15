//! レビュー (承認・却下・要確認・再開) の記録と、今の状態の計算 (ipc-contract.md §4.6、db-schema.md §3.1)。
//!
//! 承認は「確認した」という記録で、git・ファイル・Revision の復元状態には何もしない。`reviews` は追記だけで、
//! 今の状態は「その葉を含む範囲のうち、いちばん新しい行」。親 (Revision / 変更 / 操作 / セッション) の状態は
//! 保存せず、問い合わせのたびに葉から集約する。状態の決め方と集約は DB に触れない純粋な関数 (`LatestIndex`、
//! `Counts::aggregate`) で、取得の op と `scene_overlay` が同じものを使う。

use std::collections::HashMap;

use rusqlite::{params, Connection};
use serde_json::{json, Value};

/// 1 回の `review_set` が受ける対象の数の上限 (Editor が分けて送る)。
pub const MAX_TARGETS_PER_CALL: usize = 1000;
/// メモの長さの上限 (文字数)。
pub const MAX_NOTE_CHARS: usize = 4000;
/// 1 回の集約 (変更 / 操作 / セッション) で読む Revision の数の上限。超えたら `truncated`。
pub const MAX_GROUP_REVISIONS: usize = 5000;
/// `describe` が返す葉・履歴の行数の上限。
pub const MAX_DESCRIBE_ROWS: usize = 1000;

/// `review_set` の `state` に指定できる値。`pending` は指定できず、`reopened` が「未確認へ戻す」を表す。
pub const INPUT_STATES: &[&str] = &["approved", "rejected", "needs_review", "reopened"];
/// `scope.kind` の値。
pub const SCOPE_KINDS: &[&str] = &["revision", "hunk", "entity", "component", "field"];
/// `list` の `review` フィルタに指定できる (集約の) 状態。
pub const AGGREGATE_STATES: &[&str] =
    &["pending", "approved", "rejected", "needs_review", "partially_approved", "partially_rejected"];

/// 葉の今の状態。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LeafState {
    Pending,
    Approved,
    Rejected,
    NeedsReview,
}

impl LeafState {
    pub fn as_str(self) -> &'static str {
        match self {
            LeafState::Pending => "pending",
            LeafState::Approved => "approved",
            LeafState::Rejected => "rejected",
            LeafState::NeedsReview => "needs_review",
        }
    }

    /// 保存された状態の文字列から。`reopened` は「未確認へ戻した」ので `Pending`。
    pub fn from_stored(text: &str) -> LeafState {
        match text {
            "approved" => LeafState::Approved,
            "rejected" => LeafState::Rejected,
            "needs_review" => LeafState::NeedsReview,
            _ => LeafState::Pending,
        }
    }
}

/// 集約の状態 (Revision / 変更 / 操作 / セッション)。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Aggregate {
    Pending,
    Approved,
    Rejected,
    NeedsReview,
    PartiallyApproved,
    PartiallyRejected,
}

impl Aggregate {
    pub fn as_str(self) -> &'static str {
        match self {
            Aggregate::Pending => "pending",
            Aggregate::Approved => "approved",
            Aggregate::Rejected => "rejected",
            Aggregate::NeedsReview => "needs_review",
            Aggregate::PartiallyApproved => "partially_approved",
            Aggregate::PartiallyRejected => "partially_rejected",
        }
    }
}

/// 状態ごとの葉の数。
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counts {
    pub pending: u64,
    pub approved: u64,
    pub rejected: u64,
    pub needs_review: u64,
}

impl Counts {
    pub fn add(&mut self, state: LeafState) {
        match state {
            LeafState::Pending => self.pending += 1,
            LeafState::Approved => self.approved += 1,
            LeafState::Rejected => self.rejected += 1,
            LeafState::NeedsReview => self.needs_review += 1,
        }
    }

    pub fn merge(&mut self, other: &Counts) {
        self.pending += other.pending;
        self.approved += other.approved;
        self.rejected += other.rejected;
        self.needs_review += other.needs_review;
    }

    pub fn total(&self) -> u64 {
        self.pending + self.approved + self.rejected + self.needs_review
    }

    /// 集約の規則 (db-schema.md §3.1): 葉が全部同じならその状態。そうでなければ上から順に、
    /// `needs_review` がある → `needs_review`、`approved` がある → `partially_approved`、それ以外 → `partially_rejected`。
    /// 葉が 0 の入れ物は `pending` (承認できるものが無い)。
    pub fn aggregate(&self) -> Aggregate {
        let kinds = [self.pending, self.approved, self.rejected, self.needs_review].iter().filter(|n| **n > 0).count();
        if kinds <= 1 {
            return if self.approved > 0 {
                Aggregate::Approved
            } else if self.rejected > 0 {
                Aggregate::Rejected
            } else if self.needs_review > 0 {
                Aggregate::NeedsReview
            } else {
                Aggregate::Pending
            };
        }
        if self.needs_review > 0 {
            Aggregate::NeedsReview
        } else if self.approved > 0 {
            Aggregate::PartiallyApproved
        } else {
            Aggregate::PartiallyRejected
        }
    }

    pub fn to_json(&self) -> Value {
        json!({
            "pending": self.pending,
            "approved": self.approved,
            "rejected": self.rejected,
            "needsReview": self.needs_review,
        })
    }
}

/// レビューの対象になる行為者か。利用者自身の編集 (`editor_user`) と復元 (`restore`) は対象ではない。
pub fn is_reviewable_actor(actor: &str) -> bool {
    matches!(actor, "ai" | "unknown")
}

/// 状態を持つ最小の単位。
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Leaf {
    /// 葉の無い Revision (バイナリ・除外・解析不可・削除だけ)。Revision 自身が葉。
    Revision,
    Hunk(i64),
    /// 構造の変更の 1 行。コンポーネント・フィールドを持たない行は、エンティティ (または文書全体) の変更。
    Scene { file_id: Option<i64>, component: Option<String>, field: Option<String> },
}

/// 範囲の参照文字列 (`reviews.scope_ref`)。`entity` = fileId、`component` = "fileId/Component"、
/// `field` = "fileId/Component/field" (コンポーネントに属さないフィールドは "fileId//field")。
pub fn entity_ref(file_id: i64) -> String {
    file_id.to_string()
}
pub fn component_ref(file_id: i64, component: &str) -> String {
    format!("{file_id}/{component}")
}
pub fn field_ref(file_id: i64, component: Option<&str>, field: &str) -> String {
    format!("{file_id}/{}/{field}", component.unwrap_or(""))
}

impl Leaf {
    /// この葉を含む範囲 (広い順)。ここに付いた行のうち、いちばん新しいものが葉の状態を決める。
    pub fn covering_scopes(&self) -> Vec<(&'static str, String)> {
        let mut scopes = vec![("revision", String::new())];
        match self {
            Leaf::Revision => {}
            Leaf::Hunk(id) => scopes.push(("hunk", id.to_string())),
            Leaf::Scene { file_id, component, field } => {
                if let Some(file_id) = file_id {
                    scopes.push(("entity", entity_ref(*file_id)));
                    if let Some(component) = component {
                        scopes.push(("component", component_ref(*file_id, component)));
                    }
                    if let Some(field) = field {
                        scopes.push(("field", field_ref(*file_id, component.as_deref(), field)));
                    }
                }
            }
        }
        scopes
    }
}

/// `reviews` の 1 行 (状態の計算に要る列だけ)。
#[derive(Clone, Debug)]
pub struct ReviewRow {
    pub review_id: i64,
    pub scope_kind: String,
    pub scope_ref: String,
    pub state: String,
}

/// 範囲ごとの最新の行。葉の状態は「葉を含む範囲の最新の行のうち、いちばん新しいもの」(範囲の広さでなく新しさで決まる)。
pub struct LatestIndex {
    latest: HashMap<(String, String), (i64, LeafState)>,
}

impl LatestIndex {
    pub fn new(rows: &[ReviewRow]) -> LatestIndex {
        let mut latest: HashMap<(String, String), (i64, LeafState)> = HashMap::new();
        for row in rows {
            let key = (row.scope_kind.clone(), row.scope_ref.clone());
            let state = LeafState::from_stored(&row.state);
            match latest.get(&key) {
                Some((id, _)) if *id > row.review_id => {}
                _ => {
                    latest.insert(key, (row.review_id, state));
                }
            }
        }
        LatestIndex { latest }
    }

    pub fn is_empty(&self) -> bool {
        self.latest.is_empty()
    }

    pub fn state_of(&self, leaf: &Leaf) -> LeafState {
        let mut best: Option<(i64, LeafState)> = None;
        for (kind, scope_ref) in leaf.covering_scopes() {
            if let Some(&(id, state)) = self.latest.get(&(kind.to_string(), scope_ref)) {
                if best.is_none_or(|(best_id, _)| id > best_id) {
                    best = Some((id, state));
                }
            }
        }
        best.map_or(LeafState::Pending, |(_, state)| state)
    }
}

// ---- DB からの読み出し ---------------------------------------------------------------------

/// 葉と、その識別子 (Hunk なら `hunk_id`、構造の変更なら `scene_change_id`、Revision 自身なら 0)。
#[derive(Clone, Debug)]
pub struct LeafInfo {
    pub leaf: Leaf,
    pub id: i64,
}

/// Revision の葉。構造の変更があればそれ、無ければ Hunk、どちらも無ければ Revision 自身。
pub fn load_leaves(conn: &Connection, revision_id: i64) -> rusqlite::Result<Vec<LeafInfo>> {
    let mut stmt = conn.prepare_cached(
        "SELECT scene_change_id, file_id, component, field FROM scene_changes WHERE revision_id = ?1 ORDER BY scene_change_id",
    )?;
    let scene: Vec<LeafInfo> = stmt
        .query_map([revision_id], |r| {
            Ok(LeafInfo {
                id: r.get(0)?,
                leaf: Leaf::Scene { file_id: r.get(1)?, component: r.get(2)?, field: r.get(3)? },
            })
        })?
        .collect::<rusqlite::Result<_>>()?;
    if !scene.is_empty() {
        return Ok(scene);
    }
    let mut stmt = conn.prepare_cached("SELECT hunk_id FROM change_hunks WHERE revision_id = ?1 ORDER BY ordinal")?;
    let hunks: Vec<LeafInfo> = stmt
        .query_map([revision_id], |r| Ok(LeafInfo { id: r.get(0)?, leaf: Leaf::Hunk(r.get(0)?) }))?
        .collect::<rusqlite::Result<_>>()?;
    if !hunks.is_empty() {
        return Ok(hunks);
    }
    Ok(vec![LeafInfo { leaf: Leaf::Revision, id: 0 }])
}

/// Revision の葉の数 (`load_leaves` の長さを、行を読まずに数える)。
fn leaf_count(conn: &Connection, revision_id: i64) -> rusqlite::Result<u64> {
    let (scene, hunks): (i64, i64) = conn.query_row(
        "SELECT (SELECT COUNT(*) FROM scene_changes WHERE revision_id = ?1), (SELECT COUNT(*) FROM change_hunks WHERE revision_id = ?1)",
        [revision_id],
        |r| Ok((r.get(0)?, r.get(1)?)),
    )?;
    Ok(if scene > 0 {
        scene as u64
    } else if hunks > 0 {
        hunks as u64
    } else {
        1
    })
}

/// Revision の `reviews` の行 (古い順)。
pub fn load_rows(conn: &Connection, revision_id: i64) -> rusqlite::Result<Vec<ReviewRow>> {
    let mut stmt = conn.prepare_cached(
        "SELECT review_id, scope_kind, scope_ref, state FROM reviews WHERE revision_id = ?1 ORDER BY review_id",
    )?;
    let rows = stmt.query_map([revision_id], |r| {
        Ok(ReviewRow { review_id: r.get(0)?, scope_kind: r.get(1)?, scope_ref: r.get(2)?, state: r.get(3)? })
    })?;
    rows.collect()
}

/// Revision の状態ごとの葉の数。レビューの行が無ければ全部 `pending` (葉は数えるだけ)。
pub fn revision_counts(conn: &Connection, revision_id: i64) -> rusqlite::Result<Counts> {
    let rows = load_rows(conn, revision_id)?;
    counts_with_rows(conn, revision_id, &rows)
}

fn counts_with_rows(conn: &Connection, revision_id: i64, rows: &[ReviewRow]) -> rusqlite::Result<Counts> {
    let mut counts = Counts::default();
    if rows.is_empty() {
        counts.pending = leaf_count(conn, revision_id)?;
        return Ok(counts);
    }
    let index = LatestIndex::new(rows);
    for info in load_leaves(conn, revision_id)? {
        counts.add(index.state_of(&info.leaf));
    }
    Ok(counts)
}

/// 1 つの Revision のレビューの状態。行為者が対象外なら `None`。
pub fn revision_state(conn: &Connection, revision_id: i64) -> rusqlite::Result<Option<(Aggregate, Counts)>> {
    let actor: String = conn.query_row("SELECT actor FROM change_revisions WHERE revision_id = ?1", [revision_id], |r| r.get(0))?;
    if !is_reviewable_actor(&actor) {
        return Ok(None);
    }
    let counts = revision_counts(conn, revision_id)?;
    Ok(Some((counts.aggregate(), counts)))
}

/// 条件に合う Revision (対象の行為者だけ) の葉を全部集めた集約。`condition` は `change_revisions r` への条件で、
/// `params` を束縛する。読む Revision は `MAX_GROUP_REVISIONS` まで (超えたら `truncated`)。
pub fn group_counts(
    conn: &Connection,
    condition: &str,
    params: &[&dyn rusqlite::ToSql],
) -> rusqlite::Result<(Counts, bool)> {
    let sql = format!(
        "SELECT r.revision_id,
                CASE WHEN (SELECT COUNT(*) FROM scene_changes s WHERE s.revision_id = r.revision_id) > 0
                       THEN (SELECT COUNT(*) FROM scene_changes s WHERE s.revision_id = r.revision_id)
                     WHEN (SELECT COUNT(*) FROM change_hunks h WHERE h.revision_id = r.revision_id) > 0
                       THEN (SELECT COUNT(*) FROM change_hunks h WHERE h.revision_id = r.revision_id)
                     ELSE 1 END,
                EXISTS (SELECT 1 FROM reviews v WHERE v.revision_id = r.revision_id)
           FROM change_revisions r
          WHERE r.actor IN ('ai', 'unknown') AND ({condition})
          LIMIT {}",
        MAX_GROUP_REVISIONS + 1
    );
    let mut stmt = conn.prepare(&sql)?;
    let rows: Vec<(i64, i64, bool)> =
        stmt.query_map(params, |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?.collect::<rusqlite::Result<_>>()?;
    let truncated = rows.len() > MAX_GROUP_REVISIONS;
    let mut counts = Counts::default();
    for (revision_id, leaves, has_reviews) in rows.into_iter().take(MAX_GROUP_REVISIONS) {
        if has_reviews {
            counts.merge(&revision_counts(conn, revision_id)?);
        } else {
            counts.pending += leaves as u64;
        }
    }
    Ok((counts, truncated))
}

/// 集約の JSON (`state` と件数、読み切れなかったら `truncated`)。
pub fn group_json(counts: &Counts, truncated: bool) -> Value {
    json!({ "state": counts.aggregate().as_str(), "counts": counts.to_json(), "truncated": truncated })
}

/// `describe` に足す項目: 集約・葉ごとの状態・履歴・変更 / 操作の集約。
pub fn describe_fields(conn: &Connection, revision_id: i64, change_id: i64, operation_id: Option<&str>) -> rusqlite::Result<Value> {
    let actor: String = conn.query_row("SELECT actor FROM change_revisions WHERE revision_id = ?1", [revision_id], |r| r.get(0))?;
    let reviewable = is_reviewable_actor(&actor);
    let rows = load_rows(conn, revision_id)?;
    let index = LatestIndex::new(&rows);
    let leaves = load_leaves(conn, revision_id)?;
    let mut counts = Counts::default();
    let mut leaf_items: Vec<Value> = Vec::new();
    for info in &leaves {
        let state = index.state_of(&info.leaf);
        counts.add(state);
        if leaf_items.len() < MAX_DESCRIBE_ROWS {
            let item = match info.leaf {
                Leaf::Revision => json!({ "kind": "revision", "state": state.as_str() }),
                Leaf::Hunk(id) => json!({ "kind": "hunk", "hunkId": id, "state": state.as_str() }),
                Leaf::Scene { .. } => json!({ "kind": "scene", "sceneChangeId": info.id, "state": state.as_str() }),
            };
            leaf_items.push(item);
        }
    }
    let mut stmt = conn.prepare_cached(
        "SELECT review_id, scope_kind, scope_ref, state, note, decided_at FROM reviews
          WHERE revision_id = ?1 ORDER BY review_id DESC LIMIT ?2",
    )?;
    let mut history: Vec<Value> = stmt
        .query_map(params![revision_id, MAX_DESCRIBE_ROWS as i64 + 1], |r| {
            Ok(json!({
                "reviewId": r.get::<_, i64>(0)?,
                "scopeKind": r.get::<_, String>(1)?,
                "scopeRef": r.get::<_, String>(2)?,
                "state": r.get::<_, String>(3)?,
                "note": r.get::<_, Option<String>>(4)?,
                "decidedAt": r.get::<_, i64>(5)?,
            }))
        })?
        .collect::<rusqlite::Result<_>>()?;
    let history_truncated = history.len() > MAX_DESCRIBE_ROWS;
    history.truncate(MAX_DESCRIBE_ROWS);
    history.reverse();

    let mut out = json!({
        "reviewable": reviewable,
        "review": if reviewable { json!(counts.aggregate().as_str()) } else { Value::Null },
        "reviewCounts": counts.to_json(),
        "reviewLeaves": leaf_items,
        "reviewLeavesTruncated": leaves.len() > MAX_DESCRIBE_ROWS,
        "reviewHistory": history,
        "reviewHistoryTruncated": history_truncated,
    });
    if reviewable {
        let (change, change_truncated) = group_counts(conn, "r.change_id = ?1", &[&change_id])?;
        out["changeReview"] = group_json(&change, change_truncated);
        if let Some(operation_id) = operation_id {
            let (operation, operation_truncated) = group_counts(conn, "r.operation_id = ?1", &[&operation_id])?;
            out["operationReview"] = group_json(&operation, operation_truncated);
        }
    }
    Ok(out)
}

// ---- 記録 (Writer の中) --------------------------------------------------------------------

/// `review_set` の対象 1 件 (検査済み)。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Target {
    pub revision_id: i64,
    pub kind: String,
    pub scope_ref: String,
}

/// イベント `review_set` を `reviews` へ反映する。通常の記録も `rebuild_derived` も同じ関数を通る。
/// 存在しない Revision への行は足さない (作り直しで外部キーが壊れないように。通常は検査で弾かれている)。
pub fn apply_event(conn: &Connection, at: i64, payload: &Value) -> rusqlite::Result<()> {
    let (Some(state), Some(targets)) = (payload["state"].as_str(), payload["targets"].as_array()) else {
        return Ok(());
    };
    let note = payload["note"].as_str();
    for target in targets {
        let (Some(revision_id), Some(kind), Some(scope_ref)) =
            (target["revision_id"].as_i64(), target["kind"].as_str(), target["ref"].as_str())
        else {
            continue;
        };
        conn.execute(
            "INSERT INTO reviews (revision_id, scope_kind, scope_ref, state, note, decided_at)
             SELECT ?1, ?2, ?3, ?4, ?5, ?6 WHERE EXISTS (SELECT 1 FROM change_revisions WHERE revision_id = ?1)",
            params![revision_id, kind, scope_ref, state, note, at],
        )?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(id: i64, kind: &str, scope_ref: &str, state: &str) -> ReviewRow {
        ReviewRow { review_id: id, scope_kind: kind.into(), scope_ref: scope_ref.into(), state: state.into() }
    }

    fn counts(p: u64, a: u64, r: u64, n: u64) -> Counts {
        Counts { pending: p, approved: a, rejected: r, needs_review: n }
    }

    #[test]
    fn newest_row_decides_not_the_widest_scope() {
        let hunk = Leaf::Hunk(7);
        let other = Leaf::Hunk(8);
        let rows = vec![row(1, "revision", "", "approved"), row(2, "hunk", "7", "rejected")];
        let index = LatestIndex::new(&rows);
        assert_eq!(index.state_of(&hunk), LeafState::Rejected);
        assert_eq!(index.state_of(&other), LeafState::Approved);
        let rows = vec![row(1, "revision", "", "approved"), row(2, "hunk", "7", "rejected"), row(3, "revision", "", "approved")];
        let index = LatestIndex::new(&rows);
        assert_eq!(index.state_of(&hunk), LeafState::Approved);
        assert_eq!(index.state_of(&other), LeafState::Approved);
    }

    #[test]
    fn reopened_is_pending_and_no_rows_is_pending() {
        let leaf = Leaf::Hunk(1);
        assert_eq!(LatestIndex::new(&[]).state_of(&leaf), LeafState::Pending);
        let rows = vec![row(1, "hunk", "1", "approved"), row(2, "hunk", "1", "reopened")];
        assert_eq!(LatestIndex::new(&rows).state_of(&leaf), LeafState::Pending);
        let rows = vec![row(1, "hunk", "1", "approved"), row(2, "hunk", "1", "reopened"), row(3, "hunk", "1", "rejected")];
        assert_eq!(LatestIndex::new(&rows).state_of(&leaf), LeafState::Rejected);
    }

    #[test]
    fn scene_scopes_nest_entity_component_field() {
        let field = Leaf::Scene { file_id: Some(5), component: Some("Transform".into()), field: Some("position".into()) };
        let sibling = Leaf::Scene { file_id: Some(5), component: Some("Transform".into()), field: Some("scale".into()) };
        let rows = vec![row(1, "entity", "5", "approved"), row(2, "field", "5/Transform/position", "needs_review")];
        let index = LatestIndex::new(&rows);
        assert_eq!(index.state_of(&field), LeafState::NeedsReview);
        assert_eq!(index.state_of(&sibling), LeafState::Approved);
        // 別のエンティティの同じフィールドには効かない
        let elsewhere = Leaf::Scene { file_id: Some(6), component: Some("Transform".into()), field: Some("position".into()) };
        assert_eq!(index.state_of(&elsewhere), LeafState::Pending);
        // コンポーネントを持たないフィールドの参照は "fileId//field"
        let bare = Leaf::Scene { file_id: Some(5), component: None, field: Some("name".into()) };
        let rows = vec![row(1, "field", "5//name", "rejected")];
        assert_eq!(LatestIndex::new(&rows).state_of(&bare), LeafState::Rejected);
        // 文書全体の変更 (fileId なし) は Revision の範囲だけが効く
        let document = Leaf::Scene { file_id: None, component: None, field: Some("x".into()) };
        let rows = vec![row(1, "entity", "5", "approved")];
        assert_eq!(LatestIndex::new(&rows).state_of(&document), LeafState::Pending);
    }

    #[test]
    fn aggregate_table() {
        assert_eq!(counts(3, 0, 0, 0).aggregate(), Aggregate::Pending);
        assert_eq!(counts(0, 3, 0, 0).aggregate(), Aggregate::Approved);
        assert_eq!(counts(0, 0, 3, 0).aggregate(), Aggregate::Rejected);
        assert_eq!(counts(0, 0, 0, 3).aggregate(), Aggregate::NeedsReview);
        // needs_review が 1 つでもあれば needs_review
        assert_eq!(counts(1, 1, 1, 1).aggregate(), Aggregate::NeedsReview);
        assert_eq!(counts(0, 1, 0, 1).aggregate(), Aggregate::NeedsReview);
        // approved が一部
        assert_eq!(counts(1, 1, 0, 0).aggregate(), Aggregate::PartiallyApproved);
        assert_eq!(counts(0, 1, 1, 0).aggregate(), Aggregate::PartiallyApproved);
        assert_eq!(counts(1, 2, 1, 0).aggregate(), Aggregate::PartiallyApproved);
        // rejected と pending の混在
        assert_eq!(counts(1, 0, 1, 0).aggregate(), Aggregate::PartiallyRejected);
    }

    #[test]
    fn only_ai_and_unknown_are_reviewable() {
        assert!(is_reviewable_actor("ai") && is_reviewable_actor("unknown"));
        assert!(!is_reviewable_actor("editor_user") && !is_reviewable_actor("restore"));
    }
}
