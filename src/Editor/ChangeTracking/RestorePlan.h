//====================================================================================
//                          RestorePlan.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ファイル・Hunk の復元の計画 (競合の検証。書かない純関数、宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/HunkRestore.h"
#include "Editor/ChangeTracking/PathGuard.h"
#include "Editor/SourceControl/StageClassifier.h"
#include "Editor/Widgets/DiffView.h"

namespace mye {

// このファイルは「何を書くことになるか」を決めるだけで、作業ツリーを書かない (書くのは M104p2)。
// 正本: docs\ai-change-tracking\restore-safety.md §3 (競合の検証)

// 対象の単位 (ファイルの側だけ。シーンの Entity / Component / Field は M104q)
enum class RestoreScope : uint8_t { File, Hunks };

// 記録した 1 つの面 (Before / After) の内容の状態。describe の before / after から
enum class RestoreBlob : uint8_t {
    NoSnapshot, // その面が無い (追加の Before、削除の After)
    Present,    // Blob がある
    Missing,    // Blob が欠けている
    Excluded,   // 除外パスで内容を持たない
    Evicted,    // 保存期間を過ぎて回収された
    Unknown,    // サービスの新しい値 (安全側に倒す)
};

struct RestoreSide {
    RestoreBlob state = RestoreBlob::NoSnapshot;
    std::string hash; // 小文字の 16 進。無ければ空
    int64_t size = -1;
};

// 復元の対象にする Revision (describe の結果から)
struct RestoreRevision {
    int64_t revisionId = 0;
    std::string path;
    std::string oldPath; // 名前の変更のときの旧パス
    std::string changeType; // added / modified / deleted / renamed
    std::string actor;
    std::string changeState; // active / superseded / ...
    std::string diffMode;    // lines / whole / binary / excluded / unavailable
    std::string completeness;
    RestoreSide before;
    RestoreSide after;
};

RestoreRevision ParseRestoreRevision(const nlohmann::json& detail);

// 今のパスの状態 (呼び手が読んで渡す)
enum class NowKind : uint8_t {
    Missing,    // 何も無い
    File,       // 普通のファイル
    Directory,
    Link,       // シンボリックリンク・ジャンクション
    Unreadable, // 読めなかった
    TooLarge,   // 読み取りの上限 (kRestoreReadMaxBytes) を超える
};

struct NowState {
    NowKind kind = NowKind::Missing;
    std::string bytes; // File のとき (Hunk の照合に使う)
    std::string hash;  // File のとき bytes の SHA-256
    int64_t size = -1;
};

// 今のファイルの読み取りの上限 (行 diff の上限と同じ 8 MiB)。超えるファイルは判定しない
constexpr size_t kRestoreReadMaxBytes = 8u * 1024u * 1024u;

// 復元できない・競合の理由 (None 以外のとき、計画は書く操作を持たない)
enum class RestoreIssue : uint8_t {
    None,
    // 対象にできない (その Revision / 範囲は戻せない)
    PathRejected,        // 書き先の検査 (PathGuard) で拒否
    ActorNotRestorable,  // 行為者が ai / unknown でない (Editor の利用者自身の編集・復元)
    NoBefore,            // 変更前の内容を見ていない (no_before)
    BeforeUnavailable,   // Before の Blob が欠けている・除外・回収された
    AfterUnknown,        // After の内容が分からない (除外など)
    HunkNeedsModified,   // Hunk 単位で戻せるのは内容の変更だけ (追加・削除・名前の変更は全体)
    HunkNeedsLines,      // 行の Hunk が無い (whole / binary / excluded / unavailable)
    HunkOversize,        // 本文が大きすぎて届かない Hunk
    HunkNoneSelected,    // 戻す Hunk が選ばれていない
    NowUnreadable,       // 今のファイルを読めない
    NowTooLarge,         // 今のファイルが読み取りの上限を超える
    NowNotAFile,         // 今のパスがディレクトリ・リンク
    // 競合 (今の内容が記録と合わない)
    FileChanged,         // 今の内容が After とも Before とも違う
    PathOccupied,        // 削除の取り消しだが、今そのパスに何かある
    OldPathOccupied,     // 名前の変更の取り消しだが、古い名前に何かある
    HunkAmbiguous,       // Hunk の並びが 2 か所以上にある
    HunkNotFound,        // Hunk の並びが無い
    HunkOverlap,         // 戻す Hunk の範囲が重なる
};

// 判定。Ready だけが「書くことになる操作」を持つ
enum class RestoreVerdict : uint8_t {
    Ready,           // 照合に通った。書ける
    AlreadyRestored, // 今の内容が既に Before (何も書かない)
    Conflict,        // 今の内容が記録と合わない (自動では戻さない)
    NotRestorable,   // 戻せない (理由は issue)
};

struct RestoreOp {
    enum class Kind : uint8_t { Overwrite, Delete, Create, Rename };
    // 書く内容の出どころ
    enum class Source : uint8_t {
        None,       // Delete / Rename だけ
        BeforeBlob, // 記録した Before の Blob (hash の内容。書く前に blob_get で取る)
        Bytes,      // Hunk を逆適用して組んだ内容 (bytes)
    };
    Kind kind = Kind::Overwrite;
    std::string path;   // 対象。Rename は今のパス
    std::string toPath; // Rename の行き先
    Source source = Source::None;
    std::string hash;   // 書く内容の SHA-256
    int64_t size = -1;
    std::string bytes;  // Source::Bytes のとき
};

struct RestoreHunkOutcome {
    int64_t hunkId = 0;
    int64_t ordinal = 0;
    HunkFind find = HunkFind::NotFound;
    int64_t shiftLines = 0;
    int64_t bodyLine = 0;
};

struct RestorePlan {
    RestoreScope scope = RestoreScope::File;
    RestoreVerdict verdict = RestoreVerdict::NotRestorable;
    RestoreIssue issue = RestoreIssue::None;
    PathIssue pathIssue = PathIssue::None;
    std::vector<RestoreOp> ops; // Ready のときだけ
    std::vector<RestoreHunkOutcome> hunks; // Hunk 単位のとき、選んだ Hunk ごとの照合の結果
};

struct RestoreInput {
    RestoreScope scope = RestoreScope::File;
    RestoreRevision revision;
    std::vector<TrackHunk> hunks; // Hunk 単位のとき、戻す Hunk
    NowState now;                 // revision.path の今
    NowState nowOld;              // 名前の変更のときの旧パスの今
    PathIssue pathIssue = PathIssue::None; // 書き先の検査の結果 (None 以外なら計画を作らない)
};

// 計画を作る。**純関数**: 入力だけで決まり、ファイルにも DB にも触らない。競合・復元済み・戻せないとき ops は空
RestorePlan BuildRestorePlan(const RestoreInput& input);

// 計画が触るパス (ゲートの対象)。名前の変更は両方
std::vector<std::string> RestoreTargetPaths(const RestoreRevision& revision);

// 行為者が復元の対象か (ai / unknown)
bool IsRestorableActor(const std::string& actor);

// ---- 見せるための差分 (DiffViewer の入力) ----

// 選んだ Hunk を逆向き (After → Before) にしたもの。「この復元で、この Hunk がこう変わる」を見せる
std::vector<DiffHunkInput> ReverseHunkView(const std::vector<TrackHunk>& hunks);
// 照合に失敗した Hunk について、今の内容 (記録した位置の周り) と記録した After の差。全面的な 3 者の比較ではなく、
// 失敗した位置を示すためのもの。失敗した Hunk が無ければ空
std::vector<DiffHunkInput> ConflictHunkView(const std::string& currentBytes, const std::vector<TrackHunk>& hunks, const std::vector<RestoreHunkOutcome>& outcomes);

// ---- 注記 (restore-safety.md §3.4、§7) ----

// stage 済みか。collab が使えないとき Unknown (Index を読みも書きもしない。呼び手が既存の status モデルから答える)
enum class StagedState : uint8_t { Unknown, Staged, NotStaged };

struct RestoreNotes {
    bool deletesFile = false;           // 削除になる操作がある
    bool referencesUnchecked = false;   // 削除の取り消しのとき、参照しているコードを確かめていない
    ApplyStage stage = ApplyStage::A;   // 戻したあとの作り直しの見込み (A = その場で差し替え / B = 読み直し・再コンパイル / C = 再起動)
    bool metaGuidUnchecked = false;     // .meta を書く (guid が変わるかは確かめていない)
    std::vector<std::string> stagedPaths; // stage 済みと分かった対象
    bool stagedUnknown = false;         // stage 済みか分からない対象がある
    bool shifted = false;               // 位置がずれて見つかった Hunk がある
};

// 計画 (Ready) の注記。activeScene = 今開いているシーンのパス (相対)。stagedOf は既存の status の読み取りだけを使うこと (git を起動しない)
RestoreNotes BuildRestoreNotes(const RestorePlan& plan, const std::string& activeScene, const std::function<StagedState(const std::string&)>& stagedOf);

} // namespace mye
