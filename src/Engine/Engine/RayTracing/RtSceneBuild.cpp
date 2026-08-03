#include "Engine/Engine/RayTracing/RtSceneBuild.h"

#include <algorithm>

#include "Engine/Engine/Physics/MeshColliderLibrary.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr int kMaxDepth = 32; // MeshColliderLibrary の分割打ち切りと同値

XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

// 面法線 (巻き順の規約は Cube 生成と同じ: cross(v1-v0, v2-v0) が外向き)
XMFLOAT3 FaceNormal(const XMFLOAT3& e1, const XMFLOAT3& e2)
{
    XMFLOAT3 n;
    XMStoreFloat3(&n, XMVector3Normalize(XMVector3Cross(XMLoadFloat3(&e1), XMLoadFloat3(&e2))));
    return n;
}

struct TlasItem {
    int32_t index = 0;
    float cx = 0, cy = 0, cz = 0; // AABB 中心
};

// [start, count) を再帰分割して nodes に積む。戻り値 = 作ったノード index。
// MeshColliderLibrary::BuildNode と同じ手順 (決定論)
int32_t BuildTlasNode(std::vector<RtBvhNode>& nodes, std::vector<int32_t>& order,
                      const std::vector<RtAabb>& bounds, std::vector<TlasItem>& items, int start,
                      int count, int depth)
{
    RtBvhNode node;
    bool first = true;
    for (int i = start; i < start + count; ++i) {
        const RtAabb& b = bounds[static_cast<size_t>(items[i].index)];
        if (first) {
            node.aabbMin = b.min;
            node.aabbMax = b.max;
            first = false;
        } else {
            node.aabbMin = { (std::min)(node.aabbMin.x, b.min.x), (std::min)(node.aabbMin.y, b.min.y),
                             (std::min)(node.aabbMin.z, b.min.z) };
            node.aabbMax = { (std::max)(node.aabbMax.x, b.max.x), (std::max)(node.aabbMax.y, b.max.y),
                             (std::max)(node.aabbMax.z, b.max.z) };
        }
    }

    const int32_t nodeIdx = static_cast<int32_t>(nodes.size());
    nodes.push_back(node); // 子 index は後で埋める

    if (count <= kRtTlasLeafSize || depth >= kMaxDepth) {
        const int32_t outStart = static_cast<int32_t>(order.size());
        for (int i = start; i < start + count; ++i) {
            order.push_back(items[i].index);
        }
        nodes[static_cast<size_t>(nodeIdx)].left = -(outStart + 1); // 葉マーク
        nodes[static_cast<size_t>(nodeIdx)].right = count;
        return nodeIdx;
    }

    float cminX = items[start].cx, cmaxX = cminX;
    float cminY = items[start].cy, cmaxY = cminY;
    float cminZ = items[start].cz, cmaxZ = cminZ;
    for (int i = start + 1; i < start + count; ++i) {
        cminX = (std::min)(cminX, items[i].cx);
        cmaxX = (std::max)(cmaxX, items[i].cx);
        cminY = (std::min)(cminY, items[i].cy);
        cmaxY = (std::max)(cmaxY, items[i].cy);
        cminZ = (std::min)(cminZ, items[i].cz);
        cmaxZ = (std::max)(cmaxZ, items[i].cz);
    }
    const float ex = cmaxX - cminX, ey = cmaxY - cminY, ez = cmaxZ - cminZ;
    const int axis = (ex >= ey && ex >= ez) ? 0 : (ey >= ez ? 1 : 2);

    std::sort(items.begin() + start, items.begin() + start + count,
              [axis](const TlasItem& a, const TlasItem& b) {
                  const float ca = (axis == 0) ? a.cx : (axis == 1) ? a.cy : a.cz;
                  const float cb = (axis == 0) ? b.cx : (axis == 1) ? b.cy : b.cz;
                  if (ca != cb) {
                      return ca < cb;
                  }
                  return a.index < b.index; // タイブレーク = 収集順 (決定論)
              });
    const int mid = count / 2;
    const int32_t left = BuildTlasNode(nodes, order, bounds, items, start, mid, depth + 1);
    const int32_t right =
        BuildTlasNode(nodes, order, bounds, items, start + mid, count - mid, depth + 1);
    nodes[static_cast<size_t>(nodeIdx)].left = left;
    nodes[static_cast<size_t>(nodeIdx)].right = right;
    return nodeIdx;
}

} // namespace

void FlattenBlas(const MeshColliderData& src, const std::vector<XMFLOAT3>& normals,
                 const std::vector<XMFLOAT2>& uvs, RtBlas& out)
{
    out = RtBlas{};
    if (src.nodes.empty() || src.triOrder.empty()) {
        return;
    }

    // 三角形を triOrder 順に展開 (葉の [triStart, triStart+triCount) がそのまま連続範囲になる)
    const size_t triN = src.triOrder.size();
    const size_t vertN = src.positions.size();
    const bool hasNormals = normals.size() >= vertN;
    const bool hasUvs = uvs.size() >= vertN;
    out.tris.resize(triN);
    out.attrs.resize(triN);
    for (size_t i = 0; i < triN; ++i) {
        const size_t t = static_cast<size_t>(src.triOrder[i]);
        const uint32_t i0 = src.indices[t * 3 + 0];
        const uint32_t i1 = src.indices[t * 3 + 1];
        const uint32_t i2 = src.indices[t * 3 + 2];
        const XMFLOAT3& p0 = src.positions[i0];
        const XMFLOAT3 e1 = Sub(src.positions[i1], p0);
        const XMFLOAT3 e2 = Sub(src.positions[i2], p0);
        out.tris[i].p0 = p0;
        out.tris[i].e1 = e1;
        out.tris[i].e2 = e2;

        const XMFLOAT3 fn = FaceNormal(e1, e2);
        const XMFLOAT3 n0 = hasNormals ? normals[i0] : fn;
        const XMFLOAT3 n1 = hasNormals ? normals[i1] : fn;
        const XMFLOAT3 n2 = hasNormals ? normals[i2] : fn;
        const XMFLOAT2 t0 = hasUvs ? uvs[i0] : XMFLOAT2{ 0, 0 };
        const XMFLOAT2 t1 = hasUvs ? uvs[i1] : XMFLOAT2{ 0, 0 };
        const XMFLOAT2 t2 = hasUvs ? uvs[i2] : XMFLOAT2{ 0, 0 };
        out.attrs[i].n0u0 = { n0.x, n0.y, n0.z, t0.x };
        out.attrs[i].n1v0 = { n1.x, n1.y, n1.z, t0.y };
        out.attrs[i].n2u1 = { n2.x, n2.y, n2.z, t1.x };
        out.attrs[i].uvRest = { t1.y, t2.x, t2.y, 0.0f };
    }

    // ノードは index 対応を保ったまま変換 (内部の left/right をそのまま使える)
    out.nodes.resize(src.nodes.size());
    for (size_t i = 0; i < src.nodes.size(); ++i) {
        const MeshBvhNode& s = src.nodes[i];
        RtBvhNode& d = out.nodes[i];
        d.aabbMin = { s.minX, s.minY, s.minZ };
        d.aabbMax = { s.maxX, s.maxY, s.maxZ };
        if (s.left < 0) { // 葉
            d.left = -(s.triStart + 1);
            d.right = s.triCount;
        } else {
            d.left = s.left;
            d.right = s.right;
        }
    }
}

void BuildTlas(const std::vector<RtAabb>& bounds, std::vector<RtBvhNode>& outNodes,
               std::vector<int32_t>& outOrder)
{
    outNodes.clear();
    outOrder.clear();
    const int count = static_cast<int>(bounds.size());
    if (count == 0) {
        return;
    }
    std::vector<TlasItem> items(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const RtAabb& b = bounds[static_cast<size_t>(i)];
        items[static_cast<size_t>(i)].index = i;
        items[static_cast<size_t>(i)].cx = (b.min.x + b.max.x) * 0.5f;
        items[static_cast<size_t>(i)].cy = (b.min.y + b.max.y) * 0.5f;
        items[static_cast<size_t>(i)].cz = (b.min.z + b.max.z) * 0.5f;
    }
    outOrder.reserve(static_cast<size_t>(count));
    outNodes.reserve(static_cast<size_t>(count) * 2);
    BuildTlasNode(outNodes, outOrder, bounds, items, 0, count, 0);
}

} // namespace mye
