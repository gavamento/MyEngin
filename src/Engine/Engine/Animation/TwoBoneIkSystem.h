// ============================================================================
//                          TwoBoneIkSystem.h
// ============================================================================
// 2 ボーン IK の解決段 (M89l)。TwoBoneIKComponent の目標をメッシュの空間へ直して
// SkinnedMesh のポーズ入力 (poseIk) に書く。ソルバ自体は SampleSkinnedLocals の最後で走る。
// ============================================================================
#pragma once

#include <cstdint>
#include <set>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Animation/FootIkSystem.h"

namespace mye {

class World;
struct RenderResources;

// entity のワールド行列を LocalTransform の連鎖 (この tick の値) から組む (S·R·T を子から根へ)。
// WorldMatrix は物理と TransformSystem の後に書かれる前 tick の値なので、IK の目標の解決には使わない
DirectX::XMMATRIX LocalChainWorldMatrix(World& world, EntityID entity);

// SkinningSystem の後・PartFollowSystem の前に tick フェーズで呼ぶ。
// - 全 SkinnedMesh の poseIk を書き直す。TwoBoneIK が無い / ラグドール作動中 / モデル未登録は 0 本
// - 目標と曲げる側は LocalTransform の連鎖 (この tick の値) から求める。WorldMatrix (前 tick) は読まない
// - mode 0・weight 0 以下・先端ジョイントが見つからない鎖は書かない (見つからない名前は鎖ごとに 1 回だけ WARN)
// - mode 3 (接地) の鎖は、最後に呼ぶ FootIkSystem が足す。ここでは骨盤のずらし (poseIkPelvis*) も含めて空に戻すだけ
// SkinnedMesh は NoHash なので、書いた値はハッシュに入らない (姿勢が LocalTransform に出るのは部位追従を通したときだけ)
class TwoBoneIkSystem {
public:
    void Update(World& world, const RenderResources& resources);
    void Reset()
    {
        warned_.clear();
        foot_.Reset();
    }

private:
    std::set<uint64_t> warned_; // (entity index << 8) | 鎖の番号
    FootIkSystem foot_;         // 足の接地 (M89m)。通常の鎖を書いた後に足す (順序をここで固定する)
};

} // namespace mye
