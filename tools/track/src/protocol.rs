//! pipe のプロトコルの正本 (docs/ai-change-tracking/ipc-contract.md)。
//!
//! 1 行 1 JSON (LF)。要求 `{"id","op","args"}`、応答 `{"id","ok","result"|"error"}`。
//! 知らない op は `unknown_op`、知らない引数は `bad_args` で拒否し、黙って無視しない。

use serde_json::{json, Map, Value};

/// プロトコル版。意味を変える・削るときに上げる (op や引数を足すだけなら上げない)。
pub const PROTO_VERSION: u32 = 1;

/// 1 行の上限 (改行を除く)。
pub const MAX_LINE_BYTES: usize = 1024 * 1024;

/// `list` の 1 ページの既定と最大。
pub const LIST_DEFAULT_LIMIT: i64 = 50;
pub const LIST_MAX_LIMIT: i64 = 200;

/// 接続の役割。役割ごとに呼べる op を分ける (事故を防ぐ区分けで、認証ではない)。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Mcp,
    Editor,
    Hook,
}

impl Role {
    pub fn parse(text: &str) -> Option<Role> {
        match text {
            "mcp" => Some(Role::Mcp),
            "editor" => Some(Role::Editor),
            "hook" => Some(Role::Hook),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Role::Mcp => "mcp",
            Role::Editor => "editor",
            Role::Hook => "hook",
        }
    }
}

/// このサブで実装している op。ここに無い op は `unknown_op` (未実装も含む)。
pub const IMPLEMENTED_OPS: &[&str] = &[
    "hello", "status", "list", "describe", "sessions", "hunks", "blob_get", "shutdown", "subscribe", "session_begin", "session_end",
    "op_begin", "op_end", "report", "editor_saved",
];

/// `subscribe` で選べる通知の名前 (ipc-contract.md §4.5)。`resync` は購読に関わらず必ず届くので、ここには入れない。
pub const EVENT_NAMES: &[&str] =
    &["change_recorded", "attribution_updated", "coverage_gap", "session_changed", "shutting_down"];

/// 役割が op を呼べるか。`hello` は役割を決める前なので表に含めない。
pub fn role_allows(role: Role, op: &str) -> bool {
    match op {
        "status" => true,
        "list" | "describe" | "sessions" => matches!(role, Role::Mcp | Role::Editor),
        // Hunk の本文と Blob の中身は Editor の表示のためのもの (MCP・フックへ本文を渡さない)
        "hunks" | "blob_get" => role == Role::Editor,
        // サービスの終了は Editor だけ (MCP・フックが他の利用者のサービスを止めない)
        "shutdown" => role == Role::Editor,
        // 通知は Editor の窓のためのもの。MCP・フックは要求ごとに繋ぐので購読しない
        "subscribe" => role == Role::Editor,
        // 記録を伴う op は AI 側 (MCP・フック) だけ。Editor は自分の保存を editor_saved で知らせる (復元は sub-15)
        "session_begin" | "session_end" | "op_begin" | "op_end" => matches!(role, Role::Mcp | Role::Hook),
        // 申告は MCP の change_report だけ (フックは編集の前後を op_begin / op_end で知らせる)
        "report" => role == Role::Mcp,
        // Editor の保存の知らせは Editor だけ (AI 側が「Editor が書いた」と名乗って帰属を書き換えられないように)
        "editor_saved" => role == Role::Editor,
        _ => false,
    }
}

/// エラーコード (ipc-contract.md §5)。
pub mod code {
    pub const NEED_HELLO: &str = "need_hello";
    pub const PROTO_MISMATCH: &str = "proto_mismatch";
    pub const BAD_TOKEN: &str = "bad_token";
    pub const WRONG_PROJECT: &str = "wrong_project";
    pub const FORBIDDEN: &str = "forbidden";
    pub const UNKNOWN_OP: &str = "unknown_op";
    pub const BAD_ARGS: &str = "bad_args";
    pub const NOT_FOUND: &str = "not_found";
    pub const BLOB_MISSING: &str = "blob_missing";
    pub const TOO_LARGE: &str = "too_large";
    pub const BUSY: &str = "busy";
    pub const DB_DEGRADED: &str = "db_degraded";
    pub const INTERNAL: &str = "internal";
}

/// 失敗の応答になるエラー。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OpError {
    pub code: &'static str,
    pub detail: String,
}

impl OpError {
    pub fn new(code: &'static str, detail: impl Into<String>) -> OpError {
        OpError { code, detail: detail.into() }
    }

    pub fn bad_args(detail: impl Into<String>) -> OpError {
        OpError::new(code::BAD_ARGS, detail)
    }
}

/// 応答の detail に入れる名前の長さを抑える (巨大な名前で応答が 1 行の上限を超えないように)。
pub fn clip(text: &str) -> String {
    text.chars().take(64).collect()
}

/// 読み取った要求。
#[derive(Debug)]
pub struct Request {
    pub id: u64,
    pub op: String,
    pub args: Map<String, Value>,
}

/// 1 行を要求として読む。失敗のときは、読めた範囲の `id` (無ければ None) と理由を返す。
pub fn parse_request(line: &[u8]) -> Result<Request, (Option<u64>, OpError)> {
    // serde の文言は版で変わるので、位置だけを添える
    let value: Value = serde_json::from_slice(line)
        .map_err(|e| (None, OpError::bad_args(format!("JSON として読めない (行内の位置 {})", e.column()))))?;
    let Value::Object(mut object) = value else {
        return Err((None, OpError::bad_args("要求は JSON のオブジェクトでなければならない")));
    };
    let id = object.remove("id").and_then(|v| v.as_u64().filter(|n| *n > 0));
    let Some(id) = id else {
        return Err((None, OpError::bad_args("id は正の整数でなければならない")));
    };
    let fail = |detail: String| (Some(id), OpError::bad_args(detail));
    let op = match object.remove("op") {
        Some(Value::String(op)) => op,
        _ => return Err(fail("op は文字列でなければならない".to_string())),
    };
    let args = match object.remove("args") {
        None | Some(Value::Null) => Map::new(),
        Some(Value::Object(args)) => args,
        Some(_) => return Err(fail("args はオブジェクトでなければならない".to_string())),
    };
    if let Some(extra) = object.keys().next() {
        return Err(fail(format!("知らない項目: {}", clip(extra))));
    }
    Ok(Request { id, op, args })
}

pub fn ok_response(id: u64, result: Value) -> Value {
    json!({ "id": id, "ok": true, "result": result })
}

pub fn error_response(id: Option<u64>, error: &OpError) -> Value {
    json!({ "id": id, "ok": false, "error": { "code": error.code, "detail": error.detail } })
}

/// 引数を 1 つずつ取り出し、最後に残りがあれば `bad_args` にする。
pub struct Args {
    map: Map<String, Value>,
}

impl Args {
    pub fn new(map: Map<String, Value>) -> Args {
        Args { map }
    }

    fn take(&mut self, key: &str) -> Option<Value> {
        self.map.remove(key).filter(|v| !v.is_null())
    }

    pub fn string(&mut self, key: &str) -> Result<Option<String>, OpError> {
        match self.take(key) {
            None => Ok(None),
            Some(Value::String(s)) => Ok(Some(s)),
            Some(_) => Err(OpError::bad_args(format!("{key} は文字列でなければならない"))),
        }
    }

    pub fn required_string(&mut self, key: &str) -> Result<String, OpError> {
        self.string(key)?.ok_or_else(|| OpError::bad_args(format!("{key} が要る")))
    }

    pub fn int(&mut self, key: &str) -> Result<Option<i64>, OpError> {
        match self.take(key) {
            None => Ok(None),
            Some(v) => v.as_i64().map(Some).ok_or_else(|| OpError::bad_args(format!("{key} は整数でなければならない"))),
        }
    }

    pub fn required_int(&mut self, key: &str) -> Result<i64, OpError> {
        self.int(key)?.ok_or_else(|| OpError::bad_args(format!("{key} が要る")))
    }

    pub fn bool(&mut self, key: &str) -> Result<Option<bool>, OpError> {
        match self.take(key) {
            None => Ok(None),
            Some(Value::Bool(b)) => Ok(Some(b)),
            Some(_) => Err(OpError::bad_args(format!("{key} は真偽値でなければならない"))),
        }
    }

    /// 取り出した生の値 (配列・オブジェクトの引数用)。無い・null は None。
    pub fn value(&mut self, key: &str) -> Option<Value> {
        self.take(key)
    }

    /// 文字列の配列。
    pub fn string_list(&mut self, key: &str) -> Result<Option<Vec<String>>, OpError> {
        match self.take(key) {
            None => Ok(None),
            Some(Value::Array(items)) => items
                .into_iter()
                .map(|v| match v {
                    Value::String(s) => Ok(s),
                    _ => Err(OpError::bad_args(format!("{key} の要素は文字列でなければならない"))),
                })
                .collect::<Result<Vec<_>, _>>()
                .map(Some),
            Some(_) => Err(OpError::bad_args(format!("{key} は配列でなければならない"))),
        }
    }

    /// 文字列で、`allowed` のどれかでなければならない。
    pub fn choice(&mut self, key: &str, allowed: &[&str]) -> Result<Option<String>, OpError> {
        match self.string(key)? {
            Some(s) if !allowed.contains(&s.as_str()) => {
                Err(OpError::bad_args(format!("{key} は {} のどれか", allowed.join(" / "))))
            }
            other => Ok(other),
        }
    }

    /// 取り出していない引数が残っていれば `bad_args`。
    pub fn finish(self) -> Result<(), OpError> {
        match self.map.keys().next() {
            Some(key) => Err(OpError::bad_args(format!("知らない引数: {}", clip(key)))),
            None => Ok(()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_a_well_formed_request() {
        let r = parse_request(br#"{"id":3,"op":"list","args":{"limit":5}}"#).unwrap();
        assert_eq!((r.id, r.op.as_str()), (3, "list"));
        assert_eq!(r.args["limit"], 5);
    }

    #[test]
    fn rejects_bad_shapes_with_the_id_when_readable() {
        let (id, e) = parse_request(br#"{"id":4,"op":"list","extra":1}"#).unwrap_err();
        assert_eq!((id, e.code), (Some(4), code::BAD_ARGS));
        let (id, _) = parse_request(br#"{"id":0,"op":"list"}"#).unwrap_err();
        assert_eq!(id, None);
        let (id, _) = parse_request(b"not json").unwrap_err();
        assert_eq!(id, None);
        let (id, _) = parse_request(br#"{"id":5,"op":7}"#).unwrap_err();
        assert_eq!(id, Some(5));
    }

    #[test]
    fn args_reject_leftovers_and_wrong_types() {
        let mut args = Args::new(serde_json::from_str(r#"{"limit":"x","path":"a"}"#).unwrap());
        assert!(args.int("limit").is_err());
        assert_eq!(args.string("path").unwrap().as_deref(), Some("a"));
        assert!(args.finish().is_ok());
        let args = Args::new(serde_json::from_str(r#"{"zzz":1}"#).unwrap());
        assert_eq!(args.finish().unwrap_err().code, code::BAD_ARGS);
    }

    #[test]
    fn roles_gate_the_ops() {
        assert!(role_allows(Role::Hook, "status"));
        assert!(!role_allows(Role::Hook, "list"));
        assert!(role_allows(Role::Mcp, "describe"));
        assert!(!role_allows(Role::Mcp, "shutdown"));
        assert!(role_allows(Role::Editor, "shutdown"));
        assert!(role_allows(Role::Editor, "subscribe"));
        assert!(!role_allows(Role::Mcp, "subscribe"));
        assert!(!role_allows(Role::Hook, "subscribe"));
        assert!(role_allows(Role::Editor, "hunks") && role_allows(Role::Editor, "blob_get"));
        for op in ["hunks", "blob_get"] {
            assert!(!role_allows(Role::Mcp, op) && !role_allows(Role::Hook, op));
        }
        assert!(role_allows(Role::Editor, "editor_saved"));
        assert!(!role_allows(Role::Mcp, "editor_saved") && !role_allows(Role::Hook, "editor_saved"));
    }
}
