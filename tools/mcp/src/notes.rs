//====================================================================================
//                          notes.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          note_read / note_append (追記専用のメモ置き場とパスのガード)
//====================================================================================

use serde_json::{json, Value};
use std::fs::{Metadata, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

pub const TOPIC_MAX_CHARS: usize = 64;
pub const TITLE_MAX_CHARS: usize = 120;
pub const TEXT_MAX_BYTES: usize = 65536;
pub const LATEST_DEFAULT: i64 = 5;
pub const LATEST_MAX: i64 = 50;
// note_read の返却の上限。超えたら古い側を落として truncated: true。
// 生のバイトではなく JSON 文字列に直した後の長さで数える (`\` や `"` の多い本文は 2 倍近くに膨らむため)
pub const READ_MAX_BYTES: usize = 64 * 1024;

const DIR_PARENT: &str = ".mye";
const DIR_NOTES: &str = "notes";
const NOTE_EXT: &str = ".md";
// notes ディレクトリに置く .gitignore。外部プロジェクト側の .gitignore には頼れないので自分で置く
const GITIGNORE_NAME: &str = ".gitignore";
const GITIGNORE_BODY: &str = "*\n";
// "2026-10-15T09:30:00Z" の長さ
const TIMESTAMP_LEN: usize = 20;
// Windows でファイル名の拡張子付きでもデバイスとして開かれる名前 (大小無視)
const RESERVED_BASE: [&str; 4] = ["con", "prn", "aux", "nul"];
const RESERVED_NUMBERED: [&str; 2] = ["com", "lpt"];
// FILE_ATTRIBUTE_REPARSE_POINT。シンボリックリンクにもジャンクションにも付く
#[cfg(windows)]
const ATTRIBUTE_REPARSE_POINT: u32 = 0x400;

// note_read / search が見る 1 件の見出し
pub struct NoteHeading {
    pub topic: String,
    // 見出し行の "## " より後ろ (時刻 + 任意の題)
    pub heading: Option<String>,
}

struct Entry {
    heading: String,
    text: String,
}

// ---- 入力のガード ----

pub fn validate_topic(topic: &str) -> Result<(), String> {
    let chars = topic.chars().count();
    let charset_ok = topic.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-');
    if chars == 0 || chars > TOPIC_MAX_CHARS || !charset_ok {
        return Err(format!(
            "topic must match ^[A-Za-z0-9_-]{{1,{TOPIC_MAX_CHARS}}}$ (got {chars} characters)"
        ));
    }
    if is_reserved_device_name(topic) {
        return Err("topic is a reserved Windows device name".to_string());
    }
    Ok(())
}

fn is_reserved_device_name(topic: &str) -> bool {
    let lower = topic.to_ascii_lowercase();
    if RESERVED_BASE.contains(&lower.as_str()) {
        return true;
    }
    RESERVED_NUMBERED.iter().any(|prefix| {
        lower
            .strip_prefix(prefix)
            .is_some_and(|n| n.len() == 1 && matches!(n.as_bytes()[0], b'1'..=b'9'))
    })
}

fn validate_title(title: &str) -> Result<(), String> {
    let chars = title.chars().count();
    if chars == 0 || chars > TITLE_MAX_CHARS || title.contains(['\n', '\r']) {
        return Err(format!("title must be a single line of 1-{TITLE_MAX_CHARS} characters"));
    }
    Ok(())
}

fn validate_text(text: &str) -> Result<(), String> {
    if text.is_empty() || text.len() > TEXT_MAX_BYTES {
        return Err(format!("text must be 1-{TEXT_MAX_BYTES} bytes (got {})", text.len()));
    }
    Ok(())
}

// シンボリックリンクとジャンクション (どちらも再解析ポイント)
fn is_link(meta: &Metadata) -> bool {
    if meta.file_type().is_symlink() {
        return true;
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        meta.file_attributes() & ATTRIBUTE_REPARSE_POINT != 0
    }
    #[cfg(not(windows))]
    {
        false
    }
}

// 無ければ Ok(None)。リンクなら Err
fn existing_non_link(path: &Path, what: &str) -> Result<Option<Metadata>, String> {
    match std::fs::symlink_metadata(path) {
        Ok(m) if is_link(&m) => Err(format!("{what} is a symbolic link or junction; refusing")),
        Ok(m) => Ok(Some(m)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(format!("cannot inspect {what}: {e}")),
    }
}

fn notes_dir(base: &Path) -> PathBuf {
    base.join(DIR_PARENT).join(DIR_NOTES)
}

// .mye と .mye\notes がリンクでも普通のディレクトリ以外でもないこと。無いのは許す
fn guard_dirs(base: &Path) -> Result<(), String> {
    for (path, what) in [(base.join(DIR_PARENT), ".mye"), (notes_dir(base), ".mye\\notes")] {
        if let Some(m) = existing_non_link(&path, what)? {
            if !m.is_dir() {
                return Err(format!("{what} is not a directory"));
            }
        }
    }
    Ok(())
}

// 正規化した notes ディレクトリが base\.mye\notes の実体であること
fn canonical_notes_dir(base: &Path) -> Result<PathBuf, String> {
    let canon_base = std::fs::canonicalize(base).map_err(|e| format!("cannot resolve the project directory: {e}"))?;
    let canon_notes = std::fs::canonicalize(notes_dir(base)).map_err(|e| format!("cannot resolve the notes directory: {e}"))?;
    let inside = canon_notes.parent().and_then(Path::parent) == Some(canon_base.as_path());
    if !inside {
        return Err("the notes directory resolves outside the project; refusing".to_string());
    }
    Ok(canon_notes)
}

fn check_base(base: &Path) -> Result<(), String> {
    if base.is_dir() {
        Ok(())
    } else {
        Err("project directory not found (--project must name an existing directory)".to_string())
    }
}

// 追記モード (必要なら新規作成) でだけ開く。notes.rs のファイルオープンはここだけ。
// 切り詰め・上書きは append(true) では起きない。create_new は既存ならエラーになる
fn open_append(path: &Path, create_new: bool) -> std::io::Result<std::fs::File> {
    let mut options = OpenOptions::new();
    options.append(true);
    if create_new {
        options.create_new(true);
    } else {
        options.create(true);
    }
    options.open(path)
}

// ---- 時刻 ----

// UNIX 秒 → "YYYY-MM-DDTHH:MM:SSZ" (UTC)。暦計算は Hinnant の civil_from_days
fn utc_timestamp(time: SystemTime) -> String {
    let secs = time.duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) as i64;
    let days = secs.div_euclid(86_400);
    let rem = secs.rem_euclid(86_400);
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        rem / 3_600,
        rem % 3_600 / 60,
        rem % 60
    )
}

// ---- 追記 ----

// 書き込み先を確かめてから作る。戻り値は topic ファイルのパス。
fn prepare_topic_file(base: &Path, topic: &str) -> Result<PathBuf, String> {
    guard_dirs(base)?;
    let dir = notes_dir(base);
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create the notes directory: {e}"))?;
    guard_dirs(base)?;
    let canon_dir = canonical_notes_dir(base)?;

    let file = dir.join(format!("{topic}{NOTE_EXT}"));
    if let Some(m) = existing_non_link(&file, "the topic file")? {
        if !m.is_file() {
            return Err("the topic file is not a regular file".to_string());
        }
        let canon_file = std::fs::canonicalize(&file).map_err(|e| format!("cannot resolve the topic file: {e}"))?;
        if canon_file.parent() != Some(canon_dir.as_path()) {
            return Err("the topic file resolves outside the notes directory; refusing".to_string());
        }
    }

    let gitignore = dir.join(GITIGNORE_NAME);
    if existing_non_link(&gitignore, ".gitignore")?.is_none() {
        match open_append(&gitignore, true) {
            Ok(mut f) => f.write_all(GITIGNORE_BODY.as_bytes()).map_err(|e| format!("cannot write .gitignore: {e}"))?,
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {}
            Err(e) => return Err(format!("cannot create .gitignore: {e}")),
        }
    }
    Ok(file)
}

// note_append。now は呼び出し側から受ける (テストで時刻を固定するため)
pub fn append(base: &Path, topic: &str, title: Option<&str>, text: &str, now: SystemTime) -> Result<Value, String> {
    validate_topic(topic)?;
    if let Some(t) = title {
        validate_title(t)?;
    }
    validate_text(text)?;
    check_base(base)?;
    let file = prepare_topic_file(base, topic)?;

    let stamp = utc_timestamp(now);
    let heading = match title {
        Some(t) => format!("{stamp} {t}"),
        None => stamp,
    };
    let entry = format!("\n## {heading}\n\n{text}\n");
    let mut f = open_append(&file, false).map_err(|e| format!("cannot open the topic file: {e}"))?;
    f.write_all(entry.as_bytes()).map_err(|e| format!("cannot append: {e}"))?;
    let bytes = f.metadata().map(|m| m.len()).map_err(|e| format!("cannot stat the topic file: {e}"))?;
    Ok(json!({"path": format!("{DIR_PARENT}\\{DIR_NOTES}\\{topic}{NOTE_EXT}"), "bytes": bytes}))
}

// ---- 読み出し ----

fn is_timestamp(s: &str) -> bool {
    let b = s.as_bytes();
    b.len() == TIMESTAMP_LEN
        && b.iter().enumerate().all(|(i, c)| match i {
            4 | 7 => *c == b'-',
            10 => *c == b'T',
            13 | 16 => *c == b':',
            19 => *c == b'Z',
            _ => c.is_ascii_digit(),
        })
}

// "## <時刻>[ 題]" の行なら "<時刻>[ 題]"。本文に紛れた "## " だけの行は見出しにしない
fn heading_of(line: &str) -> Option<&str> {
    let rest = line.strip_prefix("## ")?;
    if !rest.is_char_boundary(TIMESTAMP_LEN) || !is_timestamp(&rest[..TIMESTAMP_LEN]) {
        return None;
    }
    let tail = &rest[TIMESTAMP_LEN..];
    (tail.is_empty() || tail.starts_with(' ')).then(|| rest.trim_end())
}

fn parse_entries(content: &str) -> Vec<Entry> {
    let mut entries: Vec<Entry> = Vec::new();
    for line in content.split('\n') {
        if let Some(h) = heading_of(line.strip_suffix('\r').unwrap_or(line)) {
            entries.push(Entry { heading: h.to_string(), text: String::new() });
        } else if let Some(last) = entries.last_mut() {
            last.text.push_str(line);
            last.text.push('\n');
        }
    }
    for e in &mut entries {
        e.text = e.text.trim_matches('\n').to_string();
    }
    entries
}

// JSON 文字列に直したときの 1 文字の長さ (serde_json の既定のエスケープと同じ規則)
fn json_char_len(c: char) -> usize {
    match c {
        '"' | '\\' | '\u{08}' | '\u{0C}' | '\n' | '\r' | '\t' => 2,
        c if (c as u32) < 0x20 => 6,
        c => c.len_utf8(),
    }
}

// JSON 文字列リテラルとしての長さ (前後の引用符を含む)
fn json_len(s: &str) -> usize {
    2 + s.chars().map(json_char_len).sum::<usize>()
}

// JSON 文字列に直した長さが max_json 以下に収まる先頭部分
fn truncate_json(s: &str, max_json: usize) -> &str {
    let mut used = 2;
    for (i, c) in s.char_indices() {
        let len = json_char_len(c);
        if used + len > max_json {
            return &s[..i];
        }
        used += len;
    }
    s
}

// notes ディレクトリ直下の通常の .md から (topic, パス) を topic 名順で返す。無ければ空
fn topic_files(base: &Path) -> Result<Vec<(String, PathBuf)>, String> {
    guard_dirs(base)?;
    let dir = notes_dir(base);
    let Ok(read_dir) = std::fs::read_dir(&dir) else {
        return Ok(Vec::new());
    };
    let mut out = Vec::new();
    for item in read_dir.flatten() {
        let name = item.file_name().to_string_lossy().into_owned();
        let Some(topic) = name.strip_suffix(NOTE_EXT) else { continue };
        if validate_topic(topic).is_err() {
            continue;
        }
        let path = item.path();
        if matches!(existing_non_link(&path, "the topic file"), Ok(Some(m)) if m.is_file()) {
            out.push((topic.to_string(), path));
        }
    }
    out.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(out)
}

fn read_entries(path: &Path) -> Vec<Entry> {
    std::fs::read(path).map(|b| parse_entries(&String::from_utf8_lossy(&b))).unwrap_or_default()
}

// note_read。topic 省略 = 一覧、指定 = 最新 latest 件
pub fn read(base: &Path, topic: Option<&str>, latest: Option<i64>) -> Result<Value, String> {
    check_base(base)?;
    let files = topic_files(base)?;
    let Some(topic) = topic else {
        let topics: Vec<Value> = files
            .iter()
            .map(|(name, path)| {
                let bytes = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
                let last = read_entries(path).pop().map(|e| e.heading);
                json!({"topic": name, "bytes": bytes, "lastHeading": last})
            })
            .collect();
        return Ok(json!({"topics": topics}));
    };
    validate_topic(topic)?;
    let latest = latest.unwrap_or(LATEST_DEFAULT);
    if !(1..=LATEST_MAX).contains(&latest) {
        return Err(format!("latest must be 1-{LATEST_MAX} (got {latest})"));
    }
    let Some((_, path)) = files.iter().find(|(name, _)| name == topic) else {
        return Err(format!("topic not found: {topic}"));
    };
    let entries = read_entries(path);
    let start = entries.len().saturating_sub(latest as usize);
    // 新しい側から詰める。入りきらなければ古い側を落とす (1 件目だけは本文を切って載せる)
    let mut budget = READ_MAX_BYTES;
    let mut picked: Vec<Value> = Vec::new();
    let mut truncated = false;
    for e in entries[start..].iter().rev() {
        let cost = json_len(&e.heading) + json_len(&e.text);
        if cost <= budget {
            budget -= cost;
            picked.push(json!({"heading": e.heading, "text": e.text}));
            continue;
        }
        truncated = true;
        if picked.is_empty() {
            let keep = budget.saturating_sub(json_len(&e.heading));
            picked.push(json!({"heading": e.heading, "text": truncate_json(&e.text, keep)}));
        }
        break;
    }
    picked.reverse();
    Ok(json!({"topic": topic, "total": entries.len(), "truncated": truncated, "entries": picked}))
}

// search 用: 各 topic 名 (heading = None) と各エントリの見出し。読めなければ空
pub fn headings(base: &Path) -> Vec<NoteHeading> {
    let mut out = Vec::new();
    for (topic, path) in topic_files(base).unwrap_or_default() {
        out.push(NoteHeading { topic: topic.clone(), heading: None });
        for e in read_entries(&path) {
            out.push(NoteHeading { topic: topic.clone(), heading: Some(e.heading) });
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};
    use std::time::Duration;

    // target\notes_test\ 配下の一意なプロジェクトディレクトリ。掃除は mcp_verify.ps1 が先頭でやる
    // (規則 15-d で、このファイルから削除 API を使えないため)
    fn project() -> PathBuf {
        static COUNTER: AtomicU32 = AtomicU32::new(0);
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let dir = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("notes_test")
            .join(format!("{}-{nanos}-{}", std::process::id(), COUNTER.fetch_add(1, Ordering::Relaxed)));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn at(secs: u64) -> SystemTime {
        UNIX_EPOCH + Duration::from_secs(secs)
    }

    fn count_entries(dir: &Path) -> usize {
        std::fs::read_dir(dir).unwrap().count()
    }

    #[test]
    fn 時刻は_utc_の_iso_8601() {
        assert_eq!(utc_timestamp(at(0)), "1970-01-01T00:00:00Z");
        assert_eq!(utc_timestamp(at(1_000_000_000)), "2001-09-09T01:46:40Z");
        assert_eq!(utc_timestamp(at(1_709_164_800)), "2024-02-29T00:00:00Z");
        assert_eq!(utc_timestamp(at(1_709_251_199)), "2024-02-29T23:59:59Z");
        assert_eq!(utc_timestamp(at(4_102_444_800)), "2100-01-01T00:00:00Z");
    }

    #[test]
    fn topic_のガード() {
        for ok in ["a", "m87-state", "A_b-9", &"x".repeat(64), "console", "com10", "com0", "lpt", "nul1"] {
            assert!(validate_topic(ok).is_ok(), "{ok}");
        }
        let long = "x".repeat(65);
        for bad in [
            "", "../x", "..\\x", "C:\\x", "/x", "a/b", "a.md", "a b", "日本語", "ｔｏｐｉｃ", "con", "NUL", "Com1", "LPT9",
            "prn", "AUX", "a\n", long.as_str(),
        ] {
            assert!(validate_topic(bad).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn 追記は積まれ_既存内容を保つ() {
        let base = project();
        std::fs::create_dir_all(base.join(".mye").join("notes")).unwrap();
        let file = base.join(".mye").join("notes").join("t.md");
        open_append(&file, false).unwrap().write_all(b"OLD CONTENT\n").unwrap();
        append(&base, "t", Some("first"), "one", at(0)).unwrap();
        let r = append(&base, "t", None, "two", at(60)).unwrap();
        let body = std::fs::read_to_string(&file).unwrap();
        assert_eq!(body, "OLD CONTENT\n\n## 1970-01-01T00:00:00Z first\n\none\n\n## 1970-01-01T00:01:00Z\n\ntwo\n");
        assert_eq!(r["bytes"], body.len());
        assert_eq!(r["path"], ".mye\\notes\\t.md");
    }

    #[test]
    fn ガードに当たると何も作らない() {
        let base = project();
        for bad in ["../x", "con", "", "a/b"] {
            assert!(append(&base, bad, None, "x", at(0)).is_err());
        }
        assert!(append(&base, "ok", Some("a\nb"), "x", at(0)).is_err());
        assert!(append(&base, "ok", Some(""), "x", at(0)).is_err());
        assert!(append(&base, "ok", None, "", at(0)).is_err());
        assert!(append(&base, "ok", None, &"y".repeat(TEXT_MAX_BYTES + 1), at(0)).is_err());
        assert_eq!(count_entries(&base), 0);
        assert!(append(&base.join("missing"), "ok", None, "x", at(0)).is_err());
        assert!(!base.join("missing").exists());
    }

    #[test]
    fn 初回に_gitignore_を置く() {
        let base = project();
        append(&base, "t", None, "x", at(0)).unwrap();
        let ignore = base.join(".mye").join("notes").join(".gitignore");
        assert_eq!(std::fs::read_to_string(&ignore).unwrap(), "*\n");
        append(&base, "t", None, "y", at(1)).unwrap();
        assert_eq!(std::fs::read_to_string(&ignore).unwrap(), "*\n");
    }

    #[test]
    fn 読み出しは最新の_n_件と一覧() {
        let base = project();
        for i in 0..4u64 {
            append(&base, "beta", Some(&format!("e{i}")), &format!("body {i}"), at(i)).unwrap();
        }
        append(&base, "alpha", None, "a", at(10)).unwrap();
        let r = read(&base, Some("beta"), Some(2)).unwrap();
        assert_eq!(r["total"], 4);
        assert_eq!(r["truncated"], false);
        let texts: Vec<&str> = r["entries"].as_array().unwrap().iter().map(|e| e["text"].as_str().unwrap()).collect();
        assert_eq!(texts, ["body 2", "body 3"]);
        let list = read(&base, None, None).unwrap();
        let names: Vec<&str> = list["topics"].as_array().unwrap().iter().map(|t| t["topic"].as_str().unwrap()).collect();
        assert_eq!(names, ["alpha", "beta"]);
        assert_eq!(list["topics"][1]["lastHeading"], "1970-01-01T00:00:03Z e3");
        assert!(read(&base, Some("nothing"), None).is_err());
        assert!(read(&base, Some("beta"), Some(0)).is_err());
        assert!(read(&base, Some("beta"), Some(LATEST_MAX + 1)).is_err());
    }

    #[test]
    fn 読み出しは_64kb_で古い側を落とす() {
        let base = project();
        let big = "あ".repeat(10_000);
        for i in 0..4u64 {
            append(&base, "big", None, &big, at(i)).unwrap();
        }
        let r = read(&base, Some("big"), Some(50)).unwrap();
        assert_eq!(r["truncated"], true);
        let n = r["entries"].as_array().unwrap().len();
        assert!(n >= 1 && n < 4, "{n}");
        assert_eq!(r["entries"][n - 1]["heading"], "1970-01-01T00:00:03Z");
        assert!(entries_json_len(&r) <= READ_MAX_BYTES);
    }

    fn entries_json_len(r: &Value) -> usize {
        r["entries"]
            .as_array()
            .unwrap()
            .iter()
            .map(|e| json_len(e["heading"].as_str().unwrap()) + json_len(e["text"].as_str().unwrap()))
            .sum()
    }

    #[test]
    fn 上限はエスケープ後の長さで数える() {
        let base = project();
        // `"` は JSON で 2 バイトになる。生のバイトで数えると 64 KB に収まって見えてしまう
        let quotes = "\"".repeat(TEXT_MAX_BYTES);
        append(&base, "q", None, &quotes, at(0)).unwrap();
        let r = read(&base, Some("q"), None).unwrap();
        assert_eq!(r["truncated"], true);
        assert!(entries_json_len(&r) <= READ_MAX_BYTES);
        let out = serde_json::to_string(&r["entries"][0]["text"]).unwrap();
        assert_eq!(out.len(), json_len(r["entries"][0]["text"].as_str().unwrap()));
    }

    #[test]
    fn json_len_は_serde_json_と一致する() {
        for s in ["", "abc", "あ\"\\", "\n\r\t\u{08}\u{0C}", "\u{01}\u{1F}\u{7F}", "😀/<>&'"] {
            assert_eq!(json_len(s), serde_json::to_string(s).unwrap().len(), "{s:?}");
        }
    }

    #[test]
    fn 最新の_1_件が_64kb_を超えるなら本文を切る() {
        let base = project();
        let big = "え".repeat(TEXT_MAX_BYTES / 3);
        append(&base, "huge", Some("t"), &big, at(0)).unwrap();
        let r = read(&base, Some("huge"), Some(1)).unwrap();
        assert_eq!(r["truncated"], true);
        let e = &r["entries"][0];
        assert!(entries_json_len(&r) <= READ_MAX_BYTES);
        assert!(!e["text"].as_str().unwrap().is_empty());
        append(&base, "huge", Some("t"), &big, at(1)).unwrap();
        let r = read(&base, Some("huge"), Some(2)).unwrap();
        assert_eq!(r["truncated"], true);
        assert_eq!(r["entries"].as_array().unwrap().len(), 1);
        assert_eq!(r["entries"][0]["heading"], "1970-01-01T00:00:01Z t");
    }

    #[test]
    fn 本文中の見出し風の行は区切りにしない() {
        let base = project();
        append(&base, "t", None, "## fake heading\n## 2000-01-01 not a stamp", at(0)).unwrap();
        let r = read(&base, Some("t"), None).unwrap();
        assert_eq!(r["total"], 1);
        assert!(r["entries"][0]["text"].as_str().unwrap().contains("## fake heading"));
    }

    #[test]
    fn 一覧と_search_は他のファイルを拾わない() {
        let base = project();
        append(&base, "t", Some("hello world"), "x", at(0)).unwrap();
        let dir = base.join(".mye").join("notes");
        open_append(&dir.join("bad topic.md"), false).unwrap();
        open_append(&dir.join("readme.txt"), false).unwrap();
        let list = read(&base, None, None).unwrap();
        assert_eq!(list["topics"].as_array().unwrap().len(), 1);
        let h = headings(&base);
        assert_eq!(h.len(), 2);
        assert!(h[0].heading.is_none());
        assert_eq!(h[1].heading.as_deref(), Some("1970-01-01T00:00:00Z hello world"));
    }

    #[test]
    fn notes_が無いプロジェクトは空() {
        let base = project();
        assert_eq!(read(&base, None, None).unwrap()["topics"].as_array().unwrap().len(), 0);
        assert!(headings(&base).is_empty());
        assert_eq!(count_entries(&base), 0);
    }
}
