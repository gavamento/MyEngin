// op の実装 (M66a = hello / repo_check / status、M66b = hint_changed、
// M66c = stage / unstage / commit / log / diff / identity_check)。
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
        "stage" => stage(state, args),
        "unstage" => unstage(state, args),
        "commit" => commit(state, args),
        "log" => log(state, args),
        "diff" => diff(state, args),
        "identity_check" => identity_check(state),
        other => Err(ErrorBody::new(code::BAD_REQUEST, format!("unknown op: {other}"))),
    }
}

/// 1 回の git 呼び出しに載せるパスの上限。
/// ★Windows のコマンドラインは 32767 文字で切れる。畳んで渡すと「多く選んだときだけ
///   静かに一部が stage されない」= 一番気付きにくい形で壊れるので、先に分割する。
///   分割しても `git add -A` / `git reset` はパスごとに独立なので結果は同じ
const MAX_PATHS_PER_CALL: usize = 64;

/// `log{n}` の上限 (spec の M66c: 上限 200)。
const MAX_LOG: i64 = 200;

/// `diff` のテキスト上限。これを超えたら切って `truncated:true` を立てる。
/// ★UI は読み取り専用の子窓に丸ごと流し込むので、巨大な差分をそのまま渡すと
///   エディタが 1 フレームで固まる。「切った」ことを伝えられれば実害は無い
const MAX_DIFF_BYTES: usize = 256 * 1024;

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

// ---- M66c: 書き込み系 (stage / unstage / commit) と読み取り系 (log / diff / identity_check) ----

/// 書き込み系 op の応答に載せる「実行後の status」。**last_status / last_head も更新する**。
///
/// ★これが無いと、自分が起こした index / refs の書き換えを監視スレッドが拾って
///   status_changed を余分に 1 本流す。commit では last_head も動かさないと
///   「外部で HEAD が移動しました」のトーストが**自分のコミットで**出る (実際に踏む形)
fn status_after_write(state: &mut State) -> Result<Value, ErrorBody> {
    let value = build_status(state)?;
    state.last_head = value.get("head").and_then(|h| h.as_str()).unwrap_or("").to_string();
    state.last_status = Some(value.clone());
    Ok(value)
}

/// `args.paths` を検証して取り出す。**toplevel 相対・'/' 区切り**だけを許す。
///
/// 弾く理由:
///   * 空配列 … `git add -A --` はパス無しだと**リポジトリ全体**を stage する。
///     「選択が空のまま押してしまった」が全ステージに化けるのが最悪
///   * `..` を含む … toplevel の外を触る (git 自身も拒否するが、拒否文が
///     ユーザーに読めない形で出るより手前で止める)
///   * 先頭 `-` … pathspec がオプションに化ける (`--` を必ず置くので実害は無いが、
///     C++ 側のバグを黙って通さないため)
fn arg_paths(args: &Value) -> Result<Vec<String>, ErrorBody> {
    let arr = match args.get("paths").and_then(|v| v.as_array()) {
        Some(a) => a,
        None => return Err(ErrorBody::new(code::BAD_REQUEST, "paths must be an array of strings")),
    };
    let mut out = Vec::with_capacity(arr.len());
    for v in arr {
        let s = match v.as_str() {
            Some(s) => s,
            None => return Err(ErrorBody::new(code::BAD_REQUEST, "paths must be strings")),
        };
        if s.is_empty() {
            return Err(ErrorBody::new(code::BAD_REQUEST, "empty path"));
        }
        if s.starts_with('-') {
            return Err(ErrorBody::new(code::BAD_REQUEST, format!("path looks like an option: {s}")));
        }
        if s.split('/').any(|seg| seg == "..") {
            return Err(ErrorBody::new(code::BAD_REQUEST, format!("path escapes the repository: {s}")));
        }
        out.push(s.to_string());
    }
    if out.is_empty() {
        // ★ここを通すと「全部 stage」になる。空は必ず弾く
        return Err(ErrorBody::new(code::BAD_REQUEST, "paths must not be empty"));
    }
    Ok(out)
}

/// パス群に対して git を 1 回叩く (上限で分割)。失敗した時点で止めて分類済みエラーを返す
fn run_per_path_chunk(state: &State, head: &[&str], paths: &[String]) -> Result<(), ErrorBody> {
    for chunk in paths.chunks(MAX_PATHS_PER_CALL) {
        let mut argv: Vec<&str> = head.to_vec();
        argv.push("--");
        for p in chunk {
            argv.push(p.as_str());
        }
        let out = git::run(&state.root, &argv)?;
        if !out.success() {
            return Err(git::classify_error(&out));
        }
    }
    Ok(())
}

/// stage — `git add -A -- <paths>`。
///
/// なぜ `-A` か: 追加 (`?`) / 変更 (`M`) / 削除 (`D`) を**1 本の呼び出しで**扱えるため。
/// 素の `git add` は削除を index に入れず、`git rm --cached` は追加を扱えない。
/// 「選んだ行を stage する」に対して 2 種類の git を打ち分けると、対 (`.meta`) の
/// 片方だけが index に入る取りこぼしが必ず出る (対の規則 = spec §4.1 の要点そのもの)。
fn stage(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let paths = arg_paths(args)?;
    run_per_path_chunk(state, &["add", "-A"], &paths)?;
    Ok(json!({ "status": status_after_write(state)? }))
}

/// unstage — `git reset -q -- <paths>`。
///
/// ★`git restore --staged` (2.23+) を使わない。**未出生ブランチ (commit が 1 つも無い
///   リポジトリ) で "fatal: could not resolve HEAD" になる**のを実測した。
///   `git reset -q -- <paths>` は同じ状況で成功し、下限の git 2.11 にも存在する。
fn unstage(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let paths = arg_paths(args)?;
    run_per_path_chunk(state, &["reset", "-q"], &paths)?;
    Ok(json!({ "status": status_after_write(state)? }))
}

/// commit — `git commit -F -` (本文は stdin)。
///
/// 戻り値に新しい HEAD を載せるのは、UI が「何が出来たか」をログに残せるようにするため
/// (`status.head` にも同じ値が入るが、commit の**成果**として明示する)。
fn commit(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let message = args.get("message").and_then(|v| v.as_str()).unwrap_or("").trim().to_string();
    if message.is_empty() {
        return Err(ErrorBody::new(code::BAD_REQUEST, "commit message is empty"));
    }
    let out = git::run_with_stdin(&state.root, &["commit", "-F", "-"], message.as_bytes())?;
    if !out.success() {
        // ★git commit は「何も staged が無い」を **stdout** に書く (stderr ではない)。
        //   classify_error は stderr しか見ないので、ここだけ両方を見る。
        //   見落とすと nothing_to_commit が git_failed に化け、UI が
        //   「git が失敗しました」+ 空の detail という何も分からない表示になる
        let mut text = out.stderr_text();
        if !text.is_empty() {
            text.push('\n');
        }
        text.push_str(&out.stdout_text());
        let low = text.to_ascii_lowercase();
        if low.contains("nothing to commit")
            || low.contains("no changes added to commit")
            || low.contains("nothing added to commit")
        {
            return Err(ErrorBody::new(code::NOTHING_TO_COMMIT, text.trim().to_string()));
        }
        let mut err = git::classify_error(&out);
        if err.detail.is_empty() {
            err.detail = text.trim().to_string();
        }
        return Err(err);
    }
    let head = git::run(&state.root, &["rev-parse", "HEAD"])?;
    let head = if head.success() { head.stdout_text() } else { String::new() };
    Ok(json!({ "head": head, "status": status_after_write(state)? }))
}

/// log — `git log -n <n> --format=... -z`。
///
/// 出力は `<sha>\0<author>\0<date>\0<subject>\0` の繰り返し (実測: 最後のレコードも
/// NUL で**終端**される)。4 個ずつ切り出し、端数は捨てる。
/// `%s` は subject = 1 行目だけなので、本文の改行がレコード境界を壊すことはない。
fn log(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let n = args.get("n").and_then(|v| v.as_i64()).unwrap_or(50).clamp(1, MAX_LOG);
    let n_text = n.to_string();
    let out = git::run(
        &state.root,
        &["log", "-n", &n_text, "--format=%H%x00%an%x00%aI%x00%s", "-z"],
    )?;
    if !out.success() {
        let low = out.stderr_text().to_ascii_lowercase();
        // ★未出生ブランチ (commit が 1 つも無い) は**エラーではない**。
        //   ここを err にすると、clone 直後や git init 直後のリポジトリを開いた瞬間に
        //   赤いエラーが出る = ユーザーに直しようが無い警告になる
        if low.contains("does not have any commits") || low.contains("bad default revision") {
            return Ok(json!({ "commits": [] }));
        }
        return Err(git::classify_error(&out));
    }
    let commits: Vec<Value> = porcelain::parse_log_z(&out.stdout)
        .into_iter()
        .map(|e| {
            json!({ "sha": e.sha, "author": e.author, "date": e.date, "subject": e.subject })
        })
        .collect();
    Ok(json!({ "commits": commits }))
}

/// diff — `git diff [--cached] --no-ext-diff -- <path>` のテキストをそのまま。
///
/// ★`--no-ext-diff` が要る。開発者が `diff.external` を設定していると、そのツールの
///   出力 (あるいは GUI の起動) がここへ返ってくる。バイナリは git の
///   "Binary files a/x and b/x differ" 1 行がそのまま載る (意味付けはしない = v1)。
fn diff(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let path = match args.get("path").and_then(|v| v.as_str()) {
        Some(p) if !p.is_empty() => p.to_string(),
        _ => return Err(ErrorBody::new(code::BAD_REQUEST, "path is required")),
    };
    // 1 本でも arg_paths と同じ検証を通す (綴りの規則を 2 箇所に分けない)
    let checked = arg_paths(&json!({ "paths": [path] }))?;
    let path = checked[0].clone();
    let staged = args.get("staged").and_then(|v| v.as_bool()).unwrap_or(false);
    let mut argv: Vec<&str> = vec!["diff", "--no-ext-diff"];
    if staged {
        argv.push("--cached");
    }
    argv.push("--");
    argv.push(path.as_str());
    let out = git::run(&state.root, &argv)?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    let raw = String::from_utf8_lossy(&out.stdout);
    let (text, truncated) = porcelain::clip_text(&raw, MAX_DIFF_BYTES);
    Ok(json!({ "path": path, "staged": staged, "text": text, "truncated": truncated }))
}

/// identity_check — `git config --get user.name` / `user.email` が両方非空か。
///
/// **未設定は ok:false であってエラーではない** (`error.code=identity_missing` を
/// 返すのは実際に commit を試みたときだけ)。設定 UI は作らない = v1 は案内のみ。
fn identity_check(state: &mut State) -> Result<Value, ErrorBody> {
    let name = config_value(&state.root, "user.name")?;
    let email = config_value(&state.root, "user.email")?;
    Ok(json!({
        "ok": !name.is_empty() && !email.is_empty(),
        "name": name,
        "email": email,
    }))
}

/// `git config --get <key>`。未設定は exit 1 = 空文字列 (エラーにしない)
fn config_value(root: &Path, key: &str) -> Result<String, ErrorBody> {
    let out = git::run(root, &["config", "--get", key])?;
    if !out.success() {
        return Ok(String::new());
    }
    Ok(out.stdout_text())
}
