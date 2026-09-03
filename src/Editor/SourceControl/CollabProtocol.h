#pragma once
#include <string_view>

// MyeCollab.dll との会話の定数 (M66a / M66b)。
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
constexpr const char* kHintChanged = "hint_changed"; // M66b: 保存直後の即時取り直し
// M66c。stage / unstage / commit は書き込み系 = CollabOpKindOf が Write に落とす
// (無期限に待つ + OpInFlight でボタンを塞ぐ)。log / diff / identity_check は
// 上の kReadOps 表に既に載っている
constexpr const char* kStage = "stage";
constexpr const char* kUnstage = "unstage";
constexpr const char* kCommit = "commit";
constexpr const char* kLog = "log";
constexpr const char* kDiff = "diff";
constexpr const char* kIdentityCheck = "identity_check";
// M66d。revert は書き込み系 = GitTransaction (ゲート + ReloadHub の一括適用) 経由でだけ
// 投げる。diff_names は読み取り系で、既に上の kReadOps 表に載っている
constexpr const char* kRevert = "revert";
constexpr const char* kDiffNames = "diff_names";
// M66e。branches は読み取り系 (上の kReadOps に既に載っている)。branch_create は
// ref を 1 本足すだけで working tree を動かさないが、**書き込み系**として扱う
// (index.lock と同じ理屈で git の直列化に乗せる)。checkout は working tree を
// 丸ごと入れ替えるので GitTransaction 経由でだけ投げる
constexpr const char* kBranches = "branches";
constexpr const char* kBranchCreate = "branch_create";
constexpr const char* kCheckout = "checkout";
// M66f。remote_state は読み取り系 (上の kReadOps に既に載っている)。fetch / push は
// working tree を触らないが**書き込み系**として扱う — ネットワーク待ちがあるので
// 30 s で打ち切られると「諦めた後に refs だけ書き換わる」食い違いが起きる。
// pull は working tree を丸ごと入れ替えるので GitTransaction 経由でだけ投げる
constexpr const char* kFetch = "fetch";
constexpr const char* kPull = "pull";
constexpr const char* kPush = "push";
constexpr const char* kRemoteState = "remote_state";
} // namespace collabop

// op の待ち方 (spec §4.4「タイムアウト」)。
//   Handshake … hello だけ。5 s (git.exe が 1 回起動できない環境は待っても無駄)
//   Read      … 30 s。落ちても状態は変わらないので打ち切ってよい
//   Write     … **無期限**。打ち切っても git は走り続けるので、「諦めた」と
//               表示した後で index だけ書き換わる = 最悪の食い違いになる
enum class CollabOpKind {
    Handshake,
    Read,
    Write,
};

constexpr int kCollabHelloTimeoutMs = 5000;
constexpr int kCollabReadTimeoutMs = 30000;

// op 名 → 待ち方。**未知の op は Write 扱い** (= 打ち切らない) に倒す。
// 分類を間違えたときの被害が「余計に待つ」で済む側を既定にしている。
// ★op 一覧は spec §4.1 で v1 に凍結済みなので、実装がまだ無い op もここには並ぶ
//   (M66c 以降が ops.rs を埋めるたびに、この表を触らなくて済むようにするため)
inline CollabOpKind CollabOpKindOf(std::string_view op)
{
    if (op == "hello") {
        return CollabOpKind::Handshake;
    }
    // 読み取り系 = リポジトリを 1 バイトも変えない op
    constexpr std::string_view kReadOps[] = {
        "repo_check", "status", "log",      "diff",         "diff_names",
        "branches",   "conflicts", "remote_state", "identity_check", "hint_changed",
    };
    for (const std::string_view r : kReadOps) {
        if (op == r) {
            return CollabOpKind::Read;
        }
    }
    return CollabOpKind::Write;
}

// error.code のうち C++ が分岐に使うもの (綴りは tools\collab\src\protocol.rs の
// code モジュールが正本。文字列の一致でしか照合できないので直書きを 1 箇所に集める)
namespace collaberr {
constexpr const char* kNotRepo = "not_repo";
constexpr const char* kGitMissing = "git_missing";
constexpr const char* kGitTooOld = "git_too_old";
constexpr const char* kServiceDead = "service_dead";
constexpr const char* kInternalPanic = "internal_panic";
constexpr const char* kBadRequest = "bad_request";
// M66e: checkout がローカルの未コミット変更と重なった。error.paths に対象が載る
constexpr const char* kLocalChangesOverwritten = "local_changes_overwritten";
// M66f: pull / push がリモートの先行に負けた。UI は「マージして pull」「先に pull」を出す
constexpr const char* kNonFastForward = "non_fast_forward";
// M66f: 認証に失敗した。Credential Manager のダイアログを閉じた場合もここへ来る
constexpr const char* kAuthFailed = "auth_failed";
// C++ 側が合成する疑似 code (サービスは返さない)。応答が期限内に来なかった要求は
// **必ずコールバックを呼んで消す** — 呼ばないと窓が「実行中」のまま二度と操作できない
constexpr const char* kTimeout = "timeout";
} // namespace collaberr

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
