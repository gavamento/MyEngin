// ============================================================================
//                          FootIkSystem.h
// ============================================================================
// 足の接地 (M89m)。TwoBoneIK の mode 3 (kGround) の鎖について、足元の地面をレイで探して
// 足首の目標と骨盤の下げ幅を決め、SkinnedMesh のポーズ入力 (poseIk / poseIkPelvis*) に足す。
// ============================================================================
#pragma once

#include <cstdint>
#include <set>

namespace mye {

class World;
struct RenderResources;

// TwoBoneIkSystem::Update の最後に呼ばれる (= SkinningSystem の後・PartFollowSystem の前、TwoBoneIkSystem が
// poseIk を空に戻して通常の鎖を書いた後)。鎖ごとに:
// - 先端 (足首) のアニメだけのポーズ (IK 前) でのワールド位置を、メッシュの LocalTransform の連鎖 (この tick) で求める
// - 足元の面 (メッシュのエンティティの原点の高さ) の ± groundProbe の範囲を真下へレイで探す。自分の体
//   (メッシュの祖先と子孫) のコライダーは除く。当てるのはコライダーの WorldMatrix (前 tick) = 静止した地面が前提
// - 当たれば足首をアニメの高さのまま地面の高さへずらした点を目標にする (振り上げた足は浮いたまま)。当たらなければ何もしない
// 足元の面より低い地面に置く足があれば、骨盤をその差 (pelvisMaxDrop まで) だけ下げる。前 tick の値は持たない
// (なめらかにする状態を持たない = 純粋に今 tick の入力で決まる。段差を越える瞬間は 1 tick で切り替わる)
class FootIkSystem {
public:
    void Update(World& world, const RenderResources& resources);
    void Reset() { warned_.clear(); }

private:
    std::set<uint64_t> warned_; // (entity index << 8) | 鎖の番号
};

} // namespace mye
