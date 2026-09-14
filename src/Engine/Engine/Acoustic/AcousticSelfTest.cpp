//====================================================================================
//                          AcousticSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響伝播のヘッドレス回帰テスト（M65a/M65b 分）
//====================================================================================
#include "Engine/Engine/Acoustic/AcousticSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "Engine/Core/Check.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/Acoustic/AcousticGrid.h"
#include "Engine/Engine/Acoustic/AcousticNav.h"
#include "Engine/Engine/Acoustic/AgentSystem.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/TransformSystem.h"
#include "Engine/Renderer/RenderTypes.h" // M65i: MakeAcousticCB (混ぜ具合が z 席へ流れるか)

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

// M65f: 流れ場テスト用の L 字迷路 (24x2x24)。MakeLMaze と同じ間取りで**廊下を 2 セル幅**に
// してある — 粗グリッド (ratio 2) で「サブセルの過半が開」を満たすには 1 セル幅では足りず、
// 廊下が丸ごと閉じてしまうため (これ自体が過半ルールの性質そのもの)
std::vector<uint8_t> MakeLMaze2(const AcousticGridDesc& g)
{
    std::vector<uint8_t> occ(static_cast<size_t>(g.CellCount()), 1u);
    for (int32_t y = 0; y < g.dimY; ++y) {
        for (int32_t x = 2; x <= 21; ++x) {
            for (int32_t z = 2; z <= 3; ++z) {
                occ[static_cast<size_t>(acoustic::CellIndex(g, x, y, z))] = 0u;
            }
        }
        for (int32_t z = 2; z <= 21; ++z) {
            for (int32_t x = 20; x <= 21; ++x) {
                occ[static_cast<size_t>(acoustic::CellIndex(g, x, y, z))] = 0u;
            }
        }
    }
    return occ;
}

// 「そのエンティティが sinceTick 以降に立てた波」の本数 (FSM テストが使う)。
// ★単に「アクティブな波が増えたか」では検査にならない — 同じ tick に別の波が
//   寿命で消えると増分が打ち消され、鳴っていても 0 に見える。**誰がいつ立てたか**で数える
size_t CountWavesFrom(const AcousticField& f, EntityID who, uint64_t sinceTick)
{
    size_t n = 0;
    for (const AcousticField::Wave& w : f.Waves()) {
        if (w.active != 0 && w.source.index == who.index && w.bornTick >= sinceTick) {
            ++n;
        }
    }
    return n;
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
            field.Advance(nullptr, 0);
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
            field.Advance(nullptr, 0);
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
            field.Advance(nullptr, 0);
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
            other.Advance(nullptr, 0);
        }
        check(other.FieldOf(0).dist == grownDist && other.FieldOf(0).parentDir == grownParent,
              "rebuild: a second field fed the same inputs lands on the same array");
    }

    // ---- (10) スロットの決定論: 最小 index の空き / 満杯は false / 敵の声だけは最古の非敵を追い出す ----
    {
        Scene scene;
        World& w = scene.GetWorld();
        AcousticField field;
        AcousticGridDesc g;
        (void)acoustic::MakeGridDesc(24, 1, 24, 0.5f, 0.0f, 0.0f, 0.0f, g);
        field.DebugSetGrid(g, MakeLMaze(g));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 2, 0, 2, wx, wy, wz);

        // ★偽の発音元は World に居ない id にする (生きた実体と index/generation が重なると
        //   「敵の声か」の判定に World の中身が混ざる)
        const auto fake = [](uint32_t i) { return EntityID{ 1000u + i, 7u }; };
        bool allTook = true;
        for (uint32_t s = 0; s < AcousticField::kMaxWaves; ++s) {
            // radius を小さくしておかないと kMaxWaves 本ぶんの局所ボックスで時間を食う
            if (!field.Emit(fake(s), wx, wy, wz, 1.0f, 1.0f, 0, 1, s)) {
                allTook = false;
            }
        }
        check(allTook && field.Waves()[AcousticField::kMaxWaves - 1].active != 0,
              "slots: kMaxWaves emits fill the table in order");
        const uint64_t survivorTick = field.Waves()[0].bornTick;
        check(!field.Emit(fake(99), wx, wy, wz, 1.0f, 1.0f, 0, 1, 99),
              "slots: a full table refuses the emit (it must not evict the oldest)");
        check(field.Waves()[0].bornTick == survivorTick,
              "slots: the refused emit left every existing wave untouched");

        // 途中のスロットが空くと、次の Emit はその**最小 index** を取る
        field.WavesForSnapshot()[4] = AcousticField::Wave{};
        check(field.Emit(fake(42), wx, wy, wz, 1.0f, 1.0f, 0, 1, 100)
                  && field.Waves()[4].source == fake(42),
              "slots: the freed slot 4 is the one reused (lowest free index wins)");

        // ★敵の声 (agentPriority = World) だけは満杯でも立つ (2026-09-14、三校)。
        //   追い出すのは AgentBrain を持たない発音元のうち bornTick が最小の波
        GameObject agent = scene.CreateGameObjectTracked("SlotAgent");
        agent.AddComponent<AgentBrainComponent>();
        w.ApplyStructuralChanges();
        check(!field.Emit(fake(98), wx, wy, wz, 1.0f, 1.0f, 0, 1, 200, 0, &w)
                  && field.Waves()[0].source == fake(0),
              "slots: a non-agent source never evicts, even when a World is passed");
        check(field.Emit(agent.Id(), wx, wy, wz, 1.0f, 1.0f, 1, 1, 201, 0, &w)
                  && field.Waves()[0].source == agent.Id() && field.Waves()[0].bornTick == 201,
              "slots: an agent voice evicts the oldest non-agent wave (bornTick 0 in slot 0)");
        check(field.Waves()[1].source == fake(1) && field.Waves()[4].source == fake(42),
              "slots: the eviction touched only that one slot");
        // 残りの非敵の波を全部追い出すと、次の敵の声は立たない (敵の声どうしは追い出さない)
        bool evictedAll = true;
        for (uint32_t i = 1; i < AcousticField::kMaxWaves; ++i) {
            if (!field.Emit(agent.Id(), wx, wy, wz, 1.0f, 1.0f, 1, 1, 201 + i, 0, &w)) {
                evictedAll = false;
            }
        }
        check(evictedAll && field.Waves()[4].source == agent.Id(),
              "slots: agent voices keep evicting while a non-agent wave is left (slot 4 = bornTick 100 goes last)");
        check(!field.Emit(agent.Id(), wx, wy, wz, 1.0f, 1.0f, 1, 1, 400, 0, &w),
              "slots: a table full of agent voices refuses another agent voice");
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

    // ---- (13) 床材が発音を決める (M65c)。**出荷している .physmat.json をそのまま読む** ----
    // ★インラインの PhysMat を組んで比べても意味が無い — ここで守りたいのは
    //   「企画 3-4 の表どおりの資産が入っていること」であって式ではない。
    //   資産の値は sim ハッシュに載らないので、壊れても replay_verify は無反応になる =
    //   このテストが唯一の防波堤
    {
        const std::wstring root = FindAssetsRoot();
        static const wchar_t* const kNames[6] = { L"carpet", L"wood",  L"gravel",
                                                  L"water",  L"metal", L"glass" };
        PhysMatLibrary lib;
        const PhysMat* mats[6] = {};
        int loaded = 0;
        for (int i = 0; i < 6; ++i) {
            const std::wstring p = root + L"\\physmats\\" + kNames[i] + L".physmat.json";
            const uint64_t h = lib.LoadFromFile(p);
            if (h != 0) {
                mats[i] = lib.Get(h);
                ++loaded;
            }
        }
        check(loaded == 6, "materials: the six acoustic floor presets are on disk");
        if (loaded == 6) {
            bool louder = true;
            bool farther = true;
            for (int i = 0; i + 1 < 5; ++i) { // carpet < wood < gravel < water < metal
                louder = louder && mats[i]->acousticLoudness < mats[i + 1]->acousticLoudness;
                farther = farther && mats[i]->acousticRadiusM < mats[i + 1]->acousticRadiusM;
            }
            check(louder, "materials: carpet < wood < gravel < water < metal in loudness");
            check(farther, "materials: the reach grows together with the loudness");
            check(mats[5]->acousticLoudness == mats[4]->acousticLoudness,
                  "materials: glass is exactly as loud as metal (the plan's 'metal ~= glass')");
            check(mats[0]->acousticLoudness > 0.0f,
                  "materials: even carpet is audible (0 would mean 'no material at all')");
            // 音色 4 種すべてに使い道があること = M65e の色分けの被覆が空にならない
            bool tones[4] = { false, false, false, false };
            for (int i = 0; i < 6; ++i) {
                tones[mats[i]->acousticTone & 3] = true;
            }
            check(tones[0] && tones[1] && tones[2] && tones[3],
                  "materials: all four tones are exercised by the presets");
        }
        // ★未割当 = 無音。これが「材料を割り当てていない既存シーンはビット同一」の入口
        check(SelectAcousticLoudness(nullptr) == 0.0f && SelectAcousticRadiusM(nullptr) == 0.0f
                  && SelectAcousticTone(nullptr) == 0,
              "materials: an unassigned physMaterial is silent");
    }

    // ---- (14) 到達距離 [m] -> maxRing は整数の純関数 (切り捨て) ----
    // ★float が整数へ落ちる境界はここ 1 箇所しかない。切り捨てが切り上げに変わると
    //   全材質の到達セルが 1 リングぶんずれる (絵では気づけない種類の変化)
    {
        AcousticField field;
        const AcousticGridDesc g = MakeTestGrid(); // 8x4x8 / 0.5m
        std::vector<uint8_t> open(static_cast<size_t>(g.CellCount()), 0u);
        field.DebugSetGrid(g, std::move(open));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 4, 2, 4, wx, wy, wz);

        // carpet 相当 (3.0m / 0.5 = 6 リングちょうど)
        check(field.Emit(EntityID{ 1, 1 }, wx, wy, wz, 0.12f, 3.0f, 0, 1, 0)
                  && field.Waves()[0].maxRing == 6,
              "reach: 3.0m at 0.5m cells is exactly 6 rings");
        // 端数は**切り捨て** (3.4 / 0.5 = 6.8 -> 6)
        check(field.Emit(EntityID{ 2, 1 }, wx, wy, wz, 0.12f, 3.4f, 0, 1, 0)
                  && field.Waves()[1].maxRing == 6,
              "reach: a fractional radius is floored, never rounded");
        // 0 より大きいが 1 セルに満たない半径でも**必ず 1 リングは進む** (無音にはしない)
        check(field.Emit(EntityID{ 3, 1 }, wx, wy, wz, 0.12f, 0.2f, 0, 1, 0)
                  && field.Waves()[2].maxRing == 1,
              "reach: a sub-cell radius still emits one ring (clamped, not silenced)");
        // 振幅 0 (= 材質未割当) は**そもそも波にならない**
        check(!field.Emit(EntityID{ 4, 1 }, wx, wy, wz, 0.0f, 30.0f, 0, 1, 0),
              "reach: zero loudness never becomes a wave");
    }

    // ---- (15) 衝撃の力積 -> 発音倍率 (M65c) ----
    {
        check(acoustic::ImpactGain(0.0f) == 0.0f, "impact: no gain below the floor threshold");
        check(acoustic::ImpactGain(acoustic::kImpactMinImpulse) == 0.0f,
              "impact: the threshold itself is silent (exclusive)");
        check(acoustic::ImpactGain(acoustic::kImpactRefImpulse) == 1.0f,
              "impact: saturates at the reference impulse");
        check(acoustic::ImpactGain(acoustic::kImpactRefImpulse * 10.0f) == 1.0f,
              "impact: never exceeds 1 no matter how hard the hit");
        const float g1 = acoustic::ImpactGain(1.0f);
        const float g2 = acoustic::ImpactGain(2.0f);
        check(g1 > 0.0f && g2 > g1, "impact: monotonic between threshold and reference");
        // ★**載っているだけの接触は鳴らない**。2kg が静止している接触の力積は m*g*dt で、
        //   そこから静止支持ぶん (x kImpactRestingMargin) を引くと必ず負になる
        const float restingImpulse = 2.0f * 9.81f * (1.0f / 60.0f);
        const float subtracted = restingImpulse * acoustic::kImpactRestingMargin;
        check(acoustic::ImpactGain(restingImpulse - subtracted) == 0.0f,
              "impact: a body that is merely resting on a floor is silent");
    }

    // ---- (16) ★残光は描画レーン: 焼いてもワールドハッシュが動かない (M65d) ----
    // ここが崩れると「絵のために場を書き換えたらリプレイが割れた」という、
    // 原因の一番遠いバグになる。(6) の裏返しで、この計画のもう 1 本の柱
    {
        Scene scene;
        GameObject a = scene.CreateGameObjectTracked("Alpha");
        (void)a;
        World& w = scene.GetWorld();
        w.ApplyStructuralChanges();

        AcousticField field;
        const AcousticGridDesc g = MakeTestGrid();
        std::vector<uint8_t> open(static_cast<size_t>(g.CellCount()), 0u);
        field.DebugSetGrid(g, std::move(open));
        const SimSources src{ nullptr, &scene.Time(), &scene.Persist(), nullptr, &field };
        const uint64_t before = HashWorld(w, src);

        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 4, 2, 4, wx, wy, wz);
        check(field.Emit(EntityID{ 1, 1 }, wx, wy, wz, 1.0f, 3.0f, 0, 1, 0),
              "glow: the probe wave was emitted");
        // ★波を立てた時点でハッシュは動く (波スロット表は sim 状態)。ここで測り直す
        const uint64_t withWave = HashWorld(w, src);
        check(before != withWave, "glow: emitting does move the hash (the wave table is sim state)");
        for (int i = 0; i < 4; ++i) {
            field.Advance(nullptr, 0); // 残光が焼かれる
        }
        check(field.VisualActive() && !field.Glow().empty(),
              "glow: advancing a wave lights cells");
        const uint64_t litHash = HashWorld(w, src);
        // 波を 4 リング進めたぶんハッシュは動いているので、**残光だけを動かして**比べる
        const uint32_t serialBefore = field.VisualSerial();
        field.DecayVisual(0.5f);
        check(HashWorld(w, src) == litHash,
              "glow: decaying the afterglow cannot move the world hash");
        field.ResetVisual();
        check(HashWorld(w, src) == litHash,
              "glow: clearing the afterglow cannot move the world hash");
        check(field.VisualSerial() != serialBefore && !field.VisualActive() && field.Glow().empty(),
              "glow: ResetVisual drops the buffer and bumps the serial");
    }

    // ---- (16b) 解析的な波面 = 「描画だけ円」(2026-09-12) も描画レーン ----
    // 先読み距離場は sim の距離場と同じ関数を回すので、sim が追いついた所ではビット一致する。
    // 見通しビットは直線で届くセルにだけ立ち、角を曲がった先には立たない。ハッシュは動かない
    {
        Scene scene;
        World& w = scene.GetWorld();
        w.ApplyStructuralChanges();
        AcousticField field;
        AcousticGridDesc g;
        (void)acoustic::MakeGridDesc(24, 1, 24, 0.5f, 0.0f, 0.0f, 0.0f, g);
        field.DebugSetGrid(g, MakeLMaze(g));
        const SimSources src{ nullptr, &scene.Time(), &scene.Persist(), nullptr, &field };
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 2, 0, 2, wx, wy, wz);
        check(field.Emit(EntityID{ 9, 1 }, wx, wy, wz, 1.0f, 20.0f, 0, 1, 0), "front: emitted");
        for (uint64_t i = 0; i < 10; ++i) {
            field.Advance(nullptr, i);
            field.UpdateFrontPreview(i);
        }
        const uint64_t h = HashWorld(w, src);
        field.UpdateFrontPreview(10);
        check(HashWorld(w, src) == h, "front: the preview cannot move the world hash");
        check(field.FrontActive() && field.FrontMask().size() == static_cast<size_t>(g.CellCount()),
              "front: a live wave produces a mask of the whole grid");
        bool same = true;
        for (int32_t x = 2; x <= 20; ++x) {
            const uint16_t d = field.DistanceAt(0, x, 0, 2);
            if (d != AcousticField::kUnreached && d != field.FrontPreviewDistanceAt(0, x, 0, 2)) {
                same = false;
            }
        }
        check(same, "front: the preview agrees with the sim field wherever the sim has arrived");
        check(field.DistanceAt(0, 14, 0, 2) == AcousticField::kUnreached
                  && field.FrontPreviewDistanceAt(0, 14, 0, 2) != AcousticField::kUnreached,
              "front: the preview runs a few rings ahead of the sim");
        for (uint64_t i = 10; i < 40; ++i) {
            field.Advance(nullptr, i);
            field.UpdateFrontPreview(i);
        }
        check(field.Waves()[0].ring == 40, "front: the wave reached its last ring");
        const std::vector<uint32_t>& mask = field.FrontMask();
        check((mask[static_cast<size_t>(acoustic::CellIndex(g, 10, 0, 2))] & 1u) != 0,
              "front: a cell straight down the corridor gets the wave's bit");
        check(field.DistanceAt(0, 20, 0, 10) != AcousticField::kUnreached
                  && (mask[static_cast<size_t>(acoustic::CellIndex(g, 20, 0, 10))] & 1u) == 0,
              "front: a cell reached around the corner is not in line of sight (no bit)");
        check((mask[static_cast<size_t>(acoustic::CellIndex(g, 10, 0, 10))] & 1u) == 0,
              "front: a wall cell never gets a bit");
        const AcousticField::FrontWave& fw = field.FrontWaves()[0];
        check(fw.active != 0 && std::fabs(fw.radiusM - 40.5f * 0.5f) < 1e-5f
                  && std::fabs(fw.maxDistM - 20.0f) < 1e-5f && fw.extraAgeTicks == 0.0f,
              "front: the wave table carries radius (ring+0.5 cells) and reach");
        // 消えた波は名残として最終半径で残り、名残が尽きると消える
        field.Advance(nullptr, 40); // ring >= maxRing = 消える
        check(field.Waves()[0].active == 0, "front: the wave died");
        field.UpdateFrontPreview(41);
        check(field.FrontActive() && field.FrontWaves()[0].active != 0
                  && field.FrontWaves()[0].extraAgeTicks == 0.0f,
              "front: a dead wave lingers with its final radius");
        field.UpdateFrontPreview(41 + AcousticField::kFrontLingerTicks + 1);
        check(!field.FrontActive() && field.FrontWaves()[0].active == 0,
              "front: the linger expires");
        field.UpdateFrontPreview(41 + AcousticField::kFrontLingerTicks + 2);
        const uint32_t fs = field.FrontSerial();
        field.ResetVisual();
        check(field.FrontSerial() != fs && !field.FrontActive() && field.FrontMask().empty(),
              "front: ResetVisual drops the mask and bumps the serial");
    }

    // ---- (17) 減衰は単調で必ず 0 に届く (M65d) ----
    // ★uint8 の乗算を四捨五入にすると 255 が 255 のまま止まって**永久に消えない**。
    //   切り捨てであることをここで固定する
    {
        AcousticField field;
        const AcousticGridDesc g = MakeTestGrid();
        std::vector<uint8_t> open(static_cast<size_t>(g.CellCount()), 0u);
        field.DebugSetGrid(g, std::move(open));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 4, 2, 4, wx, wy, wz);
        check(field.Emit(EntityID{ 1, 1 }, wx, wy, wz, 1.0f, 2.0f, 0, 1, 0), "decay: emitted");
        field.Advance(nullptr, 0);
        check(field.VisualActive(), "decay: the wave lit at least one cell");

        auto maxGlow = [&]() {
            uint8_t m = 0;
            for (uint8_t v : field.Glow()) {
                m = (v > m) ? v : m;
            }
            return m;
        };
        uint8_t prev = maxGlow();
        check(prev > 0, "decay: the source cell is at full brightness");
        // 単調非増加であること + 有限の tick で 0 に落ちること
        int ticks = 0;
        bool monotonic = true;
        while (field.VisualActive() && ticks < 20000) {
            field.DecayVisual(acoustic::kGlowDecayPerTick);
            const uint8_t now = maxGlow();
            monotonic = monotonic && (now < prev);
            prev = now;
            ++ticks;
        }
        check(monotonic, "decay: the afterglow is strictly monotonic downwards");
        check(!field.VisualActive() && maxGlow() == 0,
              "decay: the afterglow always reaches exactly zero");
        // 実測 227 tick = 約 3.8 秒 — 企画 §3-5 の「記憶の地図」の長さ。
        // ★上限に幅を持たせているのは、uint8 の切り捨てが「1 tick に最低 1 減る」を
        //   強制するせいで 255 tick より長くはならないから (割合を変えても越えられない)
        check(ticks > 150 && ticks < 320,
              "decay: the afterglow lasts a few seconds, not forever");
    }

    // ---- (18) ★残光は閉セルに絶対入らない / 聞こえた所しか光らない (M65d) ----
    // 企画 §3-1「見えている所と敵に届く所が一致する」を数値で固定する 1 本。
    // WriteShell を AdvanceWaveOneRing の外へ切り出すと、まずここが割れる
    {
        AcousticField field;
        AcousticGridDesc g;
        const bool ok = acoustic::MakeGridDesc(24, 1, 24, 0.5f, 0.0f, 0.0f, 0.0f, g);
        MYE_CHECK(ok);
        field.DebugSetGrid(g, MakeLMaze(g));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 2, 0, 2, wx, wy, wz);
        check(field.Emit(EntityID{ 7, 1 }, wx, wy, wz, 1.0f, 30.0f, 0, 1, 0), "shell: emitted");
        for (int i = 0; i < 40; ++i) {
            field.Advance(nullptr, 0);
        }
        size_t lit = 0, litSolid = 0, litUnreached = 0;
        for (int32_t z = 0; z < g.dimZ; ++z) {
            for (int32_t x = 0; x < g.dimX; ++x) {
                const size_t i = static_cast<size_t>(acoustic::CellIndex(g, x, 0, z));
                if (field.Glow()[i] == 0) {
                    continue;
                }
                ++lit;
                if (field.IsSolid(x, 0, z)) {
                    ++litSolid;
                }
                if (field.DistanceAt(0, x, 0, z) == AcousticField::kUnreached) {
                    ++litUnreached;
                }
            }
        }
        check(lit > 0, "shell: the corridor is lit");
        check(litSolid == 0, "shell: not a single solid cell is ever lit");
        check(litUnreached == 0, "shell: every lit cell is a cell the wave actually reached");
        // 壁の向こう (10,0,10) は廊下から切り離されている = 音も光も届かない
        check(field.Glow()[static_cast<size_t>(acoustic::CellIndex(g, 10, 0, 10))] == 0,
              "shell: the far side of the wall stays pitch black");
    }

    // ---- (19) 残光の符号化: 逆二乗のダイナミックレンジが uint8 に載る (M65d) ----
    // ★線形量子化にすると数メートル先が全部 0 になり、「波が遠くの壁を描く」が
    //   絵から消える。ガンマ 1/4 (sqrt 2 回) であることをここで固定する
    {
        check(acoustic::EncodeGlow(0.0f) == 0 && acoustic::EncodeGlow(-1.0f) == 0,
              "glow encode: zero and negative energy encode to zero");
        check(acoustic::EncodeGlow(1.0f) == 255 && acoustic::EncodeGlow(4.0f) == 255,
              "glow encode: saturates at 255 and never wraps");
        // 20m 先 (逆二乗で 1/1600) でも 0 にならないことが要求そのもの
        const float far20m = 1.0f / 1600.0f;
        check(acoustic::EncodeGlow(far20m) > 0,
              "glow encode: a 1/1600 energy is still visible (linear would be zero)");
        // 単調増加
        uint8_t prev = 0;
        bool mono = true;
        for (int i = 1; i <= 64; ++i) {
            const uint8_t v = acoustic::EncodeGlow(static_cast<float>(i) / 64.0f);
            mono = mono && (v >= prev);
            prev = v;
        }
        check(mono && prev == 255, "glow encode: monotonic in energy");
        // 復号は 4 乗。往復の相対誤差が 2% 未満 (シェーダ側の逆変換の正本)
        bool roundTrip = true;
        for (int i = 1; i <= 32; ++i) {
            const float e = static_cast<float>(i) / 32.0f;
            const float back = acoustic::DecodeGlow(acoustic::EncodeGlow(e));
            roundTrip = roundTrip && (std::fabs(back - e) < e * 0.02f);
        }
        check(roundTrip, "glow encode: decode(encode(e)) recovers e within 2%");
    }

    // ---- (20) ★法線押し出しが壁面で開セルを拾う (M65e) ----
    // acoustic_common.hlsli の AcousticSample の**幾何だけ**を CPU で再現して固定する。
    // 閉セルは波が絶対に訪れない = 残光は開セル側にしかないので、壁面をそのまま
    // サンプルすると常に 0 = 「壁が光らない」という、絵を見ても原因の分からない不具合になる。
    // ★シェーダ側はバイリニアで引くが、ここで固定したいのは補間ではなく
    //   「押し出し先が隣の開セルに入るか」= 最近傍で十分 (むしろそこだけを見たい)
    {
        AcousticField field;
        AcousticGridDesc g;
        const bool ok = acoustic::MakeGridDesc(16, 1, 16, 0.5f, 0.0f, 0.0f, 0.0f, g);
        MYE_CHECK(ok);
        // x = 8 の列だけを壁にした 1 枚の板。原点は x=4 側 (壁の手前)
        std::vector<uint8_t> occ(static_cast<size_t>(g.CellCount()), 0u);
        for (int32_t z = 0; z < g.dimZ; ++z) {
            occ[static_cast<size_t>(acoustic::CellIndex(g, 8, 0, z))] = 1u;
        }
        field.DebugSetGrid(g, std::move(occ));
        float ox = 0.0f, oy = 0.0f, oz = 0.0f;
        acoustic::CellToWorldCenter(g, 4, 0, 8, ox, oy, oz);
        check(field.Emit(EntityID{ 11, 1 }, ox, oy, oz, 1.0f, 30.0f, 0, 1, 0), "push: emitted");
        for (int i = 0; i < 12; ++i) {
            field.Advance(nullptr, 0);
        }

        // CPU ミラー: posW + N * push をセルへ落として残光を最近傍で引く
        auto sampleAt = [&](float wx, float wy, float wz, float nx, float ny, float nz,
                            float push) -> uint8_t {
            int32_t cx = 0, cy = 0, cz = 0;
            if (!acoustic::WorldToCell(g, wx + nx * push, wy + ny * push, wz + nz * push, cx, cy,
                                       cz)) {
                return 0u; // グリッドの外は厳密に 0 (シェーダ側の saturate 判定と同じ契約)
            }
            return field.Glow()[static_cast<size_t>(acoustic::CellIndex(g, cx, cy, cz))];
        };

        // 壁 (x=8) の -X 面のワールド座標。法線は -X (音源のほう)
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(g, 8, 0, 8, wx, wy, wz);
        const float faceX = wx - g.cellSize * 0.5f; // セル中心から半セル戻ると面
        const float push = 0.75f * g.cellSize;      // RenderSystem が view へ入れる値

        check(sampleAt(faceX, wy, wz, 0.0f, 0.0f, 0.0f, 0.0f) == 0u
                  || field.IsSolid(8, 0, 8),
              "push: sampling the wall surface itself lands on a cell that is never lit");
        check(sampleAt(faceX, wy, wz, -1.0f, 0.0f, 0.0f, push) > 0u,
              "push: pushing along the normal picks up the lit open cell (this is what makes "
              "a wall face glow at all)");
        // 押し出しが**行き過ぎない**こと: 2 セル先まで飛ぶと薄い壁の裏側を拾う
        int32_t px = 0, py = 0, pz = 0;
        const bool inside = acoustic::WorldToCell(g, faceX - push, wy, wz, px, py, pz);
        check(inside && px == 7, "push: 0.75 cells lands in the adjacent cell, never two away");
        // グリッドの外は厳密に 0 (CLAMP サンプラの外側漏れを殺している側の契約)
        check(sampleAt(-100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f) == 0u,
              "push: outside the volume the sample is exactly zero, never clamped-to-edge");
    }

    // ---- (21) 流れ場: 迷路で勾配を降りると必ず目標に着く (M65f) ----
    // ★**壁を通り抜けないこと**と**同じ入力なら 2 回計算してビット同一**の 2 本が本体。
    //   後者は「キャッシュを持たない」判断が守られている証拠でもある
    {
        AcousticField field;
        AcousticGridDesc g;
        const bool ok = acoustic::MakeGridDesc(24, 2, 24, 0.5f, 0.0f, 0.0f, 0.0f, g);
        MYE_CHECK(ok);
        field.DebugSetGrid(g, MakeLMaze2(g));

        AcousticNav nav;
        nav.Sync(field);
        check(nav.Valid(), "nav: the coarse grid is built from the field");
        // navCellRatio は AcousticVolume 由来 (DebugSetGrid 経路の既定は 2)
        check(nav.Grid().dimX == 12 && nav.Grid().dimZ == 12,
              "nav: 24 cells at ratio 2 becomes 12 coarse cells");

        nav.BeginTick();
        float tx = 0.0f, ty = 0.0f, tz = 0.0f;
        acoustic::CellToWorldCenter(g, 20, 0, 20, tx, ty, tz); // L 字の**遠い端**
        const int fi = nav.BuildFlowField(tx, ty, tz);
        check(fi >= 0, "nav: a flow field can be built toward the far end of the maze");

        // 勾配降下: 廊下の反対の端から出発して、必ず目標セルへ着く
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        acoustic::CellToWorldCenter(g, 2, 0, 2, px, py, pz);
        int steps = 0;
        bool leftMaze = false;
        while (steps < 400 && !nav.ReachedTarget(fi, px, py, pz)) {
            float dx = 0.0f, dz = 0.0f;
            if (!nav.SampleDirection(fi, px, py, pz, dx, dz)) {
                break;
            }
            // 粗セル半分ずつ進む (実機の CC より細かい刻みで、通り抜けを見逃さない)
            px += dx * nav.Grid().cellSize * 0.5f;
            pz += dz * nav.Grid().cellSize * 0.5f;
            int32_t cx = 0, cy = 0, cz = 0;
            if (!acoustic::WorldToCell(g, px, py, pz, cx, cy, cz) || field.IsSolid(cx, 0, cz)) {
                leftMaze = true; // 壁の中か箱の外へ出た
                break;
            }
            ++steps;
        }
        check(!leftMaze, "nav: gradient descent never walks into a wall");
        check(nav.ReachedTarget(fi, px, py, pz), "nav: gradient descent reaches the target");
        // L 字を曲がった証拠: 直線距離 (18 セル) よりずっと多くの歩数が要る
        // 粗セル 12x12 で (1,1) -> (10,10)。**直線に飛べれば 9 斜め歩 = 18 半歩**だが、
        // L 字を回ると 18 歩 = 36 半歩前後になる。25 はその間に置いた境
        check(steps > 25, "nav: the path is far longer than the straight line (it turned a corner)");

        // ★同じ目標なら 2 回目もビット同一 (キャッシュを持たない = 純関数である証拠)
        AcousticNav nav2;
        nav2.Sync(field);
        nav2.BeginTick();
        const int fi2 = nav2.BuildFlowField(tx, ty, tz);
        float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f;
        bool same = (fi2 == fi);
        for (int32_t cz = 0; cz < g.dimZ && same; ++cz) {
            for (int32_t cx = 0; cx < g.dimX && same; ++cx) {
                float wx = 0.0f, wy = 0.0f, wz = 0.0f;
                acoustic::CellToWorldCenter(g, cx, 0, cz, wx, wy, wz);
                const bool ra = nav.SampleDirection(fi, wx, wy, wz, ax, az);
                const bool rb = nav2.SampleDirection(fi2, wx, wy, wz, bx, bz);
                same = (ra == rb) && (ax == bx) && (az == bz);
            }
        }
        check(same, "nav: the same target rebuilds a bit-identical field (no hidden cache)");

        // 壁の向こうの閉じた区画からは「進めない」= false を返す (0 を返して迷わない)
        float ox = 0.0f, oy = 0.0f, oz = 0.0f;
        acoustic::CellToWorldCenter(g, 10, 0, 10, ox, oy, oz);
        float ddx = 0.0f, ddz = 0.0f;
        check(!nav.SampleDirection(fi, ox, oy, oz, ddx, ddz),
              "nav: a cell cut off from the target reports 'no direction' rather than guessing");
    }

    // 多目標: 8 体に加え、帰還判定などの追加要求があっても既存の場を失わない。
    {
        AcousticField field;
        AcousticGridDesc g;
        MYE_CHECK(acoustic::MakeGridDesc(32, 2, 32, 0.5f, 0.0f, 0.0f, 0.0f, g));
        field.DebugSetGrid(g, std::vector<uint8_t>(32 * 2 * 32, 0));
        AcousticNav nav;
        nav.Sync(field);
        nav.BeginTick();
        int fields[12] = {};
        float targets[12][3] = {};
        for (int i = 0; i < 12; ++i) {
            acoustic::CellToWorldCenter(g, 2 + i * 2, 0, 26,
                                       targets[i][0], targets[i][1], targets[i][2]);
            fields[i] = nav.BuildFlowField(targets[i][0], targets[i][1], targets[i][2]);
        }
        check(nav.FieldCount() == 12, "nav: twelve distinct targets are served in one tick");
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        acoustic::CellToWorldCenter(g, 2, 0, 2, sx, sy, sz);
        bool allMove = true;
        bool allShared = true;
        for (int i = 0; i < 12; ++i) {
            float dx = 0.0f, dz = 0.0f;
            allMove = nav.SampleDirection(fields[i], sx, sy, sz, dx, dz)
                && (dx != 0.0f || dz != 0.0f) && allMove;
            allShared = nav.BuildFlowField(targets[i][0], targets[i][1], targets[i][2])
                == fields[i] && allShared;
        }
        check(allMove, "nav: late requests and earlier field handles all remain usable");
        check(allShared && nav.FieldCount() == 12, "nav: repeated goals share their field");
        nav.BeginTick();
        check(nav.FieldCount() == 0, "nav: multiple goals leave no history across ticks");
    }

    // 本編予算の 8 体が別の帰還目標を持つ実際の AgentSystem 経路。
    {
        Scene scene;
        World& w = scene.GetWorld();
        auto vol = scene.CreateGameObjectTracked("Volume");
        auto* av = vol.AddComponent<AcousticVolumeComponent>();
        av->dimX = 96;
        av->dimY = 2;
        av->dimZ = 96;
        av->cellSize = 0.5f;
        EntityID ids[8] = {};
        for (int i = 0; i < 8; ++i) {
            auto agent = scene.CreateGameObjectTracked("Agent");
            agent.SetLocalPosition(-10.0f + i * 2.0f, 0.0f, -10.0f);
            agent.AddComponent<CharacterControllerComponent>();
            auto* brain = agent.AddComponent<AgentBrainComponent>();
            brain->home = { -10.0f + i * 2.0f, 0.0f, 10.0f };
            brain->target = brain->home;
            brain->state = kAgentReturn;
            brain->emitEveryTicks = 0;
            ids[i] = agent.Id();
        }
        w.ApplyStructuralChanges();
        TransformSystem ts;
        ts.Update(w);
        AcousticField field;
        field.Sync(w);
        AgentSystem sys;
        bool allMove = true;
        for (uint64_t tick = 1; tick <= 3; ++tick) {
            sys.Update(w, field, tick);
            for (const EntityID id : ids) {
                const auto* cc = w.GetComponent<CharacterControllerComponent>(id);
                allMove = cc != nullptr && cc->moveInput.z > 0.0f && allMove;
            }
        }
        check(allMove, "agent: all eight distinct return goals produce movement on every tick");
    }

    // 譲り合い: 正面から来る 2 体は index の大きいほうが横へ退く / 同じ向きの後続は譲らない。
    // ★ソリッド Collider を併用した敵の正面衝突が永久に止まる実害から来た規則
    {
        Scene scene;
        World& w = scene.GetWorld();
        auto vol = scene.CreateGameObjectTracked("Volume");
        auto* av = vol.AddComponent<AcousticVolumeComponent>();
        av->dimX = av->dimZ = 48; av->dimY = 2; av->cellSize = 0.5f;
        auto make = [&](const char* name, float x, float z, float tx, float tz) {
            auto e = scene.CreateGameObjectTracked(name);
            e.SetLocalPosition(x, 0.0f, z);
            e.AddComponent<CharacterControllerComponent>();
            auto* brain = e.AddComponent<AgentBrainComponent>();
            brain->home = { tx, 0.0f, tz };
            brain->target = brain->home;
            brain->state = kAgentReturn;
            brain->emitEveryTicks = 0;
            return e.Id();
        };
        // 生成順 = index 昇順。Lead が最小で東へ、Oncoming は 1m 先から西へ (正面)、
        // Follower は Lead の 1m 後ろから同じ東へ
        // ★z は粗セル (1m) の中心線 0.5 に置く。SampleDirection は隣のセル中心へ向かうので、
        //   セル境界 (z=0) に置くと中心線へ寄る z 成分が乗り、「進路を保つ = z 成分 0」が読めない
        const EntityID lead = make("Lead", -0.5f, 0.5f, 8.0f, 0.5f);
        const EntityID oncoming = make("Oncoming", 0.5f, 0.5f, -8.0f, 0.5f);
        const EntityID follower = make("Follower", -1.5f, 0.5f, 8.0f, 0.5f);
        w.ApplyStructuralChanges();
        TransformSystem ts;
        ts.Update(w);
        AcousticField field;
        field.Sync(w);
        AgentSystem sys;
        sys.Update(w, field, 1);
        const auto& l = w.GetComponent<CharacterControllerComponent>(lead)->moveInput;
        const auto& o = w.GetComponent<CharacterControllerComponent>(oncoming)->moveInput;
        const auto& f = w.GetComponent<CharacterControllerComponent>(follower)->moveInput;
        check(l.x > 0.0f && l.z == 0.0f, "yield: the lower index keeps its course");
        check(o.x == 0.0f && std::fabs(o.z) > 0.0f, "yield: the oncoming higher index steps aside");
        check(f.x > 0.0f && f.z == 0.0f, "yield: a follower heading the same way does not yield");
    }

    // 完成した光は耳だけの敵にも効く。通常照明や消灯後には障害を残さない。
    {
        Scene scene;
        World& w = scene.GetWorld();
        auto vol = scene.CreateGameObjectTracked("Volume");
        auto* av = vol.AddComponent<AcousticVolumeComponent>();
        av->dimX = av->dimZ = 48; av->dimY = 2; av->cellSize = 0.5f;
        auto lamp = scene.CreateGameObjectTracked("Sanctuary");
        auto* lc = lamp.AddComponent<LightComponent>();
        lc->type = 1; lc->safeRadius = 2.5f;
        auto enemy = scene.CreateGameObjectTracked("EarOnly");
        enemy.SetLocalPosition(-8.0f, 0.0f, 0.0f);
        enemy.AddComponent<CharacterControllerComponent>();
        auto* brain = enemy.AddComponent<AgentBrainComponent>();
        brain->state = kAgentReturn;
        brain->home = {8.0f, 0.0f, 0.0f}; brain->target = brain->home;
        brain->emitEveryTicks = 0;
        w.ApplyStructuralChanges();
        TransformSystem transforms;
        transforms.Update(w);
        AcousticField field; field.Sync(w);
        AgentSystem sys; sys.Update(w, field, 1);
        const auto& g = sys.Nav().Grid();
        int32_t cx = 0, cy = 0, cz = 0;
        acoustic::WorldToCell(g, 0.0f, 0.0f, 0.0f, cx, cy, cz);
        check(sys.Nav().IsSolid(cx, cy, cz), "sanctuary: completed light excludes navigation for ear-only enemy");
        AcousticNav nav;
        nav.Sync(field); nav.BeginTick(); nav.ExcludeCircle(0.0f, 0.0f, 2.5f);
        const int goal = nav.BuildFlowField(8.0f, 0.0f, 0.0f);
        float x = -8.0f, z = 0.0f;
        bool outside = true;
        for (int step = 0; step < 200 && !nav.ReachedTarget(goal, x, 0.0f, z); ++step) {
            float dx = 0.0f, dz = 0.0f;
            if (!nav.SampleDirection(goal, x, 0.0f, z, dx, dz)) break;
            x += dx * 0.25f; z += dz * 0.25f;
            outside = outside && x * x + z * z > 2.5f * 2.5f;
        }
        check(outside && nav.ReachedTarget(goal, x, 0.0f, z),
              "sanctuary: path goes around the light and reaches a goal beyond it");
        check(nav.BuildFlowField(0.0f, 0.0f, 0.0f) >= 0,
              "sanctuary: a sound inside the light maps to an outside investigation goal");
        nav.BeginTick();
        check(!nav.IsSolid(cx, cy, cz), "sanctuary: exclusions have no hidden history across ticks");
        enemy.SetLocalPosition(1.0f, 0.0f, 0.0f); transforms.Update(w);
        sys.Update(w, field, 2);
        check(w.GetComponent<CharacterControllerComponent>(enemy.Id())->moveInput.x > 0.0f,
              "sanctuary: an enemy already inside retreats outwards");
        w.GetComponent<LightComponent>(lamp.Id())->intensity = 0.0f;
        sys.Update(w, field, 3);
        check(!sys.Nav().IsSolid(cx, cy, cz), "sanctuary: extinguished light releases navigation");
        auto* light = w.GetComponent<LightComponent>(lamp.Id());
        light->intensity = 1.0f;
        const uint64_t hash = HashWorld(w);
        light->safeRadius = 0.0f;
        check(HashWorld(w) != hash, "sanctuary: radius participates in world hashing");
        light->safeRadius = 2.5f;
        const auto json = SceneSerializer::SaveToJson(scene);
        Scene loaded;
        check(SceneSerializer::LoadFromJson(loaded, json), "sanctuary: scene loads with the radius field");
        bool found = false;
        const ComponentTypeId req[] = { LightComponent::sTypeId };
        loaded.GetWorld().ForEachArchetype(req, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(LightComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                found = found || static_cast<LightComponent*>(arch.GetPtr(ci, row))->safeRadius == 2.5f;
            }
        });
        check(found, "sanctuary: scene roundtrip preserves safe radius");
        SimRefs refs = {}; refs.scene = &scene;
        std::vector<std::byte> blob;
        check(CaptureSimSnapshot(refs, blob), "sanctuary: snapshot captures radius");
        light->safeRadius = 0.0f;
        const bool restored = RestoreSimSnapshot(refs, blob.data(), blob.size());
        check(restored && w.GetComponent<LightComponent>(lamp.Id())->safeRadius == 2.5f,
              "sanctuary: snapshot restores radius");
    }

    // 入口を光で塞がれた部屋 (三校: 立てこもりは仕様)。辿れない目標は辿れる最寄りへ
    // 差し替わり、敵は入口の手前まで来て固まらない。
    // 粗グリッド 12x1x12 (1m)。x=6 の列が壁で、z=5..6 の 2m だけが入口。光は入口の真ん中
    {
        AcousticGridDesc g;
        MYE_CHECK(acoustic::MakeGridDesc(24, 2, 24, 0.5f, 0.0f, 0.0f, 0.0f, g));
        std::vector<uint8_t> occ(static_cast<size_t>(g.CellCount()), 0u);
        for (int32_t y = 0; y < g.dimY; ++y) {
            for (int32_t z = 0; z < g.dimZ; ++z) {
                if (z >= 10 && z <= 13) {
                    continue; // 入口
                }
                for (int32_t x = 12; x <= 13; ++x) {
                    occ[static_cast<size_t>(acoustic::CellIndex(g, x, y, z))] = 1u;
                }
            }
        }
        // ★MakeGridDesc は**中心**を原点に置く (最小角は -6m)。座標は最小角からの m で組む
        const float ox = g.minX, oz = g.minZ;
        const float kY = g.minY + 0.5f;                            // 粗セル y=0 の中ほど
        const float kDoorX = ox + 6.5f, kDoorZ = oz + 6.0f;        // 入口の真ん中
        const float kStartX = ox + 2.5f, kStartZ = oz + 6.5f;      // 部屋の外 (西側)
        const float kInnerX = ox + 10.5f, kInnerZ = oz + 6.5f;     // 塞がれた部屋の奥
        constexpr float kRadius = 2.5f;
        AcousticField field;
        field.DebugSetGrid(g, occ);

        AcousticNav nav;
        nav.Sync(field);
        nav.BeginTick();
        nav.ExcludeCircle(kDoorX, kDoorZ, kRadius);
        const int inner = nav.BuildFlowField(kInnerX, kY, kInnerZ);
        float dx = 0.0f, dz = 0.0f;
        check(inner >= 0 && !nav.SampleDirection(inner, kStartX, kY, kStartZ, dx, dz),
              "sealed door: the light cuts the room off (precondition: no direction)");
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        const bool swapped = nav.NearestReachable(inner, kStartX, kY, kStartZ, nx, ny, nz);
        const float ex = nx - kDoorX, ez = nz - kDoorZ;
        check(swapped && nx < ox + 6.0f
                  && ex * ex + ez * ez <= (kRadius + 2.0f) * (kRadius + 2.0f),
              "sealed door: an unreachable goal maps to a reachable spot by the door, this side");
        float nx2 = 0.0f, ny2 = 0.0f, nz2 = 0.0f;
        check(nav.NearestReachable(inner, kStartX, kY, kStartZ, nx2, ny2, nz2) && nx2 == nx
                  && ny2 == ny && nz2 == nz,
              "sealed door: the substitute is bit-identical on a second query");
        check(!nav.NearestReachable(inner, ox + 11.5f, kY, oz + 2.5f, nx2, ny2, nz2),
              "sealed door: a goal that is already reachable is left alone");

        // AgentSystem 経路。物理は通さず moveInput をそのまま積分する (CC の壁ずりは見ない)
        struct SealedRun {
            bool reached = false;   // untilState に入った
            bool outside = true;    // 一度も光の内側へ入らなかった
            float travelled = 0.0f; // 出発点からの水平距離 (固まっていない証拠)
        };
        auto runSealed = [&](bool eye, int32_t startState, int32_t untilState) {
            SealedRun r;
            Scene scene;
            World& w = scene.GetWorld();
            auto lamp = scene.CreateGameObjectTracked("DoorLight");
            lamp.SetLocalPosition(kDoorX, kY, kDoorZ);
            auto* lc = lamp.AddComponent<LightComponent>();
            lc->type = 1;
            lc->intensity = 1.0f;
            lc->safeRadius = kRadius;
            auto enemy = scene.CreateGameObjectTracked("Outside");
            enemy.SetLocalPosition(kStartX, kY, kStartZ);
            enemy.AddComponent<CharacterControllerComponent>();
            auto* brain = enemy.AddComponent<AgentBrainComponent>();
            brain->state = startState;
            brain->home = { kInnerX, kY, kInnerZ }; // 巣も塞がれた部屋の中
            brain->target = brain->home;
            brain->emitEveryTicks = 0;
            if (eye) {
                enemy.AddComponent<LightSeekerComponent>();
            }
            w.ApplyStructuralChanges();
            TransformSystem transforms;
            transforms.Update(w);
            AgentSystem sys;
            constexpr float kDt = 1.0f / 60.0f;
            float x = kStartX, z = kStartZ;
            for (uint64_t tick = 1; tick <= 1200 && !r.reached; ++tick) {
                sys.Update(w, field, tick, kDt);
                r.reached = w.GetComponent<AgentBrainComponent>(enemy.Id())->state == untilState;
                const auto& mv = w.GetComponent<CharacterControllerComponent>(enemy.Id())->moveInput;
                x += mv.x * kDt;
                z += mv.z * kDt;
                const float ox = x - kDoorX, oz = z - kDoorZ;
                r.outside = r.outside && ox * ox + oz * oz > kRadius * kRadius;
                enemy.SetLocalPosition(x, kY, z);
                transforms.Update(w);
            }
            r.travelled = std::sqrt((x - kStartX) * (x - kStartX) + (z - kStartZ) * (z - kStartZ));
            return r;
        };
        const SealedRun back = runSealed(false, kAgentReturn, kAgentPatrol);
        check(back.reached && back.travelled > 2.0f && back.outside,
              "sealed door: returning to a nest inside walks to the door and resumes patrol");
        const SealedRun lured = runSealed(true, kAgentPatrol, kAgentSearch);
        check(lured.reached && lured.travelled > 2.0f && lured.outside,
              "sealed door: a light-seeker drawn to a sealed light reaches the door and searches");
    }

    // 腰高の障害物 (書架・閉じた扉) の「上」を通らない (三校 2026-09-13)。
    // ★音響グリッドは立体なので、障害物の上の空いた層は開。登りを禁じないと流れ場が上を越える
    //   道を張り、敵は水平成分だけを受け取って側面を押し続ける (三校 stage2: 書架の前で速度 0)。
    // 細 24x6x24 (粗 12x3x12、1m)。粗 x=6 の列に高さ 2 層の壁を立て、最上層 (粗 y=2) は空ける
    {
        AcousticGridDesc g;
        MYE_CHECK(acoustic::MakeGridDesc(24, 6, 24, 0.5f, 0.0f, 0.0f, 0.0f, g));
        auto makeWall = [&](bool gap) {
            std::vector<uint8_t> occ(static_cast<size_t>(g.CellCount()), 0u);
            for (int32_t y = 0; y < 4; ++y) { // 細 y=0..3 = 粗 y=0..1
                for (int32_t z = 0; z < g.dimZ; ++z) {
                    if (gap && z >= 20) {
                        continue; // 粗 z=10..11 だけ抜け道
                    }
                    for (int32_t x = 12; x <= 13; ++x) {
                        occ[static_cast<size_t>(acoustic::CellIndex(g, x, y, z))] = 1u;
                    }
                }
            }
            return occ;
        };
        const float ox = g.minX, oz = g.minZ;
        const float kFloorY = g.minY + 0.5f; // 粗 y=0 (床の層)
        const float kAirY = g.minY + 2.5f;   // 粗 y=2 (壁より上の空いた層)
        const float kWestX = ox + 2.5f, kEastX = ox + 10.5f, kZ = oz + 2.5f;

        // 抜け道が無い: 壁の上が開いていても越える道にはならない
        {
            AcousticField field;
            field.DebugSetGrid(g, makeWall(false));
            AcousticNav nav;
            nav.Sync(field);
            nav.BeginTick();
            int32_t cx = 0, cy = 0, cz = 0;
            acoustic::WorldToCell(nav.Grid(), ox + 6.5f, kAirY, kZ, cx, cy, cz);
            check(cy == 2 && !nav.IsSolid(cx, cy, cz) && nav.IsSolid(cx, 0, cz),
                  "climb: precondition - the wall is solid on the floor and open above");
            const int fi = nav.BuildFlowField(kEastX, kFloorY, kZ);
            float dx = 0.0f, dz = 0.0f;
            check(fi >= 0 && !nav.SampleDirection(fi, kWestX, kFloorY, kZ, dx, dz),
                  "climb: open air above a wall is not a way over it (no direction)");
            float nx = 0.0f, ny = 0.0f, nz = 0.0f;
            check(nav.NearestReachable(fi, kWestX, kFloorY, kZ, nx, ny, nz) && nx < ox + 6.0f
                      && ny < g.minY + 1.0f,
                  "climb: the substitute goal stays on this side, on the floor layer");
        }

        // 抜け道がある: 勾配降下は床の層のまま抜け道を回って着く
        {
            AcousticField field;
            field.DebugSetGrid(g, makeWall(true));
            AcousticNav nav;
            nav.Sync(field);
            nav.BeginTick();
            const int fi = nav.BuildFlowField(kEastX, kFloorY, kZ);
            float px = kWestX, pz = kZ;
            bool crossedAtGap = true;
            bool stalled = false;
            for (int step = 0; step < 400 && !nav.ReachedTarget(fi, px, kFloorY, pz); ++step) {
                float dx = 0.0f, dz = 0.0f;
                if (!nav.SampleDirection(fi, px, kFloorY, pz, dx, dz)) {
                    stalled = true;
                    break;
                }
                px += dx * nav.Grid().cellSize * 0.5f;
                pz += dz * nav.Grid().cellSize * 0.5f;
                // ★斜めの 1 歩が角を半セル未満かすめるのは許す (実機では CC が壁ずりで吸収する)。
                //   見るのは「抜け道以外の所で壁の列を越えたか」だけ
                if (px >= ox + 6.0f && px < ox + 7.0f) {
                    crossedAtGap = crossedAtGap && pz >= oz + 9.5f;
                }
            }
            check(crossedAtGap, "climb: the path crosses the wall line only at the gap");
            check(!stalled && nav.ReachedTarget(fi, px, kFloorY, pz),
                  "climb: the floor-layer path reaches the goal without stalling");
        }

        // 空中で鳴った音は真下の床の層へ落ちる (着いたと言えるのは床の層)
        {
            AcousticField field;
            field.DebugSetGrid(g, std::vector<uint8_t>(static_cast<size_t>(g.CellCount()), 0u));
            AcousticNav nav;
            nav.Sync(field);
            nav.BeginTick();
            const int fi = nav.BuildFlowField(ox + 8.5f, kAirY, oz + 8.5f);
            check(fi >= 0 && nav.ReachedTarget(fi, ox + 8.5f, kFloorY, oz + 8.5f)
                      && !nav.ReachedTarget(fi, ox + 8.5f, kAirY, oz + 8.5f),
                  "climb: a goal in the air drops to the floor layer below it");
            float dx = 0.0f, dz = 0.0f;
            check(nav.SampleDirection(fi, ox + 2.5f, kFloorY, oz + 2.5f, dx, dz),
                  "climb: a floor-layer agent is guided toward a sound in the air");
        }
    }

    // ---- (22) 敵 FSM: 5 状態の遷移と「警戒中は 1 波も出さない」(M65f) ----
    // ★企画 §6-3 の中核。**警戒中に 1 本でも波が出ると「音を立てた代償」が消える**
    {
        Scene scene;
        World& w = scene.GetWorld();

        // 音響ボリューム (存在ゲートを開ける)
        GameObject vol = scene.CreateGameObjectTracked("Volume");
        auto* av = vol.AddComponent<AcousticVolumeComponent>();
        av->dimX = 24;
        av->dimY = 2;
        av->dimZ = 24;
        av->cellSize = 0.5f;

        GameObject agent = scene.CreateGameObjectTracked("Agent");
        agent.SetLocalPosition(1.0f, 0.0f, 1.0f);
        auto* cc = agent.AddComponent<CharacterControllerComponent>();
        (void)cc;
        auto* brain = agent.AddComponent<AgentBrainComponent>();
        brain->home = { 1.0f, 0.0f, 1.0f };
        brain->target = brain->home;
        brain->alertTicks = 5;
        brain->searchTicks = 10;
        brain->loseTicks = 4;
        brain->emitEveryTicks = 2; // **毎 tick 近く鳴る**設定にして「鳴らない」を厳しく見る
        brain->emitLoudness = 0.5f;
        auto* ear = agent.AddComponent<AcousticListenerComponent>();
        ear->threshold = 0.0f;
        w.ApplyStructuralChanges();
        TransformSystem ts;
        ts.Update(w);

        AcousticField field;
        field.Sync(w);
        check(field.HasVolume(), "fsm: the volume gate is open");

        AgentSystem sys;
        uint64_t tick = 1;
        auto step = [&]() {
            sys.Update(w, field, tick);
            field.Advance(&w, tick);
            ++tick;
        };
        auto brainOf = [&]() { return w.GetComponent<AgentBrainComponent>(agent.Id()); };
        auto earOf = [&]() { return w.GetComponent<AcousticListenerComponent>(agent.Id()); };

        // 巡回から始まる
        step();
        check(brainOf()->state == kAgentPatrol, "fsm: starts in patrol");

        // 音を聞かせる → 警戒
        earOf()->lastHeardTick = tick;
        earOf()->lastHeardPos = { -2.0f, 0.0f, -2.0f };
        earOf()->lastLoudness = 1.0f;
        step();
        check(brainOf()->state == kAgentAlert, "fsm: hearing a sound this tick enters alert");

        // ★警戒のあいだ**1 本も波を立てない**。alertTicks ぶん回して数える
        //   (emitEveryTicks=2 なので、鳴る実装ならこの区間で必ず何本か出る)
        // ★遷移は `stateTicks >= alertTicks` なので、入った tick を 0 として
        //   alertTicks + 1 tick 目に抜ける。**回数を数え打ちにせずループで待つ** —
        //   数え打ちにすると閾値の意味を 1 tick 変えただけでテストが壊れる
        size_t wavesDuringAlert = 0;
        int alertSteps = 0;
        const uint64_t alertFrom = tick;
        while (brainOf()->state == kAgentAlert && alertSteps < 20) {
            step();
            // ★**警戒のまま終わった tick だけ**数える。遷移した tick は既に追跡なので
            //   鳴って当然 (状態遷移は発音より前に評価される)
            if (brainOf()->state == kAgentAlert) {
                wavesDuringAlert += CountWavesFrom(field, agent.Id(), alertFrom);
            }
            ++alertSteps;
        }
        check(alertSteps >= brain->alertTicks,
              "fsm: alert is held for at least alertTicks before it releases");
        check(wavesDuringAlert == 0, "fsm: an alert agent emits not one single wave (design 6-3)");
        check(brainOf()->state == kAgentChase, "fsm: alert times out into chase");
        check(brainOf()->target.x == -2.0f && brainOf()->target.z == -2.0f,
              "fsm: chase heads for the last heard position");

        // 追跡中は音を出す (警戒との対比。ここが 0 だと「常に黙る」実装になっている)
        const uint64_t chaseFrom = tick;
        size_t wavesDuringChase = 0;
        for (int i = 0; i < 6; ++i) {
            step();
            wavesDuringChase += CountWavesFrom(field, agent.Id(), chaseFrom);
        }
        check(wavesDuringChase > 0, "fsm: a chasing agent does emit (the silence is alert-only)");

        // 音が途切れて loseTicks 経過 → 探索 → searchTicks 経過 → 帰還
        for (int i = 0; i < 8 && brainOf()->state == kAgentChase; ++i) {
            step();
        }
        check(brainOf()->state == kAgentSearch, "fsm: losing the trail falls back to search");
        for (int i = 0; i < 20 && brainOf()->state == kAgentSearch; ++i) {
            step();
        }
        check(brainOf()->state == kAgentReturn, "fsm: search times out into return");

        // 探索中に音が戻れば即追跡 (帰還からも同じ)
        earOf()->lastHeardTick = tick;
        earOf()->lastHeardPos = { 2.0f, 0.0f, 2.0f };
        step();
        check(brainOf()->state == kAgentAlert,
              "fsm: a fresh sound while returning re-enters alert (never straight to chase)");
    }

    // ---- (23) 聴覚の鏡: 自分の音は聞かない / 閾値未満は届かない (M65f) ----
    // ★自分の音を自分で聞くと**全個体が永久に追跡状態**になる。絵では
    //   「なぜか全員こちらを見ている」としか見えないので、ここで固定しておく
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject vol = scene.CreateGameObjectTracked("Volume");
        auto* av = vol.AddComponent<AcousticVolumeComponent>();
        av->dimX = 24;
        av->dimY = 2;
        av->dimZ = 24;
        av->cellSize = 0.5f;
        GameObject who = scene.CreateGameObjectTracked("Ear");
        who.SetLocalPosition(0.0f, 0.0f, 0.0f);
        auto* ear = who.AddComponent<AcousticListenerComponent>();
        ear->threshold = 0.0f;
        w.ApplyStructuralChanges();
        TransformSystem ts;
        ts.Update(w);

        AcousticField field;
        field.Sync(w);
        // 自分が原点で鳴らす → 何 tick 進めても鏡は空のまま
        check(field.Emit(who.Id(), 0.0f, 0.0f, 0.0f, 1.0f, 8.0f, 0, 1, 1), "mirror: self emit ok");
        for (int i = 0; i < 8; ++i) {
            field.Advance(&w, static_cast<uint64_t>(2 + i));
        }
        check(w.GetComponent<AcousticListenerComponent>(who.Id())->lastHeardTick == 0,
              "mirror: an agent never hears the wave it emitted itself");

        // 他人の音は聞く
        const EntityID other = { 4242u, 1u };
        check(field.Emit(other, 1.0f, 0.0f, 1.0f, 1.0f, 8.0f, 2, 1, 20), "mirror: other emit ok");
        for (int i = 0; i < 8; ++i) {
            field.Advance(&w, static_cast<uint64_t>(21 + i));
        }
        const auto* m = w.GetComponent<AcousticListenerComponent>(who.Id());
        check(m->lastHeardTick != 0, "mirror: a wave from somebody else is heard");
        check(m->lastSourceEntity.index == other.index, "mirror: the source entity is recorded");
        check(m->lastTone == 2, "mirror: the tone survives to the mirror (M65g reads it)");
        check(m->lastLoudness > 0.0f, "mirror: the loudness is the energy at the listener cell");

        // 閾値を上げると同じ音が届かなくなる (= 閾値が本当に効いている)
        w.GetComponent<AcousticListenerComponent>(who.Id())->threshold = 10.0f;
        const uint64_t before = m->lastHeardTick;
        check(field.Emit(other, 1.0f, 0.0f, 1.0f, 1.0f, 8.0f, 2, 1, 40), "mirror: third emit ok");
        for (int i = 0; i < 8; ++i) {
            field.Advance(&w, static_cast<uint64_t>(41 + i));
        }
        check(m->lastHeardTick == before, "mirror: below the threshold nothing is written");

        // ★慣れた音源 (ignoreSource) の波は配られない (2026-09-13、三校)
        {
            auto* cfg = w.GetComponent<AcousticListenerComponent>(who.Id());
            cfg->threshold = 0.0f;
            cfg->ignoreSource = other;
            const uint64_t beforeIgnore = cfg->lastHeardTick;
            check(field.Emit(other, 1.0f, 0.0f, 1.0f, 1.0f, 8.0f, 2, 1, 60), "mirror: ignored emit ok");
            for (int i = 0; i < 8; ++i) {
                field.Advance(&w, static_cast<uint64_t>(61 + i));
            }
            check(cfg->lastHeardTick == beforeIgnore,
                  "mirror: a wave from ignoreSource is never delivered");
        }
        // ★hearAgents=false なら AgentBrain を持つ実体の波は配られない / 既定 (true) なら聞く
        {
            GameObject agent = scene.CreateGameObjectTracked("Agent");
            agent.AddComponent<AgentBrainComponent>();
            w.ApplyStructuralChanges();
            // 構造変更の後はポインタを取り直す
            auto* cfg = w.GetComponent<AcousticListenerComponent>(who.Id());
            cfg->ignoreSource = kNullEntity;
            cfg->hearAgents = false;
            const uint64_t beforeAgent = cfg->lastHeardTick;
            check(field.Emit(agent.Id(), 1.0f, 0.0f, 1.0f, 1.0f, 8.0f, 1, 1, 80), "mirror: agent emit ok");
            for (int i = 0; i < 8; ++i) {
                field.Advance(&w, static_cast<uint64_t>(81 + i));
            }
            cfg = w.GetComponent<AcousticListenerComponent>(who.Id());
            check(cfg->lastHeardTick == beforeAgent,
                  "mirror: hearAgents=false never delivers a wave from an AgentBrain entity");
            cfg->hearAgents = true;
            check(field.Emit(agent.Id(), 1.0f, 0.0f, 1.0f, 1.0f, 8.0f, 1, 1, 100), "mirror: agent emit 2 ok");
            for (int i = 0; i < 8; ++i) {
                field.Advance(&w, static_cast<uint64_t>(101 + i));
            }
            cfg = w.GetComponent<AcousticListenerComponent>(who.Id());
            check(cfg->lastHeardTick != beforeAgent && cfg->lastSourceEntity == agent.Id(),
                  "mirror: hearAgents=true (the default) still hears an AgentBrain entity");
        }
    }

    // ---- (24) 残光パラメータの鏡 (M65h) ----
    // Sync がコンポーネント値を鏡へ写し、範囲外は消費側の既定へ倒れること。
    // ★glowKeepPerTick / glowIntensity は描画レーンの値なので、ここが壊れても
    //   リプレイは割れない = このセルフテストだけが検出の網
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject vol = scene.CreateGameObjectTracked("Volume");
        {
            auto* av = vol.AddComponent<AcousticVolumeComponent>();
            av->dimX = 8;
            av->dimY = 2;
            av->dimZ = 8;
            av->cellSize = 0.5f;
            av->glowKeepPerTick = 0.5f;
            av->glowIntensity = -1.0f; // 負は Sync 側で 0 (消灯) へ丸める
            av->glowAlbedoMix = 3.0f;  // 1 を超える分は Sync 側で 1 へ丸める
        }
        w.ApplyStructuralChanges();
        TransformSystem ts;
        ts.Update(w);

        AcousticField field;
        field.Sync(w);
        check(field.GlowKeepPerTick() == 0.5f, "tune: Sync mirrors glowKeepPerTick");
        check(field.GlowIntensity() == 0.0f, "tune: a negative glowIntensity clamps to zero");
        check(field.GlowAlbedoMix() == 1.0f, "tune: glowAlbedoMix above 1 clamps to 1");

        // keep=0.5 は 1 tick で半分になる = 鏡の値が DecayVisual まで実際に流れる形
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        acoustic::CellToWorldCenter(field.Grid(), 4, 1, 4, wx, wy, wz);
        check(field.Emit(EntityID{ 1, 1 }, wx, wy, wz, 1.0f, 3.0f, 0, 1, 0), "tune: emit ok");
        for (int i = 0; i < 3; ++i) {
            field.Advance(nullptr, 0);
        }
        check(field.VisualActive(), "tune: the probe wave lit at least one cell");
        uint8_t peak = 0;
        for (uint8_t v : field.Glow()) {
            peak = (v > peak) ? v : peak;
        }
        field.DecayVisual(field.GlowKeepPerTick());
        uint8_t half = 0;
        for (uint8_t v : field.Glow()) {
            half = (v > half) ? v : half;
        }
        check(peak > 0 && half <= static_cast<uint8_t>(peak / 2),
              "tune: keep=0.5 halves the glow in one tick (truncated)");

        // 0 (未設定) は DecayVisual 側の範囲ガードで既定 0.995 へ倒れる = ゆっくり減る。
        // ★AddComponent 後の生ポインタは構造変更で動くので、書き換えは取り直してから
        if (auto* av = w.GetComponent<AcousticVolumeComponent>(vol.Id())) {
            av->glowKeepPerTick = 0.0f;
            av->glowIntensity = 2.0f;
            av->glowAlbedoMix = 0.25f;
        }
        field.Sync(w);
        check(field.GlowKeepPerTick() == 0.0f && field.GlowIntensity() == 2.0f
                  && field.GlowAlbedoMix() == 0.25f,
              "tune: Sync re-mirrors after the component changes");
        const uint8_t before = half;
        field.DecayVisual(field.GlowKeepPerTick());
        uint8_t after = 0;
        for (uint8_t v : field.Glow()) {
            after = (v > after) ? v : after;
        }
        check(after < before && after + 3 >= before,
              "tune: keep=0 falls back to the engine default (slow decay)");

        // 負の混ぜ具合は 0 (= 従来の色) へ倒れる
        if (auto* av = w.GetComponent<AcousticVolumeComponent>(vol.Id())) {
            av->glowAlbedoMix = -1.0f;
        }
        field.Sync(w);
        check(field.GlowAlbedoMix() == 0.0f, "tune: a negative glowAlbedoMix clamps to zero");

        // 残光の間引き (2026-09-13)。既定 0 と負は 1 = 毎 tick、上限超えは上限、N は tick % N == 0 だけ
        check(field.GlowDecayEveryTicks() == 1 && field.ShouldDecayVisual(7),
              "tune: the default glowDecayEveryTicks (0) decays every tick");
        if (auto* av = w.GetComponent<AcousticVolumeComponent>(vol.Id())) {
            av->glowDecayEveryTicks = -5;
        }
        field.Sync(w);
        check(field.GlowDecayEveryTicks() == 1, "tune: a negative glowDecayEveryTicks decays every tick");
        if (auto* av = w.GetComponent<AcousticVolumeComponent>(vol.Id())) {
            av->glowDecayEveryTicks = 1000;
        }
        field.Sync(w);
        check(field.GlowDecayEveryTicks() == AcousticField::kGlowDecayEveryMax,
              "tune: glowDecayEveryTicks clamps to the cap");
        if (auto* av = w.GetComponent<AcousticVolumeComponent>(vol.Id())) {
            av->glowDecayEveryTicks = 3;
        }
        field.Sync(w);
        check(field.ShouldDecayVisual(0) && !field.ShouldDecayVisual(1) && !field.ShouldDecayVisual(2)
                  && field.ShouldDecayVisual(3) && field.ShouldDecayVisual(6),
              "tune: glowDecayEveryTicks=3 decays only on every third tick");

        // 混ぜ具合は CB の z 席 (M65e で予約のまま空いていた席) で運ぶ。
        // ★未バインドは全部 0 のまま = シェーダは分岐に入らない (w も 0)
        RenderView view;
        view.acousticIntensity = 1.0f;
        view.acousticAlbedoMix = 0.25f;
        const AcousticCB bound = MakeAcousticCB(view, true);
        check(bound.params.z == 0.25f && bound.params.w == 1.0f,
              "tune: MakeAcousticCB carries glowAlbedoMix in params.z");
        check(MakeAcousticCB(view, false).params.z == 0.0f,
              "tune: an unbound acoustic CB keeps params.z at zero");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Acoustic self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Acoustic self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
