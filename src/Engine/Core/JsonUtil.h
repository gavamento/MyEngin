#pragma once
#include "Engine/Core/Reflection.h"

#include "nlohmann/json.hpp"

namespace mye {

// リフレクションフィールド 1 個の JSON 変換 (シーンシリアライズ / M3 差分適用で共用)。
// EntityRef はここでは扱わない — シリアライザが fileId で再マップする

// comp = コンポーネント先頭ポインタ
nlohmann::json FieldToJson(const void* comp, const FieldDesc& field);

// 型不一致や欠落は false (呼び出し側でデフォルト値のまま続行)
bool FieldFromJson(void* comp, const FieldDesc& field, const nlohmann::json& value);

} // namespace mye
