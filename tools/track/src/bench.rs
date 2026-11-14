//! 計測用のサブコマンド (`mye_track bench ...`)。ベースラインと記録のスループットを、
//! 記録の置き場所を別ディレクトリへ向けて測る (追跡対象を書かない)。結果の読み方は
//! docs/ai-change-tracking/performance.md。

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use rusqlite::Connection;

use crate::collector::TrackerConfig;
use crate::db::now_ms;
use crate::startup::{open_project_with, ServiceState, StartupOptions};
use crate::watch::{Phase, Tracker};

/// 1 回の待ちの上限。これを超えたら「終わらなかった」として値を出す。
const WAIT_LIMIT: Duration = Duration::from_secs(45 * 60);

const USAGE: &str = "usage:\n  mye_track bench gen --dir <dir> --files <N>\n  mye_track bench baseline --project <root> --data <dir>\n  mye_track bench changes --project <root> --data <dir> [--counts 1,1000,32000]\n  mye_track bench flood --dir <新しい作業ディレクトリ> [--flood-dir bin] [--flood-files 64000] [--flood-threads 16] [--saves 200] [--interval-ms 50] [--load 0]\n  mye_track bench burst --dir <新しい作業ディレクトリ> [--files 2500] [--load 0]\n  mye_track bench load [--threads 1] [--secs 60]\n  mye_track bench plan --project <root>";

pub fn run(args: &[String]) -> u8 {
    let flag = |name: &str| -> Option<String> {
        args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned()
    };
    match args.first().map(String::as_str) {
        Some("gen") => {
            let (Some(dir), Some(files)) = (flag("--dir"), flag("--files").and_then(|n| n.parse::<usize>().ok())) else {
                eprintln!("{USAGE}");
                return 2;
            };
            generate(Path::new(&dir), files);
            0
        }
        Some(cmd @ ("baseline" | "changes")) => {
            let (Some(project), Some(data)) = (flag("--project"), flag("--data")) else {
                eprintln!("{USAGE}");
                return 2;
            };
            let counts: Vec<usize> = flag("--counts")
                .map(|v| v.split(',').filter_map(|n| n.trim().parse().ok()).collect())
                .unwrap_or_else(|| vec![1, 1000, 32000]);
            let (project, data) = (PathBuf::from(project), PathBuf::from(data));
            if cmd == "baseline" {
                baseline(&project, &data)
            } else {
                changes(&project, &data, &counts)
            }
        }
        Some("load") => {
            // 別プロセスの CPU 負荷 (sub-06 の再現条件)。指定の秒数だけ回って終わる
            let number = |name: &str, default: usize| flag(name).and_then(|v| v.parse().ok()).unwrap_or(default);
            let _load = CpuLoad::start(number("--threads", 1));
            std::thread::sleep(Duration::from_secs(number("--secs", 60) as u64));
            0
        }
        Some("plan") => {
            // 監視の分け方だけを出す (監視は張らない。追跡対象は読まない)
            let Some(project) = flag("--project") else {
                eprintln!("{USAGE}");
                return 2;
            };
            let started = Instant::now();
            let plan = crate::watch_plan::plan(Path::new(&project), crate::watch_plan::MAX_WATCHES);
            println!("watches: {} fallback: {} plan_ms: {}", plan.entries.len(), plan.fallback, started.elapsed().as_millis());
            for (rel, recursive) in &plan.entries {
                println!("  {} {}", if *recursive { "R" } else { "-" }, if rel.is_empty() { "." } else { rel });
            }
            0
        }
        Some(cmd @ ("flood" | "burst")) => {
            let Some(dir) = flag("--dir") else {
                eprintln!("{USAGE}");
                return 2;
            };
            let number = |name: &str, default: usize| flag(name).and_then(|v| v.parse().ok()).unwrap_or(default);
            if cmd == "flood" {
                let options = FloodOptions {
                    flood_dir: flag("--flood-dir").unwrap_or_else(|| "bin".to_string()),
                    flood_files: number("--flood-files", 64000),
                    flood_threads: number("--flood-threads", 16).max(1),
                    saves: number("--saves", 200),
                    interval: Duration::from_millis(number("--interval-ms", 50) as u64),
                    load: number("--load", 0),
                };
                flood(Path::new(&dir), &options)
            } else {
                burst(Path::new(&dir), number("--files", 2500), number("--load", 0))
            }
        }
        _ => {
            eprintln!("{USAGE}");
            2
        }
    }
}

/// 計測用のファイルを作る。256 ファイルずつのディレクトリに、内容の違う小さなテキストを置く。
fn generate(dir: &Path, files: usize) {
    for i in 0..files {
        let sub = dir.join(format!("d{:03}", i / 256));
        fs::create_dir_all(&sub).expect("create dir");
        fs::write(sub.join(format!("f{i:05}.txt")), content(i, 1)).expect("write file");
    }
    println!("generated: {files} files in {}", dir.display());
}

fn content(i: usize, version: u64) -> Vec<u8> {
    format!("bench file {i} version {version}\n{}\n", "x".repeat(160 + i % 64)).into_bytes()
}

fn start(project: &Path, data: &Path) -> Option<Tracker> {
    start_with(project, data, TrackerConfig::default())
}

fn start_with(project: &Path, data: &Path, cfg: TrackerConfig) -> Option<Tracker> {
    let options = StartupOptions { data_dir: Some(data.to_path_buf()), ..StartupOptions::default() };
    let (store, report) = open_project_with(project, now_ms(), options);
    if report.state != Some(ServiceState::Recording) {
        eprintln!("記録を始められない: {:?}", report.state);
        return None;
    }
    Some(Tracker::start(project, store?, cfg))
}

fn ms(ns: u64) -> u64 {
    ns / 1_000_000
}

/// Writer の中の累計 (Blob・ジャーナル・DB) を出す。
fn print_writer_stats(tracker: &Tracker, label: &str) {
    let inner = label.to_string();
    let line = tracker
        .writer()
        .call(move |store| {
            let b = &store.blobs.stats;
            let s = &store.stats;
            format!(
                "{inner}: blob_puts={} blob_existed={} blob_hash_ms={} blob_existence_check_ms={} blob_ensure_dir_ms={} blob_create_write_ms={} blob_verify_read_ms={} blob_rename_ms={} journal_append_ms={} event_insert_ms={} snapshot_insert_ms={} tx_commit_ms={}",
                b.puts.load(Ordering::Relaxed),
                b.existed.load(Ordering::Relaxed),
                ms(b.hash_ns.load(Ordering::Relaxed)),
                ms(b.existence_check_ns.load(Ordering::Relaxed)),
                ms(b.ensure_dir_ns.load(Ordering::Relaxed)),
                ms(b.create_write_ns.load(Ordering::Relaxed)),
                ms(b.verify_ns.load(Ordering::Relaxed)),
                ms(b.rename_ns.load(Ordering::Relaxed)),
                ms(s.journal_append_ns.load(Ordering::Relaxed)),
                ms(s.event_insert_ns.load(Ordering::Relaxed)),
                ms(s.snapshot_insert_ns.load(Ordering::Relaxed)),
                ms(s.tx_commit_ns.load(Ordering::Relaxed)),
            )
        })
        .unwrap_or_else(|e| format!("{label}: (Writer から取れない: {e})"));
    println!("{line}");
}

/// ディレクトリの総バイト数と本数 (Blob の重複排除の後の量)。
fn dir_total(dir: &Path) -> (u64, u64) {
    let (mut bytes, mut count) = (0, 0);
    let Ok(entries) = fs::read_dir(dir) else { return (0, 0) };
    for entry in entries.flatten() {
        let Ok(t) = entry.file_type() else { continue };
        if t.is_dir() {
            let (b, c) = dir_total(&entry.path());
            bytes += b;
            count += c;
        } else if let Ok(m) = entry.metadata() {
            bytes += m.len();
            count += 1;
        }
    }
    (bytes, count)
}

fn baseline(project: &Path, data: &Path) -> u8 {
    let Some(tracker) = start(project, data) else { return 1 };
    let wall = Instant::now();
    let status = tracker.wait_ready(WAIT_LIMIT);
    let wall_ms = wall.elapsed().as_millis();
    if status.phase != Phase::Watching {
        println!("baseline: 終わらなかった phase={:?}", status.phase);
        return 1;
    }
    let t = status.baseline_timings;
    println!("baseline_total_ms: {}", status.baseline_ms);
    println!("baseline_wait_ms: {wall_ms}");
    println!("files: {}", status.baseline_files);
    println!("source_bytes: {}", status.baseline_bytes);
    println!("walk_ms: {}", t.walk_ms);
    println!("read_ms: {}", t.read_ms);
    println!("hash_ms: {}", t.hash_ms);
    println!("commit_ms (Writer: Blob + ジャーナル + DB): {}", t.commit_ms);
    print_writer_stats(&tracker, "writer");
    tracker.stop("bench");
    let (blob_bytes, blob_files) = dir_total(&data.join("blobs"));
    println!("blob_files (重複排除後): {blob_files}");
    println!("blob_bytes (重複排除後): {blob_bytes}");
    0
}

fn count_revisions(db: &Path) -> i64 {
    Connection::open(db)
        .and_then(|c| {
            c.busy_timeout(Duration::from_secs(10))?;
            c.query_row("SELECT COUNT(*) FROM change_revisions", [], |r| r.get(0))
        })
        .unwrap_or(-1)
}

/// `count` 件を書き換え、全部の Revision が出るまでの時間を測る。
fn rewrite(project: &Path, db: &Path, tracker: &Tracker, files: &[PathBuf], first: usize, count: usize, version: u64) {
    let before = count_revisions(db);
    let started = Instant::now();
    for (i, file) in files.iter().enumerate().skip(first).take(count) {
        fs::write(file, content(i, version)).expect("rewrite");
    }
    let written = started.elapsed();
    let target = before + count as i64;
    let mut last_report = Instant::now();
    loop {
        let have = count_revisions(db);
        if have >= target {
            break;
        }
        if started.elapsed() > WAIT_LIMIT {
            println!("changes[{count}]: 終わらなかった ({}/{count} 件で打ち切り)", have - before);
            return;
        }
        if last_report.elapsed() > Duration::from_secs(15) {
            eprintln!("  ... {}/{count}", have - before);
            last_report = Instant::now();
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    let total = started.elapsed();
    println!(
        "changes[{count}]: write_ms={} total_ms={} per_sec={:.1} (project={})",
        written.as_millis(),
        total.as_millis(),
        count as f64 / total.as_secs_f64(),
        project.display()
    );
    print_writer_stats(tracker, &format!("writer after changes[{count}]"));
}

fn changes(project: &Path, data: &Path, counts: &[usize]) -> u8 {
    let Some(tracker) = start(project, data) else { return 1 };
    let status = tracker.wait_ready(WAIT_LIMIT);
    if status.phase != Phase::Watching {
        println!("baseline: 終わらなかった phase={:?}", status.phase);
        return 1;
    }
    println!("baseline_ms: {} files: {}", status.baseline_ms, status.baseline_files);
    let mut files: Vec<PathBuf> = Vec::new();
    collect(project, &mut files);
    files.sort();
    let db = data.join("tracking.db");
    // 各回は別のファイルを使う (同じファイルの連続した変更にしない)。version は回ごとに増やす
    let mut cursor = 0usize;
    // 版は実行ごとに変える (前の実行が書いた内容と同じだと、変更にならず、記録が増えないまま待ち続ける)
    let mut version = now_ms() as u64;
    for &count in counts {
        if cursor + count > files.len() {
            cursor = 0;
        }
        if count == 1 {
            // 1 件: 5 回測って個別に出す (デバウンス 300 ms を含む)
            for _ in 0..5 {
                rewrite(project, &db, &tracker, &files, cursor, 1, version);
                cursor = (cursor + 1) % files.len();
                version += 1;
            }
        } else {
            rewrite(project, &db, &tracker, &files, cursor, count, version);
            cursor += count;
            version += 1;
        }
    }
    tracker.stop("bench");
    0
}

fn collect(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else { return };
    for entry in entries.flatten() {
        let Ok(t) = entry.file_type() else { continue };
        if t.is_dir() {
            collect(&entry.path(), out);
        } else {
            out.push(entry.path());
        }
    }
}

/// 取りこぼしの計測 (M104g2) の作業場所。作業ディレクトリは新しく作る (あるものは使わない = 消さない)。
struct Lab {
    project: PathBuf,
    data: PathBuf,
}

impl Lab {
    fn create(dir: &Path) -> Option<Lab> {
        if dir.exists() {
            eprintln!("作業ディレクトリが既にある: {} (消してから、または別の名前で)", dir.display());
            return None;
        }
        let project = dir.join("project");
        let data = dir.join("data");
        fs::create_dir_all(&project).expect("create project");
        fs::create_dir_all(&data).expect("create data");
        Some(Lab { project, data })
    }

    fn db(&self) -> PathBuf {
        self.data.join("tracking.db")
    }
}

/// CPU を使い続けるスレッド (読む側が CPU を取れない状況を作る)。`drop` で止める。
struct CpuLoad {
    stop: Arc<AtomicBool>,
    threads: Vec<std::thread::JoinHandle<()>>,
}

impl CpuLoad {
    fn start(count: usize) -> CpuLoad {
        let stop = Arc::new(AtomicBool::new(false));
        let threads = (0..count)
            .map(|_| {
                let stop = Arc::clone(&stop);
                std::thread::spawn(move || {
                    let mut x = 1u64;
                    while !stop.load(Ordering::Relaxed) {
                        for _ in 0..10_000 {
                            x = std::hint::black_box(x.wrapping_mul(6364136223846793005).wrapping_add(1));
                        }
                    }
                })
            })
            .collect();
        CpuLoad { stop, threads }
    }
}

impl Drop for CpuLoad {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        for t in self.threads.drain(..) {
            let _ = t.join();
        }
    }
}

fn scalar(db: &Path, sql: &str) -> i64 {
    Connection::open(db)
        .and_then(|c| {
            c.busy_timeout(Duration::from_secs(10))?;
            c.query_row(sql, [], |r| r.get(0))
        })
        .unwrap_or(-1)
}

/// 取りこぼしの計測用の設定。検証の走査を短い間隔で回し、待ちを短くする (取りこぼしの数には影響しない)。
fn lab_config() -> TrackerConfig {
    TrackerConfig {
        verify_quiet: Duration::from_secs(1),
        verify_interval: Duration::from_secs(2),
        ..TrackerConfig::default()
    }
}

/// 検証の走査が 2 回走る (拾えるものを拾い終える) まで待つ。
fn wait_two_scans(tracker: &Tracker) {
    let runs = tracker.status().verify_runs;
    let deadline = Instant::now() + Duration::from_secs(120);
    while tracker.status().verify_runs < runs + 2 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(100));
    }
}

struct FloodOptions {
    flood_dir: String,
    flood_files: usize,
    flood_threads: usize,
    saves: usize,
    interval: Duration,
    load: usize,
}

const TRACKED_PATH: &str = "c.path LIKE 'src/s%'";

/// 実測 A: 洪水と編集の重なり。`flood_dir` (無視される場所) へ別スレッドが書き続けている間に、
/// 追跡対象の `src/s*.txt` を 1 つずつ `saves` 回保存し、通常の Revision と `gap` の数を数える。
fn flood(dir: &Path, options: &FloodOptions) -> u8 {
    let Some(lab) = Lab::create(dir) else { return 1 };
    let flood_root = lab.project.join(&options.flood_dir);
    fs::create_dir_all(&flood_root).expect("create flood dir");
    let tracked = lab.project.join("src");
    fs::create_dir_all(&tracked).expect("create src");
    for i in 0..options.saves {
        fs::write(tracked.join(format!("s{i:04}.txt")), content(i, 1)).expect("write tracked");
    }
    let Some(tracker) = start_with(&lab.project, &lab.data, lab_config()) else { return 1 };
    let status = tracker.wait_ready(WAIT_LIMIT);
    if status.phase != Phase::Watching {
        println!("baseline: 終わらなかった phase={:?}", status.phase);
        return 1;
    }
    let _load = (options.load > 0).then(|| CpuLoad::start(options.load));
    // ベースラインと保存の更新時刻が同じ刻みに入らないようにする
    std::thread::sleep(Duration::from_millis(300));

    let started = Instant::now();
    let per_thread = options.flood_files / options.flood_threads;
    let flooders: Vec<_> = (0..options.flood_threads)
        .map(|t| {
            let sub = flood_root.join(format!("t{t:02}"));
            std::thread::spawn(move || {
                fs::create_dir_all(&sub).expect("flood dir");
                for i in 0..per_thread {
                    let name = format!("long_flood_file_name_{t:02}_{i:05}_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.bin");
                    let _ = fs::write(sub.join(name), b"x");
                }
            })
        })
        .collect();
    for i in 0..options.saves {
        fs::write(tracked.join(format!("s{i:04}.txt")), content(i, 2)).expect("save");
        std::thread::sleep(options.interval);
    }
    let saves_ms = started.elapsed().as_millis();
    for f in flooders {
        let _ = f.join();
    }
    let flood_ms = started.elapsed().as_millis();

    let db = lab.db();
    let distinct = format!(
        "SELECT COUNT(DISTINCT c.change_id) FROM change_revisions r JOIN changes c ON c.change_id = r.change_id WHERE {TRACKED_PATH}"
    );
    let deadline = Instant::now() + Duration::from_secs(300);
    while scalar(&db, &distinct) < options.saves as i64 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(100));
    }
    wait_two_scans(&tracker);
    let final_status = tracker.status();
    let by = |extra: &str| {
        scalar(
            &db,
            &format!("SELECT COUNT(*) FROM change_revisions r JOIN changes c ON c.change_id = r.change_id WHERE {TRACKED_PATH} {extra}"),
        )
    };
    println!(
        "flood: dir={} flood_files={} threads={} saves={} interval_ms={} load={} saves_ms={saves_ms} flood_ms={flood_ms}",
        options.flood_dir,
        per_thread * options.flood_threads,
        options.flood_threads,
        options.saves,
        options.interval.as_millis(),
        options.load
    );
    println!(
        "result: tracked_normal={} tracked_gap={} tracked_missing={} watcher_overflow_intervals={} overflows={} verify_runs={} verify_gaps={} watches={}",
        by("AND r.completeness != 'gap'"),
        by("AND r.completeness = 'gap'"),
        options.saves as i64 - scalar(&db, &distinct),
        scalar(&db, "SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'"),
        final_status.overflows,
        final_status.verify_runs,
        final_status.verify_gaps,
        final_status.watch_count
    );
    tracker.stop("bench");
    0
}

/// 実測 B: 追跡対象 `files` 件の作成を一気に行い、記録の数と `gap` の数を数える
/// (sub-06 で取りこぼしを再現した条件。CPU 負荷は `--load`)。
fn burst(dir: &Path, files: usize, load: usize) -> u8 {
    let Some(lab) = Lab::create(dir) else { return 1 };
    let Some(tracker) = start_with(&lab.project, &lab.data, lab_config()) else { return 1 };
    let status = tracker.wait_ready(WAIT_LIMIT);
    if status.phase != Phase::Watching {
        println!("baseline: 終わらなかった phase={:?}", status.phase);
        return 1;
    }
    let _load = (load > 0).then(|| CpuLoad::start(load));
    let started = Instant::now();
    fs::create_dir_all(lab.project.join("bulk")).expect("bulk dir");
    for i in 0..files {
        fs::write(lab.project.join(format!("bulk/f{i:04}.txt")), content(i, 1)).expect("write");
    }
    let written_ms = started.elapsed().as_millis();
    let db = lab.db();
    let distinct = "SELECT COUNT(DISTINCT change_id) FROM change_revisions";
    let deadline = Instant::now() + Duration::from_secs(300);
    while scalar(&db, distinct) < files as i64 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(100));
    }
    wait_two_scans(&tracker);
    let final_status = tracker.status();
    println!("burst: files={files} load={load} written_ms={written_ms} total_ms={}", started.elapsed().as_millis());
    println!(
        "result: recorded={} gap={} watcher_overflow_intervals={} overflows={} verify_runs={} verify_gaps={} watches={}",
        scalar(&db, distinct),
        scalar(&db, "SELECT COUNT(*) FROM change_revisions WHERE completeness = 'gap'"),
        scalar(&db, "SELECT COUNT(*) FROM coverage_gaps WHERE reason = 'watcher_overflow'"),
        final_status.overflows,
        final_status.verify_runs,
        final_status.verify_gaps,
        final_status.watch_count
    );
    tracker.stop("bench");
    0
}
