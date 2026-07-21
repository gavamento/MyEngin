#pragma once
#include <DirectXMath.h>

#include "Editor/Selection.h"
#include "Engine/Core/EntityID.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

struct FieldDesc;
class UndoStack;

// リフレクション駆動 Inspector (engine_spec.md 9 章)。
// ComponentRegistry のフィールド表から widget を自動生成する —
// コンポーネント個別の UI コードは存在しない (これが M1 リフレクション設計の回収点)
class InspectorWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo);

private:
    bool DrawField(EngineContext& ctx, void* comp, const FieldDesc& field, EntityID entity,
                   Selection& selection, UndoStack& undo, uint64_t fid);
    // 参照ピッカー (ポップアップで選択。変更時は自前で Undo エントリを記録する)
    void DrawAssetRef(EngineContext& ctx, const FieldDesc& field, void* p, Selection& selection,
                      UndoStack& undo, uint64_t fid);
    void DrawEntityRef(EngineContext& ctx, const FieldDesc& field, void* p, Selection& selection,
                       UndoStack& undo, uint64_t fid);

    // 回転編集中のオイラー角キャッシュ (quat→euler→quat の往復ドリフト防止)
    DirectX::XMFLOAT3 eulerCache_ = { 0, 0, 0 };
    EntityID eulerCacheEntity_ = kNullEntity;
    const void* eulerCacheField_ = nullptr;
    bool eulerEditing_ = false;
};

} // namespace mye
