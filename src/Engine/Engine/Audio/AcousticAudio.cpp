//====================================================================================
//                          AcousticAudio.cpp
//  MyEngine/ 秋田蓮音                                                      09/06/2026
//                                          リスナー場の構築と遮蔽・回折の整形（純関数）
//====================================================================================
#include "Engine/Engine/Audio/AcousticAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "Engine/Core/Log.h"

namespace mye {
namespace {

// 箱ローカル座標 -> 線形 index。**AcousticField.cpp の LocalIndex と同じ式**
// (probe は波の場をそのまま流用するので、index の作り方まで一致していないと
//  T2 の memcmp 比較が成立しない)
inline int32_t LocalIndex(const AcousticField::WaveField& f, int32_t lx, int32_t ly, int32_t lz)
{
    return (lz * f.sy + ly) * f.sx + lx;
}

inline bool InBox(const AcousticField::WaveField& f, int32_t lx, int32_t ly, int32_t lz)
{
    return lx >= 0 && lx < f.sx && ly >= 0 && ly < f.sy && lz >= 0 && lz < f.sz;
}

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float Length3(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

// 原点 ± ring をグリッドでクリップした箱のセル数 (予算判定用)
int64_t BoxCellsFor(const AcousticGridDesc& g, int32_t ox, int32_t oy, int32_t oz, int32_t ring)
{
    const int64_t sx = (std::min)(g.dimX - 1, ox + ring) - (std::max)(0, ox - ring) + 1;
    const int64_t sy = (std::min)(g.dimY - 1, oy + ring) - (std::max)(0, oy - ring) + 1;
    const int64_t sz = (std::min)(g.dimZ - 1, oz + ring) - (std::max)(0, oz - ring) + 1;
    return sx * sy * sz;
}

// ---- Dial 法の 3 本目 (リスナー場) ----
// ★AcousticField::SeedWave + AdvanceWaveOneRing を**リング分周なしで一気に完走**した形。
//   バケットを 0..maxDist まで昇順に空にするのは、あちらを ring 0..maxRing まで
//   連続で回すのと**同じ訪問順**になる (1 リング = 幅 11 の区間を順に処理するだけなので、
//   区間の切れ目を無くしても列は変わらない)。だから dist も parentDir もビット一致する。
// ★残光 (WriteShell) は焼かない。ここは耳のための場であって、絵は波の側の仕事。
void BuildProbeField(const AcousticField& field, int32_t ox, int32_t oy, int32_t oz,
                     int32_t maxRing, AcousticField::WaveField& f)
{
    const AcousticGridDesc& g = field.Grid();
    const int32_t x0 = (std::max)(0, ox - maxRing);
    const int32_t y0 = (std::max)(0, oy - maxRing);
    const int32_t z0 = (std::max)(0, oz - maxRing);
    const int32_t x1 = (std::min)(g.dimX - 1, ox + maxRing);
    const int32_t y1 = (std::min)(g.dimY - 1, oy + maxRing);
    const int32_t z1 = (std::min)(g.dimZ - 1, oz + maxRing);
    f.x0 = x0;
    f.y0 = y0;
    f.z0 = z0;
    f.sx = x1 - x0 + 1;
    f.sy = y1 - y0 + 1;
    f.sz = z1 - z0 + 1;
    f.maxDist = static_cast<uint32_t>(maxRing) * acoustic::kFaceCost;

    const size_t n = static_cast<size_t>(f.sx) * static_cast<size_t>(f.sy)
                   * static_cast<size_t>(f.sz);
    f.dist.assign(n, AcousticField::kUnreached);
    f.parentDir.assign(n, AcousticField::kNoParent);
    // バケットは capacity を捨てずに使い回す (歩くたびに焼き直すので確保が効く)
    f.buckets.resize(static_cast<size_t>(f.maxDist) + 1);
    for (std::vector<int32_t>& b : f.buckets) {
        b.clear();
    }

    const int32_t start = LocalIndex(f, ox - x0, oy - y0, oz - z0);
    f.dist[static_cast<size_t>(start)] = 0;
    f.buckets[0].push_back(start);

    for (uint32_t d = 0; d <= f.maxDist; ++d) {
        std::vector<int32_t>& bucket = f.buckets[d];
        for (size_t k = 0; k < bucket.size(); ++k) {
            const int32_t li = bucket[k];
            if (f.dist[static_cast<size_t>(li)] != static_cast<uint16_t>(d)) {
                continue; // より短い距離で上書きされた古いエントリ
            }
            const int32_t lx = li % f.sx;
            const int32_t ly = (li / f.sx) % f.sy;
            const int32_t lz = li / (f.sx * f.sy);
            for (int i = 0; i < acoustic::kNeighborCount; ++i) {
                const acoustic::Neighbor& nb = acoustic::kNeighbors[i];
                const int32_t nlx = lx + nb.dx;
                const int32_t nly = ly + nb.dy;
                const int32_t nlz = lz + nb.dz;
                if (!InBox(f, nlx, nly, nlz)) {
                    continue;
                }
                const uint32_t nd = d + nb.cost;
                if (nd > f.maxDist) {
                    continue;
                }
                const int32_t ni = LocalIndex(f, nlx, nly, nlz);
                if (nd >= f.dist[static_cast<size_t>(ni)]) {
                    continue;
                }
                // ★閉セルは絶対に訪れない / 中間セルは見ない — 波と 1 文字も違わない条件。
                //   ここを緩めると「聞こえる範囲」と「波が届く範囲」がずれる
                if (field.IsSolid(nlx + f.x0, nly + f.y0, nlz + f.z0)) {
                    continue;
                }
                f.dist[static_cast<size_t>(ni)] = static_cast<uint16_t>(nd);
                f.parentDir[static_cast<size_t>(ni)] =
                    acoustic::OppositeNeighbor(static_cast<uint8_t>(i));
                f.buckets[nd].push_back(ni);
            }
        }
        bucket.clear();
    }
}

// 開放度 = 「半径 R 以内で実際に届くセル数 / 同じ半径の自由空間のセル数」。
// ★分母を**閉形式**で数えるのが要点。占有無しの Dial をもう 1 本回すより速いし、
//   「自由空間なら Dial と厳密に一致する」ことをテストで固定できるので、
//   分母が黙って壊れることが無い。廊下 = 0.05 級 / 部屋の真ん中 = 0.5 級に出る
float ComputeOpenness(const AcousticProbe& p, float roomProbeM, float cellSize)
{
    if (!(cellSize > 0.0f) || !(roomProbeM > 0.0f)) {
        return 0.0f;
    }
    int32_t cells = static_cast<int32_t>(std::floor(roomProbeM / cellSize + 0.5f));
    if (cells < 1) {
        cells = 1;
    }
    const uint32_t r = static_cast<uint32_t>(cells) * acoustic::kFaceCost;
    const AcousticField::WaveField& f = p.waveField;
    int64_t num = 0;
    int64_t den = 0;
    for (int32_t lz = 0; lz < f.sz; ++lz) {
        for (int32_t ly = 0; ly < f.sy; ++ly) {
            for (int32_t lx = 0; lx < f.sx; ++lx) {
                const uint32_t cf = ChamferClosedForm(lx + f.x0 - p.ox, ly + f.y0 - p.oy,
                                                      lz + f.z0 - p.oz);
                if (cf > r) {
                    continue;
                }
                ++den;
                if (f.dist[static_cast<size_t>(LocalIndex(f, lx, ly, lz))] <= r) {
                    ++num;
                }
            }
        }
    }
    return den > 0 ? static_cast<float>(num) / static_cast<float>(den) : 0.0f;
}

} // namespace

const char* AcousticPathClassName(AcousticPathClass c)
{
    switch (c) {
    case AcousticPathClass::Direct: return "Direct";
    case AcousticPathClass::Detour: return "Detour";
    case AcousticPathClass::Occluded: return "Occluded";
    default: return "Bypass";
    }
}

uint32_t ChamferClosedForm(int32_t dx, int32_t dy, int32_t dz)
{
    int32_t a = std::abs(dx);
    int32_t b = std::abs(dy);
    int32_t c = std::abs(dz);
    // 降順に整列 (3 要素なので比較交換 3 回で足りる)
    if (a < b) {
        std::swap(a, b);
    }
    if (b < c) {
        std::swap(b, c);
    }
    if (a < b) {
        std::swap(a, b);
    }
    return static_cast<uint32_t>(acoustic::kFaceCost) * static_cast<uint32_t>(a - b)
        + static_cast<uint32_t>(acoustic::kEdgeCost) * static_cast<uint32_t>(b - c)
        + static_cast<uint32_t>(acoustic::kCornerCost) * static_cast<uint32_t>(c);
}

uint16_t AcousticProbe::DistAt(int32_t cx, int32_t cy, int32_t cz) const
{
    if (!valid) {
        return AcousticField::kUnreached;
    }
    const int32_t lx = cx - waveField.x0;
    const int32_t ly = cy - waveField.y0;
    const int32_t lz = cz - waveField.z0;
    if (!InBox(waveField, lx, ly, lz)) {
        return AcousticField::kUnreached;
    }
    return waveField.dist[static_cast<size_t>(LocalIndex(waveField, lx, ly, lz))];
}

uint8_t AcousticProbe::ParentAt(int32_t cx, int32_t cy, int32_t cz) const
{
    if (!valid) {
        return AcousticField::kNoParent;
    }
    const int32_t lx = cx - waveField.x0;
    const int32_t ly = cy - waveField.y0;
    const int32_t lz = cz - waveField.z0;
    if (!InBox(waveField, lx, ly, lz)) {
        return AcousticField::kNoParent;
    }
    return waveField.parentDir[static_cast<size_t>(LocalIndex(waveField, lx, ly, lz))];
}

bool UpdateAcousticProbe(const AcousticField& field, const AcousticAudioComponent& comp,
                         AudioVec3 listenerPos, AcousticProbe& io)
{
    if (!field.HasVolume()) {
        io.valid = false; // 配列は捨てない (部屋の出入りで確保を往復させない)
        return false;
    }
    const AcousticGridDesc& g = field.Grid();
    int32_t cx = 0, cy = 0, cz = 0;
    if (!acoustic::WorldToCell(g, listenerPos.x, listenerPos.y, listenerPos.z, cx, cy, cz)) {
        io.valid = false; // グリッド外の耳は整形しない (= Bypass)
        return false;
    }
    const int32_t want = std::clamp(comp.probeMaxRing, 1, 256);
    if (io.valid && io.ox == cx && io.oy == cy && io.oz == cz && io.requestRing == want
        && io.signature == field.StaticSignature() && acoustic::SameGrid(io.grid, g)) {
        return false; // 入力が 1 つも変わっていない = 焼き直す意味が無い
    }

    // ★予算に収める。グリッド全体より小さい箱にしかならないので、既定ボリュームでは
    //   1 度も効かない。効くのは 256^3 級のグリッドで、そのときは到達範囲が縮む
    //   (= 遠い音が Occluded に倒れる) ことを 1 回だけ警告して知らせる
    int32_t ring = want;
    while (ring > 1 && BoxCellsFor(g, cx, cy, cz, ring) > kProbeCellBudget) {
        ring /= 2;
    }
    if (ring != want && !io.budgetWarned) {
        io.budgetWarned = true;
        MYE_LOG_WARN("[acaudio] probeMaxRing %d exceeds the %lld-cell budget; using %d "
                     "(distant sources will fall back to Occluded)",
                     want, static_cast<long long>(kProbeCellBudget), ring);
    }

    BuildProbeField(field, cx, cy, cz, ring, io.waveField);
    io.ox = cx;
    io.oy = cy;
    io.oz = cz;
    io.maxRing = ring;
    io.requestRing = want;
    io.signature = field.StaticSignature();
    io.grid = g;
    io.valid = true;
    io.openness = ComputeOpenness(io, comp.roomProbeM, g.cellSize);
    return true;
}

void ShapeAcousticSpatial(const AcousticField& field, const AcousticProbe& probe,
                          const AcousticAudioComponent& comp, AudioVec3 listenerPos,
                          AudioVec3 sourcePos, AudioSpatial& io, float& gainOut,
                          AcousticShapeState* smooth, float dTicks, AcousticShapeInfo* info)
{
    AcousticShapeInfo scratch;
    AcousticShapeInfo& inf = (info != nullptr) ? *info : scratch;
    inf = AcousticShapeInfo{};
    inf.dReal = Length3(sourcePos.x - listenerPos.x, sourcePos.y - listenerPos.y,
                        sourcePos.z - listenerPos.z);
    gainOut = 1.0f;

    // ---- (1) 場が無い / 耳がグリッド外 → Bypass。io には 1 バイトも触らない ----
    // ★平滑化状態は落としておく。次に有効になったときスナップさせないと、
    //   グリッドに入り直した瞬間に「前回の遮蔽値」から数百 ms かけて戻ってくる
    auto bypass = [&]() {
        inf.cls = AcousticPathClass::Bypass;
        inf.lpf = 1.0f;
        inf.gain = 1.0f;
        if (smooth != nullptr) {
            *smooth = AcousticShapeState{};
        }
    };
    if (!probe.valid) {
        bypass();
        return;
    }

    // ---- (2) 音源セル。壁の中なら Emit と同じ表順 26 近傍で開セルへ寄せる ----
    int32_t sx = 0, sy = 0, sz = 0;
    if (!acoustic::WorldToCell(probe.grid, sourcePos.x, sourcePos.y, sourcePos.z, sx, sy, sz)) {
        bypass(); // グリッドの外で鳴っている音はこの場では表現できない
        return;
    }
    AcousticPathClass cls = AcousticPathClass::Direct;
    bool occluded = false;
    if (field.IsSolid(sx, sy, sz)) {
        bool moved = false;
        for (int i = 0; i < acoustic::kNeighborCount; ++i) {
            const acoustic::Neighbor& nb = acoustic::kNeighbors[i];
            if (!field.IsSolid(sx + nb.dx, sy + nb.dy, sz + nb.dz)) {
                sx += nb.dx;
                sy += nb.dy;
                sz += nb.dz;
                moved = true;
                break;
            }
        }
        if (!moved) {
            occluded = true; // 完全に埋まっている = 密閉
        }
    }

    // ---- (3) 箱外 / 未到達 → Occluded ----
    const uint16_t dist = occluded ? AcousticField::kUnreached : probe.DistAt(sx, sy, sz);
    if (dist == AcousticField::kUnreached) {
        occluded = true;
    }

    // ---- (4) 距離と回り込み量 ----
    const float cellSize = probe.grid.cellSize;
    float dPath = -1.0f;
    float dLine = -1.0f;
    float detour = 0.0f;
    AudioVec3 dir{};
    if (!occluded) {
        dPath = acoustic::ChamferToMeters(dist, cellSize);
        float lcx = 0.0f, lcy = 0.0f, lcz = 0.0f;
        float scx = 0.0f, scy = 0.0f, scz = 0.0f;
        acoustic::CellToWorldCenter(probe.grid, probe.ox, probe.oy, probe.oz, lcx, lcy, lcz);
        acoustic::CellToWorldCenter(probe.grid, sx, sy, sz, scx, scy, scz);
        // ★直線は**セル中心間**で測る。実座標間で測ると、量子化 (両端で最大 √3 セル) と
        //   3D 角のチャンファ (19 < 11√3 = 19.05) のせいで dPath < dLine が普通に起きて
        //   「経路は直線より短い」という読めない不変条件になる (spec S5)
        dLine = Length3(scx - lcx, scy - lcy, scz - lcz);
        detour = dPath - dLine;
        if (detour < 0.0f) {
            detour = 0.0f;
        }
        // 自由空間の対角はチャンファが 2.9% 長く出る。それを Detour にすると
        // 「何も遮っていないのにこもる」ので、セル 1 個ぶん + 3% を Direct に含める
        const bool direct = detour <= (cellSize + 0.03f * dLine);
        cls = direct ? AcousticPathClass::Direct : AcousticPathClass::Detour;

        // ---- (5) 到来方向 (Detour のみ)。parentDir を耳まで辿る ----
        if (cls == AcousticPathClass::Detour) {
            int32_t cx = sx, cy = sy, cz = sz;
            uint8_t lastDir = AcousticField::kNoParent;
            const int maxSteps = probe.maxRing * 2 + 8;
            int steps = 0;
            bool broken = false;
            while (!(cx == probe.ox && cy == probe.oy && cz == probe.oz)) {
                if (++steps > maxSteps) {
                    broken = true;
                    break;
                }
                const uint8_t pd = probe.ParentAt(cx, cy, cz);
                if (pd == AcousticField::kNoParent) {
                    broken = true;
                    break;
                }
                lastDir = pd;
                cx += acoustic::kNeighbors[pd].dx;
                cy += acoustic::kNeighbors[pd].dy;
                cz += acoustic::kNeighbors[pd].dz;
            }
            if (!broken && probe.DistAt(cx, cy, cz) != 0) {
                broken = true; // 辿り着いた先が原点でない = 鎖が壊れている
            }
            if (broken || lastDir == AcousticField::kNoParent) {
                occluded = true;
                dPath = -1.0f;
                dLine = -1.0f;
            } else {
                // ★**最後の 1 歩だけ**を使う (spec S16)。仮想位置は smoothTicks で
                //   平滑化されるので跳びは既に均されるし、数歩の平均は角の直後に
                //   壁の中を指しうる。1 歩なら「腕の向き」が厳密に書ける
                const acoustic::Neighbor& nb =
                    acoustic::kNeighbors[acoustic::OppositeNeighbor(lastDir)];
                const float len = Length3(static_cast<float>(nb.dx), static_cast<float>(nb.dy),
                                          static_cast<float>(nb.dz));
                if (len > 0.0f) {
                    dir = AudioVec3{ static_cast<float>(nb.dx) / len,
                                     static_cast<float>(nb.dy) / len,
                                     static_cast<float>(nb.dz) / len };
                } else {
                    occluded = true;
                }
            }
        }
    }
    if (occluded) {
        cls = AcousticPathClass::Occluded;
        dPath = -1.0f;
        dLine = -1.0f;
    }

    // ---- (6) 目標値 ----
    AudioVec3 targetPos = sourcePos;
    float targetLpf = 1.0f;
    float targetGain = 1.0f;
    if (cls == AcousticPathClass::Detour) {
        // **仮想発音位置** = 耳から「音が入ってくる向き」へ経路長ぶん離した点。
        // 距離減衰は既存の rolloff がそのまま面倒を見る = 減衰規則を二重に持たない
        targetPos = AudioVec3{ listenerPos.x + dir.x * dPath, listenerPos.y + dir.y * dPath,
                               listenerPos.z + dir.z * dPath };
        const float bend = comp.bendFullM > 1e-3f ? comp.bendFullM : 1e-3f;
        targetLpf = std::clamp(1.0f - detour / bend, std::clamp(comp.lpfFloor, 0.0f, 1.0f), 1.0f);
        // ★仮想位置は角を曲がった瞬間に跳ぶ。ドップラーを載せるとそのたびに
        //   ピッチが飛ぶので、Detour のあいだは切る
        io.dopplerScale = 0.0f;
    } else if (cls == AcousticPathClass::Occluded) {
        targetLpf = std::clamp(comp.occludedLpf, 0.0f, 1.0f);
        targetGain = std::clamp(comp.occludedGain, 0.0f, 1.0f);
    }

    // ---- (7) 平滑化 (整数 tick 基準の半減期。kVelocityHalfLifeTicks と同型) ----
    float gain = targetGain;
    float lpf = targetLpf;
    AudioVec3 pos = targetPos;
    if (smooth != nullptr) {
        const float half = static_cast<float>(comp.smoothTicks);
        if (!smooth->valid || !(half > 0.0f)) {
            smooth->valid = true; // 初回 / スナップ指定は目標へ即座に合わせる
        } else {
            const float steps = dTicks > 1.0f ? dTicks : 1.0f;
            const float alpha = 1.0f - std::pow(0.5f, steps / half);
            gain = Lerp(smooth->gain, targetGain, alpha);
            lpf = Lerp(smooth->lpf, targetLpf, alpha);
            pos = AudioVec3{ Lerp(smooth->position.x, targetPos.x, alpha),
                             Lerp(smooth->position.y, targetPos.y, alpha),
                             Lerp(smooth->position.z, targetPos.z, alpha) };
        }
        smooth->gain = gain;
        smooth->lpf = lpf;
        smooth->position = pos;
    }

    // ---- (8) 反映。lpf は applyLpf 側で Lerp(1, c, blend) されるので二重に掛けない ----
    io.position = pos;
    io.lpfCoefficient = lpf;
    gainOut = Lerp(1.0f, gain, std::clamp(io.spatialBlend, 0.0f, 1.0f));

    inf.cls = cls;
    inf.dPath = dPath;
    inf.dLine = dLine;
    inf.lpf = lpf;
    inf.gain = gainOut;
}

} // namespace mye
