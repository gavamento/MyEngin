#pragma once
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Reflection.h"

namespace mye {

enum ComponentFlags : uint32_t {
    kComponentNone = 0,
    kComponentNoSerialize = 1u << 0, // シーン保存対象外 (WorldMatrix 等の派生値)
    kComponentScriptState = 1u << 1, // GameLogic.dll のスクリプト状態 (M4 で動的登録)
    kComponentHidden = 1u << 2,      // Inspector の Add Component 一覧に出さない
    kComponentNoHash = 1u << 3,      // ワールドハッシュ対象外 (C# スクリプト状態 = 非決定論レーン)
};

struct ComponentDesc {
    const char* name = nullptr; // 静的文字列またはレジストリが所有する文字列
    uint64_t nameHash = 0;
    uint32_t size = 0;
    uint32_t align = 0;
    uint32_t flags = kComponentNone;
    void (*construct)(void* dst) = nullptr; // デフォルト値の書き込み (placement new)
    std::vector<FieldDesc> fields;
};

// コンポーネント型の一覧。TypeId は登録順 (0 始まり)。
// 決定論のため、組み込み型の登録は static 初期化子ではなく
// RegisterBuiltinComponents() (Components.cpp) で固定順に行う。
class ComponentRegistry {
public:
    static ComponentRegistry& Get();

    // 同名 (nameHash) が登録済みならその TypeId を返す
    ComponentTypeId Register(ComponentDesc desc);

    // 記述子の差し替え (GameLogic.dll リロード用)。
    // construct 等の関数ポインタが旧 DLL を指したままにならないよう、
    // リロード時は必ずこれで更新する。サイズ変更を伴う場合は先に
    // World::ReplaceComponentStorage でカラムを移行すること
    void UpdateDesc(ComponentTypeId id, ComponentDesc desc);

    const ComponentDesc& Desc(ComponentTypeId id) const { return descs_[id]; }
    uint32_t Count() const { return static_cast<uint32_t>(descs_.size()); }
    ComponentTypeId FindByName(std::string_view name) const; // 見つからなければ kInvalidComponentType

private:
    std::vector<ComponentDesc> descs_;
};

// 組み込みコンポーネント登録ヘルパ。
// 全コンポーネントは trivially copyable であること (ECS カラムは memcpy で移動する)
template <typename T>
ComponentTypeId RegisterComponent(const char* name, std::initializer_list<FieldDesc> fields,
                                  uint32_t flags = kComponentNone)
{
    static_assert(std::is_trivially_copyable_v<T>, "ECS components must be trivially copyable PODs");
    static_assert(alignof(T) <= 16, "component alignment must be <= 16");
    ComponentDesc d;
    d.name = name;
    d.nameHash = HashStr(name);
    d.size = sizeof(T);
    d.align = alignof(T);
    d.flags = flags;
    d.construct = [](void* dst) { new (dst) T(); };
    d.fields.assign(fields.begin(), fields.end());
    const ComponentTypeId id = ComponentRegistry::Get().Register(std::move(d));
    T::sTypeId = id;
    return id;
}

} // namespace mye
