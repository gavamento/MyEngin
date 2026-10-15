//====================================================================================
//                          search.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          エンジン索引の検索 (スコア・同義語展開) と describe
//====================================================================================

use serde_json::{json, Map, Value};
use std::collections::{HashMap, HashSet};

// kind の固定順。同点の並びと enum の並びがこれ
pub const KINDS: [&str; 8] = ["component", "field", "cli", "abi", "adr", "spec", "golden", "note"];

pub const SCORE_EXACT: u32 = 100;
pub const SCORE_PREFIX: u32 = 80;
pub const SCORE_CONTAINS: u32 = 60;
pub const SCORE_TRANSLATION: u32 = 40;
pub const SCORE_SYNONYM: u32 = 30;

// describe の言及 (spec 見出し / ADR) は 1 種別あたりこの件数まで。総数は mentionsTotal で返す
const MENTION_LIMIT: usize = 20;
// 同義語に使う英単語の最短長と、l10n 全体での出現数の上限 (よく出る語は同義語として弱い)
const SYNONYM_MIN_WORD_LEN: usize = 4;
const SYNONYM_MAX_DOC_FREQ: usize = 30;
// 同義語の源にするのは短いラベルだけ (長い説明文の単語は無関係な名前に当たって雑音になる)
const SYNONYM_MAX_LABEL_CHARS: usize = 40;
const STOP_WORDS: [&str; 24] = [
    "that", "this", "with", "from", "have", "then", "when", "will", "been", "only", "into", "your",
    "none", "does", "should", "must", "there", "which", "while", "after", "before", "also", "than",
    "them",
];

// l10n から機械生成できなかった語だけを手で足す訳語表 (ja<TAB>en,en...)
const EXTRA_SYNONYMS: &str = include_str!("../synonyms_extra.tsv");

struct Entity {
    kind: usize,
    id: String,
    // 名前としての一致に使う文字列 (id、フィールドなら素の名前、ADR / spec なら題名)
    names: Vec<String>,
    // jp / tooltip など訳語・説明の文字列
    secondary: Vec<String>,
    // hit にそのまま載せる付帯 (jp を含む)
    extra: Map<String, Value>,
}

fn s(v: &Value, key: &str) -> String {
    v.get(key).and_then(Value::as_str).unwrap_or("").to_string()
}

fn arr<'a>(doc: &'a Value, key: &str) -> &'a [Value] {
    doc.get(key).and_then(Value::as_array).map(Vec::as_slice).unwrap_or(&[])
}

fn build_entities(doc: &Value, wanted: &[bool; 8]) -> Vec<Entity> {
    let mut out = Vec::new();
    let mut push = |kind: usize, id: String, names: Vec<String>, secondary: Vec<String>, extra: Map<String, Value>| {
        if wanted[kind] {
            out.push(Entity { kind, id, names, secondary, extra });
        }
    };
    for c in arr(doc, "components") {
        let name = s(c, "name");
        let mut secondary = Vec::new();
        for f in c.get("fields").and_then(Value::as_array).into_iter().flatten() {
            for key in ["jp", "tooltip"] {
                let t = s(f, key);
                if !t.is_empty() {
                    secondary.push(t);
                }
            }
        }
        let mut extra = Map::new();
        extra.insert("typeId".into(), c["typeId"].clone());
        extra.insert("size".into(), c["size"].clone());
        push(0, name.clone(), vec![name], secondary, extra);
        for f in c.get("fields").and_then(Value::as_array).into_iter().flatten() {
            let comp = s(c, "name");
            let fname = s(f, "name");
            let id = format!("{comp}.{fname}");
            let mut secondary = Vec::new();
            let mut extra = Map::new();
            extra.insert("component".into(), json!(comp));
            extra.insert("type".into(), f["type"].clone());
            extra.insert("offset".into(), f["offset"].clone());
            for key in ["jp", "tooltip"] {
                let t = s(f, key);
                if !t.is_empty() {
                    secondary.push(t.clone());
                    extra.insert(key.into(), json!(t));
                }
            }
            push(1, id.clone(), vec![id, fname], secondary, extra);
        }
    }
    let cli_sets: [(&str, &str, &str); 3] =
        [("cliEngine", "name", "engine"), ("cliShowcase", "flag", "showcase"), ("cliEditor", "flag", "editor")];
    for (section, key, layer) in cli_sets {
        for c in arr(doc, section) {
            let flag = s(c, key);
            let mut extra = Map::new();
            extra.insert("layer".into(), json!(layer));
            if let Some(v) = c.get("value") {
                extra.insert("value".into(), v.clone());
            }
            if let Some(v) = c.get("editorOnly") {
                extra.insert("editorOnly".into(), v.clone());
            }
            push(2, flag.clone(), vec![flag], vec![], extra);
        }
    }
    let version = doc["abi"]["version"].clone();
    for slot in doc["abi"]["slots"].as_array().into_iter().flatten() {
        let name = s(slot, "name");
        let mut extra = Map::new();
        extra.insert("index".into(), slot["index"].clone());
        extra.insert("args".into(), slot["args"].clone());
        extra.insert("abiVersion".into(), version.clone());
        push(3, name.clone(), vec![name], vec![], extra);
    }
    for a in arr(doc, "adr") {
        let id = s(a, "id");
        let title = s(a, "title");
        let mut extra = Map::new();
        extra.insert("title".into(), json!(title));
        extra.insert("file".into(), a["file"].clone());
        let secondary = a["headings"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|h| h.as_str().map(str::to_string))
            .collect();
        push(4, id.clone(), vec![id, title], secondary, extra);
    }
    for h in arr(doc, "spec") {
        let title = s(h, "title");
        let mut extra = Map::new();
        extra.insert("line".into(), h["line"].clone());
        extra.insert("level".into(), h["level"].clone());
        push(5, title.clone(), vec![title], vec![], extra);
    }
    for g in arr(doc, "golden") {
        let name = s(g, "name");
        let mut extra = Map::new();
        extra.insert("n".into(), g["n"].clone());
        extra.insert("args".into(), g["args"].clone());
        extra.insert("png".into(), g["png"].clone());
        push(6, name.clone(), vec![name], vec![], extra);
    }
    out
}

// ---- クエリの準備 ----

fn is_ja(c: char) -> bool {
    matches!(c,
        '\u{3400}'..='\u{4DBF}' | '\u{4E00}'..='\u{9FFF}' | '\u{30A0}'..='\u{30FF}'
        | '\u{31F0}'..='\u{31FF}' | '\u{FF66}'..='\u{FF9F}')
}

fn is_latin(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}

// 漢字・カタカナの連なり (JA) と英数字の連なり (Latin) を取り出す。ひらがなと記号は区切り。
fn runs(text: &str, class: fn(char) -> bool) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    for c in text.chars() {
        if class(c) {
            cur.push(c);
        } else if !cur.is_empty() {
            out.push(std::mem::take(&mut cur));
        }
    }
    if !cur.is_empty() {
        out.push(cur);
    }
    out
}

// 2 文字以上の連なりだけをトークンにする (「音の遮蔽」の「音」のような 1 文字は雑音)。
// 1 つも残らなければクエリ全体を 1 トークンとして扱う。
fn tokenize(raw_lower: &str) -> Vec<String> {
    let mut tokens: Vec<String> = Vec::new();
    let mut cur = String::new();
    let mut cur_ja = false;
    for c in raw_lower.chars() {
        let ja = is_ja(c);
        if ja || is_latin(c) {
            if !cur.is_empty() && cur_ja != ja {
                tokens.push(std::mem::take(&mut cur));
            }
            cur_ja = ja;
            cur.push(c);
        } else if !cur.is_empty() {
            tokens.push(std::mem::take(&mut cur));
        }
    }
    if !cur.is_empty() {
        tokens.push(cur);
    }
    tokens.retain(|t| t.chars().count() >= 2);
    if tokens.is_empty() {
        tokens.push(raw_lower.to_string());
    }
    tokens
}

// 記号と空白を落として小文字にする ("time travel" / "--timetravel-selftest" を同じ形へ)
fn squash(text: &str) -> String {
    text.chars().filter(|c| c.is_alphanumeric()).flat_map(char::to_lowercase).collect()
}

fn clean(text: &str) -> &str {
    text.split("###").next().unwrap_or(text)
}

// 名前を語に割る ("UIScrollRect.horizontalScrollbar" → ui / scroll / rect / horizontal / scrollbar)。
// 記号・空白・大文字小文字・数字の切れ目が境界。同義語は部分文字列ではなく語の先頭で引く
// ("time" が "Runtime" に当たらないように)。
fn name_words(name: &str) -> Vec<String> {
    let chars: Vec<char> = name.chars().collect();
    let mut words: Vec<String> = Vec::new();
    let mut cur = String::new();
    for (i, &c) in chars.iter().enumerate() {
        if !c.is_alphanumeric() {
            if !cur.is_empty() {
                words.push(std::mem::take(&mut cur));
            }
            continue;
        }
        let prev = if i > 0 { Some(chars[i - 1]) } else { None };
        let next = chars.get(i + 1).copied();
        let boundary = match prev {
            Some(p) if p.is_alphanumeric() => {
                (p.is_lowercase() && c.is_uppercase())
                    || (p.is_uppercase() && c.is_uppercase() && next.map_or(false, char::is_lowercase))
                    || (p.is_ascii_digit() != c.is_ascii_digit())
            }
            _ => false,
        };
        if boundary && !cur.is_empty() {
            words.push(std::mem::take(&mut cur));
        }
        cur.extend(c.to_lowercase());
    }
    if !cur.is_empty() {
        words.push(cur);
    }
    words
}

fn has_word_prefix(words: &[String], needle: &str) -> bool {
    words.iter().any(|w| w.starts_with(needle))
}

fn english_words(text: &str) -> Vec<String> {
    let mut words: Vec<String> = text
        .split(|c: char| !c.is_ascii_alphabetic())
        .map(str::to_ascii_lowercase)
        .filter(|w| w.len() >= SYNONYM_MIN_WORD_LEN && !STOP_WORDS.contains(&w.as_str()))
        .collect();
    words.sort();
    words.dedup();
    words
}

struct Query {
    raw: String,
    squashed: String,
    tokens: Vec<String>,
    // l10n で訳語が一致した英文を squash した集合 (名前と完全に一致すれば 40)
    translated_names: HashSet<String>,
    // 同義語展開 (30)。英語側は名前への部分一致、日本語側は jp / tooltip への部分一致
    expand_en: HashSet<String>,
    expand_ja: HashSet<String>,
    // 手書きの訳語表で一致した語 (40)。英語側は名前への部分文字列一致 ("navmesh" が NavMeshAgent に当たる)、日本語側は jp / tooltip への部分一致
    manual_en: HashSet<String>,
    manual_ja: HashSet<String>,
}

// 同義語の表 1 行 (ja 側の語, en 側の語)
fn extra_synonyms() -> Vec<(String, Vec<String>)> {
    EXTRA_SYNONYMS
        .lines()
        .filter(|l| !l.trim().is_empty() && !l.starts_with('#'))
        .filter_map(|l| {
            let (ja, en) = l.split_once('\t')?;
            Some((
                ja.trim().to_lowercase(),
                en.split(',').map(|w| w.trim().to_lowercase()).filter(|w| !w.is_empty()).collect(),
            ))
        })
        .collect()
}

fn prepare(doc: &Value, q: &str) -> Query {
    let raw = q.trim().to_lowercase();
    let tokens = tokenize(&raw);
    let mut query = Query {
        squashed: squash(&raw),
        raw,
        tokens,
        translated_names: HashSet::new(),
        expand_en: HashSet::new(),
        expand_ja: HashSet::new(),
        manual_en: HashSet::new(),
        manual_ja: HashSet::new(),
    };

    let l10n = arr(doc, "l10n");
    // 英単語の出現数 (よく出る語を同義語から外す)。必要になったときだけ数える
    let mut doc_freq: Option<HashMap<String, usize>> = None;
    let single_latin = query.tokens.len() == 1 && !query.tokens[0].chars().any(is_ja);
    for token in query.tokens.clone() {
        let token_is_ja = token.chars().any(is_ja);
        for entry in l10n {
            let en = clean(entry["en"].as_str().unwrap_or(""));
            let ja = clean(entry["ja"].as_str().unwrap_or(""));
            if token_is_ja {
                if !ja.to_lowercase().contains(&token) {
                    continue;
                }
                query.translated_names.insert(squash(en));
                let freq = doc_freq.get_or_insert_with(|| {
                    let mut m: HashMap<String, usize> = HashMap::new();
                    for e in l10n {
                        for w in english_words(clean(e["en"].as_str().unwrap_or(""))) {
                            *m.entry(w).or_default() += 1;
                        }
                    }
                    m
                });
                if en.chars().count() > SYNONYM_MAX_LABEL_CHARS {
                    continue;
                }
                for w in english_words(en) {
                    if freq.get(&w).copied().unwrap_or(0) <= SYNONYM_MAX_DOC_FREQ {
                        query.expand_en.insert(w);
                    }
                }
            } else if single_latin && en.to_lowercase().contains(&token) {
                for run in runs(ja, is_ja) {
                    if run.chars().count() >= 2 {
                        query.expand_ja.insert(run.to_lowercase());
                    }
                }
            }
        }
    }
    // 手書きの訳語表。ja 語 → en 語、en 語 → ja 語の両方向
    for (ja, ens) in extra_synonyms() {
        if query.tokens.iter().any(|t| t.chars().any(is_ja) && t.contains(&ja)) {
            query.manual_en.extend(ens.iter().cloned());
        }
        if query.tokens.iter().any(|t| ens.contains(t)) {
            query.manual_ja.insert(ja.clone());
        }
    }
    query
}

fn tier(hay: &str, needle: &str) -> u32 {
    if needle.is_empty() {
        0
    } else if hay == needle {
        SCORE_EXACT
    } else if hay.starts_with(needle) {
        SCORE_PREFIX
    } else if hay.contains(needle) {
        SCORE_CONTAINS
    } else {
        0
    }
}

fn score(e: &Entity, q: &Query) -> u32 {
    let names: Vec<String> = e.names.iter().map(|n| n.to_lowercase()).collect();
    let words: Vec<String> = e.names.iter().flat_map(|n| name_words(n)).collect();
    let mut best = 0;
    for n in &names {
        best = best.max(tier(n, &q.raw));
        best = best.max(tier(&squash(n), &q.squashed));
    }
    if best >= SCORE_CONTAINS {
        return best;
    }
    let joined = e.secondary.join("\n").to_lowercase();
    if !joined.is_empty()
        && (joined.contains(&q.raw) || q.tokens.iter().all(|t| joined.contains(t.as_str())))
    {
        best = best.max(SCORE_TRANSLATION);
    }
    if e.names.iter().any(|n| q.translated_names.contains(&squash(n)))
        || q.manual_en.iter().any(|w| names.iter().any(|n| n.contains(w.as_str())))
        || q.manual_ja.iter().any(|w| joined.contains(w.as_str()))
    {
        best = best.max(SCORE_TRANSLATION);
    }
    if best >= SCORE_TRANSLATION {
        return best;
    }
    if q.expand_en.iter().any(|w| has_word_prefix(&words, w))
        || q.expand_ja.iter().any(|w| joined.contains(w.as_str()))
    {
        best = best.max(SCORE_SYNONYM);
    }
    best
}

pub struct SearchOutcome {
    pub total: usize,
    pub hits: Vec<Value>,
}

// kinds: 絞り込み (空 = 全部)。名前が KINDS に無ければ呼び出し側が先に弾く。
pub fn search(doc: &Value, q: &str, kinds: &[String], limit: usize) -> SearchOutcome {
    let mut wanted = [kinds.is_empty(); 8];
    for k in kinds {
        if let Some(i) = KINDS.iter().position(|n| n == k) {
            wanted[i] = true;
        }
    }
    let query = prepare(doc, q);
    let mut scored: Vec<(u32, &Entity)> = Vec::new();
    let entities = build_entities(doc, &wanted);
    for e in &entities {
        let sc = score(e, &query);
        if sc > 0 {
            scored.push((sc, e));
        }
    }
    scored.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.kind.cmp(&b.1.kind)).then(a.1.id.cmp(&b.1.id)));
    let total = scored.len();
    let hits = scored
        .into_iter()
        .take(limit)
        .map(|(sc, e)| {
            let mut m = e.extra.clone();
            m.insert("kind".into(), json!(KINDS[e.kind]));
            m.insert("id".into(), json!(e.id));
            m.insert("score".into(), json!(sc));
            m.insert("src".into(), json!("engine"));
            Value::Object(m)
        })
        .collect();
    SearchOutcome { total, hits }
}

// ---- describe ----

fn spec_title(doc: &Value, line: i64) -> String {
    arr(doc, "spec")
        .iter()
        .find(|h| h["line"].as_i64() == Some(line))
        .map(|h| s(h, "title"))
        .unwrap_or_default()
}

fn adr_title(doc: &Value, id: &str) -> String {
    arr(doc, "adr").iter().find(|a| s(a, "id") == id).map(|a| s(a, "title")).unwrap_or_default()
}

// mentions[section][name] = {spec:[行番号], adr:[ID]} を (言及一覧, 総数) に直す。
fn mentions(doc: &Value, section: &str, name: &str) -> (Value, Value) {
    let m = &doc["mentions"][section][name];
    let lines: Vec<i64> = m["spec"].as_array().into_iter().flatten().filter_map(Value::as_i64).collect();
    let mut sorted = lines.clone();
    sorted.sort();
    let adrs: Vec<String> =
        m["adr"].as_array().into_iter().flatten().filter_map(|v| v.as_str().map(str::to_string)).collect();
    let spec_items: Vec<Value> = sorted
        .iter()
        .take(MENTION_LIMIT)
        .map(|&l| json!({"line": l, "title": spec_title(doc, l)}))
        .collect();
    let adr_items: Vec<Value> = adrs
        .iter()
        .take(MENTION_LIMIT)
        .map(|id| json!({"id": id, "title": adr_title(doc, id)}))
        .collect();
    (
        json!({"spec": spec_items, "adr": adr_items}),
        json!({"spec": lines.len(), "adr": adrs.len()}),
    )
}

fn cli_layer_of(doc: &Value, flag: &str) -> Option<&'static str> {
    if arr(doc, "cliEngine").iter().any(|c| s(c, "name") == flag) {
        Some("engine")
    } else if arr(doc, "cliShowcase").iter().any(|c| s(c, "flag") == flag) {
        Some("showcase")
    } else if arr(doc, "cliEditor").iter().any(|c| s(c, "flag") == flag) {
        Some("editor")
    } else {
        None
    }
}

fn describe_cli(doc: &Value, flag: &str, layer: &str, detail: Value) -> Value {
    let (m, total) = mentions(doc, "cli", flag);
    let goldens: Vec<Value> = arr(doc, "golden")
        .iter()
        .filter(|g| g["args"].as_array().map_or(false, |a| a.iter().any(|x| x.as_str() == Some(flag))))
        .map(|g| json!({"n": g["n"], "name": g["name"], "args": g["args"]}))
        .collect();
    let mut out = json!({"kind": "cli", "id": flag, "layer": layer, "mentions": m, "mentionsTotal": total, "goldens": goldens});
    if let (Some(o), Some(d)) = (out.as_object_mut(), detail.as_object()) {
        for (k, v) in d {
            o.insert(k.clone(), v.clone());
        }
    }
    out
}

// id の完全一致を全 kind から集める (同名が複数 kind にあれば全部)。
fn describe_exact(doc: &Value, id: &str) -> Vec<Value> {
    let mut out = Vec::new();
    for c in arr(doc, "components") {
        let name = s(c, "name");
        if name == id {
            let (m, total) = mentions(doc, "components", &name);
            out.push(json!({
                "kind": "component", "id": name, "typeId": c["typeId"], "size": c["size"],
                "align": c["align"], "flags": c["flags"], "fields": c["fields"],
                "mentions": m, "mentionsTotal": total,
            }));
        }
        for f in c.get("fields").and_then(Value::as_array).into_iter().flatten() {
            if format!("{name}.{}", s(f, "name")) == id {
                let mut o = f.as_object().cloned().unwrap_or_default();
                o.insert("kind".into(), json!("field"));
                o.insert("id".into(), json!(id));
                o.insert("component".into(), json!(name));
                out.push(Value::Object(o));
            }
        }
    }
    for c in arr(doc, "cliEngine") {
        if s(c, "name") == id {
            out.push(describe_cli(doc, id, "engine", json!({"value": c["value"]})));
        }
    }
    for c in arr(doc, "cliShowcase") {
        if s(c, "flag") == id {
            out.push(describe_cli(doc, id, "showcase", json!({"editorOnly": c["editorOnly"]})));
        }
    }
    for c in arr(doc, "cliEditor") {
        if s(c, "flag") == id {
            out.push(describe_cli(doc, id, "editor", json!({})));
        }
    }
    for slot in doc["abi"]["slots"].as_array().into_iter().flatten() {
        if s(slot, "name") == id {
            out.push(json!({"kind": "abi", "id": id, "index": slot["index"], "args": slot["args"], "abiVersion": doc["abi"]["version"]}));
        }
    }
    for a in arr(doc, "adr") {
        if s(a, "id") == id {
            out.push(json!({"kind": "adr", "id": id, "title": a["title"], "file": a["file"], "headings": a["headings"]}));
        }
    }
    for h in arr(doc, "spec") {
        if s(h, "title") == id {
            out.push(json!({"kind": "spec", "id": id, "line": h["line"], "level": h["level"]}));
        }
    }
    for g in arr(doc, "golden") {
        if s(g, "name") == id {
            let flags: Vec<Value> = g["args"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(Value::as_str)
                .filter(|a| a.starts_with("--"))
                .map(|a| json!({"flag": a, "layer": cli_layer_of(doc, a)}))
                .collect();
            out.push(json!({"kind": "golden", "id": id, "n": g["n"], "args": g["args"], "png": g["png"], "cliFlags": flags}));
        }
    }
    out
}

pub enum Described {
    Found(Vec<Value>),
    // 見つからない。上位 5 件の候補
    NotFound(Vec<Value>),
}

pub fn describe(doc: &Value, id: &str) -> Described {
    let found = describe_exact(doc, id);
    if !found.is_empty() {
        return Described::Found(found);
    }
    let candidates = search(doc, id, &[], 5)
        .hits
        .into_iter()
        .map(|h| json!({"kind": h["kind"], "id": h["id"]}))
        .collect();
    Described::NotFound(candidates)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn doc() -> Value {
        json!({
            "schema": 1,
            "components": [
                {"name": "Rigidbody", "typeId": 15, "size": 8, "align": 4, "flags": [],
                 "fields": [{"name": "mass", "type": "Float", "offset": 0, "flags": [], "jp": "質量", "tooltip": "mass in kg"}]},
                {"name": "RigidbodyHelper", "typeId": 16, "size": 4, "align": 4, "flags": [], "fields": []},
                {"name": "Terrain", "typeId": 33, "size": 4, "align": 4, "flags": [], "fields": []},
                {"name": "AcousticVolume", "typeId": 40, "size": 4, "align": 4, "flags": [],
                 "fields": [{"name": "blockLayerMask", "type": "UInt", "offset": 0, "flags": [], "jp": "遮蔽するレイヤー"}]},
            ],
            "cliEngine": [{"name": "--timetravel-selftest", "value": "none"}],
            "cliShowcase": [], "cliEditor": [],
            "abi": {"version": 30, "slots": [{"index": 0, "name": "Log", "args": 3}]},
            "adr": [], "spec": [{"level": 2, "title": "Terrain", "line": 10}], "golden": [],
            "l10n": [
                {"id": "Asset_Terrain", "en": "Terrain", "ja": "地形"},
                {"id": "TT", "en": "Time travel records only", "ja": "タイムトラベルは再生中だけ記録します"},
            ],
            "mentions": {"components": {}, "cli": {}},
        })
    }

    fn ids(o: &SearchOutcome) -> Vec<String> {
        o.hits.iter().map(|h| h["id"].as_str().unwrap().to_string()).collect()
    }

    #[test]
    fn 完全一致_前方一致_部分一致の順() {
        let d = doc();
        let r = search(&d, "rigidbody", &["component".to_string()], 10);
        assert_eq!(ids(&r), ["Rigidbody", "RigidbodyHelper"]);
        assert_eq!(r.hits[0]["score"], SCORE_EXACT);
        assert_eq!(r.hits[1]["score"], SCORE_PREFIX);
        let r = search(&d, "body", &["component".to_string()], 10);
        assert_eq!(r.hits[0]["score"], SCORE_CONTAINS);
    }

    #[test]
    fn 同点は_kind_の固定順_次に_id() {
        let d = doc();
        // Terrain は component (100) と spec (100) の両方に完全一致する
        let r = search(&d, "Terrain", &[], 10);
        assert_eq!(r.hits[0]["kind"], "component");
        assert_eq!(r.hits[1]["kind"], "spec");
    }

    #[test]
    fn jp_と_l10n_と同義語() {
        let d = doc();
        // フィールドの jp (40)
        let r = search(&d, "質量", &[], 10);
        assert_eq!(r.hits[0]["id"], "Rigidbody");
        assert_eq!(r.hits[0]["score"], SCORE_TRANSLATION);
        // l10n の訳語 (ja 地形 -> en Terrain が名前と完全一致 = 40)
        let r = search(&d, "地形", &["component".to_string()], 10);
        assert_eq!(ids(&r), ["Terrain"]);
        assert_eq!(r.hits[0]["score"], SCORE_TRANSLATION);
        // 同義語展開 (30): ラグドール -> l10n の "Ragdoll (from Skeleton)" の単語 ragdoll
        let mut d2 = doc();
        d2["l10n"].as_array_mut().unwrap().push(json!({"id": "Create_Ragdoll", "en": "Ragdoll (from Skeleton)", "ja": "ラグドール (スケルトンから)"}));
        d2["components"].as_array_mut().unwrap().push(json!({"name": "RagdollBody", "typeId": 50, "size": 4, "align": 4, "flags": [], "fields": []}));
        let r = search(&d2, "ラグドール", &["component".to_string()], 10);
        assert_eq!(ids(&r), ["RagdollBody"]);
        assert_eq!(r.hits[0]["score"], SCORE_SYNONYM);
        // 手書きの訳語表 (40): タイムトラベル -> timetravel が --timetravel-selftest に含まれる
        let r = search(&d, "タイムトラベル", &[], 10);
        assert_eq!(ids(&r), ["--timetravel-selftest"]);
        assert_eq!(r.hits[0]["score"], SCORE_TRANSLATION);
    }

    #[test]
    fn 手書きの訳語表は全行が_日本語_タブ_英語() {
        let rows = extra_synonyms();
        let data_lines = EXTRA_SYNONYMS.lines().filter(|l| !l.trim().is_empty() && !l.starts_with('#')).count();
        assert_eq!(rows.len(), data_lines);
        assert!(rows.iter().all(|(ja, en)| !ja.is_empty() && !en.is_empty()));
    }

    #[test]
    fn 名前の語分割() {
        assert_eq!(name_words("UIScrollRect.horizontalScrollbar"), ["ui", "scroll", "rect", "horizontal", "scrollbar"]);
        assert_eq!(name_words("--timetravel-selftest"), ["timetravel", "selftest"]);
        assert_eq!(name_words("bbEntityKey0"), ["bb", "entity", "key", "0"]);
        assert!(!has_word_prefix(&name_words("Runtime and Build"), "time"));
    }

    #[test]
    fn 助詞つきの問いは長い連なりだけで引く() {
        let d = doc();
        let r = search(&d, "音の遮蔽", &[], 10);
        assert_eq!(ids(&r), ["AcousticVolume", "AcousticVolume.blockLayerMask"]);
    }

    #[test]
    fn describe_は完全一致か候補() {
        let d = doc();
        match describe(&d, "Rigidbody") {
            Described::Found(v) => assert_eq!(v[0]["fields"].as_array().unwrap().len(), 1),
            Described::NotFound(_) => panic!("見つかるはず"),
        }
        match describe(&d, "Rigidbod") {
            Described::NotFound(c) => assert_eq!(c[0]["id"], "Rigidbody"),
            Described::Found(_) => panic!("完全一致ではない"),
        }
    }
}
