#include "Engine/Engine/Physics/MeshColliderLibrary.h"

#include <algorithm>

#include "Engine/Renderer/GpuResources.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr int kLeafTris = 8;   // 葉あたりの最大三角形数
constexpr int kMaxDepth = 32;  // 分割打ち切り (縮退データ対策)

struct TriInfo {
    int32_t tri = 0;
    float cx = 0, cy = 0, cz = 0; // centroid
};

// [start, count) を再帰分割して nodes に積む。戻り値 = 作ったノード index。
// 分割は centroid AABB の最長軸で (centroid, 三角形番号) ソート → 中央二分 (決定論)
int32_t BuildNode(MeshColliderData& md, std::vector<TriInfo>& tris, int start, int count,
                  int depth)
{
    MeshBvhNode node;
    // ノード AABB = 範囲内三角形の全頂点
    bool first = true;
    for (int i = start; i < start + count; ++i) {
        const int32_t t = tris[i].tri;
        for (int k = 0; k < 3; ++k) {
            const XMFLOAT3& v = md.positions[md.indices[t * 3 + k]];
            if (first) {
                node.minX = node.maxX = v.x;
                node.minY = node.maxY = v.y;
                node.minZ = node.maxZ = v.z;
                first = false;
            } else {
                node.minX = (v.x < node.minX) ? v.x : node.minX;
                node.minY = (v.y < node.minY) ? v.y : node.minY;
                node.minZ = (v.z < node.minZ) ? v.z : node.minZ;
                node.maxX = (v.x > node.maxX) ? v.x : node.maxX;
                node.maxY = (v.y > node.maxY) ? v.y : node.maxY;
                node.maxZ = (v.z > node.maxZ) ? v.z : node.maxZ;
            }
        }
    }

    const int32_t nodeIdx = static_cast<int32_t>(md.nodes.size());
    md.nodes.push_back(node); // 子 index は後で埋める

    if (count <= kLeafTris || depth >= kMaxDepth) {
        md.nodes[nodeIdx].triStart = static_cast<int32_t>(md.triOrder.size());
        md.nodes[nodeIdx].triCount = count;
        for (int i = start; i < start + count; ++i) {
            md.triOrder.push_back(tris[i].tri);
        }
        return nodeIdx;
    }

    // centroid AABB の最長軸
    float cminX = tris[start].cx, cmaxX = cminX;
    float cminY = tris[start].cy, cmaxY = cminY;
    float cminZ = tris[start].cz, cmaxZ = cminZ;
    for (int i = start + 1; i < start + count; ++i) {
        cminX = (tris[i].cx < cminX) ? tris[i].cx : cminX;
        cmaxX = (tris[i].cx > cmaxX) ? tris[i].cx : cmaxX;
        cminY = (tris[i].cy < cminY) ? tris[i].cy : cminY;
        cmaxY = (tris[i].cy > cmaxY) ? tris[i].cy : cmaxY;
        cminZ = (tris[i].cz < cminZ) ? tris[i].cz : cminZ;
        cmaxZ = (tris[i].cz > cmaxZ) ? tris[i].cz : cmaxZ;
    }
    const float ex = cmaxX - cminX, ey = cmaxY - cminY, ez = cmaxZ - cminZ;
    const int axis = (ex >= ey && ex >= ez) ? 0 : (ey >= ez ? 1 : 2);

    std::sort(tris.begin() + start, tris.begin() + start + count,
              [axis](const TriInfo& a, const TriInfo& b) {
                  const float ca = (axis == 0) ? a.cx : (axis == 1) ? a.cy : a.cz;
                  const float cb = (axis == 0) ? b.cx : (axis == 1) ? b.cy : b.cz;
                  if (ca != cb) {
                      return ca < cb;
                  }
                  return a.tri < b.tri; // タイブレーク = 三角形番号 (決定論)
              });
    const int mid = count / 2;
    const int32_t left = BuildNode(md, tris, start, mid, depth + 1);
    const int32_t right = BuildNode(md, tris, start + mid, count - mid, depth + 1);
    md.nodes[nodeIdx].left = left;
    md.nodes[nodeIdx].right = right;
    return nodeIdx;
}

} // namespace

void BuildMeshColliderData(const std::vector<XMFLOAT3>& positions,
                           const std::vector<uint32_t>& indices, MeshColliderData& out)
{
    out = MeshColliderData{};
    out.positions = positions;
    // 範囲外 index を持つ三角形は落とす (壊れたデータで OOB しない)
    out.indices.reserve(indices.size());
    const uint32_t n = static_cast<uint32_t>(positions.size());
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        if (indices[t] < n && indices[t + 1] < n && indices[t + 2] < n) {
            out.indices.push_back(indices[t]);
            out.indices.push_back(indices[t + 1]);
            out.indices.push_back(indices[t + 2]);
        }
    }
    const int32_t triCount = out.TriCount();
    if (triCount == 0) {
        return;
    }
    std::vector<TriInfo> tris(static_cast<size_t>(triCount));
    for (int32_t t = 0; t < triCount; ++t) {
        const XMFLOAT3& a = out.positions[out.indices[t * 3 + 0]];
        const XMFLOAT3& b = out.positions[out.indices[t * 3 + 1]];
        const XMFLOAT3& c = out.positions[out.indices[t * 3 + 2]];
        tris[t].tri = t;
        tris[t].cx = (a.x + b.x + c.x) / 3.0f;
        tris[t].cy = (a.y + b.y + c.y) / 3.0f;
        tris[t].cz = (a.z + b.z + c.z) / 3.0f;
    }
    out.triOrder.reserve(static_cast<size_t>(triCount));
    out.nodes.reserve(static_cast<size_t>(triCount) * 2);
    BuildNode(out, tris, 0, triCount, 0);
}

int MeshGatherTris(const MeshColliderData& m, float minX, float minY, float minZ, float maxX,
                   float maxY, float maxZ, int32_t* out, int maxOut)
{
    if (m.nodes.empty() || maxOut <= 0) {
        return 0;
    }
    int written = 0;
    int32_t stack[64];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const MeshBvhNode& node = m.nodes[stack[--top]];
        if (node.maxX < minX || node.minX > maxX || node.maxY < minY || node.minY > maxY
            || node.maxZ < minZ || node.minZ > maxZ) {
            continue;
        }
        if (node.left < 0) { // 葉
            for (int i = 0; i < node.triCount && written < maxOut; ++i) {
                out[written++] = m.triOrder[node.triStart + i];
            }
            continue;
        }
        if (top + 2 <= 64) {
            stack[top++] = node.right; // 固定順 (right を後入れ = left 先行の DFS)
            stack[top++] = node.left;
        }
    }
    // 決定論: 走査順に依らない昇順へ整列してから判定に使う
    std::sort(out, out + written);
    return written;
}

void MeshColliderLibrary::Register(AssetID id, MeshColliderData data)
{
    if (id.IsNull()) {
        return;
    }
    cache_[id.value] = std::make_unique<MeshColliderData>(std::move(data));
}

const MeshColliderData* MeshColliderLibrary::Get(AssetID meshAsset)
{
    if (meshAsset.IsNull()) {
        return nullptr;
    }
    auto it = cache_.find(meshAsset.value);
    if (it != cache_.end()) {
        return it->second.get();
    }
    if (resources_ == nullptr) {
        return nullptr;
    }
    Mesh* mesh = resources_->meshes.Get(meshAsset);
    if (!mesh || mesh->positions.empty() || mesh->indices.size() < 3) {
        return nullptr; // 未登録/CPU 頂点なし。キャッシュしない (後からロードされ得る)
    }
    auto data = std::make_unique<MeshColliderData>();
    BuildMeshColliderData(mesh->positions, mesh->indices, *data);
    const MeshColliderData* raw = data.get();
    cache_[meshAsset.value] = std::move(data);
    return raw;
}

namespace meshcol {
namespace {
MeshColliderLibrary* g_lib = nullptr;
} // namespace

void Install(MeshColliderLibrary* lib)
{
    g_lib = lib;
}

const MeshColliderData* Resolve(AssetID meshAsset)
{
    return g_lib ? g_lib->Get(meshAsset) : nullptr;
}

} // namespace meshcol
} // namespace mye
