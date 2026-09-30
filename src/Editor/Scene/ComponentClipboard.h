#pragma once
#include <string>

#include "nlohmann/json.hpp"

namespace mye {

struct ComponentDesc;

// コンポーネント copy/paste の静的クリップボード (M40a、エディタ全体で 1 個)。
// fields は FieldToJson 形式の {フィールド名: 値} オブジェクト。
// EntityRef は生値のためペースト時にスキップされる (FieldFromJson が拒否 = 対象は不変)
struct ComponentClipboard {
    std::string componentName; // 空 = 未コピー
    nlohmann::json fields;

    bool Empty() const { return componentName.empty(); }
};

ComponentClipboard& GetComponentClipboard();

// コンポーネントの全リフレクションフィールド → JSON オブジェクト (copy 用)
nlohmann::json ComponentFieldsToJson(const ComponentDesc& desc, const void* comp);

// JSON オブジェクト → コンポーネントフィールド (paste 用)。
// 欠落キー/型不一致/EntityRef は黙ってスキップ (対象の現在値を維持)
void ComponentFieldsFromJson(const ComponentDesc& desc, void* comp, const nlohmann::json& j);

} // namespace mye
