#pragma once
#include <vector>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;

// AddComponent ポップアップ / Inspector ヘッダ / Hierarchy 行で使うコンポーネントの見た目メタ。
// ComponentRegistry には手を入れず、エディタ側の name→{icon,category} 表で完結させる (M33b)
struct ComponentUiInfo {
    const char* icon;     // ICON_FA_* (UTF-8 リテラル)
    const char* category; // AddComponent のカテゴリ見出し (**キー**。表示は下の Label で引く)
    const char* ja;       // 日本語表示名 (M47c)。nullptr なら ComponentDesc.name をそのまま出す
};

// ComponentDesc.name で引く。未登録 (C++/C# スクリプトコンポーネント) は {code, "Scripts", nullptr}
const ComponentUiInfo& ComponentUiFor(const char* name);

// 表示用のコンポーネント名 (M47c)。英語モードと未登録は name をそのまま返す
const char* ComponentDisplayName(const char* name);

// AddComponent ポップアップのカテゴリ**キー**の表示順 (英語固定。照合にも使う)
const std::vector<const char*>& ComponentUiCategories();

// カテゴリキー -> 表示名 (M47c)。キーは英語のまま、見出しだけ訳す
const char* ComponentCategoryLabel(const char* categoryKey);

// Hierarchy 行用: エンティティを代表するコンポーネントのアイコン。
// General 系 (Name/LocalTransform/Active) は代表にせず、該当なしは空グループ用の丸を返す
const char* EntityIconFor(World& world, EntityID e);

} // namespace mye
