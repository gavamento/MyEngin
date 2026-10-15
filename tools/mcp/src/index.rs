//====================================================================================
//                          index.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          エンジン索引 (engine_index.json) の読み込みと stale 判定
//====================================================================================

use serde_json::Value;
use std::path::Path;
use std::time::UNIX_EPOCH;

// 読める索引の schema。gen_engine_index.ps1 が書く値と揃える
pub const SUPPORTED_SCHEMA: i64 = 1;

// 索引を読む。Err は isError にそのまま載せる文面 (環境依存の絶対パスは含めない)。
pub fn load(path: &Path, display: &str) -> Result<Value, String> {
    let bytes = std::fs::read(path).map_err(|_| {
        format!(
            "engine index not found or unreadable: {display}. Run: pwsh -NoProfile -File tools\\gen_engine_index.ps1"
        )
    })?;
    let body = bytes.strip_prefix(&[0xEF, 0xBB, 0xBF][..]).unwrap_or(&bytes);
    let doc: Value = serde_json::from_slice(body).map_err(|e| {
        format!("engine index is not valid JSON: {display} ({e}). Run: pwsh -NoProfile -File tools\\gen_engine_index.ps1")
    })?;
    match doc.get("schema").and_then(Value::as_i64) {
        Some(SUPPORTED_SCHEMA) => Ok(doc),
        other => Err(format!(
            "engine index schema {} is not supported (this server reads schema {SUPPORTED_SCHEMA}). Run: pwsh -NoProfile -File tools\\gen_engine_index.ps1",
            other.map_or("(missing)".to_string(), |n| n.to_string())
        )),
    }
}

// 索引の sources のどれかが記録時と違えば true。MCP は再生成しない (報告だけ)。
// root が分からない / ファイルが無い / 時刻が読めない場合も「確かめられない」ので true。
pub fn is_stale(doc: &Value, root: Option<&Path>) -> bool {
    let sources = match doc.get("sources").and_then(Value::as_array) {
        Some(s) => s,
        None => return true,
    };
    if sources.is_empty() {
        return false;
    }
    let root = match root {
        Some(r) => r,
        None => return true,
    };
    for source in sources {
        let (rel, recorded) = match (
            source.get("path").and_then(Value::as_str),
            source.get("mtime").and_then(Value::as_str).and_then(parse_iso_ticks),
        ) {
            (Some(p), Some(t)) => (p, t),
            _ => return true,
        };
        let actual = std::fs::metadata(root.join(rel))
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map(|d| (d.as_nanos() / 100) as i128);
        if actual != Some(recorded) {
            return true;
        }
    }
    false
}

// "2026-10-14T08:51:02.2304473Z" → UNIX epoch からの 100ns 単位。.NET の "O" 書式 (小数 7 桁)。
// 小数が無い / 7 桁未満でも読む。
pub fn parse_iso_ticks(s: &str) -> Option<i128> {
    let s = s.strip_suffix('Z')?;
    let (date, time) = s.split_once('T')?;
    let mut d = date.split('-');
    let (y, m, day): (i64, i64, i64) =
        (d.next()?.parse().ok()?, d.next()?.parse().ok()?, d.next()?.parse().ok()?);
    let (hms, frac) = match time.split_once('.') {
        Some((a, b)) => (a, b),
        None => (time, ""),
    };
    let mut t = hms.split(':');
    let (hh, mm, ss): (i64, i64, i64) =
        (t.next()?.parse().ok()?, t.next()?.parse().ok()?, t.next()?.parse().ok()?);
    if !(1..=12).contains(&m) || !(1..=31).contains(&day) || frac.len() > 9 {
        return None;
    }
    if !frac.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    // 小数を 7 桁へ揃える (足りなければ 0 埋め、多ければ切り捨て)
    let mut frac7 = frac.to_string();
    while frac7.len() < 7 {
        frac7.push('0');
    }
    let sub: i64 = frac7[..7].parse().ok()?;
    let secs = days_from_civil(y, m, day) * 86400 + hh * 3600 + mm * 60 + ss;
    Some(secs as i128 * 10_000_000 + sub as i128)
}

// 1970-01-01 からの日数 (Howard Hinnant の civil 変換)
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let mp = (m + 9) % 12;
    let doy = (153 * mp + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn 時刻の解析は_100ns_単位() {
        assert_eq!(parse_iso_ticks("1970-01-01T00:00:00.0000000Z"), Some(0));
        assert_eq!(parse_iso_ticks("1970-01-01T00:00:01.5Z"), Some(15_000_000));
        // 2000-01-01T00:00:00Z = 946684800 s
        assert_eq!(parse_iso_ticks("2000-01-01T00:00:00Z"), Some(946_684_800 * 10_000_000));
        assert_eq!(
            parse_iso_ticks("2026-10-14T08:51:02.2304473Z"),
            Some(1_791_967_862 * 10_000_000 + 2_304_473)
        );
        assert_eq!(parse_iso_ticks("not a time"), None);
        assert_eq!(parse_iso_ticks("2026-13-01T00:00:00Z"), None);
    }

    #[test]
    fn sources_が空なら_fresh() {
        assert!(!is_stale(&json!({"sources": []}), None));
        assert!(is_stale(&json!({}), None));
    }

    #[test]
    fn root_不明や欠けたファイルは_stale() {
        let doc = json!({"sources": [{"path": "Cargo.toml", "mtime": "2000-01-01T00:00:00Z"}]});
        assert!(is_stale(&doc, None));
        let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
        assert!(is_stale(&doc, Some(manifest_dir))); // 時刻が違う
        let missing = json!({"sources": [{"path": "no/such/file", "mtime": "2000-01-01T00:00:00Z"}]});
        assert!(is_stale(&missing, Some(manifest_dir)));
    }

    #[test]
    fn 実ファイルの時刻と一致すれば_fresh() {
        let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
        let actual = std::fs::metadata(manifest_dir.join("Cargo.toml"))
            .unwrap()
            .modified()
            .unwrap()
            .duration_since(UNIX_EPOCH)
            .unwrap();
        let ticks = actual.as_nanos() / 100;
        let secs = (ticks / 10_000_000) as i64;
        let sub = (ticks % 10_000_000) as i64;
        let (days, rem) = (secs / 86400, secs % 86400);
        let (y, m, d) = civil_from_days(days);
        let iso = format!(
            "{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}.{sub:07}Z",
            rem / 3600,
            rem % 3600 / 60,
            rem % 60
        );
        let doc = json!({"sources": [{"path": "Cargo.toml", "mtime": iso}]});
        assert!(!is_stale(&doc, Some(manifest_dir)), "{iso}");
    }

    fn civil_from_days(z: i64) -> (i64, i64, i64) {
        let z = z + 719468;
        let era = if z >= 0 { z } else { z - 146096 } / 146097;
        let doe = z - era * 146097;
        let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        let y = yoe + era * 400;
        let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        let mp = (5 * doy + 2) / 153;
        let d = doy - (153 * mp + 2) / 5 + 1;
        let m = if mp < 10 { mp + 3 } else { mp - 9 };
        (if m <= 2 { y + 1 } else { y }, m, d)
    }
}
