#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Engine/Core/Archetype.h"
#include "Engine/Core/ByteIo.h"
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
    // 兄弟内での位置を index 番目に変更する (遅延)。ルート (parent=null) も同じ兄弟リストで扱う。
    // 兄弟順は WorldHash に含まれない (sim 非影響) が、シーン保存/Hierarchy 表示順を決める
    void SetSiblingIndex(EntityID e, uint32_t index);
    // ルート (親を持たない) エンティティの兄弟リスト先頭。DFS 走査 (シリアライズ/Hierarchy) の起点
    EntityID FirstRoot() const { return firstRoot_; }
    bool HierarchyDirty() const { return hierarchyDirty_; }
    void ClearHierarchyDirty() { hierarchyDirty_ = false; }

    // ---- sim 索引ゲート (M51a) ----
    // false で World のクエリキャッシュと Scene の fileId 索引を素通しし、従来の
    // 線形経路に落とす (決定論 A/B / 障害切り分け用。useJobs と同じ設計)。
    // キャッシュは「結果不変・計算省略」型 — ON record → OFF verify のビット一致で
    // 透過性を実証する。既定 ON、EngineLoop が EngineConfig::useSimCache で設定する
    static void SetSimCacheEnabled(bool enabled) { sSimCacheEnabled_ = enabled; }
    static bool SimCacheEnabled() { return sSimCacheEnabled_; }

    // ---- イテレーション ----
    // required の全型を含むアーキタイプを列挙する。fn(Archetype&)。
    // コールバック中の構造変更は自動的にコマンドバッファ行きになる
    template <typename Fn>
    void ForEachArchetype(std::span<const ComponentTypeId> required, Fn&& fn)
    {
        ++iterationDepth_;
        if (const std::vector<uint32_t>* cached = QueryArchetypes(required)) {
            // キャッシュ経路: マッチ集合は archetype 生成順 = 線形経路と同一の列挙順
            for (uint32_t idx : *cached) {
                Archetype& arch = *archetypes_[idx];
                if (arch.Count() != 0) {
                    fn(arch);
                }
            }
        } else {
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

    // GameLogic.dll リロード時のスクリプト状態移行 (engine_spec.md 8.4)。
    // 旧レイアウトのカラムを新レイアウトに差し替える:
    //   名前と型が一致するフィールドのみコピー、新規フィールドはデフォルト値。
    // 最後にレジストリの記述子も更新する
    void ReplaceComponentStorage(ComponentTypeId t, ComponentDesc newDesc);

    // ---- sim スナップショット (M52d、決定台帳 1) ----
    // World の sim 状態一式 (アーキタイプのカラム生バイト / レコード表 / freeIndices /
    // ルートリスト / RNG) を生バイト列へ出し入れする。SimSnapshot 専用の入り口で、
    // private メンバへ触れる必要があるためここに置く (friend は使わない)。
    //
    // ★不変条件 1: 撮れるのは commands_ / cmdPayloads_ が空で非イテレーション中の点
    //   だけ = ApplyStructuralChanges 直後の tick 末。遅延コマンドを抱えたまま撮ると
    //   「まだ適用されていない構造変更」が消える (MYE_CHECK で固定)。
    // ★不変条件 2: アーキタイプは**生成順**で書き出す。ForEachArchetype の列挙順が
    //   そのままワールドハッシュの畳み込み順なので、復元で順序が変わるとハッシュが変わる。
    // ★復元では派生物を捨てる: queryCache_ (archetype index を持つ) は全破棄、
    //   hierarchyDirty_ は必ず true にして TransformSystem (M51c) の側テーブルを
    //   Rebuild 経由で全無効化させる (= 次 tick は全件再計算 = スキップ経路とビット同値)。
    void SnapshotWrite(ByteWriter& w) const;
    bool SnapshotRead(ByteReader& r);

    // ワールド標準の RNG ストリーム (シード管理は Scene/Replay 側)
    Pcg32& Rng() { return rng_; }

    const char* GetName(EntityID e); // NameComponent (無ければ "")

private:
    struct EntityRecord {
        uint32_t generation = 0;
        Archetype* archetype = nullptr; // null = 未使用スロット
        uint32_t row = 0;
    };

    enum class CmdType : uint8_t { AddComponent, RemoveComponent, Destroy, SetParent, SetSiblingIndex };
    struct Command {
        CmdType type;
        EntityID entity;
        ComponentTypeId component = kInvalidComponentType;
        EntityID parent;              // SetParent 用
        uint32_t payloadIndex = 0xFFFFFFFFu; // AddComponent の初期値 (cmdPayloads_ 内)
        uint32_t order = 0;           // SetSiblingIndex 用の位置
    };

    // クエリキャッシュ (M51a): required 型リスト → マッチする archetype index 列 (生成順)。
    // アーキタイプは Clear() まで append-only なので、無効化は「生成点で全エントリへ
    // 追記マッチ」だけで済む。キャッシュ無効時 (SimCacheEnabled()==false) は nullptr を
    // 返し、呼び出し側が従来の線形マッチに落ちる。
    // エントリは unique_ptr 保持 — ネストした ForEachArchetype が別クエリを充填しても
    // 外側が掴んでいる index 列が再配置されないようにするため
    struct QueryCacheEntry {
        std::vector<ComponentTypeId> required;  // 完全一致キー (ハッシュ衝突ガード)
        std::vector<uint32_t> archetypeIndices; // マッチ集合 (archetype 生成順)
    };
    const std::vector<uint32_t>* QueryArchetypes(std::span<const ComponentTypeId> required);

    Archetype* GetOrCreateArchetype(std::vector<ComponentTypeId> sortedTypes);
    void MoveEntity(EntityID e, Archetype* dst); // 共通カラムをコピーして移動
    bool IsIterating() const { return iterationDepth_ > 0; }
    void* AddComponentImmediate(EntityID e, ComponentTypeId t);
    void RemoveComponentImmediate(EntityID e, ComponentTypeId t);
    void ApplySetParent(EntityID child, EntityID parent);
    void ApplySetSiblingIndex(EntityID e, uint32_t index);
    void DestroyImmediate(EntityID e); // 子孫含む
    void UnlinkFromParent(EntityID e);
    // parent の子リスト先頭スロットへのポインタ (parent=null → &firstRoot_)。無効な parent は nullptr
    EntityID* ChildListHead(EntityID parent);
    void AppendToChildList(EntityID parent, EntityID child);
    void CollectSubtree(EntityID root, std::vector<EntityID>& out);

    std::vector<EntityRecord> records_;
    std::vector<uint32_t> freeIndices_; // LIFO (決定論)
    std::vector<std::unique_ptr<Archetype>> archetypes_;
    // クエリキャッシュ: key = required リストのハッシュ、値は同ハッシュのエントリ列
    std::unordered_map<uint64_t, std::vector<std::unique_ptr<QueryCacheEntry>>> queryCache_;
    std::vector<Command> commands_;
    // 遅延 AddComponent の初期値。コマンド追加でベクタが伸びても
    // 呼び出し側へ返した scratch ポインタが無効化されないよう、要素毎にヒープ確保
    std::vector<std::unique_ptr<std::byte[]>> cmdPayloads_;
    std::vector<ComponentTypeId> baseTypes_; // {Name, LocalTransform, WorldMatrix, Hierarchy} 昇順
    EntityID firstRoot_ = kNullEntity;       // ルートの兄弟リスト先頭 (生成順 = 既定の兄弟順)
    int iterationDepth_ = 0;
    bool hierarchyDirty_ = true;
    uint32_t aliveCount_ = 0;
    Pcg32 rng_;

    inline static bool sSimCacheEnabled_ = true;
};

} // namespace mye
