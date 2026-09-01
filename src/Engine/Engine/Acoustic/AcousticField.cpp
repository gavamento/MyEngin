//====================================================================================
//                          AcousticField.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響の場：占有ベイクと波面伝播（整数チャンファ Dijkstra）
//====================================================================================
#include "Engine/Engine/Acoustic/AcousticField.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "Engine/Core/Check.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
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

// 衝撃音の波の速さ [tick/リング]。足音と揃えてある (cellSize 0.5 なら 15 m/s)。
// 衝撃には発音要求を書くコンポーネントが無いので、ここが唯一の指定場所
constexpr uint32_t kImpactTicksPerRing = 2;

// CC の真下へレイを撃って床材を引く (M65c)。企画 3-4「踏んでいる床が音を決める」の入口。
// ★RaycastWorld は WorldMatrix 基準なので、フェーズ 3.4 では**1 tick 古い床**を見る。
//   足音のためのプローブなので実害は無い (60Hz で 1 tick ぶんの床の入れ替わりは知覚できない)。
// ★自分に当たったら**無音扱い**にする。CC と同じエンティティにソリッド Collider を
//   併用しているとレイの始点がその中に入るため — 足音が要るキャラは CC 単体で組むこと。
// ★材料が無い床は nullptr = 無音。「未割当は鳴らない」が存在ゲートの実体
const PhysMat* GroundMaterialUnder(World& world, EntityID self, float wx, float wy, float wz,
                                   float castDown)
{
    MyeRaycastHit hit = {};
    const MyeVec3 origin = { wx, wy, wz };
    const MyeVec3 down = { 0.0f, -1.0f, 0.0f };
    if (RaycastWorld(world, origin, down, castDown, &hit) == 0) {
        return nullptr;
    }
    if (hit.entity.index == self.index) {
        return nullptr;
    }
    const EntityID hitEntity = { hit.entity.index, hit.entity.generation };
    const auto* col = world.GetComponent<ColliderComponent>(hitEntity);
    return (col != nullptr) ? physmat::Resolve(col->physMaterial) : nullptr;
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
    ResetVisual(); // M65d: 残光もシーンと一緒に捨てる
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
    // M65h: 残光の見た目パラメータを鏡へ写す (描画レーン。範囲ガードは消費側 —
    // DecayVisual が (0,1) 外を既定へ倒し、強度は負だけここで落とす)
    glowKeepPerTick_ = bestVol.glowKeepPerTick;
    glowIntensity_ = (bestVol.glowIntensity > 0.0f) ? bestVol.glowIntensity : 0.0f;
    if (!acoustic::SameGrid(desc, grid_) || owner_ != best) {
        grid_ = desc;
        owner_ = best;
        derivedValid_ = false; // 形が変わったら占有も距離場も引き直し
        // M65d: 残光は「セル index -> ワールド位置」の対応がグリッドに紐づいているので、
        // 形が変わったら**再アドレスできない**。平行移動すら残光を歪めるだけなので捨てる
        // (追従グリッドを採らなかったのと同じ理由。計画 判断 4)
        ResetVisual();
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
    WriteShell(slot, w.ox, w.oy, w.oz, 0); // M65d: 音源セルそのものも残光に焼く
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
                // ★★M65d: 残光を焼くのは**ここ以外にない**。距離が確定したその場、
                //   同じ 1 ループの中で焼くから「音が届いた場所」と「光った場所」が
                //   構造的に一致する (企画 §3-1 の中核。別ループに切り出した瞬間に
                //   ずれる余地ができる)。max 合成なので後から短い距離で来ても正しい
                WriteShell(slot, nlx + f.x0, nly + f.y0, nlz + f.z0, nd);
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

void AcousticField::DrainEmitters(World& world, float dt, uint64_t tick)
{
    if (!grid_.Valid()) {
        return; // 存在ゲートの内側。ボリュームが無ければエミッタは走査すらしない
    }
    // 発音要求 1 件。**ポインタは書き戻しのハンドルにしか使わない** —
    // 並べ替えのキーは entity.index (規則 7。ポインタ値を sim に混ぜてはならない)
    struct Pending {
        EntityID entity = kNullEntity;
        AcousticEmitterComponent* comp = nullptr;
        CharacterControllerComponent* cc = nullptr; // 足音 (M65c)。無ければ歩かない
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
            // CC は同じアーキタイプに居るとは限らない (エミッタ単体の音源もある) ので
            // 型引きで取る。**居なければ足音は出ない** = autoFootstep の実質的なゲート
            p.cc = world.GetComponent<CharacterControllerComponent>(e);
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
        // ---- 足音 (M65c) ----
        // ★歩幅は**移動距離**で測る。時間で測る (n tick ごと) と、走っても歩いても
        //   同じ間隔で鳴って「速く動くほど音を出す」という企画の根っこが消える。
        // ★接地していないあいだは**溜めない**。空中で歩幅が溜まると、着地の瞬間に
        //   足音と衝撃音が二重に鳴る (着地音は DrainImpacts の担当)
        if (em.autoFootstep && em.stepDistanceM > 0.0f && p.cc != nullptr) {
            if (p.cc->isGrounded == 0) {
                em.travelAccum = 0.0f;
            } else {
                const float vx = p.cc->velocity.x;
                const float vz = p.cc->velocity.z;
                em.travelAccum += std::sqrt(vx * vx + vz * vz) * dt;
                if (em.travelAccum >= em.stepDistanceM) {
                    // 余りは持ち越す (切り捨てると速度によって歩幅が伸び縮みする)
                    em.travelAccum -= em.stepDistanceM;
                    const float castDown = p.cc->height * 0.5f + 0.4f;
                    const PhysMat* mat =
                        GroundMaterialUnder(world, p.entity, p.wx, p.wy, p.wz, castDown);
                    const float loud = SelectAcousticLoudness(mat);
                    // スクリプトが同じ tick に書いた要求を**踏み潰さない** (明示 > 自動)
                    if (loud > 0.0f && em.pendingLoudness <= 0.0f) {
                        // M65h: 速度段階の振幅係数 (企画 §3-2「走るほど大きい波」)。
                        // 振幅と到達距離の**両方**に掛ける — DrainImpacts の ImpactGain と
                        // 同じ流儀 (大きい音は明るいだけでなく遠くまで届く)。
                        // 0 以下は既定 1.0 (未設定の旧シーン。消音は autoFootstep で行う)
                        const float gain = (em.footstepGain > 0.0f) ? em.footstepGain : 1.0f;
                        em.pendingLoudness = loud * gain;
                        em.pendingRadiusM = SelectAcousticRadiusM(mat) * gain;
                        em.pendingTone = SelectAcousticTone(mat);
                    }
                }
            }
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

void AcousticField::DrainImpacts(World& world, const std::vector<SolidContact>& contacts, float dt,
                                 uint64_t tick)
{
    if (!grid_.Valid() || contacts.empty()) {
        return; // 存在ゲートの内側 (ボリュームが無ければ接触表を走査すらしない)
    }
    // 接触キーは entity.index の対。現世代のハンドルを引く表を**1 回だけ**組む
    // (CollisionSystem の resolve は接触 1 件ごとに全アーキタイプを線形走査するが、
    //  こちらは毎 tick 走るので表を作って二分探索する)。接触は必ずコライダ同士
    std::vector<std::pair<uint32_t, EntityID>> byIndex;
    const ComponentTypeId creq[] = { ColliderComponent::sTypeId };
    world.ForEachArchetype(creq, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            byIndex.emplace_back(e.index, e);
        }
    });
    std::sort(byIndex.begin(), byIndex.end(),
              [](const std::pair<uint32_t, EntityID>& a, const std::pair<uint32_t, EntityID>& b) {
                  return a.first < b.first;
              });
    const auto lookup = [&byIndex](uint32_t index) {
        const auto it = std::lower_bound(
            byIndex.begin(), byIndex.end(), index,
            [](const std::pair<uint32_t, EntityID>& p, uint32_t v) { return p.first < v; });
        return (it != byIndex.end() && it->first == index) ? it->second : kNullEntity;
    };

    // 「載っているだけ」の力積を測るための重力。★シーンの PhysicsEnvironment を見る —
    // 重力を強くしたシーンで固定 9.81 を使うと、静止した箱が閾値を超えて鳴り続ける
    const PhysicsEnvironmentComponent* env = ResolvePhysicsEnvironment(world);
    const float gx = (env != nullptr) ? env->gravity.x : 0.0f;
    const float gy = (env != nullptr) ? env->gravity.y : -9.81f; // ソルバ既定と同値
    const float gz = (env != nullptr) ? env->gravity.z : 0.0f;
    const float gMag = std::sqrt(gx * gx + gy * gy + gz * gz);

    // その接触が支えている**動的な質量**の合計 (kinematic / 静的は 0)
    const auto dynamicMass = [&world](EntityID e) {
        const auto* rb = world.GetComponent<RigidbodyComponent>(e);
        if (rb == nullptr || rb->isKinematic != 0) {
            return 0.0f;
        }
        return EffectiveMassWorld(world, e, *rb);
    };

    uint32_t emitted = 0;
    for (const SolidContact& c : contacts) { // ★key 昇順 (PhysicsSystem の出力規約)
        if (emitted >= kMaxImpactsPerTick) {
            break;
        }
        const EntityID ea = lookup(static_cast<uint32_t>(c.key >> 32));
        const EntityID eb = lookup(static_cast<uint32_t>(c.key & 0xFFFFFFFFu));
        if (ea.IsNull() || eb.IsNull()) {
            continue; // 1 tick 古い接触なので、消えたエンティティが混じりうる
        }
        const auto* ca = world.GetComponent<ColliderComponent>(ea);
        const auto* cb = world.GetComponent<ColliderComponent>(eb);
        const PhysMat* ma = (ca != nullptr) ? physmat::Resolve(ca->physMaterial) : nullptr;
        const PhysMat* mb = (cb != nullptr) ? physmat::Resolve(cb->physMaterial) : nullptr;
        // ★結合則は **max** (大きいほうが勝つ)。摩擦の sqrt 平均や反発の min とは違う —
        //   「金属板の上に落ちた木箱」は金属の音がするのであって、平均の音はしない。
        //   同値なら key の小さい側 (ea) を音源に採る = 決定論のタイブレーク
        const float la = SelectAcousticLoudness(ma);
        const float lb = SelectAcousticLoudness(mb);
        const PhysMat* mat = (lb > la) ? mb : ma;
        const EntityID src = (lb > la) ? eb : ea;
        const float loud = SelectAcousticLoudness(mat);
        if (loud <= 0.0f) {
            continue; // 材料未割当 = 無音 (存在ゲート)
        }
        const float rest = (dynamicMass(ea) + dynamicMass(eb)) * gMag * dt
                           * acoustic::kImpactRestingMargin;
        const float gain = acoustic::ImpactGain(c.impulse - rest);
        if (gain <= 0.0f) {
            continue;
        }
        // ★振幅と到達距離の**両方**を gain で縮める。振幅だけ縮めると、かすった音でも
        //   波の届く範囲は全開のまま = 敵に「小さいのに遠くまで届く音」が聞こえる
        if (Emit(src, c.px, c.py, c.pz, loud * gain, SelectAcousticRadiusM(mat) * gain,
                 static_cast<uint32_t>(SelectAcousticTone(mat)), kImpactTicksPerRing, tick)) {
            ++emitted;
        }
    }
}

bool AcousticField::TraceToOrigin(uint32_t slot, int32_t cx, int32_t cy, int32_t cz,
                                  int32_t& outX, int32_t& outY, int32_t& outZ) const
{
    if (slot >= kMaxWaves || waves_[slot].active == 0) {
        return false;
    }
    // 打ち切りは maxRing の 2 倍 + 余裕。斜めが混ざると 1 リングで 1 セルより多く進むので
    // 「リング数 = セル数」ではないが、必ず有限で終わることをこの上限が保証する
    const uint32_t limit = waves_[slot].maxRing * 2u + 8u;
    int32_t x = cx, y = cy, z = cz;
    for (uint32_t step = 0; step < limit; ++step) {
        const uint8_t dir = ParentDirAt(slot, x, y, z);
        if (dir == kNoParent) {
            // 親が無い = 音源セルそのもの (距離 0) に辿り着いた
            outX = x;
            outY = y;
            outZ = z;
            return DistanceAt(slot, x, y, z) == 0;
        }
        if (dir >= acoustic::kNeighborCount) {
            return false;
        }
        const acoustic::Neighbor& nb = acoustic::kNeighbors[dir];
        x += nb.dx;
        y += nb.dy;
        z += nb.dz;
    }
    return false; // 親の輪 = 距離場が壊れている (起きたら伝播側のバグ)
}

void AcousticField::DeliverArrivals(uint32_t slot, const std::vector<ListenerSite>& sites,
                                    uint64_t tick)
{
    const Wave& w = waves_[slot];
    const WaveField& f = fields_[slot];
    // 今のリングで確定した距離の範囲。バケット幅が面コストちょうどなので、
    // 「[(ring-1)*11, ring*11) に入っている = このリングで初めて確定した」が状態無しで言える
    const uint32_t hi = w.ring * acoustic::kFaceCost;
    const uint32_t lo = (w.ring > 0) ? (hi - acoustic::kFaceCost) : 0u;
    for (const ListenerSite& site : sites) {
        // ★自分が出した音を自分で聞かない。忘れると**全個体が永久に追跡状態**になる
        //   (自分の足音で自分を警戒し続ける)
        if (site.entity.index == w.source.index && site.entity.generation == w.source.generation) {
            continue;
        }
        const uint16_t d = DistanceAt(slot, site.cx, site.cy, site.cz);
        if (d == kUnreached || d < lo || d >= hi) {
            continue; // 未到達、または前のリングで既に配ってある
        }
        const float energy = acoustic::EnergyAt(d, f.maxDist, w.amplitude, grid_.cellSize);
        if (energy < site.mirror->threshold) {
            continue; // 閾値未満は「聞こえなかった」
        }
        // 音源の位置は**親方向を遡って**得る。距離場そのものを辿るので、角を曲がって
        // 届いた音でも「実際に音が通った道の先」が出る (企画 §6-3 と §3-1 が同じ配列から出る)
        int32_t ox = 0, oy = 0, oz = 0;
        if (!TraceToOrigin(slot, site.cx, site.cy, site.cz, ox, oy, oz)) {
            continue;
        }
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(grid_, ox, oy, oz, wx, wy, wz);
        AcousticListenerComponent& m = *site.mirror;
        // ★同じ tick に複数の波が届いたら**大きいほうが勝つ** (先着ではない)。
        //   先着だとスロット番号 = 発音順という実装都合が「どちらへ向かうか」を決めてしまう
        if (m.lastHeardTick == tick && m.lastLoudness >= energy) {
            continue;
        }
        m.lastHeardTick = tick;
        m.lastHeardPos = { wx, wy, wz };
        m.lastLoudness = energy;
        m.lastSourceEntity = w.source;
        m.lastTone = static_cast<int32_t>(w.tone);
    }
}

void AcousticField::Advance(World* world, uint64_t tick)
{
    if (!grid_.Valid()) {
        return;
    }
    MYE_PROFILE_SCOPE("acoustic.advance");
    // 聴者は**波を進める前に 1 回だけ**集める (波ごとに集め直すと N 倍の走査になる)。
    // entity.index 昇順 = 決定論のタイブレーク
    std::vector<ListenerSite> sites;
    if (world != nullptr) {
        const ComponentTypeId req[] = { AcousticListenerComponent::sTypeId,
                                        WorldMatrixComponent::sTypeId };
        world->ForEachArchetype(req, [&](Archetype& arch) {
            const int li = arch.FindTypeIndex(AcousticListenerComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(*world, e)) {
                    continue;
                }
                const auto& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))
                                     ->value;
                ListenerSite site;
                site.entity = e;
                if (!acoustic::WorldToCell(grid_, wm.m[3][0], wm.m[3][1], wm.m[3][2], site.cx,
                                           site.cy, site.cz)) {
                    continue; // ボリュームの外に居る聴者には何も届かない
                }
                site.mirror = static_cast<AcousticListenerComponent*>(arch.GetPtr(li, row));
                sites.push_back(site);
            }
        });
        std::sort(sites.begin(), sites.end(),
                  [](const ListenerSite& a, const ListenerSite& b) {
                      return a.entity.index < b.entity.index;
                  });
    }

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
        // ★**同じループの中で**配る。進めたばかりの dist 配列を、残光と同じ EnergyAt で
        //   読む — 「聞こえた場所」と「光った場所」が別式になる余地をここで潰している
        if (!sites.empty()) {
            DeliverArrivals(s, sites, tick);
        }
    }
}

// ---- 残光ボリューム (M65d) ---------------------------------------------------------
//
// ★**ここから下は描画レーンだけを触る**。sim 状態 (waves_) は 1 バイトも読まないし
//   書かない。だから glow_ が何であろうとワールドハッシュは動かず、
//   replay_verify / snapshot 往復に一切影響しない (AcousticSelfTest (16) が固定)。

void AcousticField::WriteShell(uint32_t slot, int32_t cx, int32_t cy, int32_t cz, uint32_t dist)
{
    const int64_t cells = grid_.CellCount();
    if (cells <= 0) {
        return;
    }
    // ★確保は初回の書き込みまで遅らせる。ボリュームを置いただけで一度も音が鳴っていない
    //   シーンでは 1 バイトも持たない (存在ゲートの延長)
    if (static_cast<int64_t>(glow_.size()) != cells) {
        glow_.assign(static_cast<size_t>(cells), 0u);
    }
    const Wave& w = waves_[slot];
    const float e = acoustic::EnergyAt(dist, fields_[slot].maxDist, w.amplitude, grid_.cellSize);
    const uint8_t v = acoustic::EncodeGlow(e);
    if (v == 0) {
        return; // 遠すぎて符号化しても 0 — 触らない (visualSerial_ を無駄に進めない)
    }
    uint8_t& dst = glow_[static_cast<size_t>(acoustic::CellIndex(grid_, cx, cy, cz))];
    if (v > dst) {
        dst = v;
        visualActive_ = true;
        ++visualSerial_;
    }
}

void AcousticField::DecayVisual(float perTick)
{
    if (!visualActive_ || glow_.empty()) {
        return; // 光っていないフレームは 130KB を舐めない
    }
    // 1 以上を渡されたら「減らない」= 永久に残る、になるので弾く (呼び手のバグ)
    const float keep = (perTick > 0.0f && perTick < 1.0f) ? perTick : acoustic::kGlowDecayPerTick;
    bool any = false;
    for (uint8_t& v : glow_) {
        if (v == 0) {
            continue;
        }
        // ★切り捨てなので keep < 1 なら必ず 1 以上減る = 単調に 0 へ届く。
        //   四捨五入にすると v=255,keep=0.999 が 255 のまま止まって**永久に消えない**
        v = static_cast<uint8_t>(static_cast<float>(v) * keep);
        any = any || (v != 0);
    }
    visualActive_ = any;
    ++visualSerial_;
}

void AcousticField::ResetVisual()
{
    if (!glow_.empty()) {
        glow_.clear();
        glow_.shrink_to_fit();
    }
    visualActive_ = false;
    ++visualSerial_;
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
