//====================================================================================
//                          FractureSkinBake.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          骨割り当てと骨空間への変換の実装
//====================================================================================
#include "Engine/Engine/Physics/FractureSkinBake.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

using namespace DirectX;

namespace mye {
namespace {

bool PositionLess(const XMFLOAT3& a, const XMFLOAT3& b)
{
    if (a.x != b.x) {
        return a.x < b.x;
    }
    if (a.y != b.y) {
        return a.y < b.y;
    }
    return a.z < b.z;
}

bool PositionEqual(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

XMFLOAT3 Add3(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

// FractureBake.cpp の DedupPositionsExact と同じアルゴリズム (非公開ヘルパはファイルごとに
// 複製するのがこのコードベースの慣例)
std::vector<XMFLOAT3> DedupPositionsExact(std::vector<XMFLOAT3> pts)
{
    std::sort(pts.begin(), pts.end(), PositionLess);
    pts.erase(std::unique(pts.begin(), pts.end(), PositionEqual), pts.end());
    return pts;
}

// 行ベクトル規約 (v * M、並進込み) の位置変換
XMFLOAT3 TransformPoint(const XMFLOAT3& p, const XMFLOAT4X4& m)
{
    return {
        p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41,
        p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42,
        p.x * m._13 + p.y * m._23 + p.z * m._33 + m._43,
    };
}

// 3x3 の線形部分。法線変換 (逆転置) の入出力に使う
struct Mat3 {
    float m[3][3] = {};
};

// 法線の正しい変換行列 = 逆転置 (inverse-transpose)。余因子行列を行列式で割ったものが
// ちょうど「逆行列の転置」に一致する (adj(M)^T / det = (M^-1)^T) ので、転置を 2 度
// 行き来せずに済む。加減乗除だけの閉形式 — このコードベースが Debug/Release で
// ビットが割れる要因として避けている XMMatrixInverse は使わない
// (PartFollowSystem.cpp の DecomposeRowMajorTRS 導入の理由と同じ)。
// 一様スケール (回転+並進、あるいは一様スケールを含む) では M 自身と一致する
// (直交行列や一様スケール行列は逆転置が自分自身の定数倍になるため)。特異 (det≈0) な
// 縮退行列だけ、線形部分そのものへ安全側フォールバックする
Mat3 InverseTransposeLinear(const XMFLOAT4X4& m)
{
    const float m11 = m._11, m12 = m._12, m13 = m._13;
    const float m21 = m._21, m22 = m._22, m23 = m._23;
    const float m31 = m._31, m32 = m._32, m33 = m._33;
    const float c00 = m22 * m33 - m23 * m32;
    const float c01 = -(m21 * m33 - m23 * m31);
    const float c02 = m21 * m32 - m22 * m31;
    const float c10 = -(m12 * m33 - m13 * m32);
    const float c11 = m11 * m33 - m13 * m31;
    const float c12 = -(m11 * m32 - m12 * m31);
    const float c20 = m12 * m23 - m13 * m22;
    const float c21 = -(m11 * m23 - m13 * m21);
    const float c22 = m11 * m22 - m12 * m21;
    const float det = m11 * c00 + m12 * c01 + m13 * c02;
    Mat3 out;
    if (std::fabs(det) > 1e-12f) {
        const float inv = 1.0f / det;
        out.m[0][0] = c00 * inv; out.m[0][1] = c01 * inv; out.m[0][2] = c02 * inv;
        out.m[1][0] = c10 * inv; out.m[1][1] = c11 * inv; out.m[1][2] = c12 * inv;
        out.m[2][0] = c20 * inv; out.m[2][1] = c21 * inv; out.m[2][2] = c22 * inv;
    } else {
        out.m[0][0] = m11; out.m[0][1] = m12; out.m[0][2] = m13;
        out.m[1][0] = m21; out.m[1][1] = m22; out.m[1][2] = m23;
        out.m[2][0] = m31; out.m[2][1] = m32; out.m[2][2] = m33;
    }
    return out;
}

// 行ベクトル規約 (n * A) で Mat3 を掛ける
XMFLOAT3 TransformByMat3(const XMFLOAT3& n, const Mat3& a)
{
    return {
        n.x * a.m[0][0] + n.y * a.m[1][0] + n.z * a.m[2][0],
        n.x * a.m[0][1] + n.y * a.m[1][1] + n.z * a.m[2][1],
        n.x * a.m[0][2] + n.y * a.m[1][2] + n.z * a.m[2][2],
    };
}

XMFLOAT3 NormalizeOrKeep(const XMFLOAT3& n, const XMFLOAT3& fallback)
{
    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-12f) {
        const float inv = 1.0f / len;
        return { n.x * inv, n.y * inv, n.z * inv };
    }
    return fallback;
}

void TransformMeshToBoneSpace(FractureMesh& mesh, const XMFLOAT3& oldOrigin, const XMFLOAT4X4& m,
                              const Mat3& normalMat)
{
    for (FractureVertex& v : mesh.verts) {
        const XMFLOAT3 abs = Add3(oldOrigin, v.position);
        v.position = TransformPoint(abs, m);
        v.normal = NormalizeOrKeep(TransformByMat3(v.normal, normalMat), v.normal);
    }
}

// 位置 (ビット一致) -> 骨ごとの合計ウェイト。同じ位置に複数の元頂点がある場合 (UV 継ぎ目等)
// は合算する
struct WeightTable {
    std::vector<XMFLOAT3> positions; // 昇順、重複なし
    std::vector<std::map<int32_t, float>> weights; // positions と同じ並び

    const std::map<int32_t, float>* Find(const XMFLOAT3& p) const
    {
        const auto it = std::lower_bound(positions.begin(), positions.end(), p, PositionLess);
        if (it != positions.end() && PositionEqual(*it, p)) {
            return &weights[static_cast<size_t>(it - positions.begin())];
        }
        return nullptr;
    }
};

WeightTable BuildWeightTable(const std::vector<FractureSkinVertex>& verts)
{
    const size_t n = verts.size();
    std::vector<int32_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = static_cast<int32_t>(i);
    }
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        return PositionLess(verts[static_cast<size_t>(a)].position, verts[static_cast<size_t>(b)].position);
    });
    WeightTable table;
    for (int32_t idx : order) {
        const FractureSkinVertex& v = verts[static_cast<size_t>(idx)];
        if (table.positions.empty() || !PositionEqual(table.positions.back(), v.position)) {
            table.positions.push_back(v.position);
            table.weights.emplace_back();
        }
        std::map<int32_t, float>& w = table.weights.back();
        const float weightsArr[4] = { v.boneWeights.x, v.boneWeights.y, v.boneWeights.z, v.boneWeights.w };
        for (int k = 0; k < 4; ++k) {
            if (weightsArr[k] > 0.0f) {
                w[static_cast<int32_t>(v.boneIndices[k])] += weightsArr[k];
            }
        }
    }
    return table;
}

// 骨ごとの合計ウェイトから最大の骨 (同値は index 小)。map は key 昇順で走査するので
// 「厳密に上回ったときだけ更新」で自然にタイブレークが成立する
int32_t DominantBone(const std::map<int32_t, float>& w)
{
    int32_t best = -1;
    float bestWeight = -1.0f;
    for (const auto& [bone, weight] : w) {
        if (weight > bestWeight) {
            bestWeight = weight;
            best = bone;
        }
    }
    return best;
}

// 頂点 1 個自身の最大ウェイト骨 (同値は index 小)。内部破片のフォールバック用。
// boneIndices は昇順とは限らないので、こちらは明示的にタイブレークする
int32_t DominantBoneOfVertex(const FractureSkinVertex& v)
{
    const float weightsArr[4] = { v.boneWeights.x, v.boneWeights.y, v.boneWeights.z, v.boneWeights.w };
    int32_t best = -1;
    float bestWeight = -1.0f;
    for (int k = 0; k < 4; ++k) {
        const int32_t bone = static_cast<int32_t>(v.boneIndices[k]);
        const float weight = weightsArr[k];
        if (weight > bestWeight || (weight == bestWeight && (best < 0 || bone < best))) {
            bestWeight = weight;
            best = bone;
        }
    }
    return best;
}

// sourceVerts のうち p に最も近い 1 点の index (距離、同値は index 小)。線形探索
// (ボクセル化した入力では位置が元頂点と一致しないので、外側面の頂点 1 個ごとに使う —
// spec §8 round 1 裁定「ボクセル化の経路では最も近い元の頂点のウェイトを合計する」)
int32_t NearestVertexIndex(const std::vector<FractureSkinVertex>& sourceVerts, const XMFLOAT3& p)
{
    double bestDistSq = -1.0;
    int32_t best = -1;
    for (size_t k = 0; k < sourceVerts.size(); ++k) {
        const XMFLOAT3& sp = sourceVerts[k].position;
        const double dx = static_cast<double>(sp.x) - p.x;
        const double dy = static_cast<double>(sp.y) - p.y;
        const double dz = static_cast<double>(sp.z) - p.z;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (best < 0 || d2 < bestDistSq) {
            bestDistSq = d2;
            best = static_cast<int32_t>(k);
        }
    }
    return best;
}

// 1 頂点分の骨ウェイト (4 スロット) を合計用の map へ
void AddVertexWeights(std::map<int32_t, float>& out, const FractureSkinVertex& v)
{
    const float weightsArr[4] = { v.boneWeights.x, v.boneWeights.y, v.boneWeights.z, v.boneWeights.w };
    for (int k = 0; k < 4; ++k) {
        if (weightsArr[k] > 0.0f) {
            out[static_cast<int32_t>(v.boneIndices[k])] += weightsArr[k];
        }
    }
}

// 破片の外側面 1 頂点 (ソース空間の絶対位置 abs) の骨ウェイトを out へ足し込む。
// 位置のビット一致で照合できればその全頂点のウェイトを合算 (非ボクセル化の通常経路。
// 蓋との継ぎ目や切断で新しく生まれた頂点以外はほぼ確実に一致する)。一致点が無ければ
// (ボクセル化した入力は surface nets の頂点が元頂点と位置が一致しないので、ほぼ必ずここに
// 落ちる) 最も近い元頂点 1 点の 4 スロットを足す
void AccumulateVertexWeight(std::map<int32_t, float>& out, const WeightTable& table,
                            const std::vector<FractureSkinVertex>& sourceVerts, const XMFLOAT3& abs)
{
    if (const std::map<int32_t, float>* w = table.Find(abs)) {
        for (const auto& [bone, weight] : *w) {
            out[bone] += weight;
        }
        return;
    }
    const int32_t nearest = NearestVertexIndex(sourceVerts, abs);
    if (nearest >= 0) {
        AddVertexWeights(out, sourceVerts[static_cast<size_t>(nearest)]);
    }
}

} // namespace

std::vector<std::string> AssignFractureBonesAndTransform(FractureBakeResult& bake,
                                                          const std::vector<FractureSkinVertex>& sourceVerts,
                                                          const std::vector<FractureSkinJoint>& joints)
{
    std::vector<std::string> boneNames(bake.pieces.size());
    if (joints.empty() || sourceVerts.empty()) {
        return boneNames; // 非スキン相当 (呼び出し側が骨欄を空のままにする)
    }

    const WeightTable table = BuildWeightTable(sourceVerts);

    for (size_t i = 0; i < bake.pieces.size(); ++i) {
        FracturePieceBake& piece = bake.pieces[i];

        // 外側面の頂点ごとに「位置のビット一致 → 無ければ最も近い元頂点」でウェイトを集める
        // (spec §2 の通常経路と、round 1 裁定のボクセル化経路を 1 本の規則にまとめたもの:
        // ボクセル化していない入力は蓋との継ぎ目以外ほぼ全頂点が厳密一致するので実質変わらず、
        // ボクセル化した入力 (surface nets の頂点は元頂点と位置が一致しない) は全頂点が
        // 最近傍側に落ちて意味のある多数決になる)
        std::map<int32_t, float> pieceWeight;
        for (const FractureVertex& v : piece.outer.verts) {
            const XMFLOAT3 abs = Add3(piece.origin, v.position);
            AccumulateVertexWeight(pieceWeight, table, sourceVerts, abs);
        }

        int32_t bone = pieceWeight.empty() ? -1 : DominantBone(pieceWeight);
        if (bone < 0) {
            // 外側面が無い内部の破片 (outer が 0 頂点): 原点に最も近い元頂点の最大ウェイト骨
            const int32_t nearest = NearestVertexIndex(sourceVerts, piece.origin);
            bone = nearest >= 0 ? DominantBoneOfVertex(sourceVerts[static_cast<size_t>(nearest)]) : 0;
        }
        if (bone < 0 || static_cast<size_t>(bone) >= joints.size()) {
            bone = 0; // 壊れた/範囲外のウェイトは骨 0 へ (安全側。すり抜けさせない)
        }

        boneNames[i] = joints[static_cast<size_t>(bone)].name;

        const XMFLOAT3 oldOrigin = piece.origin;
        const XMFLOAT4X4& inverseBind = joints[static_cast<size_t>(bone)].inverseBind;
        const Mat3 normalMat = InverseTransposeLinear(inverseBind);
        piece.origin = { 0, 0, 0 };
        TransformMeshToBoneSpace(piece.outer, oldOrigin, inverseBind, normalMat);
        TransformMeshToBoneSpace(piece.cap, oldOrigin, inverseBind, normalMat);

        // 凸包は骨空間の点から作り直す (体積・重心・慣性・aabb・boundRadius も一緒に
        // 正しく再計算される。回転を伴う変換なので既存 hull の平行移動だけでは済まない)
        std::vector<XMFLOAT3> hullPts;
        hullPts.reserve(piece.outer.verts.size() + piece.cap.verts.size());
        for (const FractureVertex& v : piece.outer.verts) {
            hullPts.push_back(v.position);
        }
        for (const FractureVertex& v : piece.cap.verts) {
            hullPts.push_back(v.position);
        }
        BuildConvexHull(DedupPositionsExact(std::move(hullPts)), piece.hull);
    }

    return boneNames;
}

} // namespace mye
