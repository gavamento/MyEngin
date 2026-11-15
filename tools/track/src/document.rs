//! 文書の同一性 (architecture.md §6.1、db-schema.md §3 の `documents`)。
//!
//! `documentId` はアセットの `.meta` の `guid`。`.meta` が無い・読めないときは正規化したパスの SHA-256 の先頭 16 桁を
//! 仮の ID にする (`id_source = path`)。サービスが `.meta` を自分で読む (Editor を経由しない)。
//! 観測の時点の guid は `observed` イベントの payload (`doc_guid`) に入れ、DB を作り直しても同じ `document_id` になる。
//!
//! 同一性の規則 (`bind`):
//! - 同じ guid = 同じ文書。`.meta` ごと動いたリネーム・移動は `current_path` だけ更新する。
//! - 同じパスで仮の ID から guid へ変わった (後で `.meta` が付いた) ときは、仮の文書の `successor_id` に guid の文書を書いて引き継ぐ。
//!   `scene_changes` / `changes` の `document_id` は書き換えない (履歴の行は元のまま。`resolve` でたどる)。
//! - guid が変わった・`.meta` が無くなった・パスだけが変わった (guid が無い) ときは、別の文書として扱い、
//!   古い文書に `broken_at` / `broken_note` を記録する。内容が似ていても同じ文書とは決めない。

use std::path::Path;

use rusqlite::{params, Connection, OptionalExtension};

use crate::hash::sha256_hex;
use crate::scene_diff::DocKind;

/// guid の桁数 (16 進)。
const GUID_LEN: usize = 16;
/// `resolve` が引き継ぎの鎖をたどる回数の上限 (循環の保険)。
const MAX_SUCCESSOR_HOPS: usize = 16;

/// `.meta` のパスから、対象のアセットのパスと種類を返す。対象が scene / prefab / actor でなければ None。
pub fn meta_target(path: &str) -> Option<(&str, DocKind)> {
    let base = path.strip_suffix(".meta").or_else(|| path.strip_suffix(".META"))?;
    DocKind::from_path(base).map(|k| (base, k))
}

/// `changes.kind` に入れる種類。拡張子で決まる (scene / prefab / actor / meta)。それ以外は None (内容の判定に従う)。
pub fn path_kind(path: &str) -> Option<&'static str> {
    if let Some(kind) = DocKind::from_path(path) {
        return Some(kind.as_str());
    }
    path.to_ascii_lowercase().ends_with(".meta").then_some("meta")
}

/// `.meta` の中身から guid を読む。16 桁の 16 進でなければ None。小文字にそろえる。
pub fn parse_guid(bytes: &[u8]) -> Option<String> {
    let value: serde_json::Value = serde_json::from_slice(bytes).ok()?;
    let guid = value.get("guid")?.as_str()?;
    (guid.len() == GUID_LEN && guid.bytes().all(|b| b.is_ascii_hexdigit())).then(|| guid.to_ascii_lowercase())
}

/// 作業ツリーの `<root>/<path>.meta` を読んで guid を返す。無い・読めない・不正は None。
pub fn read_meta_guid(root: &Path, rel_path: &str) -> Option<String> {
    let bytes = std::fs::read(root.join(format!("{rel_path}.meta"))).ok()?;
    parse_guid(&bytes)
}

/// 仮の ID。パスを `/` 区切りにそろえた SHA-256 の先頭 16 桁。
pub fn path_id(path: &str) -> String {
    sha256_hex(path.replace('\\', "/").as_bytes())[..GUID_LEN].to_string()
}

struct Existing {
    id: String,
    id_source: String,
}

fn find_by_path(conn: &Connection, path: &str) -> rusqlite::Result<Option<Existing>> {
    conn.query_row(
        "SELECT document_id, id_source FROM documents WHERE current_path = ?1 ORDER BY rowid DESC LIMIT 1",
        [path],
        |r| Ok(Existing { id: r.get(0)?, id_source: r.get(1)? }),
    )
    .optional()
}

/// 文書を結び付ける。`create` が false のときは、そのパスに文書が既にあるときだけ動く (`.meta` の変更の経路)。
/// 戻り値は結び付けた `document_id` (`create` が false で文書が無ければ None)。
fn bind(
    conn: &Connection,
    path: &str,
    old_path: Option<&str>,
    kind: DocKind,
    guid: Option<&str>,
    at: i64,
    create: bool,
) -> rusqlite::Result<Option<String>> {
    let prior = find_by_path(conn, old_path.unwrap_or(path))?;
    if prior.is_none() && !create {
        return Ok(None);
    }
    let (new_id, id_source) = match guid {
        Some(g) => (g.to_string(), "meta"),
        None => (path_id(path), "path"),
    };
    let known: Option<Option<String>> = conn
        .query_row("SELECT current_path FROM documents WHERE document_id = ?1", [&new_id], |r| r.get(0))
        .optional()?;
    if known.is_some() {
        conn.execute(
            "UPDATE documents SET current_path = ?2, kind = ?3 WHERE document_id = ?1",
            params![new_id, path, kind.as_str()],
        )?;
    } else {
        conn.execute(
            "INSERT INTO documents (document_id, id_source, kind, current_path) VALUES (?1, ?2, ?3, ?4)",
            params![new_id, id_source, kind.as_str(), path],
        )?;
    }
    if let Some(prior) = prior.filter(|p| p.id != new_id) {
        let inherits = prior.id_source == "path" && id_source == "meta" && old_path.is_none_or(|o| o == path);
        if inherits {
            conn.execute(
                "UPDATE documents SET successor_id = ?2, current_path = NULL WHERE document_id = ?1",
                params![prior.id, new_id],
            )?;
        } else {
            let note = match (prior.id_source.as_str(), guid) {
                ("meta", Some(g)) => format!("guid が変わった: {} -> {g}", prior.id),
                ("meta", None) => format!(".meta が無くなった (guid {})", prior.id),
                _ => "パスが変わった (.meta が無いので同一性を引き継げない)".to_string(),
            };
            conn.execute(
                "UPDATE documents SET broken_at = ?2, broken_note = ?3, current_path = NULL WHERE document_id = ?1",
                params![prior.id, at, note],
            )?;
        }
    }
    Ok(Some(new_id))
}

/// scene / prefab / actor のファイルが存在する観測 (追加・変更・リネーム) の文書を決める。
pub fn assign(
    conn: &Connection,
    path: &str,
    old_path: Option<&str>,
    kind: DocKind,
    guid: Option<&str>,
    at: i64,
) -> rusqlite::Result<String> {
    Ok(bind(conn, path, old_path, kind, guid, at, true)?.expect("create = true は必ず文書を返す"))
}

/// 削除の観測。文書の `current_path` を空にして、その文書の ID を返す (無ければ None)。
pub fn mark_deleted(conn: &Connection, path: &str) -> rusqlite::Result<Option<String>> {
    let Some(found) = find_by_path(conn, path)? else { return Ok(None) };
    conn.execute("UPDATE documents SET current_path = NULL WHERE document_id = ?1", [&found.id])?;
    Ok(Some(found.id))
}

/// `.meta` の変更の観測。対象のアセットの文書が既にあるときだけ、新しい guid で結び付け直す (引き金)。
pub fn rebind_for_meta(conn: &Connection, asset_path: &str, kind: DocKind, guid: Option<&str>, at: i64) -> rusqlite::Result<()> {
    bind(conn, asset_path, None, kind, guid, at, false)?;
    Ok(())
}

/// 引き継ぎの鎖をたどった先の `document_id` (引き継がれていなければ自分)。
pub fn resolve(conn: &Connection, document_id: &str) -> rusqlite::Result<String> {
    let mut current = document_id.to_string();
    for _ in 0..MAX_SUCCESSOR_HOPS {
        let next: Option<Option<String>> = conn
            .query_row("SELECT successor_id FROM documents WHERE document_id = ?1", [&current], |r| r.get(0))
            .optional()?;
        match next.flatten() {
            Some(id) => current = id,
            None => break,
        }
    }
    Ok(current)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn guid_parsing() {
        assert_eq!(parse_guid(br#"{"guid":"7D4D8953B427201B","type":"scene"}"#).as_deref(), Some("7d4d8953b427201b"));
        assert_eq!(parse_guid(br#"{"guid":"7d4d8953b427201"}"#), None);
        assert_eq!(parse_guid(br#"{"guid":"zzzzzzzzzzzzzzzz"}"#), None);
        assert_eq!(parse_guid(b"not json"), None);
        assert_eq!(parse_guid(br#"{"guid":1}"#), None);
    }

    #[test]
    fn path_ids_are_16_hex_and_separator_insensitive() {
        let id = path_id("assets/scenes/a.scene.json");
        assert_eq!(id.len(), 16);
        assert_eq!(id, path_id("assets\\scenes\\a.scene.json"));
        assert_ne!(id, path_id("assets/scenes/b.scene.json"));
    }

    #[test]
    fn kinds_by_path() {
        assert_eq!(path_kind("a/B.Scene.JSON"), Some("scene"));
        assert_eq!(path_kind("a.prefab.json"), Some("prefab"));
        assert_eq!(path_kind("a.actor.json"), Some("actor"));
        assert_eq!(path_kind("a.scene.json.meta"), Some("meta"));
        assert_eq!(path_kind("a.json"), None);
        assert_eq!(meta_target("x/a.scene.json.meta"), Some(("x/a.scene.json", DocKind::Scene)));
        assert_eq!(meta_target("x/a.png.meta"), None);
    }
}
