#pragma once
#include "Editor/Selection.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

// 全体検索 (engine_spec.md 9 章、M15)。
// エンティティ (名前 / コンポーネント型)、アセット (名前)、参照逆引き
// (選択エンティティを EntityRef で参照しているエンティティ — リフレクション走査)。
class SearchWindow {
public:
    void OnImGui(EngineContext& ctx, Selection& selection);

private:
    char query_[128] = {};
};

} // namespace mye
