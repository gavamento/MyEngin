#include "Engine/Core/ComponentRegistry.h"

#include "Engine/Core/Check.h"

namespace mye {

uint32_t FieldTypeSize(FieldType t)
{
    switch (t) {
    case FieldType::Float:    return 4;
    case FieldType::Int32:    return 4;
    case FieldType::UInt32:   return 4;
    case FieldType::UInt64:   return 8;
    case FieldType::Bool:     return 1;
    case FieldType::Float2:   return 8;
    case FieldType::Float3:   return 12;
    case FieldType::Float4:   return 16;
    case FieldType::Quat:     return 16;
    case FieldType::Color:    return 16;
    case FieldType::EntityRef: return 8;
    case FieldType::AssetRef: return 8;
    case FieldType::String64: return 64;
    case FieldType::Float4x4: return 64;
    case FieldType::String256: return 256;
    }
    return 0;
}

ComponentRegistry& ComponentRegistry::Get()
{
    static ComponentRegistry instance;
    return instance;
}

ComponentTypeId ComponentRegistry::Register(ComponentDesc desc)
{
    for (uint32_t i = 0; i < descs_.size(); ++i) {
        if (descs_[i].nameHash == desc.nameHash) {
            return i; // 登録済み
        }
    }
    MYE_CHECKF(desc.size > 0 && desc.construct != nullptr, "invalid ComponentDesc '%s'", desc.name);
    descs_.push_back(std::move(desc));
    return static_cast<ComponentTypeId>(descs_.size() - 1);
}

void ComponentRegistry::UpdateDesc(ComponentTypeId id, ComponentDesc desc)
{
    MYE_CHECK(id < descs_.size());
    MYE_CHECKF(descs_[id].nameHash == desc.nameHash, "UpdateDesc: name mismatch for '%s'", desc.name);
    descs_[id] = std::move(desc);
}

ComponentTypeId ComponentRegistry::FindByName(std::string_view name) const
{
    const uint64_t hash = HashStr(name);
    for (uint32_t i = 0; i < descs_.size(); ++i) {
        if (descs_[i].nameHash == hash) {
            return i;
        }
    }
    return kInvalidComponentType;
}

} // namespace mye
