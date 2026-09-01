//====================================================================================
//                          AcousticField.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響の場：ボリューム探索と静的コライダの占有ベイク
//====================================================================================
#include "Engine/Engine/Acoustic/AcousticField.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Check.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/Shapes.h"

namespace mye {
namespace {

// 署名は Core の FNV-1a (Hash.h) をそのまま使う。**ワールドハッシュ対象ではない**
// (占有は導出値) が、「焼き直すかどうか」を決める値なので構成に依らず同じでなければ
// ならない。
// ★構造体の生バイトを丸ごと混ぜないこと — パディングは未初期化なので構成で違いうる。
//   スカラーを 1 つずつ畳む (WorldHasher がフィールド単位で畳んでいるのと同じ理由)
uint64_t FoldU64(uint64_t h, uint64_t v)
{
    return HashCombine(h, v);
}

uint64_t FoldF32(uint64_t h, float v)
{
    return HashBytes(&v, sizeof(float), h); // float は**ビットパターン**で畳む
}

// 音を遮る候補 1 件。ShapePose は **meshData に生ポインタを持つ**ので署名には使えない —
// 署名はコンポーネントのフィールドとワールド行列から作り、ポーズは判定にだけ使う
struct Blocker {
    EntityID entity = kNullEntity;
    ColliderComponent col;
    DirectX::XMFLOAT4X4 wm = {};
};

uint64_t FoldBlocker(uint64_t h, const Blocker& b)
{
    h = FoldU64(h, b.entity.index);
    h = FoldU64(h, b.entity.generation);
    h = FoldU64(h, static_cast<uint64_t>(static_cast<uint32_t>(b.col.shape)));
    h = FoldF32(h, b.col.radius);
    h = FoldF32(h, b.col.halfExtents.x);
    h = FoldF32(h, b.col.halfExtents.y);
    h = FoldF32(h, b.col.halfExtents.z);
    h = FoldF32(h, b.col.height);
    h = FoldU64(h, static_cast<uint64_t>(static_cast<uint32_t>(b.col.layer)));
    h = FoldU64(h, b.col.meshAsset.value);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            h = FoldF32(h, b.wm.m[r][c]);
        }
    }
    return h;
}

} // namespace

AcousticField::AcousticField()
{
    waves_.assign(kMaxWaves, Wave{});
}

void AcousticField::Reset()
{
    grid_ = AcousticGridDesc{};
    owner_ = kNullEntity;
    navRatio_ = 2;
    occupancy_.clear();
    occupancy_.shrink_to_fit();
    staticSig_ = 0;
    derivedValid_ = false;
    waves_.assign(kMaxWaves, Wave{});
}

bool AcousticField::IsSolid(int32_t cx, int32_t cy, int32_t cz) const
{
    if (!acoustic::InBounds(grid_, cx, cy, cz)) {
        return true; // グリッド外は壁扱い — 波が箱の外へ漏れないことを型で保証する
    }
    const int64_t i = acoustic::CellIndex(grid_, cx, cy, cz);
    return occupancy_[static_cast<size_t>(i)] != 0;
}

bool AcousticField::AnyWaveActive() const
{
    for (const Wave& w : waves_) {
        if (w.active != 0) {
            return true;
        }
    }
    return false;
}

void AcousticField::DebugSetGrid(const AcousticGridDesc& grid, std::vector<uint8_t> occupancy)
{
    MYE_CHECK(grid.Valid());
    MYE_CHECK(static_cast<int64_t>(occupancy.size()) == grid.CellCount());
    grid_ = grid;
    occupancy_ = std::move(occupancy);
    owner_ = kNullEntity;
    staticSig_ = 0;
    derivedValid_ = true; // セルフテストは Sync を呼ばないので「整合済み」を主張しておく
}

void AcousticField::Sync(World& world)
{
    // ---- 存在ゲート: active な AcousticVolume を entity.index 最小で 1 個だけ採る ----
    // ここで 0 件なら**占有配列を resize すらしない**。既存シーンのハッシュと golden が
    // 1 ビットも動かない根拠は、この 1 ブロックに集約されている
    const ComponentTypeId req[] = { AcousticVolumeComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    EntityID best = kNullEntity;
    AcousticVolumeComponent bestVol;
    DirectX::XMFLOAT4X4 bestWm = {};
    int volumeCount = 0;
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int vi = arch.FindTypeIndex(AcousticVolumeComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            const auto* vol = static_cast<const AcousticVolumeComponent*>(arch.GetPtr(vi, row));
            if (!vol->enabled || !IsEntityActive(world, e)) {
                continue;
            }
            ++volumeCount;
            // アーキタイプの列挙順は生成順であって index 昇順ではない。
            // **明示的な決定論キー (entity.index 最小) で選ぶ** (規則 7)
            if (best.IsNull() || e.index < best.index) {
                best = e;
                bestVol = *vol;
                bestWm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            }
        }
    });
    if (best.IsNull()) {
        if (grid_.Valid()) {
            Reset(); // ボリュームが消えた = 場ごと畳む
        }
        return;
    }
    if (volumeCount > 1) {
        MYE_LOG_WARN("[acoustic] %d volumes in the scene; using entity %u (lowest index)",
                     volumeCount, best.index);
    }

    // ---- グリッドを組む。原点はボリュームのワールド位置 (回転は無視 = 常に軸平行) ----
    AcousticGridDesc desc;
    const bool ok =
        acoustic::MakeGridDesc(bestVol.dimX, bestVol.dimY, bestVol.dimZ, bestVol.cellSize,
                               bestWm.m[3][0], bestWm.m[3][1], bestWm.m[3][2], desc);
    if (!ok) {
        if (grid_.Valid()) {
            Reset();
        }
        return;
    }
    navRatio_ = std::clamp(bestVol.navCellRatio, 1, 8);
    if (!acoustic::SameGrid(desc, grid_) || owner_ != best) {
        grid_ = desc;
        owner_ = best;
        derivedValid_ = false; // 形が変わったら占有も距離場も引き直し
    }

    // ---- 静的コライダの署名。変化していなければ焼き直さない ----
    MYE_PROFILE_SCOPE("acoustic.sync");
    std::vector<Blocker> blockers;
    const ComponentTypeId creq[] = { ColliderComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(creq, [&](Archetype& arch) {
        // 動く物 (Rigidbody / CharacterController) は**遮蔽に入れない**。入れると
        // 毎 tick 署名が変わって全再ベイクが走り、フレームが丸ごと溶ける。
        // ドアや箱を遮蔽に載せるのは「静的ベイク + 動的オーバーレイ」の二層化が要る
        // (計画の「実装しない / できないもの」)
        if (arch.HasType(RigidbodyComponent::sTypeId)
            || arch.HasType(CharacterControllerComponent::sTypeId)) {
            return;
        }
        const int ci = arch.FindTypeIndex(ColliderComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* col = static_cast<const ColliderComponent*>(arch.GetPtr(ci, row));
            // トリガーは音を遮らない (「通り抜けられる場所」の目印として置かれるものなので)
            if (col->isTrigger || !shapes::LayerHit(bestVol.blockLayerMask, col->layer)) {
                continue;
            }
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            Blocker b;
            b.entity = e;
            b.col = *col;
            b.wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            blockers.push_back(b);
        }
    });
    // ★明示的な決定論キーで並べる。アーキタイプ列挙順のままだと、同じシーンでも
    //   エンティティを作った順で署名が変わる
    std::sort(blockers.begin(), blockers.end(),
              [](const Blocker& a, const Blocker& b) { return a.entity.index < b.entity.index; });

    uint64_t sig = kFnvOffset;
    sig = FoldU64(sig, blockers.size());
    for (const Blocker& b : blockers) {
        sig = FoldBlocker(sig, b);
    }
    if (derivedValid_ && sig == staticSig_) {
        return; // 何も変わっていない
    }
    staticSig_ = sig;
    BakeOccupancy(world, bestVol.blockLayerMask);
}

void AcousticField::BakeOccupancy(World& world, uint32_t blockLayerMask)
{
    // 署名を作った Sync が既に候補を集めているが、ここでもう一度集め直しているのは
    // 「焼く条件」と「署名の条件」が将来ずれないようにするため — 片方だけ直すと
    // 「署名は変わらないのに occupancy が変わる」= 焼き直されない静かな壊れ方になる。
    // 走査は毎 tick ではなく署名が変わった tick だけなので、二度手間の代償は無い
    occupancy_.assign(static_cast<size_t>(grid_.CellCount()), 0u);
    const float half = grid_.cellSize * 0.5f;
    std::vector<Blocker> blockers;
    const ComponentTypeId creq[] = { ColliderComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(creq, [&](Archetype& arch) {
        if (arch.HasType(RigidbodyComponent::sTypeId)
            || arch.HasType(CharacterControllerComponent::sTypeId)) {
            return;
        }
        const int ci = arch.FindTypeIndex(ColliderComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* col = static_cast<const ColliderComponent*>(arch.GetPtr(ci, row));
            if (col->isTrigger || !shapes::LayerHit(blockLayerMask, col->layer)) {
                continue;
            }
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            Blocker b;
            b.entity = e;
            b.col = *col;
            b.wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            blockers.push_back(b);
        }
    });
    std::sort(blockers.begin(), blockers.end(),
              [](const Blocker& a, const Blocker& b) { return a.entity.index < b.entity.index; });

    ShapePose cell;
    cell.shape = 1; // box。無回転なので M20 互換の AABB fast-path を通る
    cell.identityRot = 1;
    cell.hx = half;
    cell.hy = half;
    cell.hz = half;
    for (const Blocker& b : blockers) {
        // shape=3/4/5 (メッシュ / 地形 / 凸包) は meshcol / terraincol / convexcol の実体
        // 注入が要る。ここでは注入しないので meshData が null のまま = 全判定が
        // 「衝突なし」に落ちる (安全側)。壁は box / sphere / capsule で組む前提
        const ShapePose pose = shapes::MakePoseFromMatrix(b.col, b.wm);
        float minX, minY, minZ, maxX, maxY, maxZ;
        shapes::ComputeAabb(pose, minX, minY, minZ, maxX, maxY, maxZ);
        // ★AABB をセル範囲へ落としてから形状判定に掛ける。全セル x 全コライダを回すと
        //   52 万 x 数百で即死する。half を足して広げるのは、セル**中心**が AABB の外でも
        //   セルの箱は重なりうるため
        const auto lo = [&](float v, float minv, int32_t dim) {
            const float f = std::floor((v - half - minv) / grid_.cellSize);
            return f < 0.0f ? 0 : (f >= static_cast<float>(dim) ? dim : static_cast<int32_t>(f));
        };
        const auto hi = [&](float v, float minv, int32_t dim) {
            const float f = std::floor((v + half - minv) / grid_.cellSize);
            return f < 0.0f ? -1 : (f >= static_cast<float>(dim) ? dim - 1 : static_cast<int32_t>(f));
        };
        const int32_t x0 = lo(minX, grid_.minX, grid_.dimX);
        const int32_t y0 = lo(minY, grid_.minY, grid_.dimY);
        const int32_t z0 = lo(minZ, grid_.minZ, grid_.dimZ);
        const int32_t x1 = hi(maxX, grid_.minX, grid_.dimX);
        const int32_t y1 = hi(maxY, grid_.minY, grid_.dimY);
        const int32_t z1 = hi(maxZ, grid_.minZ, grid_.dimZ);
        for (int32_t cz = z0; cz <= z1; ++cz) {
            for (int32_t cy = y0; cy <= y1; ++cy) {
                for (int32_t cx = x0; cx <= x1; ++cx) {
                    const int64_t idx = acoustic::CellIndex(grid_, cx, cy, cz);
                    if (occupancy_[static_cast<size_t>(idx)] != 0) {
                        continue; // もう閉じている
                    }
                    acoustic::CellToWorldCenter(grid_, cx, cy, cz, cell.px, cell.py, cell.pz);
                    if (shapes::Overlap(cell, pose)) {
                        occupancy_[static_cast<size_t>(idx)] = 1u;
                    }
                }
            }
        }
    }
    derivedValid_ = true;
}

} // namespace mye
