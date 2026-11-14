//! projectId の固定表 (tests\project_id_cases.tsv) を通す。C++ の NormalizePathKey の実測が元。

use std::path::Path;

use mye_track::project::{normalize_path_key, project_id, project_id_source};

const CASES: &str = include_str!("project_id_cases.tsv");

fn rows() -> Vec<(&'static str, &'static str, &'static str, &'static str)> {
    CASES
        .lines()
        .filter(|l| !l.starts_with('#') && !l.trim().is_empty())
        .map(|l| {
            let cols: Vec<&str> = l.split('\t').collect();
            assert_eq!(cols.len(), 4, "表の行は 4 列: {l}");
            (cols[0], cols[1], cols[2], cols[3])
        })
        .collect()
}

#[test]
fn fixed_table_matches_cpp_normalization_and_ids() {
    let rows = rows();
    assert!(rows.len() >= 10, "表が空に近い");
    for (input, key, source, id) in rows {
        assert_eq!(normalize_path_key(Path::new(input)), key, "正規化: {input}");
        assert_eq!(project_id_source(Path::new(input)), source, "もとの文字列: {input}");
        assert_eq!(project_id(Path::new(input)), id, "projectId: {input}");
    }
}

#[test]
fn id_is_16_lowercase_hex_digits() {
    let id = project_id(Path::new(r"C:\Proj\MyGame"));
    assert_eq!(id.len(), 16);
    assert!(id.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)));
}

#[test]
fn trailing_separators_never_change_the_id_but_drive_root_is_kept() {
    let base = project_id(Path::new(r"C:\Proj\MyGame"));
    for spelled in [r"C:\Proj\MyGame\", "C:/Proj/MyGame/", r"C:\Proj\MyGame\\", r"c:\PROJ\mygame\/"] {
        assert_eq!(project_id(Path::new(spelled)), base, "{spelled}");
    }
    assert_eq!(project_id_source(Path::new(r"C:\")), r"c:\");
    assert_ne!(project_id(Path::new(r"C:\")), project_id(Path::new(r"C:\Proj")));
}
