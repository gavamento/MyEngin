//====================================================================================
//                          NavDebugDraw.cpp
//  MyEngin/ 秋田蓮音                                                     10/04/2026
//                                          ナビメッシュの表示用ジオメトリの実装
//====================================================================================
#include "Engine/Engine/Navigation/NavDebugDraw.h"

#include <algorithm>
#include <chrono>
#include <memory>

#include "DebugDraw.h"
#include "DetourDebugDraw.h"
#include "DetourNavMesh.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Navigation/NavSystem.h"

namespace mye {

namespace {

// 塗り (下) と線 (上) を床と Z ファイトさせないための持ち上げ (m)。ナビメッシュは歩ける面より少し上に出る
constexpr float kFillLift = 0.02f;
constexpr float kLineLift = 0.03f;
// 塗りの不透明度 (0..1)。床の色が透けて見え、かつ NavMesh の範囲が一目で分かる強さ
constexpr float kFillAlpha = 0.38f;
// DebugUtils が外周を太い線 (2.5)、内側の辺を細い線 (1.5) で描くことを見分けに使う
constexpr float kBoundaryWidthMin = 2.0f;

// 表示の作り直しを毎回ログに出す回数 (以降は 2 の冪の回だけ)
constexpr uint32_t kLogAlways = 8;

// 0xRRGGBBAA (DebugLineCmd と同じ並び)
constexpr uint32_t kBoundaryColor = 0x40E8FFFFu;
constexpr uint32_t kInnerEdgeColor = 0x40E8FF60u;
constexpr uint32_t kTileBoundColor = 0xFFC040C0u;

// エリア ID ごとの塗りの色 (r, g, b)。0 = 歩行可 (Unity 風の水色)、1 = 歩行不可、2 = Jump。
// 3 以降はユーザー定義で、色相を散らした固定パレット
constexpr uint8_t kAreaPalette[kNavAreaCount][3] = {
    { 0, 192, 255 }, { 128, 128, 128 }, { 255, 150, 30 }, { 120, 220, 70 },
    { 230, 90, 190 }, { 250, 220, 50 }, { 150, 110, 255 }, { 60, 220, 170 },
    { 235, 80, 70 },  { 90, 150, 60 },  { 200, 160, 110 }, { 70, 100, 230 },
    { 190, 255, 120 }, { 255, 120, 150 }, { 120, 200, 210 }, { 180, 140, 190 },
};

bool KeyLess(const EntityID& a, const EntityID& b)
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

// DebugUtils の duDebugDraw を表示用の三角形と線へ流す。点・四角形は捨てる (ナビメッシュ本体は使わない)
class GeometryCollector final : public duDebugDraw {
public:
    GeometryCollector(std::vector<DebugFillVertex>& fill, std::vector<DebugLineCmd>& lines)
        : fill_(fill), lines_(lines)
    {
    }

    void depthMask(bool) override {}
    void texture(bool) override {}

    void begin(duDebugDrawPrimitives prim, float size) override
    {
        prim_ = prim;
        lineColor_ = size >= kBoundaryWidthMin ? kBoundaryColor : kInnerEdgeColor;
        pendingLine_ = 0;
    }

    void vertex(const float* pos, unsigned int color) override { Push(pos[0], pos[1], pos[2], color); }
    void vertex(const float x, const float y, const float z, unsigned int color) override { Push(x, y, z, color); }
    void vertex(const float* pos, unsigned int color, const float*) override { Push(pos[0], pos[1], pos[2], color); }
    void vertex(const float x, const float y, const float z, unsigned int color, const float, const float) override
    {
        Push(x, y, z, color);
    }

    void end() override
    {
        prim_ = DU_DRAW_POINTS;
        pendingLine_ = 0;
    }

    // 面ごとの色は areaToCol で決め、DebugUtils が付ける半透明度は捨てて kFillAlpha に揃える
    unsigned int areaToCol(unsigned int area) override
    {
        const uint8_t* c = kAreaPalette[area < static_cast<unsigned int>(kNavAreaCount) ? area : 0];
        return duRGBA(c[0], c[1], c[2], 255);
    }

private:
    void Push(float x, float y, float z, unsigned int color)
    {
        if (prim_ == DU_DRAW_TRIS) {
            DebugFillVertex v;
            v.x = x;
            v.y = y + kFillLift;
            v.z = z;
            v.r = static_cast<float>(color & 0xFFu) / 255.0f;
            v.g = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
            v.b = static_cast<float>((color >> 16) & 0xFFu) / 255.0f;
            v.a = kFillAlpha;
            fill_.push_back(v);
            return;
        }
        if (prim_ != DU_DRAW_LINES) {
            return;
        }
        if (pendingLine_ == 0) {
            first_[0] = x;
            first_[1] = y + kLineLift;
            first_[2] = z;
            pendingLine_ = 1;
            return;
        }
        DebugLineCmd cmd;
        cmd.ax = first_[0];
        cmd.ay = first_[1];
        cmd.az = first_[2];
        cmd.bx = x;
        cmd.by = y + kLineLift;
        cmd.bz = z;
        cmd.rgba = lineColor_;
        lines_.push_back(cmd);
        pendingLine_ = 0;
    }

    std::vector<DebugFillVertex>& fill_;
    std::vector<DebugLineCmd>& lines_;
    duDebugDrawPrimitives prim_ = DU_DRAW_POINTS;
    uint32_t lineColor_ = kBoundaryColor;
    int pendingLine_ = 0;
    float first_[3] = {};
};

void AddRect(std::vector<DebugLineCmd>& out, const float* bmin, const float* bmax)
{
    const float y = bmin[1] + kLineLift;
    const float corners[4][2] = { { bmin[0], bmin[2] }, { bmax[0], bmin[2] }, { bmax[0], bmax[2] }, { bmin[0], bmax[2] } };
    for (int i = 0; i < 4; ++i) {
        DebugLineCmd cmd;
        cmd.ax = corners[i][0];
        cmd.ay = y;
        cmd.az = corners[i][1];
        cmd.bx = corners[(i + 1) % 4][0];
        cmd.by = y;
        cmd.bz = corners[(i + 1) % 4][1];
        cmd.rgba = kTileBoundColor;
        out.push_back(cmd);
    }
}

} // namespace

void NavDebugView::ScanKeys(World& world, const NavSystem* nav)
{
    scanKeys_.clear();
    // 編集中の表示にも NavMeshModifier のエリアを映す。Modifier が無いシーンでは空のまま (確保しない)
    std::vector<NavObstacleSpec> modifiers;
    std::vector<NavObstacleSpec> modifiersHere;
    NavCollectModifierSpecs(world, modifiers);
    const ComponentTypeId req[] = { NavMeshSurfaceComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(NavMeshSurfaceComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* surface = static_cast<const NavMeshSurfaceComponent*>(arch.GetPtr(si, row));
            const EntityID e = arch.EntityAt(row);
            if (surface->navAsset.value == 0 || !IsEntityActive(world, e)) {
                continue;
            }
            Key key;
            key.entity = e;
            key.assetGuid = surface->navAsset.value;
            if (nav != nullptr) {
                // sim が読み込み済みの Surface は、その実行時のナビメッシュ (世代が変わるたびに作り直す)
                for (const NavSurfaceRuntime& rt : nav->Surfaces()) {
                    if (rt.state == NavSurfaceState::Loaded && rt.entity == e && rt.assetGuid == key.assetGuid) {
                        key.generation = rt.store->Generation();
                        key.live = rt.store.get();
                        break;
                    }
                }
            }
            if (key.live == nullptr && !modifiers.empty()) {
                NavFilterSpecsToSurface(world, e, modifiers, modifiersHere);
                uint64_t h = kNavFnvSeed;
                for (const NavObstacleSpec& spec : modifiersHere) {
                    h = NavFnv1a(h, &spec.key, sizeof(spec.key));
                    h = NavFnv1a(h, &spec.type, sizeof(spec.type));
                    h = NavFnv1a(h, spec.v, sizeof(spec.v));
                    h = NavFnv1a(h, &spec.yaw, sizeof(spec.yaw));
                    h = NavFnv1a(h, &spec.area, sizeof(spec.area));
                }
                key.modifierHash = modifiersHere.empty() ? 0 : h;
            }
            key.flags = static_cast<uint8_t>((surface->drawNavMesh ? kOutline : 0)
                                             | (surface->drawNavMeshFill ? kFill : 0)
                                             | (surface->drawTileBounds ? kTileBounds : 0));
            scanKeys_.push_back(key);
        }
    });
    // アーキタイプの列挙順は生成順に依るので、エンティティキー順に並べる
    std::sort(scanKeys_.begin(), scanKeys_.end(),
              [](const Key& a, const Key& b) { return KeyLess(a.entity, b.entity); });
}

void NavDebugView::BuildFromMesh(Geometry& g, const dtNavMesh& mesh)
{
    for (int i = 0; i < mesh.getMaxTiles(); ++i) {
        const dtMeshTile* tile = mesh.getTile(i);
        if (tile != nullptr && tile->header != nullptr) {
            AddRect(g.tileBounds, tile->header->bmin, tile->header->bmax);
        }
    }
    GeometryCollector collector(g.fill, g.outline);
    duDebugDrawNavMesh(&collector, mesh, 0);
    g.loaded = true;
}

NavDebugView::Geometry& NavDebugView::GeometryFor(World& world, const Key& key)
{
    for (Geometry& g : geometries_) {
        if (g.entity == key.entity && g.assetGuid == key.assetGuid && g.generation == key.generation
            && g.modifierHash == key.modifierHash) {
            return g;
        }
    }
    Geometry& g = geometries_.emplace_back();
    g.entity = key.entity;
    g.assetGuid = key.assetGuid;
    g.generation = key.generation;
    g.modifierHash = key.modifierHash;
    if (key.live != nullptr) {
        ++stats_.liveCount;
        BuildFromMesh(g, *static_cast<const NavTileStore*>(key.live)->NavMesh());
        return g;
    }
    ++stats_.loadCount;

    const char* name = world.GetName(key.entity);
    NavMeshAsset::Data data;
    auto store = std::make_unique<NavTileStore>();
    if (!NavMeshAsset::LoadByGuid(key.assetGuid, data) || !NavMeshAsset::BuildStore(data, *store)) {
        MYE_LOG_WARN("[nav] surface '%s': the navigation mesh could not be loaded for display (guid %016llx)", name,
                     static_cast<unsigned long long>(key.assetGuid));
        return g;
    }
    if (key.modifierHash != 0) {
        // 編集中: .mnav に NavMeshModifier を重ねた姿を見せる (Play 中の NavSystem と同じ関数・同じ塗り方)
        std::vector<NavObstacleSpec> modifiers;
        std::vector<NavObstacleSpec> modifiersHere;
        NavCollectModifierSpecs(world, modifiers);
        NavFilterSpecsToSurface(world, key.entity, modifiers, modifiersHere);
        int failures = 0;
        NavApplyModifiers(*store, modifiersHere, failures);
        if (failures > 0) {
            MYE_LOG_WARN("[nav] surface '%s': %d modifier(s) could not be applied to the displayed navigation mesh", name,
                         failures);
        }
    }
    BuildFromMesh(g, *store->NavMesh());
    return g;
}

void NavDebugView::Rebuild(World& world)
{
    const auto t0 = std::chrono::steady_clock::now();

    // 構成から消えた Surface の分を捨てる
    geometries_.erase(std::remove_if(geometries_.begin(), geometries_.end(),
                                     [this](const Geometry& g) {
                                         return std::none_of(scanKeys_.begin(), scanKeys_.end(), [&g](const Key& k) {
                                             return k.entity == g.entity && k.assetGuid == g.assetGuid
                                                 && k.generation == g.generation && k.modifierHash == g.modifierHash;
                                         });
                                     }),
                      geometries_.end());

    fill_.clear();
    lines_.clear();
    for (const Key& key : scanKeys_) {
        const Geometry& g = GeometryFor(world, key); // 結合まで使い切るので、次の読み込みで再配置されても参照は残らない
        if (!g.loaded) {
            continue;
        }
        if ((key.flags & kFill) != 0) {
            fill_.insert(fill_.end(), g.fill.begin(), g.fill.end());
        }
        if ((key.flags & kOutline) != 0) {
            lines_.insert(lines_.end(), g.outline.begin(), g.outline.end());
        }
        if ((key.flags & kTileBounds) != 0) {
            lines_.insert(lines_.end(), g.tileBounds.begin(), g.tileBounds.end());
        }
    }
    keys_ = scanKeys_;
    ++fillSerial_;
    ++stats_.rebuildCount;
    stats_.lastRebuildUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    stats_.lastTriangles = static_cast<int>(fill_.size() / 3);
    stats_.lastLines = static_cast<int>(lines_.size());
    // 動く障害物で毎フレーム作り直す場合にログを埋めないよう、最初の数回と 2 の冪の回だけ出す (回数は Stats に残る)
    const uint32_t n = stats_.rebuildCount;
    if (n <= kLogAlways || (n & (n - 1)) == 0) {
        MYE_LOG_INFO("[nav] debug view rebuilt (#%u, %u from .mnav, %u from the live navmesh): %zu surface(s), %d triangles, %d lines, %.2f ms",
                     n, stats_.loadCount, stats_.liveCount, keys_.size(), stats_.lastTriangles, stats_.lastLines,
                     stats_.lastRebuildUs / 1000.0);
    }
}

void NavDebugView::Refresh(World& world, const NavSystem* nav)
{
    ++stats_.refreshCount;
    ScanKeys(world, nav);
    if (scanKeys_ == keys_) {
        return;
    }
    Rebuild(world);
}

void NavDebugView::Reset()
{
    keys_.clear();
    scanKeys_.clear();
    geometries_.clear();
    fill_.clear();
    lines_.clear();
    ++fillSerial_;
}

} // namespace mye
