#pragma once
#include "Editor/Selection.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

class World;

// シーンツリー表示 / 選択 / 作成・削除 / ドラッグ&ドロップ再ペアレント (engine_spec.md 9 章)
class HierarchyWindow {
public:
    void OnImGui(EngineContext& ctx, Selection& selection);

private:
    void DrawEntityNode(EngineContext& ctx, World& world, EntityID e, Selection& selection);
};

} // namespace mye
