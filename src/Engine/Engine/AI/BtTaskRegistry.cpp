//====================================================================================
//                          BtTaskRegistry.cpp
//  MyEngin/ 秋田蓮音                                                     10/06/2026
//                                          BT の C++ タスクの登録表の実装
//====================================================================================
#include "Engine/Engine/AI/BtTaskRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {

namespace {

using nlohmann::json;

constexpr uint32_t kMaxAlign = 16;

std::string HexOf(uint64_t value)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buf);
}

// float 配列 (count 個) の読み書き。要素数・非有限の値が合わなければ false
bool WriteFloats(const json& value, float* dst, size_t count)
{
    if (!value.is_array() || value.size() != count) {
        return false;
    }
    float parsed[16] = {};
    for (size_t i = 0; i < count; ++i) {
        if (!value[i].is_number()) {
            return false;
        }
        parsed[i] = value[i].get<float>();
        if (!std::isfinite(parsed[i])) {
            return false;
        }
    }
    std::memcpy(dst, parsed, count * sizeof(float));
    return true;
}

json ReadFloats(const float* src, size_t count)
{
    json array = json::array();
    for (size_t i = 0; i < count; ++i) {
        array.push_back(src[i]);
    }
    return array;
}

bool WriteString(const json& value, char* dst, size_t capacity)
{
    if (!value.is_string()) {
        return false;
    }
    const std::string text = value.get<std::string>();
    if (text.size() >= capacity) {
        return false;
    }
    std::memset(dst, 0, capacity); // 終端より後ろもゼロ (状態のバイト列がハッシュに入るため)
    std::memcpy(dst, text.data(), text.size());
    return true;
}

} // namespace

uint32_t BtTaskFieldSize(int32_t type)
{
    switch (type) {
    case MYE_FIELD_FLOAT:
    case MYE_FIELD_INT32:
    case MYE_FIELD_UINT32: return 4;
    case MYE_FIELD_UINT64:
    case MYE_FIELD_ASSETREF:
    case MYE_FIELD_FLOAT2:
    case MYE_FIELD_ENTITYREF: return 8;
    case MYE_FIELD_BOOL: return 1;
    case MYE_FIELD_FLOAT3: return 12;
    case MYE_FIELD_FLOAT4:
    case MYE_FIELD_QUAT:
    case MYE_FIELD_COLOR: return 16;
    case MYE_FIELD_STRING64:
    case MYE_FIELD_FLOAT4X4: return 64;
    case MYE_FIELD_STRING256: return 256;
    default: return 0;
    }
}

bool BtTaskWriteField(const BtTaskField& field, const json& value, void* state)
{
    uint8_t* at = static_cast<uint8_t*>(state) + field.offset;
    switch (field.type) {
    case MYE_FIELD_FLOAT: {
        if (!value.is_number() || !std::isfinite(value.get<float>())) {
            return false;
        }
        const float v = value.get<float>();
        std::memcpy(at, &v, sizeof(v));
        return true;
    }
    case MYE_FIELD_INT32: {
        if (!value.is_number_integer()) {
            return false;
        }
        const int64_t wide = value.get<int64_t>();
        const int32_t v = static_cast<int32_t>((std::clamp)(wide, static_cast<int64_t>(INT32_MIN), static_cast<int64_t>(INT32_MAX)));
        std::memcpy(at, &v, sizeof(v));
        return true;
    }
    case MYE_FIELD_UINT32: {
        if (!value.is_number_integer() || value.get<int64_t>() < 0) {
            return false;
        }
        const uint32_t v = static_cast<uint32_t>((std::min)(value.get<int64_t>(), static_cast<int64_t>(UINT32_MAX)));
        std::memcpy(at, &v, sizeof(v));
        return true;
    }
    case MYE_FIELD_UINT64:
    case MYE_FIELD_ASSETREF: {
        uint64_t v = 0;
        if (value.is_number_unsigned()) {
            v = value.get<uint64_t>();
        } else if (value.is_string() && !value.get<std::string>().empty()) {
            v = std::strtoull(value.get<std::string>().c_str(), nullptr, 16);
        } else {
            return false;
        }
        std::memcpy(at, &v, sizeof(v));
        return true;
    }
    case MYE_FIELD_BOOL: {
        if (!value.is_boolean()) {
            return false;
        }
        *at = value.get<bool>() ? 1 : 0;
        return true;
    }
    case MYE_FIELD_FLOAT2: return WriteFloats(value, reinterpret_cast<float*>(at), 2);
    case MYE_FIELD_FLOAT3: return WriteFloats(value, reinterpret_cast<float*>(at), 3);
    case MYE_FIELD_FLOAT4:
    case MYE_FIELD_QUAT:
    case MYE_FIELD_COLOR: return WriteFloats(value, reinterpret_cast<float*>(at), 4);
    case MYE_FIELD_FLOAT4X4: return WriteFloats(value, reinterpret_cast<float*>(at), 16);
    case MYE_FIELD_STRING64: return WriteString(value, reinterpret_cast<char*>(at), 64);
    case MYE_FIELD_STRING256: return WriteString(value, reinterpret_cast<char*>(at), 256);
    default: return false; // EntityRef (ファイルに書けない) と未知の型
    }
}

json BtTaskReadField(const BtTaskField& field, const void* state)
{
    const uint8_t* at = static_cast<const uint8_t*>(state) + field.offset;
    switch (field.type) {
    case MYE_FIELD_FLOAT: {
        float v = 0.0f;
        std::memcpy(&v, at, sizeof(v));
        return v;
    }
    case MYE_FIELD_INT32: {
        int32_t v = 0;
        std::memcpy(&v, at, sizeof(v));
        return v;
    }
    case MYE_FIELD_UINT32: {
        uint32_t v = 0;
        std::memcpy(&v, at, sizeof(v));
        return v;
    }
    case MYE_FIELD_UINT64: {
        uint64_t v = 0;
        std::memcpy(&v, at, sizeof(v));
        return v;
    }
    case MYE_FIELD_ASSETREF: {
        uint64_t v = 0;
        std::memcpy(&v, at, sizeof(v));
        return HexOf(v);
    }
    case MYE_FIELD_BOOL: return *at != 0;
    case MYE_FIELD_FLOAT2: return ReadFloats(reinterpret_cast<const float*>(at), 2);
    case MYE_FIELD_FLOAT3: return ReadFloats(reinterpret_cast<const float*>(at), 3);
    case MYE_FIELD_FLOAT4:
    case MYE_FIELD_QUAT:
    case MYE_FIELD_COLOR: return ReadFloats(reinterpret_cast<const float*>(at), 4);
    case MYE_FIELD_FLOAT4X4: return ReadFloats(reinterpret_cast<const float*>(at), 16);
    case MYE_FIELD_STRING64: return std::string(reinterpret_cast<const char*>(at), strnlen(reinterpret_cast<const char*>(at), 64));
    case MYE_FIELD_STRING256: return std::string(reinterpret_cast<const char*>(at), strnlen(reinterpret_cast<const char*>(at), 256));
    default: return nullptr;
    }
}

void BtTaskCanonicalizeState(const BtTaskType& task, void* state)
{
    uint8_t clean[kBtCppTaskMaxStateBytes] = {};
    const uint8_t* source = static_cast<const uint8_t*>(state);
    for (const BtTaskField& field : task.fields) {
        std::memcpy(clean + field.offset, source + field.offset, BtTaskFieldSize(field.type));
    }
    std::memcpy(state, clean, task.stateSize);
}

void BtTaskInitState(const BtTaskType& task, const json& fields, void* state)
{
    std::memset(state, 0, task.stateSize);
    if (task.construct != nullptr) {
        task.construct(state);
    }
    if (fields.is_object()) {
        for (const BtTaskField& field : task.fields) {
            const auto it = fields.find(field.name);
            if (it != fields.end()) {
                BtTaskWriteField(field, *it, state);
            }
        }
    }
    BtTaskCanonicalizeState(task, state);
}

void BtTaskRegistry::Update(const MyeEngineApi* api, const MyeBtTaskDesc* descs, uint32_t count)
{
    api_ = api;
    ++generation_;
    // 今回の DLL にある名前だけ残す (関数ポインタは旧 DLL を指していて、解放後に呼べない)
    std::map<std::string, BtTaskType, std::less<>> next;
    for (uint32_t i = 0; i < count; ++i) {
        const MyeBtTaskDesc& desc = descs[i];
        if (desc.name == nullptr || desc.name[0] == '\0') {
            MYE_LOG_WARN("[behaviortree] a C++ task without a name is ignored");
            continue;
        }
        const std::string name = desc.name;
        if (next.contains(name)) {
            MYE_LOG_WARN("[behaviortree] C++ task '%s' is registered twice; the first one is used", name.c_str());
            continue;
        }
        if (desc.stateSize > kBtCppTaskMaxStateBytes || desc.stateAlign > kMaxAlign || desc.fieldCount > kBtCppTaskMaxFields) {
            MYE_LOG_WARN("[behaviortree] C++ task '%s' is not registered (state %u bytes / align %u / %u fields; limits %u / %u / %u)",
                         name.c_str(), desc.stateSize, desc.stateAlign, desc.fieldCount, kBtCppTaskMaxStateBytes, kMaxAlign,
                         kBtCppTaskMaxFields);
            continue;
        }
        BtTaskType type;
        type.name = name;
        type.stateSize = desc.stateSize;
        type.layoutHash = desc.layoutHash;
        type.construct = desc.construct;
        type.onStart = desc.onStart;
        type.onTick = desc.onTick;
        type.onAbort = desc.onAbort;
        bool fieldsOk = true;
        for (uint32_t f = 0; f < desc.fieldCount; ++f) {
            const MyeScriptField& src = desc.fields[f];
            const uint32_t size = BtTaskFieldSize(src.type);
            if (src.name == nullptr || size == 0 || src.offset + size > desc.stateSize) {
                MYE_LOG_WARN("[behaviortree] C++ task '%s' is not registered (field %u has an unsupported type or lies outside the state)",
                             name.c_str(), f);
                fieldsOk = false;
                break;
            }
            BtTaskField field;
            field.name = src.name;
            field.type = src.type;
            field.offset = src.offset;
            field.displayName = src.displayName != nullptr ? src.displayName : "";
            field.rangeMin = src.rangeMin;
            field.rangeMax = src.rangeMax;
            type.fields.push_back(std::move(field));
        }
        if (fieldsOk) {
            if (!types_.contains(name)) {
                MYE_LOG_INFO("[behaviortree] C++ task registered: %s (%u bytes, %u fields)", name.c_str(), type.stateSize,
                             static_cast<uint32_t>(type.fields.size()));
            }
            next.emplace(name, std::move(type));
        }
    }
    types_ = std::move(next);
}

void BtTaskRegistry::Clear()
{
    if (!types_.empty()) {
        ++generation_;
    }
    types_.clear();
}

const BtTaskType* BtTaskRegistry::Find(std::string_view name) const
{
    const auto it = types_.find(name);
    return it != types_.end() ? &it->second : nullptr;
}

std::vector<const BtTaskType*> BtTaskRegistry::Enumerate() const
{
    std::vector<const BtTaskType*> out;
    out.reserve(types_.size());
    for (const auto& entry : types_) {
        out.push_back(&entry.second);
    }
    return out;
}

} // namespace mye
