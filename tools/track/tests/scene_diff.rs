//! シーン構造 diff と文書の同一性の結合テスト (M104l)。
//!
//! 固定の入力は `tests\track\fixture\` (実シーン・実プレハブから切り出したコピー)。
//! 純粋な比較 (`scene_diff::diff`) と、Tracker を通した記録 (DB の `documents` / `scene_changes`) の 2 層で確かめる。
//! 待ちは固定の sleep ではなく上限付きの待ち合わせ。全テストの後処理で、テスト自身が書いた分を除いて
//! 作業ツリー (`.mye` を除く) が変わっていないことを検査する (spec §2 D17)。

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use rusqlite::types::ValueRef;
use rusqlite::Connection;

use mye_track::collector::TrackerConfig;
use mye_track::db::now_ms;
use mye_track::hash::sha256_hex;
use mye_track::scene_diff::{change_type, diff, DocKind, SceneChange, SceneOutcome};
use mye_track::startup::{open_project, ServiceState};
use mye_track::store::Paths;
use mye_track::watch::{Phase, Tracker};

const WAIT_LIMIT: Duration = Duration::from_secs(60);
const LIMIT: usize = 8 * 1024 * 1024;
const GUID_A: &str = "aaaaaaaaaaaaaaaa";
const GUID_B: &str = "bbbbbbbbbbbbbbbb";
const GUID_C: &str = "cccccccccccccccc";

fn fixture(name: &str) -> String {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("..").join("tests").join("track").join("fixture").join(name);
    fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn meta(guid: &str) -> String {
    format!("{{\n  \"guid\": \"{guid}\",\n  \"type\": \"scene\",\n  \"version\": 1\n}}")
}

fn changes(kind: DocKind, before: &str, after: &str) -> Vec<SceneChange> {
    match diff(kind, before.as_bytes(), after.as_bytes(), LIMIT) {
        SceneOutcome::Changes(c) => c,
        SceneOutcome::Unparsed(reason) => panic!("解析不可: {reason}"),
    }
}

// ---- 純粋な比較 (実ファイルの切り出し) ----

#[test]
fn a_one_field_edit_in_a_real_scene_is_one_field_modified_with_exact_lexemes() {
    let before = fixture("scene_small.scene.json");
    let after = before.replacen("\"nearZ\": 0.10000000149011612", "\"nearZ\": 0.2", 1);
    let c = changes(DocKind::Scene, &before, &after);
    assert_eq!(c.len(), 1, "{c:?}");
    assert_eq!(c[0].change_type, change_type::FIELD_MODIFIED);
    assert_eq!((c[0].component.as_deref(), c[0].field.as_deref()), (Some("Camera"), Some("nearZ")));
    assert!(c[0].file_id.is_some());
    assert_eq!(c[0].before_value.as_deref(), Some("0.10000000149011612"));
    assert_eq!(c[0].after_value.as_deref(), Some("0.2"));
}

#[test]
fn reordering_and_respacing_a_real_scene_changes_nothing_but_a_number_spelling_does() {
    let base = fixture("scene_small.scene.json");
    let reordered = fixture("scene_small_reordered.scene.json");
    assert_ne!(base, reordered);
    assert_eq!(changes(DocKind::Scene, &base, &reordered), Vec::new(), "キー・エンティティの並びと空白だけの違い");
    // 数値の書き方だけが違う (0.1 と 0.10000000149011612) は別の値
    let short = fixture("scene_small_nearZ_short.scene.json");
    let c = changes(DocKind::Scene, &base, &short);
    assert_eq!(c.len(), 1, "{c:?}");
    assert_eq!(c[0].after_value.as_deref(), Some("0.1"));
}

#[test]
fn fragments_are_slices_of_the_source_that_read_back_as_json() {
    let before = fixture("scene_small.scene.json");
    // 入れ子の中だけを変える (position の 2 つ目の要素)
    let after = before.replacen("6.0,\n            -14.0", "7.5,\n            -14.0", 1);
    assert_ne!(before, after, "固定ファイルの形が想定と違う");
    let c = changes(DocKind::Scene, &before, &after);
    assert_eq!(c.len(), 1, "{c:?}");
    assert_eq!(c[0].field.as_deref(), Some("position"));
    let (b, a) = (c[0].before_value.as_deref().unwrap(), c[0].after_value.as_deref().unwrap());
    assert!(before.contains(b) && after.contains(a), "元のバイト列の断片であること");
    let (vb, va): (serde_json::Value, serde_json::Value) = (serde_json::from_str(b).unwrap(), serde_json::from_str(a).unwrap());
    assert_eq!(vb[1], 6.0);
    assert_eq!(va[1], 7.5);
}

#[test]
fn actor_and_prefab_files_report_the_asset_change_plus_inner_changes() {
    let actor = fixture("part_sample.actor.json");
    let edited = actor.replacen("\"name\": \"弱点\"", "\"name\": \"急所\"", 1);
    assert_ne!(actor, edited);
    let c = changes(DocKind::Actor, &actor, &edited);
    let types: Vec<&str> = c.iter().map(|x| x.change_type).collect();
    assert_eq!(types, vec![change_type::PREFAB_ASSET_CHANGED, change_type::ENTITY_MODIFIED], "{c:?}");

    let prefab = fixture("collision.prefab.json");
    assert_eq!(changes(DocKind::Prefab, &prefab, &prefab), Vec::new());
    // 版の宣言が prefab から actor に変わった文書は比べない
    let swapped = prefab.replacen("\"prefab\": 1", "\"actor\": 1", 1);
    assert!(matches!(diff(DocKind::Prefab, prefab.as_bytes(), swapped.as_bytes(), LIMIT), SceneOutcome::Unparsed(_)));
}

#[test]
fn removing_and_adding_entities_in_a_real_scene() {
    let base = fixture("scene_small.scene.json");
    // 2 つ目のエンティティ (Sun、fileId 15) を本文から切り取る
    let start = base.find("    {\n      \"childIndex\": 1,").unwrap();
    let tail = "\"name\": \"Sun\"\n    },\n";
    let end = base.find(tail).unwrap() + tail.len();
    let fewer_text = format!("{}{}", &base[..start], &base[end..]);
    let c = changes(DocKind::Scene, &base, &fewer_text);
    assert_eq!(c.len(), 1, "{c:?}");
    assert_eq!((c[0].change_type, c[0].file_id), (change_type::ENTITY_REMOVED, Some(15)));
    let c = changes(DocKind::Scene, &fewer_text, &base);
    assert_eq!((c.len(), c[0].change_type), (1, change_type::ENTITY_ADDED));
}

// ---- Tracker を通した記録 ----

fn cfg() -> TrackerConfig {
    TrackerConfig {
        debounce: Duration::from_millis(150),
        max_hold: Duration::from_secs(1),
        busy_retry: Duration::from_millis(50),
        settle_wait: Duration::from_millis(100),
        settle_give_up: Duration::from_secs(10),
        verify_quiet: Duration::from_millis(250),
        verify_interval: Duration::from_millis(500),
        unrecorded_retry: Duration::from_millis(100),
        ..TrackerConfig::default()
    }
}

fn wait_until(what: &str, mut cond: impl FnMut() -> bool) {
    let deadline = Instant::now() + WAIT_LIMIT;
    loop {
        if cond() {
            return;
        }
        assert!(Instant::now() < deadline, "待ちの上限を超えた: {what}");
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// テストごとの作業ツリー。成功したテストは終わりに消す (失敗したときは調べられるよう残す)。
struct Fixture {
    root: PathBuf,
    model: BTreeMap<String, Vec<u8>>,
}

impl Fixture {
    fn new(name: &str) -> Fixture {
        let root = std::env::temp_dir().join(format!("mye_track_sd_{}_{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        Fixture { root, model: BTreeMap::new() }
    }

    fn write(&mut self, rel: &str, text: &str) {
        let p = self.root.join(rel);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(&p, text.as_bytes()).unwrap();
        self.model.insert(rel.to_string(), text.as_bytes().to_vec());
    }

    fn remove(&mut self, rel: &str) {
        fs::remove_file(self.root.join(rel)).unwrap();
        self.model.remove(rel);
    }

    fn rename(&mut self, from: &str, to: &str) {
        let p = self.root.join(to);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::rename(self.root.join(from), p).unwrap();
        let bytes = self.model.remove(from).unwrap();
        self.model.insert(to.to_string(), bytes);
    }

    fn start(&self) -> Tracker {
        let (store, report) = open_project(&self.root, now_ms());
        assert_eq!(report.state, Some(ServiceState::Recording));
        let tracker = Tracker::start(&self.root, store.unwrap(), cfg());
        let status = tracker.wait_ready(WAIT_LIMIT);
        assert_eq!(status.phase, Phase::Watching, "{status:?}");
        tracker
    }

    fn db(&self) -> Connection {
        let conn = Connection::open(Paths::for_project(&self.root).db).unwrap();
        conn.busy_timeout(Duration::from_secs(10)).unwrap();
        conn
    }

    fn count(&self, sql: &str) -> i64 {
        self.db().query_row(sql, [], |r| r.get(0)).unwrap()
    }

    fn revisions(&self, path: &str) -> i64 {
        self.db()
            .query_row(
                "SELECT COUNT(*) FROM change_revisions r JOIN changes c ON c.change_id = r.change_id WHERE c.path = ?1",
                [path],
                |r| r.get(0),
            )
            .unwrap()
    }

    fn wait_revisions(&self, path: &str, n: i64) {
        wait_until(&format!("{path} の Revision {n} 件"), || self.revisions(path) >= n);
    }

    /// パスの `seq` 番目 (1 始まり) の Revision の構造の変更。
    fn scene_rows(&self, path: &str, seq: i64) -> Vec<SceneRow> {
        let conn = self.db();
        let mut stmt = conn
            .prepare(
                "SELECT s.document_id, s.file_id, s.change_type, s.component, s.field, s.before_value, s.after_value
                   FROM scene_changes s JOIN change_revisions r ON r.revision_id = s.revision_id
                   JOIN changes c ON c.change_id = r.change_id
                  WHERE c.path = ?1 AND r.seq = ?2 ORDER BY s.scene_change_id",
            )
            .unwrap();
        stmt.query_map(rusqlite::params![path, seq], |r| {
            Ok(SceneRow {
                document_id: r.get(0)?,
                file_id: r.get(1)?,
                change_type: r.get(2)?,
                component: r.get(3)?,
                field: r.get(4)?,
                before: r.get(5)?,
                after: r.get(6)?,
            })
        })
        .unwrap()
        .map(Result::unwrap)
        .collect()
    }

    /// 文書の (id_source, kind, current_path, broken_at が有るか, broken_note, successor_id)。
    fn document(&self, id: &str) -> Option<(String, String, Option<String>, bool, Option<String>, Option<String>)> {
        self.db()
            .query_row(
                "SELECT id_source, kind, current_path, broken_at IS NOT NULL, broken_note, successor_id FROM documents WHERE document_id = ?1",
                [id],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?, r.get(5)?)),
            )
            .ok()
    }

    /// 番兵を書き、その Revision が出るまで待つ。それより前に書いたパスの処理は済んでいる。
    fn settle(&mut self, name: &str) {
        let rel = format!("zz_sentinel_{name}.txt");
        self.write(&rel, name);
        wait_until(&format!("番兵 {name}"), || self.revisions(&rel) == 1);
    }
}

#[derive(Debug, PartialEq, Eq)]
struct SceneRow {
    document_id: String,
    file_id: Option<i64>,
    change_type: String,
    component: Option<String>,
    field: Option<String>,
    before: Option<String>,
    after: Option<String>,
}

impl Drop for Fixture {
    fn drop(&mut self) {
        if std::thread::panicking() {
            return;
        }
        let actual = tree(&self.root);
        let expected: BTreeMap<String, String> = self.model.iter().map(|(k, v)| (k.clone(), sha256_hex(v))).collect();
        assert_eq!(actual, expected, "作業ツリーがテストの書いた分と違う (追跡が書き込んだ?)");
        let _ = fs::remove_dir_all(&self.root);
    }
}

fn tree(root: &Path) -> BTreeMap<String, String> {
    fn walk(root: &Path, dir: &Path, out: &mut BTreeMap<String, String>) {
        for entry in fs::read_dir(dir).unwrap().flatten() {
            let path = entry.path();
            let rel = path.strip_prefix(root).unwrap().to_string_lossy().replace('\\', "/");
            if rel == ".mye" {
                continue;
            }
            if entry.file_type().unwrap().is_dir() {
                walk(root, &path, out);
            } else {
                out.insert(rel, sha256_hex(&fs::read(&path).unwrap()));
            }
        }
    }
    let mut out = BTreeMap::new();
    walk(root, root, &mut out);
    out
}

fn edit_near_z(text: &str, to: &str) -> String {
    let out = text.replacen("\"nearZ\": 0.10000000149011612", &format!("\"nearZ\": {to}"), 1);
    assert_ne!(out, text);
    out
}

#[test]
fn the_same_field_of_the_same_fileid_in_two_scenes_stays_in_two_documents() {
    let mut fx = Fixture::new("two_scenes");
    let small = fixture("scene_small.scene.json");
    fx.write("a.scene.json.meta", &meta(GUID_A));
    fx.write("a.scene.json", &small);
    fx.write("b.scene.json.meta", &meta(GUID_B));
    fx.write("b.scene.json", &small);
    let tracker = fx.start();

    fx.write("a.scene.json", &edit_near_z(&small, "0.2"));
    fx.write("b.scene.json", &edit_near_z(&small, "0.3"));
    fx.wait_revisions("a.scene.json", 1);
    fx.wait_revisions("b.scene.json", 1);
    tracker.stop("test");

    let a = fx.scene_rows("a.scene.json", 1);
    let b = fx.scene_rows("b.scene.json", 1);
    assert_eq!((a.len(), b.len()), (1, 1), "{a:?} {b:?}");
    assert_eq!((a[0].document_id.as_str(), b[0].document_id.as_str()), (GUID_A, GUID_B));
    assert_eq!(a[0].file_id, b[0].file_id, "同じ fileId");
    assert_eq!((a[0].change_type.as_str(), a[0].component.as_deref(), a[0].field.as_deref()), ("FieldModified", Some("Camera"), Some("nearZ")));
    assert_eq!((a[0].before.as_deref(), a[0].after.as_deref()), (Some("0.10000000149011612"), Some("0.2")));
    assert_eq!(b[0].after.as_deref(), Some("0.3"));
    assert_eq!(fx.count("SELECT COUNT(*) FROM documents"), 2);
    let doc = fx.document(GUID_A).unwrap();
    assert_eq!((doc.0.as_str(), doc.1.as_str(), doc.2.as_deref()), ("meta", "scene", Some("a.scene.json")));
    // 文書の種類は拡張子で決まる: changes.kind
    assert_eq!(fx.count("SELECT COUNT(*) FROM changes WHERE path = 'a.scene.json' AND kind = 'scene' AND document_id = 'aaaaaaaaaaaaaaaa'"), 1);
    assert_eq!(fx.count("SELECT COUNT(*) FROM changes WHERE kind = 'meta'"), 0, "baseline の .meta は変更ではない");
}

#[test]
fn formatting_only_rewrite_keeps_the_line_diff_but_has_no_structural_change() {
    let mut fx = Fixture::new("format_only");
    let small = fixture("scene_small.scene.json");
    fx.write("a.scene.json.meta", &meta(GUID_A));
    fx.write("a.scene.json", &small);
    let tracker = fx.start();
    fx.write("a.scene.json", &fixture("scene_small_reordered.scene.json"));
    fx.wait_revisions("a.scene.json", 1);
    tracker.stop("test");
    assert_eq!(fx.scene_rows("a.scene.json", 1), Vec::new());
    assert!(fx.count("SELECT COUNT(*) FROM change_hunks") >= 1, "行 diff は残る");
    assert_eq!(fx.count("SELECT COUNT(*) FROM documents"), 1, "文書は結び付く");
}

#[test]
fn a_rename_that_moves_the_meta_along_keeps_the_document() {
    let mut fx = Fixture::new("rename_same_guid");
    let small = fixture("scene_small.scene.json");
    fx.write("a.scene.json.meta", &meta(GUID_A));
    fx.write("a.scene.json", &small);
    let tracker = fx.start();
    fx.write("a.scene.json", &edit_near_z(&small, "0.2"));
    fx.wait_revisions("a.scene.json", 1);

    fx.rename("a.scene.json.meta", "sub/a.scene.json.meta");
    fx.rename("a.scene.json", "sub/a.scene.json");
    fx.wait_revisions("sub/a.scene.json", 1);
    fx.settle("after_rename");
    tracker.stop("test");

    assert_eq!(fx.count("SELECT COUNT(*) FROM documents"), 1, "同じ文書のまま");
    let doc = fx.document(GUID_A).unwrap();
    assert_eq!((doc.2.as_deref(), doc.3), (Some("sub/a.scene.json"), false), "{doc:?}");
}

#[test]
fn a_changed_meta_guid_breaks_the_old_document_and_starts_a_new_one() {
    let mut fx = Fixture::new("guid_changed");
    let small = fixture("scene_small.scene.json");
    fx.write("a.scene.json.meta", &meta(GUID_A));
    fx.write("a.scene.json", &small);
    let tracker = fx.start();
    fx.write("a.scene.json", &edit_near_z(&small, "0.2"));
    fx.wait_revisions("a.scene.json", 1);

    // .meta の guid だけが変わる (エディタの外で付け替えた)
    fx.write("a.scene.json.meta", &meta(GUID_B));
    fx.wait_revisions("a.scene.json.meta", 1);
    // 次のシーンの変更は新しい文書の下に付く
    fx.write("a.scene.json", &edit_near_z(&small, "0.3"));
    fx.wait_revisions("a.scene.json", 2);
    tracker.stop("test");

    let old = fx.document(GUID_A).unwrap();
    assert!(old.3, "古い文書に断絶が記録される: {old:?}");
    assert_eq!(old.2, None);
    assert!(old.4.as_deref().is_some_and(|n| n.contains(GUID_A) && n.contains(GUID_B)), "{old:?}");
    assert_eq!(old.5, None, "引き継ぎ先ではない");
    let new = fx.document(GUID_B).unwrap();
    assert_eq!((new.2.as_deref(), new.3), (Some("a.scene.json"), false));
    assert_eq!(fx.scene_rows("a.scene.json", 1)[0].document_id, GUID_A, "断絶より前の変更は古い文書のまま");
    assert_eq!(fx.scene_rows("a.scene.json", 2)[0].document_id, GUID_B);
    // `.meta` の変更自体は 1 つのテキストの変更 (kind = meta)
    assert_eq!(fx.count("SELECT COUNT(*) FROM changes WHERE path = 'a.scene.json.meta' AND kind = 'meta'"), 1);
}

#[test]
fn a_rename_outside_the_editor_with_a_new_guid_is_a_different_document() {
    let mut fx = Fixture::new("rename_new_guid");
    let small = fixture("scene_small.scene.json");
    fx.write("a.scene.json.meta", &meta(GUID_A));
    fx.write("a.scene.json", &small);
    let tracker = fx.start();
    fx.write("a.scene.json", &edit_near_z(&small, "0.2"));
    fx.wait_revisions("a.scene.json", 1);

    // 新しいパスに新しい guid の .meta ができ、古い .meta は消える。内容は同じでも同じ文書とは決めない
    fx.write("c.scene.json.meta", &meta(GUID_C));
    fx.rename("a.scene.json", "c.scene.json");
    fx.remove("a.scene.json.meta");
    fx.wait_revisions("c.scene.json", 1);
    fx.settle("after_outside_rename");
    tracker.stop("test");

    let old = fx.document(GUID_A).unwrap();
    assert!(old.3 && old.2.is_none(), "古い文書に断絶: {old:?}");
    let new = fx.document(GUID_C).unwrap();
    assert_eq!(new.2.as_deref(), Some("c.scene.json"));
}

#[test]
fn a_meta_added_later_inherits_the_provisional_document_and_keeps_the_history() {
    let mut fx = Fixture::new("late_meta");
    let prefab = fixture("collision.prefab.json");
    fx.write("p.prefab.json", &prefab);
    let tracker = fx.start();
    let edited = prefab.replacen("\"name\": \"", "\"name\": \"x", 1);
    assert_ne!(edited, prefab);
    fx.write("p.prefab.json", &edited);
    fx.wait_revisions("p.prefab.json", 1);

    let rows = fx.scene_rows("p.prefab.json", 1);
    assert!(!rows.is_empty() && rows[0].change_type == "PrefabAssetChanged", "{rows:?}");
    let provisional = rows[0].document_id.clone();
    assert_eq!(provisional.len(), 16);
    let doc = fx.document(&provisional).unwrap();
    assert_eq!((doc.0.as_str(), doc.1.as_str()), ("path", "prefab"));

    // 後から .meta が付く
    fx.write("p.prefab.json.meta", &meta(GUID_A));
    fx.wait_revisions("p.prefab.json.meta", 1);
    wait_until("引き継ぎ", || fx.document(&provisional).is_some_and(|d| d.5.is_some()));
    let inherited = fx.document(&provisional).unwrap();
    assert_eq!(inherited.5.as_deref(), Some(GUID_A), "{inherited:?}");
    assert!(!inherited.3, "断絶ではなく引き継ぎ");
    let now = fx.document(GUID_A).unwrap();
    assert_eq!((now.0.as_str(), now.2.as_deref()), ("meta", Some("p.prefab.json")));

    // 以前の scene_changes は元の行のまま残り、新しい ID からたどれる
    assert_eq!(fx.count(&format!("SELECT COUNT(*) FROM scene_changes WHERE document_id = '{provisional}'")), rows.len() as i64);
    fx.write("p.prefab.json", &edited.replacen("\"name\": \"x", "\"name\": \"y", 1));
    fx.wait_revisions("p.prefab.json", 2);
    tracker.stop("test");
    assert_eq!(fx.scene_rows("p.prefab.json", 2)[0].document_id, GUID_A);
    let conn = fx.db();
    assert_eq!(mye_track::document::resolve(&conn, &provisional).unwrap(), GUID_A);
}

#[test]
fn unparsable_files_keep_the_revision_and_the_line_diff_and_the_service_goes_on() {
    let mut fx = Fixture::new("unparsed");
    let small = fixture("scene_small.scene.json");
    fx.write("a.scene.json.meta", &meta(GUID_A));
    fx.write("a.scene.json", &small);
    let tracker = fx.start();

    fx.write("a.scene.json", "");
    fx.wait_revisions("a.scene.json", 1);
    fx.write("a.scene.json", &small);
    fx.wait_revisions("a.scene.json", 2);
    fx.write("a.scene.json", &small.replacen("\"version\": 4", "\"version\": 3", 1));
    fx.wait_revisions("a.scene.json", 3);
    fx.write("a.scene.json", "{\"version\": 3, \"entities\": [");
    fx.wait_revisions("a.scene.json", 4);
    fx.settle("alive");
    tracker.stop("test");

    for (seq, needle) in [(1, "空のファイル"), (2, "空のファイル"), (3, "版が違う"), (4, "JSON を読めない")] {
        let rows = fx.scene_rows("a.scene.json", seq);
        assert_eq!(rows.len(), 1, "seq {seq}: {rows:?}");
        assert_eq!(rows[0].change_type, "unparsed");
        let reason: String = serde_json::from_str(rows[0].after.as_deref().unwrap()).unwrap();
        assert!(reason.contains(needle), "seq {seq}: {reason}");
        assert_eq!(rows[0].file_id, None);
    }
    // 行 diff は今までどおり (空のファイルへの変更は全行削除)
    assert!(fx.count("SELECT COUNT(*) FROM change_hunks h JOIN change_revisions r ON r.revision_id = h.revision_id JOIN changes c ON c.change_id = r.change_id WHERE c.path = 'a.scene.json'") >= 4);
}

fn dump(conn: &Connection, sql: &str) -> Vec<String> {
    let mut stmt = conn.prepare(sql).unwrap();
    let columns = stmt.column_count();
    let rows = stmt
        .query_map([], |row| {
            let mut cells = Vec::new();
            for i in 0..columns {
                cells.push(match row.get_ref(i)? {
                    ValueRef::Null => "NULL".to_string(),
                    ValueRef::Integer(v) => v.to_string(),
                    ValueRef::Real(v) => v.to_string(),
                    ValueRef::Text(t) => format!("{:?}", String::from_utf8_lossy(t)),
                    ValueRef::Blob(b) => format!("blob{}", b.len()),
                });
            }
            Ok(cells.join("|"))
        })
        .unwrap();
    rows.map(Result::unwrap).collect()
}

fn derived(conn: &Connection) -> BTreeMap<&'static str, Vec<String>> {
    let mut out = BTreeMap::new();
    out.insert("documents", dump(conn, "SELECT * FROM documents ORDER BY document_id"));
    out.insert("scene_changes", dump(conn, "SELECT * FROM scene_changes ORDER BY scene_change_id"));
    out.insert("changes", dump(conn, "SELECT * FROM changes ORDER BY change_id"));
    out.insert("change_revisions", dump(conn, "SELECT * FROM change_revisions ORDER BY revision_id"));
    out
}

#[test]
fn a_broken_db_is_rebuilt_to_the_same_documents_and_scene_changes() {
    let mut fx = Fixture::new("rebuild");
    let small = fixture("scene_small.scene.json");
    let prefab = fixture("collision.prefab.json");
    fx.write("a.scene.json.meta", &meta(GUID_A));
    fx.write("a.scene.json", &small);
    fx.write("p.prefab.json", &prefab);
    let tracker = fx.start();
    fx.write("a.scene.json", &edit_near_z(&small, "0.2"));
    fx.wait_revisions("a.scene.json", 1);
    fx.write("a.scene.json.meta", &meta(GUID_B));
    fx.wait_revisions("a.scene.json.meta", 1);
    fx.write("a.scene.json", "broken");
    fx.wait_revisions("a.scene.json", 2);
    fx.write("p.prefab.json", &prefab.replacen("\"name\": \"", "\"name\": \"x", 1));
    fx.wait_revisions("p.prefab.json", 1);
    fx.write("p.prefab.json.meta", &meta(GUID_C));
    fx.wait_revisions("p.prefab.json.meta", 1);
    fx.write("a.scene.json", &small);
    fx.wait_revisions("a.scene.json", 3);
    fx.settle("rebuild");
    tracker.stop("test");

    let before = derived(&fx.db());
    assert!(before["documents"].len() >= 3 && before["scene_changes"].len() >= 4, "{before:#?}");

    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert_eq!(report.state, Some(ServiceState::Recording), "{report:?}");
    assert!(report.broken_db.is_some() && report.rebuilt.is_some(), "{report:?}");
    drop(store);

    let after = derived(&fx.db());
    for table in ["documents", "scene_changes", "changes", "change_revisions"] {
        assert_eq!(before[table], after[table], "{table} が壊す前と違う");
    }
}

/// 計測 (`cargo test --release --test scene_diff -- --ignored --nocapture`)。エンティティ 2 万個のシーンの構造比較。
#[test]
#[ignore]
fn measure_a_20k_entity_scene() {
    let entity = |i: usize, x: f64| {
        format!(
            "{{\"childIndex\":{i},\"components\":{{\"LocalTransform\":{{\"position\":[{x},1.5,2.25],\"rotation\":[0.0,0.0,0.0,1.0],\"scale\":[1.0,1.0,1.0]}},\"MeshRenderer\":{{\"material\":6777745483033203856,\"mesh\":1506918697593860217}}}},\"fileId\":{},\"name\":\"Entity_{i}\"}}",
            i + 1
        )
    };
    let scene = |bump: bool| {
        let items: Vec<String> = (0..20_000).map(|i| entity(i, if bump && i % 1000 == 0 { 9.5 } else { 0.5 })).collect();
        format!("{{\"engine\":\"MyEngine\",\"entities\":[{}],\"nextFileId\":20001,\"sceneName\":\"big\",\"version\":4}}", items.join(",\n"))
    };
    let (before, after) = (scene(false), scene(true));
    println!("入力: {} バイト、エンティティ 20000 個", before.len());
    let started = Instant::now();
    let out = diff(DocKind::Scene, before.as_bytes(), after.as_bytes(), LIMIT);
    let elapsed = started.elapsed();
    let SceneOutcome::Changes(c) = out else { panic!("解析不可: {out:?}") };
    assert_eq!(c.len(), 20);
    println!("構造比較 (純粋な関数): {elapsed:?}、変更 {} 件", c.len());
}

/// 計測 (同上)。Tracker を通して、大きなシーンの記録が他の変更の記録を止め続けないことと所要時間を見る。
#[test]
#[ignore]
fn measure_recording_a_20k_entity_scene_through_the_tracker() {
    let scene = |bump: bool| {
        let items: Vec<String> = (0..20_000)
            .map(|i| {
                let x = if bump && i % 1000 == 0 { 9.5 } else { 0.5 };
                format!(
                    "{{\"childIndex\":{i},\"components\":{{\"LocalTransform\":{{\"position\":[{x},1.5,2.25],\"rotation\":[0.0,0.0,0.0,1.0],\"scale\":[1.0,1.0,1.0]}},\"MeshRenderer\":{{\"material\":6777745483033203856,\"mesh\":1506918697593860217}}}},\"fileId\":{},\"name\":\"Entity_{i}\"}}",
                    i + 1
                )
            })
            .collect();
        format!("{{\"engine\":\"MyEngine\",\"entities\":[{}],\"nextFileId\":20001,\"sceneName\":\"big\",\"version\":4}}", items.join(",\n"))
    };
    let mut fx = Fixture::new("measure");
    fx.write("big.scene.json.meta", &meta(GUID_A));
    fx.write("big.scene.json", &scene(false));
    let tracker = fx.start();
    let started = Instant::now();
    fx.write("big.scene.json", &scene(true));
    fx.write("other.txt", "x");
    fx.wait_revisions("other.txt", 1);
    let other_done = started.elapsed();
    fx.wait_revisions("big.scene.json", 1);
    let big_done = started.elapsed();
    tracker.stop("test");
    println!("big.scene.json: 書き込みから Revision 確定まで {big_done:?}、同時に書いた other.txt は {other_done:?}");
    assert_eq!(fx.scene_rows("big.scene.json", 1).len(), 20);
}
