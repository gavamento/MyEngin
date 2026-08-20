#include "Engine/Core/JsonUtil.h"

#include <cstring>

#include "Engine/Core/EntityID.h"

namespace mye {

using nlohmann::json;

namespace {

template <typename T>
json ArrayOf(const void* p, size_t count)
{
    const T* v = static_cast<const T*>(p);
    json arr = json::array();
    for (size_t i = 0; i < count; ++i) {
        arr.push_back(v[i]);
    }
    return arr;
}

template <typename T>
bool ReadArray(void* p, size_t count, const json& value)
{
    if (!value.is_array() || value.size() != count) {
        return false;
    }
    T* v = static_cast<T*>(p);
    for (size_t i = 0; i < count; ++i) {
        v[i] = value[i].get<T>();
    }
    return true;
}

} // namespace

json FieldToJson(const void* comp, const FieldDesc& field)
{
    const void* p = static_cast<const uint8_t*>(comp) + field.offset;
    switch (field.type) {
    case FieldType::Float:   return *static_cast<const float*>(p);
    case FieldType::Int32:   return *static_cast<const int32_t*>(p);
    case FieldType::UInt32:  return *static_cast<const uint32_t*>(p);
    case FieldType::UInt64:  return *static_cast<const uint64_t*>(p);
    case FieldType::Bool:    return *static_cast<const uint8_t*>(p) != 0;
    case FieldType::Float2:  return ArrayOf<float>(p, 2);
    case FieldType::Float3:  return ArrayOf<float>(p, 3);
    case FieldType::Float4:
    case FieldType::Quat:
    case FieldType::Color:   return ArrayOf<float>(p, 4);
    case FieldType::AssetRef: return static_cast<const AssetID*>(p)->value;
    case FieldType::String64: {
        const char* s = static_cast<const char*>(p);
        return std::string(s, strnlen(s, 64));
    }
    case FieldType::String256: {
        const char* s = static_cast<const char*>(p);
        return std::string(s, strnlen(s, 256));
    }
    case FieldType::EntityRef: // シリアライザ側で fileId 再マップ (ここでは生値)
        return json::array({ static_cast<const EntityID*>(p)->index,
                             static_cast<const EntityID*>(p)->generation });
    case FieldType::Float4x4: return ArrayOf<float>(p, 16);
    }
    return nullptr;
}

bool FieldFromJson(void* comp, const FieldDesc& field, const json& value)
{
    void* p = static_cast<uint8_t*>(comp) + field.offset;
    try {
        switch (field.type) {
        case FieldType::Float:  *static_cast<float*>(p) = value.get<float>(); return true;
        case FieldType::Int32:  *static_cast<int32_t*>(p) = value.get<int32_t>(); return true;
        case FieldType::UInt32: *static_cast<uint32_t*>(p) = value.get<uint32_t>(); return true;
        case FieldType::UInt64: *static_cast<uint64_t*>(p) = value.get<uint64_t>(); return true;
        case FieldType::Bool:
            // 旧データ互換: isTrigger の int32→bool 化 (M51 後続) 以前は 0/1 数値で保存
            if (value.is_number()) {
                *static_cast<uint8_t*>(p) = (value.get<double>() != 0.0) ? 1 : 0;
                return true;
            }
            *static_cast<uint8_t*>(p) = value.get<bool>() ? 1 : 0;
            return true;
        case FieldType::Float2: return ReadArray<float>(p, 2, value);
        case FieldType::Float3: return ReadArray<float>(p, 3, value);
        case FieldType::Float4:
        case FieldType::Quat:
        case FieldType::Color:  return ReadArray<float>(p, 4, value);
        case FieldType::AssetRef:
            static_cast<AssetID*>(p)->value = value.get<uint64_t>();
            return true;
        case FieldType::String64: {
            const std::string s = value.get<std::string>();
            char* dst = static_cast<char*>(p);
            const size_t n = (s.size() < 63) ? s.size() : 63;
            memcpy(dst, s.data(), n);
            dst[n] = '\0';
            return true;
        }
        case FieldType::String256: {
            const std::string s = value.get<std::string>();
            char* dst = static_cast<char*>(p);
            const size_t n = (s.size() < 255) ? s.size() : 255;
            memcpy(dst, s.data(), n);
            dst[n] = '\0';
            return true;
        }
        case FieldType::EntityRef:
            return false; // fileId 再マップが必要 — シリアライザの責務
        case FieldType::Float4x4:
            return ReadArray<float>(p, 16, value);
        }
    } catch (const json::exception&) {
        return false;
    }
    return false;
}

bool FieldJsonEquals(const FieldDesc& field, const json& a, const json& b)
{
    if (field.type == FieldType::Bool) {
        // 旧データは 0/1 数値、新データは真偽値 — 真理値で同一視しないと
        // プレハブ override 判定が偽陽性を出す (伝播停止 = 静かなデータ劣化)
        auto truthy = [](const json& v) -> int {
            if (v.is_boolean()) { return v.get<bool>() ? 1 : 0; }
            if (v.is_number()) { return (v.get<double>() != 0.0) ? 1 : 0; }
            return -1;
        };
        const int ta = truthy(a);
        const int tb = truthy(b);
        if (ta >= 0 && tb >= 0) {
            return ta == tb;
        }
    }
    return a == b;
}

} // namespace mye
