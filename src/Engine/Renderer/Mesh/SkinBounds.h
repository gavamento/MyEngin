//====================================================================================
//                          SkinBounds.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          スキンメッシュの保守的 AABB (全クリップの全姿勢を包む箱)
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Renderer/Device/GpuResources.h"

namespace mye {

// スキンメッシュの出力空間 (ボーンパレットを掛けた後、ワールド行列を掛ける前) の AABB。
struct SkinnedLocalAabb {
    DirectX::XMFLOAT3 min = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 max = { 0.0f, 0.0f, 0.0f };
};

// model の全クリップの全キーフレーム (キーの間は 1/30 秒刻み) + バインドポーズで、
// 各ボーンに影響する頂点の箱 (バインド空間) を骨行列で動かした和集合 + 余白を out に返す。
// スキニングは頂点位置の凸結合 (重みの和 1) なので、骨ごとの箱の和集合が姿勢の全頂点を包む。
// 余白 = 最長辺の半分 (下限 5cm): クリップ間のブレンドと 2 ボーン IK / 足の接地が作る、
// どのキーフレームにも無い姿勢の分。ラグドールは骨がアニメと無関係に動くので呼び出し側が常に可視にする。
// 重みが全 0 の頂点 (シェーダが恒等で描く) はバインド位置のまま含める。
// skin が空 (重みを持たないメッシュ) は positions の AABB。決定的 (入力だけで決まる)。
// 属性の数が合わない・結果が非有限のときは false (呼び出し側は常に可視として扱う)。
bool ComputeSkinnedLocalAabb(const SkinnedModel& model, const std::vector<DirectX::XMFLOAT3>& positions,
                             const std::vector<MeshSkinVertex>& skin, SkinnedLocalAabb& out);

// (SkinnedModel, Mesh) の組ごとの保守的 AABB のキャッシュ。メッシュとスケルトンは別々に登録され
// 組はエンティティでしか決まらないので、登録時ではなく最初に描くときに計算する。
// 登録の通番 (revision) が変わったら計算し直す (ホットリロード)。描画スレッドの直列部でだけ呼ぶ。
class SkinBoundsCache {
public:
    // 求められない (未登録・属性不整合) は nullptr。返した箱は次の Get まで有効
    const SkinnedLocalAabb* Get(const SkinnedModelLibrary& models, AssetID model, MeshLibrary& meshes,
                                AssetID mesh);
    void Clear() { entries_.clear(); }
    size_t ComputedCount() const { return computed_; } // 計算した回数 (テスト用)

private:
    struct Entry {
        uint64_t modelRevision = 0;
        uint64_t meshRevision = 0;
        bool valid = false;
        SkinnedLocalAabb box;
    };
    std::map<std::pair<uint64_t, uint64_t>, Entry> entries_;
    size_t computed_ = 0;
};

} // namespace mye
