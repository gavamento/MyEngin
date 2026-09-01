//====================================================================================
//                          AcousticField.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響の場：占有ベイクと波面伝播（整数チャンファ Dijkstra）
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
    fields_.assign(kMaxWaves, WaveField{});
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
    fields_.assign(kMaxWaves, WaveField{});
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
    Rebuild();            // 占有が変わった = 距離場は引き直し (Sync の末尾と同じ扱い)
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
    // ★シーン構築直後の 1 tick だけは**必ず 2 回焼く**。フェーズ 3.4 は TransformSystem
    //   (フェーズ 4) より前なので、最初の tick では全コライダの WorldMatrix が既定値の
    //   まま = 全部が原点に重なった署名になり、次の tick で本物の配置に変わるため。
    //   決定論的な 2 回なので害は無い (実測: --acoustic-demo で 4 セル → 8064 セル)
    staticSig_ = sig;
    BakeOccupancy(world, bestVol.blockLayerMask);
    // ★占有が変わったら距離場は**全部引き直す**。増分で育った波は古い占有で育っている
    //   ので、ここを通さないと「巻き戻すと波の形が変わる」= 最悪の型のバグになる。
    //   snapshot 復元後の Invalidate() もこの経路 (derivedValid_=false) でここへ来る
    Rebuild();
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

// ---- 波面伝播 (M65b) ---------------------------------------------------------------
//
// Dial 法 (バケット付き Dijkstra)。26 近傍 x Borgefors の整数重み <11,16,19> なので
// キーが 0..maxDist の整数に収まり、優先度キューが要らない。
//
// ★**キーが整数であることが決定論の核心**。物理の float は 1 ulp ずれても剛体が
//   微動するだけだが、伝播は順序比較が 1 ulp ずれると訪問順が入れ替わり、親リンクが
//   変わり、AI が聞く方向が変わる。規約は「順序を決めるものは全部整数。float は
//   整数から導く末端の 1 式 (EnergyAt) だけ」。
//
// メモリの見積り (kMaxWaves = 16 の根拠):
//   局所ボックスは軸ごとに **min(2*maxRing+1, dim)** セル。★グリッドでクリップされる
//   のが効いていて、既定ボリューム (64x16x64 = 65,536 セル) なら 1 波 196 KB
//   (uint16 dist + uint8 parentDir = 3 B/セル) → 16 本で 3.1 MB。
//   上限が出るので固定本数で持ってよい (プールも LRU も要らない = 隠れた状態が無い)。
// 重くなったときの縮退はこの順で (絵の劣化が小さい順):
//   (1) ticksPerRing を上げる (2) cellSize 0.5→0.75 (セル数は 1/s^3)
//   (3) kMaxWaves を下げる (4) 26→6 近傍 ← **波が菱形になるので最後の手段**

namespace {

// 局所ボックス内の線形 index。**x が最速** (acoustic::CellIndex と同じ並び)
inline int32_t LocalIndex(const AcousticField::WaveField& f, int32_t lx, int32_t ly, int32_t lz)
{
    return (lz * f.sy + ly) * f.sx + lx;
}

} // namespace

void AcousticField::SeedWave(uint32_t slot)
{
    const Wave& w = waves_[slot];
    WaveField& f = fields_[slot];

    // 面コストが 11 = 1 リングぶんなので、軸方向に maxRing セルより遠いセルは
    // 必ず maxRing*11 を超える。だから箱は原点 ± maxRing で足りる (証明付きの切り詰め)
    const int32_t r = static_cast<int32_t>(w.maxRing);
    const int32_t x0 = (std::max)(0, w.ox - r);
    const int32_t y0 = (std::max)(0, w.oy - r);
    const int32_t z0 = (std::max)(0, w.oz - r);
    const int32_t x1 = (std::min)(grid_.dimX - 1, w.ox + r);
    const int32_t y1 = (std::min)(grid_.dimY - 1, w.oy + r);
    const int32_t z1 = (std::min)(grid_.dimZ - 1, w.oz + r);
    f.x0 = x0;
    f.y0 = y0;
    f.z0 = z0;
    f.sx = x1 - x0 + 1;
    f.sy = y1 - y0 + 1;
    f.sz = z1 - z0 + 1;
    f.maxDist = w.maxRing * acoustic::kFaceCost;

    const size_t n = static_cast<size_t>(f.sx) * static_cast<size_t>(f.sy)
                   * static_cast<size_t>(f.sz);
    f.dist.assign(n, kUnreached);
    f.parentDir.assign(n, kNoParent);
    // バケットは capacity を捨てずに使い回す (波は毎秒何本も立つ)
    f.buckets.resize(static_cast<size_t>(f.maxDist) + 1);
    for (std::vector<int32_t>& b : f.buckets) {
        b.clear();
    }

    const int32_t li = LocalIndex(f, w.ox - x0, w.oy - y0, w.oz - z0);
    f.dist[static_cast<size_t>(li)] = 0;
    f.buckets[0].push_back(li);
}

void AcousticField::AdvanceWaveOneRing(uint32_t slot)
{
    Wave& w = waves_[slot];
    WaveField& f = fields_[slot];

    // 1 リング = チャンファ距離 11 ぶん。[ring*11, (ring+1)*11) のバケットを昇順に空にする。
    // ★relax の挿入先は必ず d+11 以上 = 現区間の**外**なので、処理済みバケットへ
    //   書き戻されることが構造的に起きない (これが「1 リング = 幅 11」を選んだ理由)
    const uint32_t lo = w.ring * acoustic::kFaceCost;
    const uint32_t hi = lo + acoustic::kFaceCost; // 排他
    for (uint32_t d = lo; d < hi && d <= f.maxDist; ++d) {
        std::vector<int32_t>& bucket = f.buckets[d];
        for (size_t k = 0; k < bucket.size(); ++k) {
            const int32_t li = bucket[k];
            if (f.dist[static_cast<size_t>(li)] != static_cast<uint16_t>(d)) {
                continue; // 後からより短い距離で上書きされた古いエントリ
            }
            const int32_t lx = li % f.sx;
            const int32_t ly = (li / f.sx) % f.sy;
            const int32_t lz = li / (f.sx * f.sy);
            for (int i = 0; i < acoustic::kNeighborCount; ++i) {
                const acoustic::Neighbor& nb = acoustic::kNeighbors[i];
                const int32_t nlx = lx + nb.dx;
                const int32_t nly = ly + nb.dy;
                const int32_t nlz = lz + nb.dz;
                if (nlx < 0 || nlx >= f.sx || nly < 0 || nly >= f.sy || nlz < 0 || nlz >= f.sz) {
                    continue; // 箱の外 = この波では必ず maxDist 超え
                }
                const uint32_t nd = d + nb.cost;
                if (nd > f.maxDist) {
                    continue;
                }
                const int32_t ni = LocalIndex(f, nlx, nly, nlz);
                if (nd >= f.dist[static_cast<size_t>(ni)]) {
                    continue;
                }
                // 閉セルは**絶対に訪れない**。だから壁面は「開セルと閉セルの境界」に
                // 現れる — これが「壁を貫通せず、当たった面だけが光る」の正体。
                // ★中間セルは見ない (= 斜めの角抜けを許す)。凸角を 11+11 ではなく 16 で
                //   曲がれるので回折らしく見えるうえ、内側ループの IsSolid が 26 回で
                //   済む (中間セル判定を入れると最大 74 回 = 伝播の主コストが 3 倍)。
                //   代償は「斜め 1 セル厚の壁」が漏れること。壁は box で組む前提なので
                //   ボクセル化すると必ず軸平行 1 セル厚以上になり、実際には起きない
                //   (AcousticSelfTest (8b) が軸平行の 1 セル厚で漏れないことを固定)。
                //   塞ぐ必要が出たら「面隣接の中間セルが 1 つでも開いていること」を条件に足す
                if (IsSolid(nlx + f.x0, nly + f.y0, nlz + f.z0)) {
                    continue;
                }
                f.dist[static_cast<size_t>(ni)] = static_cast<uint16_t>(nd);
                // ★親は「隣から自分へ戻る向き」。表を (dz,dy,dx) 辞書順で中心だけ
                //   抜いて並べてあるので反転は 25-i で求まる (表が要らない)
                f.parentDir[static_cast<size_t>(ni)] =
                    acoustic::OppositeNeighbor(static_cast<uint8_t>(i));
                f.buckets[nd].push_back(ni);
            }
        }
        bucket.clear();
    }
    ++w.ring;
}

void AcousticField::Rebuild()
{
    if (!grid_.Valid()) {
        for (WaveField& f : fields_) {
            f = WaveField{};
        }
        return;
    }
    for (uint32_t s = 0; s < kMaxWaves; ++s) {
        Wave& w = waves_[s];
        if (w.active == 0) {
            fields_[s] = WaveField{};
            continue;
        }
        // ★ここが本システムで最も重要な不変条件。ring 0 から target まで
        //   AdvanceWaveOneRing を回すのは、増分で育てたときと**同じ関数を同じ回数**
        //   同じ入力で呼ぶということ。だから結果は memcmp で一致する。
        //   逆に「途中で占有が変わった波」は増分側が古い占有で育っているので一致しない —
        //   だから占有を焼き直したら必ずここを通す (Sync の末尾)
        const uint32_t target = w.ring;
        w.ring = 0;
        SeedWave(s);
        while (w.ring < target) {
            AdvanceWaveOneRing(s);
        }
    }
}

bool AcousticField::Emit(EntityID source, float wx, float wy, float wz, float loudness,
                         float radiusM, uint32_t tone, uint32_t ticksPerRing, uint64_t tick)
{
    if (!grid_.Valid() || loudness <= 0.0f || radiusM <= 0.0f) {
        return false;
    }
    int32_t cx = 0, cy = 0, cz = 0;
    if (!acoustic::WorldToCell(grid_, wx, wy, wz, cx, cy, cz)) {
        return false; // ボリュームの外で鳴った音はこの場では表現しない
    }
    if (IsSolid(cx, cy, cz)) {
        // 足元が床コライダの中、という状況は普通に起きる。26 近傍を**表の順**に見て
        // 最初の開セルへ寄せる (順序が決まっているので機種に依らない)
        bool moved = false;
        for (int i = 0; i < acoustic::kNeighborCount; ++i) {
            const acoustic::Neighbor& nb = acoustic::kNeighbors[i];
            if (!IsSolid(cx + nb.dx, cy + nb.dy, cz + nb.dz)) {
                cx += nb.dx;
                cy += nb.dy;
                cz += nb.dz;
                moved = true;
                break;
            }
        }
        if (!moved) {
            return false; // 完全に埋まっている
        }
    }

    // ★満杯なら**最古を潰さず false を返す**。潰す実装にすると「満杯時にどの波が
    //   生き残るか」が到着順に依存し、しかも .rep には現れない形で挙動が変わる
    uint32_t slot = kMaxWaves;
    for (uint32_t s = 0; s < kMaxWaves; ++s) {
        if (waves_[s].active == 0) {
            slot = s; // **最小 index の空き** = 決定論のタイブレーク
            break;
        }
    }
    if (slot == kMaxWaves) {
        return false;
    }

    // 到達上限。★float→int の変換はこの 1 式に閉じる (**切り捨て**)。
    //   順序を決める比較には float を一切使わないので、ここが唯一の境界
    const uint32_t rings = static_cast<uint32_t>(radiusM / grid_.cellSize);
    Wave& w = waves_[slot];
    w = Wave{};
    w.active = 1;
    w.source = source;
    w.ox = cx;
    w.oy = cy;
    w.oz = cz;
    w.ring = 0;
    w.maxRing = std::clamp(rings, 1u, acoustic::kMaxWaveRing);
    w.ticksPerRing = std::clamp(ticksPerRing, 1u, 64u);
    w.phase = 0;
    w.amplitude = loudness;
    w.tone = tone;
    w.bornTick = tick;
    SeedWave(slot);
    return true;
}

void AcousticField::DrainEmitters(World& world, uint64_t tick)
{
    if (!grid_.Valid()) {
        return; // 存在ゲートの内側。ボリュームが無ければエミッタは走査すらしない
    }
    // 発音要求 1 件。**ポインタは書き戻しのハンドルにしか使わない** —
    // 並べ替えのキーは entity.index (規則 7。ポインタ値を sim に混ぜてはならない)
    struct Pending {
        EntityID entity = kNullEntity;
        AcousticEmitterComponent* comp = nullptr;
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    };
    std::vector<Pending> pend;
    const ComponentTypeId req[] = { AcousticEmitterComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ei = arch.FindTypeIndex(AcousticEmitterComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            Pending p;
            p.entity = e;
            p.comp = static_cast<AcousticEmitterComponent*>(arch.GetPtr(ei, row));
            const auto& wm =
                static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            p.wx = wm.m[3][0];
            p.wy = wm.m[3][1];
            p.wz = wm.m[3][2];
            pend.push_back(p);
        }
    });
    std::sort(pend.begin(), pend.end(),
              [](const Pending& a, const Pending& b) { return a.entity.index < b.entity.index; });

    for (const Pending& p : pend) {
        AcousticEmitterComponent& em = *p.comp;
        if (em.cooldown > 0) {
            --em.cooldown;
        }
        if (em.pendingLoudness <= 0.0f) {
            continue;
        }
        if (em.cooldown > 0) {
            em.pendingLoudness = 0.0f; // クールダウン中の要求は落とす (溜め込まない)
            continue;
        }
        const uint32_t tone = static_cast<uint32_t>(std::clamp(em.pendingTone, 0, 3));
        const uint32_t tpr = static_cast<uint32_t>(std::clamp(em.ticksPerRing, 1, 64));
        if (Emit(p.entity, p.wx, p.wy, p.wz, em.pendingLoudness, em.pendingRadiusM, tone, tpr,
                 tick)) {
            em.cooldown = (std::max)(0, em.cooldownTicks);
        }
        em.pendingLoudness = 0.0f;
        em.pendingRadiusM = 0.0f;
    }
}

void AcousticField::Advance()
{
    if (!grid_.Valid()) {
        return;
    }
    MYE_PROFILE_SCOPE("acoustic.advance");
    for (uint32_t s = 0; s < kMaxWaves; ++s) {
        Wave& w = waves_[s];
        if (w.active == 0) {
            continue;
        }
        ++w.phase;
        if (w.phase < w.ticksPerRing) {
            continue; // 分周中。伝播速度 = cellSize * 60 / ticksPerRing [m/s]
        }
        w.phase = 0;
        if (w.ring >= w.maxRing) {
            // 最終リングを ticksPerRing のあいだ見せてから消す
            w = Wave{};
            fields_[s] = WaveField{};
            continue;
        }
        AdvanceWaveOneRing(s);
    }
}

uint16_t AcousticField::DistanceAt(uint32_t slot, int32_t cx, int32_t cy, int32_t cz) const
{
    if (slot >= kMaxWaves || waves_[slot].active == 0) {
        return kUnreached;
    }
    const WaveField& f = fields_[slot];
    const int32_t lx = cx - f.x0;
    const int32_t ly = cy - f.y0;
    const int32_t lz = cz - f.z0;
    if (lx < 0 || lx >= f.sx || ly < 0 || ly >= f.sy || lz < 0 || lz >= f.sz) {
        return kUnreached;
    }
    return f.dist[static_cast<size_t>(LocalIndex(f, lx, ly, lz))];
}

uint8_t AcousticField::ParentDirAt(uint32_t slot, int32_t cx, int32_t cy, int32_t cz) const
{
    if (slot >= kMaxWaves || waves_[slot].active == 0) {
        return kNoParent;
    }
    const WaveField& f = fields_[slot];
    const int32_t lx = cx - f.x0;
    const int32_t ly = cy - f.y0;
    const int32_t lz = cz - f.z0;
    if (lx < 0 || lx >= f.sx || ly < 0 || ly >= f.sy || lz < 0 || lz >= f.sz) {
        return kNoParent;
    }
    return f.parentDir[static_cast<size_t>(LocalIndex(f, lx, ly, lz))];
}
} // namespace mye
