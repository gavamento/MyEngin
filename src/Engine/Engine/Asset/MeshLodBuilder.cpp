//====================================================================================
//                          MeshLodBuilder.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          メッシュ LOD の自動生成の実装
//====================================================================================
#include "Engine/Engine/Asset/MeshLodBuilder.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <mutex>
#include <set>

#include "Engine/Core/Diagnostics/Log.h"
#include "meshoptimizer/meshoptimizer.h"

namespace mye::ModelCook {
namespace {

// これより少ない三角形のメッシュは単純化しても段にならない
constexpr size_t kMinTrianglesForLod = 16;
// 1 段の単純化で許す位置誤差 (メッシュ全体の大きさに対する比)。届かなければ段を作らない
constexpr float kMaxSimplifyError = 0.05f;
// 目標の index 数に対してこの倍率までは「届いた」とみなす (境界固定で目標ちょうどには届かないため)
constexpr float kReachTolerance = 1.25f;
// 画面高さ比のしきい値が前の段より必ず小さくなるようにする倍率
constexpr float kScreenSizeStep = 0.8f;
// 単純化の誤差指標に混ぜる属性の重み (位置の誤差 1 に対する相対値)
constexpr float kNormalWeight = 0.5f;
constexpr float kUvWeight = 0.5f;
constexpr float kBoneWeightWeight = 0.5f;

bool IsSkinned(const std::vector<MeshVertex>& vertices)
{
    return std::any_of(vertices.begin(), vertices.end(), [](const MeshVertex& v) {
        return v.boneWeights.x != 0.0f || v.boneWeights.y != 0.0f || v.boneWeights.z != 0.0f
            || v.boneWeights.w != 0.0f;
    });
}

// 同じメッシュの警告を毎回出さない (ホットリロードや D&D の再登録で何度も通るため)
void WarnOnce(const std::string& key, const char* reason)
{
    static std::mutex mutex;
    static std::set<std::string> warned;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!warned.insert(key).second) {
            return;
        }
    }
    MYE_LOG_WARN("[lod] no LOD generated, registering without LOD: %s (%s)", key.c_str(), reason);
}

// 単純化の入力。位置と属性が同じ頂点を 1 つに溶接した、単純化専用の頂点表
// (FBX は三角形ごとに頂点を持つので、溶接しないと全辺が境界になり何も減らない)
struct WeldedMesh {
    std::vector<uint32_t> indices;       // 溶接後の頂点番号で張った LOD0
    std::vector<float> positions;        // 3 float x 溶接後の頂点数
    std::vector<float> attributes;       // attributeCount float x 溶接後の頂点数
    std::vector<float> attributeWeights; // attributeCount
    std::vector<uint32_t> toOriginal;    // 溶接後の頂点 → LOD0 の頂点バッファでの番号 (最初に現れたもの)
    size_t attributeCount = 0;
};

bool Weld(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices, WeldedMesh& out)
{
    std::vector<unsigned int> remap(vertices.size());
    const size_t uniqueCount = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(),
                                                           vertices.data(), vertices.size(), sizeof(MeshVertex));
    if (uniqueCount == 0) {
        return false;
    }
    out.indices.resize(indices.size());
    meshopt_remapIndexBuffer(out.indices.data(), indices.data(), indices.size(), remap.data());

    const bool skinned = IsSkinned(vertices);
    out.attributeCount = skinned ? 9 : 5;
    out.toOriginal.assign(uniqueCount, UINT_MAX);
    out.positions.assign(uniqueCount * 3, 0.0f);
    out.attributes.assign(uniqueCount * out.attributeCount, 0.0f);
    for (uint32_t i = 0; i < static_cast<uint32_t>(vertices.size()); ++i) {
        const unsigned int u = remap[i];
        if (u == UINT_MAX || out.toOriginal[u] != UINT_MAX) {
            continue; // 参照されない頂点 / 溶接済みの 2 つ目以降
        }
        out.toOriginal[u] = i;
        const MeshVertex& v = vertices[i];
        float* p = &out.positions[static_cast<size_t>(u) * 3];
        p[0] = v.position.x;
        p[1] = v.position.y;
        p[2] = v.position.z;
        float* a = &out.attributes[static_cast<size_t>(u) * out.attributeCount];
        a[0] = v.normal.x;
        a[1] = v.normal.y;
        a[2] = v.normal.z;
        a[3] = v.uv.x;
        a[4] = v.uv.y;
        if (skinned) {
            a[5] = v.boneWeights.x;
            a[6] = v.boneWeights.y;
            a[7] = v.boneWeights.z;
            a[8] = v.boneWeights.w;
        }
    }
    out.attributeWeights = { kNormalWeight, kNormalWeight, kNormalWeight, kUvWeight, kUvWeight };
    if (skinned) {
        out.attributeWeights.insert(out.attributeWeights.end(), 4, kBoneWeightWeight);
    }
    return true;
}

// weldedIn を targetIndexCount 近くまで単純化する。results は溶接後の頂点番号のまま
size_t Simplify(const WeldedMesh& mesh, const std::vector<uint32_t>& weldedIn, size_t targetIndexCount,
                std::vector<uint32_t>& results)
{
    results.assign(weldedIn.size(), 0);
    const size_t written = meshopt_simplifyWithAttributes(
        results.data(), weldedIn.data(), weldedIn.size(), mesh.positions.data(), mesh.positions.size() / 3,
        sizeof(float) * 3, mesh.attributes.data(), sizeof(float) * mesh.attributeCount,
        mesh.attributeWeights.data(), mesh.attributeCount, nullptr, targetIndexCount, kMaxSimplifyError,
        meshopt_SimplifyLockBorder, nullptr);
    results.resize(written);
    return written;
}

} // namespace

MeshLodData BuildMeshLods(const std::string& key, const std::vector<MeshVertex>& vertices,
                          const std::vector<uint32_t>& indices, const importmeta::ModelLodSettings& settings)
{
    MeshLodData result;
    if (settings.levels <= 0) {
        return result;
    }
    if (indices.size() % 3 != 0 || indices.size() / 3 < kMinTrianglesForLod) {
        WarnOnce(key, "too few triangles");
        return result;
    }
    if (std::any_of(indices.begin(), indices.end(), [&](uint32_t i) { return i >= vertices.size(); })) {
        WarnOnce(key, "index out of range");
        return result;
    }

    WeldedMesh welded;
    if (!Weld(vertices, indices, welded)) {
        WarnOnce(key, "no referenced vertices");
        return result;
    }

    const size_t lod0Count = indices.size();
    std::vector<uint32_t> previous = welded.indices; // 1 つ前の段 (溶接後の頂点番号)
    float previousScreenSize = 1.0f;
    std::string stopReason; // 段が作れなかったときの理由 (届いた index 数と目標)
    for (int level = 0; level < settings.levels; ++level) {
        // 目標は LOD0 に対する比。三角形単位に丸める
        const size_t targetTriangles =
            std::max<size_t>(1, static_cast<size_t>(static_cast<double>(lod0Count / 3) * settings.ratio[level]));
        const size_t targetIndexCount = targetTriangles * 3;

        std::vector<uint32_t> simplified;
        const size_t written = Simplify(welded, previous, targetIndexCount, simplified);
        const size_t limit = static_cast<size_t>(static_cast<float>(targetIndexCount) * kReachTolerance);
        if (written < 3 || written > limit || written >= previous.size()) {
            // これ以上減らない。以降の段は目標がさらに小さいので試さない
            char reason[128];
            std::snprintf(reason, sizeof(reason), "stage %d reached %zu of %zu indices (target %zu)", level + 1,
                          written, previous.size(), targetIndexCount);
            stopReason = reason;
            break;
        }

        MeshLodLevel info;
        info.indexStart = static_cast<uint32_t>(lod0Count + result.indices.size());
        info.indexCount = static_cast<uint32_t>(written);
        const float wanted = (settings.screenSize[level] > 0.0f) ? settings.screenSize[level]
                                                                 : AutoLodScreenSize(settings.ratio[level]);
        info.screenSize = std::min(wanted, previousScreenSize * kScreenSizeStep);
        previousScreenSize = info.screenSize;
        result.levels.push_back(info);
        for (const uint32_t u : simplified) {
            result.indices.push_back(welded.toOriginal[u]);
        }
        previous = std::move(simplified);
    }
    if (result.levels.empty()) {
        WarnOnce(key, stopReason.empty() ? "simplification did not reach the target" : stopReason.c_str());
    }
    return result;
}

} // namespace mye::ModelCook
