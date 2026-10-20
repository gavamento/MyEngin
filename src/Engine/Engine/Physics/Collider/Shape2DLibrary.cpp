//====================================================================================
//                          Shape2DLibrary.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          2D 形状 (.shape2d.json) と衝突データの登録表
//====================================================================================
#include "Engine/Engine/Physics/Collider/Shape2DLibrary.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>

#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;

namespace {

constexpr float kMinEdgeRadius = 1e-4f;
constexpr float kMaxEdgeRadius = 100.0f;
constexpr float kMinDepth = 1e-3f;
constexpr float kMaxDepth = 1000.0f;
// 面積の符号を見るときの許容。これ以下の外積は共線として扱う
constexpr float kCollinearCross = 1e-9f;

// (b - a) x (c - a)。正 = a→b→c が反時計回り
float Cross2(const XMFLOAT2& a, const XMFLOAT2& b, const XMFLOAT2& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// 反時計回りの三角形 abc の内側 (辺上を含む) に p があるか
bool PointInTriangle(const XMFLOAT2& p, const XMFLOAT2& a, const XMFLOAT2& b, const XMFLOAT2& c)
{
    return Cross2(a, b, p) >= 0.0f && Cross2(b, c, p) >= 0.0f && Cross2(c, a, p) >= 0.0f;
}

// 符号つき面積の 2 倍 (反時計回りが正)
float SignedArea2(const std::vector<XMFLOAT2>& pts)
{
    float sum = 0.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
        const XMFLOAT2& a = pts[i];
        const XMFLOAT2& b = pts[(i + 1) % pts.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return sum;
}

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.shape2d.json" → "X.shape2d"
    const std::string suf = ".shape2d";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

} // namespace

// ==== 耳刈り ====

bool TriangulatePolygon2D(const std::vector<XMFLOAT2>& points, std::vector<uint32_t>& outTris)
{
    outTris.clear();
    if (points.size() < 3) {
        return false;
    }
    // 残っている頂点の添字を反時計回りに並べる (時計回りの入力は逆順にする)
    std::vector<uint32_t> idx(points.size());
    std::iota(idx.begin(), idx.end(), 0u);
    if (SignedArea2(points) < 0.0f) {
        std::reverse(idx.begin(), idx.end());
    }
    const auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
        outTris.push_back(a);
        outTris.push_back(b);
        outTris.push_back(c);
    };
    while (idx.size() > 3) {
        const size_t m = idx.size();
        bool clipped = false;
        for (size_t k = 0; k < m && !clipped; ++k) {
            const uint32_t prev = idx[(k + m - 1) % m];
            const uint32_t cur = idx[k];
            const uint32_t next = idx[(k + 1) % m];
            const float cross = Cross2(points[prev], points[cur], points[next]);
            if (cross <= kCollinearCross) {
                if (cross >= -kCollinearCross) { // 共線: 面積 0 の三角形は出さず頂点だけ落とす
                    idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(k));
                    clipped = true;
                }
                continue; // 凹頂点は耳にならない
            }
            bool blocked = false;
            for (size_t j = 0; j < m && !blocked; ++j) {
                const uint32_t v = idx[j];
                if (v == prev || v == cur || v == next) {
                    continue;
                }
                blocked = PointInTriangle(points[v], points[prev], points[cur], points[next]);
            }
            if (blocked) {
                continue;
            }
            emit(prev, cur, next);
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(k));
            clipped = true;
        }
        if (!clipped) {
            // 耳が無い (自己交差など)。先頭を強制的に切って必ず終わらせる
            emit(idx[m - 1], idx[0], idx[1]);
            idx.erase(idx.begin());
        }
    }
    if (idx.size() == 3 && std::fabs(Cross2(points[idx[0]], points[idx[1]], points[idx[2]])) > kCollinearCross) {
        emit(idx[0], idx[1], idx[2]);
    }
    return true;
}

// ==== Shape2D ====

void Shape2D::RebuildDerived()
{
    soup = MeshColliderData{};
    hull = ConvexHullData{};
    if (points.size() < 3) {
        return;
    }
    const uint32_t n = static_cast<uint32_t>(points.size());
    const float halfDepth = depth * 0.5f;

    // 頂点: [0, n) = 前面 (+z)、[n, 2n) = 背面 (-z)
    std::vector<XMFLOAT3> verts;
    verts.reserve(static_cast<size_t>(n) * 2);
    for (uint32_t i = 0; i < n; ++i) {
        verts.push_back({ points[i].x, points[i].y, halfDepth });
    }
    for (uint32_t i = 0; i < n; ++i) {
        verts.push_back({ points[i].x, points[i].y, -halfDepth });
    }

    std::vector<uint32_t> tris;
    TriangulatePolygon2D(points, tris);
    std::vector<uint32_t> indices;
    indices.reserve(tris.size() * 2 + static_cast<size_t>(n) * 6);
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
        indices.insert(indices.end(), { a, b, c });                // 前面 (+z 向き)
        indices.insert(indices.end(), { n + a, n + c, n + b });    // 背面 (-z 向き)
    }
    // 側面: 反時計回りの辺 u→v ごとに外向きの四角形 (2 枚)
    std::vector<uint32_t> ring(n);
    std::iota(ring.begin(), ring.end(), 0u);
    if (SignedArea2(points) < 0.0f) {
        std::reverse(ring.begin(), ring.end());
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t u = ring[i];
        const uint32_t v = ring[(i + 1) % n];
        indices.insert(indices.end(), { n + u, n + v, v });
        indices.insert(indices.end(), { n + u, v, u });
    }
    BuildMeshColliderData(verts, indices, soup);
    BuildConvexHull(verts, hull);
}

int32_t Shape2D::SegmentCount() const
{
    const int32_t n = static_cast<int32_t>(points.size());
    if (n < 2) {
        return 0;
    }
    return (closed && n >= 3) ? n : n - 1;
}

void Shape2D::Segment(int32_t i, XMFLOAT2& a, XMFLOAT2& b) const
{
    const size_t n = points.size();
    a = points[static_cast<size_t>(i)];
    b = points[(static_cast<size_t>(i) + 1) % n];
}

// ==== ShapeLibrary2D ====

uint64_t ShapeLibrary2D::HashForPath(const std::wstring& path)
{
    // 移動 / 改名済みアセットは .meta の GUID がキー (未移動は path-hash と同値)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t ShapeLibrary2D::Register(const std::wstring& path, Shape2D shape)
{
    const uint64_t hash = HashForPath(path);
    shape.hash = hash;
    shape.path = path;
    if (shape.name.empty()) {
        shape.name = NameFromPath(path);
    }
    shape.RebuildDerived();
    shapes_[hash] = std::move(shape);
    return hash;
}

uint64_t ShapeLibrary2D::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[shape2d] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    Shape2D s;
    if (!FromJson(j, s)) {
        MYE_LOG_WARN("[shape2d] not a 2D shape: %s", WideToUtf8(path).c_str());
        return 0;
    }
    return Register(path, std::move(s));
}

const Shape2D* ShapeLibrary2D::Get(uint64_t hash) const
{
    const auto it = shapes_.find(hash);
    return it != shapes_.end() ? &it->second : nullptr;
}

std::vector<Shape2DEntry> ShapeLibrary2D::Enumerate() const
{
    std::vector<Shape2DEntry> out;
    out.reserve(shapes_.size());
    for (const auto& [hash, shape] : shapes_) {
        out.push_back(Shape2DEntry{ hash, shape.name });
    }
    std::sort(out.begin(), out.end(), [](const Shape2DEntry& a, const Shape2DEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.hash < b.hash;
    });
    return out;
}

json ShapeLibrary2D::ToJson(const Shape2D& s)
{
    json j;
    j["engine"] = "MyEngine";
    j["shape2d"] = 1;
    json pts = json::array();
    for (const XMFLOAT2& p : s.points) {
        pts.push_back(json::array({ p.x, p.y }));
    }
    j["points"] = std::move(pts);
    j["closed"] = s.closed;
    j["edgeRadius"] = s.edgeRadius;
    j["depth"] = s.depth;
    return j;
}

bool ShapeLibrary2D::FromJson(const json& j, Shape2D& out)
{
    if (!j.is_object() || !j.contains("shape2d")) {
        return false; // 種別キー必須 (別の資産を黙って 2D 形状に読まない)
    }
    out.points.clear();
    if (j.contains("points") && j["points"].is_array()) {
        for (const json& pj : j["points"]) {
            if (pj.is_array() && pj.size() >= 2 && pj[0].is_number() && pj[1].is_number()) {
                out.points.push_back({ pj[0].get<float>(), pj[1].get<float>() });
            }
        }
    }
    out.closed = !(j.contains("closed") && j["closed"].is_boolean()) || j["closed"].get<bool>();
    out.edgeRadius = (j.contains("edgeRadius") && j["edgeRadius"].is_number()) ? j["edgeRadius"].get<float>()
                                                                               : kDefaultShape2DEdgeRadius;
    out.depth = (j.contains("depth") && j["depth"].is_number()) ? j["depth"].get<float>() : kDefaultShape2DDepth;
    Sanitize(out);
    return true;
}

void ShapeLibrary2D::Sanitize(Shape2D& s)
{
    std::vector<XMFLOAT2> kept;
    kept.reserve(s.points.size());
    for (const XMFLOAT2& p : s.points) {
        if (std::isfinite(p.x) && std::isfinite(p.y)) {
            kept.push_back(p);
        }
    }
    if (kept.size() > kMaxShape2DPoints) {
        MYE_LOG_WARN("[shape2d] %zu points; only the first %zu are kept", kept.size(), kMaxShape2DPoints);
        kept.resize(kMaxShape2DPoints);
    }
    s.points = std::move(kept);
    s.edgeRadius = std::isfinite(s.edgeRadius) ? std::clamp(s.edgeRadius, kMinEdgeRadius, kMaxEdgeRadius)
                                               : kDefaultShape2DEdgeRadius;
    s.depth = std::isfinite(s.depth) ? std::clamp(s.depth, kMinDepth, kMaxDepth) : kDefaultShape2DDepth;
    s.RebuildDerived();
}

// ==== shape2d:: モジュール注入 ====

namespace shape2d {

namespace {
ShapeLibrary2D* sLibrary = nullptr;
} // namespace

void Install(ShapeLibrary2D* lib)
{
    sLibrary = lib;
}

ShapeLibrary2D* Library()
{
    return sLibrary;
}

const Shape2D* Resolve(AssetID id)
{
    if (sLibrary == nullptr || id.IsNull()) {
        return nullptr;
    }
    return sLibrary->Get(id.value);
}

const MeshColliderData* ResolveSoup(AssetID id)
{
    const Shape2D* s = Resolve(id);
    return (s != nullptr && s->soup.TriCount() > 0) ? &s->soup : nullptr;
}

const ConvexHullData* ResolveHull(AssetID id)
{
    const Shape2D* s = Resolve(id);
    return (s != nullptr && s->hull.Valid()) ? &s->hull : nullptr;
}

} // namespace shape2d

} // namespace mye
