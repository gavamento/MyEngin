//! フックが渡した編集 (`declaredEdit`) を Before に当てて、After の内容を計算する純粋な関数。
//!
//! 規則は attribution-rules.md §6.1 と試作 5 (prototype-results.md)。当てられないときは None を返し、
//! 呼び出し側は根拠を作らない (Verified にならないだけで、誤りではない)。

use serde_json::{json, Value};

/// 宣言された編集。対象は `targets` の 1 つ目だけ。
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DeclaredEdit {
    Replace { old: String, new: String, replace_all: bool },
    Write { content: String },
}

impl DeclaredEdit {
    /// `{"kind":"replace","oldString":..,"newString":..,"replaceAll":bool}` か `{"kind":"write","content":..}`。
    pub fn from_json(value: &Value) -> Result<DeclaredEdit, String> {
        let text = |key: &str| {
            value[key].as_str().map(str::to_string).ok_or_else(|| format!("declaredEdit.{key} は文字列でなければならない"))
        };
        match value["kind"].as_str() {
            Some("replace") => Ok(DeclaredEdit::Replace {
                old: text("oldString")?,
                new: text("newString")?,
                replace_all: value["replaceAll"].as_bool().unwrap_or(false),
            }),
            Some("write") => Ok(DeclaredEdit::Write { content: text("content")? }),
            _ => Err("declaredEdit.kind は replace / write のどちらか".to_string()),
        }
    }

    pub fn to_json(&self) -> Value {
        match self {
            DeclaredEdit::Replace { old, new, replace_all } => {
                json!({ "kind": "replace", "oldString": old, "newString": new, "replaceAll": replace_all })
            }
            DeclaredEdit::Write { content } => json!({ "kind": "write", "content": content }),
        }
    }

    /// Before (ファイルが無ければ None) に当てた結果。当てられなければ None。行末は変換せず、バイト列として扱う。
    pub fn apply(&self, before: Option<&[u8]>) -> Option<Vec<u8>> {
        match self {
            DeclaredEdit::Write { content } => Some(content.as_bytes().to_vec()),
            DeclaredEdit::Replace { old, new, replace_all } => {
                let text = std::str::from_utf8(before?).ok()?;
                if old.is_empty() {
                    return None;
                }
                let count = text.matches(old.as_str()).count();
                if count == 0 || (count > 1 && !replace_all) {
                    return None;
                }
                Some(text.replace(old.as_str(), new).into_bytes())
            }
        }
    }
}
