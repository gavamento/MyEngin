#include "Engine/Core/Archetype.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Check.h"
#include "Engine/Core/Hash.h"

namespace mye {

uint64_t ComputeSignatureHash(std::span<const ComponentTypeId> sortedTypes)
{
    return HashBytes(sortedTypes.data(), sortedTypes.size_bytes());
}

Archetype::Archetype(std::vector<ComponentTypeId> sortedTypes, uint64_t signatureHash)
    : signatureHash_(signatureHash), types_(std::move(sortedTypes))
{
    const ComponentRegistry& reg = ComponentRegistry::Get();
    sizes_.reserve(types_.size());
    columns_.resize(types_.size());
    for (ComponentTypeId t : types_) {
        sizes_.push_back(reg.Desc(t).size);
    }
}

int Archetype::FindTypeIndex(ComponentTypeId t) const
{
    // types_ は昇順 (コンストラクタ不変)。スキーマコンポーネント (最大 64 型) が積まれると
    // 型数が伸びるため二分探索 (M51a)。線形との等価性は EcsSelfTest が固定する
    const auto it = std::lower_bound(types_.begin(), types_.end(), t);
    if (it != types_.end() && *it == t) {
        return static_cast<int>(it - types_.begin());
    }
    return -1;
}

uint32_t Archetype::AddRow(EntityID e)
{
    const ComponentRegistry& reg = ComponentRegistry::Get();
    const uint32_t row = Count();
    entities_.push_back(e);
    for (size_t i = 0; i < types_.size(); ++i) {
        columns_[i].resize(columns_[i].size() + sizes_[i]);
        void* dst = columns_[i].data() + static_cast<size_t>(row) * sizes_[i];
        reg.Desc(types_[i]).construct(dst);
    }
    return row;
}

EntityID Archetype::RemoveRow(uint32_t row)
{
    MYE_CHECK(row < Count());
    const uint32_t last = Count() - 1;
    EntityID moved = kNullEntity;
    if (row != last) {
        for (size_t i = 0; i < types_.size(); ++i) {
            std::byte* col = columns_[i].data();
            memcpy(col + static_cast<size_t>(row) * sizes_[i],
                   col + static_cast<size_t>(last) * sizes_[i], sizes_[i]);
        }
        entities_[row] = entities_[last];
        moved = entities_[row];
    }
    entities_.pop_back();
    for (size_t i = 0; i < types_.size(); ++i) {
        columns_[i].resize(columns_[i].size() - sizes_[i]);
    }
    return moved;
}

void* Archetype::GetPtr(int typeIndex, uint32_t row)
{
    return columns_[typeIndex].data() + static_cast<size_t>(row) * sizes_[typeIndex];
}

const void* Archetype::GetPtr(int typeIndex, uint32_t row) const
{
    return columns_[typeIndex].data() + static_cast<size_t>(row) * sizes_[typeIndex];
}

} // namespace mye
