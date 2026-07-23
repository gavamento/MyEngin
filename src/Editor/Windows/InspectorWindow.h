#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

#include "Editor/Selection.h"
#include "Engine/Core/EntityID.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

struct FieldDesc;
class UndoStack;

// リフレクション駆動 Inspector (engine_spec.md 9 章)。
// ComponentRegistry のフィールド表から widget を自動生成する —
// コンポーネント個別の UI コードは存在しない (これが M1 リフレクション設計の回収点)
// M40a マルチ選択: 全選択が共通に持つコンポーネントを表示 (値は primary のもの)、
// 編集/削除/追加/paste/reset は全選択へバッチ適用 (1 Undo エントリ)。ギズモは primary のみ
class InspectorWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo);

private:
    // fids/comps は同コンポーネントを持つ選択エンティティ列 (要素 [0] = primary、comp と同一)。
    // 単一選択では要素 1 個。ポップアップ系 (mask/参照ピッカー) はこの列へバッチ書込する
    bool DrawField(EngineContext& ctx, const char* componentName, void* comp,
                   const FieldDesc& field, EntityID entity, Selection& selection, UndoStack& undo,
                   const std::vector<uint64_t>& fids, const std::vector<void*>& comps);
    // 参照ピッカー (ポップアップで選択。変更時は自前で Undo エントリを記録する)
    void DrawAssetRef(EngineContext& ctx, const FieldDesc& field, void* p, Selection& selection,
                      UndoStack& undo, const std::vector<uint64_t>& fids,
                      const std::vector<void*>& comps, uint32_t fieldOffset);
    void DrawEntityRef(EngineContext& ctx, const FieldDesc& field, void* p, Selection& selection,
                       UndoStack& undo, const std::vector<uint64_t>& fids,
                       const std::vector<void*>& comps, uint32_t fieldOffset);

    // Add Component ポップアップの検索フィルタ (開くたびにクリア)
    char addComponentFilter_[64] = {};

    // 回転編集中のオイラー角キャッシュ (quat→euler→quat の往復ドリフト防止)
    DirectX::XMFLOAT3 eulerCache_ = { 0, 0, 0 };
    EntityID eulerCacheEntity_ = kNullEntity;
    const void* eulerCacheField_ = nullptr;
    bool eulerEditing_ = false;
};

} // namespace mye
