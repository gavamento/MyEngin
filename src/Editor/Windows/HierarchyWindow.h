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
    // 兄弟並べ替え (M27d): src を target の前/後に挿入する (必要なら再ペアレントも)。1 Undo
    void ReorderAsSibling(EngineContext& ctx, World& world, EntityID src, EntityID target,
                          bool after, Selection& selection, UndoStack& undo);

    char searchBuf_[64] = {};
    uint64_t renamingFid_ = 0; // インラインリネーム中の fileId (0 = なし)
    bool renameFocus_ = false;
    std::string renameOriginal_; // 編集開始時の名前 (確定時に同一なら改名しない、M48b)

    // Shift 範囲選択 (M27d): 前フレームの表示順 + 範囲の起点
    std::vector<uint64_t> visibleOrder_;
    std::vector<uint64_t> visibleOrderPrev_;
    uint64_t anchorFid_ = 0;

    // Create Prefab 命名モーダル (M50b)。要求フラグ → 次フレームで OpenPopup
    // (コンテキストメニューの中からは開けないため。AssetBrowser の Create と同じ)
    uint64_t prefabModalFid_ = 0; // 対象エンティティの fileId (0 = なし)
    bool prefabModalRequest_ = false;
    char prefabNameBuf_[128] = {};
    bool prefabAsActor_ = true; // true=.actor.json (既定) / false=.prefab.json
    // Unpack Prefab 確認モーダル (M50b)
    uint64_t unpackModalFid_ = 0;
    bool unpackModalRequest_ = false;
};

} // namespace mye
