//====================================================================================
//                          tools.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          MCP の道具表 (tools/list の定義と tools/call の実行)
//====================================================================================

use crate::handler::Context;
use crate::index;
use crate::notes;
use crate::rpc::{RpcError, INVALID_PARAMS};
use crate::search::{self, Described, KINDS};
use crate::track_client::{self, TrackError};
use crate::verify;
use serde_json::{json, Value};

const QUERY_MAX_CHARS: usize = 200;
const LIMIT_DEFAULT: i64 = 20;
const LIMIT_MAX: i64 = 100;
// change_describe の hunks と evidence の合計の上限 (note_read と同じ 64 KB の考え方)
const DESCRIBE_MAX_BYTES: usize = 64 * 1024;
const TRACK_PROVIDERS: [&str; 4] = ["claude", "codex", "gemini", "other"];
const TRACK_CHANGE_TYPES: [&str; 4] = ["added", "modified", "deleted", "renamed"];

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

// tools/list の中身。固定順。
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
        },
        {
            "name": "verify_job",
            "description": "Run one tools\\replay_verify.bat job (prebuilt Debug and Release Editor.exe required; nothing is built) in the background and read its structured result. op=start returns a jobId; op=poll returns running or, when done, exitCode plus a summary (failed stage, first mismatching tick, diagnostic lines; not the whole log). At most 2 jobs run at once and a job name cannot be started twice at the same time.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "op": {"type": "string", "enum": ["start", "poll"]},
                    "job": {"type": "string", "enum": verify::JOBS, "description": "op=start: the job name (the :job_* labels of replay_verify.bat)."},
                    "ticks": {"type": "integer", "minimum": verify::TICKS_MIN, "maximum": verify::TICKS_MAX, "default": verify::TICKS_DEFAULT, "description": "op=start: ticks per scene (MYE_RV_TICKS)."},
                    "jobId": {"type": "string", "description": "op=poll: the jobId returned by start."}
                },
                "required": ["op"],
                "additionalProperties": false
            }
        },
        {
            "name": "note_read",
            "description": "Read the project's append-only notes (.mye/notes/<topic>.md, gitignored; the engine index is separate). Omit topic to list topics ([{topic, bytes, lastHeading}] in name order). With topic, returns the latest `latest` entries (default 5, max 50) oldest first; the entries are capped at 64 KB measured as JSON-escaped heading + text (oldest entries are dropped, truncated: true).",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "topic": {"type": "string", "pattern": "^[A-Za-z0-9_-]{1,64}$", "description": "Topic name. Omit to list topics."},
                    "latest": {"type": "integer", "minimum": 1, "maximum": notes::LATEST_MAX, "default": notes::LATEST_DEFAULT, "description": "Number of newest entries."}
                },
                "additionalProperties": false
            }
        },
        {
            "name": "note_append",
            "description": "Append one entry to a topic (created on first use) as `## <UTC time> <title>` followed by the text. Append-only: entries cannot be edited, overwritten or deleted. Returns the relative path and the file size after the append. Use for the current state of work that should survive the session; not for facts that belong in docs or ADRs.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "topic": {"type": "string", "pattern": "^[A-Za-z0-9_-]{1,64}$", "description": "Topic name (letters, digits, _ and -; Windows device names such as con / nul / com1 are refused)."},
                    "title": {"type": "string", "minLength": 1, "maxLength": notes::TITLE_MAX_CHARS, "description": "Optional one-line heading, 1-120 characters."},
                    "text": {"type": "string", "minLength": 1, "description": "Entry body, 1-65536 bytes (UTF-8)."}
                },
                "required": ["topic", "text"],
                "additionalProperties": false
            }
        },
        {
            "name": "change_session_begin",
            "description": "Start an AI change-tracking session for this project before you edit files (the tracking service is started if it is not running). Returns a sessionId; pass it to change_report and change_session_end. Sessions are optional: edits are recorded either way, a session only lets them be attributed to you.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "provider": {"type": "string", "enum": TRACK_PROVIDERS, "description": "Who is editing."},
                    "workspace": {"type": "string", "description": "Directory you work in (default: the project root)."},
                    "clientSessionId": {"type": "string", "minLength": 1, "description": "Your own stable session id; beginning again with the same id resumes the session."},
                    "isolated": {"type": "boolean", "description": "true if you work in a dedicated git worktree."}
                },
                "required": ["provider"],
                "additionalProperties": false
            }
        },
        {
            "name": "change_session_end",
            "description": "End a change-tracking session when your edits are done. Returns the session status and changeCount (the changes recorded for it).",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "sessionId": {"type": "string", "minLength": 1, "description": "The sessionId returned by change_session_begin."}
                },
                "required": ["sessionId"],
                "additionalProperties": false
            }
        },
        {
            "name": "change_report",
            "description": "After editing, report the files you changed. The service compares your report with what it observed on disk and answers per path: matched, mismatch or not_observed, plus the attribution. A report alone never makes a change `verified` (it stays `claimed`); hashes are optional (without them the result is mismatch). Begin a session first, edit, then report.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "sessionId": {"type": "string", "minLength": 1, "description": "The sessionId returned by change_session_begin."},
                    "operationId": {"type": "string", "minLength": 1, "description": "Optional operationId to tie the report to."},
                    "paths": {"type": "array", "items": {"type": "string", "minLength": 1}, "minItems": 1, "description": "Changed files (project-relative or absolute, inside the project)."},
                    "beforeHashes": {"description": "Optional SHA-256 of each file before your edit: an array in the order of paths, or an object keyed by path (null = did not exist).", "type": ["array", "object"]},
                    "afterHashes": {"description": "Optional SHA-256 of each file after your edit, same shape as beforeHashes (null = deleted).", "type": ["array", "object"]}
                },
                "required": ["sessionId", "paths"],
                "additionalProperties": false
            }
        },
        {
            "name": "change_list",
            "description": "List recorded changes (newest first), optionally for one session, path (a trailing / matches a prefix), time range (UTC milliseconds), review state, attribution or change type. Returns items and nextCursor (pass it as cursor for the next page). Blobs and hunk bodies are never included. If coverage.baselining is true the list may be incomplete.",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "sessionId": {"type": "string", "minLength": 1},
                    "path": {"type": "string", "minLength": 1},
                    "from": {"type": "integer", "description": "Earliest observedAt, UTC milliseconds."},
                    "to": {"type": "integer", "description": "Latest observedAt, UTC milliseconds."},
                    "review": {"type": "string"},
                    "attribution": {"type": "string"},
                    "changeType": {"type": "string", "enum": TRACK_CHANGE_TYPES},
                    "cursor": {"type": "string", "description": "nextCursor of the previous page."},
                    "limit": {"type": "integer", "minimum": 1, "maximum": 200, "description": "Items per page (default 50)."}
                },
                "additionalProperties": false
            }
        },
        {
            "name": "change_describe",
            "description": "Describe one recorded change by revisionId (from change_list): before / after hashes and sizes, hunk summaries (line ranges, not bodies), attribution with its evidence, restorability. The hunks and evidence are capped at 64 KB in total (truncated: true when cut).",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "revisionId": {"type": "integer", "minimum": 1, "description": "The revisionId returned by change_list."}
                },
                "required": ["revisionId"],
                "additionalProperties": false
            }
        }
    ])
}

// tools/call。params の形が悪ければ -32602、道具の中の失敗は isError の結果にする。
pub fn call(ctx: &mut Context, params: &Value) -> Result<Value, RpcError> {
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
        "verify_job" => verify_job_tool(ctx, args),
        "note_read" => note_read_tool(ctx, args),
        "note_append" => note_append_tool(ctx, args),
        "change_session_begin" => change_session_begin_tool(ctx, args),
        "change_session_end" => change_session_end_tool(ctx, args),
        "change_report" => change_report_tool(ctx, args),
        "change_list" => change_list_tool(ctx, args),
        "change_describe" => change_describe_tool(ctx, args),
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
    // notes を引かない絞り込みなら notes には触らない。notes が読めなければ engine だけで答える
    let wants_notes = kinds.is_empty() || kinds.iter().any(|k| k == "note");
    let note_headings = match ctx.notes_base() {
        Ok(base) if wants_notes => notes::headings(base),
        _ => Vec::new(),
    };
    let outcome = search::search(&doc, q, &kinds, limit as usize, &note_headings);
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

fn verify_job_tool(ctx: &mut Context, args: &Value) -> Result<Value, ToolError> {
    let op = args
        .get("op")
        .and_then(Value::as_str)
        .ok_or_else(|| ToolError::new("op is required and must be \"start\" or \"poll\""))?;
    match op {
        "start" => {
            reject_unknown(args, &["op", "job", "ticks"])?;
            let job = args
                .get("job")
                .and_then(Value::as_str)
                .ok_or_else(|| ToolError::new("job is required for op=start and must be a string"))?;
            let ticks = match args.get("ticks") {
                None | Some(Value::Null) => None,
                Some(v) => Some(v.as_i64().ok_or_else(|| ToolError::new("ticks must be an integer"))?),
            };
            let root = ctx.root.clone();
            ctx.jobs.start(root.as_deref(), job, ticks).map_err(ToolError::new)
        }
        "poll" => {
            reject_unknown(args, &["op", "jobId"])?;
            let id = args
                .get("jobId")
                .and_then(Value::as_str)
                .ok_or_else(|| ToolError::new("jobId is required for op=poll and must be a string"))?;
            ctx.jobs.poll(id).map_err(ToolError::new)
        }
        _ => Err(ToolError::new("op must be \"start\" or \"poll\"")),
    }
}

fn optional_string<'a>(args: &'a Value, key: &str) -> Result<Option<&'a str>, ToolError> {
    match args.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(v) => v.as_str().map(Some).ok_or_else(|| ToolError::new(format!("{key} must be a string"))),
    }
}

fn note_read_tool(ctx: &Context, args: &Value) -> Result<Value, ToolError> {
    reject_unknown(args, &["topic", "latest"])?;
    let topic = optional_string(args, "topic")?;
    let latest = match args.get("latest") {
        None | Some(Value::Null) => None,
        Some(v) => Some(v.as_i64().ok_or_else(|| ToolError::new("latest must be an integer"))?),
    };
    if topic.is_none() && latest.is_some() {
        return Err(ToolError::new("latest needs a topic"));
    }
    let base = ctx.notes_base().map_err(ToolError::new)?;
    notes::read(base, topic, latest).map_err(ToolError::new)
}

fn note_append_tool(ctx: &Context, args: &Value) -> Result<Value, ToolError> {
    reject_unknown(args, &["topic", "title", "text"])?;
    let topic = optional_string(args, "topic")?.ok_or_else(|| ToolError::new("topic is required and must be a string"))?;
    let title = optional_string(args, "title")?;
    let text = optional_string(args, "text")?.ok_or_else(|| ToolError::new("text is required and must be a string"))?;
    let base = ctx.notes_base().map_err(ToolError::new)?;
    notes::append(base, topic, title, text, std::time::SystemTime::now()).map_err(ToolError::new)
}

// ---- 変更の追跡 (M104h)。引数を検査して追跡サービスへ取り次ぐだけで、判定はサービスがする ----

impl From<TrackError> for ToolError {
    fn from(e: TrackError) -> Self {
        ToolError {
            message: format!("{}: {}", e.code, e.detail),
            data: Some(json!({"code": e.code, "detail": e.detail})),
        }
    }
}

// 追跡するプロジェクトは notes と同じ決め方 (--project、無ければ root)。存在しないディレクトリでは何も起動しない
fn track_call(ctx: &Context, op: &str, args: Value) -> Result<Value, ToolError> {
    let project = ctx.notes_base().map_err(ToolError::new)?;
    if !project.is_dir() {
        return Err(ToolError::new(format!("project directory does not exist: {}", project.display())));
    }
    Ok(track_client::call(project, op, args)?)
}

fn required_string<'a>(args: &'a Value, key: &str) -> Result<&'a str, ToolError> {
    match optional_string(args, key)? {
        Some(s) if !s.is_empty() => Ok(s),
        Some(_) => Err(ToolError::new(format!("{key} must not be empty"))),
        None => Err(ToolError::new(format!("{key} is required and must be a string"))),
    }
}

fn optional_int(args: &Value, key: &str) -> Result<Option<i64>, ToolError> {
    match args.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(v) => v.as_i64().map(Some).ok_or_else(|| ToolError::new(format!("{key} must be an integer"))),
    }
}

fn change_session_begin_tool(ctx: &Context, args: &Value) -> Result<Value, ToolError> {
    reject_unknown(args, &["provider", "workspace", "clientSessionId", "isolated"])?;
    let provider = required_string(args, "provider")?;
    if !TRACK_PROVIDERS.contains(&provider) {
        return Err(ToolError::new(format!("provider must be one of: {}", TRACK_PROVIDERS.join(", "))));
    }
    let workspace = optional_string(args, "workspace")?;
    if workspace.is_some_and(str::is_empty) {
        return Err(ToolError::new("workspace must not be empty"));
    }
    let mut service_args = json!({"provider": provider, "source": "mcp"});
    if let Some(id) = optional_string(args, "clientSessionId")? {
        if id.is_empty() {
            return Err(ToolError::new("clientSessionId must not be empty"));
        }
        service_args["clientSessionId"] = json!(id);
    }
    match args.get("isolated") {
        None | Some(Value::Null) => {}
        Some(Value::Bool(b)) => service_args["isolated"] = json!(b),
        Some(_) => return Err(ToolError::new("isolated must be a boolean")),
    }
    // 既定の作業場所はプロジェクトのルート
    service_args["workspace"] = match workspace {
        Some(w) => json!(w),
        None => json!(ctx.notes_base().map_err(ToolError::new)?.to_string_lossy()),
    };
    track_call(ctx, "session_begin", service_args)
}

fn change_session_end_tool(ctx: &Context, args: &Value) -> Result<Value, ToolError> {
    reject_unknown(args, &["sessionId"])?;
    let session_id = required_string(args, "sessionId")?;
    track_call(ctx, "session_end", json!({"sessionId": session_id}))
}

// beforeHashes / afterHashes: paths と同じ並びの配列 (要素は文字列か null)、またはパスをキーにしたオブジェクト (値は文字列か null)
fn hashes_arg(args: &Value, key: &str) -> Result<Option<Value>, ToolError> {
    let is_hash = |v: &Value| v.is_null() || v.is_string();
    match args.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(v @ Value::Array(items)) if items.iter().all(is_hash) => Ok(Some(v.clone())),
        Some(v @ Value::Object(map)) if map.values().all(is_hash) => Ok(Some(v.clone())),
        Some(_) => Err(ToolError::new(format!("{key} must be an array or an object of hash strings (or null)"))),
    }
}

fn change_report_tool(ctx: &Context, args: &Value) -> Result<Value, ToolError> {
    reject_unknown(args, &["sessionId", "operationId", "paths", "beforeHashes", "afterHashes"])?;
    let session_id = required_string(args, "sessionId")?;
    let paths = match args.get("paths") {
        Some(Value::Array(items)) if !items.is_empty() && items.iter().all(|p| p.as_str().is_some_and(|s| !s.is_empty())) => {
            Value::Array(items.clone())
        }
        None | Some(Value::Null) => return Err(ToolError::new("paths is required and must be an array of strings")),
        Some(_) => return Err(ToolError::new("paths must be a non-empty array of non-empty strings")),
    };
    let mut service_args = json!({"sessionId": session_id, "paths": paths});
    if let Some(op) = optional_string(args, "operationId")? {
        if op.is_empty() {
            return Err(ToolError::new("operationId must not be empty"));
        }
        service_args["operationId"] = json!(op);
    }
    for key in ["beforeHashes", "afterHashes"] {
        if let Some(v) = hashes_arg(args, key)? {
            service_args[key] = v;
        }
    }
    track_call(ctx, "report", service_args)
}

fn change_list_tool(ctx: &Context, args: &Value) -> Result<Value, ToolError> {
    reject_unknown(
        args,
        &["sessionId", "path", "from", "to", "review", "attribution", "changeType", "cursor", "limit"],
    )?;
    let mut service_args = json!({});
    for key in ["sessionId", "path", "review", "attribution", "changeType", "cursor"] {
        if let Some(v) = optional_string(args, key)? {
            service_args[key] = json!(v);
        }
    }
    if let Some(t) = service_args.get("changeType").and_then(Value::as_str) {
        if !TRACK_CHANGE_TYPES.contains(&t) {
            return Err(ToolError::new(format!("changeType must be one of: {}", TRACK_CHANGE_TYPES.join(", "))));
        }
    }
    for key in ["from", "to", "limit"] {
        if let Some(v) = optional_int(args, key)? {
            service_args[key] = json!(v);
        }
    }
    track_call(ctx, "list", service_args)
}

// hunks と evidence を合計 DESCRIBE_MAX_BYTES で切る (先頭から残す)。切ったかどうかを truncated に入れる
fn cap_describe(mut result: Value) -> Value {
    let mut budget = DESCRIBE_MAX_BYTES;
    let mut truncated = false;
    for key in ["hunks", "evidence"] {
        if let Some(Value::Array(items)) = result.get_mut(key) {
            let mut keep = 0;
            for item in items.iter() {
                let size = item.to_string().len();
                if size > budget {
                    truncated = true;
                    break;
                }
                budget -= size;
                keep += 1;
            }
            items.truncate(keep);
        }
    }
    if let Some(obj) = result.as_object_mut() {
        obj.insert("truncated".to_string(), json!(truncated));
    }
    result
}

fn change_describe_tool(ctx: &Context, args: &Value) -> Result<Value, ToolError> {
    reject_unknown(args, &["revisionId"])?;
    let revision_id = optional_int(args, "revisionId")?
        .ok_or_else(|| ToolError::new("revisionId is required and must be an integer"))?;
    if revision_id < 1 {
        return Err(ToolError::new("revisionId must be 1 or greater"));
    }
    let result = track_call(ctx, "describe", json!({"revisionId": revision_id}))?;
    Ok(cap_describe(result))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn no_project() -> Context {
        Context { root: None, index: None, project: None, jobs: verify::JobTable::default() }
    }

    #[test]
    fn 追跡の道具は引数の誤りをサービスへ届く前に弾く() {
        let ctx = no_project();
        let bad = |f: fn(&Context, &Value) -> Result<Value, ToolError>, args: Value| f(&ctx, &args).err().unwrap().message;
        assert!(bad(change_session_begin_tool, json!({"provider": "claude", "x": 1})).starts_with("unknown argument"));
        assert!(bad(change_session_begin_tool, json!({})).contains("provider is required"));
        assert!(bad(change_session_begin_tool, json!({"provider": "nobody"})).contains("one of"));
        assert!(bad(change_session_begin_tool, json!({"provider": "claude", "isolated": "yes"})).contains("boolean"));
        assert!(bad(change_session_end_tool, json!({"sessionId": 3})).contains("must be a string"));
        assert!(bad(change_report_tool, json!({"sessionId": "s"})).contains("paths is required"));
        assert!(bad(change_report_tool, json!({"sessionId": "s", "paths": []})).contains("non-empty"));
        assert!(bad(change_report_tool, json!({"sessionId": "s", "paths": ["a"], "afterHashes": 5})).contains("afterHashes"));
        assert!(bad(change_list_tool, json!({"limit": "9"})).contains("limit must be an integer"));
        assert!(bad(change_list_tool, json!({"changeType": "moved"})).contains("changeType"));
        assert!(bad(change_describe_tool, json!({})).contains("revisionId is required"));
        assert!(bad(change_describe_tool, json!({"revisionId": 0})).contains("1 or greater"));
    }

    #[test]
    fn 検査を通ってもプロジェクトが無ければ_iserror() {
        let ctx = no_project();
        let e = change_session_end_tool(&ctx, &json!({"sessionId": "s"})).err().unwrap();
        assert!(e.message.contains("--project"));
    }

    #[test]
    fn describe_は_64kb_で切って_truncated_を立てる() {
        let big = "x".repeat(1000);
        let hunks: Vec<Value> = (0..100).map(|i| json!({"ordinal": i, "pad": big})).collect();
        let capped = cap_describe(json!({"revisionId": 1, "hunks": hunks, "evidence": [{"kind": "k"}]}));
        assert_eq!(capped["truncated"], true);
        let kept = capped["hunks"].as_array().unwrap().len();
        assert!(kept > 0 && kept < 100);
        assert!(capped["hunks"].to_string().len() <= DESCRIBE_MAX_BYTES + kept);
        let small = cap_describe(json!({"hunks": [{"ordinal": 0}], "evidence": []}));
        assert_eq!(small["truncated"], false);
        assert_eq!(small["hunks"].as_array().unwrap().len(), 1);
    }
}
