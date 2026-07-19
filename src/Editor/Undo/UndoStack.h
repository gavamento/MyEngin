#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mye {

class Scene;
struct Selection;

// エディタの Undo/Redo スタック (M8)。
//
// 方針:
//   - エンティティは **fileId** で識別する。Play/Stop や Undo 復元で EntityID が
//     作り直されても追跡できる (SceneSerializer::ApplyPartial が fileId 照合で再適用)。
//   - 1 エントリは「before / after の状態ペア」+「生成/破棄された fileId 集合」。
//     modify (フィールド変更・コンポーネント追加削除・親子付け・並べ替え) は
//     before/after 両方に同じ fileId が入り、create は after のみ、destroy は before のみ。
//
// 記録トランザクション:
//   undo.BeginRecord("Move", selBefore);
//   undo.CaptureBefore(scene, fid);   // op 前 (modify/destroy 対象。root ならサブツリー全部)
//   ... エディタが編集 (create/destroy/フィールド変更) ...
//   undo.CaptureAfter(scene, fid);    // op 後 (modify/create 対象)
//   undo.EndRecord(selAfter);
//
// Begin と End は複数フレームに跨いでよい (ドラッグ操作を 1 エントリにまとめる = transient マージ)。
class UndoStack {
public:
    // ---- 記録 ----
    void BeginRecord(const char* label, const Selection& selBefore);
    void CaptureBefore(Scene& scene, uint64_t fileId); // op 前の状態 (サブツリー丸ごと)
    void CaptureAfter(Scene& scene, uint64_t fileId);  // op 後の状態 (サブツリー丸ごと)
    void EndRecord(const Selection& selAfter);          // 変化があればスタックへ積む
    void CancelRecord();
    bool IsRecording() const { return recording_; }

    // ---- 実行 ----
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    void Undo(Scene& scene, Selection& sel);
    void Redo(Scene& scene, Selection& sel);
    const char* NextUndoLabel() const { return undo_.empty() ? "" : undo_.back().label.c_str(); }
    const char* NextRedoLabel() const { return redo_.empty() ? "" : redo_.back().label.c_str(); }

    // ---- Play セッション (Play 中の編集は Stop で破棄する) ----
    void BeginPlaySession() { currentSession_ = ++sessionCounter_; }
    void EndPlaySession();

    // ---- シーンロード / New Scene で全消去 ----
    void ClearAll();

private:
    struct Entry {
        std::string label;
        nlohmann::json before = nlohmann::json::array(); // ApplyPartial ペイロード (復元)
        nlohmann::json after = nlohmann::json::array();  // ApplyPartial ペイロード (再適用)
        std::vector<uint64_t> createdIds;   // after にあり before に無い → undo で破棄
        std::vector<uint64_t> destroyedIds; // before にあり after に無い → redo で破棄
        std::vector<uint64_t> selBefore, selAfter;
        uint64_t primaryBefore = 0, primaryAfter = 0;
        uint32_t session = 0;
    };

    static void ApplyStep(Scene& scene, Selection& sel, const nlohmann::json& payload,
                          const std::vector<uint64_t>& destroyFirst,
                          const std::vector<uint64_t>& selIds, uint64_t primary);
    static std::vector<uint64_t> FileIdsOf(const nlohmann::json& arr);

    std::vector<Entry> undo_;
    std::vector<Entry> redo_;

    bool recording_ = false;
    Entry pending_;

    uint32_t sessionCounter_ = 0;
    uint32_t currentSession_ = 0;
};

} // namespace mye
