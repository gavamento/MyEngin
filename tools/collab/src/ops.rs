// op の実装 (M66a = hello / repo_check / status、M66b = hint_changed、
// M66c = stage / unstage / commit / log / diff / identity_check、
// M66d = revert / diff_names、M66e = branches / branch_create / checkout、
// M66f = fetch / pull / push / remote_state + 定期 fetch、
// M66g = conflicts / resolve / merge_abort / continue)。
// op 一覧そのものは spec §4.1 で v1 に凍結済み。ここに無い op は bad_request を返す
// — 「知らない op を黙って ok で返す」と C++ 側が永久に待つ形の不具合になる。

use std::path::{Path, PathBuf};
use std::time::Instant;

use serde_json::{json, Value};

use crate::git;
use crate::porcelain;
use crate::protocol::{self, code, event, ErrorBody};

/// worker スレッドが 1 本だけ持つ状態。**sim にも UI にも触らない**
pub struct State {
    pub root: PathBuf,
    /// hello で受け取る設定 (M66f: worker のタイマーがこの 2 つを読む)
    pub fetch_interval_min: i64,
    pub auto_fetch: bool,
    /// 次に定期 fetch を走らせる時刻。None = 走らせない (auto_fetch が false / CLI)。
    /// ★**worker スレッドのタイマー**で回す (spec §4.0)。専用スレッドを立てない —
    ///   notify の Drop が join しないせいで FreeLibrary を撤去した経緯があり、
    ///   「join できないスレッド」をこれ以上増やすとアンロード時の即死が戻ってくる
    pub next_fetch_at: Option<Instant>,
    /// 直近の remote_state。**中身が同じなら remote_changed を出さない**
    /// (5 分ごとに同じトーストが出ると、本当に誰かが push したときに誰も見なくなる)
    last_remote: Option<Value>,
    /// 直近の背景 fetch の失敗 code。**同じ code が続く間は 1 回だけ**通知する
    /// (spec §4.1「背景 fetch と認証」)。オフラインのまま作業する人に
    /// 5 分ごとの「ネットワークに到達できません」を投げつけないため
    last_fetch_error: String,
    /// 直近に組み立てた status の結果。監視スレッド由来の取り直しで
    /// **同じ物なら通知を出さない**ための比較用 (M66b)。
    /// ★これが無いと、エディタが cache\ の外に書いたどんな 1 バイトでも
    ///   status_changed が飛び、窓が毎秒作り直される
    last_status: Option<Value>,
    /// 直近の HEAD (porcelain の `# branch.oid`)。変化で repo_changed を出す
    last_head: String,
    /// `.git` の絶対パス。**1 回聞いて覚える** (M66g)。
    /// ★status はファイル監視のたびに走るので、そのたびに `rev-parse` を 1 本
    ///   増やすと「エディタが何もしていないのに git が毎回 2 プロセス」になる。
    ///   パスを自前で組み立てないのは worktree / submodule / `.git` がファイルの
    ///   場合まで git 自身に解かせるため (repo_check と同じ理由)
    git_dir: Option<PathBuf>,
}

impl State {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        State {
            root: root.into(),
            fetch_interval_min: 5,
            auto_fetch: true,
            next_fetch_at: None,
            last_remote: None,
            last_fetch_error: String::new(),
            last_status: None,
            last_head: String::new(),
            git_dir: None,
        }
    }

    /// hello / タイマーが使う間隔。**負や巨大な値をそのまま Duration にしない**
    /// (設定ファイルを手で書き換えられる = 0 除算ならぬオーバーフローの口になる)
    pub fn fetch_interval(&self) -> std::time::Duration {
        let minutes = self.fetch_interval_min.clamp(0, 24 * 60) as u64;
        std::time::Duration::from_secs(minutes * 60)
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
        "revert" => revert(state, args),
        "diff_names" => diff_names(state, args),
        "branches" => branches(state),
        "branch_create" => branch_create(state, args),
        "checkout" => checkout(state, args),
        "fetch" => fetch(state),
        "pull" => pull(state, args),
        "push" => push(state, args),
        "remote_state" => remote_state(state),
        // M66g。conflicts は読み取り系、残り 3 本は working tree を書き換える
        "conflicts" => conflicts(state),
        "resolve" => resolve(state, args),
        "merge_abort" => merge_abort(state),
        "continue" => merge_continue(state),
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
    // ★設定の反映は hello の**再送**で行う (spec の M66f)。ここでタイマーを組み直すので、
    //   エディタが歯車で間隔を変えたら次の tick から新しい間隔になる。
    //   `Some(now)` = 「すぐ 1 回」= spec の「起動直後 + 間隔ごと」の起動直後の分。
    //   auto_fetch を切ったら None にして**タイマーごと止める**
    //   (フラグだけ見て走らせない形にすると、切った直後の 1 回が漏れる)
    state.next_fetch_at = if state.auto_fetch { Some(Instant::now()) } else { None };
    let ver = git::version(&state.root)?;
    Ok(json!({ "gitVersion": ver }))
}

/// `.git` の絶対パス (覚えたものがあればそれ)。リポジトリでなければ空。
///
/// ★失敗を覚えない — エディタを開いたままリポジトリを外で `git init` される経路が
///   あり、覚えてしまうと「その回の起動の間は永久にリポジトリ外」になる
fn git_dir(state: &mut State) -> PathBuf {
    if let Some(dir) = &state.git_dir {
        return dir.clone();
    }
    let out = match git::run(&state.root, &["rev-parse", "--absolute-git-dir"]) {
        Ok(o) if o.success() => PathBuf::from(o.stdout_text()),
        _ => return PathBuf::new(),
    };
    state.git_dir = Some(out.clone());
    out
}

/// (マージ中, リベース中)。**status の応答にも載せる** (M66g) —
/// ★競合中は他の書き込み系をゲートで止める (spec §4.1 決定 9) のに、この 2 つを
///   repo_check (= 起動時 1 回) でしか配らないと、**pull が競合した直後のゲートが
///   開いたまま**になる。監視は `.git\MERGE_HEAD` も見ているので、status に載せれば
///   外部での `git merge` も同じ 1 本で伝わる
fn merge_state(state: &mut State) -> (bool, bool) {
    let dir = git_dir(state);
    if dir.as_os_str().is_empty() {
        return (false, false);
    }
    (
        dir.join("MERGE_HEAD").exists(),
        // rebase-merge = 対話/マージ型、rebase-apply = am 型。どちらも「rebase 中」
        dir.join("rebase-merge").exists() || dir.join("rebase-apply").exists(),
    )
}

/// `.git` の中を見るのではなく `git rev-parse` に聞く。worktree / submodule /
/// `.git` がファイル (gitdir: リンク) の場合まで git 自身が面倒を見てくれる
fn repo_check(state: &mut State) -> Result<Value, ErrorBody> {
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
    // 未出生ブランチ (init 直後) では rev-parse HEAD が失敗する = head は空
    let head = git::run(&state.root, &["rev-parse", "HEAD"])?;
    let head = if head.success() { head.stdout_text() } else { String::new() };
    let (merging, rebasing) = merge_state(state);
    Ok(json!({
        "isRepo": true,
        "toplevel": toplevel,
        "head": head,
        "mergeInProgress": merging,
        "rebaseInProgress": rebasing,
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

fn build_status(state: &mut State) -> Result<Value, ErrorBody> {
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
    // ★マージ / リベースの途中かを **status に載せる** (M66g)。ゲート (決定 9) が
    //   これを毎回読むので、repo_check だけで配ると競合した直後に開いたままになる
    let (merging, rebasing) = merge_state(state);
    Ok(json!({
        "branch": info.branch,
        "upstream": info.upstream,
        "ahead": info.ahead,
        "behind": info.behind,
        "mergeInProgress": merging,
        "rebaseInProgress": rebasing,
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

// ---- M66d: revert / diff_names ----

/// revert — 選んだパスを「最後に stage / commit した状態」へ戻す。
///
/// 追跡済みと未追跡で**別の git を打つ**必要がある:
///   * 追跡済み … `git checkout -- <paths>` (index の内容で working tree を上書き)
///   * 未追跡   … `git clean -f -- <paths>` (= ファイルを消す)
/// `checkout` に未追跡パスを混ぜると `error: pathspec ... did not match` で
/// **呼び出しごと**失敗する = 一緒に選んだ追跡済みファイルまで戻らない。
/// 逆に `clean` に追跡済みを混ぜても何も起きない (clean は追跡済みを消さない) ので、
/// 「どちらに振るか」は status を 1 回読んで決める。
///
/// ★消えたファイルは元に戻せない。UI 側 (GitTransaction) が確認モーダルで
///   「未追跡ファイルは削除されます」と明示してから呼ぶこと (spec §4.1)。
fn revert(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let paths = arg_paths(args)?;
    // 未追跡集合を作るために status を 1 回読む。**revert の直前に読む**こと —
    // C++ 側が持っている status は古いことがあり、その差で「消すつもりの無い
    // ファイルを消す」形の事故になる
    let out = git::run(
        &state.root,
        &["status", "--porcelain=v2", "-z", "--branch", "--untracked-files=all"],
    )?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    let info = porcelain::parse_status_v2(&out.stdout);
    let mut tracked: Vec<String> = Vec::new();
    let mut untracked: Vec<String> = Vec::new();
    for p in paths {
        let is_untracked = info
            .entries
            .iter()
            .any(|e| e.path == p && e.index == '?' && !e.conflict);
        if is_untracked {
            untracked.push(p);
        } else {
            tracked.push(p);
        }
    }
    if !tracked.is_empty() {
        // run_per_path_chunk が "--" を足すのでここでは書かない (二重に置くと
        // pathspec "--" を探しに行って失敗する)
        run_per_path_chunk(state, &["checkout"], &tracked)?;
    }
    if !untracked.is_empty() {
        // -f = 「本当に消す」。-d は付けない (ディレクトリごと消すのは v1 の範囲外で、
        // 「1 ファイルを戻したらフォルダが消えた」は取り返しがつかない)
        run_per_path_chunk(state, &["clean", "-q", "-f"], &untracked)?;
    }
    Ok(json!({
        "reverted": tracked.len() + untracked.len(),
        "deleted": untracked.len(),
        "status": status_after_write(state)?,
    }))
}

/// diff_names — 2 つのリビジョン間で名前が動いたファイルの一覧 (段階の**事前**判定用)。
///
/// `from` / `to` は省略可 (既定 `HEAD`)。`..` を使うのは「共通祖先から」ではなく
/// 「今の HEAD から見て何が変わるか」を知りたいため — 段階分類は
/// **working tree に実際に降ってくるファイル**が対象で、履歴の枝分かれは関係ない。
fn diff_names(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let from = rev_arg(args, "from")?;
    let to = rev_arg(args, "to")?;
    let range = format!("{from}..{to}");
    let out = git::run(&state.root, &["diff", "--name-status", "-z", "--no-renames", &range])?;
    // ★--no-renames を付けるのは、リネーム検出の**閾値**が git の設定と版で変わるため。
    //   同じ 2 コミットの差分が機体によって R にも A+D にもなると、段階分類 (R は A 段階、
    //   D は B 段階) が機体依存になる。検出を切って A+D に固定すれば必ず重い側に倒れる
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    let names: Vec<Value> = porcelain::parse_name_status_z(&out.stdout)
        .into_iter()
        .map(|e| {
            let mut o = serde_json::Map::new();
            o.insert("path".into(), Value::String(e.path));
            o.insert("status".into(), Value::String(e.status.to_string()));
            if let Some(old) = e.old_path {
                o.insert("oldPath".into(), Value::String(old));
            }
            Value::Object(o)
        })
        .collect();
    Ok(json!({ "from": from, "to": to, "names": names }))
}

/// リビジョン名の検証。**`-` 始まりと空白を弾く**だけの最小限。
/// git のオプションに化けるのを防ぐのが目的で、実在確認は git に任せる
fn rev_arg(args: &Value, key: &str) -> Result<String, ErrorBody> {
    let raw = args.get(key).and_then(|v| v.as_str()).unwrap_or("HEAD").trim();
    if raw.is_empty() {
        return Ok("HEAD".to_string());
    }
    if raw.starts_with('-') || raw.contains(char::is_whitespace) {
        return Err(ErrorBody::new(code::BAD_REQUEST, format!("bad revision: {raw}")));
    }
    Ok(raw.to_string())
}

// ---- M66e: branches / branch_create / checkout ----

/// ブランチ名の検証 (**必須**引数)。
///
/// ★`checkout` には `--` を置けない — `git checkout -- <name>` は「その**パス**を
///   index から復元する」という**まったく別の操作**になる (working tree を上書きする側)。
///   つまりオプション化を止める最後の砦がここしか無いので、rev_arg より厳しく見る。
///   実在確認と綴りの厳密な検証は git (`check-ref-format` 相当) に任せる
fn branch_name_arg(args: &Value, key: &str) -> Result<String, ErrorBody> {
    let raw = match args.get(key).and_then(|v| v.as_str()) {
        Some(s) => s.trim(),
        None => return Err(ErrorBody::new(code::BAD_REQUEST, format!("{key} is required"))),
    };
    if raw.is_empty() {
        return Err(ErrorBody::new(code::BAD_REQUEST, format!("{key} is empty")));
    }
    if raw.starts_with('-') {
        return Err(ErrorBody::new(code::BAD_REQUEST, format!("name looks like an option: {raw}")));
    }
    if raw.contains(char::is_whitespace) {
        return Err(ErrorBody::new(code::BAD_REQUEST, format!("name has whitespace: {raw}")));
    }
    if raw.split('/').any(|seg| seg == "..") {
        return Err(ErrorBody::new(code::BAD_REQUEST, format!("bad branch name: {raw}")));
    }
    Ok(raw.to_string())
}

/// 今の HEAD の oid。未出生ブランチ (commit が 1 つも無い) は空文字列
fn head_oid(root: &Path) -> Result<String, ErrorBody> {
    let out = git::run(root, &["rev-parse", "HEAD"])?;
    Ok(if out.success() { out.stdout_text() } else { String::new() })
}

/// `refs/<prefix>/<name>` が実在するか。`--verify --quiet` は不在で exit 1 (エラーではない)
fn ref_exists(root: &Path, full_ref: &str) -> Result<bool, ErrorBody> {
    let out = git::run(root, &["rev-parse", "--verify", "--quiet", full_ref])?;
    Ok(out.success())
}

/// branches — ローカルとリモート追跡の一覧 (段階判定の前に「どこへ行けるか」を出す)。
///
/// ★`for-each-ref` に `-z` は無い (git 2.48 で "unknown switch `z'")。フィールドだけ
///   `%00` で区切り、レコードは LF。ref 名に制御文字は入れられないので安全。
///   `refs/remotes/*/HEAD` (symbolic ref) は checkout 先として意味が無いので落とす
fn branches(state: &mut State) -> Result<Value, ErrorBody> {
    toplevel(&state.root)?;
    let out = git::run(
        &state.root,
        &[
            "for-each-ref",
            "--format=%(refname)%00%(refname:short)%00%(objectname)%00%(upstream:short)%00%(HEAD)",
            "refs/heads",
            "refs/remotes",
        ],
    )?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    let mut current = String::new();
    let mut locals: Vec<Value> = Vec::new();
    let mut remotes: Vec<Value> = Vec::new();
    for e in porcelain::parse_for_each_ref(&out.stdout) {
        if e.current {
            current = e.name.clone();
        }
        let row = json!({ "name": e.name, "oid": e.oid, "upstream": e.upstream });
        if e.refname.starts_with("refs/remotes/") {
            if e.refname.ends_with("/HEAD") {
                continue;
            }
            remotes.push(row);
        } else {
            locals.push(row);
        }
    }
    // detached HEAD では `%(HEAD)` が 1 件も立たない = current は空のまま
    // (UI は「(detached)」を出す。ここで嘘の名前を作らない)
    Ok(json!({ "current": current, "locals": locals, "remotes": remotes }))
}

/// branch_create — `git branch <name> <from>` (既定 `from = HEAD`)。
/// **working tree は 1 バイトも動かない**ので、段階判定もトランザクションも要らない
fn branch_create(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let name = branch_name_arg(args, "name")?;
    let from = rev_arg(args, "from")?;
    let out = git::run(&state.root, &["branch", &name, &from])?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    Ok(json!({ "name": name, "status": status_after_write(state)? }))
}

/// checkout — ブランチを切り替え、**この 1 回の応答で「何が変わったか」まで返す**。
///
/// ★変更集合 (`diff --name-status <before>..<after>`) を C++ からもう 1 往復させない。
///   させると、checkout と diff の間に別の要求 (監視由来の status など) が挟まりうるし、
///   何より「切り替わったが、何が変わったかは分からない」中途半端な状態が 1 フレーム
///   でも作れてしまう。段階 A/B/C の判定はこの names が唯一の入力なので、
///   **切り替えと同じ応答で確定させる**のが安全側。
fn checkout(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let name = branch_name_arg(args, "name")?;
    let before = head_oid(&state.root)?;

    // ローカルに同名が無く、リモート追跡だけある = 追跡ブランチを作って乗る。
    // ★素の `git checkout origin/x` は **detached HEAD** になる (コミットしても
    //   どのブランチにも残らない = 作業が迷子になる最悪の形)
    let local_exists = ref_exists(&state.root, &format!("refs/heads/{name}"))?;
    let remote_exists = !local_exists && ref_exists(&state.root, &format!("refs/remotes/{name}"))?;
    let mut argv: Vec<&str> = vec!["checkout"];
    if remote_exists {
        argv.push("-t");
    }
    argv.push(name.as_str());
    let out = git::run(&state.root, &argv)?;
    if !out.success() {
        let mut err = git::classify_error(&out);
        if err.code == code::LOCAL_CHANGES_OVERWRITTEN {
            let paths = porcelain::parse_overwritten_paths(&err.detail);
            if !paths.is_empty() {
                // ★detail を**自分の文言に差し替える**。git の案内文
                //   ("Please commit your changes or stash them before you switch branches")
                //   は版で変わるので、そのまま載せると期待 NDJSON が git の更新で割れる。
                //   ユーザーが要るのは「どのファイルか」= paths の方
                err = ErrorBody::with_paths(
                    code::LOCAL_CHANGES_OVERWRITTEN,
                    "local changes would be overwritten by checkout",
                    paths,
                );
            }
        }
        return Err(err);
    }

    let after = head_oid(&state.root)?;
    let names = changed_names(state, &before, &after)?;
    let status = status_after_write(state)?;
    let branch = status.get("branch").and_then(|b| b.as_str()).unwrap_or("").to_string();
    Ok(json!({ "branch": branch, "head": after, "names": names, "status": status }))
}

/// `before` から `after` へ移ったときに working tree で入れ替わったファイル。
///
/// `before` が空 = 未出生ブランチから乗り換えた (clone 直後 + fetch の形)。
/// この場合 `diff` は使えないので **`ls-tree` で「全部 A」**にする。
/// ★ここで空を返すと段階 A (= 何もしない) と読まれる。実際にはファイルが
///   丸ごと降ってきているので、シーンもアセットも古いまま動き続ける
fn changed_names(state: &State, before: &str, after: &str) -> Result<Vec<Value>, ErrorBody> {
    if after.is_empty() {
        return Ok(Vec::new());
    }
    if before.is_empty() {
        let out = git::run(&state.root, &["ls-tree", "-r", "-z", "--name-only", after])?;
        if !out.success() {
            return Err(git::classify_error(&out));
        }
        return Ok(porcelain::parse_z_paths(&out.stdout)
            .into_iter()
            .map(|p| json!({ "path": p, "status": "A" }))
            .collect());
    }
    if before == after {
        return Ok(Vec::new()); // 同じコミットを指すブランチへ移った (よくある)
    }
    let range = format!("{before}..{after}");
    // --no-renames の理由は diff_names と同じ (検出閾値が機体依存になるのを断つ)
    let out = git::run(&state.root, &["diff", "--name-status", "-z", "--no-renames", &range])?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    Ok(porcelain::parse_name_status_z(&out.stdout)
        .into_iter()
        .map(|e| {
            let mut o = serde_json::Map::new();
            o.insert("path".into(), Value::String(e.path));
            o.insert("status".into(), Value::String(e.status.to_string()));
            if let Some(old) = e.old_path {
                o.insert("oldPath".into(), Value::String(old));
            }
            Value::Object(o)
        })
        .collect())
}

// ---- M66f: fetch / pull / push / remote_state + 定期 fetch ----

/// 「upstream に何が来ているか」の一覧に載せる上限 (spec の M66f: 最大 20 件)。
/// ★全部返さない理由: 長期間 pull していないブランチでは数百件になり、
///   窓の帯に流し込むだけで整形が効いてくる。20 件あれば
///   「誰が何を入れたか」は十分伝わる
const MAX_REMOTE_COMMITS: i64 = 20;

/// 現在のブランチ名 (detached HEAD では "HEAD")。push の `-u origin <branch>` に使う
fn current_branch(root: &Path) -> Result<String, ErrorBody> {
    let out = git::run(root, &["rev-parse", "--abbrev-ref", "HEAD"])?;
    Ok(if out.success() { out.stdout_text() } else { String::new() })
}

/// 追跡先の短縮名 ("origin/main")。**未設定はエラーではなく空文字列**。
///
/// ★実測: upstream が無いと `rev-parse @{u}` は exit 128 +
///   `fatal: no upstream configured for branch 'main'` を返す。これを err にすると
///   「まだ 1 回も push していないブランチ」で窓が赤くなる = 直しようの無い警告になる
fn upstream_name(root: &Path) -> Result<String, ErrorBody> {
    let out = git::run(root, &["rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"])?;
    Ok(if out.success() { out.stdout_text() } else { String::new() })
}

/// リモートが 1 つでも設定されているか (`git remote` の出力が非空)。
/// ★UI が push のボタンを塞ぐ判断に使う。無いまま push すると
///   `fatal: No configured push destination.` = git_failed で返ってくるだけで、
///   ユーザーには「押したのに謎のエラー」にしか見えない
fn has_remote(root: &Path) -> Result<bool, ErrorBody> {
    let out = git::run(root, &["remote"])?;
    Ok(out.success() && !out.stdout_text().is_empty())
}

/// `HEAD...@{u}` の左右件数 = (ahead, behind)。upstream が無ければ (0, 0)。
/// ★`...` (3 点) であること。`..` だと片側しか数えない
fn ahead_behind(root: &Path) -> Result<(i64, i64), ErrorBody> {
    let out = git::run(root, &["rev-list", "--left-right", "--count", "HEAD...@{u}"])?;
    if !out.success() {
        return Ok((0, 0));
    }
    let text = out.stdout_text();
    let mut it = text.split_whitespace();
    let ahead = it.next().and_then(|s| s.parse::<i64>().ok()).unwrap_or(0);
    let behind = it.next().and_then(|s| s.parse::<i64>().ok()).unwrap_or(0);
    Ok((ahead, behind))
}

/// `HEAD..@{u}` = 「まだ手元に無いコミット」。log と同じ整形で返す
fn incoming_commits(root: &Path) -> Result<Vec<Value>, ErrorBody> {
    let n = MAX_REMOTE_COMMITS.to_string();
    let out = git::run(
        root,
        &["log", "-n", &n, "--format=%H%x00%an%x00%aI%x00%s", "-z", "HEAD..@{u}"],
    )?;
    if !out.success() {
        // upstream が無い / 未出生ブランチ。**空で返す** (エラーにしない)
        return Ok(Vec::new());
    }
    Ok(porcelain::parse_log_z(&out.stdout)
        .into_iter()
        .map(|e| json!({ "sha": e.sha, "author": e.author, "date": e.date, "subject": e.subject }))
        .collect())
}

/// remote_state の本体。**last_remote は更新しない** (更新するのは
/// 「ユーザーに見せた」ことが確定する fetch / remote_state / 背景 fetch の側)
fn build_remote_state(state: &State) -> Result<Value, ErrorBody> {
    toplevel(&state.root)?;
    let upstream = upstream_name(&state.root)?;
    let (ahead, behind) = ahead_behind(&state.root)?;
    let commits = if upstream.is_empty() { Vec::new() } else { incoming_commits(&state.root)? };
    Ok(json!({
        "upstream": upstream,
        "hasRemote": has_remote(&state.root)?,
        "ahead": ahead,
        "behind": behind,
        "commits": commits,
    }))
}

/// remote_state — 読み取り系。窓の帯 (「upstream に N 件の新しいコミット」) の入力
fn remote_state(state: &mut State) -> Result<Value, ErrorBody> {
    let value = build_remote_state(state)?;
    state.last_remote = Some(value.clone());
    Ok(value)
}

/// fetch — `git fetch --prune`。**ユーザーが押したとき用**なので GCM の GUI を許す
/// (`git::run` = GIT_TERMINAL_PROMPT=0 のみ)。背景の定期 fetch は background_fetch。
///
/// ★リモートが 1 つも無いリポジトリでも `git fetch` は **exit 0 + 無出力**で返る
///   (実測 git 2.48.1)。特別扱いは要らない
fn fetch(state: &mut State) -> Result<Value, ErrorBody> {
    toplevel(&state.root)?;
    let out = git::run(&state.root, &["fetch", "--prune"])?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    let remote = build_remote_state(state)?;
    state.last_remote = Some(remote.clone());
    Ok(json!({ "remote": remote, "status": status_after_write(state)? }))
}

/// pull — 既定は `--ff-only`、`allowMerge=true` で `--no-rebase` (マージを作る)。
///
/// 応答は checkout と**同じ型** (`{head, names, status}`) にしてある (spec §4.1
/// 「ブランチ周り」)。C++ 側は段階 A/B/C の判定を `names` 1 本から行うので、
/// 「working tree を入れ替える op」はすべてこの形で返すのが契約。
///
/// ★rebase を既定にしない。rebase 中の中断状態 (rebase-apply) をエディタから
///   復帰させる手段が v1 に無く、ゲートが永久に閉じたリポジトリができる
fn pull(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    toplevel(&state.root)?;
    let allow_merge = args.get("allowMerge").and_then(|v| v.as_bool()).unwrap_or(false);
    let before = head_oid(&state.root)?;
    let mode = if allow_merge { "--no-rebase" } else { "--ff-only" };
    let out = git::run(&state.root, &["pull", mode])?;
    if !out.success() {
        let mut err = classify_pull_failure(&out);
        if err.code == code::CONFLICT {
            // ★競合したファイルは **status の `u` レコード**から採る (spec §4.1)。
            //   git の案内文 (`CONFLICT (modify/delete): ...`) は種別ごとに形が
            //   違ううえ版で変わるので、1 バイトも当てにしない。
            //   ここで載せておくと、UI は失敗モーダルにそのまま一覧を出せる
            if let Ok(list) = unmerged(state) {
                if !list.is_empty() {
                    err.paths = Some(list.into_iter().map(|e| e.path).collect());
                }
            }
        }
        return Err(err);
    }
    let after = head_oid(&state.root)?;
    let names = changed_names(state, &before, &after)?;
    let status = status_after_write(state)?;
    let remote = build_remote_state(state)?;
    state.last_remote = Some(remote.clone());
    Ok(json!({ "head": after, "names": names, "status": status, "remote": remote }))
}

/// pull の失敗を error.code へ。
///
/// ★マージの競合は **stdout** に出る (`CONFLICT (content): Merge conflict in x` /
///   `Automatic merge failed; fix conflicts and then commit the result.`)。
///   classify_error は stderr しか見ないので、commit と同じくここで両方を読む。
///   拾い損ねると `git_failed` に化け、リポジトリがマージ途中で止まっているのに
///   UI は「git が失敗しました」としか言わない = 一番危ない見落とし方になる。
/// ★detail は**固定文**にする。競合したファイルは sub-07 の `conflicts` op が
///   `git status` の未マージ行から取る — git の案内文を解析すると
///   `CONFLICT (modify/delete)` のような別形で必ず外れる
/// `non_fast_forward` の detail を**固定文へ差し替える**。
///
/// ★git の原文には短縮 SHA (`From ../origin  a609161..68eeffa main -> origin/main`) と
///   版で変わる hint (`Disable this message with "git config set advice.diverging false"`)
///   が載る。そのまま返すと collab_verify の期待 NDJSON が**毎回**赤くなる
///   (実際に 1 度撮って気付いた)。UI は既知 code を Tr() の文言に置き換えるので
///   detail は表示に使われない = 固定文にして失うものは無い。
///   分類できなかった失敗 (`git_failed`) は今までどおり stderr 全文を載せる —
///   そちらは「何が起きたか分からない」を避ける方が大事
fn stable_non_fast_forward(err: ErrorBody) -> ErrorBody {
    if err.code == code::NON_FAST_FORWARD {
        return ErrorBody::new(
            code::NON_FAST_FORWARD,
            "the remote has commits that are not in this branch",
        );
    }
    err
}

fn classify_pull_failure(out: &git::GitOutput) -> ErrorBody {
    let mut text = out.stderr_text();
    if !text.is_empty() {
        text.push('\n');
    }
    text.push_str(&out.stdout_text());
    let low = text.to_ascii_lowercase();
    if low.contains("automatic merge failed") || low.contains("merge conflict in") {
        return ErrorBody::new(code::CONFLICT, "the merge produced conflicts");
    }
    let mut err = git::classify_error(out);
    if err.code == code::LOCAL_CHANGES_OVERWRITTEN {
        let paths = porcelain::parse_overwritten_paths(&err.detail);
        if !paths.is_empty() {
            // checkout と同じ扱い (spec S7): 案内文ではなく **paths[]** が正
            err = ErrorBody::with_paths(
                code::LOCAL_CHANGES_OVERWRITTEN,
                "local changes would be overwritten by pull",
                paths,
            );
        }
    }
    if err.detail.is_empty() {
        err.detail = text.trim().to_string();
    }
    stable_non_fast_forward(err)
}

/// push — upstream が無ければ `-u origin <branch>` で作ってから押す。
///
/// ★upstream が無いまま素の `git push` を打つと
///   `fatal: The current branch X has no upstream branch.` で止まる。
///   「初めての push」はチーム作業で最も普通の操作なので、ここで自動的に
///   追跡を張る (`setUpstream` はそれを明示的に要求する口)
fn push(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    toplevel(&state.root)?;
    let set_upstream = args.get("setUpstream").and_then(|v| v.as_bool()).unwrap_or(false);
    let upstream = upstream_name(&state.root)?;
    let branch = current_branch(&state.root)?;
    let need_upstream = set_upstream || upstream.is_empty();
    if need_upstream && (branch.is_empty() || branch == "HEAD") {
        // detached HEAD。**どのブランチにも属さないコミットを push しようとしている**
        return Err(ErrorBody::new(code::BAD_REQUEST, "cannot push a detached HEAD"));
    }
    let mut argv: Vec<&str> = vec!["push"];
    if need_upstream {
        argv.push("-u");
        argv.push("origin");
        argv.push(branch.as_str());
    }
    let out = git::run(&state.root, &argv)?;
    if !out.success() {
        return Err(stable_non_fast_forward(git::classify_error(&out)));
    }
    let remote = build_remote_state(state)?;
    state.last_remote = Some(remote.clone());
    Ok(json!({ "remote": remote, "status": status_after_write(state)? }))
}

/// 定期 fetch の本体 (worker のタイマーから呼ばれる)。**通知行だけ**を返す。
///
/// 認証は `git::run_background` = `GIT_TERMINAL_PROMPT=0` + `GCM_INTERACTIVE=never`。
/// 誰も見ていない 5 分ごとの fetch が資格情報ダイアログを積み上げるのを防ぐ
/// (実測はその関数のコメント)。
pub fn background_fetch(state: &mut State) -> Vec<String> {
    let mut lines = Vec::new();
    if toplevel(&state.root).is_err() {
        // リポジトリでなくなった (フォルダごと移動された等)。**黙って諦める** —
        // 通知経路でエラーを出すと、リポジトリ外のプロジェクトで 5 分ごとにトーストが出る
        return lines;
    }
    let out = match git::run_background(&state.root, &["fetch", "--prune"]) {
        Ok(o) => o,
        Err(e) => {
            push_fetch_error(state, e, &mut lines);
            return lines;
        }
    };
    if !out.success() {
        push_fetch_error(state, git::classify_error(&out), &mut lines);
        return lines;
    }
    // 成功したら「次に失敗したらまた 1 回知らせる」状態へ戻す
    state.last_fetch_error.clear();
    // ahead/behind は status にも載っているので、監視経路と同じ 1 本に流す
    lines.extend(refresh_status(state));
    if let Ok(remote) = build_remote_state(state) {
        if state.last_remote.as_ref() != Some(&remote) {
            state.last_remote = Some(remote.clone());
            lines.push(protocol::event_line(event::REMOTE_CHANGED, json!({ "remote": remote })));
        }
    }
    lines
}

/// 背景 fetch の失敗通知。**同じ code が続く間は 1 回だけ** (spec §4.1)
fn push_fetch_error(state: &mut State, err: ErrorBody, lines: &mut Vec<String>) {
    if state.last_fetch_error == err.code {
        return;
    }
    state.last_fetch_error = err.code.clone();
    lines.push(protocol::event_line(event::REMOTE_CHANGED, json!({ "error": err })));
}

// ---- M66g: 競合 (conflicts / resolve / merge_abort / continue) ----

const SIDE_OURS: &str = "ours";
const SIDE_THEIRS: &str = "theirs";

/// 未マージ (`u` レコード) の一覧。
///
/// ★git の案内文 (`CONFLICT (modify/delete): ...`) を解析しない。あれは種別ごとに
///   形が違ううえ版で変わる。porcelain v2 の `u` レコードが唯一の正本 (spec §4.1)
fn unmerged(state: &State) -> Result<Vec<porcelain::UnmergedEntry>, ErrorBody> {
    let out = git::run(&state.root, &["status", "--porcelain=v2", "-z"])?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    Ok(porcelain::parse_unmerged(&out.stdout))
}

/// HEAD と working tree の差 (パスと A/M/D)。
///
/// マージ競合の最中に呼ぶと「このマージがきれいに入れたファイル」+「元から
/// あった未コミット変更」が返る (未マージのファイルは呼び手が差し引く)。
/// ★元からあった未コミット変更まで混ざるのは避けられない (git は両者を区別しない)。
///   混ざった分は「変わっていないファイルをもう一度読み直す」だけで実害が無い方に倒す
fn diff_head_names(state: &State) -> Result<Vec<porcelain::NameStatusEntry>, ErrorBody> {
    let out = git::run(&state.root, &["diff", "--name-status", "-z", "--no-renames", "HEAD"])?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    Ok(porcelain::parse_name_status_z(&out.stdout))
}

/// conflicts — 競合一覧と「競合せずにマージ済みのファイル」。**読み取り系**。
///
/// merged を返す理由: 競合した pull でも、競合しなかったファイルは既にディスクへ
/// 書かれている。エディタは一括適用 (ReloadHub) の最中なので、この一覧を返して
/// やらないと、それらの変更が 1 件も反映されないまま競合の解決に入ることになる
fn conflicts(state: &mut State) -> Result<Value, ErrorBody> {
    toplevel(&state.root)?;
    let list = unmerged(state)?;
    let (merging, rebasing) = merge_state(state);
    let conflicted: Vec<Value> = list
        .iter()
        .map(|e| {
            json!({
                "path": e.path,
                "kind": e.kind(),
                // ★「その側の版が実在するか」まで返す。UI が ours / theirs の
                //   ボタンを「消す」と読み替えて出せる (modify/delete の競合)
                "ours": e.has_ours,
                "theirs": e.has_theirs,
            })
        })
        .collect();
    let merged: Vec<Value> = if merging || rebasing {
        diff_head_names(state)?
            .into_iter()
            .filter(|n| !list.iter().any(|u| u.path == n.path))
            .map(|n| json!({ "path": n.path, "status": n.status.to_string() }))
            .collect()
    } else {
        // マージ中でなければ「マージ済み」という概念が無い。ここで HEAD との差を
        // 返すと、ただの未コミット変更が「マージが入れたもの」として読まれる
        Vec::new()
    };
    Ok(json!({
        "conflicts": conflicted,
        "merged": merged,
        "mergeInProgress": merging,
        "rebaseInProgress": rebasing,
    }))
}

/// resolve — 競合したファイルを ours / theirs のどちらかで確定する。
///
/// ★`paths` は配列で受ける (sub-07 の `{path, side}` から変更)。本体と `.meta` は
///   対で解決しないと「本体は theirs、`.meta` は競合のまま」という中途半端な状態が
///   1 往復ぶん存在してしまう。stage / revert と同じ `arg_paths` を通せる利点もある。
/// ★どちらの版を採るかの分岐は XY の文字ではなく**モード** (has_ours / has_theirs)。
///   相手が消したファイル (`UD`) に `checkout --theirs` を打つと
///   `error: path 'x' does not have their version` で落ちる (実測)。
///   その場合の「theirs を採る」は `git rm` = 消す、が正しい意味になる
fn resolve(state: &mut State, args: &Value) -> Result<Value, ErrorBody> {
    let paths = arg_paths(args)?;
    let side = args.get("side").and_then(|v| v.as_str()).unwrap_or("");
    if side != SIDE_OURS && side != SIDE_THEIRS {
        return Err(ErrorBody::new(code::BAD_REQUEST, "side must be ours or theirs"));
    }
    let list = unmerged(state)?;
    let mut resolved = 0;
    for p in &paths {
        // 競合していないパスは黙って飛ばす — 対で送られてくる `.meta` は
        // 片方だけが競合していることの方が多い
        let entry = match list.iter().find(|e| &e.path == p) {
            Some(e) => e,
            None => continue,
        };
        let keep = if side == SIDE_OURS { entry.has_ours } else { entry.has_theirs };
        let one = std::slice::from_ref(p);
        if keep {
            let flag = if side == SIDE_OURS { "--ours" } else { "--theirs" };
            run_per_path_chunk(state, &["checkout", flag], one)?;
            // ★checkout だけでは index は未マージのまま (実測)。add まで打って
            //   初めて「解決した」ことになる
            run_per_path_chunk(state, &["add"], one)?;
        } else {
            // 採る側の版が無い = そちらでは削除されている。`git rm` が
            // index と working tree の両方から落とす
            run_per_path_chunk(state, &["rm", "-q"], one)?;
        }
        resolved += 1;
    }
    if resolved == 0 {
        // ★黙って ok を返さない。「解決したつもりで何も起きていない」は
        //   一覧が減らない形でしか気付けず、原因が読めない
        return Err(ErrorBody::new(code::BAD_REQUEST, "none of the given paths are unmerged"));
    }
    Ok(json!({ "resolved": resolved, "side": side, "status": status_after_write(state)? }))
}

/// 実行前後の「ディスクに在るか」で A / D / M を決める (M66g)。
///
/// ★`merge --abort` は HEAD を動かさないので `diff <before>..<after>` は必ず空。
///   それを names として返すと「何も変わっていない」= 段階 A で何もしない、と
///   読まれるが、実際にはファイルが丸ごと入れ替わっている
fn names_by_existence(state: &State, paths: &[String], before: &[bool]) -> Vec<Value> {
    let mut out = Vec::new();
    for (i, p) in paths.iter().enumerate() {
        let existed = before.get(i).copied().unwrap_or(false);
        let exists = state.root.join(p).exists();
        let status = match (existed, exists) {
            (false, true) => "A",
            (true, false) => "D",
            (true, true) => "M",
            (false, false) => continue, // 元から無く今も無い = 何も起きていない
        };
        out.push(json!({ "path": p, "status": status }));
    }
    out
}

/// merge_abort — マージ (無ければリベース) を中止して pull 前の状態へ戻す。
///
/// 応答は pull と同じ型 (`{head, names, status, remote}`)。C++ 側は
/// `GitTransaction` の OpKind を 1 つ足すだけで、段階分類から後処理まで同じ 1 本を通る
fn merge_abort(state: &mut State) -> Result<Value, ErrorBody> {
    toplevel(&state.root)?;
    let (merging, rebasing) = merge_state(state);
    if !merging && !rebasing {
        return Err(ErrorBody::new(code::BAD_REQUEST, "no merge or rebase is in progress"));
    }
    // 中止でディスクが動きうるのは「HEAD と違うもの」+「未マージのもの」。
    // 前者にはマージが入れたファイルも元からの未コミット変更も入る (後者は
    // 中止しても動かない = 実行前後で在り方が同じなので M として出るだけ)
    let mut candidates: Vec<String> =
        diff_head_names(state)?.into_iter().map(|n| n.path).collect();
    for e in unmerged(state)? {
        candidates.push(e.path);
    }
    candidates.sort();
    candidates.dedup();
    let before: Vec<bool> = candidates.iter().map(|p| state.root.join(p).exists()).collect();
    let argv: &[&str] = if merging { &["merge", "--abort"] } else { &["rebase", "--abort"] };
    let out = git::run(&state.root, argv)?;
    if !out.success() {
        return Err(git::classify_error(&out));
    }
    let names = names_by_existence(state, &candidates, &before);
    let head = head_oid(&state.root)?;
    let status = status_after_write(state)?;
    let remote = build_remote_state(state)?;
    state.last_remote = Some(remote.clone());
    Ok(json!({ "head": head, "names": names, "status": status, "remote": remote }))
}

/// continue — 全件解決済みのマージを 1 コミットにして閉じる。
///
/// ★`--no-edit` を付けないとエディタ (`GIT_EDITOR`) が起動する = GUI の無い
///   孫プロセスで開かれ、応答が永久に返らない。付ければ `MERGE_MSG` を
///   そのまま使って即コミットする (実測 git 2.48.1)
fn merge_continue(state: &mut State) -> Result<Value, ErrorBody> {
    toplevel(&state.root)?;
    let (merging, rebasing) = merge_state(state);
    if rebasing {
        // v1 は rebase を始めない (spec §4.1 の pull は --no-rebase)。外で始まった
        // リベースの続行はエディタが要る経路なので受けない — 中止だけ受ける
        return Err(ErrorBody::new(
            code::BAD_REQUEST,
            "a rebase cannot be continued from the editor - abort it or finish it in a terminal",
        ));
    }
    if !merging {
        return Err(ErrorBody::new(code::BAD_REQUEST, "no merge is in progress"));
    }
    let left = unmerged(state)?;
    if !left.is_empty() {
        // 残りのパスを返す。UI は「まだ N 件残っています」を出せる
        return Err(ErrorBody::with_paths(
            code::MERGE_IN_PROGRESS,
            "some files are still unmerged",
            left.into_iter().map(|e| e.path).collect(),
        ));
    }
    let before = head_oid(&state.root)?;
    let out = git::run(&state.root, &["commit", "--no-edit"])?;
    if !out.success() {
        // commit と同じく stdout も見る (git commit は失敗理由をそちらに書く)
        let mut text = out.stderr_text();
        if !text.is_empty() {
            text.push('\n');
        }
        text.push_str(&out.stdout_text());
        let mut err = git::classify_error(&out);
        if err.detail.is_empty() {
            err.detail = text.trim().to_string();
        }
        return Err(err);
    }
    let after = head_oid(&state.root)?;
    // ★変更集合は「マージ前の HEAD → マージコミット」。continue の瞬間に
    //   ディスクは動かない (解決のときに動いている) が、その差分は競合の間に
    //   一括適用 (ReloadHub) の外で起きているので、ここでまとめて配り直す方が
    //   取りこぼしが無い
    let names = changed_names(state, &before, &after)?;
    let status = status_after_write(state)?;
    let remote = build_remote_state(state)?;
    state.last_remote = Some(remote.clone());
    Ok(json!({ "head": after, "names": names, "status": status, "remote": remote }))
}
