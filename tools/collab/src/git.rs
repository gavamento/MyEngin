// git.exe の呼び出し口。**git を起動するのはこのファイルだけ**にしてある
// (共通引数と env を 1 箇所に閉じ込めないと、op を足すたびに「この op だけ
// ロケール依存のエラー文が返る」形で静かに崩れる)。

use std::io::{ErrorKind, Write};
use std::path::Path;
use std::process::{Command, Stdio};

use crate::protocol::{code, ErrorBody};

#[cfg(windows)]
struct SetupJob(*mut std::ffi::c_void);

#[cfg(windows)]
#[link(name = "kernel32")]
extern "system" {
    fn CreateJobObjectW(attributes: *mut std::ffi::c_void, name: *const u16) -> *mut std::ffi::c_void;
    fn AssignProcessToJobObject(job: *mut std::ffi::c_void, process: *mut std::ffi::c_void) -> i32;
    fn TerminateJobObject(job: *mut std::ffi::c_void, code: u32) -> i32;
    fn CloseHandle(handle: *mut std::ffi::c_void) -> i32;
}

#[cfg(windows)]
impl SetupJob {
    fn attach(child: &std::process::Child) -> Result<Self, ErrorBody> {
        use std::os::windows::io::AsRawHandle;
        let handle = unsafe { CreateJobObjectW(std::ptr::null_mut(), std::ptr::null()) };
        if handle.is_null() { return Err(spawn_error(std::io::Error::last_os_error())); }
        let job = Self(handle);
        if unsafe { AssignProcessToJobObject(job.0, child.as_raw_handle()) } == 0 {
            return Err(spawn_error(std::io::Error::last_os_error()));
        }
        Ok(job)
    }
    fn terminate(&self) { unsafe { TerminateJobObject(self.0, 1); } }
}

#[cfg(windows)]
impl Drop for SetupJob {
    fn drop(&mut self) { self.terminate(); unsafe { CloseHandle(self.0); } }
}

/// Setup commands have a real process deadline; the UI must not release its lock
/// while a timed-out authentication helper is still running.
pub fn run_setup(cwd: &Path, args: &[&str], seconds: u64,
                 cancel: &std::sync::atomic::AtomicU64, generation: u64,
                 interactive: bool) -> Result<GitOutput, ErrorBody> {
    use std::io::Read;
    use std::sync::atomic::Ordering;
    if cancel.load(Ordering::SeqCst) >= generation {
        return Err(ErrorBody::new("cancelled", "cancelled"));
    }
    let mut cmd = build(cwd, args)?;
    cmd.env("GCM_INTERACTIVE", if interactive { "always" } else { "never" });
    let mut child = cmd.spawn().map_err(spawn_error)?;
    #[cfg(windows)]
    let job = match SetupJob::attach(&child) {
        Ok(job) => job,
        Err(error) => { let _ = child.kill(); let _ = child.wait(); return Err(error); }
    };
    let stdout = child.stdout.take();
    let stderr = child.stderr.take();
    let read = |pipe: Option<Box<dyn Read + Send>>| std::thread::spawn(move || {
        let mut bytes = Vec::new();
        if let Some(mut p) = pipe {
            let mut block = [0u8; 4096];
            while let Ok(n) = p.read(&mut block) {
                if n == 0 { break; }
                if bytes.len() < 262144 { bytes.extend_from_slice(&block[..n]); }
            }
        }
        bytes
    });
    let out = read(stdout.map(|p| Box::new(p) as Box<dyn Read + Send>));
    let err = read(stderr.map(|p| Box::new(p) as Box<dyn Read + Send>));
    let started = std::time::Instant::now();
    let mut stopped = None;
    let status = loop {
        if let Some(status) = child.try_wait().map_err(spawn_error)? { break status; }
        if cancel.load(Ordering::SeqCst) >= generation || started.elapsed().as_secs() >= seconds {
            stopped = Some(if cancel.load(Ordering::SeqCst) >= generation { "cancelled" } else { "timeout" });
            #[cfg(windows)]
            job.terminate();
            let _ = child.kill();
            break child.wait().map_err(spawn_error)?;
        }
        std::thread::sleep(std::time::Duration::from_millis(50));
    };
    #[cfg(windows)]
    job.terminate();
    let stdout = out.join().unwrap_or_default();
    let stderr = err.join().unwrap_or_default();
    if let Some(reason) = stopped { return Err(ErrorBody::new(reason, reason)); }
    Ok(GitOutput { status: status.code().unwrap_or(-1), stdout, stderr })
}

/// `git status --porcelain=v2` が使える最小バージョン。
///
/// 出典: https://raw.githubusercontent.com/git/git/v2.11.0/Documentation/git-status.txt
///   "Version 2 format adds more detailed information about the state of
///    the worktree and changed items.  Version 2 also defines an extensible
///    set of easy to parse optional headers."
///   "# branch.ab +<ahead> -<behind>           If upstream is set and
///                                             the commit is present."
/// 対照: https://raw.githubusercontent.com/git/git/v2.10.0/Documentation/git-status.txt
///   には --porcelain=<version> も "Porcelain Format Version 2" も存在しない。
/// -z (NUL 終端) の v2 対応も同じ 2.11.0 から。
pub const MIN_MAJOR: u32 = 2;
pub const MIN_MINOR: u32 = 11;

/// CREATE_NO_WINDOW。エディタから呼ぶと git のコンソールが一瞬ちらつくため
/// (bin/ 直下の Editor.exe は GUI サブシステム = 本来コンソールを持たない)
#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

pub struct GitOutput {
    pub status: i32,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
}

impl GitOutput {
    pub fn success(&self) -> bool {
        self.status == 0
    }

    /// stderr を UTF-8 として読む (git は LC_ALL=C でも UTF-8 のパスをそのまま出す)。
    /// 不正バイトは置換文字にする — エラー文の**表示**にしか使わないので落とさない
    pub fn stderr_text(&self) -> String {
        String::from_utf8_lossy(&self.stderr).trim().to_string()
    }

    pub fn stdout_text(&self) -> String {
        String::from_utf8_lossy(&self.stdout).trim().to_string()
    }
}

/// git を 1 回叩く。`cwd` はリポジトリ内のディレクトリ。
///
/// 共通引数と env の意図 (spec §4.1):
///   -c core.quotepath=false … 非 ASCII パスを \NNN でエスケープさせない (UTF-8 生で欲しい)
///   -c color.ui=false       … 開発者の color.ui=always に引きずられて ANSI 混じりにしない
///   --no-pager              … less が起動して**永久に待つ**のを防ぐ
///   LC_ALL=C                … エラー文の照合 (error.code 分類) を言語設定から独立させる
///   GIT_TERMINAL_PROMPT=0   … 認証プロンプトで固まらない (stdin は NUL なので答えられない)
pub fn run(cwd: &Path, args: &[&str]) -> Result<GitOutput, ErrorBody> {
    let mut cmd = build(cwd, args)?;
    match cmd.output() {
        Ok(out) => Ok(GitOutput {
            // 異常終了 (シグナル相当) は -1 にまとめる。呼び出し側は success() しか見ない
            status: out.status.code().unwrap_or(-1),
            stdout: out.stdout,
            stderr: out.stderr,
        }),
        Err(e) => Err(spawn_error(e)),
    }
}

/// stdin へ `input` を流してから git を待つ (`git commit -F -` 用、M66c)。
///
/// なぜ `-m <msg>` ではなく `-F -` か: コミット本文は複数行で、引用符も `%` も
/// バックスラッシュも入りうる。コマンドライン経由だと Windows の引数分解と
/// git のオプション解釈の**両方**を通るので、`-` で始まる行が混ざっただけで
/// 別の意味になる。stdin なら中身は 1 バイトも解釈されない。
pub fn run_with_stdin(cwd: &Path, args: &[&str], input: &[u8]) -> Result<GitOutput, ErrorBody> {
    let mut cmd = build(cwd, args)?;
    cmd.stdin(Stdio::piped());
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => return Err(spawn_error(e)),
    };
    if let Some(mut stdin) = child.stdin.take() {
        // ★書き終えたら**必ず閉じる** (drop)。閉じないと git は EOF を待ち続け、
        //   worker スレッドが 1 本しかない以上そこで全体が固まる
        let _ = stdin.write_all(input);
        drop(stdin);
    }
    match child.wait_with_output() {
        Ok(out) => Ok(GitOutput {
            status: out.status.code().unwrap_or(-1),
            stdout: out.stdout,
            stderr: out.stderr,
        }),
        Err(e) => Err(ErrorBody::new(code::GIT_FAILED, format!("git failed: {e}"))),
    }
}

/// git の起動失敗を error.code へ。**NotFound だけ**を「git が無い」に落とす
fn spawn_error(e: std::io::Error) -> ErrorBody {
    if e.kind() == ErrorKind::NotFound {
        ErrorBody::new(code::GIT_MISSING, "git was not found on PATH")
    } else {
        ErrorBody::new(code::GIT_FAILED, format!("cannot start git: {e}"))
    }
}

/// 背景 (定期 fetch) 専用。**認証プロンプトを完全に殺す** (M66f、spec §4.1「背景 fetch と認証」)。
///
/// ★実測 (2026-09-03、GCM 2.x + git 2.48.1、`CreateNoWindow` + stdio リダイレクトの
///   孫プロセスで確認):
///     env 無し                              … GUI ダイアログ「Git Credential Manager」が出る
///     GIT_TERMINAL_PROMPT=0 だけ            … **やはり GUI ダイアログが出る**
///                                             (GCM は GIT_TERMINAL_PROMPT を「端末プロンプト」
///                                              の可否としか読まない)
///     GIT_TERMINAL_PROMPT=0 + GCM_INTERACTIVE=never
///                                           … 出ない。`fatal: Cannot prompt because user
///                                              interactivity has been disabled.` で即終了
///   つまり **GCM_INTERACTIVE=never が無いと、誰も見ていない 5 分ごとの fetch が
///   ダイアログを画面に積み上げる**。ユーザーが押した fetch / pull / push は
///   `run` (= GIT_TERMINAL_PROMPT=0 のみ) を使い、GUI を許す
pub fn run_background(cwd: &Path, args: &[&str]) -> Result<GitOutput, ErrorBody> {
    let mut cmd = build(cwd, args)?;
    cmd.env("GCM_INTERACTIVE", "never");
    match cmd.output() {
        Ok(out) => Ok(GitOutput {
            status: out.status.code().unwrap_or(-1),
            stdout: out.stdout,
            stderr: out.stderr,
        }),
        Err(e) => Err(spawn_error(e)),
    }
}

/// 共通引数 + env を積んだ `Command` を組む (stdin は既定で NUL)。
/// **git を起動する経路が 2 本になっても引数と env が 1 箇所で決まる**ようにするため
fn build(cwd: &Path, args: &[&str]) -> Result<Command, ErrorBody> {
    // ★cwd が存在しないと CreateProcess は ERROR_DIRECTORY で失敗する。std はこれを
    //   NotFound 系に丸めることがあり、そのままだと **git_missing** (= 「git を入れて
    //   ください」) という**まったく見当違いの案内**が UI に出る。先に切り分ける
    if !cwd.is_dir() {
        return Err(ErrorBody::new(
            code::BAD_REQUEST,
            format!("working directory does not exist: {}", cwd.display()),
        ));
    }
    let mut cmd = Command::new("git");
    cmd.arg("-c")
        .arg("core.quotepath=false")
        .arg("-c")
        .arg("color.ui=false")
        .arg("--no-pager");
    for a in args {
        cmd.arg(a);
    }
    cmd.current_dir(cwd)
        .env("LC_ALL", "C")
        .env("GIT_TERMINAL_PROMPT", "0")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    Ok(cmd)
}

/// "git version 2.48.1.windows.1" → ("2.48.1.windows.1", 2, 48)。
/// 先頭 2 つの整数しか見ない (Windows 版のサフィックスや RC 表記に依存しないため)
pub fn parse_version(line: &str) -> Option<(String, u32, u32)> {
    let rest = line.trim().strip_prefix("git version ")?;
    let ver = rest.split_whitespace().next()?.to_string();
    let mut it = ver.split('.');
    let major: u32 = it.next()?.parse().ok()?;
    let minor: u32 = it.next().unwrap_or("0").trim_end_matches(|c: char| !c.is_ascii_digit()).parse().ok()?;
    Some((ver, major, minor))
}

/// `git --version` を取り、下限 (2.11) を満たすか判定する。
/// **cwd はリポジトリでなくてよい** — hello はプロジェクトが git 管理下でなくても通す
pub fn version(cwd: &Path) -> Result<String, ErrorBody> {
    let out = run(cwd, &["--version"])?;
    if !out.success() {
        return Err(ErrorBody::new(code::GIT_MISSING, out.stderr_text()));
    }
    let text = out.stdout_text();
    match parse_version(&text) {
        Some((ver, major, minor)) => {
            if (major, minor) < (MIN_MAJOR, MIN_MINOR) {
                Err(ErrorBody::new(
                    code::GIT_TOO_OLD,
                    format!("git {ver} is too old (need {MIN_MAJOR}.{MIN_MINOR}+ for status --porcelain=v2)"),
                ))
            } else {
                Ok(ver)
            }
        }
        None => Err(ErrorBody::new(code::GIT_MISSING, format!("cannot parse: {text}"))),
    }
}

/// git の失敗を `error.code` に落とす。分類できないものは `git_failed` +
/// stderr 全文 (UI は未知 code をそのまま表示する契約なので握り潰さない)。
///
/// ★照合は LC_ALL=C を前提にした英語の部分一致。ここで拾えなかった文言は
///   「未分類だが detail は見える」で済む — 誤分類の方が害が大きいので広げすぎない
pub fn classify_error(out: &GitOutput) -> ErrorBody {
    let text = out.stderr_text();
    let low = text.to_ascii_lowercase();
    let code = if low.contains("index.lock") {
        code::LOCKED_INDEX
    } else if low.contains("would be overwritten") {
        code::LOCAL_CHANGES_OVERWRITTEN
    } else if low.contains("not a git repository") {
        code::NOT_REPO
    // M66c: commit の直前で初めて出る。identity_check を先に通していても、
    // 「エディタを開いたまま別端末で global config を消した」経路が残るので分類する
    } else if low.contains("please tell me who you are")
        || low.contains("unable to auto-detect email")
        || low.contains("empty ident name")
    {
        code::IDENTITY_MISSING
    } else if low.contains("authentication failed") || low.contains("could not read username") {
        code::AUTH_FAILED
    } else if low.contains("non-fast-forward")
        || low.contains("fetch first")
        // M66f: `git pull --ff-only` が分岐を見つけたときの文言 (実測 git 2.48.1、LC_ALL=C):
        //   hint: Diverging branches can't be fast-forwarded, you need to either:
        //   fatal: Not possible to fast-forward, aborting.
        // ★ここを拾い損ねると git_failed に落ち、UI が「マージして pull」を出せない
        //   = ユーザーはターミナルへ逃げるしかなくなる
        || low.contains("not possible to fast-forward")
        || low.contains("can't be fast-forwarded")
    {
        code::NON_FAST_FORWARD
    // M66h: マージ途中で pull / commit を投げたとき。git 2.48.1 (LC_ALL=C) は
    //   error: Pulling is not possible because you have unmerged files.
    //   error: Committing is not possible because you have unmerged files.
    // ★動詞が違うので「you have unmerged files」だけを見る。ここを拾えないと
    //   git_failed に落ち、UI が「まず競合を解決する」案内を出せない
    } else if low.contains("you have unmerged files") {
        code::MERGE_IN_PROGRESS
    } else if low.contains("could not resolve host") || low.contains("connection timed out") {
        code::NETWORK
    } else {
        code::GIT_FAILED
    };
    ErrorBody::new(code, text)
}
