//! シーン / プレハブ / アクターの構造比較 (architecture.md §6.3、§6.4)。
//!
//! (Before のバイト列, After のバイト列, 種類) → 構造の変更の列、または「解析不可」と理由、を返す純粋な関数。
//! DB・ファイルに触れない。比べるのは値の正規形 (lexjson.rs)。数値は字句のまま、オブジェクトはキーの集合として見る。
//! 計算量は入力の大きさにほぼ比例する (エンティティは `fileId` のハッシュで対応付ける)。

use std::collections::{HashMap, HashSet};

use crate::lexjson::{self, Kind, Node};

/// 構造の変更の種別 (`scene_changes.change_type`。要求 §7.2 の 11 種のうち `PrefabPropagation` 以外は、このサブが出す)。
pub mod change_type {
    pub const ENTITY_ADDED: &str = "EntityAdded";
    pub const ENTITY_REMOVED: &str = "EntityRemoved";
    pub const HIERARCHY_CHANGED: &str = "HierarchyChanged";
    pub const COMPONENT_ADDED: &str = "ComponentAdded";
    pub const COMPONENT_REMOVED: &str = "ComponentRemoved";
    pub const FIELD_MODIFIED: &str = "FieldModified";
    pub const ENTITY_MODIFIED: &str = "EntityModified";
    pub const PREFAB_ASSET_CHANGED: &str = "PrefabAssetChanged";
    pub const PREFAB_INSTANCE_CHANGED: &str = "PrefabInstanceChanged";
    pub const PREFAB_OVERRIDE_CHANGED: &str = "PrefabOverrideChanged";
    /// 構造を比べられなかった。`after_value` に理由の JSON 文字列を入れる (db-schema.md §3)。
    pub const UNPARSED: &str = "unparsed";
}

/// 文書の種類 (拡張子で決まる)。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DocKind {
    Scene,
    Prefab,
    Actor,
}

impl DocKind {
    pub fn as_str(self) -> &'static str {
        match self {
            DocKind::Scene => "scene",
            DocKind::Prefab => "prefab",
            DocKind::Actor => "actor",
        }
    }

    /// 相対パスの拡張子から。大文字小文字は区別しない。
    pub fn from_path(path: &str) -> Option<DocKind> {
        let lower = path.to_ascii_lowercase();
        if lower.ends_with(".scene.json") {
            Some(DocKind::Scene)
        } else if lower.ends_with(".prefab.json") {
            Some(DocKind::Prefab)
        } else if lower.ends_with(".actor.json") {
            Some(DocKind::Actor)
        } else {
            None
        }
    }
}

/// 構造の変更 1 件。値は元のバイト列から切り出した JSON の断片。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SceneChange {
    pub file_id: Option<i64>,
    pub change_type: &'static str,
    pub component: Option<String>,
    pub field: Option<String>,
    pub before_value: Option<String>,
    pub after_value: Option<String>,
}

impl SceneChange {
    fn new(file_id: Option<i64>, change_type: &'static str) -> SceneChange {
        SceneChange { file_id, change_type, component: None, field: None, before_value: None, after_value: None }
    }
}

/// 構造比較の結果。
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SceneOutcome {
    Changes(Vec<SceneChange>),
    /// 比べられなかった理由。ファイル単位の Revision と行 diff は残る。
    Unparsed(String),
}

impl SceneOutcome {
    /// 保存する行 (解析不可は 1 行)。
    pub fn rows(&self) -> Vec<SceneChange> {
        match self {
            SceneOutcome::Changes(changes) => changes.clone(),
            SceneOutcome::Unparsed(reason) => {
                let mut row = SceneChange::new(None, change_type::UNPARSED);
                row.after_value = Some(serde_json::to_string(reason).unwrap_or_else(|_| "\"\"".to_string()));
                vec![row]
            }
        }
    }
}

/// シーンの知っている版 (`Scene::kDocVersion` が現行 4。版の無い文書は 1 として読まれる)。
const SCENE_VERSIONS: std::ops::RangeInclusive<i64> = 1..=4;
/// プレハブ / アクターの宣言キー (`"prefab": 1` / `"actor": 1`) の知っている値。
const COMPOSE_VERSION: i64 = 1;
const COMPONENT_PREFAB_INSTANCE: &str = "PrefabInstanceComponent";
const FIELD_PREFAB_HASH: &str = "prefabHash";

/// 比べる。`max_side_bytes` は 1 つの文書の大きさの上限 (超えたら解析不可)。
pub fn diff(kind: DocKind, before: &[u8], after: &[u8], max_side_bytes: usize) -> SceneOutcome {
    match diff_inner(kind, before, after, max_side_bytes) {
        Ok(changes) => SceneOutcome::Changes(changes),
        Err(reason) => SceneOutcome::Unparsed(reason),
    }
}

/// Before か After の片方しか無い (追加 / 削除) とき。シーンは構造の変更を出さず、プレハブ / アクターは文書全体の 1 件。
pub fn whole_file(kind: DocKind) -> SceneOutcome {
    match kind {
        DocKind::Scene => SceneOutcome::Changes(Vec::new()),
        DocKind::Prefab | DocKind::Actor => {
            SceneOutcome::Changes(vec![SceneChange::new(None, change_type::PREFAB_ASSET_CHANGED)])
        }
    }
}

struct Doc<'a> {
    src: &'a str,
    root: Node,
}

/// 読む。理由には `side` (前 / 後) を付ける。
fn load<'a>(side: &str, bytes: &'a [u8], max: usize) -> Result<Doc<'a>, String> {
    if bytes.len() > max {
        return Err(format!("大きすぎる ({side}: {} バイト、上限 {max})", bytes.len()));
    }
    let src = std::str::from_utf8(bytes).map_err(|_| format!("UTF-8 でない ({side})"))?;
    let root = lexjson::parse(src).map_err(|e| format!("JSON を読めない ({side}): {e}"))?;
    if !matches!(root.kind, Kind::Object(_)) {
        return Err(format!("ルートがオブジェクトでない ({side})"));
    }
    match root.get("entities") {
        Some(Node { kind: Kind::Array(_), .. }) => {}
        _ => return Err(format!("entities が無い、または配列でない ({side})")),
    }
    Ok(Doc { src, root })
}

/// 版を整数で返す。シーンは `version` (無ければ 1)、プレハブ / アクターは `prefab` / `actor`。
fn version_of(kind: DocKind, doc: &Doc, side: &str) -> Result<(String, i64), String> {
    let key = match kind {
        DocKind::Scene => "version",
        DocKind::Prefab | DocKind::Actor => {
            if doc.root.get("actor").is_some() {
                "actor"
            } else {
                "prefab"
            }
        }
    };
    let Some(node) = doc.root.get(key) else {
        return match kind {
            DocKind::Scene => Ok((key.to_string(), 1)),
            _ => Err(format!("prefab / actor の宣言が無い ({side})")),
        };
    };
    let text = node.fragment(doc.src);
    let value = match node.kind {
        Kind::Number => text.parse::<i64>().map_err(|_| format!("版が整数でない ({side}): {text}"))?,
        _ => return Err(format!("版が数値でない ({side}): {text}")),
    };
    Ok((key.to_string(), value))
}

fn diff_inner(kind: DocKind, before: &[u8], after: &[u8], max: usize) -> Result<Vec<SceneChange>, String> {
    let b = load("前", before, max)?;
    let a = load("後", after, max)?;
    let (key_b, ver_b) = version_of(kind, &b, "前")?;
    let (key_a, ver_a) = version_of(kind, &a, "後")?;
    if key_b != key_a {
        return Err(format!("宣言のキーが違う: 前 {key_b} / 後 {key_a}"));
    }
    if ver_b != ver_a {
        return Err(format!("版が違う: 前 {ver_b} / 後 {ver_a}"));
    }
    let known = match kind {
        DocKind::Scene => SCENE_VERSIONS.contains(&ver_a),
        _ => ver_a == COMPOSE_VERSION,
    };
    if !known {
        return Err(format!("知らない版: {ver_a}"));
    }

    let ents_b = entities(&b, "前")?;
    let ents_a = entities(&a, "後")?;
    let mut out: Vec<SceneChange> = Vec::new();

    // 文書全体の変更 (ルート直下)。`nextFileId` はエンティティの追加に付随するので単独では出さない
    diff_root(&b, &a, &key_a, &mut out);

    let index_b: HashMap<i64, usize> = ents_b.iter().enumerate().map(|(i, e)| (e.file_id, i)).collect();
    let mut seen: HashSet<i64> = HashSet::with_capacity(ents_a.len());
    for ea in &ents_a {
        seen.insert(ea.file_id);
        match index_b.get(&ea.file_id) {
            None => {
                let mut c = SceneChange::new(Some(ea.file_id), change_type::ENTITY_ADDED);
                c.after_value = Some(ea.node.fragment(a.src).to_string());
                out.push(c);
            }
            Some(&i) => diff_entity(ea.file_id, ents_b[i].node, b.src, ea.node, a.src, &mut out),
        }
    }
    for eb in &ents_b {
        if !seen.contains(&eb.file_id) {
            let mut c = SceneChange::new(Some(eb.file_id), change_type::ENTITY_REMOVED);
            c.before_value = Some(eb.node.fragment(b.src).to_string());
            out.push(c);
        }
    }

    if matches!(kind, DocKind::Prefab | DocKind::Actor) && !out.is_empty() {
        out.insert(0, SceneChange::new(None, change_type::PREFAB_ASSET_CHANGED));
    }
    Ok(out)
}

struct Entity<'a> {
    file_id: i64,
    node: &'a Node,
}

/// `entities` を読む。`fileId` が無い・整数でない・重複しているものは解析不可。
fn entities<'a>(doc: &'a Doc, side: &str) -> Result<Vec<Entity<'a>>, String> {
    let Some(Node { kind: Kind::Array(items), .. }) = doc.root.get("entities") else {
        return Err(format!("entities が無い ({side})"));
    };
    let mut out = Vec::with_capacity(items.len());
    let mut seen: HashSet<i64> = HashSet::with_capacity(items.len());
    for (position, node) in items.iter().enumerate() {
        if !matches!(node.kind, Kind::Object(_)) {
            return Err(format!("entities[{position}] がオブジェクトでない ({side})"));
        }
        let Some(id_node) = node.get("fileId") else {
            return Err(format!("entities[{position}] に fileId が無い ({side})"));
        };
        let text = id_node.fragment(doc.src);
        let file_id = match id_node.kind {
            Kind::Number => text.parse::<i64>().map_err(|_| format!("fileId が整数でない ({side}): {text}"))?,
            _ => return Err(format!("fileId が数値でない ({side}): {text}")),
        };
        if !seen.insert(file_id) {
            return Err(format!("fileId が重複している ({side}): {file_id}"));
        }
        out.push(Entity { file_id, node });
    }
    Ok(out)
}

fn fragment_of(node: Option<&Node>, src: &str) -> Option<String> {
    node.map(|n| n.fragment(src).to_string())
}

/// ルート直下のキー (`entities` / 版の宣言 / `nextFileId` を除く) の変化。
fn diff_root(b: &Doc, a: &Doc, version_key: &str, out: &mut Vec<SceneChange>) {
    let skip = |k: &str| matches!(k, "entities" | "nextFileId") || k == version_key;
    diff_keys(
        b.root.as_object().unwrap_or(&[]),
        b.src,
        a.root.as_object().unwrap_or(&[]),
        a.src,
        &skip,
        &mut |key, vb, va| {
            let mut c = SceneChange::new(None, change_type::FIELD_MODIFIED);
            c.field = Some(key.to_string());
            c.before_value = fragment_of(vb, b.src);
            c.after_value = fragment_of(va, a.src);
            out.push(c);
        },
    );
}

/// 2 つのオブジェクトのキーごとの差。After の順に、続けて Before にだけ有るキー。`skip` に当たるキーは見ない。
fn diff_keys(
    pb: &[(String, Node)],
    sb: &str,
    pa: &[(String, Node)],
    sa: &str,
    skip: &dyn Fn(&str) -> bool,
    emit: &mut dyn FnMut(&str, Option<&Node>, Option<&Node>),
) {
    let index_b: HashMap<&str, &Node> = pb.iter().map(|(k, v)| (k.as_str(), v)).collect();
    let keys_a: HashSet<&str> = pa.iter().map(|(k, _)| k.as_str()).collect();
    for (key, va) in pa {
        if skip(key) {
            continue;
        }
        match index_b.get(key.as_str()) {
            Some(vb) if lexjson::same(vb, sb, va, sa) => {}
            other => emit(key, other.copied(), Some(va)),
        }
    }
    for (key, vb) in pb {
        if !skip(key) && !keys_a.contains(key.as_str()) {
            emit(key, Some(vb), None);
        }
    }
}

/// `overrides` を文字列の集合として読む。無ければ空。配列でない・文字列以外を含むものは None (汎用の値として比べる)。
fn override_keys<'a>(entity: &'a Node) -> Option<Vec<&'a str>> {
    match entity.get("overrides") {
        None => Some(Vec::new()),
        Some(Node { kind: Kind::Array(items), .. }) => items
            .iter()
            .map(|n| match &n.kind {
                Kind::Str(s) => Some(s.as_str()),
                _ => None,
            })
            .collect(),
        Some(_) => None,
    }
}

/// `"Comp.field"` / `"+Comp"` / `"-Comp"` / `"name"` / `"tagMask"` を (component, field) に分ける。
fn split_override(key: &str) -> (Option<String>, Option<String>) {
    if let Some(rest) = key.strip_prefix('+') {
        return (Some(rest.to_string()), Some("+".to_string()));
    }
    if let Some(rest) = key.strip_prefix('-') {
        return (Some(rest.to_string()), Some("-".to_string()));
    }
    match key.rsplit_once('.') {
        Some((component, field)) => (Some(component.to_string()), Some(field.to_string())),
        None => (None, Some(key.to_string())),
    }
}

fn diff_entity(file_id: i64, eb: &Node, sb: &str, ea: &Node, sa: &str, out: &mut Vec<SceneChange>) {
    if eb.fragment(sb) == ea.fragment(sa) {
        return;
    }
    let (Some(pb), Some(pa)) = (eb.as_object(), ea.as_object()) else { return };
    let keys_b = override_keys(eb);
    let keys_a = override_keys(ea);
    let overrides_are_sets = keys_b.is_some() && keys_a.is_some();
    let overridden: HashSet<&str> =
        keys_b.iter().flatten().chain(keys_a.iter().flatten()).copied().collect();

    // 親子
    for key in ["parent", "childIndex"] {
        let (vb, va) = (eb.get(key), ea.get(key));
        let differs = match (vb, va) {
            (None, None) => false,
            (Some(x), Some(y)) => !lexjson::same(x, sb, y, sa),
            _ => true,
        };
        if differs {
            let mut c = SceneChange::new(Some(file_id), change_type::HIERARCHY_CHANGED);
            c.field = Some(key.to_string());
            c.before_value = fragment_of(vb, sb);
            c.after_value = fragment_of(va, sa);
            out.push(c);
        }
    }

    // コンポーネント
    match (eb.get("components"), ea.get("components")) {
        (Some(Node { kind: Kind::Object(cb), .. }), Some(Node { kind: Kind::Object(ca), .. })) => {
            diff_components(file_id, cb, sb, ca, sa, &overridden, out);
        }
        (None, None) => {}
        (vb, va) => {
            let same = matches!((vb, va), (Some(x), Some(y)) if lexjson::same(x, sb, y, sa));
            if !same {
                let mut c = SceneChange::new(Some(file_id), change_type::ENTITY_MODIFIED);
                c.field = Some("components".to_string());
                c.before_value = fragment_of(vb, sb);
                c.after_value = fragment_of(va, sa);
                out.push(c);
            }
        }
    }

    // エンティティ直下のその他 (name / tagMask など)。overrides が集合として読めるときは別に比べる
    let skip = |k: &str| {
        matches!(k, "fileId" | "parent" | "childIndex" | "components") || (k == "overrides" && overrides_are_sets)
    };
    diff_keys(pb, sb, pa, sa, &skip, &mut |key, vb, va| {
        let overridden_field = matches!(key, "name" | "tagMask") && overridden.contains(key);
        let mut c = SceneChange::new(
            Some(file_id),
            if overridden_field { change_type::PREFAB_OVERRIDE_CHANGED } else { change_type::ENTITY_MODIFIED },
        );
        c.field = Some(key.to_string());
        c.before_value = fragment_of(vb, sb);
        c.after_value = fragment_of(va, sa);
        out.push(c);
    });

    // overrides の集合の増減
    if let (Some(kb), Some(ka)) = (keys_b, keys_a) {
        let set_b: HashSet<&str> = kb.iter().copied().collect();
        let set_a: HashSet<&str> = ka.iter().copied().collect();
        for key in &ka {
            if !set_b.contains(key) {
                out.push(override_set_change(file_id, key, true));
            }
        }
        for key in &kb {
            if !set_a.contains(key) {
                out.push(override_set_change(file_id, key, false));
            }
        }
    }
}

fn override_set_change(file_id: i64, key: &str, added: bool) -> SceneChange {
    let (component, field) = split_override(key);
    let mut c = SceneChange::new(Some(file_id), change_type::PREFAB_OVERRIDE_CHANGED);
    c.component = component;
    c.field = field;
    // 集合の要素そのもの (キーの文字列) を値として残す
    let literal = serde_json::to_string(key).ok();
    if added {
        c.after_value = literal;
    } else {
        c.before_value = literal;
    }
    c
}

fn diff_components(
    file_id: i64,
    cb: &[(String, Node)],
    sb: &str,
    ca: &[(String, Node)],
    sa: &str,
    overridden: &HashSet<&str>,
    out: &mut Vec<SceneChange>,
) {
    let index_b: HashMap<&str, &Node> = cb.iter().map(|(k, v)| (k.as_str(), v)).collect();
    let names_a: HashSet<&str> = ca.iter().map(|(k, _)| k.as_str()).collect();
    for (name, va) in ca {
        match index_b.get(name.as_str()) {
            None => {
                let mut c = SceneChange::new(Some(file_id), change_type::COMPONENT_ADDED);
                c.component = Some(name.clone());
                c.after_value = Some(va.fragment(sa).to_string());
                out.push(c);
            }
            Some(vb) => diff_component(file_id, name, vb, sb, va, sa, overridden, out),
        }
    }
    for (name, vb) in cb {
        if !names_a.contains(name.as_str()) {
            let mut c = SceneChange::new(Some(file_id), change_type::COMPONENT_REMOVED);
            c.component = Some(name.clone());
            c.before_value = Some(vb.fragment(sb).to_string());
            out.push(c);
        }
    }
}

fn diff_component(
    file_id: i64,
    name: &str,
    vb: &Node,
    sb: &str,
    va: &Node,
    sa: &str,
    overridden: &HashSet<&str>,
    out: &mut Vec<SceneChange>,
) {
    if lexjson::same(vb, sb, va, sa) {
        return;
    }
    match (vb.as_object(), va.as_object()) {
        (Some(pb), Some(pa)) => {
            // 入れ子の中だけが変わっても、最上位のキー 1 つの変更として出す
            diff_keys(pb, sb, pa, sa, &|_| false, &mut |key, fb, fa| {
                let change_type = if overridden.contains(format!("{name}.{key}").as_str()) {
                    change_type::PREFAB_OVERRIDE_CHANGED
                } else if name == COMPONENT_PREFAB_INSTANCE && key == FIELD_PREFAB_HASH {
                    change_type::PREFAB_INSTANCE_CHANGED
                } else {
                    change_type::FIELD_MODIFIED
                };
                let mut c = SceneChange::new(Some(file_id), change_type);
                c.component = Some(name.to_string());
                c.field = Some(key.to_string());
                c.before_value = fragment_of(fb, sb);
                c.after_value = fragment_of(fa, sa);
                out.push(c);
            });
        }
        // どちらかがオブジェクトでない: コンポーネント全体を 1 つの値として出す
        _ => {
            let mut c = SceneChange::new(Some(file_id), change_type::FIELD_MODIFIED);
            c.component = Some(name.to_string());
            c.before_value = Some(vb.fragment(sb).to_string());
            c.after_value = Some(va.fragment(sa).to_string());
            out.push(c);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LIMIT: usize = 8 * 1024 * 1024;

    fn scene(entities: &str) -> String {
        format!("{{\"engine\":\"MyEngine\",\"version\":4,\"sceneName\":\"s\",\"nextFileId\":9,\"entities\":[{entities}]}}")
    }

    fn changes(kind: DocKind, before: &str, after: &str) -> Vec<SceneChange> {
        match diff(kind, before.as_bytes(), after.as_bytes(), LIMIT) {
            SceneOutcome::Changes(c) => c,
            SceneOutcome::Unparsed(reason) => panic!("解析不可: {reason}"),
        }
    }

    fn unparsed(before: &str, after: &str) -> String {
        match diff(DocKind::Scene, before.as_bytes(), after.as_bytes(), LIMIT) {
            SceneOutcome::Unparsed(reason) => reason,
            SceneOutcome::Changes(c) => panic!("解析できてしまった: {c:?}"),
        }
    }

    fn ent(file_id: i64, body: &str) -> String {
        format!("{{\"fileId\":{file_id},\"name\":\"e\",\"childIndex\":0,\"components\":{{{body}}}}}")
    }

    #[test]
    fn one_field_change_is_one_field_modified() {
        let b = scene(&ent(1, "\"LocalTransform\":{\"position\":[0.0,1.0,0.0],\"scale\":[1,1,1]}"));
        let a = scene(&ent(1, "\"LocalTransform\":{\"position\":[0.0,2.0,0.0],\"scale\":[1,1,1]}"));
        let c = changes(DocKind::Scene, &b, &a);
        assert_eq!(c.len(), 1, "{c:?}");
        assert_eq!(c[0].change_type, change_type::FIELD_MODIFIED);
        assert_eq!((c[0].file_id, c[0].component.as_deref(), c[0].field.as_deref()), (Some(1), Some("LocalTransform"), Some("position")));
        assert_eq!(c[0].before_value.as_deref(), Some("[0.0,1.0,0.0]"));
        assert_eq!(c[0].after_value.as_deref(), Some("[0.0,2.0,0.0]"));
    }

    #[test]
    fn formatting_and_order_changes_are_not_changes() {
        let b = scene(&format!("{},{}", ent(1, "\"A\":{\"x\":1,\"y\":2}"), ent(2, "\"B\":{\"z\":\"A\"}")));
        // キーの順序・空白・改行・エンティティの並び・文字列のエスケープだけを変える
        let a = format!(
            "{{ \"entities\" : [\n {{\"components\":{{\"B\":{{\"z\":\"\\u0041\"}}}},\"childIndex\":0,\"name\":\"e\",\"fileId\":2}},\n{{\"fileId\":1,\"name\":\"e\",\"childIndex\":0,\"components\":{{\"A\":{{\"y\":2,\"x\":1}}}}}} ],\n\"nextFileId\":9,\"sceneName\":\"s\",\"version\":4,\"engine\":\"MyEngine\" }}"
        );
        assert_eq!(changes(DocKind::Scene, &b, &a), Vec::new());
    }

    #[test]
    fn number_lexemes_are_distinct() {
        for (x, y) in [("0.1", "0.10000000149011612"), ("1.0", "1"), ("1e0", "1")] {
            let b = scene(&ent(1, &format!("\"A\":{{\"v\":{x}}}")));
            let a = scene(&ent(1, &format!("\"A\":{{\"v\":{y}}}")));
            let c = changes(DocKind::Scene, &b, &a);
            assert_eq!(c.len(), 1, "{x} と {y}");
            assert_eq!((c[0].before_value.as_deref(), c[0].after_value.as_deref()), (Some(x), Some(y)));
        }
        let same = scene(&ent(1, "\"A\":{\"v\":0.1}"));
        assert!(changes(DocKind::Scene, &same, &same).is_empty());
    }

    #[test]
    fn structural_kinds() {
        let b = scene(&format!("{},{}", ent(1, "\"A\":{\"x\":1}"), ent(2, "\"B\":{\"x\":1}")));
        // 2 を削除、3 を追加、1 の親・childIndex・name を変え、A を消して C を足す
        let a = scene(&format!(
            "{{\"fileId\":1,\"name\":\"renamed\",\"parent\":3,\"childIndex\":5,\"components\":{{\"C\":{{}}}}}},{}",
            ent(3, "")
        ));
        let c = changes(DocKind::Scene, &b, &a);
        let kinds: Vec<(&str, Option<i64>, Option<&str>)> =
            c.iter().map(|x| (x.change_type, x.file_id, x.field.as_deref())).collect();
        assert!(kinds.contains(&(change_type::ENTITY_ADDED, Some(3), None)), "{kinds:?}");
        assert!(kinds.contains(&(change_type::ENTITY_REMOVED, Some(2), None)), "{kinds:?}");
        assert!(kinds.contains(&(change_type::HIERARCHY_CHANGED, Some(1), Some("parent"))), "{kinds:?}");
        assert!(kinds.contains(&(change_type::HIERARCHY_CHANGED, Some(1), Some("childIndex"))), "{kinds:?}");
        assert!(kinds.contains(&(change_type::ENTITY_MODIFIED, Some(1), Some("name"))), "{kinds:?}");
        assert!(kinds.contains(&(change_type::COMPONENT_ADDED, Some(1), None)), "{kinds:?}");
        assert!(kinds.contains(&(change_type::COMPONENT_REMOVED, Some(1), None)), "{kinds:?}");
        assert_eq!(c.len(), 7, "{kinds:?}");
    }

    #[test]
    fn nested_change_is_one_top_level_field() {
        let b = scene(&ent(1, "\"A\":{\"m\":{\"a\":{\"b\":[1,2]},\"c\":3},\"k\":0}"));
        let a = scene(&ent(1, "\"A\":{\"m\":{\"a\":{\"b\":[1,9]},\"c\":3},\"k\":0}"));
        let c = changes(DocKind::Scene, &b, &a);
        assert_eq!(c.len(), 1);
        assert_eq!(c[0].field.as_deref(), Some("m"));
        assert_eq!(c[0].before_value.as_deref(), Some("{\"a\":{\"b\":[1,2]},\"c\":3}"));
    }

    #[test]
    fn prefab_overrides_and_instance() {
        let base = |extra: &str, pos: &str, hash: &str| {
            format!(
                "{{\"fileId\":1,\"name\":\"p\",\"childIndex\":0{extra},\"components\":{{\"LocalTransform\":{{\"position\":{pos},\"scale\":1}},\"PrefabInstanceComponent\":{{\"prefabHash\":{hash},\"x\":1}}}}}}"
            )
        };
        // 値の変化だけ (override 済み) → PrefabOverrideChanged 1 件、FieldModified は出ない
        let b = scene(&base(",\"overrides\":[\"LocalTransform.position\"]", "[1]", "5"));
        let a = scene(&base(",\"overrides\":[\"LocalTransform.position\"]", "[2]", "5"));
        let c = changes(DocKind::Scene, &b, &a);
        assert_eq!(c.len(), 1, "{c:?}");
        assert_eq!(c[0].change_type, change_type::PREFAB_OVERRIDE_CHANGED);
        // 集合の変化 (並びだけの変更は変化でない)
        let b = scene(&base(",\"overrides\":[\"A.x\",\"+C\"]", "[1]", "5"));
        let a = scene(&base(",\"overrides\":[\"+C\",\"-D\"]", "[1]", "5"));
        let c = changes(DocKind::Scene, &b, &a);
        assert_eq!(c.len(), 2, "{c:?}");
        assert!(c.iter().all(|x| x.change_type == change_type::PREFAB_OVERRIDE_CHANGED));
        // prefabHash は PrefabInstanceChanged、他のフィールドは FieldModified
        let b = scene(&base("", "[1]", "5"));
        let a = scene(&base("", "[1]", "6"));
        let c = changes(DocKind::Scene, &b, &a);
        assert_eq!(c.len(), 1);
        assert_eq!(c[0].change_type, change_type::PREFAB_INSTANCE_CHANGED);
    }

    #[test]
    fn prefab_file_change_adds_asset_changed() {
        let doc = |name: &str| format!("{{\"engine\":\"MyEngine\",\"prefab\":1,\"name\":\"{name}\",\"entities\":[{}]}}", ent(1, "\"A\":{\"x\":1}"));
        let c = changes(DocKind::Prefab, &doc("p"), &doc("p"));
        assert!(c.is_empty());
        let c = changes(DocKind::Prefab, &doc("p"), &doc("q"));
        let types: Vec<&str> = c.iter().map(|x| x.change_type).collect();
        assert_eq!(types, vec![change_type::PREFAB_ASSET_CHANGED, change_type::FIELD_MODIFIED]);
        assert_eq!(c[1].file_id, None);
        assert_eq!(c[1].field.as_deref(), Some("name"));
    }

    #[test]
    fn unparsable_documents_give_reasons() {
        let ok = scene("");
        assert!(unparsed(&ok, "").contains("空のファイル"));
        assert!(unparsed(&ok, "{").contains("JSON を読めない"));
        assert!(unparsed(&ok, "[]").contains("オブジェクトでない"));
        assert!(unparsed(&ok, "{\"version\":4}").contains("entities"));
        assert!(unparsed(&ok, "{\"version\":4,\"entities\":{}}").contains("entities"));
        assert!(unparsed(&ok, &ok.replace("\"version\":4", "\"version\":3")).contains("版が違う"));
        let v9 = ok.replace("\"version\":4", "\"version\":9");
        assert!(unparsed(&v9, &v9).contains("知らない版"));
        let dup = scene(&format!("{},{}", ent(1, ""), ent(1, "")));
        assert!(unparsed(&ok, &dup).contains("重複"));
        let nofid = scene("{\"name\":\"x\",\"components\":{}}");
        assert!(unparsed(&ok, &nofid).contains("fileId"));
        let fracid = scene("{\"fileId\":1.5,\"components\":{}}");
        assert!(unparsed(&ok, &fracid).contains("fileId"));
        // 上限
        let big = diff(DocKind::Scene, ok.as_bytes(), ok.as_bytes(), 10);
        assert!(matches!(big, SceneOutcome::Unparsed(r) if r.contains("大きすぎる")));
        // UTF-8 でない
        let bad = diff(DocKind::Scene, ok.as_bytes(), &[0xff, 0xfe], LIMIT);
        assert!(matches!(bad, SceneOutcome::Unparsed(r) if r.contains("UTF-8")));
    }

    #[test]
    fn root_changes_skip_next_file_id() {
        let b = scene("");
        let a = b.replace("\"sceneName\":\"s\"", "\"sceneName\":\"t\"").replace("\"nextFileId\":9", "\"nextFileId\":12");
        let c = changes(DocKind::Scene, &b, &a);
        assert_eq!(c.len(), 1);
        assert_eq!((c[0].file_id, c[0].change_type, c[0].field.as_deref()), (None, change_type::FIELD_MODIFIED, Some("sceneName")));
        assert_eq!(c[0].after_value.as_deref(), Some("\"t\""));
    }
}
