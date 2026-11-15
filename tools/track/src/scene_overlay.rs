//! 開いている文書にかかっている構造の変更の集約 (`scene_overlay`) と、パスから文書の ID を引く `document_for_path`
//! (architecture.md §6、ipc-contract.md)。
//!
//! 集約 (`fold`) は (Revision の列, 行の列) → エンティティごとの状態、を返す純粋な関数 (DB に触れない)。
//! Editor が Revision を 1 つずつ取って自分でまとめないよう、まとめ方をここに 1 か所だけ置く。

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::Path;

use rusqlite::{params_from_iter, Connection, OptionalExtension};
use serde_json::{json, Value};

use crate::document;
use crate::lexjson;
use crate::ops::HUNKS_PAGE_BUDGET;
use crate::ops_record::normalize_path;
use crate::protocol::{code, Args, OpError};
use crate::review;
use crate::scene_diff::{self, change_type, DocKind, SceneOutcome};

/// 1 回の集約で読む `scene_changes` の行の上限。超えたら `truncated` (巨大なシーンでも応答が返るように)。
pub const MAX_OVERLAY_ROWS: usize = 400_000;
/// 引き継ぎの鎖を逆にたどって集める文書の数の上限。
const MAX_CHAIN_DOCUMENTS: usize = 64;
/// 値 (Before / After) 1 つの上限 (バイト)。超えたら値を返さず `oversize`。
const MAX_VALUE_BYTES: usize = 8 * 1024;
/// 名前を取り出すためにパースする断片の上限。
const MAX_NAME_PARSE_BYTES: usize = 64 * 1024;
/// 差分を取り直すときの文書の大きさの上限 (削除 → 追加の合成にだけ使う)。
const SYNTH_LIMIT: usize = 8 * 1024 * 1024;

/// 集約の入力 1 行 (`scene_changes` の 1 行)。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OverlayRow {
    pub file_id: Option<i64>,
    pub change_type: String,
    pub component: Option<String>,
    pub field: Option<String>,
    pub before: Option<String>,
    pub after: Option<String>,
}

/// 1 つの Revision の行 (古い順に並べて渡す)。
#[derive(Clone, Debug)]
pub struct RevisionRows {
    pub revision_id: i64,
    pub rows: Vec<OverlayRow>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Added,
    Modified,
    Removed,
}

impl Status {
    fn as_str(self) -> &'static str {
        match self {
            Status::Added => "added",
            Status::Modified => "modified",
            Status::Removed => "removed",
        }
    }
}

#[derive(Clone, Debug)]
pub struct FieldOut {
    pub name: String,
    pub before: Option<String>,
    pub after: Option<String>,
    pub last_revision: i64,
    pub change_type: String,
}

#[derive(Clone, Debug)]
pub struct ComponentOut {
    pub name: String,
    pub status: Status,
    pub fields: Vec<FieldOut>,
}

#[derive(Clone, Debug)]
pub struct EntityOut {
    pub file_id: i64,
    pub status: Status,
    pub name: Option<String>,
    pub hierarchy: Vec<FieldOut>,
    pub entity_fields: Vec<FieldOut>,
    pub components: Vec<ComponentOut>,
    pub revision_ids: Vec<i64>,
}

#[derive(Clone, Debug, Default)]
pub struct Overlay {
    pub entities: Vec<EntityOut>,
    /// ルート直下の変更 (`file_id` が無いもの)。
    pub document_changes: Vec<FieldOut>,
    /// プレハブ / アクターの文書そのものが変わった (`PrefabAssetChanged`)。最後の Revision。
    pub asset_changed: Option<i64>,
    /// 解析できなかった Revision が含まれ、畳み込みが信用できない。
    pub incomplete: bool,
    pub unparsed_revisions: Vec<i64>,
}

#[derive(Default)]
struct FieldAcc {
    before: Option<String>,
    after: Option<String>,
    last_revision: i64,
    change_type: String,
}

struct CompAcc {
    /// 範囲の始めに有ったか。最初に見た出来事で決まる。
    start: Option<bool>,
    present: bool,
    removed_fragment: Option<String>,
}

#[derive(Default)]
struct EntAcc {
    start: Option<bool>,
    present: bool,
    removed_fragment: Option<String>,
    name: Option<String>,
    components: BTreeMap<String, CompAcc>,
    fields: BTreeMap<(Option<String>, Option<String>), FieldAcc>,
    hierarchy: BTreeMap<String, FieldAcc>,
    revisions: BTreeSet<i64>,
}

/// 字句が同じか。書き方 (空白・キーの順) が違うだけの JSON も同じとみなす。
fn same_fragment(a: &Option<String>, b: &Option<String>) -> bool {
    match (a, b) {
        (None, None) => true,
        (Some(x), Some(y)) => {
            if x == y {
                return true;
            }
            match (lexjson::parse(x), lexjson::parse(y)) {
                (Ok(nx), Ok(ny)) => lexjson::same(&nx, x, &ny, y),
                _ => false,
            }
        }
        _ => false,
    }
}

fn fold_field(map_entry: &mut FieldAcc, fresh: bool, revision: i64, row: &OverlayRow) {
    if fresh {
        map_entry.before = row.before.clone();
    }
    map_entry.after = row.after.clone();
    map_entry.last_revision = revision;
    map_entry.change_type = row.change_type.clone();
}

fn name_of_entity_fragment(fragment: &str) -> Option<String> {
    if fragment.len() > MAX_NAME_PARSE_BYTES {
        return None;
    }
    let value: Value = serde_json::from_str(fragment).ok()?;
    value.get("name")?.as_str().map(str::to_string)
}

fn name_of_literal(fragment: &str) -> Option<String> {
    serde_json::from_str::<String>(fragment).ok()
}

struct Folder {
    entities: BTreeMap<i64, EntAcc>,
    doc_fields: BTreeMap<String, FieldAcc>,
    asset_changed: Option<i64>,
    unparsed: Vec<i64>,
}

impl Folder {
    fn apply(&mut self, revision: i64, row: &OverlayRow) {
        if row.change_type == change_type::UNPARSED {
            self.unparsed.push(revision);
            return;
        }
        let Some(file_id) = row.file_id else {
            if row.change_type == change_type::PREFAB_ASSET_CHANGED {
                self.asset_changed = Some(revision);
            } else if let Some(field) = &row.field {
                let fresh = !self.doc_fields.contains_key(field);
                fold_field(self.doc_fields.entry(field.clone()).or_default(), fresh, revision, row);
            }
            return;
        };
        let mut synth: Vec<OverlayRow> = Vec::new();
        {
            let ent = self.entities.entry(file_id).or_default();
            ent.revisions.insert(revision);
            match row.change_type.as_str() {
                change_type::ENTITY_ADDED => {
                    if ent.start.is_none() {
                        ent.start = Some(false);
                    } else if !ent.present && ent.start == Some(true) {
                        if let (Some(b), Some(a)) = (&ent.removed_fragment, &row.after) {
                            synth = synth_rows(file_id, b, a, None);
                        }
                    }
                    ent.present = true;
                    ent.removed_fragment = None;
                    if let Some(name) = row.after.as_deref().and_then(name_of_entity_fragment) {
                        ent.name = Some(name);
                    }
                }
                change_type::ENTITY_REMOVED => {
                    if ent.start.is_none() {
                        ent.start = Some(true);
                    }
                    ent.present = false;
                    ent.removed_fragment = row.before.clone();
                    if ent.name.is_none() {
                        ent.name = row.before.as_deref().and_then(name_of_entity_fragment);
                    }
                }
                other => {
                    if ent.start.is_none() {
                        ent.start = Some(true);
                        ent.present = true;
                    }
                    match other {
                        change_type::HIERARCHY_CHANGED => {
                            if let Some(field) = &row.field {
                                let fresh = !ent.hierarchy.contains_key(field);
                                fold_field(ent.hierarchy.entry(field.clone()).or_default(), fresh, revision, row);
                            }
                        }
                        change_type::COMPONENT_ADDED | change_type::COMPONENT_REMOVED => {
                            if let Some(name) = &row.component {
                                let added = other == change_type::COMPONENT_ADDED;
                                let comp = ent.components.entry(name.clone()).or_insert(CompAcc {
                                    start: None,
                                    present: false,
                                    removed_fragment: None,
                                });
                                if added {
                                    if comp.start.is_none() {
                                        comp.start = Some(false);
                                    } else if !comp.present && comp.start == Some(true) {
                                        if let (Some(b), Some(a)) = (&comp.removed_fragment, &row.after) {
                                            synth = synth_rows(file_id, b, a, Some(name));
                                        }
                                    }
                                    comp.present = true;
                                    comp.removed_fragment = None;
                                } else {
                                    if comp.start.is_none() {
                                        comp.start = Some(true);
                                    }
                                    comp.present = false;
                                    comp.removed_fragment = row.before.clone();
                                }
                            }
                        }
                        // FIELD_MODIFIED / ENTITY_MODIFIED / PREFAB_* は (component, field) の値の変化
                        _ => {
                            let key = (row.component.clone(), row.field.clone());
                            if row.component.is_none() && row.field.as_deref() == Some("name") {
                                if let Some(name) = row.after.as_deref().and_then(name_of_literal) {
                                    ent.name = Some(name);
                                }
                            }
                            let fresh = !ent.fields.contains_key(&key);
                            fold_field(ent.fields.entry(key).or_default(), fresh, revision, row);
                        }
                    }
                }
            }
        }
        for extra in synth {
            self.apply(revision, &extra);
        }
    }
}

/// 削除 → 追加 (エンティティ / コンポーネント) の中身の差を、同じ行の形で作る。
/// 比べられなければ行は空 (その場合の信用の低下は呼び出し側で扱わない: 同じ文書から取った断片なので通常は比べられる)。
fn synth_rows(file_id: i64, before: &str, after: &str, component: Option<&String>) -> Vec<OverlayRow> {
    let wrap = |fragment: &str| match component {
        None => format!("{{\"version\":4,\"entities\":[{fragment}]}}"),
        Some(name) => {
            let key = serde_json::to_string(name).unwrap_or_else(|_| "\"\"".to_string());
            format!("{{\"version\":4,\"entities\":[{{\"fileId\":{file_id},\"components\":{{{key}:{fragment}}}}}]}}")
        }
    };
    match scene_diff::diff(DocKind::Scene, wrap(before).as_bytes(), wrap(after).as_bytes(), SYNTH_LIMIT) {
        SceneOutcome::Changes(changes) => changes
            .into_iter()
            .filter(|c| c.file_id == Some(file_id))
            .map(|c| OverlayRow {
                file_id: c.file_id,
                change_type: c.change_type.to_string(),
                component: c.component,
                field: c.field,
                before: c.before_value,
                after: c.after_value,
            })
            .collect(),
        SceneOutcome::Unparsed(_) => Vec::new(),
    }
}

fn surviving(map: BTreeMap<String, FieldAcc>) -> Vec<FieldOut> {
    map.into_iter()
        .filter(|(_, acc)| !same_fragment(&acc.before, &acc.after))
        .map(|(name, acc)| FieldOut {
            name,
            before: acc.before,
            after: acc.after,
            last_revision: acc.last_revision,
            change_type: acc.change_type,
        })
        .collect()
}

/// 古い Revision から順に畳む。
pub fn fold(revisions: &[RevisionRows]) -> Overlay {
    let mut folder = Folder { entities: BTreeMap::new(), doc_fields: BTreeMap::new(), asset_changed: None, unparsed: Vec::new() };
    for revision in revisions {
        for row in &revision.rows {
            folder.apply(revision.revision_id, row);
        }
    }
    let mut out = Overlay {
        incomplete: !folder.unparsed.is_empty(),
        unparsed_revisions: folder.unparsed.clone(),
        asset_changed: folder.asset_changed,
        ..Overlay::default()
    };
    out.document_changes = surviving(folder.doc_fields);
    for (file_id, ent) in folder.entities {
        if let Some(entity) = finalize(file_id, ent) {
            out.entities.push(entity);
        }
    }
    out
}

fn finalize(file_id: i64, ent: EntAcc) -> Option<EntityOut> {
    let status = match (ent.start?, ent.present) {
        (false, true) => Status::Added,
        (false, false) => return None,
        (true, false) => Status::Removed,
        (true, true) => Status::Modified,
    };
    let revision_ids: Vec<i64> = ent.revisions.iter().copied().collect();
    if status != Status::Modified {
        return Some(EntityOut {
            file_id,
            status,
            name: ent.name,
            hierarchy: Vec::new(),
            entity_fields: Vec::new(),
            components: Vec::new(),
            revision_ids,
        });
    }
    let hierarchy = surviving(ent.hierarchy);
    let mut entity_fields: Vec<FieldOut> = Vec::new();
    let mut by_component: BTreeMap<String, Vec<FieldOut>> = BTreeMap::new();
    for ((component, field), acc) in ent.fields {
        if same_fragment(&acc.before, &acc.after) {
            continue;
        }
        let Some(name) = field else { continue };
        let out = FieldOut { name, before: acc.before, after: acc.after, last_revision: acc.last_revision, change_type: acc.change_type };
        match component {
            None => entity_fields.push(out),
            Some(c) => by_component.entry(c).or_default().push(out),
        }
    }
    let mut components: Vec<ComponentOut> = Vec::new();
    for (name, acc) in &ent.components {
        let status = match (acc.start, acc.present) {
            (Some(false), true) => Some(Status::Added),
            (Some(true), false) => Some(Status::Removed),
            (Some(false), false) => {
                by_component.remove(name);
                continue;
            }
            _ => None,
        };
        let fields = by_component.remove(name).unwrap_or_default();
        match status {
            Some(Status::Added) | Some(Status::Removed) => {
                components.push(ComponentOut { name: name.clone(), status: status.unwrap_or(Status::Modified), fields: Vec::new() })
            }
            _ if !fields.is_empty() => components.push(ComponentOut { name: name.clone(), status: Status::Modified, fields }),
            _ => {}
        }
    }
    for (name, fields) in by_component {
        components.push(ComponentOut { name, status: Status::Modified, fields });
    }
    components.sort_by(|a, b| a.name.cmp(&b.name));
    if hierarchy.is_empty() && entity_fields.is_empty() && components.is_empty() {
        return None;
    }
    Some(EntityOut { file_id, status, name: ent.name, hierarchy, entity_fields, components, revision_ids })
}

// ---- 応答の組み立て ----

fn field_json(f: &FieldOut, budget_left: &mut usize) -> Value {
    let size = f.before.as_ref().map_or(0, String::len) + f.after.as_ref().map_or(0, String::len);
    let mut v = json!({ "name": f.name, "lastRevisionId": f.last_revision, "changeType": f.change_type,
        "override": f.change_type == change_type::PREFAB_OVERRIDE_CHANGED });
    let fits = f.before.as_ref().map_or(0, String::len) <= MAX_VALUE_BYTES
        && f.after.as_ref().map_or(0, String::len) <= MAX_VALUE_BYTES
        && size <= *budget_left;
    if fits {
        *budget_left -= size;
        v["before"] = json!(f.before);
        v["after"] = json!(f.after);
        v["oversize"] = json!(false);
    } else {
        v["before"] = Value::Null;
        v["after"] = Value::Null;
        v["oversize"] = json!(true);
        v["beforeBytes"] = json!(f.before.as_ref().map(String::len));
        v["afterBytes"] = json!(f.after.as_ref().map(String::len));
    }
    v
}

fn entity_json(e: &EntityOut) -> Value {
    // 1 つのエンティティの値の合計が 1 ページの予算の半分を超えたら、以後の値は oversize にする
    let mut budget = HUNKS_PAGE_BUDGET / 2;
    let hierarchy: Vec<Value> = e.hierarchy.iter().map(|f| field_json(f, &mut budget)).collect();
    let entity_fields: Vec<Value> = e.entity_fields.iter().map(|f| field_json(f, &mut budget)).collect();
    let components: Vec<Value> = e
        .components
        .iter()
        .map(|c| {
            let fields: Vec<Value> = c.fields.iter().map(|f| field_json(f, &mut budget)).collect();
            json!({ "name": c.name, "status": c.status.as_str(), "fields": fields })
        })
        .collect();
    json!({
        "fileId": e.file_id,
        "status": e.status.as_str(),
        "name": e.name,
        "hierarchyChanged": !e.hierarchy.is_empty(),
        "hierarchy": hierarchy,
        "entityFields": entity_fields,
        "components": components,
        "revisionIds": e.revision_ids,
    })
}

/// 文書の ID から、引き継ぎで同じ文書になる ID を全部集める (自分を含む)。
fn chain_ids(conn: &Connection, target: &str) -> rusqlite::Result<Vec<String>> {
    let mut ids = vec![target.to_string()];
    let mut i = 0;
    while i < ids.len() && ids.len() < MAX_CHAIN_DOCUMENTS {
        let mut stmt = conn.prepare_cached("SELECT document_id FROM documents WHERE successor_id = ?1")?;
        let found: Vec<String> = stmt.query_map([&ids[i]], |r| r.get(0))?.collect::<rusqlite::Result<_>>()?;
        for id in found {
            if !ids.contains(&id) {
                ids.push(id);
            }
        }
        i += 1;
    }
    Ok(ids)
}

/// 文書にかかる Revision (対象の行為者だけ) のレビューの行を、Revision ごとの索引にする。行が無い Revision は載らない。
fn load_review_indexes(conn: &Connection, marks: &str, ids: &[String]) -> rusqlite::Result<HashMap<i64, review::LatestIndex>> {
    let sql = format!(
        "SELECT v.revision_id, v.review_id, v.scope_kind, v.scope_ref, v.state
           FROM reviews v JOIN change_revisions r ON r.revision_id = v.revision_id
          WHERE r.actor IN ('ai', 'unknown')
            AND v.revision_id IN (SELECT revision_id FROM scene_changes WHERE document_id IN ({marks}))
          ORDER BY v.revision_id, v.review_id"
    );
    let mut stmt = conn.prepare(&sql)?;
    let mut grouped: HashMap<i64, Vec<review::ReviewRow>> = HashMap::new();
    let mut rows = stmt.query(params_from_iter(ids.iter()))?;
    while let Some(r) = rows.next()? {
        grouped.entry(r.get(0)?).or_default().push(review::ReviewRow {
            review_id: r.get(1)?,
            scope_kind: r.get(2)?,
            scope_ref: r.get(3)?,
            state: r.get(4)?,
        });
    }
    Ok(grouped.into_iter().map(|(id, rows)| (id, review::LatestIndex::new(&rows))).collect())
}

fn internal(e: rusqlite::Error) -> OpError {
    OpError::new(code::INTERNAL, format!("DB の読み取りに失敗: {e}"))
}

/// `scene_overlay`: 文書にかかっている構造の変更をエンティティごとにまとめて返す。
pub fn scene_overlay(conn: &Connection, mut args: Args) -> Result<Value, OpError> {
    let document_id = args.required_string("documentId")?;
    let after_revision = args.int("afterRevisionId")?;
    let cursor = args.string("cursor")?;
    // 承認済みの構造の変更は強調から外す (確認が済んだものにバッジを出し続けない)。既定は外す
    let exclude_approved = args.bool("excludeApproved")?.unwrap_or(true);
    args.finish()?;
    let after_entity = match &cursor {
        None => None,
        Some(text) => Some(text.parse::<i64>().map_err(|_| OpError::bad_args("cursor が読めない"))?),
    };
    let resolved = document::resolve(conn, &document_id).map_err(internal)?;
    let ids = chain_ids(conn, &resolved).map_err(internal)?;
    let marks = (1..=ids.len()).map(|n| format!("?{n}")).collect::<Vec<_>>().join(", ");

    let latest: Option<i64> = conn
        .query_row(
            &format!(
                "SELECT MAX(r.revision_id) FROM scene_changes s JOIN change_revisions r ON r.revision_id = s.revision_id
                  WHERE s.document_id IN ({marks})"
            ),
            params_from_iter(ids.iter()),
            |r| r.get(0),
        )
        .map_err(internal)?;

    // 行為者が AI か不明の Revision だけ (利用者の編集と復元は強調しない)。無効にされた行は除く
    let sql = format!(
        "SELECT r.revision_id, s.file_id, s.change_type, s.component, s.field, s.before_value, s.after_value
           FROM scene_changes s JOIN change_revisions r ON r.revision_id = s.revision_id
          WHERE s.document_id IN ({marks}) AND s.state = 'active' AND r.actor IN ('ai', 'unknown')
            AND r.revision_id > {after}
          ORDER BY r.revision_id, s.scene_change_id LIMIT {limit}",
        after = after_revision.unwrap_or(0),
        limit = MAX_OVERLAY_ROWS + 1
    );
    let reviews = if exclude_approved { load_review_indexes(conn, &marks, &ids).map_err(internal)? } else { HashMap::new() };
    let mut approved_hidden = 0usize;
    let mut stmt = conn.prepare(&sql).map_err(internal)?;
    let mut rows = stmt.query(params_from_iter(ids.iter())).map_err(internal)?;
    let mut revisions: Vec<RevisionRows> = Vec::new();
    let mut count = 0usize;
    let mut truncated = false;
    while let Some(r) = rows.next().map_err(internal)? {
        if count >= MAX_OVERLAY_ROWS {
            truncated = true;
            break;
        }
        count += 1;
        let revision_id: i64 = r.get(0).map_err(internal)?;
        let row = OverlayRow {
            file_id: r.get(1).map_err(internal)?,
            change_type: r.get(2).map_err(internal)?,
            component: r.get(3).map_err(internal)?,
            field: r.get(4).map_err(internal)?,
            before: r.get(5).map_err(internal)?,
            after: r.get(6).map_err(internal)?,
        };
        if let Some(index) = reviews.get(&revision_id) {
            let leaf = review::Leaf::Scene { file_id: row.file_id, component: row.component.clone(), field: row.field.clone() };
            if index.state_of(&leaf) == review::LeafState::Approved {
                approved_hidden += 1;
                continue;
            }
        }
        match revisions.last_mut() {
            Some(last) if last.revision_id == revision_id => last.rows.push(row),
            _ => revisions.push(RevisionRows { revision_id, rows: vec![row] }),
        }
    }
    let overlay = fold(&revisions);

    // エンティティを fileId 順にページ送り。1 ページは JSON の合計が予算以内
    let mut items: Vec<Value> = Vec::new();
    let mut used = 0usize;
    let mut next_cursor: Option<String> = None;
    for entity in overlay.entities.iter().filter(|e| after_entity.is_none_or(|after| e.file_id > after)) {
        let item = entity_json(entity);
        let size = serde_json::to_string(&item).map_or(usize::MAX, |t| t.len());
        if !items.is_empty() && used + size > HUNKS_PAGE_BUDGET {
            next_cursor = items.last().and_then(|v| v["fileId"].as_i64()).map(|n| n.to_string());
            break;
        }
        used += size;
        items.push(item);
    }
    let mut result = json!({
        "documentId": resolved,
        "entities": items,
        "nextCursor": next_cursor,
        "incomplete": overlay.incomplete || truncated,
        "truncated": truncated,
        "unparsedRevisionIds": overlay.unparsed_revisions,
        "latestRevisionId": latest,
        "approvedHidden": approved_hidden,
    });
    // ルート直下の変更は最初のページだけに入れる (ページをまたいで重複させない)
    if after_entity.is_none() {
        let mut budget = HUNKS_PAGE_BUDGET / 4;
        let changes: Vec<Value> = overlay.document_changes.iter().map(|f| field_json(f, &mut budget)).collect();
        result["documentChanges"] = Value::Array(changes);
        result["assetChangedRevisionId"] = json!(overlay.asset_changed);
    }
    Ok(result)
}

/// `document_for_path`: パスの今の文書の `documentId` (引き継ぎをたどったもの)。記録が無ければ同じ規則でその場で決める。
pub fn document_for_path(conn: &Connection, root: &Path, mut args: Args) -> Result<Value, OpError> {
    let path = args.required_string("path")?;
    args.finish()?;
    let rel = normalize_path(root, &path, false)?;
    let Some(kind) = DocKind::from_path(&rel) else {
        return Err(OpError::bad_args("path は .scene.json / .prefab.json / .actor.json でなければならない"));
    };
    let found: Option<String> = conn
        .query_row(
            "SELECT document_id FROM documents WHERE current_path = ?1 ORDER BY rowid DESC LIMIT 1",
            [&rel],
            |r| r.get(0),
        )
        .optional()
        .map_err(internal)?;
    if let Some(id) = found {
        let resolved = document::resolve(conn, &id).map_err(internal)?;
        let id_source: String = conn
            .query_row("SELECT id_source FROM documents WHERE document_id = ?1", [&resolved], |r| r.get(0))
            .map_err(internal)?;
        return Ok(json!({ "documentId": resolved, "idSource": id_source, "kind": kind.as_str(), "known": true, "path": rel }));
    }
    let (id, id_source) = match document::read_meta_guid(root, &rel) {
        Some(guid) => (guid, "meta"),
        None => (document::path_id(&rel), "path"),
    };
    Ok(json!({ "documentId": id, "idSource": id_source, "kind": kind.as_str(), "known": false, "path": rel }))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(file_id: Option<i64>, ct: &str, component: Option<&str>, field: Option<&str>, before: Option<&str>, after: Option<&str>) -> OverlayRow {
        OverlayRow {
            file_id,
            change_type: ct.to_string(),
            component: component.map(str::to_string),
            field: field.map(str::to_string),
            before: before.map(str::to_string),
            after: after.map(str::to_string),
        }
    }

    fn rev(id: i64, rows: Vec<OverlayRow>) -> RevisionRows {
        RevisionRows { revision_id: id, rows }
    }

    fn field_row(fid: i64, comp: &str, field: &str, b: &str, a: &str) -> OverlayRow {
        row(Some(fid), change_type::FIELD_MODIFIED, Some(comp), Some(field), Some(b), Some(a))
    }

    fn ent_json(fid: i64, name: &str, comps: &str) -> String {
        format!("{{\"fileId\":{fid},\"name\":\"{name}\",\"childIndex\":0,\"components\":{{{comps}}}}}")
    }

    #[test]
    fn added_then_modified_stays_added() {
        let o = fold(&[
            rev(1, vec![row(Some(5), change_type::ENTITY_ADDED, None, None, None, Some(&ent_json(5, "Box", "")))]),
            rev(2, vec![field_row(5, "T", "x", "1", "2")]),
        ]);
        assert_eq!(o.entities.len(), 1);
        assert_eq!((o.entities[0].status, o.entities[0].name.as_deref()), (Status::Added, Some("Box")));
        assert!(o.entities[0].components.is_empty());
        assert_eq!(o.entities[0].revision_ids, vec![1, 2]);
    }

    #[test]
    fn added_then_removed_vanishes_and_modified_then_removed_is_removed() {
        let o = fold(&[
            rev(1, vec![row(Some(5), change_type::ENTITY_ADDED, None, None, None, Some(&ent_json(5, "a", "")))]),
            rev(2, vec![row(Some(5), change_type::ENTITY_REMOVED, None, None, Some(&ent_json(5, "a", "")), None)]),
            rev(3, vec![field_row(6, "T", "x", "1", "2")]),
            rev(4, vec![row(Some(6), change_type::ENTITY_REMOVED, None, None, Some(&ent_json(6, "b", "")), None)]),
        ]);
        assert_eq!(o.entities.len(), 1);
        assert_eq!((o.entities[0].file_id, o.entities[0].status), (6, Status::Removed));
    }

    #[test]
    fn removed_then_added_is_modified_from_first_before_to_last_after() {
        let before = ent_json(7, "x", "\"T\":{\"v\":1}");
        let after = ent_json(7, "x", "\"T\":{\"v\":2}");
        let o = fold(&[
            rev(1, vec![row(Some(7), change_type::ENTITY_REMOVED, None, None, Some(&before), None)]),
            rev(2, vec![row(Some(7), change_type::ENTITY_ADDED, None, None, None, Some(&after))]),
        ]);
        assert_eq!(o.entities.len(), 1);
        let e = &o.entities[0];
        assert_eq!(e.status, Status::Modified);
        assert_eq!(e.components.len(), 1);
        let f = &e.components[0].fields[0];
        assert_eq!((f.name.as_str(), f.before.as_deref(), f.after.as_deref()), ("v", Some("1"), Some("2")));
        // 中身が同じに戻っていれば消える
        let o = fold(&[
            rev(1, vec![row(Some(7), change_type::ENTITY_REMOVED, None, None, Some(&before), None)]),
            rev(2, vec![row(Some(7), change_type::ENTITY_ADDED, None, None, None, Some(&before))]),
        ]);
        assert!(o.entities.is_empty());
    }

    #[test]
    fn a_field_changed_back_vanishes_and_spans_revisions() {
        let o = fold(&[
            rev(1, vec![field_row(1, "T", "x", "1", "2")]),
            rev(2, vec![field_row(1, "T", "x", "2", "3"), field_row(1, "T", "y", "0", "9")]),
            rev(3, vec![field_row(2, "T", "x", "1", "2")]),
            rev(4, vec![field_row(2, "T", "x", "2", "1.0")]),
            rev(5, vec![field_row(3, "T", "p", "[1, 2]", "[3]")]),
            rev(6, vec![field_row(3, "T", "p", "[3]", "[1,2]")]),
        ]);
        // 1: x は 最初の Before と最後の After。2: 元へ戻ったのでは無い (1 と 1.0 は別の字句)。3: 空白だけ違う同じ値に戻った → 消える
        let ids: Vec<i64> = o.entities.iter().map(|e| e.file_id).collect();
        assert_eq!(ids, vec![1, 2]);
        let x = o.entities[0].components[0].fields.iter().find(|f| f.name == "x").unwrap();
        assert_eq!((x.before.as_deref(), x.after.as_deref(), x.last_revision), (Some("1"), Some("3"), 2));
        let back = fold(&[rev(1, vec![field_row(2, "T", "x", "1", "2")]), rev(2, vec![field_row(2, "T", "x", "2", "1")])]);
        assert!(back.entities.is_empty());
    }

    #[test]
    fn components_added_then_removed_vanish() {
        let o = fold(&[
            rev(1, vec![row(Some(1), change_type::COMPONENT_ADDED, Some("C"), None, None, Some("{}"))]),
            rev(2, vec![field_row(1, "C", "k", "1", "2")]),
            rev(3, vec![row(Some(1), change_type::COMPONENT_REMOVED, Some("C"), None, Some("{}"), None)]),
        ]);
        assert!(o.entities.is_empty());
        let o = fold(&[rev(1, vec![row(Some(1), change_type::COMPONENT_REMOVED, Some("C"), None, Some("{}"), None)])]);
        assert_eq!(o.entities[0].components[0].status, Status::Removed);
    }

    #[test]
    fn hierarchy_and_override_keys_fold_like_fields() {
        let o = fold(&[
            rev(1, vec![row(Some(1), change_type::HIERARCHY_CHANGED, None, Some("parent"), Some("2"), Some("3"))]),
            rev(2, vec![row(Some(1), change_type::HIERARCHY_CHANGED, None, Some("parent"), Some("3"), Some("2"))]),
            rev(3, vec![row(Some(4), change_type::PREFAB_OVERRIDE_CHANGED, Some("T"), Some("x"), None, Some("\"T.x\""))]),
            rev(4, vec![row(Some(4), change_type::PREFAB_OVERRIDE_CHANGED, Some("T"), Some("x"), Some("\"T.x\""), None)]),
            rev(5, vec![row(Some(8), change_type::PREFAB_OVERRIDE_CHANGED, Some("T"), Some("x"), Some("1"), Some("2"))]),
        ]);
        assert_eq!(o.entities.len(), 1);
        assert_eq!(o.entities[0].file_id, 8);
        assert!(o.entities[0].components[0].fields[0].change_type == change_type::PREFAB_OVERRIDE_CHANGED);
    }

    #[test]
    fn unparsed_revisions_make_the_result_incomplete_and_document_changes_fold() {
        let o = fold(&[
            rev(1, vec![row(None, change_type::FIELD_MODIFIED, None, Some("sceneName"), Some("\"a\""), Some("\"b\""))]),
            rev(2, vec![row(None, change_type::UNPARSED, None, None, None, Some("\"理由\""))]),
            rev(3, vec![row(None, change_type::PREFAB_ASSET_CHANGED, None, None, None, None)]),
        ]);
        assert!(o.incomplete);
        assert_eq!(o.unparsed_revisions, vec![2]);
        assert_eq!(o.document_changes.len(), 1);
        assert_eq!(o.asset_changed, Some(3));
        assert!(!fold(&[]).incomplete);
    }

    #[test]
    fn entity_name_comes_from_the_added_fragment_or_a_name_change() {
        let o = fold(&[rev(1, vec![row(Some(1), change_type::ENTITY_MODIFIED, None, Some("name"), Some("\"a\""), Some("\"b\""))])]);
        assert_eq!(o.entities[0].name.as_deref(), Some("b"));
        assert_eq!(o.entities[0].entity_fields[0].name, "name");
    }
}
