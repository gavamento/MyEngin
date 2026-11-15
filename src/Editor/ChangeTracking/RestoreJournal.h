//====================================================================================
//                          RestoreJournal.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元ジャーナル (適用前の内容と進み具合を、復元のたびにディスクへ残す。宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/RestorePlan.h"
#include "Editor/ChangeTracking/RestoreWriteIo.h"

namespace mye {

// 正本: docs\ai-change-tracking\restore-safety.md §5、§6。<project>\.mye\ai_changes\recovery\<restoreId>\ を Editor が所有する
// (サービスは書かない)。追跡サービスが居なくても、DB が壊れていても、ジャーナルだけで巻き戻せる

enum class JournalState : uint8_t {
    Prepared,         // 適用前の内容を保存した。まだ何も書いていない
    Applying,         // 書き込みの途中
    Committed,        // 全部書いて確かめた
    RollingBack,      // 失敗して、保存した内容へ戻している途中
    RolledBack,       // 元の内容へ戻し終えた
    RecoveryRequired, // 自動では整合した状態へ戻せなかった
    Recovered,        // 回復の画面で利用者が選んで済ませた (resolution に選んだ内容)
};

const char* JournalStateName(JournalState state);
bool ParseJournalState(const std::string& text, JournalState& out);
// 起動時に回復の画面を出す状態
bool NeedsRecovery(JournalState state);

struct JournalOp {
    std::string kind;   // overwrite / delete / create / rename
    std::string path;
    std::string toPath;
    std::string source; // none / before_blob / bytes
    std::string hash;
    int64_t size = -1;
};

// 復元が触るパス 1 つ。states は「軌跡」: states[0] = 適用前、states[i] = i 個目の操作まで済んだあとの内容のハッシュ ("" = ファイルが無い)。
// 巻き戻しは、今の内容が軌跡のどれかに合うときだけ触る (自分が書いたものだけを戻す)
struct JournalFile {
    std::string path;
    bool existed = false;
    std::string preHash;
    int64_t preSize = -1;
    std::string preFile; // existed のとき、ジャーナルの中の相対 ("pre\0001.bin")
    std::vector<std::string> states;

    const std::string& IntendedHash() const { return states.back(); }
};

struct JournalManifest {
    int version = 1;
    std::string restoreId;
    JournalState state = JournalState::Prepared;
    int64_t revisionId = 0;
    std::string scope; // file / hunks
    std::vector<int64_t> hunkIds;
    int64_t startedAt = 0; // 実時間 (ms、UNIX epoch)
    int64_t updatedAt = 0;
    std::vector<JournalOp> ops;
    std::vector<JournalFile> files;
    int completedOps = 0; // 済んだ操作の数 (書いたあとに更新する)
    std::string error;
    std::string resolution; // Recovered のとき: restored_pre / kept_current / dismissed
};

nlohmann::json ManifestToJson(const JournalManifest& manifest);
// 形が違う・必須の項目が無いときは false
bool ManifestFromJson(const nlohmann::json& json, JournalManifest& out);

// 計画の操作列から、パスごとの軌跡を作る (純関数)。initial = (パス, 適用前のハッシュ。"" = 無い) の並び。操作が触るパスは全部 initial に要る
std::vector<JournalFile> BuildJournalFiles(const std::vector<RestoreOp>& ops, const std::vector<std::pair<std::string, std::string>>& initial);
std::vector<JournalOp> ToJournalOps(const std::vector<RestoreOp>& ops);
// 計画の操作が触るパス (重複なし、操作の出現順。名前の変更は両方)
std::vector<std::string> TouchedPaths(const std::vector<RestoreOp>& ops);

// 今の内容のハッシュ (""=無い) が、軌跡のどこに当たるか。-1 = どれにも合わない (他者が書いた)
int FindInTrajectory(const JournalFile& file, const std::string& currentHash);

// 新しい restoreId (UUID v4 の文字列)
std::string NewRestoreId();
int64_t WallClockMs();

// 保存期間 (committed / rolled_back / recovered を残す日数)
constexpr int64_t kRestoreJournalKeepMs = 7ll * 24 * 60 * 60 * 1000;

class RestoreJournalStore {
public:
    struct Entry {
        std::string id;
        bool broken = false; // manifest.json が読めない・壊れている
        std::string brokenReason;
        JournalManifest manifest;
    };

    RestoreJournalStore(std::wstring projectRoot, RestoreWriteIo& io) : root_(std::move(projectRoot)), io_(&io) {}

    std::wstring RecoveryRoot() const;
    std::wstring DirOf(const std::string& id) const;
    // ジャーナルの中のファイルの絶対パス (rel は "pre\0001.bin" のような相対)
    std::wstring FileIn(const std::string& id, const std::string& rel) const;

    // ディレクトリと manifest.json を作る (pre\ も作る)
    bool Begin(const JournalManifest& manifest);
    // manifest.json を WriteFileReplacing で更新する (半分書かれた状態を作らない)
    bool Save(const JournalManifest& manifest);
    bool Discard(const std::string& id);
    // manifest が壊れたジャーナルを「回復待ちから外す」印 (中身は消さない)
    bool MarkDismissed(const std::string& id);

    // 読み取りだけ。印のあるものは含めない。起動時の検出と画面の更新に使う (id の昇順)
    std::vector<Entry> Scan() const;
    // 古いジャーナルの掃除 (Prepared は残っていれば捨てる。Committed / RolledBack / Recovered は保存期間を過ぎたもの)。消した数
    int Prune(int64_t nowMs, int64_t keepMs);

private:
    std::wstring root_;
    RestoreWriteIo* io_;
};

} // namespace mye
