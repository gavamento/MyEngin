//====================================================================================
//                          GridNav2D.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          タイルマップ上の 2D グリッド A* の実装
//====================================================================================
#include "Engine/Engine/Navigation/GridNav2D.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>

#include "Engine/Core/Diagnostics/Profiler.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Engine/Tilemap/TilemapSystem.h"

namespace mye {
namespace {

// 辺のコスト (spec 4.1.13)
constexpr int32_t kWalkCost = 10;
constexpr int32_t kJumpBaseCost = 20;
constexpr int32_t kHeuristicScale = 10; // マンハッタン距離 x 10
// 足元のセルを決めるとき、足の高さへ足すセル単位の下駄 (床の天面ちょうどに立つと一つ下のセルに入るのを避ける)
constexpr float kFeetBias = 0.1f;
// 進展の判定: 距離がこれ以上縮まったら進展 (ワールド単位)
constexpr float kProgressEpsilon = 0.01f;
constexpr float kUnsetDistance = 1.0e30f;

enum class CellKind : uint8_t { kEmpty, kSolid, kOneWay };
enum class EdgeKind : uint8_t { kWalk, kFall, kJump };

struct Edge {
    int32_t x = 0;
    int32_t y = 0;
    int32_t cost = 0;
    EdgeKind kind = EdgeKind::kWalk;
};

struct JumpParams {
    int32_t height = 2;
    int32_t length = 3;
};

} // namespace

// 1 つの Tilemap のセル問い合わせ。Update の間だけ生きる (セルは Update の間変わらない)
struct GridNavView {
    World* world = nullptr;
    const TilemapSystem* tiles = nullptr;
    EntityID owner = kNullEntity;
    const TilemapAsset* map = nullptr;
    const TilePalette* palette = nullptr;
    std::vector<int32_t> layers; // collision が立ったレイヤー
    bool any = false;            // 空でないセルが 1 つでもある
    int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
    float originX = 0.0f, originY = 0.0f, cellW = 1.0f, cellH = 1.0f;
    std::unordered_map<uint32_t, CellKind> cache; // 引くだけ (走査しない)

    CellKind Kind(int32_t x, int32_t y)
    {
        if (x < TilemapSystem::kCellCoordMin || x > TilemapSystem::kCellCoordMax || y < TilemapSystem::kCellCoordMin
            || y > TilemapSystem::kCellCoordMax) {
            return CellKind::kEmpty;
        }
        const uint32_t key = TilemapSystem::MakeCellKey(x, y);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
        CellKind kind = CellKind::kEmpty;
        for (const int32_t layer : layers) {
            const uint32_t value = tiles->GetCell(*world, owner, layer, x, y);
            const TileDef* def = palette->Find(tilecell::TileId(value));
            if (def == nullptr || def->collider == 0) {
                continue;
            }
            if (!def->oneWay) {
                kind = CellKind::kSolid;
                break;
            }
            kind = CellKind::kOneWay;
        }
        cache.emplace(key, kind);
        return kind;
    }
    bool Solid(int32_t x, int32_t y) { return Kind(x, y) == CellKind::kSolid; }
    bool Ground(int32_t x, int32_t y) { return Kind(x, y) != CellKind::kEmpty; }
    // 歩行可能 = 範囲内で、固体でなく、直下が固体 / 一方通行
    bool Walkable(int32_t x, int32_t y)
    {
        if (!any || x < minX || x > maxX || y < minY + 1 || y > maxY + 1) {
            return false;
        }
        return !Solid(x, y) && Ground(x, y - 1);
    }
};

namespace {

// from から出る辺を決まった順 (左 → 右、歩き / 落下 → ジャンプの距離昇順・高さ昇順) に fn へ渡す
template <typename Fn>
void ForEachEdge(GridNavView& v, int32_t x, int32_t y, const JumpParams& jp, Fn&& fn)
{
    for (int32_t dirIndex = 0; dirIndex < 2; ++dirIndex) {
        const int32_t dir = (dirIndex == 0) ? -1 : 1;
        const int32_t x1 = x + dir;
        if (!v.Solid(x1, y)) {
            if (v.Walkable(x1, y)) {
                fn(Edge{ x1, y, kWalkCost, EdgeKind::kWalk });
            } else {
                // 落下: 隣の列を、直下が床になる最初のセルまで落ちる (段差は任意)
                for (int32_t r = y - 1; r >= v.minY + 1; --r) {
                    if (v.Walkable(x1, r)) {
                        fn(Edge{ x1, r, kWalkCost + (y - r), EdgeKind::kFall });
                        break;
                    }
                    if (v.Solid(x1, r)) {
                        break;
                    }
                }
            }
        }
        for (int32_t dx = 1; dx <= jp.length; ++dx) {
            const int32_t tx = x + dir * dx;
            for (int32_t dy = -jp.height; dy <= jp.height; ++dy) {
                if (dx == 1 && dy <= 0) {
                    continue; // 歩き / 落下と同じ
                }
                const int32_t ty = y + dy;
                if (!v.Walkable(tx, ty)) {
                    continue;
                }
                bool clear = true;
                if (dy >= 0) {
                    // 先に真上へ昇り、その高さで水平に渡る
                    for (int32_t r = y + 1; r <= y + dy && clear; ++r) {
                        clear = !v.Solid(x, r);
                    }
                    for (int32_t i = 1; i <= dx && clear; ++i) {
                        clear = !v.Solid(x + dir * i, ty);
                    }
                    // 同じ高さで途中が全部歩ける = 歩けば済む (平らな床の上でジャンプしない)
                    if (clear && dy == 0) {
                        bool walkAll = true;
                        for (int32_t i = 1; i < dx && walkAll; ++i) {
                            walkAll = v.Walkable(x + dir * i, y);
                        }
                        if (walkAll) {
                            continue;
                        }
                    }
                } else {
                    // 先に水平に渡り、目的の列を真下へ降りる
                    for (int32_t i = 1; i <= dx && clear; ++i) {
                        clear = !v.Solid(x + dir * i, y);
                    }
                    for (int32_t r = ty; r < y && clear; ++r) {
                        clear = !v.Solid(tx, r);
                    }
                }
                if (clear) {
                    fn(Edge{ tx, ty, kJumpBaseCost + dx + std::abs(dy), EdgeKind::kJump });
                }
            }
        }
    }
}

// from から to への辺 (複数あれば最も安いもの)。無ければ false
bool EdgeBetween(GridNavView& v, int32_t fx, int32_t fy, int32_t tx, int32_t ty, const JumpParams& jp, Edge& out)
{
    bool found = false;
    ForEachEdge(v, fx, fy, jp, [&](const Edge& e) {
        if (e.x == tx && e.y == ty && (!found || e.cost < out.cost)) {
            out = e;
            found = true;
        }
    });
    return found;
}

// (x, y) か、その近く (チェビシェフ距離 kGoalSearchRadius 以内) の歩行可能セルのうち最も近いもの。
// 距離の二乗が同じなら cellKey の小さい方。無ければ false
bool NearestWalkable(GridNavView& v, int32_t x, int32_t y, int32_t& outX, int32_t& outY)
{
    if (v.Walkable(x, y)) {
        outX = x;
        outY = y;
        return true;
    }
    bool found = false;
    int32_t bestDist = 0;
    uint32_t bestKey = 0;
    constexpr int32_t r = GridNavSystem::kGoalSearchRadius;
    for (int32_t dy = -r; dy <= r; ++dy) {
        for (int32_t dx = -r; dx <= r; ++dx) {
            if (!v.Walkable(x + dx, y + dy)) {
                continue;
            }
            const int32_t dist = dx * dx + dy * dy;
            const uint32_t key = TilemapSystem::MakeCellKey(x + dx, y + dy);
            if (!found || dist < bestDist || (dist == bestDist && key < bestKey)) {
                found = true;
                bestDist = dist;
                bestKey = key;
                outX = x + dx;
                outY = y + dy;
            }
        }
    }
    return found;
}

enum class SearchKind : uint8_t { kReached, kNoPath, kPartial };

struct SearchResult {
    SearchKind kind = SearchKind::kNoPath;
    bool hasStep = false; // 開始セルの次に進むセル
    int32_t stepX = 0;
    int32_t stepY = 0;
};

struct SearchNode {
    int32_t x = 0;
    int32_t y = 0;
    int32_t g = 0;
    int32_t parent = -1;
    uint32_t key = 0;
};

struct OpenEntry {
    int32_t f = 0;
    int32_t g = 0;
    uint32_t key = 0;
    int32_t node = 0;
};

// 二分ヒープ (std::push_heap) の「優先度が低い」順。キー (f, g, cellKey) の全順序
bool OpenWorse(const OpenEntry& a, const OpenEntry& b)
{
    return std::tie(a.f, a.g, a.key) > std::tie(b.f, b.g, b.key);
}

int32_t Heuristic(int32_t x, int32_t y, int32_t gx, int32_t gy)
{
    return (std::abs(x - gx) + std::abs(y - gy)) * kHeuristicScale;
}

// start から goal への A*。開始・目的のセルは歩行可能であること。
// 展開が上限を超えたら、届いたノードのうち目的地に最も近い (h, g, cellKey の最小) ノードへの経路を返す (kPartial)
SearchResult Search(GridNavView& v, const JumpParams& jp, int32_t sx, int32_t sy, int32_t gx, int32_t gy)
{
    SearchResult result;
    if (sx == gx && sy == gy) {
        result.kind = SearchKind::kReached;
        return result;
    }
    std::vector<SearchNode> nodes;
    std::vector<OpenEntry> open;
    std::unordered_map<uint32_t, int32_t> indexOf; // 引くだけ
    nodes.reserve(256);
    open.reserve(256);

    const uint32_t startKey = TilemapSystem::MakeCellKey(sx, sy);
    nodes.push_back(SearchNode{ sx, sy, 0, -1, startKey });
    indexOf.emplace(startKey, 0);
    open.push_back(OpenEntry{ Heuristic(sx, sy, gx, gy), 0, startKey, 0 });

    int32_t bestNode = 0;
    int32_t bestH = Heuristic(sx, sy, gx, gy);
    const auto noteBest = [&](int32_t nodeIndex) {
        const SearchNode& n = nodes[static_cast<size_t>(nodeIndex)];
        const int32_t h = Heuristic(n.x, n.y, gx, gy);
        const SearchNode& b = nodes[static_cast<size_t>(bestNode)];
        if (std::tie(h, n.g, n.key) < std::tie(bestH, b.g, b.key)) {
            bestNode = nodeIndex;
            bestH = h;
        }
    };

    int32_t expansions = 0;
    int32_t endNode = -1;
    bool limited = false;
    while (!open.empty()) {
        std::pop_heap(open.begin(), open.end(), OpenWorse);
        const OpenEntry top = open.back();
        open.pop_back();
        const SearchNode cur = nodes[static_cast<size_t>(top.node)];
        if (top.g != cur.g) {
            continue; // より安い経路が見つかって積み直された古い項
        }
        if (cur.x == gx && cur.y == gy) {
            endNode = top.node;
            break;
        }
        if (expansions >= GridNavSystem::kMaxExpansionsPerPlan) {
            limited = true;
            break;
        }
        ++expansions;
        ForEachEdge(v, cur.x, cur.y, jp, [&](const Edge& e) {
            const int32_t g = cur.g + e.cost;
            const uint32_t key = TilemapSystem::MakeCellKey(e.x, e.y);
            const auto found = indexOf.find(key);
            int32_t index = 0;
            if (found == indexOf.end()) {
                index = static_cast<int32_t>(nodes.size());
                nodes.push_back(SearchNode{ e.x, e.y, g, top.node, key });
                indexOf.emplace(key, index);
            } else {
                index = found->second;
                if (g >= nodes[static_cast<size_t>(index)].g) {
                    return;
                }
                nodes[static_cast<size_t>(index)].g = g;
                nodes[static_cast<size_t>(index)].parent = top.node;
            }
            open.push_back(OpenEntry{ g + Heuristic(e.x, e.y, gx, gy), g, key, index });
            std::push_heap(open.begin(), open.end(), OpenWorse);
            noteBest(index);
        });
    }

    int32_t tail = -1;
    if (endNode >= 0) {
        result.kind = SearchKind::kReached;
        tail = endNode;
    } else if (limited) {
        result.kind = SearchKind::kPartial;
        tail = bestNode;
    } else {
        result.kind = SearchKind::kNoPath;
        return result;
    }
    // 開始の次のノードまで親をたどる
    while (tail > 0 && nodes[static_cast<size_t>(tail)].parent > 0) {
        tail = nodes[static_cast<size_t>(tail)].parent;
    }
    if (tail > 0) {
        result.hasStep = true;
        result.stepX = nodes[static_cast<size_t>(tail)].x;
        result.stepY = nodes[static_cast<size_t>(tail)].y;
    }
    return result;
}

void ResetPlanState(GridAgent2DComponent& ga)
{
    ga.hasPlan = false;
    ga.planAge = 0;
    ga.stuckTicks = 0;
    ga.bestDistance = kUnsetDistance;
}

} // namespace

GridNavSystem::GridNavSystem() = default;
GridNavSystem::~GridNavSystem() = default;

GridNavView* GridNavSystem::ViewFor(World& world, const TilemapSystem& tiles, EntityID tilemapEntity)
{
    for (const ViewEntry& entry : views_) {
        if (entry.owner == tilemapEntity) {
            return entry.view.get();
        }
    }
    ViewEntry entry;
    entry.owner = tilemapEntity;
    if (!tilemapEntity.IsNull() && world.IsAlive(tilemapEntity) && IsEntityActive(world, tilemapEntity)) {
        const TilemapAsset* map = TilemapSystem::AssetOf(world, tilemapEntity);
        const TilePalette* palette = (map != nullptr) ? tilepalette::Resolve(map->palette) : nullptr;
        if (map != nullptr && palette != nullptr) {
            auto view = std::make_unique<GridNavView>();
            view->world = &world;
            view->tiles = &tiles;
            view->owner = tilemapEntity;
            view->map = map;
            view->palette = palette;
            const DirectX::XMFLOAT3 origin = TilemapSystem::OriginOf(world, tilemapEntity);
            view->originX = origin.x;
            view->originY = origin.y;
            view->cellW = std::max(map->cellSize[0], 1.0e-4f);
            view->cellH = std::max(map->cellSize[1], 1.0e-4f);
            for (size_t li = 0; li < map->layers.size(); ++li) {
                if (!map->layers[li].collision) {
                    continue;
                }
                const int32_t layer = static_cast<int32_t>(li);
                view->layers.push_back(layer);
                int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                if (!tiles.GetLayerBounds(world, tilemapEntity, layer, x0, y0, x1, y1)) {
                    continue;
                }
                if (!view->any) {
                    view->minX = x0;
                    view->minY = y0;
                    view->maxX = x1;
                    view->maxY = y1;
                    view->any = true;
                } else {
                    view->minX = std::min(view->minX, x0);
                    view->minY = std::min(view->minY, y0);
                    view->maxX = std::max(view->maxX, x1);
                    view->maxY = std::max(view->maxY, y1);
                }
            }
            entry.view = std::move(view);
        }
    }
    views_.push_back(std::move(entry));
    return views_.back().view.get();
}

void GridNavSystem::Update(World& world, const TilemapSystem* tiles, float dt)
{
    if (tiles == nullptr) {
        return;
    }
    agents_.clear();
    const ComponentTypeId req[] = { GridAgent2DComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (IsEntityActive(world, e)) {
                agents_.push_back(e);
            }
        }
    });
    if (agents_.empty()) {
        return;
    }
    // アーキタイプの走査順に依存しない: entity.index 昇順 (明示キー)
    std::sort(agents_.begin(), agents_.end(), [](EntityID a, EntityID b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    });
    views_.clear();
    for (const EntityID e : agents_) {
        const prof::ItemScope timing("gridnav", "grid agent", e.index, e.generation);
        UpdateAgent(world, *tiles, e, dt);
    }
    views_.clear(); // 作業バッファが World の資産を指したまま残らないようにする
}

void GridNavSystem::UpdateAgent(World& world, const TilemapSystem& tiles, EntityID e, float dt)
{
    GridAgent2DComponent* ga = world.GetComponent<GridAgent2DComponent>(e);
    CharacterControllerComponent* cc = world.GetComponent<CharacterControllerComponent>(e);
    if (ga == nullptr) {
        return;
    }
    GridNavView* view = (cc != nullptr && cc->plane2D) ? ViewFor(world, tiles, ga->tilemap) : nullptr;
    if (view == nullptr) {
        // CC が無い / 2D でない / Tilemap が引けない: 動かせない (CC には触らない)
        ga->status = navagentstatus::kInactive;
        ResetPlanState(*ga);
        return;
    }
    if (!ga->hasDestination) {
        if (ga->status != navagentstatus::kIdle) {
            cc->moveInput.x = 0.0f; // 目的地を取り下げられたら止める (Idle の間は CC に触らない)
            ga->status = navagentstatus::kIdle;
        }
        ResetPlanState(*ga);
        return;
    }

    float px = 0.0f, py = 0.0f, pz = 0.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    ComposeEntityWorldPose(world, e, px, py, pz, qx, qy, qz, qw);
    float sx = 1.0f, sy = 1.0f, sz = 1.0f;
    ComposeEntityWorldScale(world, e, sx, sy, sz);
    const float feetY = py - 0.5f * cc->height * std::fabs(sy);
    const int32_t curX = static_cast<int32_t>(std::floor((px - view->originX) / view->cellW));
    const int32_t curY = static_cast<int32_t>(std::floor((feetY - view->originY) / view->cellH + kFeetBias));
    const int32_t rawGoalX = static_cast<int32_t>(std::floor((ga->destination.x - view->originX) / view->cellW));
    const int32_t rawGoalY = static_cast<int32_t>(std::floor((ga->destination.y - view->originY) / view->cellH));
    const JumpParams jp{ std::max(ga->maxJumpHeight, 0), std::max(ga->maxJumpLength, 0) };
    const bool grounded = cc->isGrounded;

    const bool following = ga->hasPlan
                           && (ga->status == navagentstatus::kMoving || ga->status == navagentstatus::kStuck);
    const bool goalChanged = !ga->hasPlan || ga->goalCellX != rawGoalX || ga->goalCellY != rawGoalY;
    bool needPlan = goalChanged || (ga->replanInterval > 0 && ga->planAge >= ga->replanInterval);
    if (!needPlan && following) {
        const float nextCenterX = view->originX + (static_cast<float>(ga->nextCellX) + 0.5f) * view->cellW;
        const bool reached = curX == ga->nextCellX && curY == ga->nextCellY
                             && std::fabs(nextCenterX - px) <= GridNavSystem::kCellArriveTolerance && grounded;
        if (reached) {
            needPlan = true;
        } else if (grounded && !(curX == ga->nextCellX && curY == ga->nextCellY) && view->Walkable(curX, curY)) {
            Edge edge;
            needPlan = !EdgeBetween(*view, curX, curY, ga->nextCellX, ga->nextCellY, jp, edge);
        }
    }
    if (needPlan && !grounded) {
        // 空中では引き直さない (着地後に引く)。計画が無い間は待つ
        needPlan = false;
        if (!ga->hasPlan) {
            ga->status = navagentstatus::kMoving;
            cc->moveInput.x = 0.0f;
            return;
        }
    }

    if (needPlan) {
        const int32_t prevNextX = ga->nextCellX;
        const int32_t prevNextY = ga->nextCellY;
        const bool wasFollowing = following;
        ga->planAge = 0;
        ga->goalCellX = rawGoalX;
        ga->goalCellY = rawGoalY;
        ga->hasPlan = true;
        int32_t startX = 0, startY = 0, goalX = 0, goalY = 0;
        if (!NearestWalkable(*view, curX, curY, startX, startY) || !NearestWalkable(*view, rawGoalX, rawGoalY, goalX, goalY)) {
            ga->status = navagentstatus::kNoPath;
            cc->moveInput.x = 0.0f;
            return;
        }
        const SearchResult found = Search(*view, jp, startX, startY, goalX, goalY);
        if (found.kind == SearchKind::kNoPath) {
            ga->status = navagentstatus::kNoPath;
            cc->moveInput.x = 0.0f;
            return;
        }
        if (found.kind == SearchKind::kReached && !found.hasStep) {
            ga->status = navagentstatus::kArrived;
            ga->stuckTicks = 0;
            cc->moveInput.x = 0.0f;
            return;
        }
        // 進める: 同じセルを狙い続けるなら進展の記録を引き継ぐ
        const int32_t stepX = found.hasStep ? found.stepX : startX;
        const int32_t stepY = found.hasStep ? found.stepY : startY;
        if (!wasFollowing || stepX != prevNextX || stepY != prevNextY) {
            ga->stuckTicks = 0;
            ga->bestDistance = kUnsetDistance;
        }
        ga->nextCellX = stepX;
        ga->nextCellY = stepY;
        ga->status = navagentstatus::kMoving;
    } else {
        ++ga->planAge;
        if (ga->status != navagentstatus::kMoving && ga->status != navagentstatus::kStuck) {
            // Arrived / NoPath の待機 (目的地が変わるか interval が来るまで)
            cc->moveInput.x = 0.0f;
            return;
        }
    }

    // ---- 次のセルへ向かう ----
    const float nextCenterX = view->originX + (static_cast<float>(ga->nextCellX) + 0.5f) * view->cellW;
    const float nextBottomY = view->originY + static_cast<float>(ga->nextCellY) * view->cellH;
    const float dx = nextCenterX - px;
    const float limit = std::max(ga->speed, 0.0f);
    const float wanted = (dt > 0.0f) ? dx / dt : 0.0f;
    cc->moveInput.x = std::clamp(wanted, -limit, limit);

    if (grounded && cc->jumpSpeed <= 0.0f && view->Walkable(curX, curY)
        && !(curX == ga->nextCellX && curY == ga->nextCellY)) {
        Edge edge;
        if (EdgeBetween(*view, curX, curY, ga->nextCellX, ga->nextCellY, jp, edge) && edge.kind == EdgeKind::kJump) {
            cc->jumpSpeed = ga->jumpSpeed;
        }
    }

    // 進展: 次のセルへの距離が縮まっているか
    const float distance = std::fabs(dx) + std::fabs(nextBottomY - feetY);
    if (distance < ga->bestDistance - kProgressEpsilon) {
        ga->bestDistance = distance;
        ga->stuckTicks = 0;
    } else {
        ga->stuckTicks = std::min(ga->stuckTicks + 1, GridNavSystem::kStuckTicks);
    }
    ga->status = (ga->stuckTicks >= GridNavSystem::kStuckTicks) ? navagentstatus::kStuck : navagentstatus::kMoving;
}

} // namespace mye
