// op の実装 (M66a = hello / repo_check / status、M66b = hint_changed)。
// op 一覧そのものは spec §4.1 で v1 に凍結済み。ここに無い op は bad_request を返す
// — 「知らない op を黙って ok で返す」と C++ 側が永久に待つ形の不具合になる。

use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use crate::git;
use crate::porcelain;
use crate::protocol::{self, code, event, ErrorBody};

/// worker スレッドが 1 本だけ持つ状態。**sim にも UI にも触らない**
pub struct State {
    pub root: PathBuf,
    /// hello で受け取る設定 (定期 fetch は M66f。ここでは保持するだけ)
    pub fetch_interval_min: i64,
    pub auto_fetch: bool,
    /// 直近に組み立てた status の結果。監視スレッド由来の取り直しで
    /// **同じ物なら通知を出さない**ための比較用 (M66b)。
    /// ★これが無いと、エディタが cache\ の外に書いたどんな 1 バイトでも
    ///   status_changed が飛び、窓が毎秒作り直される
    last_status: Option<Value>,
    /// 直近の HEAD (porcelain の `# branch.oid`)。変化で repo_changed を出す
    last_head: String,
}

impl State {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        State {
            root: root.into(),
            fetch_interval_min: 5,
            auto_fetch: true,
            last_status: None,
            last_head: String::new(),
        }
    }
}

pub type Dispatcher = fn(&mut State, &str, &Value) -> Result<Value, ErrorBody>;

pub fn dispatch(state: &mut State, op: &str, args: &Value) -> Result<Value, ErrorBody> {
    match op {
        "hello" => hello(state, args),
        "repo_check" => repo_check(state),
        "status" => status(state),
        // hint_changed — 「今このパスを保存した」ので監視のデバウンス (300 ms) を
        // 待たずに取り直す口 (M66i がアセット保存の直後に叩く)。
        // ★**応答に status をそのまま載せる**。ここで status_changed 通知を出す形にも
        //   できるが、そうすると CLI (script モード) の出力に event 行が混ざる
        //   = spec §4.4「CLI は event 行を出さない」と食い違う。要求した側が
        //   応答で受け取れば C++ 側は監視経路と同じ 1 本の関数へ流せる
        "hint_changed" => status(state),
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

/// status を取り直し、**前回と違うときだけ**通知行を組む (監視スレッド用)。
/// 失敗 (リポジトリでなくなった等) は黙って空を返す — 通知経路で
/// エラーを出すと、リポジトリ外のプロジェクトで毎回トーストが出る
pub fn refresh_status(state: &mut State) -> Vec<String> {
    // ★status() ではなく build_status() を呼ぶこと。status() は「要求に答えた」
    //   時点で last_status を更新するので、それを経由すると下の比較が**常に等しく**
    //   なり通知が 1 件も出なくなる (実際に踏んだ。cargo test の 2 本が同時に赤くなる)
    let value = match build_status(state) {
        Ok(v) => v,
        Err(_) => return Vec::new(),
    };
    let head = value.get("head").and_then(|h| h.as_str()).unwrap_or("").to_string();
    let mut lines = Vec::new();
    // HEAD が動いた = 外部で checkout / commit / pull された (spec §4.1「外部 git 操作の検知」)。
    // 初回 (last_head が空) は「動いた」ではないので出さない
    if !state.last_head.is_empty() && head != state.last_head {
        lines.push(protocol::event_line(event::REPO_CHANGED, json!({ "head": head })));
    }
    state.last_head = head;
    if state.last_status.as_ref() == Some(&value) {
        return lines; // 中身が同じ = 出す意味が無い
    }
    state.last_status = Some(value.clone());
    lines.push(protocol::event_line(event::STATUS_CHANGED, json!({ "status": value })));
    lines
}

fn status(state: &mut State) -> Result<Value, ErrorBody> {
    let value = build_status(state)?;
    // 要求側が受け取った時点で「窓は最新」なので、監視が直後に同じ物を出さないよう
    // ここでも覚える (last_head は refresh_status だけが動かす — 明示的な status で
    // 覚えてしまうと、その直後の HEAD 移動が repo_changed にならない…わけではないが、
    // 「初回の status で覚える」方が外部移動の基準として素直)
    state.last_status = Some(value.clone());
    if state.last_head.is_empty() {
        state.last_head =
            value.get("head").and_then(|h| h.as_str()).unwrap_or("").to_string();
    }
    Ok(value)
}

fn build_status(state: &State) -> Result<Value, ErrorBody> {
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
        // porcelain の `# branch.oid`。**ここに載せておくと HEAD の移動検知に
        // rev-parse を 1 回も足さずに済む** (未出生ブランチでは空文字列)
        "head": info.oid,
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
