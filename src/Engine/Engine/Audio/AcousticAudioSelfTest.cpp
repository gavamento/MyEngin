//====================================================================================
//                          AcousticAudioSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      09/06/2026
//                                          リスナー場と整形の回帰テスト（T1〜T15）
//====================================================================================
#include "Engine/Engine/Audio/AcousticAudioSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/Core/Check.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/Acoustic/AcousticGrid.h"
#include "Engine/Engine/Audio/AcousticAudio.h"
#include "Engine/Engine/Audio/SpatialMath.h"
#include "Engine/Engine/Replay/SimSnapshot.h"

namespace mye {
namespace {

// ---- 土台 ----
// L 字の 1 セル幅の廊下 (AcousticSelfTest の MakeLMaze と同じ間取り)。
//   腕 1: x in [2,20], z = 2   /   腕 2: x = 20, z in [2,20]
// ★写しにしてあるのは AcousticSelfTest.cpp の無名 namespace に閉じているため。
//   間取りを変えるときは両方を直すこと (どちらも「回り込み」を測る土台で、
//   別の形にすると 2 つのテストが別の主張になってしまう)
AcousticGridDesc MakeMazeGrid()
{
    AcousticGridDesc g;
    const bool ok = acoustic::MakeGridDesc(24, 1, 24, 0.5f, 0.0f, 0.0f, 0.0f, g);
    MYE_CHECK(ok);
    return g;
}

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

// 何も遮らない箱 (開放度の分母と「自由空間なら Dial == 閉形式」の検査用)
AcousticGridDesc MakeFreeGrid()
{
    AcousticGridDesc g;
    const bool ok = acoustic::MakeGridDesc(24, 4, 24, 0.5f, 0.0f, 0.0f, 0.0f, g);
    MYE_CHECK(ok);
    return g;
}

AudioVec3 CellCenter(const AcousticGridDesc& g, int32_t cx, int32_t cy, int32_t cz)
{
    AudioVec3 v;
    acoustic::CellToWorldCenter(g, cx, cy, cz, v.x, v.y, v.z);
    return v;
}

// 3D 定位のブレンドを 1 にした素の spatial (整形の効きがそのまま出る形)
AudioSpatial MakeSpatial()
{
    AudioSpatial s;
    s.spatialBlend = 1.0f;
    s.dopplerScale = 1.0f;
    return s;
}

bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// 波を 1 本立てて maxRing まで育てる (probe と突き合わせる基準)。
// ★ring == maxRing で止めるのが要点 — もう 1 回 Advance するとスロットが消える
void GrowWave(AcousticField& field, const AudioVec3& origin, uint32_t rings)
{
    const bool ok = field.Emit(kNullEntity, origin.x, origin.y, origin.z, 1.0f,
                               static_cast<float>(rings) * field.Grid().cellSize, 0, 1, 0);
    MYE_CHECK(ok);
    for (uint32_t i = 0; i < rings; ++i) {
        field.Advance(nullptr, i + 1);
    }
}

} // namespace

bool RunAcousticAudioSelfTest()
{
    MYE_LOG_INFO("==== Acoustic audio self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    const AcousticGridDesc maze = MakeMazeGrid();
    const AcousticAudioComponent kDefault; // 既定値。テストは基本これで呼ぶ

    // ---- (T1) 登録 ----
    // ★TypeId は**登録順そのもの**。50 からずれたら既存シーンと .rep が壊れる側の変更が
    //   混ざっている (途中挿入 or 登録漏れ) ので、番号を書き換えて通してはいけない
    {
        RegisterBuiltinComponents(); // 多重呼び出しは無害
        check(AcousticAudioComponent::sTypeId == 50, "T1: AcousticAudio is TypeId 50");
        const ComponentDesc& d =
            ComponentRegistry::Get().Desc(AcousticAudioComponent::sTypeId);
        check((d.flags & kComponentNoHash) != 0, "T1: AcousticAudio is kComponentNoHash");
        // NoHash = WorldHasher が丸ごと飛ばす = snapshot の版を上げる理由が無い
        check(kSimSnapshotVersion == 11, "T1: kSimSnapshotVersion stays 11");
    }

    // ---- (T2) 同一原点なら波の場と probe が**ビット一致**する ----
    // ★これが「見える波 / 敵が聞く波 / 耳に届く音」が同じ距離場から出ていることの証明。
    //   Dial を 3 本目まで写した以上、写し間違いはここでしか捕まえられない
    {
        AcousticField field;
        field.DebugSetGrid(maze, MakeLMaze(maze));
        const uint32_t rings = 24;
        const AudioVec3 origin = CellCenter(maze, 4, 0, 2);
        GrowWave(field, origin, rings);

        AcousticAudioComponent comp = kDefault;
        comp.probeMaxRing = static_cast<int32_t>(rings);
        AcousticProbe probe;
        const bool built = UpdateAcousticProbe(field, comp, origin, probe);
        check(built && probe.valid, "T2: probe built at the wave origin");

        const AcousticField::WaveField& wf = field.FieldOf(0);
        const bool sameBox = probe.waveField.x0 == wf.x0 && probe.waveField.y0 == wf.y0
            && probe.waveField.z0 == wf.z0 && probe.waveField.sx == wf.sx
            && probe.waveField.sy == wf.sy && probe.waveField.sz == wf.sz
            && probe.waveField.maxDist == wf.maxDist;
        check(sameBox, "T2: the probe box matches the wave box");
        const bool sameField = sameBox && probe.waveField.dist.size() == wf.dist.size()
            && std::memcmp(probe.waveField.dist.data(), wf.dist.data(),
                           wf.dist.size() * sizeof(uint16_t)) == 0
            && std::memcmp(probe.waveField.parentDir.data(), wf.parentDir.data(),
                           wf.parentDir.size()) == 0;
        check(sameField, "T2: dist and parentDir are bit-identical to the wave field");
    }

    // ---- (T3) 対称性: probe(L) から見た S == 波(S) から見た L ----
    {
        struct Pair {
            int32_t lx, lz, sx, sz;
        };
        const Pair pairs[3] = { { 4, 2, 18, 2 }, { 3, 2, 20, 15 }, { 20, 20, 2, 2 } };
        bool allSame = true;
        for (const Pair& p : pairs) {
            AcousticField field;
            field.DebugSetGrid(maze, MakeLMaze(maze));
            const uint32_t rings = 48; // 箱はグリッド全体、maxDist 528 = 迷路の最長路より長い
            GrowWave(field, CellCenter(maze, p.sx, 0, p.sz), rings);
            const uint16_t fromWave = field.DistanceAt(0, p.lx, 0, p.lz);

            AcousticAudioComponent comp = kDefault;
            comp.probeMaxRing = static_cast<int32_t>(rings);
            AcousticProbe probe;
            (void)UpdateAcousticProbe(field, comp, CellCenter(maze, p.lx, 0, p.lz), probe);
            allSame = allSame && probe.DistAt(p.sx, 0, p.sz) == fromWave;
        }
        check(allSame, "T3: chamfer distance is symmetric between the wave and the probe");
    }

    // ---- (T4) 再現性: 同じ入力で 2 回組んでも同じバイト列 ----
    {
        AcousticField field;
        field.DebugSetGrid(maze, MakeLMaze(maze));
        const AudioVec3 l = CellCenter(maze, 6, 0, 2);
        AcousticProbe a;
        AcousticProbe b;
        (void)UpdateAcousticProbe(field, kDefault, l, a);
        (void)UpdateAcousticProbe(field, kDefault, l, b);
        const bool same = a.waveField.dist.size() == b.waveField.dist.size()
            && std::memcmp(a.waveField.dist.data(), b.waveField.dist.data(),
                           a.waveField.dist.size() * sizeof(uint16_t)) == 0
            && std::memcmp(a.waveField.parentDir.data(), b.waveField.parentDir.data(),
                           a.waveField.parentDir.size()) == 0
            && a.openness == b.openness;
        check(same, "T4: two probes built from the same input are byte-identical");

        // 同じ入力なら焼き直さない (再構築契機がキャッシュではなく純関数の性質に乗っている)
        check(!UpdateAcousticProbe(field, kDefault, l, a),
              "T4: an unchanged input does not rebuild");
        check(UpdateAcousticProbe(field, kDefault, CellCenter(maze, 7, 0, 2), a),
              "T4: moving one cell rebuilds");
    }

    // ---- (T5) 密閉: 廊下から切り離した開セルの音源 ----
    {
        AcousticField field;
        std::vector<uint8_t> occ = MakeLMaze(maze);
        occ[static_cast<size_t>(acoustic::CellIndex(maze, 10, 0, 10))] = 0u; // 壁の中の 1 部屋
        field.DebugSetGrid(maze, std::move(occ));

        AcousticProbe probe;
        (void)UpdateAcousticProbe(field, kDefault, CellCenter(maze, 4, 0, 2), probe);
        const AudioVec3 s = CellCenter(maze, 10, 0, 10);
        AudioSpatial io = MakeSpatial();
        float gain = -1.0f;
        AcousticShapeInfo info;
        ShapeAcousticSpatial(field, probe, kDefault, CellCenter(maze, 4, 0, 2), s, io, gain,
                             nullptr, 1.0f, &info);
        check(info.cls == AcousticPathClass::Occluded, "T5: a sealed source is Occluded");
        check(Near(gain, kDefault.occludedGain, 1e-6f), "T5: gain falls to occludedGain");
        check(Near(io.lpfCoefficient, kDefault.occludedLpf, 1e-6f),
              "T5: lpf falls to occludedLpf");
        check(io.position.x == s.x && io.position.y == s.y && io.position.z == s.z,
              "T5: an occluded source keeps its real position");
        check(info.dPath < 0.0f && info.dLine < 0.0f, "T5: occluded rows report dPath = -1");
    }

    // ---- (T6) 経路上限超え / グリッド外 ----
    {
        AcousticField field;
        field.DebugSetGrid(maze, MakeLMaze(maze));
        const AudioVec3 l = CellCenter(maze, 4, 0, 2);

        AcousticAudioComponent small = kDefault;
        small.probeMaxRing = 8; // 箱が S を包まない = 到達しようが無い
        AcousticProbe probe;
        (void)UpdateAcousticProbe(field, small, l, probe);
        AudioSpatial io = MakeSpatial();
        float gain = -1.0f;
        AcousticShapeInfo info;
        ShapeAcousticSpatial(field, probe, small, l, CellCenter(maze, 20, 0, 18), io, gain,
                             nullptr, 1.0f, &info);
        check(info.cls == AcousticPathClass::Occluded && info.dPath < 0.0f,
              "T6: a source past probeMaxRing is Occluded");

        // グリッド外の音源 = Bypass。**io は 1 バイトも変わらない**
        AcousticProbe full;
        (void)UpdateAcousticProbe(field, kDefault, l, full);
        AudioSpatial before = MakeSpatial();
        AudioSpatial after = before;
        gain = -1.0f;
        ShapeAcousticSpatial(field, full, kDefault, l, AudioVec3{ 1000.0f, 0.0f, 0.0f }, after,
                             gain, nullptr, 1.0f, &info);
        check(info.cls == AcousticPathClass::Bypass && gain == 1.0f
                  && std::memcmp(&before, &after, sizeof(AudioSpatial)) == 0,
              "T6: a source outside the grid bypasses the shaping untouched");

        // リスナーがグリッド外 → probe 無効 → Bypass
        AcousticProbe outside;
        const bool built =
            UpdateAcousticProbe(field, kDefault, AudioVec3{ 0.0f, 500.0f, 0.0f }, outside);
        check(!built && !outside.valid, "T6: a listener outside the grid invalidates the probe");
        after = before;
        gain = -1.0f;
        ShapeAcousticSpatial(field, outside, kDefault, AudioVec3{ 0.0f, 500.0f, 0.0f },
                             CellCenter(maze, 4, 0, 2), after, gain, nullptr, 1.0f, &info);
        check(info.cls == AcousticPathClass::Bypass && gain == 1.0f
                  && std::memcmp(&before, &after, sizeof(AudioSpatial)) == 0,
              "T6: an invalid probe bypasses the shaping untouched");
    }

    // ---- (T7) 到来方向 = 廊下の腕の向き ----
    // ★企画の中核 (「戸口の方向から聞こえる」) が成立していることの機械証明。
    //   最後の 1 歩しか見ないので「腕の向き」が厳密に書ける
    {
        AcousticField field;
        field.DebugSetGrid(maze, MakeLMaze(maze));
        const AudioVec3 l = CellCenter(maze, 5, 0, 2);
        AcousticProbe probe;
        (void)UpdateAcousticProbe(field, kDefault, l, probe);
        AudioSpatial io = MakeSpatial();
        float gain = -1.0f;
        AcousticShapeInfo info;
        ShapeAcousticSpatial(field, probe, kDefault, l, CellCenter(maze, 20, 0, 18), io, gain,
                             nullptr, 1.0f, &info);
        check(info.cls == AcousticPathClass::Detour, "T7: around the corner is Detour");
        const float expected = acoustic::ChamferToMeters(335, maze.cellSize);
        check(Near(info.dPath, expected, 1e-4f), "T7: dPath is the chamfer path length");
        check(Near(io.position.x, l.x + expected, 1e-4f) && Near(io.position.y, l.y, 1e-4f)
                  && Near(io.position.z, l.z, 1e-4f),
              "T7: the virtual source sits along +X (the corridor arm) at dPath");
        check(io.dopplerScale == 0.0f, "T7: doppler is off while a virtual position is used");
    }

    // ---- (T8) 直達: 直線廊下と自由空間の対角 ----
    {
        AcousticField field;
        field.DebugSetGrid(maze, MakeLMaze(maze));
        const AudioVec3 l = CellCenter(maze, 3, 0, 2);
        const AudioVec3 s = CellCenter(maze, 15, 0, 2);
        AcousticProbe probe;
        (void)UpdateAcousticProbe(field, kDefault, l, probe);
        AudioSpatial io = MakeSpatial();
        float gain = -1.0f;
        AcousticShapeInfo info;
        ShapeAcousticSpatial(field, probe, kDefault, l, s, io, gain, nullptr, 1.0f, &info);
        check(info.cls == AcousticPathClass::Direct, "T8: a straight corridor is Direct");
        check(io.lpfCoefficient == 1.0f && gain == 1.0f, "T8: Direct applies no lpf and no gain");
        check(io.position.x == s.x && io.position.y == s.y && io.position.z == s.z,
              "T8: Direct keeps the real position exactly");
        check(io.dopplerScale == 1.0f, "T8: Direct leaves dopplerScale alone");

        // 自由空間の対角。チャンファは真の距離より 2.9% 長いが、閾値がそれを吸収する
        const AcousticGridDesc free = MakeFreeGrid();
        AcousticField openField;
        openField.DebugSetGrid(free, std::vector<uint8_t>(
                                         static_cast<size_t>(free.CellCount()), 0u));
        const AudioVec3 fl = CellCenter(free, 2, 1, 2);
        AcousticProbe fp;
        (void)UpdateAcousticProbe(openField, kDefault, fl, fp);
        AudioSpatial fio = MakeSpatial();
        gain = -1.0f;
        ShapeAcousticSpatial(openField, fp, kDefault, fl, CellCenter(free, 20, 1, 20), fio, gain,
                             nullptr, 1.0f, &info);
        check(info.cls == AcousticPathClass::Direct,
              "T8: a free-space diagonal stays Direct (the 2.9% chamfer bias is absorbed)");
    }

    // ---- (T9) クラス列: L 字を歩くと Occluded* -> Detour* -> Direct* ----
    // ★実機ログでは --synth-input が部屋 A から出ないので遷移が撮れない (spec S8)。
    //   「壁の向こう -> 回り込み -> 直達」が順に起きることの証明はここだけにある
    {
        AcousticField field;
        field.DebugSetGrid(maze, MakeLMaze(maze));
        AcousticAudioComponent comp = kDefault;
        comp.probeMaxRing = 20;

        std::vector<int32_t> lx;
        std::vector<int32_t> lz;
        for (int32_t x = 2; x <= 20; ++x) {
            lx.push_back(x);
            lz.push_back(2);
        }
        for (int32_t z = 3; z <= 16; ++z) {
            lx.push_back(20);
            lz.push_back(z);
        }

        int phase = 0; // 0 = Occluded / 1 = Detour / 2 = Direct
        bool ordered = true;
        bool ratioOk = true;
        bool monotone = true;
        int counts[3] = {};
        float prevPath = 1e9f;
        AcousticProbe probe;
        for (size_t i = 0; i < lx.size(); ++i) {
            const AudioVec3 l = CellCenter(maze, lx[i], 0, lz[i]);
            (void)UpdateAcousticProbe(field, comp, l, probe);
            AudioSpatial io = MakeSpatial();
            float gain = 1.0f;
            AcousticShapeInfo info;
            ShapeAcousticSpatial(field, probe, comp, l, CellCenter(maze, 20, 0, 18), io, gain,
                                 nullptr, 1.0f, &info);
            const int stage = (info.cls == AcousticPathClass::Occluded)   ? 0
                              : (info.cls == AcousticPathClass::Detour)   ? 1
                              : (info.cls == AcousticPathClass::Direct)   ? 2
                                                                          : 3;
            if (stage == 3 || stage < phase) {
                ordered = false; // Bypass が出た / 逆戻りした
            }
            phase = (stage > phase) ? stage : phase;
            if (stage <= 2) {
                ++counts[stage];
            }
            if (info.dPath >= 0.0f) {
                ratioOk = ratioOk && info.dPath >= 0.99f * info.dLine;
                monotone = monotone && info.dPath <= prevPath + 1e-4f;
                prevPath = info.dPath;
            }
        }
        check(ordered, "T9: the class sequence never goes backwards");
        check(counts[0] > 0 && counts[1] > 0 && counts[2] > 0,
              "T9: all three of Occluded / Detour / Direct occur");
        check(ratioOk, "T9: every reachable step satisfies dPath >= 0.99 * dLine");
        check(monotone, "T9: dPath decreases monotonically as the listener closes in");
    }

    // ---- (T10) 単調: 回り込みが伸びるほど LPF が閉じ、床で止まる ----
    {
        AcousticField field;
        field.DebugSetGrid(maze, MakeLMaze(maze));
        const AudioVec3 l = CellCenter(maze, 2, 0, 2);
        AcousticProbe probe;
        (void)UpdateAcousticProbe(field, kDefault, l, probe);

        float prevDetour = -1.0f;
        float prevLpf = 2.0f;
        bool detourUp = true;
        bool lpfDown = true;
        bool aboveFloor = true;
        for (int32_t z = 3; z <= 18; ++z) {
            AudioSpatial io = MakeSpatial();
            float gain = 1.0f;
            AcousticShapeInfo info;
            ShapeAcousticSpatial(field, probe, kDefault, l, CellCenter(maze, 20, 0, z), io, gain,
                                 nullptr, 1.0f, &info);
            const float detour = info.dPath - info.dLine;
            detourUp = detourUp && detour >= prevDetour - 1e-4f;
            lpfDown = lpfDown && info.lpf <= prevLpf + 1e-6f;
            aboveFloor = aboveFloor && info.lpf >= kDefault.lpfFloor - 1e-6f;
            prevDetour = detour;
            prevLpf = info.lpf;
        }
        check(detourUp, "T10: the detour grows as the source moves down the far arm");
        check(lpfDown, "T10: the lpf coefficient never rises while the detour grows");
        check(aboveFloor, "T10: the lpf coefficient never dips below lpfFloor");

        // ★この迷路 (12m 四方) で稼げる回り込みは最大 5m 弱なので、既定の bendFullM = 8 では
        //   床に**着かない**。「床でちゃんと止まる」を固定するには飽和長を縮めるしかない
        AcousticAudioComponent steep = kDefault;
        steep.bendFullM = 4.0f; // 4 * (1 - 0.25) = 3m の回り込みで床
        AudioSpatial io = MakeSpatial();
        float gain = 1.0f;
        AcousticShapeInfo info;
        ShapeAcousticSpatial(field, probe, steep, l, CellCenter(maze, 20, 0, 18), io, gain,
                             nullptr, 1.0f, &info);
        check(Near(info.lpf, steep.lpfFloor, 1e-6f),
              "T10: a long enough detour lands exactly on lpfFloor");
    }

    // ---- (T11) 開放度と閉形式 ----
    {
        const AcousticGridDesc free = MakeFreeGrid();
        AcousticField openField;
        openField.DebugSetGrid(free,
                               std::vector<uint8_t>(static_cast<size_t>(free.CellCount()), 0u));
        AcousticProbe fp;
        (void)UpdateAcousticProbe(openField, kDefault, CellCenter(free, 12, 2, 12), fp);
        check(fp.openness >= 0.95f, "T11: free space reads as fully open");

        // 閉形式 == 自由空間の Dial。**分母の正しさはこれでしか担保できない**
        bool formulaOk = true;
        const AcousticField::WaveField& f = fp.waveField;
        for (int32_t lz = 0; lz < f.sz && formulaOk; ++lz) {
            for (int32_t ly = 0; ly < f.sy && formulaOk; ++ly) {
                for (int32_t lx = 0; lx < f.sx && formulaOk; ++lx) {
                    const uint32_t cf = ChamferClosedForm(lx + f.x0 - fp.ox, ly + f.y0 - fp.oy,
                                                          lz + f.z0 - fp.oz);
                    const uint16_t d = fp.DistAt(lx + f.x0, ly + f.y0, lz + f.z0);
                    formulaOk = (cf <= f.maxDist) ? (d == static_cast<uint16_t>(cf))
                                                  : (d == AcousticField::kUnreached);
                }
            }
        }
        check(formulaOk, "T11: the closed-form chamfer matches the free-space Dial cell by cell");

        AcousticField mazeField;
        mazeField.DebugSetGrid(maze, MakeLMaze(maze));
        AcousticProbe mp;
        (void)UpdateAcousticProbe(mazeField, kDefault, CellCenter(maze, 11, 0, 2), mp);
        check(mp.openness < 0.3f, "T11: the middle of a one-cell corridor reads as closed");
    }

    // ---- (T12) 平滑化: 整数 tick 基準の半減期 ----
    {
        AcousticField field;
        field.DebugSetGrid(maze, MakeLMaze(maze));
        const AudioVec3 l = CellCenter(maze, 5, 0, 2);
        AcousticProbe probe;
        (void)UpdateAcousticProbe(field, kDefault, l, probe);
        const AudioVec3 sNear = CellCenter(maze, 15, 0, 2);  // Direct
        const AudioVec3 sFar = CellCenter(maze, 20, 0, 18);  // Detour (段状に切り替える)

        AcousticShapeState a;
        AcousticShapeState b;
        AudioSpatial io = MakeSpatial();
        float gain = 1.0f;
        ShapeAcousticSpatial(field, probe, kDefault, l, sNear, io, gain, &a, 1.0f, nullptr);
        io = MakeSpatial();
        ShapeAcousticSpatial(field, probe, kDefault, l, sNear, io, gain, &b, 1.0f, nullptr);
        check(a.valid && a.lpf == 1.0f && a.position.x == sNear.x,
              "T12: the first shaping snaps to the target");

        for (int i = 0; i < 3; ++i) {
            io = MakeSpatial();
            ShapeAcousticSpatial(field, probe, kDefault, l, sFar, io, gain, &a, 1.0f, nullptr);
        }
        io = MakeSpatial();
        ShapeAcousticSpatial(field, probe, kDefault, l, sFar, io, gain, &b, 3.0f, nullptr);
        check(Near(a.lpf, b.lpf, 1e-5f) && Near(a.gain, b.gain, 1e-5f)
                  && Near(a.position.x, b.position.x, 1e-5f)
                  && Near(a.position.z, b.position.z, 1e-5f),
              "T12: three 1-tick steps equal one 3-tick step");

        AcousticAudioComponent snap = kDefault;
        snap.smoothTicks = 0;
        AcousticShapeState c;
        io = MakeSpatial();
        ShapeAcousticSpatial(field, probe, snap, l, sNear, io, gain, &c, 1.0f, nullptr);
        io = MakeSpatial();
        ShapeAcousticSpatial(field, probe, snap, l, sFar, io, gain, &c, 1.0f, nullptr);
        AudioSpatial ref = MakeSpatial();
        float refGain = 1.0f;
        ShapeAcousticSpatial(field, probe, snap, l, sFar, ref, refGain, nullptr, 1.0f, nullptr);
        check(c.lpf == ref.lpfCoefficient && c.position.x == ref.position.x,
              "T12: smoothTicks = 0 snaps every tick");
    }

    // ---- (T13) 波 -> spatial の関係式 (M68b の MakeWaveShotPlay がここに乗る) ----
    // ★**gain の 2 乗がエネルギー**。逆二乗をそのまま振幅に掛けると 10m で -52dB =
    //   無音になる (spec S2)。ここが崩れると「波が届いているのに聞こえない」になる
    {
        const float amp = 0.7f;
        const float cell = 0.5f;
        const uint32_t maxD = 220; // = 10 m
        bool ok = true;
        const float ds[3] = { 1.0f, 3.0f, 7.0f };
        for (float d : ds) {
            const float g = RolloffGain(0, cell, 10.0f, d);
            const uint32_t chamfer = static_cast<uint32_t>(d / cell * 11.0f + 0.5f);
            const float e = acoustic::EnergyAt(chamfer, maxD, amp, cell);
            ok = ok && Near(g * g * amp, e, 1e-6f);
        }
        check(ok, "T13: gain^2 * amplitude equals EnergyAt at 1 / 3 / 7 m");
        check(RolloffGain(0, cell, 10.0f, 10.0f) == 0.0f
                  && acoustic::EnergyAt(maxD + 1, maxD, amp, cell) == 0.0f,
              "T13: both sides are exactly zero past the reach");
    }

    // ---- (T14) 壁の中の音源は 26 近傍へ寄る ----
    {
        AcousticField field;
        field.DebugSetGrid(maze, MakeLMaze(maze));
        const AudioVec3 l = CellCenter(maze, 8, 0, 2);
        AcousticProbe probe;
        (void)UpdateAcousticProbe(field, kDefault, l, probe);
        AudioSpatial io = MakeSpatial();
        float gain = 1.0f;
        AcousticShapeInfo info;
        // (4,0,1) は廊下 (z=2) の真横の壁セル。足元が床コライダの中、と同じ状況
        ShapeAcousticSpatial(field, probe, kDefault, l, CellCenter(maze, 4, 0, 1), io, gain,
                             nullptr, 1.0f, &info);
        check(info.cls != AcousticPathClass::Occluded && info.dPath >= 0.0f,
              "T14: a source inside a wall is nudged to the adjacent open cell");
    }

    // ---- (T15) ボリュームが無いシーンは 1 バイトも触らない ----
    // ★これが「M68 を足しても既存シーンの音が 1 ビットも変わらない」の根拠
    {
        AcousticField empty; // Sync も DebugSetGrid も呼ばない = HasVolume() false
        AcousticProbe probe;
        const bool built = UpdateAcousticProbe(empty, kDefault, AudioVec3{}, probe);
        check(!built && !probe.valid, "T15: no volume means no probe");
        const AudioSpatial before = MakeSpatial();
        AudioSpatial after = before;
        float gain = -1.0f;
        AcousticShapeInfo info;
        ShapeAcousticSpatial(empty, probe, kDefault, AudioVec3{}, AudioVec3{ 1.0f, 0.0f, 0.0f },
                             after, gain, nullptr, 1.0f, &info);
        check(info.cls == AcousticPathClass::Bypass && gain == 1.0f
                  && std::memcmp(&before, &after, sizeof(AudioSpatial)) == 0,
              "T15: without a volume the spatial parameters are untouched");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Acoustic audio self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Acoustic audio self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
