// `git status --porcelain=v2 -z --branch` の解析。
//
// v1 (`--porcelain` = `XY path`) を使わない理由: リネームの旧パスとステージ済み/未ステージの
// 区別が曖昧で、`-z` でも「1 レコードの区切りがどこか」を XY から推測する羽目になる。
// v2 はレコードの先頭 1 文字 (`1` `2` `u` `?` `!` `#`) が種別なので、パスに空白・引用符・
// 改行が入っていても曖昧さが無い (core.quotepath=false + -z との組み合わせが要点)。
//
// レコード形 (git 2.11 以降、Documentation/git-status.txt):
//   # branch.oid <sha> | (initial)
//   # branch.head <branch> | (detached)
//   # branch.upstream <upstream>
//   # branch.ab +<ahead> -<behind>
//   1 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <path>
//   2 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <X><score> <path>\0<origPath>   ← -z では旧パスが次レコード
//   u <XY> <sub> <m1> <m2> <m3> <mW> <h1> <h2> <h3> <path>
//   ? <path>
//   ! <path>

/// `git log --format=%H%x00%an%x00%aI%x00%s -z` の 1 レコード (M66c)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LogEntry {
    pub sha: String,
    pub author: String,
    /// 厳密 ISO-8601 (`%aI`)。表示の整形は C++ 側の仕事
    pub date: String,
    /// subject = 本文の 1 行目だけ (`%s`)。改行はここに入らない
    pub subject: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StatusEntry {
    /// toplevel 相対・'/' 区切り (git がそのまま出す形)
    pub path: String,
    /// リネーム/コピーの旧パス
    pub old_path: Option<String>,
    /// index 側の状態文字 (`M` `A` `D` `R` `C` `.`)。未追跡は `?`、無視は `!`
    pub index: char,
    /// working tree 側の状態文字
    pub worktree: char,
    /// 未マージ (`u` レコード)
    pub conflict: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct StatusInfo {
    pub oid: String,
    pub branch: String,
    pub upstream: String,
    pub ahead: i64,
    pub behind: i64,
    pub entries: Vec<StatusEntry>,
}

/// n 個のフィールドを空白で切り出し、**残り全部**をパスとして返す。
/// パスに空白が入っていても壊れない (-z なのでパスの終端は NUL = レコード境界)
fn split_fields(rec: &str, count: usize) -> Option<(Vec<&str>, &str)> {
    let mut parts = rec.splitn(count + 1, ' ');
    let mut fields = Vec::with_capacity(count);
    for _ in 0..count {
        fields.push(parts.next()?);
    }
    let path = parts.next()?;
    Some((fields, path))
}

fn xy(field: &str) -> (char, char) {
    let mut it = field.chars();
    let x = it.next().unwrap_or('.');
    let y = it.next().unwrap_or('.');
    (x, y)
}

pub fn parse_status_v2(raw: &[u8]) -> StatusInfo {
    let mut info = StatusInfo::default();
    // -z なので NUL 区切り。末尾の空レコードは捨てる
    let records: Vec<&[u8]> = raw.split(|b| *b == 0).collect();
    let mut i = 0usize;
    while i < records.len() {
        let rec = String::from_utf8_lossy(records[i]).to_string();
        i += 1;
        if rec.is_empty() {
            continue;
        }
        if let Some(header) = rec.strip_prefix("# ") {
            let (key, value) = match header.split_once(' ') {
                Some((k, v)) => (k, v),
                None => (header, ""),
            };
            match key {
                // "(initial)" = 未出生ブランチ。空文字にして「HEAD 無し」を表す
                "branch.oid" => info.oid = if value == "(initial)" { String::new() } else { value.to_string() },
                "branch.head" => info.branch = value.to_string(),
                "branch.upstream" => info.upstream = value.to_string(),
                "branch.ab" => {
                    // "+3 -1"。upstream が無ければヘッダ自体が出ない = 0/0 のまま
                    for tok in value.split_whitespace() {
                        if let Some(n) = tok.strip_prefix('+') {
                            info.ahead = n.parse().unwrap_or(0);
                        } else if let Some(n) = tok.strip_prefix('-') {
                            info.behind = n.parse().unwrap_or(0);
                        }
                    }
                }
                _ => {}
            }
            continue;
        }
        if let Some(body) = rec.strip_prefix("1 ") {
            if let Some((fields, path)) = split_fields(body, 7) {
                let (x, y) = xy(fields[0]);
                info.entries.push(StatusEntry {
                    path: path.to_string(),
                    old_path: None,
                    index: x,
                    worktree: y,
                    conflict: false,
                });
            }
            continue;
        }
        if let Some(body) = rec.strip_prefix("2 ") {
            if let Some((fields, path)) = split_fields(body, 8) {
                let (x, y) = xy(fields[0]);
                // -z では旧パスが**次の NUL レコード**として来る。ここで 1 個消費する
                let old = if i < records.len() {
                    let o = String::from_utf8_lossy(records[i]).to_string();
                    i += 1;
                    Some(o)
                } else {
                    None
                };
                info.entries.push(StatusEntry {
                    path: path.to_string(),
                    old_path: old,
                    index: x,
                    worktree: y,
                    conflict: false,
                });
            }
            continue;
        }
        if let Some(body) = rec.strip_prefix("u ") {
            if let Some((fields, path)) = split_fields(body, 9) {
                let (x, y) = xy(fields[0]);
                info.entries.push(StatusEntry {
                    path: path.to_string(),
                    old_path: None,
                    index: x,
                    worktree: y,
                    conflict: true,
                });
            }
            continue;
        }
        if let Some(path) = rec.strip_prefix("? ") {
            // 未追跡は index/worktree とも '?' (v1 の "??" と同じ見え方に揃える)
            info.entries.push(StatusEntry {
                path: path.to_string(),
                old_path: None,
                index: '?',
                worktree: '?',
                conflict: false,
            });
            continue;
        }
        if let Some(path) = rec.strip_prefix("! ") {
            info.entries.push(StatusEntry {
                path: path.to_string(),
                old_path: None,
                index: '!',
                worktree: '!',
                conflict: false,
            });
            continue;
        }
        // 未知のレコード種別は黙って捨てる (将来 git が種別を足しても落ちない)
    }
    info
}

/// `git log ... -z` の出力を 4 フィールドずつ切り出す (M66c)。
///
/// 実測 (git 2.48.1): 各レコードは NUL で**終端**される = 末尾に空要素が 1 個余る。
/// 端数は捨てる — 「途中で切れた出力から半端なコミットを作る」より、
/// 「その 1 件を出さない」方が読み手を騙さない。
pub fn parse_log_z(raw: &[u8]) -> Vec<LogEntry> {
    let fields: Vec<&[u8]> = raw.split(|b| *b == 0).collect();
    let mut out = Vec::new();
    for chunk in fields.chunks(4) {
        if chunk.len() < 4 {
            break;
        }
        let text = |b: &[u8]| String::from_utf8_lossy(b).to_string();
        // 4 個そろっていても全部空 = 終端 NUL が連続しただけ、なら捨てる
        if chunk.iter().all(|f| f.is_empty()) {
            continue;
        }
        out.push(LogEntry {
            sha: text(chunk[0]),
            author: text(chunk[1]),
            date: text(chunk[2]),
            subject: text(chunk[3]),
        });
    }
    out
}

/// 差分テキストを上限バイトで切る。戻り値の bool = 切ったか。
///
/// ★UTF-8 の文字境界まで戻してから切ること。バイト位置で truncate すると
///   マルチバイトの途中で割れて **panic** する (String::truncate の契約)。
///   日本語のコミットや日本語を含む JSON の差分で普通に踏む
pub fn clip_text(text: &str, max: usize) -> (String, bool) {
    if text.len() <= max {
        return (text.to_string(), false);
    }
    let mut cut = max;
    while cut > 0 && !text.is_char_boundary(cut) {
        cut -= 1;
    }
    (text[..cut].to_string(), true)
}
