//! プレハブ伝播 (`PrefabPropagation`) の結合テスト (M104t、要求 §16.3 の 5 番目)。
//!
//! プレハブのファイルのフィールドを変えたとき、そのプレハブを直に参照するシーンのインスタンスのうち override していない
//! メンバにだけ伝播の行が出ること、原因の行を指すこと、DB を作り直しても同じ行になることを Tracker を通して確かめる。
//! 待ちは固定の sleep ではなく上限付きの待ち合わせ。

use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use rusqlite::types::ValueRef;
use rusqlite::Connection;
use serde_json::{json, Map, Value};

use mye_track::collector::TrackerConfig;
use mye_track::db::now_ms;
use mye_track::protocol::Args;
use mye_track::scene_overlay::scene_overlay;
use mye_track::startup::{open_project, ServiceState};
use mye_track::store::Paths;
use mye_track::watch::{Phase, Tracker};

const WAIT_LIMIT: Duration = Duration::from_secs(60);
const GUID_PREFAB: &str = "00000000000000a1";
const GUID_SCENE: &str = "5ce0000000000001";
const PREFAB_HASH: u64 = 0xa1;

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

struct Fixture {
    root: PathBuf,
}

impl Fixture {
    fn new(name: &str) -> Fixture {
        let root = std::env::temp_dir().join(format!("mye_track_pp_{}_{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        Fixture { root }
    }

    fn write(&self, rel: &str, text: &str) {
        let p = self.root.join(rel);
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(&p, text.as_bytes()).unwrap();
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
}

impl Drop for Fixture {
    fn drop(&mut self) {
        if !std::thread::panicking() {
            let _ = fs::remove_dir_all(&self.root);
        }
    }
}

fn meta(guid: &str, kind: &str) -> String {
    format!("{{\n  \"guid\": \"{guid}\",\n  \"type\": \"{kind}\",\n  \"version\": 1\n}}")
}

/// プレハブ: ルート (fileId 1、LocalTransform) と子 (fileId 2、Camera)。
fn prefab(position_x: &str, fov: &str) -> String {
    format!(
        "{{\"prefab\":1,\"entities\":[\
         {{\"fileId\":1,\"name\":\"Root\",\"childIndex\":0,\"components\":{{\"LocalTransform\":{{\"position\":[{position_x},0,0],\"scale\":1}}}}}},\
         {{\"fileId\":2,\"name\":\"Kid\",\"parent\":1,\"childIndex\":0,\"components\":{{\"Camera\":{{\"fov\":{fov},\"nearZ\":0.1}}}}}}]}}"
    )
}

/// インスタンス 1 つ (ルート `root_fid`、子 `root_fid + 1`)。`overrides` は (ルート, 子)。None なら旧形式 (キー無し)。
fn instance(root_fid: i64, hash: u64, root_overrides: Option<&str>, kid_overrides: Option<&str>) -> String {
    let ov = |o: Option<&str>| o.map_or(String::new(), |list| format!(",\"overrides\":[{list}]"));
    let kid = root_fid + 1;
    format!(
        "{{\"fileId\":{root_fid},\"name\":\"Root\",\"childIndex\":0,\"components\":{{\"LocalTransform\":{{\"position\":[5,0,0],\"scale\":1}},\
         \"PrefabInstance\":{{\"prefabHash\":{hash},\"outerLocalId\":0}},\"PrefabLink\":{{\"localId\":1}}}}{r}}},\
         {{\"fileId\":{kid},\"name\":\"Kid\",\"parent\":{root_fid},\"childIndex\":0,\"components\":{{\"Camera\":{{\"fov\":70,\"nearZ\":0.1}},\
         \"PrefabLink\":{{\"localId\":2}}}}{k}}}",
        r = ov(root_overrides),
        k = ov(kid_overrides),
    )
}

fn scene(entities: &[String]) -> String {
    format!("{{\"engine\":\"MyEngine\",\"version\":4,\"sceneName\":\"s\",\"nextFileId\":100,\"entities\":[{}]}}", entities.join(","))
}

type Row = (String, Option<i64>, Option<String>, Option<String>, Option<String>, Option<String>, Option<i64>);

fn propagation_rows(conn: &Connection) -> Vec<Row> {
    let mut stmt = conn
        .prepare(
            "SELECT document_id, file_id, component, field, before_value, after_value, caused_by
               FROM scene_changes WHERE change_type = 'PrefabPropagation' ORDER BY scene_change_id",
        )
        .unwrap();
    stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?, r.get(5)?, r.get(6)?)))
        .unwrap()
        .map(Result::unwrap)
        .collect()
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
    out.insert("scene_changes", dump(conn, "SELECT * FROM scene_changes ORDER BY scene_change_id"));
    out.insert("change_revisions", dump(conn, "SELECT * FROM change_revisions ORDER BY revision_id"));
    out
}

fn overlay(conn: &Connection, document_id: &str) -> Value {
    let mut map = Map::new();
    map.insert("documentId".to_string(), json!(document_id));
    scene_overlay(conn, Args::new(map)).unwrap()
}

#[test]
fn a_base_change_reaches_only_the_instances_that_did_not_override_it_and_survives_a_rebuild() {
    let fx = Fixture::new("reach");
    fx.write("p.prefab.json", &prefab("0", "60"));
    fx.write("p.prefab.json.meta", &meta(GUID_PREFAB, "prefab"));
    // インスタンス A (10, 11): override なし。B (20, 21): ルートの position と子の nearZ を override。C は別のプレハブ
    let s = scene(&[
        instance(10, PREFAB_HASH, Some(""), Some("")),
        instance(20, PREFAB_HASH, Some("\"LocalTransform.position\""), Some("\"Camera.nearZ\"")),
        instance(30, 0xb2, Some(""), Some("")),
    ]);
    fx.write("a.scene.json", &s);
    fx.write("a.scene.json.meta", &meta(GUID_SCENE, "scene"));
    // 参照していないシーン (同じ数字が別の意味で出てくる)
    fx.write("b.scene.json", &scene(&[instance(10, 0xb2, Some(""), Some(""))]));
    let tracker = fx.start();

    // プレハブのルートの position と子の fov を変える (子の nearZ は変えない)
    fx.write("p.prefab.json", &prefab("9", "75"));
    fx.wait_revisions("p.prefab.json", 1);
    tracker.stop("test");

    let conn = fx.db();
    let rows = propagation_rows(&conn);
    let mut summary: Vec<(i64, String, Option<String>, Option<String>)> = rows
        .iter()
        .map(|r| (r.1.unwrap_or(0), r.0.clone(), r.2.clone(), r.3.clone()))
        .collect();
    summary.sort();
    // A のルート (10) の position、A の子 (11) の fov、B の子 (21) の fov。B のルート (20) は position を override している
    let expected: Vec<(i64, String, Option<String>, Option<String>)> = vec![
        (10, GUID_SCENE.to_string(), Some("LocalTransform".to_string()), Some("position".to_string())),
        (11, GUID_SCENE.to_string(), Some("Camera".to_string()), Some("fov".to_string())),
        (21, GUID_SCENE.to_string(), Some("Camera".to_string()), Some("fov".to_string())),
    ];
    assert_eq!(summary, expected, "{rows:#?}");
    // 値はプレハブの Before / After
    let fov = rows.iter().find(|r| r.1 == Some(11)).unwrap();
    assert_eq!((fov.4.as_deref(), fov.5.as_deref()), (Some("60"), Some("75")));
    // 原因は同じ Revision のプレハブ側の行 (同じコンポーネント・項目の FieldModified)
    for row in &rows {
        let (cause_type, cause_component, cause_field, cause_revision): (String, Option<String>, Option<String>, i64) = conn
            .query_row(
                "SELECT change_type, component, field, revision_id FROM scene_changes WHERE scene_change_id = ?1",
                [row.6.unwrap()],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
            )
            .unwrap();
        assert_eq!((cause_type.as_str(), &cause_component, &cause_field), ("FieldModified", &row.2, &row.3));
        let own_revision: i64 = conn
            .query_row("SELECT revision_id FROM scene_changes WHERE change_type = 'PrefabPropagation' AND file_id = ?1 LIMIT 1", [row.1.unwrap()], |r| r.get(0))
            .unwrap();
        assert_eq!(own_revision, cause_revision, "伝播の行は原因の Revision に付く");
    }
    // 参照していないシーン (b) と別のプレハブのインスタンス (30) には出ない
    assert!(rows.iter().all(|r| r.1 != Some(30) && r.1 != Some(31)));

    // scene_overlay: シーン a の文書に伝播として出る (直接の編集とは別の印と、原因のプレハブのパス)
    let o = overlay(&conn, GUID_SCENE);
    let entities = o["entities"].as_array().unwrap();
    let ids: Vec<i64> = entities.iter().map(|e| e["fileId"].as_i64().unwrap()).collect();
    assert_eq!(ids, vec![10, 11, 21], "{o}");
    let kid = entities.iter().find(|e| e["fileId"] == 11).unwrap();
    let field = &kid["components"][0]["fields"][0];
    assert_eq!(field["changeType"], "PrefabPropagation", "{field}");
    assert_eq!((field["propagated"].as_bool(), field["override"].as_bool()), (Some(true), Some(false)));
    assert_eq!(field["causePath"], "p.prefab.json");

    // プレハブの Revision の葉の数は、伝播の行を含まない (確認の対象にしない)
    let leaves: i64 = conn
        .query_row(
            "SELECT COUNT(*) FROM scene_changes WHERE change_type <> 'PrefabPropagation' AND revision_id =
               (SELECT MAX(revision_id) FROM change_revisions)",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(leaves, 3, "PrefabAssetChanged + position + fov");
    drop(conn);

    // DB を壊して作り直しても、伝播の行は同じ (spec D26)
    let before = derived(&fx.db());
    let paths = Paths::for_project(&fx.root);
    fs::write(&paths.db, b"this is not a sqlite database").unwrap();
    for suffix in ["-wal", "-shm"] {
        let _ = fs::remove_file(format!("{}{suffix}", paths.db.display()));
    }
    let (store, report) = open_project(&fx.root, now_ms());
    assert_eq!(report.state, Some(ServiceState::Recording), "{report:?}");
    assert!(report.rebuilt.is_some(), "{report:?}");
    drop(store);
    let after = derived(&fx.db());
    for table in ["scene_changes", "change_revisions"] {
        assert_eq!(before[table], after[table], "{table} が壊す前と違う");
    }
}

#[test]
fn legacy_members_and_unreadable_scenes_are_reported_in_a_summary_row_not_hidden() {
    let fx = Fixture::new("limits");
    fx.write("p.prefab.json", &prefab("0", "60"));
    fx.write("p.prefab.json.meta", &meta(GUID_PREFAB, "prefab"));
    // 旧形式 (overrides キー無し) のインスタンスは override かどうか分からない → 行を出さず数える
    fx.write("a.scene.json", &scene(&[instance(10, PREFAB_HASH, None, None)]));
    // ハッシュの数字を含むが JSON として読めないシーン
    fx.write("broken.scene.json", &format!("{{\"prefabHash\":{PREFAB_HASH}, nope"));
    let tracker = fx.start();
    fx.write("p.prefab.json", &prefab("0", "75"));
    fx.wait_revisions("p.prefab.json", 1);
    tracker.stop("test");

    let conn = fx.db();
    let rows = propagation_rows(&conn);
    assert_eq!(rows.len(), 1, "{rows:#?}");
    let summary: Value = serde_json::from_str(rows[0].5.as_deref().unwrap()).unwrap();
    assert_eq!(rows[0].1, None, "要約の行は file_id を持たない");
    assert_eq!(summary["legacyMembers"], 1, "{summary}");
    assert_eq!(summary["scenesUnreadable"], 1, "{summary}");
    assert_eq!(summary["scenesChecked"], 1, "{summary}");
}

#[test]
fn deleted_scenes_and_scenes_without_the_prefab_are_left_out() {
    let fx = Fixture::new("gone");
    fx.write("p.prefab.json", &prefab("0", "60"));
    fx.write("p.prefab.json.meta", &meta(GUID_PREFAB, "prefab"));
    fx.write("gone.scene.json", &scene(&[instance(10, PREFAB_HASH, Some(""), Some(""))]));
    fx.write("zz.txt", "x");
    let tracker = fx.start();
    fs::remove_file(fx.root.join("gone.scene.json")).unwrap();
    fx.wait_revisions("gone.scene.json", 1);
    fx.write("p.prefab.json", &prefab("0", "75"));
    fx.wait_revisions("p.prefab.json", 1);
    tracker.stop("test");
    assert_eq!(propagation_rows(&fx.db()), Vec::new(), "削除済みのシーンへ伝播を出さない");
}

// ---- 計測 (`cargo test --release --test propagation -- --ignored --nocapture --test-threads=1`。結果は docs/ai-change-tracking/performance.md §10) ----

/// エンティティ `count` 個のシーン (各 1 コンポーネント)。`edited` 個目までの Light.intensity を `value` にする。
fn big_scene(count: usize, edited: usize, value: &str) -> String {
    let mut entities = Vec::with_capacity(count);
    for i in 0..count {
        let intensity = if i < edited { value } else { "1.0" };
        entities.push(format!(
            "{{\"fileId\":{},\"name\":\"E{i}\",\"childIndex\":0,\"components\":{{\"Light\":{{\"intensity\":{intensity},\"range\":10.0}},\"LocalTransform\":{{\"position\":[0,0,0],\"scale\":1}}}}}}",
            i + 1
        ));
    }
    scene(&entities)
}

#[test]
#[ignore]
fn measure_scene_overlay_response_for_a_20k_entity_scene() {
    for edited in [10usize, 1000, 20_000] {
        let fx = Fixture::new(&format!("ovl{edited}"));
        fx.write("big.scene.json", &big_scene(20_000, 0, "1.0"));
        fx.write("big.scene.json.meta", &meta(GUID_SCENE, "scene"));
        let tracker = fx.start();
        fx.write("big.scene.json", &big_scene(20_000, edited, "2.5"));
        fx.wait_revisions("big.scene.json", 1);
        tracker.stop("measure");
        let conn = fx.db();
        let rows: i64 = conn.query_row("SELECT COUNT(*) FROM scene_changes", [], |r| r.get(0)).unwrap();
        let started = Instant::now();
        let mut pages = 0usize;
        let mut bytes = 0usize;
        let mut cursor: Option<String> = None;
        loop {
            let mut map = Map::new();
            map.insert("documentId".to_string(), json!(GUID_SCENE));
            if let Some(c) = &cursor {
                map.insert("cursor".to_string(), json!(c));
            }
            let page = scene_overlay(&conn, Args::new(map)).unwrap();
            pages += 1;
            bytes += page.to_string().len();
            match page["nextCursor"].as_str() {
                Some(next) => cursor = Some(next.to_string()),
                None => break,
            }
        }
        println!(
            "[perf] scene_overlay edited={edited} scene_changes_rows={rows} total={:.1} ms pages={pages} bytes={bytes}",
            started.elapsed().as_secs_f64() * 1000.0
        );
    }
}

#[test]
#[ignore]
fn measure_prefab_propagation_cost() {
    // 100 のシーン x 100 組のインスタンス (参照するプレハブは p)。対照は、どのシーンも参照しないプレハブ q の同じ編集
    let fx = Fixture::new("propcost");
    fx.write("p.prefab.json", &prefab("0", "60"));
    fx.write("p.prefab.json.meta", &meta(GUID_PREFAB, "prefab"));
    fx.write("q.prefab.json", &prefab("0", "60"));
    fx.write("q.prefab.json.meta", &meta("00000000000000a2", "prefab"));
    for s in 0..100 {
        let instances: Vec<String> = (0..100).map(|i| instance(10 + (i as i64) * 2, PREFAB_HASH, Some(""), Some(""))).collect();
        fx.write(&format!("s{s}.scene.json"), &scene(&instances));
    }
    let tracker = fx.start();
    let timed = |path: &str, fov: &str| {
        let started = Instant::now();
        fx.write(path, &prefab("0", fov));
        fx.wait_revisions(path, 1);
        started.elapsed().as_secs_f64() * 1000.0
    };
    let control = timed("q.prefab.json", "75");
    let with_scenes = timed("p.prefab.json", "75");
    tracker.stop("measure");
    let conn = fx.db();
    let rows = propagation_rows(&conn).len();
    println!("[perf] propagation control(no scene)={control:.0} ms with 100 scenes x 100 instances={with_scenes:.0} ms rows={rows} (デバウンス 150 ms と書き手の待ちを含む)");
}
