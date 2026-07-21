#include "Editor/Undo/UndoStack.h"

#include <algorithm>

#include "Editor/Selection.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"

namespace mye {

using nlohmann::json;

std::vector<uint64_t> UndoStack::FileIdsOf(const json& arr)
{
    std::vector<uint64_t> out;
    if (!arr.is_array()) {
        return out;
    }
    for (const json& item : arr) {
        const uint64_t fid = item.value("fileId", 0ull);
        if (fid != 0 && std::find(out.begin(), out.end(), fid) == out.end()) {
            out.push_back(fid);
        }
    }
    return out;
}

void UndoStack::BeginRecord(const char* label, const Selection& selBefore)
{
    pending_ = Entry{};
    pending_.label = label ? label : "Edit";
    pending_.selBefore = selBefore.ids;
    pending_.primaryBefore = selBefore.primary;
    pending_.session = currentSession_;
    recording_ = true;
}

void UndoStack::CaptureBefore(Scene& scene, uint64_t fileId)
{
    if (!recording_) {
        return;
    }
    GameObject g = scene.FindByFileId(fileId);
    if (!g) {
        return;
    }
    json sub = SceneSerializer::SubtreeToJson(scene, g.Id());
    for (json& item : sub) {
        pending_.before.push_back(std::move(item));
    }
}

void UndoStack::CaptureAfter(Scene& scene, uint64_t fileId)
{
    if (!recording_) {
        return;
    }
    GameObject g = scene.FindByFileId(fileId);
    if (!g) {
        return;
    }
    json sub = SceneSerializer::SubtreeToJson(scene, g.Id());
    for (json& item : sub) {
        pending_.after.push_back(std::move(item));
    }
}

void UndoStack::EndRecord(const Selection& selAfter)
{
    if (!recording_) {
        return;
    }
    recording_ = false;

    pending_.selAfter = selAfter.ids;
    pending_.primaryAfter = selAfter.primary;

    const std::vector<uint64_t> beforeIds = FileIdsOf(pending_.before);
    const std::vector<uint64_t> afterIds = FileIdsOf(pending_.after);
    for (uint64_t fid : afterIds) {
        if (std::find(beforeIds.begin(), beforeIds.end(), fid) == beforeIds.end()) {
            pending_.createdIds.push_back(fid);
        }
    }
    for (uint64_t fid : beforeIds) {
        if (std::find(afterIds.begin(), afterIds.end(), fid) == afterIds.end()) {
            pending_.destroyedIds.push_back(fid);
        }
    }

    // 実変化が無ければ積まない (フィールドを触っただけで値が変わっていない等)
    if (pending_.createdIds.empty() && pending_.destroyedIds.empty()
        && pending_.before == pending_.after) {
        return;
    }

    pending_.serial = ++serialCounter_; // 新しい状態 ID (StateSerial / ダーティ判定用)
    undo_.push_back(std::move(pending_));
    redo_.clear();
}

void UndoStack::CancelRecord()
{
    recording_ = false;
    pending_ = Entry{};
}

void UndoStack::ApplyStep(Scene& scene, Selection& sel, const json& payload,
                          const std::vector<uint64_t>& destroyFirst,
                          const std::vector<uint64_t>& selIds, uint64_t primary)
{
    World& world = scene.GetWorld();
    for (uint64_t fid : destroyFirst) {
        GameObject g = scene.FindByFileId(fid);
        if (g) {
            world.DestroyEntity(g.Id());
        }
    }
    world.ApplyStructuralChanges(); // 破棄を確定してから再適用する
    SceneSerializer::ApplyPartial(scene, payload);
    sel.Set(selIds, primary);
}

void UndoStack::Undo(Scene& scene, Selection& sel)
{
    if (undo_.empty()) {
        return;
    }
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    // op が生成したものを破棄してから before を適用 (= op 前の状態へ)
    ApplyStep(scene, sel, e.before, e.createdIds, e.selBefore, e.primaryBefore);
    redo_.push_back(std::move(e));
}

void UndoStack::Redo(Scene& scene, Selection& sel)
{
    if (redo_.empty()) {
        return;
    }
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    // op が破棄したものを破棄してから after を適用 (= op 後の状態へ)
    ApplyStep(scene, sel, e.after, e.destroyedIds, e.selAfter, e.primaryAfter);
    undo_.push_back(std::move(e));
}

void UndoStack::EndPlaySession()
{
    const uint32_t s = currentSession_;
    auto pred = [s](const Entry& e) { return e.session == s; };
    undo_.erase(std::remove_if(undo_.begin(), undo_.end(), pred), undo_.end());
    redo_.erase(std::remove_if(redo_.begin(), redo_.end(), pred), redo_.end());
    currentSession_ = 0;
}

void UndoStack::ClearAll()
{
    undo_.clear();
    redo_.clear();
    recording_ = false;
    pending_ = Entry{};
    baseSerial_ = ++serialCounter_; // 新しいシーンの基底状態 (過去の serial と重ならない)
}

} // namespace mye
