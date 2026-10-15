//====================================================================================
//                          rpc.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          JSON-RPC 2.0 の要求の解析と応答の組み立て
//====================================================================================

use serde_json::{json, Value};

pub const PARSE_ERROR: i64 = -32700;
pub const INVALID_REQUEST: i64 = -32600;
pub const METHOD_NOT_FOUND: i64 = -32601;
pub const INVALID_PARAMS: i64 = -32602;
pub const INTERNAL_ERROR: i64 = -32603;
// MCP modern: _meta の protocolVersion をサーバが受けられないとき
pub const UNSUPPORTED_VERSION: i64 = -32022;

pub struct Request {
    // None = 通知 (応答しない)
    pub id: Option<Value>,
    pub method: String,
    // 無ければ Null
    pub params: Value,
}

pub struct RpcError {
    pub code: i64,
    pub message: String,
    pub data: Option<Value>,
}

impl RpcError {
    pub fn new(code: i64, message: impl Into<String>) -> Self {
        RpcError { code, message: message.into(), data: None }
    }
}

pub struct Response {
    pub id: Value,
    pub outcome: Result<Value, RpcError>,
}

impl Response {
    pub fn error(id: Value, code: i64, message: impl Into<String>) -> Self {
        Response { id, outcome: Err(RpcError::new(code, message)) }
    }

    // 1 行の JSON (改行なし)
    pub fn to_line(&self) -> String {
        let body = match &self.outcome {
            Ok(result) => json!({"jsonrpc": "2.0", "id": self.id, "result": result}),
            Err(e) => {
                let mut err = json!({"code": e.code, "message": e.message});
                if let Some(data) = &e.data {
                    err["data"] = data.clone();
                }
                json!({"jsonrpc": "2.0", "id": self.id, "error": err})
            }
        };
        body.to_string()
    }
}

// 1 行を要求へ解析する。解析できない行は、そのまま返せる誤り応答を Err で返す。
pub fn parse_request(line: &str) -> Result<Request, Response> {
    let value: Value = match serde_json::from_str(line) {
        Ok(v) => v,
        Err(e) => {
            // 詳細は serde_json の版で文面が変わるので応答には載せない (期待ファイルの安定のため)
            eprintln!("[mye_mcp] parse error: {e}");
            return Err(Response::error(Value::Null, PARSE_ERROR, "Parse error"));
        }
    };
    let obj = match value {
        Value::Array(_) => {
            return Err(Response::error(
                Value::Null,
                INVALID_REQUEST,
                "Batch requests are not supported",
            ));
        }
        Value::Object(o) => o,
        _ => {
            return Err(Response::error(Value::Null, INVALID_REQUEST, "Request must be an object"));
        }
    };

    // id: 文字列か数値だけ (MCP は null を許さない)。誤り応答に載せるのは妥当な id だけ
    let id = match obj.get("id") {
        None => None,
        Some(v @ (Value::String(_) | Value::Number(_))) => Some(v.clone()),
        Some(_) => {
            return Err(Response::error(
                Value::Null,
                INVALID_REQUEST,
                "id must be a string or a number",
            ));
        }
    };
    let reply_id = id.clone().unwrap_or(Value::Null);

    if obj.get("jsonrpc").and_then(Value::as_str) != Some("2.0") {
        return Err(Response::error(reply_id, INVALID_REQUEST, "jsonrpc must be \"2.0\""));
    }
    let method = match obj.get("method").and_then(Value::as_str) {
        Some(m) => m.to_string(),
        None => {
            return Err(Response::error(reply_id, INVALID_REQUEST, "method must be a string"));
        }
    };
    let params = obj.get("params").cloned().unwrap_or(Value::Null);
    Ok(Request { id, method, params })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn err_code(line: &str) -> i64 {
        match parse_request(line) {
            Err(r) => r.outcome.err().unwrap().code,
            Ok(_) => 0,
        }
    }

    #[test]
    fn 解析できない行は_32700() {
        assert_eq!(err_code("{"), PARSE_ERROR);
        assert_eq!(err_code(""), PARSE_ERROR);
    }

    #[test]
    fn バッチと非オブジェクトは_32600() {
        assert_eq!(err_code("[]"), INVALID_REQUEST);
        assert_eq!(err_code("1"), INVALID_REQUEST);
        assert_eq!(err_code(r#"{"jsonrpc":"2.0","id":null,"method":"ping"}"#), INVALID_REQUEST);
        assert_eq!(err_code(r#"{"jsonrpc":"1.0","id":1,"method":"ping"}"#), INVALID_REQUEST);
        assert_eq!(err_code(r#"{"jsonrpc":"2.0","id":1}"#), INVALID_REQUEST);
    }

    #[test]
    fn id_が無ければ通知() {
        let r = parse_request(r#"{"jsonrpc":"2.0","method":"notifications/initialized"}"#).ok().unwrap();
        assert!(r.id.is_none());
    }
}
