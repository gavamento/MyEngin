//====================================================================================
//                          handler.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          JSON-RPC 要求の処理 (MCP の legacy / modern 両対応。transport は知らない)
//====================================================================================

use crate::rpc::{
    Request, Response, RpcError, INVALID_PARAMS, METHOD_NOT_FOUND, UNSUPPORTED_VERSION,
};
use crate::tools;
use serde_json::{json, Value};
use std::path::PathBuf;

pub const SERVER_NAME: &str = "mye-engine";
pub const SERVER_VERSION: &str = env!("CARGO_PKG_VERSION");

// initialize で echo する legacy の版。それ以外は LEGACY_DEFAULT を返す
const LEGACY_VERSIONS: [&str; 3] = ["2025-11-25", "2025-06-18", "2025-03-26"];
const LEGACY_DEFAULT: &str = "2025-11-25";
const MODERN_VERSION: &str = "2026-07-28";
const SUPPORTED_VERSIONS: [&str; 4] = ["2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"];
// modern のリクエストが版を運ぶ _meta のキー
const META_VERSION_KEY: &str = "io.modelcontextprotocol/protocolVersion";
const META_SERVER_INFO_KEY: &str = "io.modelcontextprotocol/serverInfo";
// modern の list 結果が必須とするキャッシュ指示 (Claude Code は欠けると tools/list を拒否する)。
// 道具表はプロセスの寿命の間固定で、クライアントごとに変わらない。
const LIST_TTL_MS: u64 = 3_600_000;
const LIST_CACHE_SCOPE: &str = "public";

pub struct Context {
    // リポジトリのルート。見つからなければ None (道具が isError で理由を返す)
    pub root: Option<PathBuf>,
    // --index で明示された索引。None なら <root>\cache\engine_index.json
    pub index: Option<PathBuf>,
}

impl Context {
    // 索引の場所と、エラー文に出す表示名 (絶対パスを避けるため、指定された文字列かルート相対)
    pub fn index_location(&self) -> Result<(PathBuf, String), String> {
        if let Some(p) = &self.index {
            return Ok((p.clone(), p.display().to_string()));
        }
        match &self.root {
            Some(r) => Ok((r.join("cache").join("engine_index.json"), "cache\\engine_index.json".to_string())),
            None => Err("repo root not found (MyEngine.sln); start the server with --root <repo>".to_string()),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Era {
    Legacy,
    Modern,
}

// 要求 1 件を処理する。通知 (id なし) には None。
pub fn handle(ctx: &mut Context, req: Request) -> Option<Response> {
    let era = match requested_version(&req.params) {
        None => Era::Legacy,
        Some(v) if v.as_str() == Some(MODERN_VERSION) => Era::Modern,
        Some(v) => {
            log(&req.method, "unsupported", &v.to_string());
            let id = req.id?;
            return Some(Response {
                id,
                outcome: Err(RpcError {
                    code: UNSUPPORTED_VERSION,
                    message: "Unsupported protocol version".to_string(),
                    data: Some(json!({"supported": SUPPORTED_VERSIONS, "requested": v})),
                }),
            });
        }
    };
    log(&req.method, if era == Era::Modern { "modern" } else { "legacy" }, "");
    let id = req.id?;
    let method = req.method.as_str();
    let outcome = dispatch(ctx, era, method, &req.params).map(|mut result| {
        if era == Era::Modern {
            if let Some(o) = result.as_object_mut() {
                o.insert("resultType".to_string(), json!("complete"));
                if method == "tools/list" {
                    o.insert("ttlMs".to_string(), json!(LIST_TTL_MS));
                    o.insert("cacheScope".to_string(), json!(LIST_CACHE_SCOPE));
                }
            }
        }
        result
    });
    Some(Response { id, outcome })
}

// params._meta の protocolVersion。無ければ None (= legacy の要求)
fn requested_version(params: &Value) -> Option<Value> {
    params.get("_meta")?.get(META_VERSION_KEY).cloned()
}

fn dispatch(ctx: &Context, era: Era, method: &str, params: &Value) -> Result<Value, RpcError> {
    match method {
        "initialize" => initialize(params),
        "ping" => Ok(json!({})),
        "server/discover" => Ok(discover()),
        "tools/list" => Ok(json!({"tools": tools::list()})),
        "tools/call" => tools::call(ctx, params),
        _ => {
            let _ = era;
            Err(RpcError::new(METHOD_NOT_FOUND, format!("Method not found: {method}")))
        }
    }
}

fn server_info() -> Value {
    json!({"name": SERVER_NAME, "version": SERVER_VERSION})
}

fn capabilities() -> Value {
    json!({"tools": {}})
}

fn initialize(params: &Value) -> Result<Value, RpcError> {
    let requested = params
        .get("protocolVersion")
        .and_then(Value::as_str)
        .ok_or_else(|| RpcError::new(INVALID_PARAMS, "params.protocolVersion must be a string"))?;
    let version = if LEGACY_VERSIONS.contains(&requested) { requested } else { LEGACY_DEFAULT };
    Ok(json!({
        "protocolVersion": version,
        "capabilities": capabilities(),
        "serverInfo": server_info(),
    }))
}

fn discover() -> Value {
    json!({
        "supportedVersions": SUPPORTED_VERSIONS,
        "capabilities": capabilities(),
        "_meta": {META_SERVER_INFO_KEY: server_info()},
    })
}

// どの時代で繋がったかを残す (stderr。stdout は JSON-RPC 専用)
fn log(method: &str, era: &str, detail: &str) {
    if detail.is_empty() {
        eprintln!("[mye_mcp] {method} era={era}");
    } else {
        eprintln!("[mye_mcp] {method} era={era} {detail}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpc::parse_request;

    fn run(line: &str) -> Option<Value> {
        let mut ctx = Context { root: None, index: None };
        let req = parse_request(line).ok().unwrap();
        handle(&mut ctx, req).map(|r| serde_json::from_str(&r.to_line()).unwrap())
    }

    #[test]
    fn legacy_initialize_は版を_echo_する() {
        for v in LEGACY_VERSIONS {
            let r = run(&format!(
                r#"{{"jsonrpc":"2.0","id":1,"method":"initialize","params":{{"protocolVersion":"{v}"}}}}"#
            ))
            .unwrap();
            assert_eq!(r["result"]["protocolVersion"], v);
        }
        let r = run(r#"{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1999-01-01"}}"#).unwrap();
        assert_eq!(r["result"]["protocolVersion"], LEGACY_DEFAULT);
    }

    #[test]
    fn modern_は_resulttype_を付け_未知版は_32022() {
        let ok = run(r#"{"jsonrpc":"2.0","id":2,"method":"ping","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}"#).unwrap();
        assert_eq!(ok["result"]["resultType"], "complete");
        let bad = run(r#"{"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2030-01-01"}}}"#).unwrap();
        assert_eq!(bad["error"]["code"], UNSUPPORTED_VERSION);
        assert_eq!(bad["error"]["data"]["requested"], "2030-01-01");
    }

    #[test]
    fn modern_の_tools_list_はキャッシュ指示を持つ() {
        let r = run(r#"{"jsonrpc":"2.0","id":5,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}"#).unwrap();
        assert_eq!(r["result"]["ttlMs"], LIST_TTL_MS);
        assert_eq!(r["result"]["cacheScope"], LIST_CACHE_SCOPE);
        let legacy = run(r#"{"jsonrpc":"2.0","id":6,"method":"tools/list"}"#).unwrap();
        assert!(legacy["result"].get("ttlMs").is_none());
    }

    #[test]
    fn 通知には応答しない() {
        assert!(run(r#"{"jsonrpc":"2.0","method":"notifications/initialized"}"#).is_none());
        assert!(run(r#"{"jsonrpc":"2.0","method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2030-01-01"}}}"#).is_none());
    }

    #[test]
    fn 未知メソッドは_32601() {
        let r = run(r#"{"jsonrpc":"2.0","id":4,"method":"resources/list"}"#).unwrap();
        assert_eq!(r["error"]["code"], METHOD_NOT_FOUND);
    }
}
