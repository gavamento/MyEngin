#pragma once
#include <vector>

#include <DirectXMath.h>

#include "Engine/Renderer/RayTracing/RtTypes.h"

namespace mye {

struct MeshColliderData;

// GPU 転送前の BLAS 1 個分 (連結前のローカル index)
struct RtBlas {
    std::vector<RtBvhNode> nodes;
    std::vector<RtTri> tris;
    std::vector<RtTriAttr> attrs;
};

// 物理と共用の決定論 BVH (MeshColliderData) + 頂点属性から GPU 用 BLAS を焼く。
// 三角形は triOrder の順に並べ替えて出力するので、葉は triOrder を間接参照せず
// [start, start+count) の連続範囲を直接指せる。
// normals/uvs が足りないメッシュは面法線 / UV=0 で補う。純関数 (selftest 対象)
void FlattenBlas(const MeshColliderData& src, const std::vector<DirectX::XMFLOAT3>& normals,
                 const std::vector<DirectX::XMFLOAT2>& uvs, RtBlas& out);

// TLAS 構築の入力 (インスタンスのワールド AABB)
struct RtAabb {
    DirectX::XMFLOAT3 min = { 0, 0, 0 };
    DirectX::XMFLOAT3 max = { 0, 0, 0 };
};

// インスタンスのワールド AABB 列から TLAS を構築する。
// outOrder = 葉が参照するインスタンス番号列 — 呼び出し側はこの順にインスタンスバッファを
// 並べ替えること (葉が連続範囲を指す前提)。
// 分割は BLAS と同じ「centroid の最長軸 + (centroid, index) ソート + 中央二分」= 決定論。
// 純関数 (selftest 対象)
void BuildTlas(const std::vector<RtAabb>& bounds, std::vector<RtBvhNode>& outNodes,
               std::vector<int32_t>& outOrder);

} // namespace mye
