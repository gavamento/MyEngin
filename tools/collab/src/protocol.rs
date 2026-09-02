// 要求 / 応答 / 通知のメッセージ形 (spec §4.0 決定 2)。
//
// C++ とは **UTF-8 の JSON 文字列 1 本**でしか会話しない。op ごとの C ABI スロットは
// 作らない — 作ると Interop.cs と同じ「位置ベースのミラー」を新しく 1 本増やすことに
// なり、規則 11 と同種の照合をもう 1 組維持する羽目になる。
//
// ★JSON のオブジェクトキーは serde_json の既定 (BTreeMap) で **辞書順**に出る。
//   preserve_order を有効にしていないのはこのため — 宣言順に依存しない = 期待
//   NDJSON (collab_verify) がフィールドの並べ替えで割れない。

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

/// C++ の `kCollabProtoVersion` (src\Editor\SourceControl\CollabProtocol.h) と
/// 一致していなければならない。**check_rules.ps1 の規則 9 ($constGroups) が機械照合する**
/// — DLL と exe は別々にビルドされて別々に配られるので、食い違いは「起動はするが
/// 応答の形だけ違う」= 最も気付きにくい壊れ方をする。
pub const PROTO_VERSION: u32 = 1;

/// `error.code` の一覧 (spec §4.1 で v1 に凍結)。
/// 文字列を直書きせずここを通すのは、C++ 側が code → Tr() の表を持つため
/// (綴りがずれると未知 code 扱いで生の英語が UI に出る)。
pub mod code {
    pub const NOT_REPO: &str = "not_repo";
    pub const TOPLEVEL_MISMATCH: &str = "toplevel_mismatch";
    pub const GIT_MISSING: &str = "git_missing";
    pub const GIT_TOO_OLD: &str = "git_too_old";
    pub const IDENTITY_MISSING: &str = "identity_missing";
    pub const LOCAL_CHANGES_OVERWRITTEN: &str = "local_changes_overwritten";
    pub const LOCKED_INDEX: &str = "locked_index";
    pub const LOCKED_FILE: &str = "locked_file";
    pub const AUTH_FAILED: &str = "auth_failed";
    pub const NON_FAST_FORWARD: &str = "non_fast_forward";
    pub const CONFLICT: &str = "conflict";
    pub const MERGE_IN_PROGRESS: &str = "merge_in_progress";
    pub const NOTHING_TO_COMMIT: &str = "nothing_to_commit";
    pub const NETWORK: &str = "network";
    pub const INTERNAL_PANIC: &str = "internal_panic";
    pub const SERVICE_DEAD: &str = "service_dead";
    pub const BAD_REQUEST: &str = "bad_request";
    /// 未分類。`detail` に git の stderr をそのまま載せる
    pub const GIT_FAILED: &str = "git_failed";
}

/// 通知の `event` 名 (spec §4.1)。
pub mod event {
    pub const STATUS_CHANGED: &str = "status_changed";
    pub const REMOTE_CHANGED: &str = "remote_changed";
    pub const REPO_CHANGED: &str = "repo_changed";
    pub const SERVICE_ERROR: &str = "service_error";
}

#[derive(Debug, Clone, Deserialize)]
pub struct Request {
    pub id: u64,
    pub op: String,
    #[serde(default)]
    pub args: Value,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ErrorBody {
    pub code: String,
    pub detail: String,
    /// `local_changes_overwritten` などで対象ファイルを列挙する枠 (v1 では省略可)
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub paths: Option<Vec<String>>,
}

impl ErrorBody {
    pub fn new(code: &str, detail: impl Into<String>) -> Self {
        ErrorBody { code: code.to_string(), detail: detail.into(), paths: None }
    }

    pub fn with_paths(code: &str, detail: impl Into<String>, paths: Vec<String>) -> Self {
        ErrorBody { code: code.to_string(), detail: detail.into(), paths: Some(paths) }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct Response {
    pub id: u64,
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub result: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<ErrorBody>,
}

impl Response {
    pub fn ok(id: u64, result: Value) -> Self {
        Response { id, ok: true, result: Some(result), error: None }
    }

    pub fn err(id: u64, error: ErrorBody) -> Self {
        Response { id, ok: false, result: None, error: Some(error) }
    }

    /// 1 行の JSON へ。**シリアライズが失敗しうる型は入っていない**ので、
    /// 万一失敗したら最低限の JSON を手で組んで返す (poll が黙って詰まるより良い)
    pub fn to_line(&self) -> String {
        serde_json::to_string(self).unwrap_or_else(|e| {
            format!(
                "{{\"id\":{},\"ok\":false,\"error\":{{\"code\":\"{}\",\"detail\":\"serialize failed: {}\"}}}}",
                self.id,
                code::GIT_FAILED,
                e
            )
        })
    }
}

/// 通知を 1 行の JSON にする。`body` はオブジェクトであること
/// (オブジェクト以外が来たら `detail` に押し込んで捨てない)。
pub fn event_line(name: &str, body: Value) -> String {
    let mut map = serde_json::Map::new();
    map.insert("event".to_string(), Value::String(name.to_string()));
    match body {
        Value::Object(o) => {
            for (k, v) in o {
                map.insert(k, v);
            }
        }
        Value::Null => {}
        other => {
            map.insert("detail".to_string(), other);
        }
    }
    Value::Object(map).to_string()
}

/// `service_error` 通知 (spec §4.0: worker の panic は 1 回だけ通知してハンドルを dead 化)
pub fn service_error_line(code: &str, detail: &str) -> String {
    event_line(event::SERVICE_ERROR, json!({ "code": code, "detail": detail }))
}
