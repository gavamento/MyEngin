//====================================================================================
//                          AcousticSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響伝播のヘッドレス回帰テスト（M65a 分）
//====================================================================================
#include "Engine/Engine/Acoustic/AcousticSelfTest.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/Core/Check.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/Acoustic/AcousticGrid.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/TransformSystem.h"

namespace mye {
namespace {

// 8x4x8 / 0.5m の小さな箱を原点中心に置く (テストの共通土台)
AcousticGridDesc MakeTestGrid()
{
    AcousticGridDesc g;
    const bool ok = acoustic::MakeGridDesc(8, 4, 8, 0.5f, 0.0f, 0.0f, 0.0f, g);
    MYE_CHECK(ok);
    return g;
}

// 占有セル数を数える
size_t CountSolid(const AcousticField& f)
{
    size_t n = 0;
    for (uint8_t v : f.Occupancy()) {
        n += (v != 0) ? 1u : 0u;
    }
    return n;
}

} // namespace

bool RunAcousticSelfTest()
{
    MYE_LOG_INFO("==== Acoustic self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (1) グリッドの座標変換 ----
    {
        const AcousticGridDesc g = MakeTestGrid();
        check(g.Valid() && g.CellCount() == 8 * 4 * 8, "grid: dims and cell count");
        // 中心を与えて最小角を導く規約 (箱の中心が center、セル中心ではない)
        check(g.minX == -2.0f && g.minY == -1.0f && g.minZ == -2.0f, "grid: min corner is derived from the centre");

        // セル中心 -> セル座標の往復が全セルで恒等
        bool roundTrip = true;
        for (int32_t cz = 0; cz < g.dimZ && roundTrip; ++cz) {
            for (int32_t cy = 0; cy < g.dimY && roundTrip; ++cy) {
                for (int32_t cx = 0; cx < g.dimX && roundTrip; ++cx) {
                    float wx, wy, wz;
                    acoustic::CellToWorldCenter(g, cx, cy, cz, wx, wy, wz);
                    int32_t bx, by, bz;
                    if (!acoustic::WorldToCell(g, wx, wy, wz, bx, by, bz) || bx != cx || by != cy
                        || bz != cz) {
                        roundTrip = false;
                    }
                }
            }
        }
        check(roundTrip, "grid: cell centre round-trips back to the same cell");

        // 境界は「最小角を含み、最大角を含まない」半開区間。
        // ここを閉区間にすると最大角が dim 番目のセルへ落ちて範囲外書き込みになる
        int32_t bx, by, bz;
        check(acoustic::WorldToCell(g, g.minX, g.minY, g.minZ, bx, by, bz) && bx == 0 && by == 0
                  && bz == 0,
              "grid: the min corner belongs to cell 0");
        const float maxX = g.minX + static_cast<float>(g.dimX) * g.cellSize;
        check(!acoustic::WorldToCell(g, maxX, 0.0f, 0.0f, bx, by, bz),
              "grid: the max corner is outside (half-open)");
        check(!acoustic::WorldToCell(g, g.minX - 0.01f, 0.0f, 0.0f, bx, by, bz),
              "grid: just outside the min corner is rejected");
        // 負側は切り捨てではなく floor でなければ 2 セルが同じ index に落ちる
        check(acoustic::WorldToCell(g, -0.25f, 0.0f, 0.0f, bx, by, bz) && bx == 3,
              "grid: negative offsets use floor, not truncation");

        // 線形 index は x が最内
        check(acoustic::CellIndex(g, 1, 0, 0) == 1 && acoustic::CellIndex(g, 0, 1, 0) == g.dimX
                  && acoustic::CellIndex(g, 0, 0, 1) == static_cast<int64_t>(g.dimX) * g.dimY,
              "grid: linear index runs x fastest");
    }

    // ---- (2) チャンファ重み ----
    // 3 つの正準方向について「チャンファ距離 / 真の距離」を出す。完全な球ならこの 3 つが
    // 一致するので、**ばらつきがそのまま波の角張り**になる。
    // ★比べるのは 16/11 と sqrt(2) ではない — チャンファ距離は「真の距離の s 倍」の近似で、
    //   s は自由に選べる。s を最適に取ったときの最大相対誤差が、この重み表の実力
    {
        const double face = 11.0 / 1.0;
        const double edge = 16.0 / std::sqrt(2.0);
        const double corner = 19.0 / std::sqrt(3.0);
        const double lo = std::min(std::min(face, edge), corner);
        const double hi = std::max(std::max(face, edge), corner);
        const double best = std::sqrt(lo * hi); // 上下の誤差を釣り合わせる最適スケール
        const double err = std::max(hi / best - 1.0, 1.0 - lo / best);
        check(err < 0.02, "chamfer: <11,16,19> stays within 2% of a sphere at its best scale");

        // ★重みを付けない BFS がどれだけ球から外れるかを並べて固定しておく。
        //   6 近傍は菱形 (L1)、26 近傍の等コストは立方体 (L-inf) にしかならず、
        //   どちらも 40% 級に外れる = 「音の波」には見えない。これがこの表の存在理由
        const double diamond = 2.0 / std::sqrt(2.0);   // 6 近傍で対角へ行くと 2 歩
        const double cube = 1.0 / std::sqrt(3.0);      // 26 近傍等コストは角も 1 歩
        check(diamond - 1.0 > 0.4 && 1.0 - cube > 0.4,
              "chamfer: plain 6- and 26-neighbour BFS are 40%+ off a sphere");

        // ChamferToMeters は**面方向を厳密に合わせる** (= 到達距離 [m] が軸方向でぴったり)。
        // その代わり対角は 2.9% 長く出るが、オーサリングの分かりやすさを優先した取引
        check(acoustic::ChamferToMeters(acoustic::kFaceCost, 0.5f) == 0.5f,
              "chamfer: one face step is exactly one cell in metres");
        check(edge / face - 1.0 < 0.03,
              "chamfer: measured against the face scale the diagonal is under 3% long");
    }

    // ---- (3) 26 近傍表 ----
    // 並び順は決定論のタイブレークそのものなので、性質を毎回固定しておく
    {
        bool costsOk = true;
        bool uniqueOk = true;
        bool oppositeOk = true;
        int seen[3][3][3] = {};
        for (int i = 0; i < acoustic::kNeighborCount; ++i) {
            const acoustic::Neighbor& n = acoustic::kNeighbors[i];
            const int nonZero = (n.dx != 0 ? 1 : 0) + (n.dy != 0 ? 1 : 0) + (n.dz != 0 ? 1 : 0);
            const uint16_t want = (nonZero == 1)   ? acoustic::kFaceCost
                                  : (nonZero == 2) ? acoustic::kEdgeCost
                                                   : acoustic::kCornerCost;
            if (nonZero == 0 || n.cost != want) {
                costsOk = false;
            }
            ++seen[n.dx + 1][n.dy + 1][n.dz + 1];
            const acoustic::Neighbor& o =
                acoustic::kNeighbors[acoustic::OppositeNeighbor(static_cast<uint8_t>(i))];
            if (o.dx != -n.dx || o.dy != -n.dy || o.dz != -n.dz || o.cost != n.cost) {
                oppositeOk = false;
            }
        }
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                for (int c = 0; c < 3; ++c) {
                    const int want = (a == 1 && b == 1 && c == 1) ? 0 : 1;
                    if (seen[a][b][c] != want) {
                        uniqueOk = false;
                    }
                }
            }
        }
        check(costsOk, "neighbours: every weight matches its non-zero component count");
        check(uniqueOk, "neighbours: all 26 offsets appear exactly once, centre excluded");
        check(oppositeOk, "neighbours: OppositeNeighbor negates the offset without a table");
    }

    // ---- (4) グリッドの上限 ----
    {
        AcousticGridDesc g;
        check(!acoustic::MakeGridDesc(0, 4, 4, 0.5f, 0, 0, 0, g), "grid: a zero dimension is rejected");
        check(!acoustic::MakeGridDesc(4, 4, 4, 0.0f, 0, 0, 0, g), "grid: a zero cell size is rejected");
        check(!acoustic::MakeGridDesc(512, 4, 4, 0.5f, 0, 0, 0, g), "grid: dims over the cap are rejected");
        check(!acoustic::MakeGridDesc(256, 256, 256, 0.5f, 0, 0, 0, g),
              "grid: a cell count over the cap is rejected");
    }

    // ---- (5) 占有ベイク ----
    // ★**AcousticVolume が無ければ場を張らない**のが存在ゲートの実体。
    //   ここが崩れると既存 6 replay ペアと golden 17 枚が丸ごと動く
    {
        Scene scene;
        World& w = scene.GetWorld();
        TransformSystem transforms;
        AcousticField field;

        GameObject wall = scene.CreateGameObjectTracked("Wall");
        {
            auto* col = wall.AddComponent<ColliderComponent>();
            col->shape = 1; // box
            col->halfExtents = { 0.5f, 0.5f, 0.5f };
            col->isTrigger = false;
        }
        w.ApplyStructuralChanges();
        transforms.Update(w);

        field.Sync(w);
        check(!field.HasVolume() && field.Occupancy().empty(),
              "gate: with no AcousticVolume the field never allocates anything");

        GameObject vol = scene.CreateGameObjectTracked("Volume");
        {
            auto* v = vol.AddComponent<AcousticVolumeComponent>();
            v->dimX = 8;
            v->dimY = 4;
            v->dimZ = 8;
            v->cellSize = 0.5f;
        }
        w.ApplyStructuralChanges();
        transforms.Update(w);

        field.Sync(w);
        check(field.HasVolume() && field.Occupancy().size() == 8u * 4u * 8u,
              "bake: adding a volume allocates exactly dim^3 cells");
        // 1m 角の箱は 0.5m セルで 2x2x2 = 8 セル。セルの箱と面で接するだけの隣は
        // Overlap の規約 (貫通量 > 0) で閉じない
        check(CountSolid(field) == 8, "bake: a 1m box closes exactly 2x2x2 cells");
        check(field.IsSolid(4, 2, 4), "bake: the cell at the box centre is solid");
        check(!field.IsSolid(0, 0, 0), "bake: a cell far from the box stays open");
        check(field.IsSolid(-1, 0, 0) && field.IsSolid(0, 99, 0),
              "bake: outside the grid counts as solid so waves cannot leak out");

        // 署名: 同じ world を 2 回 Sync しても焼き直さない / コライダを動かせば焼き直す
        const uint64_t sig1 = field.StaticSignature();
        field.Sync(w);
        check(field.StaticSignature() == sig1, "signature: an unchanged world keeps the same signature");
        if (auto* t = w.GetComponent<LocalTransform>(wall.Id())) {
            t->position = { 1.5f, 0.0f, 0.0f };
        }
        transforms.Update(w);
        field.Sync(w);
        check(field.StaticSignature() != sig1, "signature: moving a collider changes the signature");
        check(field.IsSolid(7, 2, 4) && !field.IsSolid(4, 2, 4),
              "bake: the closed cells followed the collider");

        // トリガーは音を遮らない (通り抜けられる場所の目印なので)
        if (auto* col = w.GetComponent<ColliderComponent>(wall.Id())) {
            col->isTrigger = true;
        }
        field.Sync(w);
        check(CountSolid(field) == 0, "bake: triggers do not block sound");
    }

    // ---- (6) ハッシュの中立性 (この計画で最も重要な 1 本) ----
    // 波が 1 本も無ければ**節ごと畳まない** = 「AcousticField を配線しただけ」で
    // 既存シーンのワールドハッシュが 1 ビットも動かないこと。
    // ここが崩れると .rep 版の bump が要り、既存 6 replay ペアが全部録り直しになる
    {
        Scene scene;
        GameObject a = scene.CreateGameObjectTracked("Alpha");
        (void)a;
        World& w = scene.GetWorld();
        w.ApplyStructuralChanges();

        AcousticField field;
        const SimSources bare{ nullptr, &scene.Time(), &scene.Persist(), nullptr, nullptr };
        const SimSources wired{ nullptr, &scene.Time(), &scene.Persist(), nullptr, &field };
        check(HashWorld(w, bare) == HashWorld(w, wired),
              "hash: an empty wave table folds nothing (wiring alone cannot move the hash)");

        // 逆に、波が 1 本でも立てばハッシュは必ず動く (被覆がある証拠)
        field.WavesForSnapshot()[0].active = 1;
        field.WavesForSnapshot()[0].ring = 3;
        check(HashWorld(w, bare) != HashWorld(w, wired),
              "hash: one active wave does move the hash");

        // スロット表は常に kMaxWaves 本 (空きスロットの位置も sim 状態)
        check(field.Waves().size() == AcousticField::kMaxWaves,
              "waves: the slot table is always kMaxWaves entries long");
        check(field.AnyWaveActive(), "waves: AnyWaveActive sees the active slot");
        field.Reset();
        check(!field.AnyWaveActive() && field.Waves().size() == AcousticField::kMaxWaves,
              "waves: Reset clears the slots but keeps the table length");
    }

    // ---- (7) DebugSetGrid (M65b の迷路テストの土台) ----
    {
        AcousticField field;
        const AcousticGridDesc g = MakeTestGrid();
        std::vector<uint8_t> occ(static_cast<size_t>(g.CellCount()), 0u);
        occ[static_cast<size_t>(acoustic::CellIndex(g, 2, 1, 2))] = 1u;
        field.DebugSetGrid(g, std::move(occ));
        check(field.HasVolume() && field.IsSolid(2, 1, 2) && !field.IsSolid(2, 1, 3),
              "debug: DebugSetGrid installs a hand-built occupancy grid");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Acoustic self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Acoustic self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
