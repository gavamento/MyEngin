//! 字句を保つ JSON の読み手 (構造比較用。architecture.md §6.3)。
//!
//! `serde_json::Value` は数値を f64 / i64 へ読むので `0.1` と `0.10000000149011612`、`1.0` と `1` が区別できない。
//! ここでは各値が元のバイト列の範囲 (`start..end`) を持ち、数値は字句のまま比べる。
//! 範囲はそのまま `before_value` / `after_value` に切り出せる (そのまま JSON として読める断片)。
//! 入力は UTF-8 であること (呼び出し側が検証する)。

use std::collections::HashMap;

/// 入れ子の深さの上限。超えたら読み取りを諦める (再帰でスタックを使い切らないため)。
pub const MAX_DEPTH: usize = 200;

#[derive(Debug)]
pub struct Node {
    pub start: usize,
    pub end: usize,
    pub kind: Kind,
}

#[derive(Debug)]
pub enum Kind {
    Null,
    Bool(bool),
    /// 字句は `src[start..end]`。
    Number,
    /// 復号した内容。
    Str(String),
    Array(Vec<Node>),
    /// 出現順のまま。キーの重複は読み取りの失敗にする。
    Object(Vec<(String, Node)>),
}

impl Node {
    pub fn fragment<'a>(&self, src: &'a str) -> &'a str {
        &src[self.start..self.end]
    }

    pub fn get(&self, key: &str) -> Option<&Node> {
        match &self.kind {
            Kind::Object(pairs) => pairs.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_object(&self) -> Option<&[(String, Node)]> {
        match &self.kind {
            Kind::Object(pairs) => Some(pairs),
            _ => None,
        }
    }
}

/// 文書全体を読む。空白だけ・末尾に余計な文字があるものは Err。
pub fn parse(src: &str) -> Result<Node, String> {
    let mut p = Parser { s: src.as_bytes(), i: 0 };
    // UTF-8 の BOM は読み飛ばす (範囲は BOM の後ろから)
    if p.s.starts_with(&[0xEF, 0xBB, 0xBF]) {
        p.i = 3;
    }
    p.skip_ws();
    if p.i >= p.s.len() {
        return Err("空のファイル".to_string());
    }
    let node = p.value(0)?;
    p.skip_ws();
    if p.i != p.s.len() {
        return Err(p.err("末尾に余計な文字がある"));
    }
    Ok(node)
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
}

impl Parser<'_> {
    fn err(&self, what: &str) -> String {
        format!("{what} (バイト位置 {})", self.i)
    }

    fn skip_ws(&mut self) {
        while matches!(self.s.get(self.i), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.i += 1;
        }
    }

    fn value(&mut self, depth: usize) -> Result<Node, String> {
        if depth > MAX_DEPTH {
            return Err(self.err("入れ子が深すぎる"));
        }
        let start = self.i;
        let kind = match self.s.get(self.i) {
            None => return Err(self.err("値が途中で終わっている")),
            Some(b'{') => self.object(depth)?,
            Some(b'[') => self.array(depth)?,
            Some(b'"') => Kind::Str(self.string()?),
            Some(b't') => self.literal("true", Kind::Bool(true))?,
            Some(b'f') => self.literal("false", Kind::Bool(false))?,
            Some(b'n') => self.literal("null", Kind::Null)?,
            Some(b'-' | b'0'..=b'9') => self.number()?,
            Some(_) => return Err(self.err("値として読めない文字")),
        };
        Ok(Node { start, end: self.i, kind })
    }

    fn literal(&mut self, word: &str, kind: Kind) -> Result<Kind, String> {
        if self.s[self.i..].starts_with(word.as_bytes()) {
            self.i += word.len();
            Ok(kind)
        } else {
            Err(self.err("true / false / null の綴りが違う"))
        }
    }

    fn digits(&mut self) -> usize {
        let from = self.i;
        while matches!(self.s.get(self.i), Some(b'0'..=b'9')) {
            self.i += 1;
        }
        self.i - from
    }

    fn number(&mut self) -> Result<Kind, String> {
        if self.s[self.i] == b'-' {
            self.i += 1;
        }
        match self.s.get(self.i) {
            Some(b'0') => self.i += 1,
            Some(b'1'..=b'9') => {
                self.digits();
            }
            _ => return Err(self.err("数値の整数部が無い")),
        }
        if self.s.get(self.i) == Some(&b'.') {
            self.i += 1;
            if self.digits() == 0 {
                return Err(self.err("小数点の後ろに数字が無い"));
            }
        }
        if matches!(self.s.get(self.i), Some(b'e' | b'E')) {
            self.i += 1;
            if matches!(self.s.get(self.i), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if self.digits() == 0 {
                return Err(self.err("指数部に数字が無い"));
            }
        }
        Ok(Kind::Number)
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let digits = self.s.get(self.i..self.i + 4).ok_or_else(|| self.err("\\u の後ろが足りない"))?;
        let text = std::str::from_utf8(digits).map_err(|_| self.err("\\u の後ろが 16 進でない"))?;
        let value = u32::from_str_radix(text, 16).map_err(|_| self.err("\\u の後ろが 16 進でない"))?;
        self.i += 4;
        Ok(value)
    }

    fn string(&mut self) -> Result<String, String> {
        self.i += 1; // 開きの "
        let mut out = String::new();
        loop {
            let from = self.i;
            while let Some(&b) = self.s.get(self.i) {
                if b == b'"' || b == b'\\' || b < 0x20 {
                    break;
                }
                self.i += 1;
            }
            // 入力は UTF-8 で、区切りは ASCII なので境界は文字の切れ目
            out.push_str(std::str::from_utf8(&self.s[from..self.i]).map_err(|_| self.err("UTF-8 でない"))?);
            match self.s.get(self.i) {
                None => return Err(self.err("文字列が閉じていない")),
                Some(b'"') => {
                    self.i += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    self.i += 1;
                    let esc = *self.s.get(self.i).ok_or_else(|| self.err("エスケープが途中で終わっている"))?;
                    self.i += 1;
                    match esc {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let first = self.hex4()?;
                            let code = if (0xD800..0xDC00).contains(&first) {
                                // 上位サロゲートは下位サロゲートと対で 1 文字
                                if self.s.get(self.i..self.i + 2) != Some(b"\\u") {
                                    return Err(self.err("対の無いサロゲート"));
                                }
                                self.i += 2;
                                let second = self.hex4()?;
                                if !(0xDC00..0xE000).contains(&second) {
                                    return Err(self.err("対の無いサロゲート"));
                                }
                                0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00)
                            } else if (0xDC00..0xE000).contains(&first) {
                                return Err(self.err("対の無いサロゲート"));
                            } else {
                                first
                            };
                            out.push(char::from_u32(code).ok_or_else(|| self.err("不正な文字コード"))?);
                        }
                        _ => return Err(self.err("知らないエスケープ")),
                    }
                }
                Some(_) => return Err(self.err("文字列の中の制御文字")),
            }
        }
    }

    fn array(&mut self, depth: usize) -> Result<Kind, String> {
        self.i += 1;
        let mut items = Vec::new();
        self.skip_ws();
        if self.s.get(self.i) == Some(&b']') {
            self.i += 1;
            return Ok(Kind::Array(items));
        }
        loop {
            self.skip_ws();
            items.push(self.value(depth + 1)?);
            self.skip_ws();
            match self.s.get(self.i) {
                Some(b',') => self.i += 1,
                Some(b']') => {
                    self.i += 1;
                    return Ok(Kind::Array(items));
                }
                _ => return Err(self.err("配列の , か ] が無い")),
            }
        }
    }

    fn object(&mut self, depth: usize) -> Result<Kind, String> {
        self.i += 1;
        let mut pairs: Vec<(String, Node)> = Vec::new();
        self.skip_ws();
        if self.s.get(self.i) == Some(&b'}') {
            self.i += 1;
            return Ok(Kind::Object(pairs));
        }
        loop {
            self.skip_ws();
            if self.s.get(self.i) != Some(&b'"') {
                return Err(self.err("オブジェクトのキーが文字列でない"));
            }
            let key = self.string()?;
            self.skip_ws();
            if self.s.get(self.i) != Some(&b':') {
                return Err(self.err("キーの後ろに : が無い"));
            }
            self.i += 1;
            self.skip_ws();
            let value = self.value(depth + 1)?;
            pairs.push((key, value));
            self.skip_ws();
            match self.s.get(self.i) {
                Some(b',') => self.i += 1,
                Some(b'}') => {
                    self.i += 1;
                    break;
                }
                _ => return Err(self.err("オブジェクトの , か } が無い")),
            }
        }
        // 重複キーは、どちらの値を採るかが読み手で割れる。曖昧なものは比べない
        if pairs.len() > 1 {
            let mut seen: HashMap<&str, ()> = HashMap::with_capacity(pairs.len());
            for (k, _) in &pairs {
                if seen.insert(k.as_str(), ()).is_some() {
                    return Err(format!("キーが重複している: {k}"));
                }
            }
        }
        Ok(Kind::Object(pairs))
    }
}

/// 値の正規形が同じか。オブジェクトはキーの集合として (順序を無視)、配列は順序つき、文字列は復号した内容、数値は字句。
pub fn same(a: &Node, sa: &str, b: &Node, sb: &str) -> bool {
    // 全く同じ字句なら正規形も同じ (大半のエンティティはここで終わる)
    if a.fragment(sa) == b.fragment(sb) {
        return true;
    }
    match (&a.kind, &b.kind) {
        (Kind::Null, Kind::Null) => true,
        (Kind::Bool(x), Kind::Bool(y)) => x == y,
        (Kind::Number, Kind::Number) => false, // 字句が違う (上で同じ字句は返してある)
        (Kind::Str(x), Kind::Str(y)) => x == y,
        (Kind::Array(x), Kind::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y).all(|(p, q)| same(p, sa, q, sb))
        }
        (Kind::Object(x), Kind::Object(y)) => {
            if x.len() != y.len() {
                return false;
            }
            let index: HashMap<&str, &Node> = y.iter().map(|(k, v)| (k.as_str(), v)).collect();
            x.iter().all(|(k, v)| index.get(k.as_str()).is_some_and(|w| same(v, sa, w, sb)))
        }
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn eq(a: &str, b: &str) -> bool {
        let (na, nb) = (parse(a).unwrap(), parse(b).unwrap());
        same(&na, a, &nb, b)
    }

    #[test]
    fn numbers_compare_by_lexeme() {
        assert!(eq("0.1", "0.1"));
        assert!(!eq("0.1", "0.10000000149011612"));
        assert!(!eq("1.0", "1"));
        assert!(!eq("1e0", "1"));
        assert!(eq("[1, 2]", "[1,2]"));
    }

    #[test]
    fn objects_ignore_order_and_strings_are_decoded() {
        assert!(eq(r#"{"a":1,"b":[1,2]}"#, "{ \"b\" : [1,2],\n\"a\":1 }"));
        assert!(eq(r#""\u0041""#, r#""A""#));
        assert!(eq(r#""\ud83d\ude00""#, "\"\u{1F600}\""));
        assert!(!eq("[1,2]", "[2,1]"));
        assert!(!eq(r#"{"a":1}"#, r#"{"a":1,"b":2}"#));
    }

    #[test]
    fn fragments_are_exact_slices() {
        let src = "{\"a\": [1.0 ,  2]}";
        let node = parse(src).unwrap();
        assert_eq!(node.get("a").unwrap().fragment(src), "[1.0 ,  2]");
    }

    #[test]
    fn broken_inputs_are_errors() {
        for bad in ["", "   ", "{", "[1,]", "{\"a\":1,}", "01", "1.", "\"\\x\"", "{\"a\":1,\"a\":2}", "[1] x", "\"\\ud800\"", "tru"] {
            assert!(parse(bad).is_err(), "{bad:?} が読めてしまった");
        }
        let deep = "[".repeat(MAX_DEPTH + 10) + &"]".repeat(MAX_DEPTH + 10);
        assert!(parse(&deep).is_err());
    }
}
