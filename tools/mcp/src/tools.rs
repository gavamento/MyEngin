//====================================================================================
//                          tools.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          MCP の道具表 (tools/list の定義と tools/call の実行)
//====================================================================================

use crate::handler::Context;
use crate::index;
use crate::rpc::{RpcError, INVALID_PARAMS};
use crate::search::{self, Described, KINDS};
use serde_json::{json, Value};

const QUERY_MAX_CHARS: usize = 200;
const LIMIT_DEFAULT: i64 = 20;
const LIMIT_MAX: i64 = 100;

// 道具の実行エラー (isError: true で返す)。JSON-RPC の誤りではない。
pub struct ToolError {
    pub message: String,
    pub data: Option<Value>,
}

impl ToolError {
    fn new(message: impl Into<String>) -> Self {
        ToolError { message: message.into(), data: None }
    }
}

// tools/list の中身。固定順。sub-03 / sub-04 で verify_job / note_* を後ろへ足す。
pub fn list() -> Value {
    json!([
        {
            "name": "search",
            "description": "Search the MyEngine index (components, fields, CLI flags, ABI slots, ADRs, engine_spec.md headings, golden screenshots) by English or Japanese words. Returns ranked hits with their source; use describe on a hit id for details. `stale: true` means the index is older than its sources (run tools\\gen_engine_index.ps1).",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "q": {"type": "string", "minLength": 1, "maxLength": QUERY_MAX_CHARS, "description": "Search words (Japanese or English), 1-200 chars."},
                    "kind": {
                        "type": "array",
                        "items": {"type": "string", "enum": KINDS},
                        "uniqueItems": true,
                        "description": "Restrict to these kinds. Omit for all."
                    },
                    "limit": {"type": "integer", "minimum": 1, "maximum": LIMIT_MAX, "default": LIMIT_DEFAULT, "description": "Maximum hits."}
                },
                "required": ["q"],
                "additionalProperties": false
            }
        },
        {
            "name": "describe",
            "description": "Look up one id exactly (component name, Component.field, CLI flag, ABI slot name, ADR-NNN, spec heading, golden name). Returns all matches across kinds: component fields, mentioning spec headings / ADRs (up to 20 each, with mentionsTotal), related goldens. Unknown ids return the top 5 candidates.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "id": {"type": "string", "minLength": 1, "description": "Exact id as returned by search."}
                },
                "required": ["id"],
                "additionalProperties": false
            }
        }
    ])
}

// tools/call。params の形が悪ければ -32602、道具の中の失敗は isError の結果にする。
pub fn call(ctx: &Context, params: &Value) -> Result<Value, RpcError> {
    let obj = params
        .as_object()
        .ok_or_else(|| RpcError::new(INVALID_PARAMS, "params must be an object"))?;
    let name = obj
        .get("name")
        .and_then(Value::as_str)
        .ok_or_else(|| RpcError::new(INVALID_PARAMS, "params.name must be a string"))?;
    let empty = json!({});
    let args = match obj.get("arguments") {
        None | Some(Value::Null) => &empty,
        Some(v) if v.is_object() => v,
        Some(_) => return Err(RpcError::new(INVALID_PARAMS, "params.arguments must be an object")),
    };
    let outcome = match name {
        "search" => search_tool(ctx, args),
        "describe" => describe_tool(ctx, args),
        _ => return Err(RpcError::new(INVALID_PARAMS, format!("Unknown tool: {name}"))),
    };
    Ok(match outcome {
        Ok(v) => wrap(v, false),
        Err(e) => {
            let mut body = json!({"error": e.message});
            if let Some(d) = e.data {
                body["details"] = d;
            }
            wrap(body, true)
        }
    })
}

// 結果は text (JSON 文字列) と structuredContent の両方に同じ JSON を入れる
fn wrap(body: Value, is_error: bool) -> Value {
    let mut result = json!({
        "content": [{"type": "text", "text": body.to_string()}],
        "structuredContent": body,
    });
    if is_error {
        result["isError"] = json!(true);
    }
    result
}

// 許可された引数名以外が来たら弾く (inputSchema の additionalProperties: false と揃える)
fn reject_unknown(args: &Value, allowed: &[&str]) -> Result<(), ToolError> {
    for key in args.as_object().into_iter().flat_map(|o| o.keys()) {
        if !allowed.contains(&key.as_str()) {
            return Err(ToolError::new(format!("unknown argument: {key}")));
        }
    }
    Ok(())
}

fn load_index(ctx: &Context) -> Result<(Value, bool), ToolError> {
    let (path, display) = ctx.index_location().map_err(ToolError::new)?;
    let doc = index::load(&path, &display).map_err(ToolError::new)?;
    let stale = index::is_stale(&doc, ctx.root.as_deref());
    Ok((doc, stale))
}

fn search_tool(ctx: &Context, args: &Value) -> Result<Value, ToolError> {
    reject_unknown(args, &["q", "kind", "limit"])?;
    let q = args
        .get("q")
        .and_then(Value::as_str)
        .ok_or_else(|| ToolError::new("q is required and must be a string"))?;
    let q_chars = q.chars().count();
    if q.trim().is_empty() || q_chars > QUERY_MAX_CHARS {
        return Err(ToolError::new(format!("q must be 1-{QUERY_MAX_CHARS} characters (got {q_chars})")));
    }
    let kinds: Vec<String> = match args.get("kind") {
        None | Some(Value::Null) => Vec::new(),
        Some(Value::String(k)) => vec![k.clone()],
        Some(Value::Array(items)) => {
            let mut v = Vec::new();
            for item in items {
                v.push(
                    item.as_str().ok_or_else(|| ToolError::new("kind must be an array of strings"))?.to_string(),
                );
            }
            v
        }
        Some(_) => return Err(ToolError::new("kind must be an array of strings")),
    };
    for k in &kinds {
        if !KINDS.contains(&k.as_str()) {
            return Err(ToolError::new(format!("unknown kind: {k} (one of: {})", KINDS.join(", "))));
        }
    }
    let limit = match args.get("limit") {
        None | Some(Value::Null) => LIMIT_DEFAULT,
        Some(v) => v.as_i64().ok_or_else(|| ToolError::new("limit must be an integer"))?,
    };
    if !(1..=LIMIT_MAX).contains(&limit) {
        return Err(ToolError::new(format!("limit must be 1-{LIMIT_MAX} (got {limit})")));
    }
    let (doc, stale) = load_index(ctx)?;
    let outcome = search::search(&doc, q, &kinds, limit as usize);
    Ok(json!({"stale": stale, "total": outcome.total, "hits": outcome.hits}))
}

fn describe_tool(ctx: &Context, args: &Value) -> Result<Value, ToolError> {
    reject_unknown(args, &["id"])?;
    let id = args
        .get("id")
        .and_then(Value::as_str)
        .ok_or_else(|| ToolError::new("id is required and must be a string"))?;
    if id.is_empty() {
        return Err(ToolError::new("id must not be empty"));
    }
    let (doc, stale) = load_index(ctx)?;
    match search::describe(&doc, id) {
        Described::Found(matches) => Ok(json!({"stale": stale, "matches": matches})),
        Described::NotFound(candidates) => Err(ToolError {
            message: format!("not found: {id}"),
            data: Some(json!({"stale": stale, "candidates": candidates})),
        }),
    }
}
