//====================================================================================
//                          Tags.h
//  MyEngin/ 秋田蓮音                                                       09/17/2026
//                                          汎用タグの判定と検索 (sim / 描画 / スクリプト共通)
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/EntityID.h"

namespace mye {

class World;

// 汎用タグ (TagComponent) の判定と検索。**規則はここの 1 本きり** —
// スクリプトの ABI (HasTag / FindEntitiesWithTag)、描画の RT フィルタ、エディタの表示が
// 同じ関数を見ることで「エディタで付けたタグ」と「実行時に効くタグ」が食い違わないようにする。
namespace Tags {

// タグ番号 → ビット。範囲外 (負 / kMaxTags 以上) は 0 = どのタグとも一致しない
constexpr uint64_t BitOf(int32_t tagIndex)
{
    return (tagIndex >= 0 && tagIndex < kMaxTags) ? (1ull << static_cast<uint32_t>(tagIndex)) : 0ull;
}

// 自分のタグ集合 (TagComponent が無ければ 0)
uint64_t OwnMask(World& world, EntityID e);

// 自分 + 祖先すべてのタグ集合の OR。
// ★描画 (RT のフィルタ) はこちらを使う — FBX / glTF のモデルはメッシュが子孫エンティティに
//   分かれているので、ルートに付けたタグが子のメッシュに効かないと「モデルに付けたのに
//   何も起きない」になる。IsEntityActive (Active の伝播) と同じ向きの規則。
// ★スクリプトの HasTag は OwnMask を使う (Unity の Tag と同じく「その物自身の印」) —
//   継承込みで答えると、敵の子に付いた武器まで「敵」として引っかかる
uint64_t EffectiveMask(World& world, EntityID e);

// フィルタ判定。filterMask == 0 は「制限なし」= 常に true (既定 = 従来の挙動)。
// 非 0 なら、mask がフィルタのどれか 1 つでも持っていれば true
constexpr bool PassesFilter(uint64_t mask, uint64_t filterMask)
{
    return filterMask == 0 || (mask & filterMask) != 0;
}

// tagIndex を**自分で**持つ生存エンティティを EntityID の index 昇順で集める。
// ★並びを index 昇順に固定しているのは、アーキタイプ内の行順が生成・破棄・構造変更の履歴で
//   変わるため。スクリプトが結果の先頭を使ったとき、スナップショット復元やネットの再シムで
//   答えがずれない順序にする (規則 7)
void FindEntitiesWithTag(World& world, int32_t tagIndex, std::vector<EntityID>& out);

} // namespace Tags
} // namespace mye
