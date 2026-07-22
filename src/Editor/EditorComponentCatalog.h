#pragma once
#include <vector>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;

// AddComponent ポップアップ / Inspector ヘッダ / Hierarchy 行で使うコンポーネントの見た目メタ。
// ComponentRegistry には手を入れず、エディタ側の name→{icon,category} 表で完結させる (M33b)
struct ComponentUiInfo {
    const char* icon;     // ICON_FA_* (UTF-8 リテラル)
    const char* category; // AddComponent のカテゴリ見出し
};

// ComponentDesc.name で引く。未登録 (C++/C# スクリプトコンポーネント) は {code, "Scripts"}
const ComponentUiInfo& ComponentUiFor(const char* name);

// AddComponent ポップアップのカテゴリ表示順
const std::vector<const char*>& ComponentUiCategories();

// Hierarchy 行用: エンティティを代表するコンポーネントのアイコン。
// General 系 (Name/LocalTransform/Active) は代表にせず、該当なしは空グループ用の丸を返す
const char* EntityIconFor(World& world, EntityID e);

} // namespace mye
