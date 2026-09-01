//====================================================================================
//                          AcousticSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響伝播のヘッドレス回帰テスト（M65a/M65b 分）
//====================================================================================
#include "Engine/Engine/Acoustic/AcousticSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

// L 字の 1 セル幅の廊下 (M65b の伝播テストの土台)。dimY=1 の平面迷路。
//   腕 1: x in [2,20], z = 2   /   腕 2: x = 20, z in [2,20]
// 廊下以外は全部壁なので、斜めで角をすり抜けることもできない。
// (10,0,10) は廊下から完全に切り離された「壁の向こう」の代表点
std::vector<uint8_t> MakeLMaze(const AcousticGridDesc& g)
{
    std::vector<uint8_t> occ(static_cast<size_t>(g.CellCount()), 1u);
    for (int32_t x = 2; x <= 20; ++x) {
        occ[static_cast<size_t>(acoustic::CellIndex(g, x, 0, 2))] = 0u;
    }
    for (int32_t z = 2; z <= 20; ++z) {
        occ[static_cast<size_t>(acoustic::CellIndex(g, 20, 0, z))] = 0u;
    }
    return occ;
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

    // ---- (8) 迷路の伝播: 壁を貫通せず、角を回り込む ----
    // L 字の 1 セル幅の廊下。原点 (2,0,2) から角 (20,0,2) を経て (20,0,20) へ。
    // 経路は 34 面ステップ + 角の斜め 1 歩 = 34*11 + 16 = 390。
    // ★**斜めの角抜けを許している**ことをここで固定する — 凸角を 22 ではなく 16 で
    //   曲がれる。もし空間が空いていれば通る 18 斜めステップ = 288 より**長い**
    //   という大小関係が「回り込んだ (直進していない)」証拠
    {
        AcousticField field;
        AcousticGridDesc g;
        const bool gok = acoustic::MakeGridDesc(24, 1, 24, 0.5f, 0.0f, 0.0f, 0.0f, g);
        check(gok, "maze: grid built");
        field.DebugSetGrid(g, MakeLMaze(g));

        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 2, 0, 2, wx, wy, wz);
        const bool emitted = field.Emit(EntityID{ 7, 1 }, wx, wy, wz, 1.0f, 20.0f, 0, 1, 0);
        check(emitted && field.Waves()[0].active != 0 && field.Waves()[0].ox == 2
                  && field.Waves()[0].oz == 2 && field.Waves()[0].maxRing == 40,
              "maze: Emit takes slot 0 and floors radius into rings");

        for (int i = 0; i < 40; ++i) {
            field.Advance();
        }
        check(field.Waves()[0].ring == 40, "maze: one ring per tick at ticksPerRing=1");

        const uint16_t dCorner = field.DistanceAt(0, 20, 0, 20);
        check(dCorner == 34 * acoustic::kFaceCost + acoustic::kEdgeCost,
              "maze: the far arm is reached by walking the corner (34 face steps + one diagonal)");
        check(dCorner > 18 * acoustic::kEdgeCost,
              "maze: the folded path is longer than the straight-line chamfer distance");
        check(field.DistanceAt(0, 10, 0, 10) == AcousticField::kUnreached,
              "maze: a sealed cell behind the walls is never reached");
        check(field.DistanceAt(0, 3, 0, 3) == AcousticField::kUnreached,
              "maze: the wall cell next to the corridor stays unreached (walls are not entered)");

        // 親方向を辿ると必ず原点へ戻り、距離は単調に減る
        int32_t cx = 20, cy = 0, cz = 20;
        int steps = 0;
        bool walkOk = true;
        uint16_t prev = dCorner;
        while (!(cx == 2 && cy == 0 && cz == 2)) {
            const uint8_t pd = field.ParentDirAt(0, cx, cy, cz);
            if (pd == AcousticField::kNoParent || ++steps > 200) {
                walkOk = false;
                break;
            }
            const acoustic::Neighbor& nb = acoustic::kNeighbors[pd];
            cx += nb.dx;
            cy += nb.dy;
            cz += nb.dz;
            const uint16_t d = field.DistanceAt(0, cx, cy, cz);
            if (d >= prev) {
                walkOk = false;
                break;
            }
            prev = d;
        }
        check(walkOk && steps == 35 && prev == 0,
              "maze: parentDir walks back to the source with strictly decreasing distance");
    }

    // ---- (8b) 軸平行な 1 セル厚の壁は漏らさない ----
    // ★これが企画 §3-1 の「壁を貫通しない」の本体。斜めの角抜けを許しているので
    //   「斜め 1 セル厚の壁」は理屈のうえでは漏れるが、壁は box で組む前提
    //   (= ボクセル化すると必ず軸平行で 1 セル以上の厚みになる) なので起きない。
    //   塞ぐなら「斜め移動は面隣接の中間セルが 1 つでも開いていること」を条件に足す
    {
        AcousticField field;
        AcousticGridDesc g;
        (void)acoustic::MakeGridDesc(12, 1, 12, 0.5f, 0.0f, 0.0f, 0.0f, g);
        std::vector<uint8_t> occ(static_cast<size_t>(g.CellCount()), 0u);
        for (int32_t z = 0; z < 12; ++z) {
            occ[static_cast<size_t>(acoustic::CellIndex(g, 6, 0, z))] = 1u; // 端から端までの仕切り
        }
        field.DebugSetGrid(g, std::move(occ));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 2, 0, 6, wx, wy, wz);
        (void)field.Emit(EntityID{ 3, 1 }, wx, wy, wz, 1.0f, 20.0f, 0, 1, 0);
        for (int i = 0; i < 40; ++i) {
            field.Advance();
        }
        bool leaked = false;
        for (int32_t z = 0; z < 12; ++z) {
            for (int32_t x = 6; x < 12; ++x) {
                if (field.DistanceAt(0, x, 0, z) != AcousticField::kUnreached) {
                    leaked = true;
                }
            }
        }
        check(!leaked, "wall: a one-cell-thick axis-aligned wall is never crossed");
        check(field.DistanceAt(0, 5, 0, 11) != AcousticField::kUnreached,
              "wall: the near side of that wall is fully reached (the test is not vacuous)");
    }

    // ---- (9) ★引き直しの同値性 (この計画で最も重要な不変条件) ----
    // 「増分で ring 0→20 まで育てた場」と「Invalidate() 後に引き直した場」が
    // memcmp で同一。ここが割れると **リプレイは通るのに巻き戻しでだけ割れる**
    {
        AcousticField field;
        AcousticGridDesc g;
        (void)acoustic::MakeGridDesc(24, 1, 24, 0.5f, 0.0f, 0.0f, 0.0f, g);
        field.DebugSetGrid(g, MakeLMaze(g));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 2, 0, 2, wx, wy, wz);
        (void)field.Emit(EntityID{ 7, 1 }, wx, wy, wz, 1.0f, 20.0f, 0, 1, 0);
        for (int i = 0; i < 20; ++i) {
            field.Advance();
        }
        const std::vector<uint16_t> grownDist = field.FieldOf(0).dist;
        const std::vector<uint8_t> grownParent = field.FieldOf(0).parentDir;

        field.Invalidate();
        field.Rebuild();
        const AcousticField::WaveField& re = field.FieldOf(0);
        const bool same =
            re.dist.size() == grownDist.size() && re.parentDir.size() == grownParent.size()
            && std::memcmp(re.dist.data(), grownDist.data(), grownDist.size() * sizeof(uint16_t)) == 0
            && std::memcmp(re.parentDir.data(), grownParent.data(), grownParent.size()) == 0;
        check(same, "rebuild: growing incrementally and rebuilding from ring 0 are bit-identical");
        check(field.Waves()[0].ring == 20, "rebuild: the wave is left at the same ring");

        // 別インスタンスで同じ手順を踏んでも同一 (状態の持ち越しが無いことの確認)
        AcousticField other;
        other.DebugSetGrid(g, MakeLMaze(g));
        (void)other.Emit(EntityID{ 7, 1 }, wx, wy, wz, 1.0f, 20.0f, 0, 1, 0);
        for (int i = 0; i < 20; ++i) {
            other.Advance();
        }
        check(other.FieldOf(0).dist == grownDist && other.FieldOf(0).parentDir == grownParent,
              "rebuild: a second field fed the same inputs lands on the same array");
    }

    // ---- (10) スロットの決定論: 最小 index の空き / 満杯は false ----
    {
        AcousticField field;
        AcousticGridDesc g;
        (void)acoustic::MakeGridDesc(24, 1, 24, 0.5f, 0.0f, 0.0f, 0.0f, g);
        field.DebugSetGrid(g, MakeLMaze(g));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 2, 0, 2, wx, wy, wz);

        bool allTook = true;
        for (uint32_t s = 0; s < AcousticField::kMaxWaves; ++s) {
            // radius を小さくしておかないと 16 本ぶんの局所ボックスで時間を食う
            if (!field.Emit(EntityID{ s + 1, 1 }, wx, wy, wz, 1.0f, 1.0f, 0, 1, s)) {
                allTook = false;
            }
        }
        check(allTook && field.Waves()[15].active != 0, "slots: 16 emits fill the table in order");
        const uint64_t survivorTick = field.Waves()[0].bornTick;
        check(!field.Emit(EntityID{ 99, 1 }, wx, wy, wz, 1.0f, 1.0f, 0, 1, 99),
              "slots: a full table refuses the emit (it must not evict the oldest)");
        check(field.Waves()[0].bornTick == survivorTick,
              "slots: the refused emit left every existing wave untouched");

        // 途中のスロットが空くと、次の Emit はその**最小 index** を取る
        field.WavesForSnapshot()[4] = AcousticField::Wave{};
        check(field.Emit(EntityID{ 42, 1 }, wx, wy, wz, 1.0f, 1.0f, 0, 1, 100)
                  && field.Waves()[4].source.index == 42,
              "slots: the freed slot 4 is the one reused (lowest free index wins)");
    }

    // ---- (11) 原点が閉セルなら開セルへ寄せる / 完全に埋まっていれば false ----
    {
        AcousticField field;
        AcousticGridDesc g;
        (void)acoustic::MakeGridDesc(8, 4, 8, 0.5f, 0.0f, 0.0f, 0.0f, g);
        std::vector<uint8_t> occ(static_cast<size_t>(g.CellCount()), 0u);
        occ[static_cast<size_t>(acoustic::CellIndex(g, 4, 2, 4))] = 1u; // 足元だけ塞ぐ
        field.DebugSetGrid(g, std::move(occ));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 4, 2, 4, wx, wy, wz);
        const bool nudged = field.Emit(EntityID{ 1, 1 }, wx, wy, wz, 1.0f, 2.0f, 0, 1, 0);
        const AcousticField::Wave& w0 = field.Waves()[0];
        check(nudged && !(w0.ox == 4 && w0.oy == 2 && w0.oz == 4)
                  && !field.IsSolid(w0.ox, w0.oy, w0.oz),
              "emit: an origin inside a collider is nudged to the first open neighbour");

        AcousticField sealed;
        std::vector<uint8_t> full(static_cast<size_t>(g.CellCount()), 1u);
        sealed.DebugSetGrid(g, std::move(full));
        check(!sealed.Emit(EntityID{ 1, 1 }, wx, wy, wz, 1.0f, 2.0f, 0, 1, 0),
              "emit: a fully sealed grid refuses the emit");
    }

    // ---- (12) エネルギーは整数距離の純関数 (減衰は Audio の RolloffGain と同じ式) ----
    {
        const float amp = 1.0f;
        const float cs = 0.5f;
        const uint32_t maxD = 40 * acoustic::kFaceCost;
        const float e0 = acoustic::EnergyAt(0, maxD, amp, cs);
        const float e1 = acoustic::EnergyAt(11, maxD, amp, cs);
        const float e2 = acoustic::EnergyAt(22, maxD, amp, cs);
        check(e0 >= e1 && e1 > e2 && e2 > 0.0f, "energy: monotonically decreasing with distance");
        check(acoustic::EnergyAt(maxD + 1, maxD, amp, cs) == 0.0f,
              "energy: nothing past the reach (RolloffGain returns exactly 0 at maxDistance)");
        // 同じ整数距離なら何度呼んでも同値 = 純関数 (経路も履歴も持たない)
        check(acoustic::EnergyAt(33, maxD, amp, cs) == acoustic::EnergyAt(33, maxD, amp, cs),
              "energy: a pure function of the integer chamfer distance");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Acoustic self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Acoustic self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
