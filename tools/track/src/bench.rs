//! 計測用のサブコマンド (`mye_track bench ...`)。ベースラインと記録のスループットを、
//! 記録の置き場所を別ディレクトリへ向けて測る (追跡対象を書かない)。結果の読み方は
//! docs/ai-change-tracking/performance.md。

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use rusqlite::Connection;

use crate::collector::TrackerConfig;
use crate::db::now_ms;
use crate::startup::{open_project_with, ServiceState, StartupOptions};
use crate::watch::{Phase, Tracker};

/// 1 回の待ちの上限。これを超えたら「終わらなかった」として値を出す。
const WAIT_LIMIT: Duration = Duration::from_secs(45 * 60);

const USAGE: &str = "usage:\n  mye_track bench gen --dir <dir> --files <N>\n  mye_track bench baseline --project <root> --data <dir>\n  mye_track bench changes --project <root> --data <dir> [--counts 1,1000,32000]";

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
    let options = StartupOptions { data_dir: Some(data.to_path_buf()), ..StartupOptions::default() };
    let (store, report) = open_project_with(project, now_ms(), options);
    if report.state != Some(ServiceState::Recording) {
        eprintln!("記録を始められない: {:?}", report.state);
        return None;
    }
    Some(Tracker::start(project, store?, TrackerConfig::default()))
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
