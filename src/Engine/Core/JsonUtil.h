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

// フィールド値 JSON の同値比較。Bool は旧形式 (0/1 数値) と真偽値を同一視する。
// プレハブ override 判定 (Prefab.cpp) のような「保存値 vs 現在値」比較は必ずこれを通す
bool FieldJsonEquals(const FieldDesc& field, const nlohmann::json& a, const nlohmann::json& b);

} // namespace mye
