//! プレハブ伝播 (`PrefabPropagation`) の影響範囲の計算 (architecture.md §6.4)。
//!
//! プレハブ / アクターのファイルでフィールドが変わったとき、そのプレハブを直に参照しているシーンのインスタンスのうち、
//! その項目を override していないものを返す。伝播はロード時とホットリロード時にメモリ上で起きるので、シーンのファイルには
//! 現れない。ここで出すのは「影響を受けるはずの範囲」で、インスタンスのライブの値は分からない (Before / After はプレハブの値)。
//!
//! 入力は DB の `snapshots` (各シーンの最新の記録) と Blob だけで、ディスクも時計も読まない。
//! 通常の記録も `rebuild_derived` も同じ関数を通るので、作り直した DB は元と同じ行になる (spec D26)。
//! 入れ子のプレハブは、シーンに書かれている内側のルート (`prefabHash` が一致するもの) までを対象にし、
//! 外側のプレハブの変更が内側へ及ぶ連鎖は追わない。

use std::collections::{HashMap, HashSet};
use std::path::Path;

use rusqlite::{Connection, OptionalExtension};
use serde_json::json;

use crate::blob::BlobStore;
use crate::document;
use crate::lexjson::{self, Kind, Node};
use crate::scene_diff::{change_type, DocKind};

/// 1 回の伝播の計算で内容を読むシーンの数の上限。
pub const MAX_SCENES_READ: usize = 300;
/// 1 つのシーンの大きさの上限 (バイト)。
pub const MAX_SCENE_BYTES: usize = 16 * 1024 * 1024;
/// 1 回の計算で読むシーンの合計の大きさの上限 (バイト)。
pub const MAX_TOTAL_BYTES: usize = 256 * 1024 * 1024;
/// 1 回の計算で出す行の数の上限。
pub const MAX_ROWS: usize = 20_000;

/// 計算に使う入れ物 (DB・Blob・プロジェクトのルート)。
pub struct Env<'a> {
    pub conn: &'a Connection,
    pub blobs: &'a BlobStore,
    /// パスのハッシュ (`.meta` の無いプレハブのキー) を作るのに使う。無ければ `.meta` の guid だけで引く。
    pub root: Option<&'a Path>,
}

/// 原因になったプレハブ側の変更 (`scene_changes` の 1 行)。
#[derive(Clone, Debug)]
pub struct Cause {
    pub scene_change_id: i64,
    /// プレハブの中のエンティティの `fileId` (= インスタンスの `PrefabLink.localId`)。
    pub local_id: i64,
    pub component: Option<String>,
    pub field: String,
    pub before: Option<String>,
    pub after: Option<String>,
}

/// 保存する伝播の行。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PropagationRow {
    pub document_id: String,
    pub file_id: i64,
    pub component: Option<String>,
    pub field: String,
    pub before: Option<String>,
    pub after: Option<String>,
    pub caused_by: i64,
}

/// 計算の結果と、出し切れなかった分の数。
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Outcome {
    pub rows: Vec<PropagationRow>,
    pub scenes_checked: usize,
    /// 内容を読めなかった (Blob が無い・UTF-8 でない・JSON でない) シーン。
    pub scenes_unreadable: usize,
    /// 数・大きさの上限で読まなかったシーン。
    pub scenes_skipped: usize,
    /// 上限で出さなかった行。
    pub rows_omitted: usize,
    /// `overrides` の記録の無い (旧形式の) メンバ。override かどうか分からないので行を出していない。
    pub legacy_members: usize,
}

impl Outcome {
    /// 読み飛ばし・打ち切りがあったか。あれば要約の行を 1 つ残す。
    pub fn has_limits(&self) -> bool {
        self.scenes_unreadable + self.scenes_skipped + self.rows_omitted + self.legacy_members > 0
    }

    /// 要約の行の本文 (JSON)。
    pub fn summary_json(&self) -> String {
        json!({
            "scenesChecked": self.scenes_checked,
            "scenesUnreadable": self.scenes_unreadable,
            "scenesSkipped": self.scenes_skipped,
            "rowsOmitted": self.rows_omitted,
            "legacyMembers": self.legacy_members,
        })
        .to_string()
    }
}

const FNV_OFFSET: u64 = 14_695_981_039_346_656_037;
const FNV_PRIME: u64 = 1_099_511_628_211;

fn fnv1a(bytes: &[u8]) -> u64 {
    bytes.iter().fold(FNV_OFFSET, |h, b| (h ^ u64::from(*b)).wrapping_mul(FNV_PRIME))
}

/// エンジンの `PrefabLibrary::HashForPath` と同じキー。`.meta` の guid (16 桁の 16 進) があればその値、無ければ
/// 正規化した絶対パス (小文字・`\` 区切り) の FNV-1a 64。
pub fn prefab_hash(rel_path: &str, guid: Option<&str>, root: Option<&Path>) -> Option<u64> {
    if let Some(guid) = guid {
        if let Ok(value) = u64::from_str_radix(guid, 16) {
            return Some(value);
        }
    }
    let root = root?.to_string_lossy().into_owned();
    let root = root.strip_prefix("\\\\?\\").unwrap_or(&root).trim_end_matches(['\\', '/']);
    let key = format!("{root}\\{rel_path}").replace('/', "\\").to_lowercase();
    Some(fnv1a(key.as_bytes()))
}

/// そのパスの最新の記録 (`snapshots`) の Blob のハッシュ。除外・Blob 無しは None。
fn latest_blob(conn: &Connection, path: &str) -> rusqlite::Result<Option<String>> {
    conn.query_row(
        "SELECT blob_hash FROM snapshots WHERE path = ?1 AND blob_state = 'present' AND blob_hash IS NOT NULL
          ORDER BY snapshot_id DESC LIMIT 1",
        [path],
        |r| r.get(0),
    )
    .optional()
}

/// 最新の記録がシーンの内容か (削除・リネームで去ったシーンの古い記録を使わない)。
fn is_gone(conn: &Connection, path: &str) -> rusqlite::Result<bool> {
    let latest: Option<(String, String, Option<String>)> = conn
        .query_row(
            "SELECT r.change_type, c.path, r.old_path FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
              WHERE c.path = ?1 OR r.old_path = ?1 ORDER BY r.revision_id DESC LIMIT 1",
            [path],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .optional()?;
    Ok(match latest {
        Some((kind, at, old)) => (kind == "deleted" && at == path) || old.as_deref() == Some(path),
        None => false,
    })
}

/// シーンの文書の ID。記録のある文書があればそれ。無ければ `.meta` の最新の記録の guid (無ければパス) で文書を作る
/// (`document_for_path` と同じ規則。ただしディスクではなく記録を読むので、作り直しても同じ ID になる)。
/// `scene_changes.document_id` が文書を参照するので、一度も変更の記録が無いシーンもここで文書にする。
fn scene_document_id(env: &Env, path: &str, at: i64) -> rusqlite::Result<String> {
    let existing: Option<String> = env
        .conn
        .query_row("SELECT document_id FROM documents WHERE current_path = ?1 ORDER BY rowid DESC LIMIT 1", [path], |r| r.get(0))
        .optional()?;
    if let Some(id) = existing {
        return Ok(id);
    }
    let mut guid: Option<String> = None;
    if let Some(hash) = latest_blob(env.conn, &format!("{path}.meta"))? {
        if let Ok(bytes) = env.blobs.read(&hash) {
            guid = document::parse_guid(&bytes);
        }
    }
    document::assign(env.conn, path, None, DocKind::Scene, guid.as_deref(), at)
}

/// 候補のシーン (最新の記録があるもの)。パスの昇順 (順序を決定的にする)。
fn candidate_scenes(conn: &Connection) -> rusqlite::Result<Vec<(String, String)>> {
    let mut stmt = conn.prepare(
        "SELECT s.path, s.blob_hash FROM snapshots s
          WHERE s.path LIKE '%.scene.json' AND s.blob_state = 'present' AND s.blob_hash IS NOT NULL
            AND s.snapshot_id = (SELECT MAX(t.snapshot_id) FROM snapshots t WHERE t.path = s.path)
          ORDER BY s.path",
    )?;
    let rows = stmt.query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))?;
    rows.collect()
}

fn contains_bytes(haystack: &[u8], needle: &[u8]) -> bool {
    !needle.is_empty() && haystack.windows(needle.len()).any(|w| w == needle)
}

/// 変更したプレハブの `causes` が、シーンのインスタンスのどのメンバへ伝わるか。
pub fn propagate(env: &Env, prefab_path: &str, guid: Option<&str>, causes: &[Cause], at: i64) -> rusqlite::Result<Outcome> {
    let mut outcome = Outcome::default();
    let Some(hash) = prefab_hash(prefab_path, guid, env.root) else { return Ok(outcome) };
    if causes.is_empty() {
        return Ok(outcome);
    }
    let needle = hash.to_string();
    let mut total_bytes = 0usize;
    let mut read_scenes = 0usize;
    for (path, blob) in candidate_scenes(env.conn)? {
        if read_scenes >= MAX_SCENES_READ || total_bytes >= MAX_TOTAL_BYTES {
            outcome.scenes_skipped += 1;
            continue;
        }
        read_scenes += 1;
        let Ok(bytes) = env.blobs.read(&blob) else {
            outcome.scenes_unreadable += 1;
            continue;
        };
        total_bytes += bytes.len();
        // 先に文字列で絞る (参照していないシーンを JSON として読まない)
        if !contains_bytes(&bytes, needle.as_bytes()) {
            continue;
        }
        if bytes.len() > MAX_SCENE_BYTES {
            outcome.scenes_skipped += 1;
            continue;
        }
        if is_gone(env.conn, &path)? {
            continue;
        }
        let Ok(src) = std::str::from_utf8(&bytes) else {
            outcome.scenes_unreadable += 1;
            continue;
        };
        let Ok(root_node) = lexjson::parse(src) else {
            outcome.scenes_unreadable += 1;
            continue;
        };
        let document_id = scene_document_id(env, &path, at)?;
        outcome.scenes_checked += 1;
        scan_scene(&root_node, src, hash, causes, &document_id, &mut outcome);
    }
    Ok(outcome)
}

struct Ent<'a> {
    file_id: i64,
    parent: i64,
    node: &'a Node,
}

fn int_of(node: Option<&Node>, src: &str) -> Option<i64> {
    let node = node?;
    matches!(node.kind, Kind::Number).then(|| node.fragment(src).parse::<i64>().ok()).flatten()
}

fn uint_of(node: Option<&Node>, src: &str) -> Option<u64> {
    let node = node?;
    matches!(node.kind, Kind::Number).then(|| node.fragment(src).parse::<u64>().ok()).flatten()
}

fn component<'a>(entity: &'a Node, name: &str) -> Option<&'a Node> {
    entity.get("components")?.get(name)
}

/// override の集合。`overrides` が無い (旧形式) とき None。
fn override_set(entity: &Node) -> Option<HashSet<&str>> {
    match &entity.get("overrides")?.kind {
        Kind::Array(items) => Some(
            items
                .iter()
                .filter_map(|n| match &n.kind {
                    Kind::Str(s) => Some(s.as_str()),
                    _ => None,
                })
                .collect(),
        ),
        _ => None,
    }
}

/// 1 つのシーンの中の、`hash` のインスタンスのメンバへ `causes` を当てる。
fn scan_scene(root: &Node, src: &str, hash: u64, causes: &[Cause], document_id: &str, out: &mut Outcome) {
    let Some(Node { kind: Kind::Array(items), .. }) = root.get("entities") else { return };
    let mut ents: Vec<Ent> = Vec::with_capacity(items.len());
    for node in items {
        let Some(file_id) = int_of(node.get("fileId"), src) else { continue };
        ents.push(Ent { file_id, parent: int_of(node.get("parent"), src).unwrap_or(0), node });
    }
    let mut children: HashMap<i64, Vec<usize>> = HashMap::new();
    let mut is_instance_root: Vec<bool> = Vec::with_capacity(ents.len());
    let mut targets: Vec<usize> = Vec::new();
    for (i, ent) in ents.iter().enumerate() {
        children.entry(ent.parent).or_default().push(i);
        let instance = component(ent.node, "PrefabInstance");
        is_instance_root.push(instance.is_some());
        if uint_of(instance.and_then(|c| c.get("prefabHash")), src) == Some(hash) {
            targets.push(i);
        }
    }
    for root_index in targets {
        // ルートと、別のインスタンスの境界を越えない子孫 (文書の並びの順)
        let mut members: Vec<usize> = vec![root_index];
        let mut stack: Vec<usize> = vec![root_index];
        while let Some(at) = stack.pop() {
            for &child in children.get(&ents[at].file_id).into_iter().flatten().rev() {
                if !is_instance_root[child] {
                    members.push(child);
                    stack.push(child);
                }
            }
        }
        let mut by_local: HashMap<i64, usize> = HashMap::new();
        for &m in &members {
            if let Some(local) = int_of(component(ents[m].node, "PrefabLink").and_then(|c| c.get("localId")), src) {
                if local != 0 {
                    by_local.entry(local).or_insert(m);
                }
            }
        }
        for cause in causes {
            let Some(&m) = by_local.get(&cause.local_id) else { continue };
            let Some(overridden) = override_set(ents[m].node) else {
                out.legacy_members += 1;
                continue;
            };
            let key = match &cause.component {
                Some(c) => format!("{c}.{}", cause.field),
                None => cause.field.clone(),
            };
            if overridden.contains(key.as_str()) {
                continue;
            }
            if out.rows.len() >= MAX_ROWS {
                out.rows_omitted += 1;
                continue;
            }
            out.rows.push(PropagationRow {
                document_id: document_id.to_string(),
                file_id: ents[m].file_id,
                component: cause.component.clone(),
                field: cause.field.clone(),
                before: cause.before.clone(),
                after: cause.after.clone(),
                caused_by: cause.scene_change_id,
            });
        }
    }
}

/// 伝播の原因になりうるプレハブ側の行か (フィールドの値の変化)。伝播の行として出す形に直す。
pub fn cause_of(
    scene_change_id: i64,
    file_id: Option<i64>,
    kind: &str,
    component: &Option<String>,
    field: &Option<String>,
    before: &Option<String>,
    after: &Option<String>,
) -> Option<Cause> {
    let local_id = file_id?;
    let field = field.clone()?;
    let ok = match kind {
        change_type::FIELD_MODIFIED => component.is_some(),
        change_type::ENTITY_MODIFIED => component.is_none() && matches!(field.as_str(), "name" | "tagMask"),
        _ => false,
    };
    ok.then(|| Cause { scene_change_id, local_id, component: component.clone(), field, before: before.clone(), after: after.clone() })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fnv_matches_the_engine_hash() {
        // `HashStr` (Hash.h) の FNV-1a 64。空文字列は offset basis、"a" は既知の値
        assert_eq!(fnv1a(b""), 0xcbf2_9ce4_8422_2325);
        assert_eq!(fnv1a(b"a"), 0xaf63_dc4c_8601_ec8c);
    }

    #[test]
    fn guid_wins_and_path_hash_is_lowercase_backslash() {
        assert_eq!(prefab_hash("a/b.prefab.json", Some("00000000000000ff"), None), Some(255));
        let root = Path::new("C:\\Proj");
        let a = prefab_hash("Assets/Hero.prefab.json", None, Some(root));
        let b = prefab_hash("assets\\hero.prefab.json", None, Some(Path::new("c:\\proj\\")));
        assert_eq!(a, b);
        assert_eq!(a, Some(fnv1a(b"c:\\proj\\assets\\hero.prefab.json")));
        assert_eq!(prefab_hash("a.prefab.json", None, None), None);
    }
}
