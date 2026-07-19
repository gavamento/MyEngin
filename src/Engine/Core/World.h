#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "Engine/Core/Archetype.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/EntityID.h"
#include "Engine/Core/Random.h"

namespace mye {

// アーキタイプ ECS の本体 (engine_spec.md 4 章)。
//
// 構造変更ポリシー (ADR-005 / spec 4.5):
//   - AddComponent / RemoveComponent / SetParent:
//       イテレーション外 → 即時適用
//       イテレーション中 → コマンドバッファに積み、tick 末に呼び出し順で適用
//   - Destroy: 常に tick 末に適用 (子階層も同時に破棄)
//   - GetComponent が返すポインタは「当該 tick 内のみ有効」。アーキタイプ移動で
//     無効化されるため、tick を跨ぐ参照は必ず EntityID で保持する
class World {
public:
    World();

    // ---- エンティティ ----
    // 基本アーキタイプ {Name, LocalTransform, WorldMatrix, Hierarchy} で即時生成
    EntityID CreateEntity(std::string_view name);
    void DestroyEntity(EntityID e); // 遅延 (tick 末)。子孫も破棄
    bool IsAlive(EntityID e) const;
    uint32_t AliveCount() const { return aliveCount_; }

    // ---- コンポーネント ----
    // 追加して初期値 (デフォルト構築済み) へのポインタを返す。
    // イテレーション中は scratch 領域を返し、tick 末に実体へ memcpy される。
    // 既に持っている場合は既存のポインタを返す
    void* AddComponentRaw(EntityID e, ComponentTypeId t);
    void RemoveComponentRaw(EntityID e, ComponentTypeId t);
    void* GetComponentRaw(EntityID e, ComponentTypeId t);
    bool HasComponent(EntityID e, ComponentTypeId t) const;

    template <typename T> T* AddComponent(EntityID e) { return static_cast<T*>(AddComponentRaw(e, T::sTypeId)); }
    template <typename T> T* GetComponent(EntityID e) { return static_cast<T*>(GetComponentRaw(e, T::sTypeId)); }
    template <typename T> void RemoveComponent(EntityID e) { RemoveComponentRaw(e, T::sTypeId); }

    // ---- 階層 ----
    void SetParent(EntityID child, EntityID parent); // 遅延 (tick 末)。parent=kNullEntity でルート化
    EntityID GetParent(EntityID e);
    bool HierarchyDirty() const { return hierarchyDirty_; }
    void ClearHierarchyDirty() { hierarchyDirty_ = false; }

    // ---- イテレーション ----
    // required の全型を含むアーキタイプを列挙する。fn(Archetype&)。
    // コールバック中の構造変更は自動的にコマンドバッファ行きになる
    template <typename Fn>
    void ForEachArchetype(std::span<const ComponentTypeId> required, Fn&& fn)
    {
        ++iterationDepth_;
        for (auto& arch : archetypes_) {
            if (arch->Count() == 0) {
                continue;
            }
            bool match = true;
            for (ComponentTypeId t : required) {
                if (!arch->HasType(t)) {
                    match = false;
                    break;
                }
            }
            if (match) {
                fn(*arch);
            }
        }
        --iterationDepth_;
    }

    // 全アーキタイプ列挙 (ハッシュ/シリアライズ用)
    std::span<const std::unique_ptr<Archetype>> Archetypes() const { return archetypes_; }

    // エンティティの所属アーキタイプ (死んでいれば nullptr)
    const Archetype* GetArchetype(EntityID e) const
    {
        return IsAlive(e) ? records_[e.index].archetype : nullptr;
    }

    // ---- 構造変更適用 (tick 末 / spec フェーズ 7) ----
    void ApplyStructuralChanges();

    // 全エンティティを即時破棄 (シーンロード / Play 復元用)。
    // generation は保持・加算されるため、古いハンドルが誤って新エンティティに
    // 一致することはない
    void Clear();

    // ワールド標準の RNG ストリーム (シード管理は Scene/Replay 側)
    Pcg32& Rng() { return rng_; }

    const char* GetName(EntityID e); // NameComponent (無ければ "")

private:
    struct EntityRecord {
        uint32_t generation = 0;
        Archetype* archetype = nullptr; // null = 未使用スロット
        uint32_t row = 0;
    };

    enum class CmdType : uint8_t { AddComponent, RemoveComponent, Destroy, SetParent };
    struct Command {
        CmdType type;
        EntityID entity;
        ComponentTypeId component = kInvalidComponentType;
        EntityID parent;              // SetParent 用
        uint32_t payloadIndex = 0xFFFFFFFFu; // AddComponent の初期値 (cmdPayloads_ 内)
    };

    Archetype* GetOrCreateArchetype(std::vector<ComponentTypeId> sortedTypes);
    void MoveEntity(EntityID e, Archetype* dst); // 共通カラムをコピーして移動
    bool IsIterating() const { return iterationDepth_ > 0; }
    void* AddComponentImmediate(EntityID e, ComponentTypeId t);
    void RemoveComponentImmediate(EntityID e, ComponentTypeId t);
    void ApplySetParent(EntityID child, EntityID parent);
    void DestroyImmediate(EntityID e); // 子孫含む
    void UnlinkFromParent(EntityID e);
    void CollectSubtree(EntityID root, std::vector<EntityID>& out);

    std::vector<EntityRecord> records_;
    std::vector<uint32_t> freeIndices_; // LIFO (決定論)
    std::vector<std::unique_ptr<Archetype>> archetypes_;
    std::vector<Command> commands_;
    // 遅延 AddComponent の初期値。コマンド追加でベクタが伸びても
    // 呼び出し側へ返した scratch ポインタが無効化されないよう、要素毎にヒープ確保
    std::vector<std::unique_ptr<std::byte[]>> cmdPayloads_;
    std::vector<ComponentTypeId> baseTypes_; // {Name, LocalTransform, WorldMatrix, Hierarchy} 昇順
    int iterationDepth_ = 0;
    bool hierarchyDirty_ = true;
    uint32_t aliveCount_ = 0;
    Pcg32 rng_;
};

} // namespace mye
