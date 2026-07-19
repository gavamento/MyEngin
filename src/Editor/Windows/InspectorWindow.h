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
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo);

private:
    bool DrawField(void* comp, const FieldDesc& field, EntityID entity);

    // 回転編集中のオイラー角キャッシュ (quat→euler→quat の往復ドリフト防止)
    DirectX::XMFLOAT3 eulerCache_ = { 0, 0, 0 };
    EntityID eulerCacheEntity_ = kNullEntity;
    const void* eulerCacheField_ = nullptr;
    bool eulerEditing_ = false;
};

} // namespace mye
