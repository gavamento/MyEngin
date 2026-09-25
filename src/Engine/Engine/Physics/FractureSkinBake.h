//====================================================================================
//                          FractureSkinBake.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          スキン破壊: 骨割り当てと骨空間への変換
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Engine/Physics/FractureBake.h"

namespace mye {

// ---- スキン破壊 (M80j) ----
// FractureBake.h (分割コア) は骨を知らない。焼き結果を骨空間へ写す処理をここに分離する
// (spec §2「スキンメッシュの破片をどう描き、どう追従させるか」の裁定)。

// 骨割り当てに使うソース頂点 1 個。BakeFracture に渡した FractureBakeInput.sourceMesh.verts と
// 同じ並び・同じ個数であること (呼び出し側が保証する。ずれると誤った骨に割り当たる)
struct FractureSkinVertex {
    DirectX::XMFLOAT3 position{ 0, 0, 0 }; // ソース (バインドポーズ) 空間
    uint8_t boneIndices[4] = { 0, 0, 0, 0 };
    DirectX::XMFLOAT4 boneWeights = { 0, 0, 0, 0 };
};

// 骨空間変換に使う骨 1 個 (SkinnedModel.joints の抜粋)。Skeleton.h の SkinnedModel をそのまま
// 使わないのは、この層 (Engine/Physics) がスケルトンのアニメクリップまで知る必要が無いため
struct FractureSkinJoint {
    std::string name;
    // メッシュ空間 → ジョイントのバインド局所空間 (Skeleton.h の SkeletonJoint.inverseBind と同じ意味)
    DirectX::XMFLOAT4X4 inverseBind{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
};

// 焼き結果 (bake、BakeFracture の出力) を破片ごとに骨へ割り当て、origin/outer/cap/hull を
// その骨の inverseBind を掛けた空間 (原点 = 骨の原点。体積重心ではない) へ書き換える。
// 法線は inverseBind の逆転置 (inverse-transpose) で変換する (一様スケールでは inverseBind
// 自身と同じ結果。XMMatrixInverse は使わない決定的な閉形式)。
//
// 骨の割り当ては、破片の外側面 (outer) の頂点 1 個ごとに sourceVerts と位置のビット一致で
// 照合し、一致すればその頂点 (群) のウェイトを、一致しなければ**最も近い sourceVerts の 1 点**
// (距離、同値は index 小) のウェイトを合計し、最大の骨を選ぶ (同値は骨 index 小)。
// ボクセル化した入力 (surface nets の頂点は元頂点と位置が一致しない) では実質すべての頂点が
// 最近傍側に落ちる (spec §8 round 1 裁定)。外側面が無い内部の破片は、原点に最も近い
// sourceVerts の 1 点が持つ最大ウェイトの骨。
//
// sourceVerts / joints のどちらかが空なら bake は一切変更せず、全破片へ空文字列を返す
// (非スキン相当のフォールバック)。戻り値は bake.pieces と同じ並びの割り当て骨名
std::vector<std::string> AssignFractureBonesAndTransform(
    FractureBakeResult& bake, const std::vector<FractureSkinVertex>& sourceVerts,
    const std::vector<FractureSkinJoint>& joints);

} // namespace mye
