//====================================================================================
//                          verify.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          verify_job (replay_verify.bat のジョブ起動とログの構造化)
//====================================================================================

use serde_json::{json, Value};
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::Instant;

// replay_verify.bat の `:job_*` と `-Jobs` 列の順。規則 15-e (tools\check_rules.ps1) が三者一致を見る。
// 1 行 1 名で書くこと (15-e はこのブロックを行単位で読む)。
pub const JOBS: [&str; 21] = [
    "demo",
    "parts",
    "flow",
    "mp",
    "physics",
    "joints",
    "acoustic",
    "ui",
    "fracture",
    "nav",
    "perception",
    "bt",
    "anim",
    "compute",
    "jobsab",
    "computeab",
    "ttdebug",
    "ttrelease",
    "whatifdebug",
    "whatifrelease",
    "rules",
];

pub const TICKS_DEFAULT: i64 = 600;
pub const TICKS_MIN: i64 = 1;
pub const TICKS_MAX: i64 = 100_000;

// Editor 1 本で約 2.2 GB 使う (run_parallel.ps1 と同じ見積もり)
const MAX_RUNNING: usize = 2;
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const LOG_DIR: &str = "cache\\mcp_logs";
// ビルド済み前提。replay_verify.bat のジョブは Debug / Release の両方を呼ぶ
const REQUIRED_EXES: [&str; 2] = ["bin\\x64\\Debug\\Editor.exe", "bin\\x64\\Release\\Editor.exe"];
// rules は check_rules.ps1 を pwsh で回すだけで Editor.exe を使わないので前提を課さない
const JOB_WITHOUT_EXE: &str = "rules";

const ERRORS_MAX: usize = 20;
const STAGE_LINES_MAX: usize = 50;
const LINE_CHARS_MAX: usize = 300;
const HASH_MISMATCH_MARK: &str = "HASH MISMATCH at tick ";
const FIELD_DIFF_HINT: &str =
    "field-level diff is not available per job; run tools\\replay_verify.bat (full run) to get it";

enum State {
    Running(Child),
    Done { exit_code: Option<i32>, elapsed_sec: u64, summary: Value },
}

struct Job {
    id: String,
    name: &'static str,
    log_abs: PathBuf,
    started: Instant,
    state: State,
}

// プロセス内のジョブ表。jobId は起動順の連番 (jb_01 ...)
#[derive(Default)]
pub struct JobTable {
    jobs: Vec<Job>,
}

// 子に渡す cmd の引数 (run_parallel.ps1 と同じ形)。log はクォートして渡す
pub fn child_command_line(name: &str, log: &Path) -> String {
    format!("/c chcp 437 >nul & call tools\\replay_verify.bat --job {name} > \"{}\" 2>&1", log.display())
}

// 子の cmd.exe。stdio は全部 null (MCP の stdout を汚さない。出力は cmd の > でログへ)
fn build_command(root: &Path, name: &str, ticks: i64, log: &Path) -> Command {
    let mut cmd = Command::new("cmd.exe");
    cmd.raw_arg(child_command_line(name, log))
        .creation_flags(CREATE_NO_WINDOW)
        .current_dir(root)
        .env("MYE_RV_TICKS", ticks.to_string())
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    cmd
}

// 同名走行中と同時数の上限を弾く。running は走行中ジョブの名前
fn check_admission(running: &[&str], name: &str) -> Result<(), String> {
    if running.contains(&name) {
        return Err(format!("job '{name}' is already running; poll it or wait for it to finish"));
    }
    if running.len() >= MAX_RUNNING {
        return Err(format!(
            "busy: {} jobs are already running ({}); wait for one to finish",
            running.len(),
            running.join(", ")
        ));
    }
    Ok(())
}

impl JobTable {
    // op=start。検証はすべて起動より前。enum 外の名前はシェルへ届かない
    pub fn start(&mut self, root: Option<&Path>, job: &str, ticks: Option<i64>) -> Result<Value, String> {
        let name = JOBS
            .iter()
            .copied()
            .find(|n| *n == job)
            .ok_or_else(|| format!("unknown job: one of {}", JOBS.join(", ")))?;
        let ticks = ticks.unwrap_or(TICKS_DEFAULT);
        if !(TICKS_MIN..=TICKS_MAX).contains(&ticks) {
            return Err(format!("ticks must be {TICKS_MIN}-{TICKS_MAX} (got {ticks})"));
        }
        let root = root.ok_or("repo root not found (MyEngine.sln); start the server with --root <repo>")?;
        if name != JOB_WITHOUT_EXE {
            for exe in REQUIRED_EXES {
                if !root.join(exe).is_file() {
                    return Err(format!("{exe} not found; build Debug and Release first (verify_job does not build)"));
                }
            }
        }
        self.refresh();
        let running: Vec<&str> = self.running_names();
        check_admission(&running, name)?;

        let log_abs = root.join(LOG_DIR).join(format!("{name}.log"));
        std::fs::create_dir_all(root.join(LOG_DIR)).map_err(|e| format!("cannot create {LOG_DIR}: {e}"))?;
        let cmd = build_command(root, name, ticks, &log_abs);
        let id = self.spawn(name, log_abs, cmd)?;
        Ok(json!({"jobId": id, "job": name, "ticks": ticks, "logPath": log_rel(name)}))
    }

    // op=poll
    pub fn poll(&mut self, id: &str) -> Result<Value, String> {
        self.refresh();
        let job = self.jobs.iter().find(|j| j.id == id).ok_or_else(|| {
            format!("unknown jobId: {id} (the job table lives in this server process only)")
        })?;
        Ok(match &job.state {
            State::Running(_) => json!({"state": "running", "elapsedSec": job.started.elapsed().as_secs()}),
            State::Done { exit_code, elapsed_sec, summary } => {
                json!({"state": "done", "exitCode": exit_code, "elapsedSec": elapsed_sec, "summary": summary})
            }
        })
    }

    fn spawn(&mut self, name: &'static str, log_abs: PathBuf, mut cmd: Command) -> Result<String, String> {
        let child = cmd.spawn().map_err(|e| format!("cannot start cmd.exe: {e}"))?;
        let id = format!("jb_{:02}", self.jobs.len() + 1);
        self.jobs.push(Job { id: id.clone(), name, log_abs, started: Instant::now(), state: State::Running(child) });
        Ok(id)
    }

    fn running_names(&self) -> Vec<&str> {
        self.jobs.iter().filter(|j| matches!(j.state, State::Running(_))).map(|j| j.name).collect()
    }

    // 終わった子を Done へ進め、その時点でログを読んで summary を固定する。
    // 同名ジョブの再起動でログが上書きされる前に必ずここを通る (start が先に呼ぶ)
    fn refresh(&mut self) {
        for job in &mut self.jobs {
            let State::Running(child) = &mut job.state else { continue };
            let exit_code = match child.try_wait() {
                Ok(None) => continue,
                Ok(Some(status)) => status.code(),
                Err(_) => None,
            };
            let log = std::fs::read(&job.log_abs).ok();
            let summary = summarize(log.as_deref(), exit_code, job.name);
            job.state = State::Done { exit_code, elapsed_sec: job.started.elapsed().as_secs(), summary };
        }
    }
}

fn log_rel(name: &str) -> String {
    format!("{LOG_DIR}\\{name}.log")
}

// ログ全体を返さず、落ちた段・最初に割れた tick・診断行だけを取り出す。
// ログにはエンジンの出力 (UTF-8 として不正なバイトがありうる) が混ざるので lossy で読む。
// log が None (ファイルが無い) なら FAIL + noLog。
fn summarize(log: Option<&[u8]>, exit_code: Option<i32>, name: &str) -> Value {
    let text = log.map(String::from_utf8_lossy).unwrap_or_default();
    let mut failed_stages: Vec<String> = Vec::new();
    let mut diagnostic: Vec<String> = Vec::new();
    let mut errors: Vec<String> = Vec::new();
    let mut first_mismatch_tick: Option<u64> = None;
    for raw in text.lines() {
        let line = raw.trim_end();
        if first_mismatch_tick.is_none() {
            if let Some(pos) = line.find(HASH_MISMATCH_MARK) {
                let digits: String =
                    line[pos + HASH_MISMATCH_MARK.len()..].chars().take_while(char::is_ascii_digit).collect();
                first_mismatch_tick = digits.parse().ok();
            }
        }
        if line.starts_with("[FAIL] ") {
            push_capped(&mut failed_stages, line, STAGE_LINES_MAX);
        } else if line.starts_with("[diag] ") {
            push_capped(&mut diagnostic, line, STAGE_LINES_MAX);
        } else if line.starts_with("[ERROR]") || line.starts_with("ERROR ") {
            // "ERROR " は rules ジョブ (check_rules.ps1) の出力
            push_capped(&mut errors, line, ERRORS_MAX);
        }
    }
    let no_log = log.is_none();
    let passed = exit_code == Some(0) && !no_log;
    json!({
        "result": if passed { "PASS" } else { "FAIL" },
        "stage": failed_stages.first(),
        "failedStages": failed_stages,
        "firstMismatchTick": first_mismatch_tick,
        "diagnostic": diagnostic,
        "errors": errors,
        "fieldDiff": Value::Null,
        "hint": FIELD_DIFF_HINT,
        "logPath": log_rel(name),
        "logBytes": log.map_or(0, <[u8]>::len),
        "noLog": no_log,
    })
}

fn push_capped(out: &mut Vec<String>, line: &str, max: usize) {
    if out.len() < max {
        out.push(line.chars().take(LINE_CHARS_MAX).collect());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixed_log(file: &str) -> Vec<u8> {
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("..\\..\\tests\\mcp\\logs").join(file);
        std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
    }

    #[test]
    fn ジョブ表は_21_名で重複がない() {
        assert_eq!(JOBS.len(), 21);
        let mut sorted = JOBS.to_vec();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(sorted.len(), 21);
        assert!(JOBS.iter().all(|n| n.chars().all(|c| c.is_ascii_lowercase())));
    }

    #[test]
    fn 子のコマンド文字列は_run_parallel_と同じ形() {
        let log = Path::new("C:\\repo\\cache\\mcp_logs\\demo.log");
        assert_eq!(
            child_command_line("demo", log),
            "/c chcp 437 >nul & call tools\\replay_verify.bat --job demo > \"C:\\repo\\cache\\mcp_logs\\demo.log\" 2>&1"
        );
    }

    #[test]
    fn 子はカレントがルートで_ticks_を環境変数に載せる() {
        let root = Path::new("C:\\repo");
        let cmd = build_command(root, "demo", 60, Path::new("C:\\repo\\cache\\mcp_logs\\demo.log"));
        assert_eq!(cmd.get_program(), "cmd.exe");
        assert_eq!(cmd.get_current_dir(), Some(root));
        let envs: Vec<_> = cmd.get_envs().collect();
        assert_eq!(envs.len(), 1);
        assert_eq!(envs[0].0, "MYE_RV_TICKS");
        assert_eq!(envs[0].1.and_then(|v| v.to_str()), Some("60"));
        // CREATE_NO_WINDOW と stdio null は Command から読み戻せない。定数と build_command の本文で担保する
        assert_eq!(CREATE_NO_WINDOW, 0x0800_0000);
    }

    #[test]
    fn enum_外のジョブと_ticks_範囲外は起動前に弾く() {
        let mut table = JobTable::default();
        let root = Path::new("C:\\nonexistent");
        for bad in ["demo & calc", "../x", "", "Demo", "demo ", "demo\n"] {
            let e = table.start(Some(root), bad, None).unwrap_err();
            assert!(e.starts_with("unknown job"), "{bad:?}: {e}");
        }
        for bad in [0, -1, TICKS_MAX + 1] {
            let e = table.start(Some(root), "demo", Some(bad)).unwrap_err();
            assert!(e.starts_with("ticks must be"), "{bad}: {e}");
        }
        assert!(table.jobs.is_empty());
    }

    #[test]
    fn 前提_exe_が無ければ弾く() {
        let mut table = JobTable::default();
        let root = Path::new(env!("CARGO_MANIFEST_DIR")); // Editor.exe の無いディレクトリ
        let e = table.start(Some(root), "demo", None).unwrap_err();
        assert!(e.contains("bin\\x64\\Debug\\Editor.exe not found"), "{e}");
        assert!(table.start(None, "demo", None).unwrap_err().contains("repo root not found"));
        assert!(table.jobs.is_empty());
    }

    #[test]
    fn 同名走行中と_3_本目は弾く() {
        assert!(check_admission(&[], "demo").is_ok());
        assert!(check_admission(&["parts"], "demo").is_ok());
        assert!(check_admission(&["demo"], "demo").unwrap_err().contains("already running"));
        assert!(check_admission(&["parts", "demo"], "flow").unwrap_err().starts_with("busy"));
    }

    #[test]
    fn 起動から_poll_で_done_まで進む() {
        let mut table = JobTable::default();
        let mut cmd = Command::new("cmd.exe");
        cmd.raw_arg("/c exit 3").creation_flags(CREATE_NO_WINDOW).stdin(Stdio::null()).stdout(Stdio::null());
        let id = table.spawn("demo", PathBuf::from("C:\\nonexistent\\demo.log"), cmd).unwrap();
        assert_eq!(id, "jb_01");
        let mut done = Value::Null;
        for _ in 0..200 {
            let v = table.poll(&id).unwrap();
            if v["state"] == "done" {
                done = v;
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        assert_eq!(done["exitCode"], 3);
        assert_eq!(done["summary"]["result"], "FAIL");
        assert_eq!(done["summary"]["noLog"], true);
        assert!(table.poll("jb_99").unwrap_err().starts_with("unknown jobId"));
    }

    #[test]
    fn ログなしは_fail_と_nolog() {
        let s = summarize(None, Some(0), "demo");
        assert_eq!(s["result"], "FAIL");
        assert_eq!(s["noLog"], true);
        assert_eq!(s["logBytes"], 0);
        assert_eq!(s["stage"], Value::Null);
    }

    #[test]
    fn pass_ログ() {
        let log = fixed_log("pass.log");
        let s = summarize(Some(&log), Some(0), "demo");
        assert_eq!(s["result"], "PASS");
        assert_eq!(s["failedStages"], json!([]));
        assert_eq!(s["stage"], Value::Null);
        assert_eq!(s["firstMismatchTick"], Value::Null);
        assert_eq!(s["diagnostic"], json!([]));
        assert_eq!(s["fieldDiff"], Value::Null);
        assert_eq!(s["logPath"], "cache\\mcp_logs\\demo.log");
        assert_eq!(s["logBytes"], log.len());
        assert_eq!(s["noLog"], false);
    }

    #[test]
    fn stress_失敗で_plain_も失敗() {
        let log = fixed_log("stress_fail_plain_fail.log");
        let s = summarize(Some(&log), Some(1), "physics");
        assert_eq!(s["result"], "FAIL");
        assert_eq!(s["stage"], "[FAIL] Debug verify with snapshot stress: cache\\golden_physics.rep");
        assert_eq!(s["failedStages"].as_array().unwrap().len(), 1);
        assert_eq!(s["firstMismatchTick"], 241);
        let diag = s["diagnostic"].as_array().unwrap();
        assert_eq!(diag.len(), 2);
        assert!(diag[1].as_str().unwrap().contains("base determinism is broken"));
        assert_eq!(s["errors"].as_array().unwrap().len(), 5);
    }

    #[test]
    fn stress_失敗で_plain_は成功() {
        let log = fixed_log("stress_fail_plain_ok.log");
        let s = summarize(Some(&log), Some(1), "joints");
        assert_eq!(s["result"], "FAIL");
        assert_eq!(s["firstMismatchTick"], 74);
        let diag = s["diagnostic"].as_array().unwrap();
        assert!(diag[1].as_str().unwrap().contains("snapshot restore asymmetry"));
    }

    #[test]
    fn release_失敗() {
        let log = fixed_log("release_fail.log");
        let s = summarize(Some(&log), Some(1), "demo");
        assert_eq!(s["stage"], "[FAIL] Release verify: cache\\golden.rep");
        assert_eq!(s["firstMismatchTick"], 389);
        assert_eq!(s["diagnostic"], json!([]));
    }

    #[test]
    fn 不正な_utf8_と長い行と_exit_0_でも壊れない() {
        let mut log: Vec<u8> = b"[ERROR] bad \xFF\xFE byte\r\n".to_vec();
        log.extend_from_slice(format!("[FAIL] {}\r\n", "x".repeat(1000)).as_bytes());
        for i in 0..100 {
            log.extend_from_slice(format!("[ERROR] e{i}\n").as_bytes());
        }
        let s = summarize(Some(&log), Some(0), "demo");
        assert_eq!(s["result"], "PASS");
        assert_eq!(s["errors"].as_array().unwrap().len(), ERRORS_MAX);
        assert_eq!(s["stage"].as_str().unwrap().chars().count(), LINE_CHARS_MAX);
        assert!(s["errors"][0].as_str().unwrap().contains('\u{FFFD}'));
    }

    #[test]
    fn 負の終了コードは_fail() {
        let s = summarize(Some(b"=== x ===\n"), Some(-1073741819), "demo");
        assert_eq!(s["result"], "FAIL");
        let s = summarize(Some(b"=== x ===\n"), None, "demo");
        assert_eq!(s["result"], "FAIL");
    }
}
