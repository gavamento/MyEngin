//! Context-menu operations. A preview is owned by the serial worker and consumed once.
use super::*;
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, OpenOptions};
use std::io::{Read, Write};

#[derive(Clone, PartialEq)]
struct FileStamp {
    exists: bool,
    len: u64,
    modified: Option<std::time::SystemTime>,
    hash: String,
}

pub(super) struct Preview {
    token: u64,
    args: Value,
    response: Value,
    status: Value,
    index: Vec<u8>,
    ignore: Option<Vec<u8>>,
    ignore_stamp: Option<FileStamp>,
    files: BTreeMap<String, FileStamp>,
}

fn bad(detail: impl Into<String>) -> ErrorBody {
    ErrorBody::new(code::BAD_REQUEST, detail)
}

fn io_error(e: std::io::Error) -> ErrorBody {
    ErrorBody::new(code::LOCKED_FILE, e.to_string())
}

fn checked(root: &Path, args: &[&str]) -> Result<git::GitOutput, ErrorBody> {
    let out = git::run(root, args)?;
    if out.success() {
        Ok(out)
    } else {
        Err(git::classify_error(&out))
    }
}

fn bytes_if_exists(path: &Path) -> Result<Option<Vec<u8>>, ErrorBody> {
    match fs::read(path) {
        Ok(b) => Ok(Some(b)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(io_error(e)),
    }
}

fn ignore_bytes(root: &Path) -> Result<Option<Vec<u8>>, ErrorBody> {
    let path = root.join(".gitignore");
    if let Ok(m) = fs::symlink_metadata(&path) {
        if m.file_type().is_symlink() || !m.is_file() {
            return Err(bad(".gitignore must be a regular file"));
        }
    }
    bytes_if_exists(&path)
}

fn index_bytes(state: &mut State) -> Result<Vec<u8>, ErrorBody> {
    Ok(bytes_if_exists(&git_dir(state).join("index"))?.unwrap_or_default())
}

fn stamp(root: &Path, path: &str) -> Result<FileStamp, ErrorBody> {
    let p = root.join(path);
    let m = match fs::symlink_metadata(&p) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            return Ok(FileStamp {
                exists: false,
                len: 0,
                modified: None,
                hash: String::new(),
            })
        }
        Err(e) => return Err(io_error(e)),
    };
    if !m.is_file() || m.file_type().is_symlink() {
        return Err(bad(format!("unsupported file type: {path}")));
    }
    // Git hashes without writing an object; content changes with the same status are detected.
    let out = checked(root, &["hash-object", "--no-filters", "--", path])?;
    Ok(FileStamp {
        exists: true,
        len: m.len(),
        modified: m.modified().ok(),
        hash: out.stdout_text(),
    })
}

fn validate_path(root: &Path, p: &str) -> Result<(), ErrorBody> {
    if p.is_empty()
        || p.contains(['\\', ':', '\0', '\r', '\n'])
        || p.starts_with('/')
        || p.split('/')
            .any(|s| s.is_empty() || s == "." || s == ".." || s.eq_ignore_ascii_case(".git"))
    {
        return Err(bad(format!("invalid repository-relative path: {p}")));
    }
    let canonical_root = fs::canonicalize(root).map_err(io_error)?;
    let mut part = root.to_path_buf();
    for s in p.split('/') {
        part.push(s);
        if let Ok(m) = fs::symlink_metadata(&part) {
            if m.file_type().is_symlink() {
                return Err(bad(format!("symbolic links are not supported: {p}")));
            }
            let canonical = fs::canonicalize(&part).map_err(io_error)?;
            if !canonical.starts_with(&canonical_root) {
                return Err(bad(format!("path escapes repository: {p}")));
            }
        }
    }
    Ok(())
}

fn primary(p: &str) -> String {
    if let Some(base) = p.strip_suffix(".meta") {
        return primary(base);
    }
    if let Some(base) = p.strip_suffix(".terrain.edit") {
        return format!("{base}.terrain.json");
    }
    p.to_string()
}

fn paths_from_z(bytes: &[u8]) -> Result<BTreeSet<String>, ErrorBody> {
    bytes
        .split(|b| *b == 0)
        .filter(|b| !b.is_empty())
        .map(|b| String::from_utf8(b.to_vec()).map_err(|_| bad("non-UTF-8 path")))
        .collect()
}

pub(super) fn targets(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let selected = args
        .get("paths")
        .and_then(Value::as_array)
        .ok_or_else(|| bad("paths required"))?;
    if selected.is_empty() {
        return Err(bad("no targets"));
    }
    let mut roots = Vec::new();
    for v in selected {
        let p = v.as_str().ok_or_else(|| bad("invalid path"))?;
        validate_path(&state.root, p)?;
        roots.push(p.to_string());
    }
    roots.sort();
    roots.dedup();
    let tracked = paths_from_z(&checked(&state.root, &["ls-files", "--cached", "-z"])?.stdout)?;
    let others = paths_from_z(
        &checked(
            &state.root,
            &["ls-files", "--others", "--exclude-standard", "-z"],
        )?
        .stdout,
    )?;
    let status = build_status(state)?;
    let all: BTreeSet<String> = tracked
        .union(&others)
        .cloned()
        .chain(roots.iter().cloned())
        .collect();
    let mut rows = Vec::new();
    for p in all {
        let base = primary(&p);
        if !roots
            .iter()
            .any(|r| p == *r || p.starts_with(&format!("{r}/")) || base == primary(r))
        {
            continue;
        }
        if state.root.join(&p).is_dir() {
            continue;
        }
        validate_path(&state.root, &p)?;
        let entry = status["entries"]
            .as_array()
            .and_then(|a| a.iter().find(|e| e["path"] == p));
        rows.push(json!({ "path": p, "tracked": tracked.contains(&p),
            "exists": state.root.join(&p).is_file(),
            "index": entry.and_then(|e| e["index"].as_str()).unwrap_or("."),
            "worktree": entry.and_then(|e| e["worktree"].as_str()).unwrap_or("."),
            "conflict": entry.and_then(|e| e["conflict"].as_bool()).unwrap_or(false) }));
    }
    Ok(json!({"root": toplevel(&state.root)?, "roots": roots, "targets": rows, "status": status}))
}

fn sha_arg(state: &State, args: &Value) -> Result<String, ErrorBody> {
    let sha = args["sha"]
        .as_str()
        .ok_or_else(|| bad("full sha required"))?;
    if !matches!(sha.len(), 40 | 64) || !sha.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(bad("full sha required"));
    }
    let resolved = checked(
        &state.root,
        &["rev-parse", "--verify", &format!("{sha}^{{commit}}")],
    )?
    .stdout_text();
    if resolved != sha.to_ascii_lowercase() {
        return Err(bad("sha is not a commit"));
    }
    Ok(resolved)
}

fn commit_base(state: &State, sha: &str) -> Result<(String, usize), ErrorBody> {
    let line = checked(&state.root, &["rev-list", "--parents", "-n", "1", sha])?.stdout_text();
    let parts: Vec<&str> = line.split_whitespace().collect();
    let count = parts.len().saturating_sub(1);
    let base = if count > 0 {
        parts[1].to_string()
    } else {
        git::run_with_stdin(&state.root, &["hash-object", "-t", "tree", "--stdin"], b"")?
            .stdout_text()
    };
    Ok((base, count))
}

pub(super) fn commit_diff(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let sha = sha_arg(state, args)?;
    let (base, parents) = commit_base(state, &sha)?;
    let out = checked(
        &state.root,
        &["diff", "--no-ext-diff", "--no-textconv", &base, &sha, "--"],
    )?;
    let raw = String::from_utf8_lossy(&out.stdout);
    let (text, truncated) = porcelain::clip_text(&raw, MAX_DIFF_BYTES);
    Ok(json!({"sha": sha, "path": sha, "text": text, "truncated": truncated, "parents": parents}))
}

pub(super) fn operation(state: &mut State) -> String {
    let dir = git_dir(state);
    if dir.as_os_str().is_empty() {
        return String::new();
    }
    if dir.join("rebase-merge").exists() || dir.join("rebase-apply").exists() {
        return "rebase".into();
    }
    if dir.join("MERGE_HEAD").exists() {
        return "merge".into();
    }
    if dir.join("CHERRY_PICK_HEAD").exists() {
        return "cherry-pick".into();
    }
    if dir.join("REVERT_HEAD").exists() {
        return "revert".into();
    }
    if dir.join("sequencer").exists() {
        if let Ok(todo) = fs::read_to_string(dir.join("sequencer/todo")) {
            if todo.lines().any(|l| l.starts_with("revert ")) {
                return "revert".into();
            }
            if todo.lines().any(|l| l.starts_with("pick ")) {
                return "cherry-pick".into();
            }
        }
        return "sequencer".into();
    }
    String::new()
}

fn ignore_rule(p: &str, folder: bool) -> String {
    let mut out = String::from("/");
    for c in p.chars() {
        if matches!(c, '\\' | '*' | '?' | '[' | ']' | ' ' | '#' | '!') {
            out.push('\\');
        }
        out.push(c);
    }
    if folder {
        out.push('/');
    }
    out
}

fn branch_validate(state: &State, name: &str) -> Result<(), ErrorBody> {
    checked(
        &state.root,
        &["check-ref-format", &format!("refs/heads/{name}")],
    )?;
    if name.starts_with('-') {
        return Err(bad("branch name starts with '-'"));
    }
    Ok(())
}

pub(super) fn branch_available(
    state: &State,
    name: &str,
    allow_current: bool,
) -> Result<(), ErrorBody> {
    let list = checked(&state.root, &["worktree", "list", "--porcelain"])?.stdout_text();
    let root = fs::canonicalize(&state.root).map_err(io_error)?;
    let mut current = false;
    for line in list.lines() {
        if let Some(path) = line.strip_prefix("worktree ") {
            current = fs::canonicalize(path).map(|p| p == root).unwrap_or(false);
        }
        if line == format!("branch refs/heads/{name}") && !(allow_current && current) {
            return Err(bad("branch is checked out in a worktree"));
        }
    }
    Ok(())
}

fn clean_history(state: &mut State, status: &Value) -> Result<(), ErrorBody> {
    if !operation(state).is_empty() {
        return Err(bad("Git operation already in progress"));
    }
    if status["entries"].as_array().map_or(true, |a| !a.is_empty()) {
        return Err(bad(
            "history operations require a clean worktree including untracked files",
        ));
    }
    if !checked(&state.root, &["symbolic-ref", "--quiet", "HEAD"])?
        .stdout_text()
        .starts_with("refs/heads/")
    {
        return Err(bad("a local branch is required"));
    }
    Ok(())
}

pub(super) fn preview(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    state.action_preview = None;
    if !operation(state).is_empty() {
        return Err(bad("Git operation already in progress"));
    }
    let action = args["action"]
        .as_str()
        .ok_or_else(|| bad("action required"))?;
    let status = build_status(state)?;
    let mut response = json!({"action": action, "root": toplevel(&state.root)?,
        "head": status["head"], "paths": [], "excluded": [], "names": [], "rules": []});
    let mut paths = BTreeSet::new();
    match action {
        "stage" | "unstage" | "discard" | "untrack" | "untrack_ignore" | "ignore" => {
            let list = targets(state, args)?;
            let mut excluded = Vec::new();
            for row in list["targets"]
                .as_array()
                .ok_or_else(|| bad("targets missing"))?
            {
                let p = row["path"].as_str().unwrap_or("");
                let tracked = row["tracked"] == true;
                let index = row["index"].as_str().unwrap_or(".");
                let work = row["worktree"].as_str().unwrap_or(".");
                let reason = if row["conflict"] == true {
                    "conflict"
                } else if matches!(action, "untrack" | "untrack_ignore" | "ignore")
                    && p.eq_ignore_ascii_case(".gitignore")
                {
                    "protected_gitignore"
                } else if matches!(action, "untrack" | "untrack_ignore") && !tracked {
                    "not_tracked"
                } else if action == "unstage" && matches!(index, "." | "?") {
                    "not_staged"
                } else if matches!(action, "stage" | "discard") && work == "." && index != "?" {
                    "no_worktree_change"
                } else if !tracked && row["exists"] != true {
                    "missing"
                } else {
                    ""
                };
                if reason.is_empty() {
                    paths.insert(p.to_string());
                } else {
                    excluded.push(json!({"path": p, "reason": reason}));
                }
            }
            response["excluded"] = json!(excluded);
            if matches!(action, "ignore" | "untrack_ignore") {
                let mut rules = BTreeSet::new();
                for root in list["roots"].as_array().unwrap() {
                    let root = root.as_str().unwrap();
                    if !root.eq_ignore_ascii_case(".gitignore") && state.root.join(root).is_dir() {
                        rules.insert(ignore_rule(root, true));
                    }
                }
                for p in &paths {
                    rules.insert(ignore_rule(p, false));
                }
                response["rules"] = json!(rules);
            }
            if action == "discard" {
                response["names"] = json!(paths.iter().map(|p| json!({"path": p,
                    "status": if list["targets"].as_array().unwrap().iter().any(|r| r["path"] == *p && r["tracked"] == false) { "D" } else { "M" }})).collect::<Vec<_>>());
            }
        }
        "soft" | "mixed" | "commit_revert" | "cherry_pick" => {
            clean_history(state, &status)?;
            let sha = sha_arg(state, args)?;
            let (base, parents) = commit_base(state, &sha)?;
            let tree = checked(&state.root, &["ls-tree", "-r", "-z", &sha])?;
            for record in tree.stdout.split(|b| *b == 0) {
                if record.starts_with(b"120000 ") || record.starts_with(b"160000 ") {
                    return Err(bad(
                        "commit contains symbolic links or submodules; use an external Git client",
                    ));
                }
            }
            if matches!(action, "commit_revert" | "cherry_pick") {
                if parents > 1 {
                    return Err(bad("merge commits require parent selection"));
                }
                if identity_check(state)?["ok"] != true {
                    return Err(ErrorBody::new(
                        code::IDENTITY_MISSING,
                        "user.name and user.email required",
                    ));
                }
            }
            let from = if matches!(action, "soft" | "mixed") {
                status["head"].as_str().unwrap_or("")
            } else {
                &base
            };
            let names = if action == "commit_revert" {
                changed_names(state, &sha, &base)?
            } else {
                changed_names(state, from, &sha)?
            };
            for n in &names {
                if let Some(p) = n["path"].as_str() {
                    paths.insert(p.to_string());
                }
            }
            response["names"] = json!(names);
            response["sha"] = json!(sha);
            response["subject"] =
                json!(checked(&state.root, &["show", "-s", "--format=%s", &sha])?.stdout_text());
            response["upstream"] = status["upstream"].clone();
        }
        "branch_create" | "branch_rename" | "branch_delete" => {
            let name = branch_name_arg(args, "name")?;
            branch_validate(state, &name)?;
            response["name"] = json!(name);
            response["ref"] = json!(format!("refs/heads/{name}"));
            if action == "branch_create" {
                if ref_exists(&state.root, &format!("refs/heads/{name}"))? {
                    return Err(bad("branch already exists"));
                }
                response["sha"] = json!(sha_arg(state, args)?);
            } else {
                branch_available(state, &name, action == "branch_rename")?;
                let sha = checked(
                    &state.root,
                    &["rev-parse", "--verify", &format!("refs/heads/{name}")],
                )?
                .stdout_text();
                response["sha"] = json!(sha);
                if action == "branch_delete" {
                    checked(&state.root, &["merge-base", "--is-ancestor", &sha, "HEAD"])?;
                } else {
                    let new_name = branch_name_arg(args, "newName")?;
                    branch_validate(state, &new_name)?;
                    if ref_exists(&state.root, &format!("refs/heads/{new_name}"))? {
                        return Err(bad("branch already exists"));
                    }
                    response["newName"] = json!(new_name);
                }
            }
        }
        _ => return Err(bad("unknown action")),
    }
    if matches!(
        action,
        "stage" | "unstage" | "discard" | "untrack" | "untrack_ignore" | "ignore"
    ) && paths.is_empty()
    {
        return Err(bad(format!(
            "no eligible targets: {}",
            response["excluded"]
        )));
    }
    response["paths"] = json!(paths);
    let mut files = BTreeMap::new();
    for p in &paths {
        files.insert(p.clone(), stamp(&state.root, p)?);
    }
    let ignore = if matches!(action, "ignore" | "untrack_ignore") {
        ignore_bytes(&state.root)?
    } else {
        None
    };
    if let Some(bytes) = &ignore {
        std::str::from_utf8(bytes).map_err(|_| bad(".gitignore is not UTF-8"))?;
    }
    let ignore_stamp = if matches!(action, "ignore" | "untrack_ignore") {
        Some(stamp(&state.root, ".gitignore")?)
    } else {
        None
    };
    // status can refresh index stat data; capture the index after all read operations.
    let index = index_bytes(state)?;
    state.action_serial = state.action_serial.wrapping_add(1);
    response["token"] = json!(state.action_serial);
    state.action_preview = Some(Preview {
        token: state.action_serial,
        args: args.clone(),
        response: response.clone(),
        status,
        index,
        ignore,
        ignore_stamp,
        files,
    });
    Ok(response)
}

fn append_ignore(
    root: &Path,
    expected: &Option<Vec<u8>>,
    expected_stamp: &Option<FileStamp>,
    rules: &[Value],
) -> Result<(), ErrorBody> {
    if ignore_bytes(root)? != *expected || Some(stamp(root, ".gitignore")?) != *expected_stamp {
        return Err(bad(
            ".gitignore changed since confirmation started; confirm again",
        ));
    }
    let original = expected.clone().unwrap_or_default();
    let text = std::str::from_utf8(&original).map_err(|_| bad(".gitignore is not UTF-8"))?;
    let newline = if original.windows(2).any(|w| w == b"\r\n") {
        "\r\n"
    } else {
        "\n"
    };
    let mut suffix = Vec::new();
    for rule in rules.iter().filter_map(Value::as_str) {
        // A trailing negation can override an earlier identical rule. Refuse instead of silently duplicating.
        if text
            .trim_start_matches('\u{feff}')
            .lines()
            .any(|l| l == rule)
        {
            continue;
        }
        if suffix.is_empty() && !original.is_empty() && !original.ends_with(b"\n") {
            suffix.extend_from_slice(newline.as_bytes());
        }
        suffix.extend_from_slice(rule.as_bytes());
        suffix.extend_from_slice(newline.as_bytes());
    }
    if suffix.is_empty() {
        return Ok(());
    }
    let path = root.join(".gitignore");
    let mut options = OpenOptions::new();
    options.read(true).append(true);
    if expected.is_none() {
        options.create_new(true);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        options.share_mode(1); // FILE_SHARE_READ: reject concurrent writers during comparison+append.
    }
    let mut file = options.open(path).map_err(io_error)?;
    let mut current = Vec::new();
    file.read_to_end(&mut current).map_err(io_error)?;
    if current != original {
        return Err(bad(
            ".gitignore changed since confirmation started; confirm again",
        ));
    }
    file.write_all(&suffix).map_err(io_error)?;
    file.sync_all().map_err(io_error)?;
    Ok(())
}

pub(super) fn execute(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let p = state
        .action_preview
        .take()
        .ok_or_else(|| bad("preview expired; confirm again"))?;
    if args["token"].as_u64() != Some(p.token) {
        return Err(bad("preview token mismatch"));
    }
    let action = p.args["action"].as_str().unwrap_or("");
    if matches!(action, "ignore" | "untrack_ignore")
        && (ignore_bytes(&state.root)? != p.ignore
            || Some(stamp(&state.root, ".gitignore")?) != p.ignore_stamp)
    {
        return Err(bad(
            ".gitignore changed since confirmation started; confirm again",
        ));
    }
    let status = build_status(state)?;
    if status != p.status || index_bytes(state)? != p.index || !operation(state).is_empty() {
        return Err(bad("HEAD, index or worktree changed; confirm again"));
    }
    for (path, before) in &p.files {
        validate_path(&state.root, path)?;
        if stamp(&state.root, path)? != *before {
            return Err(bad("target content changed; confirm again"));
        }
    }
    let paths: Vec<String> = p.files.keys().cloned().collect();
    let mut completed = Vec::<String>::new();
    let result: Result<(), ErrorBody> = (|| {
        if matches!(action, "ignore" | "untrack_ignore") {
            let text = std::str::from_utf8(p.ignore.as_deref().unwrap_or(b""))
                .map_err(|_| bad(".gitignore is not UTF-8"))?;
            for path in &paths {
                let rule = ignore_rule(path, false);
                if text
                    .trim_start_matches('\u{feff}')
                    .lines()
                    .any(|line| line == rule)
                {
                    let out = git::run(
                        &state.root,
                        &["check-ignore", "--no-index", "-q", "--", path],
                    )?;
                    if !out.success() {
                        return Err(bad(format!(
                            "existing rule is overridden: {path}; inspect negation rules"
                        )));
                    }
                }
            }
            if p.ignore.is_some() {
                OpenOptions::new()
                    .write(true)
                    .open(state.root.join(".gitignore"))
                    .map_err(io_error)?;
            }
        }
        match action {
            "stage" | "unstage" | "untrack" | "untrack_ignore" => {
                let command: &[&str] = match action {
                    "stage" => &["add", "-A"],
                    "unstage" => &["reset", "-q"],
                    _ => &["rm", "--cached"],
                };
                if matches!(action, "untrack" | "untrack_ignore") {
                    run_per_path_chunk(state, &["rm", "--cached", "--dry-run"], &paths)?;
                }
                for chunk in path_chunks(&paths) {
                    run_per_path_chunk(state, command, chunk)?;
                    completed.extend_from_slice(chunk);
                }
                if action == "untrack_ignore" {
                    append_ignore(
                        &state.root,
                        &p.ignore,
                        &p.ignore_stamp,
                        p.response["rules"].as_array().unwrap(),
                    )?;
                }
            }
            "ignore" => {
                append_ignore(
                    &state.root,
                    &p.ignore,
                    &p.ignore_stamp,
                    p.response["rules"].as_array().unwrap(),
                )?;
                completed = paths.clone();
            }
            "discard" => {
                revert(state, &json!({"paths": paths}))?;
                completed = paths.clone();
            }
            "soft" | "mixed" | "commit_revert" | "cherry_pick" => {
                clean_history(state, &status)?;
                let sha = p.response["sha"].as_str().unwrap();
                let argv: Vec<&str> = match action {
                    "soft" => vec!["reset", "--soft", sha],
                    "mixed" => vec!["reset", "--mixed", sha],
                    "commit_revert" => vec!["-c", "core.editor=true", "revert", "--no-edit", sha],
                    _ => vec!["-c", "core.editor=true", "cherry-pick", "--no-edit", sha],
                };
                checked(&state.root, &argv)?;
                completed = paths.clone();
            }
            "branch_create" => {
                branch_create(
                    state,
                    &json!({"name": p.args["name"], "from": p.response["sha"]}),
                )?;
            }
            "branch_rename" | "branch_delete" => {
                let name = p.args["name"].as_str().unwrap();
                branch_available(state, name, action == "branch_rename")?;
                let current = checked(
                    &state.root,
                    &["rev-parse", "--verify", &format!("refs/heads/{name}")],
                )?
                .stdout_text();
                if p.response["sha"] != current {
                    return Err(bad("branch tip changed; confirm again"));
                }
                if action == "branch_delete" {
                    checked(
                        &state.root,
                        &["merge-base", "--is-ancestor", &current, "HEAD"],
                    )?;
                    checked(&state.root, &["branch", "-d", name])?;
                } else {
                    checked(
                        &state.root,
                        &["branch", "-m", name, p.args["newName"].as_str().unwrap()],
                    )?;
                }
            }
            _ => return Err(bad("unknown action")),
        }
        if matches!(action, "ignore" | "untrack_ignore") {
            for path in &paths {
                let out = git::run(
                    &state.root,
                    &["check-ignore", "--no-index", "-q", "--", path],
                )?;
                if !out.success() {
                    return Err(bad(format!(
                        "ignore rule did not match {path}; inspect existing negation rules"
                    )));
                }
            }
        }
        Ok(())
    })();
    let mut names = Vec::new();
    if matches!(action, "discard" | "commit_revert" | "cherry_pick") {
        for (path, before) in &p.files {
            let after = stamp(&state.root, path)?;
            if before != &after {
                names.push(json!({"path": path, "status": if !after.exists {"D"} else if !before.exists {"A"} else {"M"}}));
            }
        }
    }
    let remaining: Vec<_> = paths
        .iter()
        .filter(|p| !completed.contains(p))
        .cloned()
        .collect();
    let error = result
        .as_ref()
        .err()
        .map(|e| json!({"code": e.code, "detail": e.detail}));
    Ok(
        json!({"success": result.is_ok(), "error": error, "completed": completed, "remaining": remaining,
        "names": names, "head": head_oid(&state.root)?, "status": status_after_write(state)?, "operation": operation(state)}),
    )
}

pub(super) fn finish(state: &mut State, op: &str, continuing: bool) -> Result<Value, ErrorBody> {
    let mut paths: BTreeSet<String> = diff_head_names(state)?
        .into_iter()
        .map(|n| n.path)
        .collect();
    for e in unmerged(state)? {
        paths.insert(e.path);
    }
    let dir = git_dir(state);
    if let Ok(original) = fs::read_to_string(dir.join("sequencer/head")) {
        let original = original.trim();
        if matches!(original.len(), 40 | 64) && original.bytes().all(|b| b.is_ascii_hexdigit()) {
            for n in changed_names(state, original, &head_oid(&state.root)?)? {
                if let Some(p) = n["path"].as_str() {
                    paths.insert(p.to_string());
                }
            }
        }
    }
    if let Ok(todo) = fs::read_to_string(dir.join("sequencer/todo")) {
        for line in todo.lines() {
            let mut fields = line.split_whitespace();
            if !matches!(fields.next(), Some("pick" | "revert")) {
                continue;
            }
            let rev = fields.next().unwrap_or("");
            if rev.is_empty() || !rev.bytes().all(|b| b.is_ascii_hexdigit()) {
                return Err(bad("invalid sequencer revision"));
            }
            let sha = checked(
                &state.root,
                &["rev-parse", "--verify", &format!("{rev}^{{commit}}")],
            )?
            .stdout_text();
            let (base, _) = commit_base(state, &sha)?;
            for n in changed_names(state, &base, &sha)? {
                if let Some(p) = n["path"].as_str() {
                    paths.insert(p.to_string());
                }
            }
        }
    }
    let marker = if op == "revert" {
        "REVERT_HEAD"
    } else {
        "CHERRY_PICK_HEAD"
    };
    if let Ok(out) = checked(&state.root, &["rev-parse", "--verify", marker]) {
        let sha = out.stdout_text();
        let (base, _) = commit_base(state, &sha)?;
        for n in changed_names(state, &base, &sha)? {
            if let Some(p) = n["path"].as_str() {
                paths.insert(p.to_string());
            }
        }
    }
    let mut before = BTreeMap::new();
    for p in &paths {
        before.insert(p.clone(), stamp(&state.root, p)?);
    }
    let out = git::run(
        &state.root,
        &[
            "-c",
            "core.editor=true",
            op,
            if continuing { "--continue" } else { "--abort" },
        ],
    )?;
    let mut names = Vec::new();
    for (p, old) in before {
        let new = stamp(&state.root, &p)?;
        // Continue also reapplies resolved files: edits may have happened while reload was guarded.
        if old != new || continuing {
            names.push(json!({"path": p, "status": if !new.exists {"D"} else if !old.exists {"A"} else {"M"}}));
        }
    }
    let error = if out.success() {
        Value::Null
    } else {
        json!({"code": code::GIT_FAILED,
        "detail": format!("{}\n{}", out.stderr_text(), out.stdout_text())})
    };
    Ok(
        json!({"success": out.success(), "error": error, "names": names,
        "head": head_oid(&state.root)?, "status": status_after_write(state)?, "operation": operation(state)}),
    )
}
