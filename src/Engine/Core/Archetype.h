#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/EntityID.h"

namespace mye {

// アーキタイプ = コンポーネント型の組み合わせ 1 つ分の SoA ストレージ。
// types_ (昇順ソート済み) と並行して、型ごとに 1 本の byte 列カラムを持つ。
// 行 = エンティティ。削除は swap-and-pop (レコード表の修正は World が行う)。
class Archetype {
public:
    Archetype(std::vector<ComponentTypeId> sortedTypes, uint64_t signatureHash);

    uint64_t SignatureHash() const { return signatureHash_; }
    std::span<const ComponentTypeId> Types() const { return types_; }
    uint32_t Count() const { return static_cast<uint32_t>(entities_.size()); }
    EntityID EntityAt(uint32_t row) const { return entities_[row]; }

    // 型がなければ -1
    int FindTypeIndex(ComponentTypeId t) const;
    bool HasType(ComponentTypeId t) const { return FindTypeIndex(t) >= 0; }

    // 行を末尾に追加し、全カラムをデフォルト構築。戻り値 = 行番号
    uint32_t AddRow(EntityID e);

    // swap-and-pop。row に移動してきたエンティティ (= 旧末尾) を返す。
    // 末尾自身を消した場合は kNullEntity
    EntityID RemoveRow(uint32_t row);

    void* GetPtr(int typeIndex, uint32_t row);
    const void* GetPtr(int typeIndex, uint32_t row) const;

    // カラム一式の差し替え (要素サイズ変更可)。スクリプトのレイアウト移行専用
    // (World::ReplaceComponentStorage 以外から呼ばないこと)
    void ReplaceColumn(int typeIndex, std::vector<std::byte> data, uint32_t elemSize)
    {
        columns_[typeIndex] = std::move(data);
        sizes_[typeIndex] = elemSize;
    }

    template <typename T>
    T* ColumnData(int typeIndex)
    {
        return reinterpret_cast<T*>(columns_[typeIndex].data());
    }

private:
    uint64_t signatureHash_;
    std::vector<ComponentTypeId> types_;          // 昇順
    std::vector<uint32_t> sizes_;                 // types_ と並行
    std::vector<std::vector<std::byte>> columns_; // types_ と並行
    std::vector<EntityID> entities_;              // 行 → エンティティ
};

// 型リスト (昇順) からシグネチャハッシュを計算
uint64_t ComputeSignatureHash(std::span<const ComponentTypeId> sortedTypes);

} // namespace mye
