use super::{State, toplevel};
use crate::{git, protocol::{code, ErrorBody}};
use serde_json::{json, Value};

fn bad(detail: &str) -> ErrorBody { ErrorBody::new(code::BAD_REQUEST, detail) }

fn config(state: &State, key: &str) -> String {
    git::run(&state.root, &["config", "--get", key])
        .ok().filter(|o| o.success()).map(|o| o.stdout_text()).unwrap_or_default()
}

fn require_root(state: &State) -> Result<(), ErrorBody> {
    let root = std::fs::canonicalize(&state.root).map_err(|_| bad("project directory is unavailable"))?;
    let top = toplevel(&state.root)?;
    let top = std::fs::canonicalize(top).map_err(|_| bad("repository directory is unavailable"))?;
    if root != top { return Err(ErrorBody::new(code::TOPLEVEL_MISMATCH, "project is inside another repository")); }
    Ok(())
}

fn valid_url(url: &str) -> bool {
    let Some(path) = url.strip_prefix("https://github.com/") else { return false; };
    let parts: Vec<_> = path.split('/').collect();
    parts.len() == 2 && parts.iter().all(|p| !p.is_empty() && *p != "." && *p != ".."
        && p.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)))
        && parts[1] != ".git"
}

fn run(state: &State, args: &[&str], seconds: u64, generation: u64, interactive: bool) -> Result<git::GitOutput, ErrorBody> {
    if generation == 0 { return Err(bad("missing setup generation")); }
    let out = git::run_setup(&state.root, args, seconds, &state.cancel_setup, generation, interactive)?;
    if !out.success() { return Err(git::classify_error(&out)); }
    Ok(out)
}

pub fn dispatch(state: &mut State, op: &str, args: &Value) -> Result<Value, ErrorBody> {
    let generation = args["generation"].as_u64().unwrap_or(0);
    match op {
        "setup_state" => {
            let gcm = git::run(&state.root, &["credential-manager", "--version"])
                .ok().filter(|o| o.success()).map(|o| o.stdout_text()).unwrap_or_default();
            let accounts = if gcm.is_empty() { String::new() } else {
                git::run_setup(&state.root, &["credential-manager", "github", "list", "--no-ui"], 10,
                    &state.cancel_setup, u64::MAX, false)
                    .ok().filter(|o| o.success()).map(|o| o.stdout_text()).unwrap_or_default()
            };
            let accounts: Vec<_> = accounts.lines().map(str::trim).filter(|s| !s.is_empty()).collect();
            Ok(json!({"root": std::fs::canonicalize(&state.root).unwrap_or(state.root.clone()),
                "git": git::version(&state.root)?, "gcm": gcm, "accounts": accounts,
                "name": config(state, "user.name"), "email": config(state, "user.email"),
                "account": config(state, "credential.https://github.com.username"),
                "origin": config(state, "remote.origin.url"), "pushUrl": config(state, "remote.origin.pushurl")}))
        }
        "repo_init" => {
            if state.root.join(".git").exists() { return Err(bad("existing Git metadata must not be overwritten")); }
            if toplevel(&state.root).is_ok() { return Err(bad("repository already exists, or project is inside another repository")); }
            let branch = args["branch"].as_str().unwrap_or("main");
            run(state, &["check-ref-format", "--branch", branch], 10, generation, false)?;
            run(state, &["init"], 10, generation, false)?;
            run(state, &["symbolic-ref", "HEAD", &format!("refs/heads/{branch}")], 10, generation, false)?;
            state.git_dir = None;
            Ok(json!({}))
        }
        "identity_save" => {
            require_root(state)?;
            let name = args["name"].as_str().unwrap_or("").trim();
            let email = args["email"].as_str().unwrap_or("").trim();
            let account = args["account"].as_str().unwrap_or("");
            if [name, email].iter().any(|s| s.is_empty() || s.chars().any(char::is_control))
                || (!account.is_empty() && !account.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')) {
                return Err(bad("invalid identity or GitHub account"));
            }
            run(state, &["config", "--local", "user.name", name], 10, generation, false)?;
            run(state, &["config", "--local", "user.email", email], 10, generation, false)?;
            if !account.is_empty() {
                run(state, &["config", "--local", "credential.https://github.com.username", account], 10, generation, false)?;
                run(state, &["config", "--local", "credential.https://github.com.helper", ""], 10, generation, false)?;
                run(state, &["config", "--local", "--add", "credential.https://github.com.helper", "manager"], 10, generation, false)?;
            }
            Ok(json!({}))
        }
        "github_login" => {
            if generation == 0 { return Err(bad("missing setup generation")); }
            // Never return GCM output: authentication diagnostics may contain secrets.
            let out = git::run_setup(&state.root, &["credential-manager", "github", "login", "--browser"],
                300, &state.cancel_setup, generation, true)?;
            if !out.success() { return Err(ErrorBody::new(code::AUTH_FAILED, "GitHub browser authentication failed")); }
            Ok(json!({}))
        }
        "remote_connect" => {
            require_root(state)?;
            let url = args["url"].as_str().unwrap_or("");
            if !valid_url(url) { return Err(bad("expected https://github.com/owner/repository")); }
            let old = config(state, "remote.origin.url");
            if old != args["expectedOrigin"].as_str().unwrap_or("") { return Err(bad("origin changed; refresh settings")); }
            if !config(state, "remote.origin.pushurl").is_empty() { return Err(bad("origin has an explicit push URL")); }
            let refs = run(state, &["ls-remote", "--", url], 30, generation, false)?;
            if config(state, "remote.origin.url") != old || !config(state, "remote.origin.pushurl").is_empty() {
                return Err(bad("origin changed during connection check; refresh settings"));
            }
            if old != url {
                let verb = if old.is_empty() { "add" } else { "set-url" };
                run(state, &["remote", verb, "origin", url], 10, generation, false)?;
            }
            Ok(json!({"hasHistory": !refs.stdout.is_empty()}))
        }
        _ => Err(bad("unknown setup operation")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn github_urls() {
        for url in ["https://github.com/owner/repo", "https://github.com/owner/repo.git"] { assert!(valid_url(url)); }
        for url in ["", "https://github.com/owner", "https://github.com/a/b/c", "https://user:token@github.com/a/b", "https://github.com/a/b?x", "https://github.com/a/..", "git@github.com:a/b"] { assert!(!valid_url(url)); }
    }

    #[test]
    fn setup_preserves_files_and_rejects_parent_repository() {
        let root = std::env::temp_dir().join(format!("mye_setup_日本語 space_{}_{}", std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()));
        std::fs::create_dir_all(&root).unwrap();
        std::fs::write(root.join(".gitignore"), "# preserve\n").unwrap();
        let mut state = State::new(&root);
        dispatch(&mut state, "repo_init", &json!({"branch":"main", "generation":1})).unwrap();
        assert!(!git::run(&root, &["rev-parse", "--verify", "HEAD"]).unwrap().success());
        assert_eq!(std::fs::read_to_string(root.join(".gitignore")).unwrap(), "# preserve\n");
        assert!(dispatch(&mut state, "repo_init", &json!({"generation":2})).is_err());
        dispatch(&mut state, "identity_save", &json!({"name":"日本語 author", "email":"test@example.com", "generation":3})).unwrap();
        assert_eq!(config(&state, "user.name"), "日本語 author");
        let restarted = State::new(&root);
        assert_eq!(config(&restarted, "user.email"), "test@example.com");
        let nested = root.join("nested");
        std::fs::create_dir(&nested).unwrap();
        let mut nested_state = State::new(&nested);
        assert!(dispatch(&mut nested_state, "repo_init", &json!({"generation":1})).is_err());
        assert!(!nested.join(".git").exists());
        assert!(dispatch(&mut state, "remote_connect", &json!({"url":"https://github.com/a/b", "expectedOrigin":"changed", "generation":4})).is_err());
        git::run(&root, &["config", "--local", "remote.origin.pushurl", "https://github.com/a/push"]).unwrap();
        assert!(dispatch(&mut state, "remote_connect", &json!({"url":"https://github.com/a/b", "generation":5})).is_err());
        assert!(config(&state, "remote.origin.url").is_empty());
        state.cancel_setup.store(6, std::sync::atomic::Ordering::SeqCst);
        assert!(dispatch(&mut state, "identity_save", &json!({"name":"changed", "email":"x@y", "generation":6})).is_err());
        assert_eq!(config(&state, "user.name"), "日本語 author");
        // Keep the isolated fixture for diagnosis; this test does not remove user files.
    }

    #[test]
    fn process_deadline_reaps_helper_and_allows_next_operation() {
        let cancel = std::sync::atomic::AtomicU64::new(0);
        let args = ["-c", "alias.setupwait=!powershell -NoProfile -Command Start-Sleep -Seconds 20", "setupwait"];
        let start = std::time::Instant::now();
        let error = git::run_setup(std::path::Path::new("."), &args, 1, &cancel, 1, false).err().unwrap();
        assert_eq!(error.code, "timeout");
        assert!(start.elapsed().as_secs() < 10);
        assert!(git::run_setup(std::path::Path::new("."), &["--version"], 5, &cancel, 2, false).unwrap().success());
    }

    #[test]
    fn process_cancellation_does_not_wait_for_worker_queue() {
        let cancel = std::sync::Arc::new(std::sync::atomic::AtomicU64::new(0));
        let worker_cancel = cancel.clone();
        let start = std::time::Instant::now();
        let worker = std::thread::spawn(move || {
            git::run_setup(std::path::Path::new("."),
                &["-c", "alias.setupwait=!powershell -NoProfile -Command Start-Sleep -Seconds 20", "setupwait"],
                30, &worker_cancel, 1, false)
        });
        std::thread::sleep(std::time::Duration::from_millis(200));
        cancel.store(1, std::sync::atomic::Ordering::SeqCst);
        assert_eq!(worker.join().unwrap().err().unwrap().code, "cancelled");
        assert!(start.elapsed().as_secs() < 10);
    }
}
