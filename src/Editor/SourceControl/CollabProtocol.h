#pragma once

// MyeCollab.dll との会話の定数 (M66a)。
// 実装形態と ABI の正本は plans\m66-git-collab\spec.md §4.0 / ADR-015。

namespace mye {

// MyeCollab.dll の PROTO_VERSION (tools\collab\src\protocol.rs) と一致していること。
// ★DLL と Editor.exe は**別々にビルドされて別々に配られる**ので、食い違いは
//   「起動はするが応答の形だけ違う」= 最も気付きにくい壊れ方をする。
//   check_rules.ps1 の規則 9 ($constGroups) が両者を機械照合しているので、
//   片方だけ変えると規則検査で止まる (そこが唯一の防波堤)
constexpr int kCollabProtoVersion = 1;

// op 名 (spec §4.1 で v1 に凍結)。文字列直書きを避けるのは、綴り違いが
// bad_request として**実行時にしか**現れないため。M66a で使うのは 3 本だけで、
// 残りは後続サブが埋める
namespace collabop {
constexpr const char* kHello = "hello";
constexpr const char* kRepoCheck = "repo_check";
constexpr const char* kStatus = "status";
} // namespace collabop

// Source Control が使えない理由 (spec §4.3)。UI はこれを Tr() で文言にする。
// 「使えない」を 1 つの bool に潰さないのは、理由ごとに**ユーザーがすべきことが違う**ため
// (DLL が無い → ビルドし直す / git が古い → 入れ替える / リポジトリでない → clone する)
enum class Unavailable {
    None,             // 利用可能
    NoProject,        // 裸起動 (プロジェクトを開いていない)
    NoService,        // MyeCollab.dll が無い / ロードできない
    ProtoMismatch,    // DLL の proto 版が違う
    NoGit,            // git が PATH に無い
    GitTooOld,        // git < 2.11 (status --porcelain=v2 が無い)
    NotRepo,          // プロジェクトが git 管理下でない
    ToplevelMismatch, // プロジェクトルートがリポジトリのトップでない
    ServiceDied,      // worker が panic した (service_error 通知を受けた)
};

} // namespace mye
