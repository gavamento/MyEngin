//====================================================================================
//                          NavSystem.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ナビメッシュの実行時の持ち主の実装
//====================================================================================
#include "Engine/Engine/Navigation/NavSystem.h"

#include <algorithm>
#include <cstdio>

#include "DebugDraw.h"
#include "DetourDebugDraw.h"
#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// 床と Z ファイトしないための持ち上げ (m)
constexpr float kOutlineLift = 0.03f;
// 0xRRGGBBAA (DebugLineCmd と同じ並び)
constexpr uint32_t kBoundaryColor = 0x40E8FFFFu;
constexpr uint32_t kInnerEdgeColor = 0x40E8FF60u;
constexpr uint32_t kTileBoundColor = 0xFFC040C0u;
// DebugUtils が外周を太い線 (2.5)、内側の辺を細い線 (1.5) で描くことを見分けに使う
constexpr float kBoundaryWidthMin = 2.0f;

// DebugUtils の duDebugDraw を DebugLineCmd へ流す。M82b は輪郭の線だけを使うので、
// 三角形・四角形・点は捨てる (塗りは M82d)
class LineCollector final : public duDebugDraw {
public:
    explicit LineCollector(std::vector<DebugLineCmd>& out) : out_(out) {}

    void depthMask(bool) override {}
    void texture(bool) override {}

    void begin(duDebugDrawPrimitives prim, float size) override
    {
        collecting_ = prim == DU_DRAW_LINES;
        color_ = size >= kBoundaryWidthMin ? kBoundaryColor : kInnerEdgeColor;
        pending_ = 0;
    }

    void vertex(const float* pos, unsigned int) override { Push(pos[0], pos[1], pos[2]); }
    void vertex(const float x, const float y, const float z, unsigned int) override { Push(x, y, z); }
    void vertex(const float* pos, unsigned int, const float*) override { Push(pos[0], pos[1], pos[2]); }
    void vertex(const float x, const float y, const float z, unsigned int, const float, const float) override
    {
        Push(x, y, z);
    }

    void end() override
    {
        collecting_ = false;
        pending_ = 0;
    }

private:
    void Push(float x, float y, float z)
    {
        if (!collecting_) {
            return;
        }
        if (pending_ == 0) {
            first_[0] = x;
            first_[1] = y + kOutlineLift;
            first_[2] = z;
            pending_ = 1;
            return;
        }
        DebugLineCmd cmd;
        cmd.ax = first_[0];
        cmd.ay = first_[1];
        cmd.az = first_[2];
        cmd.bx = x;
        cmd.by = y + kOutlineLift;
        cmd.bz = z;
        cmd.rgba = color_;
        out_.push_back(cmd);
        pending_ = 0;
    }

    std::vector<DebugLineCmd>& out_;
    bool collecting_ = false;
    uint32_t color_ = kBoundaryColor;
    int pending_ = 0;
    float first_[3] = {};
};

void AddRect(std::vector<DebugLineCmd>& out, const float* bmin, const float* bmax)
{
    const float y = bmin[1] + kOutlineLift;
    const float x0 = bmin[0];
    const float x1 = bmax[0];
    const float z0 = bmin[2];
    const float z1 = bmax[2];
    const float corners[4][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
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

bool KeyLess(const EntityID& a, const EntityID& b)
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

} // namespace

void NavSystem::Load(NavSurfaceRuntime& surface, const char* name)
{
    const std::wstring path = assetguid::ResolvePath(surface.assetGuid);
    NavMeshAsset::Data data;
    if (path.empty() || !NavMeshAsset::Load(path, data)) {
        surface.state = NavSurfaceState::Failed;
        MYE_LOG_ERROR("[nav] surface '%s': failed to load the .mnav asset (guid %016llx)", name,
                      static_cast<unsigned long long>(surface.assetGuid));
        return;
    }
    auto store = std::make_unique<NavTileStore>();
    if (!NavMeshAsset::BuildStore(data, *store)) {
        surface.state = NavSurfaceState::Failed;
        MYE_LOG_ERROR("[nav] surface '%s': failed to build the navigation mesh from %s", name,
                      WideToUtf8(path).c_str());
        return;
    }

    const dtNavMesh* nav = store->NavMesh();
    surface.layerCount = store->LayerCount();
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* tile = nav->getTile(i);
        if (tile == nullptr || tile->header == nullptr) {
            continue;
        }
        ++surface.tileCount;
        surface.polyCount += tile->header->polyCount;
        AddRect(surface.tileBoundLines, tile->header->bmin, tile->header->bmax);
    }
    LineCollector collector(surface.outlineLines);
    duDebugDrawNavMesh(&collector, *nav, 0);

    surface.store = std::move(store);
    surface.state = NavSurfaceState::Loaded;
    MYE_LOG_INFO("[nav] surface '%s' loaded: %d tiles, %d layers, %d polygons", name, surface.tileCount,
                 surface.layerCount, surface.polyCount);
}

void NavSystem::Update(World& world)
{
    scanKeys_.clear();
    const ComponentTypeId req[] = { NavMeshSurfaceComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(NavMeshSurfaceComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* surface = static_cast<const NavMeshSurfaceComponent*>(arch.GetPtr(si, row));
            scanKeys_.push_back({ e, surface->navAsset.value });
        }
    });
    // アーキタイプの列挙順は生成順に依るので、エンティティキー順に並べる
    std::sort(scanKeys_.begin(), scanKeys_.end(),
              [](const Key& a, const Key& b) { return KeyLess(a.entity, b.entity); });

    const bool same = scanKeys_.size() == loadedKeys_.size()
        && std::equal(scanKeys_.begin(), scanKeys_.end(), loadedKeys_.begin(),
                      [](const Key& a, const Key& b) { return a.entity == b.entity && a.assetGuid == b.assetGuid; });
    if (same) {
        return;
    }

    surfaces_.clear();
    surfaces_.reserve(scanKeys_.size());
    for (const Key& key : scanKeys_) {
        NavSurfaceRuntime& surface = surfaces_.emplace_back();
        surface.entity = key.entity;
        surface.assetGuid = key.assetGuid;
        if (key.assetGuid != 0) {
            Load(surface, world.GetName(key.entity));
        }
    }
    loadedKeys_ = scanKeys_;
}

void NavSystem::Reset()
{
    surfaces_.clear();
    loadedKeys_.clear();
}

void NavSystem::AppendDebugLines(World& world, std::vector<DebugLineCmd>& out) const
{
    for (const NavSurfaceRuntime& surface : surfaces_) {
        if (surface.state != NavSurfaceState::Loaded) {
            continue;
        }
        const auto* comp = world.GetComponent<NavMeshSurfaceComponent>(surface.entity);
        if (comp == nullptr) {
            continue;
        }
        if (comp->drawNavMesh) {
            out.insert(out.end(), surface.outlineLines.begin(), surface.outlineLines.end());
        }
        if (comp->drawTileBounds) {
            out.insert(out.end(), surface.tileBoundLines.begin(), surface.tileBoundLines.end());
        }
    }
}

} // namespace mye
