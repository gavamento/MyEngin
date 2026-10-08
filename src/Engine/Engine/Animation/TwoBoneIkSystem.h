// ============================================================================
//                          TwoBoneIkSystem.h
// ============================================================================
// 2 ボーン IK の解決段 (M89l)。TwoBoneIKComponent の目標をメッシュの空間へ直して
// SkinnedMesh のポーズ入力 (poseIk) に書く。ソルバ自体は SampleSkinnedLocals の最後で走る。
// ============================================================================
#pragma once

#include <cstdint>
#include <set>

namespace mye {

class World;
struct RenderResources;

// SkinningSystem の後・PartFollowSystem の前に tick フェーズで呼ぶ。
// - 全 SkinnedMesh の poseIk を書き直す。TwoBoneIK が無い / ラグドール作動中 / モデル未登録は 0 本
// - 目標と曲げる側は LocalTransform の連鎖 (この tick の値) から求める。WorldMatrix (前 tick) は読まない
// - mode 0・weight 0 以下・先端ジョイントが見つからない鎖は書かない (見つからない名前は鎖ごとに 1 回だけ WARN)
// SkinnedMesh は NoHash なので、書いた値はハッシュに入らない (姿勢が LocalTransform に出るのは部位追従を通したときだけ)
class TwoBoneIkSystem {
public:
    void Update(World& world, const RenderResources& resources);
    void Reset() { warned_.clear(); }

private:
    std::set<uint64_t> warned_; // (entity index << 8) | 鎖の番号
};

} // namespace mye
