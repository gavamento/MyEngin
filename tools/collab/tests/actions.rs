use mye_collab::ops::{dispatch, State};
use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicU64, Ordering};

static SERIAL: AtomicU64 = AtomicU64::new(0);

#[test]
fn ignore_detects_same_bytes_rewritten_on_disk() {
    let (dir, mut state) = fixture();
    write(&dir, ".gitignore", b"# keep\n");
    let p = preview(
        &mut state,
        json!({"action":"untrack_ignore","paths":["file.txt"]}),
    );
    std::thread::sleep(std::time::Duration::from_millis(30));
    write(&dir, ".gitignore", b"# keep\n");
    let error = dispatch(&mut state, "action_execute", &json!({"token":p["token"]})).unwrap_err();
    assert!(error.detail.contains(".gitignore changed"));
    assert!(git(&dir, &["ls-files"]).contains("file.txt"));
}

#[test]
fn rename_current_branch_and_reject_changed_target_tip() {
    let (dir, mut state) = fixture();
    let p = preview(
        &mut state,
        json!({"action":"branch_rename","name":"main","newName":"renamed"}),
    );
    assert_eq!(execute(&mut state, &p)["success"], true);
    assert_eq!(git(&dir, &["branch", "--show-current"]), "renamed");
    let root = git(&dir, &["rev-parse", "HEAD"]);
    git(&dir, &["branch", "topic"]);
    write(&dir, "file.txt", b"next\n");
    git(&dir, &["add", "."]);
    git(&dir, &["commit", "-qm", "next"]);
    let p = preview(
        &mut state,
        json!({"action":"branch_rename","name":"topic","newName":"moved"}),
    );
    git(&dir, &["branch", "-f", "topic", "HEAD"]);
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], false);
    assert_ne!(git(&dir, &["rev-parse", "topic"]), root);
}

#[test]
fn empty_cherry_pick_is_not_success_and_keeps_abort_available() {
    let (dir, mut state) = fixture();
    write(&dir, "file.txt", b"next\n");
    git(&dir, &["add", "."]);
    git(&dir, &["commit", "-qm", "next"]);
    let sha = git(&dir, &["rev-parse", "HEAD"]);
    let p = preview(&mut state, json!({"action":"cherry_pick","sha":sha}));
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], false);
    assert_eq!(r["operation"], "cherry-pick");
    assert_eq!(
        dispatch(&mut state, "merge_abort", &json!({})).unwrap()["success"],
        true
    );
    assert_eq!(std::fs::read(dir.join("file.txt")).unwrap(), b"next\n");
}

#[test]
fn preview_does_not_write_ignore_or_index_and_repeated_ignore_is_idempotent() {
    let (dir, mut state) = fixture();
    let index = std::fs::read(dir.join(".git/index")).unwrap();
    let _cancelled = preview(
        &mut state,
        json!({"action":"untrack_ignore","paths":["file.txt"]}),
    );
    assert!(!dir.join(".gitignore").exists());
    // Git status may refresh index stat data, so compare the staged tree as well as local contents.
    assert!(!index.is_empty());
    assert!(git(&dir, &["diff", "--cached", "--name-only"]).is_empty());
    let p = preview(&mut state, json!({"action":"ignore","paths":["file.txt"]}));
    assert_eq!(execute(&mut state, &p)["success"], true);
    let bytes = std::fs::read(dir.join(".gitignore")).unwrap();
    let p = preview(&mut state, json!({"action":"ignore","paths":["file.txt"]}));
    assert_eq!(execute(&mut state, &p)["success"], true);
    assert_eq!(std::fs::read(dir.join(".gitignore")).unwrap(), bytes);
    assert!(git(&dir, &["ls-files"]).contains("file.txt"));
}

#[test]
fn overriding_negation_fails_before_untracking() {
    let (dir, mut state) = fixture();
    write(&dir, ".gitignore", b"/file.txt\n!/file.txt\n");
    let p = preview(
        &mut state,
        json!({"action":"untrack_ignore","paths":["file.txt"]}),
    );
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], false);
    assert_eq!(r["completed"], json!([]));
    assert!(git(&dir, &["ls-files"]).contains("file.txt"));
    assert_eq!(
        std::fs::read(dir.join(".gitignore")).unwrap(),
        b"/file.txt\n!/file.txt\n"
    );
}

// Retain fixtures for diagnosis. The caller sets TEMP to the authorized test directory.
fn fixture() -> (PathBuf, State) {
    let dir = std::env::temp_dir().join(format!(
        "context_{}_{}",
        std::process::id(),
        SERIAL.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir_all(&dir).unwrap();
    git(&dir, &["init", "-q", "-b", "main"]);
    // The assertions below compare working-tree bytes. A global core.autocrlf=true
    // (the default on GitHub's Windows runners) would make revert / abort check files out as CRLF.
    git(&dir, &["config", "core.autocrlf", "false"]);
    git(&dir, &["config", "user.name", "Context Test"]);
    git(&dir, &["config", "user.email", "context@example.invalid"]);
    write(&dir, "file.txt", b"original\n");
    write(&dir, "file.txt.meta", b"metadata\n");
    git(&dir, &["add", "."]);
    git(&dir, &["commit", "-qm", "initial"]);
    let state = State::new(&dir);
    (dir, state)
}

fn git(dir: &Path, args: &[&str]) -> String {
    let out = Command::new("git")
        .args(args)
        .current_dir(dir)
        .output()
        .unwrap();
    assert!(
        out.status.success(),
        "{args:?}: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8_lossy(&out.stdout).trim().to_string()
}

fn write(dir: &Path, path: &str, bytes: &[u8]) {
    let path = dir.join(path);
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    std::fs::write(path, bytes).unwrap();
}

fn preview(state: &mut State, args: Value) -> Value {
    dispatch(state, "action_preview", &args).unwrap()
}

fn execute(state: &mut State, preview: &Value) -> Value {
    dispatch(state, "action_execute", &json!({"token": preview["token"]})).unwrap()
}

#[test]
fn untrack_preserves_primary_and_sidecar_and_is_single_use() {
    let (dir, mut state) = fixture();
    let p = preview(
        &mut state,
        json!({"action":"untrack", "paths":["file.txt"]}),
    );
    assert_eq!(p["paths"], json!(["file.txt", "file.txt.meta"]));
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], true, "{r}");
    assert_eq!(std::fs::read(dir.join("file.txt")).unwrap(), b"original\n");
    assert_eq!(
        std::fs::read(dir.join("file.txt.meta")).unwrap(),
        b"metadata\n"
    );
    assert!(git(&dir, &["ls-files"]).is_empty());
    assert!(dispatch(&mut state, "action_execute", &json!({"token":p["token"]})).is_err());
}

#[test]
fn confirmation_detects_same_status_content_change() {
    let (dir, mut state) = fixture();
    write(&dir, "file.txt", b"changed1\n");
    let p = preview(
        &mut state,
        json!({"action":"untrack", "paths":["file.txt"]}),
    );
    write(&dir, "file.txt", b"changed2\n");
    let r = dispatch(&mut state, "action_execute", &json!({"token":p["token"]}));
    assert!(r.is_err());
    assert!(git(&dir, &["ls-files"]).contains("file.txt"));
}

#[test]
fn ignore_detects_disk_change_since_confirmation_including_creation() {
    let (dir, mut state) = fixture();
    let p = preview(
        &mut state,
        json!({"action":"untrack_ignore", "paths":["file.txt"]}),
    );
    write(&dir, ".gitignore", b"# external edit\n");
    assert!(dispatch(&mut state, "action_execute", &json!({"token":p["token"]})).is_err());
    assert!(git(&dir, &["ls-files"]).contains("file.txt"));
    let p = preview(
        &mut state,
        json!({"action":"untrack_ignore", "paths":["file.txt"]}),
    );
    write(&dir, ".gitignore", b"# another edit!\n");
    assert!(dispatch(&mut state, "action_execute", &json!({"token":p["token"]})).is_err());
    assert_eq!(
        std::fs::read(dir.join(".gitignore")).unwrap(),
        b"# another edit!\n"
    );
}

#[test]
fn ignore_preserves_bom_crlf_and_escapes_literal_paths() {
    let (dir, mut state) = fixture();
    let original = b"\xef\xbb\xbf# existing\r\n!keep.txt";
    write(&dir, ".gitignore", original);
    for path in [
        "日本語 [1].txt",
        "-leading.txt",
        "#hash.txt",
        "!bang.txt",
        "a*b.txt",
    ] {
        // '*' cannot be created on Windows; the escaping itself is checked in a unit test.
        if path.contains('*') {
            continue;
        }
        write(&dir, path, b"contents");
        let p = preview(&mut state, json!({"action":"ignore", "paths":[path]}));
        let r = execute(&mut state, &p);
        assert_eq!(r["success"], true, "{r}");
        assert!(std::fs::read(dir.join(".gitignore"))
            .unwrap()
            .starts_with(original));
        git(&dir, &["check-ignore", "--no-index", "--", path]);
    }
}

#[test]
fn unsafe_paths_and_gitignore_itself_are_rejected() {
    let (_, mut state) = fixture();
    for p in [
        "../file",
        "/file",
        "C:/file",
        "x\\y",
        ".git/config",
        "x/../file",
    ] {
        assert!(
            dispatch(
                &mut state,
                "action_preview",
                &json!({"action":"ignore","paths":[p]})
            )
            .is_err(),
            "{p}"
        );
    }
    assert!(dispatch(
        &mut state,
        "action_preview",
        &json!({"action":"untrack","paths":[".gitignore"]})
    )
    .is_err());
}

#[test]
fn root_commit_diff_and_reset_preserve_working_files() {
    let (dir, mut state) = fixture();
    let root = git(&dir, &["rev-parse", "HEAD"]);
    let diff = dispatch(&mut state, "commit_diff", &json!({"sha":root})).unwrap();
    assert!(diff["text"].as_str().unwrap().contains("original"));
    write(&dir, "file.txt", b"second\n");
    git(&dir, &["add", "."]);
    git(&dir, &["commit", "-qm", "second"]);
    let p = preview(&mut state, json!({"action":"soft","sha":root}));
    assert_eq!(execute(&mut state, &p)["success"], true);
    assert_eq!(git(&dir, &["rev-parse", "HEAD"]), root);
    assert_eq!(std::fs::read(dir.join("file.txt")).unwrap(), b"second\n");
    assert!(git(&dir, &["diff", "--cached", "--name-only"]).contains("file.txt"));
}

#[test]
fn dirty_history_and_staged_untrack_conflict_are_rejected() {
    let (dir, mut state) = fixture();
    let sha = git(&dir, &["rev-parse", "HEAD"]);
    write(&dir, "untracked.txt", b"new");
    assert!(dispatch(
        &mut state,
        "action_preview",
        &json!({"action":"mixed","sha":sha})
    )
    .is_err());
    write(&dir, "file.txt", b"index\n");
    git(&dir, &["add", "file.txt"]);
    write(&dir, "file.txt", b"worktree\n");
    let p = preview(&mut state, json!({"action":"untrack","paths":["file.txt"]}));
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], false);
    assert!(git(&dir, &["ls-files"]).contains("file.txt"));
}

#[test]
fn branch_operations_validate_tip_and_keep_worktree() {
    let (dir, mut state) = fixture();
    let sha = git(&dir, &["rev-parse", "HEAD"]);
    let p = preview(
        &mut state,
        json!({"action":"branch_create","name":"topic","sha":sha}),
    );
    assert_eq!(execute(&mut state, &p)["success"], true);
    let p = preview(
        &mut state,
        json!({"action":"branch_rename","name":"topic","newName":"renamed"}),
    );
    assert_eq!(execute(&mut state, &p)["success"], true);
    assert!(dispatch(
        &mut state,
        "action_preview",
        &json!({"action":"branch_delete","name":"main"})
    )
    .is_err());
    let p = preview(
        &mut state,
        json!({"action":"branch_delete","name":"renamed"}),
    );
    assert_eq!(execute(&mut state, &p)["success"], true);
    assert_eq!(std::fs::read(dir.join("file.txt")).unwrap(), b"original\n");
}

#[test]
fn mixed_reset_keeps_contents_and_clears_index_changes() {
    let (dir, mut state) = fixture();
    let root = git(&dir, &["rev-parse", "HEAD"]);
    write(&dir, "file.txt", b"second\n");
    git(&dir, &["add", "."]);
    git(&dir, &["commit", "-qm", "second"]);
    let p = preview(&mut state, json!({"action":"mixed","sha":root}));
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], true, "{r}");
    assert_eq!(std::fs::read(dir.join("file.txt")).unwrap(), b"second\n");
    assert!(git(&dir, &["diff", "--cached", "--name-only"]).is_empty());
    assert!(git(&dir, &["diff", "--name-only"]).contains("file.txt"));
    assert_eq!(r["names"], json!([]));
}

#[test]
fn revert_and_cherry_pick_modify_files_and_report_changes() {
    let (dir, mut state) = fixture();
    write(&dir, "file.txt", b"second\n");
    git(&dir, &["add", "."]);
    git(&dir, &["commit", "-qm", "second"]);
    let sha = git(&dir, &["rev-parse", "HEAD"]);
    let p = preview(&mut state, json!({"action":"commit_revert","sha":sha}));
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], true, "{r}");
    assert_eq!(std::fs::read(dir.join("file.txt")).unwrap(), b"original\n");
    assert_eq!(r["names"], json!([{"path":"file.txt","status":"M"}]));
    let p = preview(&mut state, json!({"action":"cherry_pick","sha":sha}));
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], true, "{r}");
    assert_eq!(std::fs::read(dir.join("file.txt")).unwrap(), b"second\n");
}

fn conflict_fixture() -> (PathBuf, State, String) {
    let (dir, state) = fixture();
    let root = git(&dir, &["rev-parse", "HEAD"]);
    write(&dir, "file.txt", b"topic\n");
    git(&dir, &["add", "."]);
    git(&dir, &["commit", "-qm", "topic"]);
    let sha = git(&dir, &["rev-parse", "HEAD"]);
    git(&dir, &["checkout", "-qb", "other", &root]);
    write(&dir, "file.txt", b"other\n");
    git(&dir, &["add", "."]);
    git(&dir, &["commit", "-qm", "other"]);
    (dir, state, sha)
}

#[test]
fn cherry_pick_conflict_survives_restart_and_abort_restores_contents() {
    let (dir, mut state, sha) = conflict_fixture();
    let p = preview(&mut state, json!({"action":"cherry_pick","sha":sha}));
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], false);
    assert_eq!(r["status"]["operation"], "cherry-pick");
    assert!(!r["names"].as_array().unwrap().is_empty());
    let mut restarted = State::new(&dir);
    assert_eq!(
        dispatch(&mut restarted, "status", &json!({})).unwrap()["operation"],
        "cherry-pick"
    );
    let r = dispatch(&mut restarted, "merge_abort", &json!({})).unwrap();
    assert_eq!(r["success"], true, "{r}");
    assert_eq!(r["status"]["operation"], "");
    assert_eq!(std::fs::read(dir.join("file.txt")).unwrap(), b"other\n");
}

#[test]
fn cherry_pick_continue_reports_failure_then_resolves_without_editor() {
    let (dir, mut state, sha) = conflict_fixture();
    let p = preview(&mut state, json!({"action":"cherry_pick","sha":sha}));
    assert_eq!(execute(&mut state, &p)["success"], false);
    let r = dispatch(&mut state, "continue", &json!({})).unwrap();
    assert_eq!(r["success"], false);
    assert_eq!(r["status"]["operation"], "cherry-pick");
    write(&dir, "file.txt", b"resolved\n");
    dispatch(&mut state, "stage", &json!({"paths":["file.txt"]})).unwrap();
    let r = dispatch(&mut state, "continue", &json!({})).unwrap();
    assert_eq!(r["success"], true, "{r}");
    assert_eq!(r["status"]["operation"], "");
    assert_eq!(std::fs::read(dir.join("file.txt")).unwrap(), b"resolved\n");
}

#[test]
fn folder_targets_pair_terrain_and_folder_metadata_without_generating_files() {
    let (dir, mut state) = fixture();
    for p in [
        "assets.meta",
        "assets/ground.terrain.json",
        "assets/ground.terrain.json.meta",
        "assets/ground.terrain.edit",
        "assets/ground.terrain.edit.meta",
    ] {
        write(&dir, p, b"asset");
    }
    git(&dir, &["add", "."]);
    git(&dir, &["commit", "-qm", "assets"]);
    write(&dir, "assets/new.txt", b"untracked");
    let p = preview(&mut state, json!({"action":"untrack","paths":["assets"]}));
    assert_eq!(p["paths"].as_array().unwrap().len(), 5);
    assert!(p["excluded"]
        .as_array()
        .unwrap()
        .iter()
        .any(|r| r["path"] == "assets/new.txt"));
    let r = execute(&mut state, &p);
    assert_eq!(r["success"], true, "{r}");
    assert!(!dir.join("assets/new.txt.meta").exists());
    assert_eq!(std::fs::read(dir.join("assets.meta")).unwrap(), b"asset");
}
