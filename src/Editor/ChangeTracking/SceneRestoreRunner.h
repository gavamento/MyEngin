//====================================================================================
//                          SceneRestoreRunner.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ライブのシーンの復元の実行 (Undo に積んで World を書き、読み直して確かめる。宣言)
//====================================================================================
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/RestoreGate.h"
#include "Editor/ChangeTracking/SceneRestorePlan.h"

namespace mye {

class Scene;
class ManagedHost;
class UndoStack;
struct Selection;

// ライブのシーンの復元が使うもの。EditorApp が毎フレーム作って渡す (ミニシーンの編集中は scene が差し替わっている)
struct SceneRestoreEnv {
    Scene* scene = nullptr;
    UndoStack* undo = nullptr;
    Selection* selection = nullptr;
    ManagedHost* managed = nullptr; // C# のホスト。無ければ C# のコンポーネントは判定も書き込みもしない
};

// ライブの Scene を SceneReadView として読む。World を書かない (fileId の無いエンティティには採番しない)
class LiveSceneReadView final : public SceneReadView {
public:
    explicit LiveSceneReadView(const SceneRestoreEnv& env) : env_(env) {}
    bool Entity(uint64_t fileId, nlohmann::json& out) const override;
    std::vector<uint64_t> Children(uint64_t fileId) const override;
    bool IsManagedComponent(const std::string& componentName) const override;

private:
    SceneRestoreEnv env_;
};

enum class SceneRestoreOutcome : uint8_t {
    Committed,    // 書いて、読み直して確かめた。Undo に 1 エントリ積んだ
    AlreadyDone,  // 全部が既に Before (何も書いていない)
    Blocked,      // 書く前に止めた (ゲート)
    NotReady,     // 計画が Ready でない (競合・戻せない)。何も書いていない
    PlanChanged,  // プレビューのときと計画が違う。何も書いていない
    VerifyFailed, // 書いたあとの読み直しが合わず、自分で書いた分を戻した (Undo には何も積んでいない)
    NoScene,      // 今のシーンを使えない
};

struct SceneRestoreRequest {
    SceneRestoreEnv env;
    int64_t revisionId = 0;
    std::string actor;
    std::string openDocumentId;
    std::string scenePath;                    // 履歴に載せる、シーンのパス (プロジェクトからの相対)
    std::vector<TrackSceneChange> rows;       // 戻す行
    std::vector<SceneRestoreOp> previewOps;   // プレビューの計画の ops。判定し直した計画がこれと同じでなければ書かない
    // 今のゲートの判定 (止める理由の全部。空 = 通る)。実行の直前に呼ぶ
    std::function<std::vector<RestoreBlock>()> blocks;
    std::string undoLabel;
};

struct SceneRestoreResult {
    SceneRestoreOutcome outcome = SceneRestoreOutcome::NoScene;
    std::string restoreId;
    std::vector<RestoreBlock> blocks;
    SceneRestorePlan plan;       // 判定し直した計画
    std::string detail;          // VerifyFailed のとき、合わなかった行
    int appliedOps = 0;
};

// 実行。**必ず UndoStack::Record の中で書く** (Ctrl+Z で戻せる。プレハブの上書きの記録を取りこぼさない)。ディスクは書かない。
// 編集モードのセーフポイント (ImGui の外ではなく、毎フレームの Editor の更新) で呼ぶこと
SceneRestoreResult ApplySceneRestore(const SceneRestoreRequest& request);

// restore_record の引数 (state = committed / rolled_back)。ids は戻した行
nlohmann::json BuildSceneRestoreRecord(const std::string& restoreId, const std::string& state, int64_t revisionId, const std::vector<int64_t>& sceneChangeIds,
                                       const std::string& scenePath, const std::string& error);

// 戻した行 (Ready か AlreadyRestored の行。連動して変わる上書きの印を含む)
std::vector<int64_t> RestoredSceneChangeIds(const SceneRestorePlan& plan);

} // namespace mye
