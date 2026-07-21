#pragma once
#include "Editor/Selection.h"
#include "Engine/Core/EntityID.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

class World;
class UndoStack;

// シーンツリー表示 / 選択 / 作成・削除 / ドラッグ&ドロップ再ペアレント (engine_spec.md 9 章)
class HierarchyWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo);

private:
    void DrawEntityNode(EngineContext& ctx, World& world, EntityID e, Selection& selection,
                        UndoStack& undo);
    void DrawFiltered(EngineContext& ctx, World& world, Selection& selection);
    void ApplyClick(EngineContext& ctx, EntityID e, Selection& selection); // 修飾キーで選択更新

    char searchBuf_[64] = {};
    uint64_t renamingFid_ = 0; // インラインリネーム中の fileId (0 = なし)
    bool renameFocus_ = false;
};

} // namespace mye
