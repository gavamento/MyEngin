//====================================================================================
//                          MeshShape.cpp
//  MyEngin/ 秋田蓮音                                                       11/19/2026
//                                          物理・音がメッシュから形を作るときの頂点
//====================================================================================
#include "Engine/Renderer/Mesh/MeshShape.h"

#include <cmath>
#include <string>

#include "Engine/Core/Diagnostics/Log.h"

using namespace DirectX;

namespace mye {
namespace {

// forward_skinned.hlsl / shadow_depth_skinned.hlsl と同じ「スキニングする頂点」の閾値
constexpr float kWeightSum = 1e-4f;

} // namespace

bool ComputeBindPosePositions(const SkinnedModel& model, const std::vector<XMFLOAT3>& positions,
                              const std::vector<MeshSkinVertex>& skin, std::vector<XMFLOAT3>& out)
{
    out.clear();
    if (positions.empty() || (!skin.empty() && skin.size() != positions.size())) {
        return false;
    }
    if (skin.empty()) {
        out = positions; // 重みを持たないメッシュはシェーダでも恒等
        return true;
    }
    // 描画と同じ関数で clip -1 (= バインドポーズ) のパレットを作る。返るのは
    // シェーダへ上げる転置済みの行列なので、行ベクトル規約へ戻してから掛ける
    std::vector<XMFLOAT4X4> palette;
    ComputeBonePalette(model, -1, 0.0f, palette);
    std::vector<XMMATRIX> bones(palette.size());
    for (size_t b = 0; b < palette.size(); ++b) {
        bones[b] = XMMatrixTranspose(XMLoadFloat4x4(&palette[b]));
    }

    out.resize(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        const MeshSkinVertex& sv = skin[i];
        const float w[4] = { sv.boneWeights.x, sv.boneWeights.y, sv.boneWeights.z, sv.boneWeights.w };
        if (w[0] + w[1] + w[2] + w[3] <= kWeightSum) {
            out[i] = positions[i];
            continue;
        }
        const XMVECTOR p = XMLoadFloat3(&positions[i]);
        XMVECTOR sum = XMVectorZero();
        for (int k = 0; k < 4; ++k) {
            if (w[k] != 0.0f && sv.boneIndices[k] < bones.size()) {
                sum = XMVectorAdd(sum, XMVectorScale(XMVector3Transform(p, bones[sv.boneIndices[k]]), w[k]));
            }
        }
        XMStoreFloat3(&out[i], sum);
        if (!std::isfinite(out[i].x) || !std::isfinite(out[i].y) || !std::isfinite(out[i].z)) {
            out.clear();
            return false;
        }
    }
    return true;
}

const std::vector<XMFLOAT3>* MeshShapePositions(const SkinnedModelLibrary& models, const Mesh& mesh,
                                                std::vector<XMFLOAT3>& scratch)
{
    if (mesh.skinModel.IsNull()) {
        return &mesh.positions;
    }
    const SkinnedModel* model = models.Get(mesh.skinModel);
    if (model == nullptr) {
        return nullptr;
    }
    if (!ComputeBindPosePositions(*model, mesh.positions, mesh.skin, scratch)) {
        MYE_LOG_WARN("mesh shape: バインドポーズの頂点を求められないので、スキンを掛ける前の頂点で形を作ります"
                     " (頂点 %zu / スキン属性 %zu / ジョイント %zu)",
                     mesh.positions.size(), mesh.skin.size(), model->joints.size());
        return &mesh.positions;
    }
    return &scratch;
}

} // namespace mye
