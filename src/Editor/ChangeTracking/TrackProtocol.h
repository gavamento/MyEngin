//====================================================================================
//                          TrackProtocol.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          追跡サービスとの pipe の取り決め (版・op・通知・エラーコード)
//====================================================================================
#pragma once

namespace mye {

// 追跡サービス (tools\track) との pipe の取り決め。正本: docs\ai-change-tracking\ipc-contract.md
// 版の定数は tools\track\src\protocol.rs の PROTO_VERSION と一致させる (tools\check_rules.ps1 規則 9)
constexpr int kTrackProtoVersion = 1;

// 1 行 (改行を除く) の上限。超えたら接続を切る
constexpr int kTrackMaxLineBytes = 1024 * 1024;

namespace trackop {
inline constexpr const char* kHello = "hello";
inline constexpr const char* kStatus = "status";
inline constexpr const char* kSubscribe = "subscribe";
inline constexpr const char* kList = "list";
inline constexpr const char* kDescribe = "describe";
inline constexpr const char* kSessions = "sessions";
inline constexpr const char* kHunks = "hunks";
inline constexpr const char* kSceneDiff = "scene_diff";
// 開いている文書にかかっている構造の変更をエンティティごとにまとめて返す / パスから文書の ID を引く (role = editor だけ)
inline constexpr const char* kSceneOverlay = "scene_overlay";
inline constexpr const char* kDocumentForPath = "document_for_path";
inline constexpr const char* kBlobGet = "blob_get";
inline constexpr const char* kShutdown = "shutdown";
// Editor が保存した内容のハッシュを知らせる (role = editor だけ)。行為者を「Editor の利用者」にする根拠になる
inline constexpr const char* kEditorSaved = "editor_saved";
} // namespace trackop

namespace trackevent {
inline constexpr const char* kChangeRecorded = "change_recorded";
inline constexpr const char* kAttributionUpdated = "attribution_updated";
inline constexpr const char* kCoverageGap = "coverage_gap";
inline constexpr const char* kSessionChanged = "session_changed";
inline constexpr const char* kShuttingDown = "shutting_down";
// subscribe の対象ではなく、キューのあふれで通知を落としたとき必ず届く
inline constexpr const char* kResync = "resync";
} // namespace trackevent

namespace trackerror {
inline constexpr const char* kProtoMismatch = "proto_mismatch";
inline constexpr const char* kWrongProject = "wrong_project";
inline constexpr const char* kBadToken = "bad_token";
inline constexpr const char* kDbDegraded = "db_degraded";
inline constexpr const char* kBusy = "busy";
// クライアントが合成するもの (サービスは返さない)
inline constexpr const char* kTimeout = "timeout";
inline constexpr const char* kDisconnected = "disconnected";
inline constexpr const char* kNotConnected = "not_connected";
} // namespace trackerror

} // namespace mye
