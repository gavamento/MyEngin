//====================================================================================
//                          SpriteShape.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          SpriteShape2D のプロファイル登録表と輪郭・縁・塗りの実装
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/SpriteShape.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Physics/Collider/Shape2DLibrary.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;
using DirectX::XMFLOAT2;

namespace {

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.spriteshape.json" → "X.spriteshape"
    const std::string suf = ".spriteshape";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

// 同じ位置とみなす距離の二乗 (これ以下の点はまとめる)
constexpr float kSamePointDist2 = 1.0e-10f;
// 区間の長さがこれ未満なら法線を作れないので飛ばす
constexpr float kMinSegmentLength = 1.0e-6f;
// スプライトの位相が 1 に十分近ければ 0 に戻す (端数のごく薄いクアッドを作らない)
constexpr float kPhaseEpsilon = 1.0e-4f;
constexpr float kMaxMiterScale = kSpriteShapeMaxMiterScale;
// 1 三角形あたり格子セルを数える上限 (極端に小さい tile / 巨大な輪郭で止まらなくなるのを避ける)
constexpr int64_t kMaxFillCellsPerTriangle = 4096;
// 格子で切ったかけらの面積 (の 2 倍) がこれ以下なら、線や点に潰れたものとして捨てる
constexpr float kMinFillPieceArea2 = 1.0e-9f;
constexpr float kRadToDeg = 57.29577951308232f;
constexpr float kHalfTurnDeg = 180.0f;
constexpr float kFullTurnDeg = 360.0f;

float Dist2(const XMFLOAT2& a, const XMFLOAT2& b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

XMFLOAT2 CatmullRom(const XMFLOAT2& p0, const XMFLOAT2& p1, const XMFLOAT2& p2, const XMFLOAT2& p3, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    const auto axis = [&](float a, float b, float c, float d) {
        return 0.5f * ((2.0f * b) + (-a + c) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2
                       + (-a + 3.0f * b - 3.0f * c + d) * t3);
    };
    return { axis(p0.x, p1.x, p2.x, p3.x), axis(p0.y, p1.y, p2.y, p3.y) };
}

// 重複した連続点と、閉じた輪郭の末尾 = 先頭をまとめる。結果が閉じた輪郭として成立するか (3 点以上) を isClosed へ
void DedupOutline(const std::vector<XMFLOAT2>& in, bool closed, std::vector<XMFLOAT2>& out, bool& isClosed)
{
    out.clear();
    for (const XMFLOAT2& p : in) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            continue;
        }
        if (out.empty() || Dist2(p, out.back()) > kSamePointDist2) {
            out.push_back(p);
        }
    }
    isClosed = closed;
    if (isClosed && out.size() >= 2 && Dist2(out.front(), out.back()) <= kSamePointDist2) {
        out.pop_back();
    }
    if (isClosed && out.size() < 3) {
        isClosed = false;
    }
}

// 符号つき面積の 2 倍 (正 = 反時計回り)
float SignedArea2(const std::vector<XMFLOAT2>& pts)
{
    float area2 = 0.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
        const XMFLOAT2& a = pts[i];
        const XMFLOAT2& b = pts[(i + 1) % pts.size()];
        area2 += a.x * b.y - b.x * a.y;
    }
    return area2;
}

// 単位法線 n0, n1 の継ぎ目で、帯の端点を押し出すオフセット (半厚 1 あたり)。向きは平均、長さは 1 / cos(半角) で上限つき
XMFLOAT2 MiterOffset(const XMFLOAT2& n0, const XMFLOAT2& n1)
{
    const float mx = n0.x + n1.x;
    const float my = n0.y + n1.y;
    const float len = std::sqrt(mx * mx + my * my);
    if (len < kMinSegmentLength) {
        return n0; // 折り返し (180 度): 平均が作れない
    }
    const float ux = mx / len;
    const float uy = my / len;
    const float cosHalf = ux * n0.x + uy * n0.y;
    const float scale = 1.0f / std::max(cosHalf, 1.0f / kMaxMiterScale);
    return { ux * scale, uy * scale };
}

} // namespace

// ==== SpriteShapeLibrary ====

uint64_t SpriteShapeLibrary::HashForPath(const std::wstring& path)
{
    // 移動 / 改名済みアセットは .meta の GUID がキー (未移動は path-hash と同値)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t SpriteShapeLibrary::Register(const std::wstring& path, SpriteShapeProfile profile)
{
    const uint64_t hash = HashForPath(path);
    profile.hash = hash;
    profile.path = path;
    if (profile.name.empty()) {
        profile.name = NameFromPath(path);
    }
    profiles_[hash] = std::move(profile);
    return hash;
}

uint64_t SpriteShapeLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[spriteshape] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    SpriteShapeProfile p;
    if (!FromJson(j, p)) {
        MYE_LOG_WARN("[spriteshape] not a sprite shape profile: %s", WideToUtf8(path).c_str());
        return 0;
    }
    return Register(path, std::move(p));
}

const SpriteShapeProfile* SpriteShapeLibrary::Get(uint64_t hash) const
{
    const auto it = profiles_.find(hash);
    return it != profiles_.end() ? &it->second : nullptr;
}

std::vector<SpriteShapeEntry> SpriteShapeLibrary::Enumerate() const
{
    std::vector<SpriteShapeEntry> out;
    out.reserve(profiles_.size());
    for (const auto& [hash, profile] : profiles_) {
        out.push_back(SpriteShapeEntry{ hash, profile.name });
    }
    std::sort(out.begin(), out.end(), [](const SpriteShapeEntry& a, const SpriteShapeEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.hash < b.hash;
    });
    return out;
}

json SpriteShapeLibrary::ToJson(const SpriteShapeProfile& p)
{
    json j;
    j["engine"] = "MyEngine";
    j["spriteshape"] = 1;
    j["name"] = p.name;
    j["fill"] = p.fill;
    json edges = json::array();
    for (const SpriteShapeEdge& e : p.edges) {
        edges.push_back({ { "angleFrom", e.angleFrom }, { "angleTo", e.angleTo }, { "sprite", e.sprite } });
    }
    j["edges"] = std::move(edges);
    j["height"] = p.height;
    return j;
}

bool SpriteShapeLibrary::FromJson(const json& j, SpriteShapeProfile& out)
{
    if (!j.is_object() || !j.contains("spriteshape")) {
        return false; // 種別キー必須 (別の資産を黙ってプロファイルに読まない)
    }
    out.name = (j.contains("name") && j["name"].is_string()) ? j["name"].get<std::string>() : std::string();
    out.fill = (j.contains("fill") && j["fill"].is_number_integer()) ? j["fill"].get<uint64_t>() : 0;
    out.height = (j.contains("height") && j["height"].is_number()) ? j["height"].get<float>() : kDefaultSpriteShapeHeight;
    out.edges.clear();
    if (j.contains("edges") && j["edges"].is_array()) {
        for (const json& ej : j["edges"]) {
            if (!ej.is_object()) {
                continue;
            }
            SpriteShapeEdge e;
            if (ej.contains("angleFrom") && ej["angleFrom"].is_number()) {
                e.angleFrom = ej["angleFrom"].get<float>();
            }
            if (ej.contains("angleTo") && ej["angleTo"].is_number()) {
                e.angleTo = ej["angleTo"].get<float>();
            }
            if (ej.contains("sprite") && ej["sprite"].is_number_integer()) {
                e.sprite = ej["sprite"].get<uint64_t>();
            }
            out.edges.push_back(e);
        }
    }
    Sanitize(out);
    return true;
}

void SpriteShapeLibrary::Sanitize(SpriteShapeProfile& p)
{
    if (p.edges.size() > kMaxSpriteShapeEdges) {
        MYE_LOG_WARN("[spriteshape] %zu edges; only the first %zu are kept", p.edges.size(), kMaxSpriteShapeEdges);
        p.edges.resize(kMaxSpriteShapeEdges);
    }
    for (SpriteShapeEdge& e : p.edges) {
        if (!std::isfinite(e.angleFrom)) {
            e.angleFrom = -45.0f;
        }
        if (!std::isfinite(e.angleTo)) {
            e.angleTo = 45.0f;
        }
    }
    p.height = std::isfinite(p.height) ? std::clamp(p.height, kMinSpriteShapeHeight, kMaxSpriteShapeHeight)
                                       : kDefaultSpriteShapeHeight;
}

// ==== spriteshape:: ====

namespace spriteshape {

namespace {
SpriteShapeLibrary* sLibrary = nullptr;
} // namespace

void Install(SpriteShapeLibrary* lib)
{
    sLibrary = lib;
}

SpriteShapeLibrary* Library()
{
    return sLibrary;
}

const SpriteShapeProfile* Resolve(AssetID id)
{
    if (sLibrary == nullptr || id.IsNull()) {
        return nullptr;
    }
    return sLibrary->Get(id.value);
}

void EvaluateSpline(const XMFLOAT2* points, int count, bool closed, bool smooth, std::vector<XMFLOAT2>& out)
{
    out.clear();
    if (count <= 0 || points == nullptr) {
        return;
    }
    const bool loop = IsClosedOutline(count, closed);
    if (!smooth || count < 2) {
        out.assign(points, points + count);
        return;
    }
    const int spans = loop ? count : count - 1;
    out.reserve(static_cast<size_t>(spans) * kSpriteShapeSubdivisions + 1);
    for (int i = 0; i < spans; ++i) {
        const int i1 = i;
        const int i2 = loop ? (i + 1) % count : i + 1;
        const int i0 = loop ? (i + count - 1) % count : std::max(i - 1, 0);
        const int i3 = loop ? (i + 2) % count : std::min(i + 2, count - 1);
        for (int k = 0; k < kSpriteShapeSubdivisions; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(kSpriteShapeSubdivisions);
            out.push_back(CatmullRom(points[i0], points[i1], points[i2], points[i3], t));
        }
    }
    if (!loop) {
        out.push_back(points[count - 1]);
    }
}

int SelectEdge(const std::vector<SpriteShapeEdge>& edges, float angleDeg)
{
    if (!std::isfinite(angleDeg)) {
        return -1;
    }
    // [-180, 180] へ畳む
    float a = std::fmod(angleDeg, kFullTurnDeg);
    if (a > kHalfTurnDeg) {
        a -= kFullTurnDeg;
    } else if (a < -kHalfTurnDeg) {
        a += kFullTurnDeg;
    }
    for (size_t i = 0; i < edges.size(); ++i) {
        const float lo = std::min(edges[i].angleFrom, edges[i].angleTo);
        const float hi = std::max(edges[i].angleFrom, edges[i].angleTo);
        for (const float shift : { 0.0f, kFullTurnDeg, -kFullTurnDeg }) {
            if (a + shift >= lo && a + shift <= hi) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

float OutwardAngleDeg(float nx, float ny)
{
    // +Y を 0 度に、-X 側 (反時計回り) を正にする
    return std::atan2(-nx, ny) * kRadToDeg;
}

bool BuildEdgeQuads(const std::vector<XMFLOAT2>& polyline, bool closed, const std::vector<SpriteShapeEdge>& edges,
                    const std::vector<EdgeSpriteInfo>& sprites, float height, size_t maxQuads,
                    std::vector<EdgeQuad>& out)
{
    out.clear();
    std::vector<XMFLOAT2> pts;
    bool isClosed = false;
    DedupOutline(polyline, closed, pts, isClosed);
    if (pts.size() < 2 || !(height > 0.0f) || edges.empty()) {
        return false;
    }
    // 閉じた輪郭は時計回りに揃える: 進行方向の左 (= 法線) が外側になる
    if (isClosed && SignedArea2(pts) > 0.0f) {
        std::reverse(pts.begin(), pts.end());
    }
    const size_t n = pts.size();
    const size_t segCount = isClosed ? n : n - 1;

    std::vector<XMFLOAT2> normal(segCount);
    std::vector<float> length(segCount, 0.0f);
    std::vector<int> edgeOf(segCount, -1);
    for (size_t s = 0; s < segCount; ++s) {
        const XMFLOAT2& a = pts[s];
        const XMFLOAT2& b = pts[(s + 1) % n];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        length[s] = len;
        if (len < kMinSegmentLength) {
            continue;
        }
        normal[s] = { -dy / len, dx / len };
        int e = SelectEdge(edges, OutwardAngleDeg(normal[s].x, normal[s].y));
        if (e >= 0 && (static_cast<size_t>(e) >= sprites.size() || !(sprites[static_cast<size_t>(e)].width > 0.0f))) {
            e = -1; // スプライトが解決できない縁は描かない
        }
        edgeOf[s] = e;
    }

    const float halfH = 0.5f * height;
    float phase = 0.0f;
    int prevEdge = -1;
    for (size_t s = 0; s < segCount; ++s) {
        const int e = edgeOf[s];
        if (e < 0) {
            prevEdge = -1;
            phase = 0.0f;
            continue;
        }
        if (e != prevEdge) {
            phase = 0.0f;
        }
        prevEdge = e;
        const EdgeSpriteInfo& sp = sprites[static_cast<size_t>(e)];

        // 継ぎ目: 前後が同じ縁ならマイター、違えば自分の法線のまま
        const bool hasPrev = isClosed || s > 0;
        const bool hasNext = isClosed || s + 1 < segCount;
        const size_t prevSeg = (s + segCount - 1) % segCount;
        const size_t nextSeg = (s + 1) % segCount;
        const XMFLOAT2 offStart = (hasPrev && edgeOf[prevSeg] == e) ? MiterOffset(normal[prevSeg], normal[s]) : normal[s];
        const XMFLOAT2 offEnd = (hasNext && edgeOf[nextSeg] == e) ? MiterOffset(normal[s], normal[nextSeg]) : normal[s];

        const XMFLOAT2& a = pts[s];
        const XMFLOAT2& b = pts[(s + 1) % n];
        const XMFLOAT2 outerA = { a.x + offStart.x * halfH, a.y + offStart.y * halfH };
        const XMFLOAT2 outerB = { b.x + offEnd.x * halfH, b.y + offEnd.y * halfH };
        const XMFLOAT2 innerA = { a.x - offStart.x * halfH, a.y - offStart.y * halfH };
        const XMFLOAT2 innerB = { b.x - offEnd.x * halfH, b.y - offEnd.y * halfH };

        const float len = length[s];
        float pos = 0.0f;
        float uf0 = phase;
        float uf1 = phase;
        while (pos < len) {
            if (out.size() >= maxQuads) {
                return true;
            }
            if (uf0 >= 1.0f - kPhaseEpsilon) {
                uf0 = 0.0f;
            }
            float tileLen = (1.0f - uf0) * sp.width;
            uf1 = 1.0f;
            bool last = false;
            if (pos + tileLen >= len - kMinSegmentLength) {
                tileLen = len - pos;
                uf1 = uf0 + tileLen / sp.width;
                last = true;
            }
            const float f0 = pos / len;
            const float f1 = (pos + tileLen) / len;
            const auto lerp = [](const XMFLOAT2& p, const XMFLOAT2& q, float f) {
                return XMFLOAT2{ p.x + (q.x - p.x) * f, p.y + (q.y - p.y) * f };
            };
            const XMFLOAT2 tl = lerp(outerA, outerB, f0);
            const XMFLOAT2 tr = lerp(outerA, outerB, f1);
            const XMFLOAT2 br = lerp(innerA, innerB, f1);
            const XMFLOAT2 bl = lerp(innerA, innerB, f0);
            EdgeQuad q;
            q.edge = e;
            q.x[0] = tl.x; q.y[0] = tl.y;
            q.x[1] = tr.x; q.y[1] = tr.y;
            q.x[2] = br.x; q.y[2] = br.y;
            q.x[3] = bl.x; q.y[3] = bl.y;
            const float ua = sp.u0 + (sp.u1 - sp.u0) * uf0;
            const float ub = sp.u0 + (sp.u1 - sp.u0) * uf1;
            q.u[0] = ua; q.v[0] = sp.v0;
            q.u[1] = ub; q.v[1] = sp.v0;
            q.u[2] = ub; q.v[2] = sp.v1;
            q.u[3] = ua; q.v[3] = sp.v1;
            out.push_back(q);
            pos += tileLen;
            uf0 = 0.0f;
            if (last) {
                break;
            }
        }
        phase = (uf1 >= 1.0f - kPhaseEpsilon) ? 0.0f : uf1;
    }
    return false;
}

namespace {

// 軸 (0 = x, 1 = y) の half-plane で凸でない多角形も切る (Sutherland-Hodgman)。keepGreater = bound 以上を残す
void ClipHalfPlane(const std::vector<XMFLOAT2>& in, int axis, float bound, bool keepGreater, std::vector<XMFLOAT2>& out)
{
    out.clear();
    if (in.empty()) {
        return;
    }
    const auto coord = [axis](const XMFLOAT2& p) { return axis == 0 ? p.x : p.y; };
    const auto inside = [&](const XMFLOAT2& p) { return keepGreater ? coord(p) >= bound : coord(p) <= bound; };
    for (size_t i = 0; i < in.size(); ++i) {
        const XMFLOAT2& a = in[i];
        const XMFLOAT2& b = in[(i + 1) % in.size()];
        const bool aIn = inside(a);
        const bool bIn = inside(b);
        if (aIn) {
            out.push_back(a);
        }
        if (aIn != bIn) {
            const float t = (bound - coord(a)) / (coord(b) - coord(a));
            XMFLOAT2 p = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
            (axis == 0 ? p.x : p.y) = bound; // 境界上の座標は厳密に bound (隣のセルと割れ目を作らない)
            out.push_back(p);
        }
    }
    // 頂点が境界上にあると交点と元の点が重なる: 連続した重複をまとめ、面積 0 の三角形を作らない
    for (size_t i = 0; i < out.size() && out.size() > 1;) {
        if (Dist2(out[i], out[(i + 1) % out.size()]) <= kSamePointDist2) {
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

} // namespace

bool BuildFill(const std::vector<XMFLOAT2>& polyline, float tileW, float tileH, float u0, float v0, float u1, float v1,
               size_t maxVertices, std::vector<FillVertex>& out)
{
    out.clear();
    if (!(tileW > 0.0f) || !(tileH > 0.0f)) {
        return false;
    }
    std::vector<XMFLOAT2> pts;
    bool isClosed = false;
    DedupOutline(polyline, true, pts, isClosed);
    if (!isClosed) {
        return false;
    }
    std::vector<uint32_t> tris;
    if (!TriangulatePolygon2D(pts, tris)) {
        return false;
    }
    std::vector<XMFLOAT2> poly;
    std::vector<XMFLOAT2> clipped;
    std::vector<XMFLOAT2> scratch;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const XMFLOAT2 tri[3] = { pts[tris[t]], pts[tris[t + 1]], pts[tris[t + 2]] };
        const float minX = std::min({ tri[0].x, tri[1].x, tri[2].x });
        const float maxX = std::max({ tri[0].x, tri[1].x, tri[2].x });
        const float minY = std::min({ tri[0].y, tri[1].y, tri[2].y });
        const float maxY = std::max({ tri[0].y, tri[1].y, tri[2].y });
        const double ix0 = std::floor(static_cast<double>(minX) / tileW);
        const double ix1 = std::floor(static_cast<double>(maxX) / tileW);
        const double iy0 = std::floor(static_cast<double>(minY) / tileH);
        const double iy1 = std::floor(static_cast<double>(maxY) / tileH);
        const double cells = (ix1 - ix0 + 1.0) * (iy1 - iy0 + 1.0);
        if (!(cells <= static_cast<double>(kMaxFillCellsPerTriangle))) {
            return true; // 格子が細かすぎる / 輪郭が巨大: 描かず打ち切りとして知らせる
        }
        for (int64_t iy = static_cast<int64_t>(iy0); iy <= static_cast<int64_t>(iy1); ++iy) {
            for (int64_t ix = static_cast<int64_t>(ix0); ix <= static_cast<int64_t>(ix1); ++ix) {
                const float cx0 = static_cast<float>(ix) * tileW;
                const float cy0 = static_cast<float>(iy) * tileH;
                const float cx1 = static_cast<float>(ix + 1) * tileW;
                const float cy1 = static_cast<float>(iy + 1) * tileH;
                poly.assign(tri, tri + 3);
                ClipHalfPlane(poly, 0, cx0, true, clipped);
                ClipHalfPlane(clipped, 0, cx1, false, scratch);
                ClipHalfPlane(scratch, 1, cy0, true, clipped);
                ClipHalfPlane(clipped, 1, cy1, false, scratch);
                if (scratch.size() < 3) {
                    continue;
                }
                if (std::fabs(SignedArea2(scratch)) <= kMinFillPieceArea2) {
                    continue; // 線に潰れたかけら
                }
                if (out.size() + (scratch.size() - 2) * 3 > maxVertices) {
                    return true;
                }
                const auto vertex = [&](const XMFLOAT2& p) {
                    FillVertex fv;
                    fv.x = p.x;
                    fv.y = p.y;
                    fv.u = u0 + (p.x - cx0) / tileW * (u1 - u0);
                    fv.v = v1 - (p.y - cy0) / tileH * (v1 - v0); // テクスチャの上 = +y
                    return fv;
                };
                for (size_t k = 1; k + 1 < scratch.size(); ++k) {
                    out.push_back(vertex(scratch[0]));
                    out.push_back(vertex(scratch[k]));
                    out.push_back(vertex(scratch[k + 1]));
                }
            }
        }
    }
    return false;
}

} // namespace spriteshape
} // namespace mye
