// op の実装 (M66a 分 = hello / repo_check / status)。
// op 一覧そのものは spec §4.1 で v1 に凍結済み。ここに無い op は bad_request を返す
// — 「知らない op を黙って ok で返す」と C++ 側が永久に待つ形の不具合になる。

use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use crate::git;
use crate::porcelain;
use crate::protocol::{code, ErrorBody};

/// worker スレッドが 1 本だけ持つ状態。**sim にも UI にも触らない**
pub struct State {
    pub root: PathBuf,
    /// hello で受け取る設定 (定期 fetch は M66f。ここでは保持するだけ)
    pub fetch_interval_min: i64,
    pub auto_fetch: bool,
}

impl State {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        State { root: root.into(), fetch_interval_min: 5, auto_fetch: true }
    }
}

pub type Dispatcher = fn(&mut State, &str, &Value) -> Result<Value, ErrorBody>;

pub fn dispatch(state: &mut State, op: &str, args: &Value) -> Result<Value, ErrorBody> {
    match op {
        "hello" => hello(state, args),
        "repo_check" => repo_check(state),
        "status" => status(state),
        other => Err(ErrorBody::new(code::BAD_REQUEST, format!("unknown op: {other}"))),
    }
}

/// hello — git の所在と版だけを確かめる。**リポジトリでなくても通る**
/// (プロジェクトが git 管理下かどうかは repo_check の仕事)
fn hello(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    if let Some(v) = args.get("fetchIntervalMin").and_then(|v| v.as_i64()) {
        state.fetch_interval_min = v;
    }
    if let Some(v) = args.get("autoFetch").and_then(|v| v.as_bool()) {
        state.auto_fetch = v;
    }
    let ver = git::version(&state.root)?;
    Ok(json!({ "gitVersion": ver }))
}

/// `.git` の中を見るのではなく `git rev-parse` に聞く。worktree / submodule /
/// `.git` がファイル (gitdir: リンク) の場合まで git 自身が面倒を見てくれる
fn repo_check(state: &State) -> Result<Value, ErrorBody> {
    let toplevel = match toplevel(&state.root) {
        Ok(t) => t,
        // 「リポジトリではない」は repo_check にとっては**正常な答え**なので
        // ok:false ではなく isRepo:false で返す (UI は Unavailable::NotRepo を出す)
        Err(_) => {
            return Ok(json!({
                "isRepo": false,
                "toplevel": "",
                "head": "",
                "mergeInProgress": false,
                "rebaseInProgress": false,
            }))
        }
    };
    let git_dir = git::run(&state.root, &["rev-parse", "--absolute-git-dir"])?;
    let git_dir = if git_dir.success() { PathBuf::from(git_dir.stdout_text()) } else { PathBuf::new() };
    // 未出生ブランチ (init 直後) では rev-parse HEAD が失敗する = head は空
    let head = git::run(&state.root, &["rev-parse", "HEAD"])?;
    let head = if head.success() { head.stdout_text() } else { String::new() };
    Ok(json!({
        "isRepo": true,
        "toplevel": toplevel,
        "head": head,
        "mergeInProgress": git_dir.join("MERGE_HEAD").exists(),
        // rebase-merge = 対話/マージ型、rebase-apply = am 型。どちらも「rebase 中」
        "rebaseInProgress": git_dir.join("rebase-merge").exists() || git_dir.join("rebase-apply").exists(),
    }))
}

fn status(state: &State) -> Result<Value, ErrorBody> {
    // リポジトリでないときは status を空で返さない — 「清浄」と区別が付かなくなる
    toplevel(&state.root)?;
    // -uall: 未追跡ディレクトリを "dir/" に畳まず 1 ファイルずつ出す。
    // Content Browser のバッジ (M66i) がファイル単位である以上、畳まれると
    // 「フォルダには印が付くが中身のどれが新規か分からない」になる
    let out = git::run(
        &state.root,
        &["status", "--porcelain=v2", "-z", "--branch", "--untracked-files=all"],
    )?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    let info = porcelain::parse_status_v2(&out.stdout);
    let entries: Vec<Value> = info
        .entries
        .iter()
        .map(|e| {
            let mut o = serde_json::Map::new();
            o.insert("path".into(), Value::String(e.path.clone()));
            if let Some(old) = &e.old_path {
                o.insert("oldPath".into(), Value::String(old.clone()));
            }
            o.insert("index".into(), Value::String(e.index.to_string()));
            o.insert("worktree".into(), Value::String(e.worktree.to_string()));
            o.insert("conflict".into(), Value::Bool(e.conflict));
            Value::Object(o)
        })
        .collect();
    Ok(json!({
        "branch": info.branch,
        "upstream": info.upstream,
        "ahead": info.ahead,
        "behind": info.behind,
        "entries": entries,
    }))
}

/// `git rev-parse --show-toplevel`。失敗 = リポジトリ外
fn toplevel(root: &Path) -> Result<String, ErrorBody> {
    let out = git::run(root, &["rev-parse", "--show-toplevel"])?;
    if !out.success() {
        return Err(ErrorBody::new(code::NOT_REPO, out.stderr_text()));
    }
    Ok(out.stdout_text())
}
