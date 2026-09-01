//====================================================================================
//                          AcousticDebugDraw.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響の場のデバッグ線を組み立てる
//====================================================================================
#include "Engine/Engine/Acoustic/AcousticDebugDraw.h"

#include <algorithm>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/DebugDraw.h"

namespace mye {
namespace {

// EditorLinePass::Unpack と同じ 0xRRGGBBAA
constexpr uint32_t kOccupancyColor = 0x606878FFu; // 占有面 (くすんだ青灰)
constexpr uint32_t kListenerColor = 0x40FF90FFu;  // 聴者 (緑)
constexpr uint32_t kHeardColor = 0xFFD040FFu;     // 最後に聞いた位置への線 (黄)

// 音色 0..3 の色。M65e のライティングでも同じ 4 色を使う予定なので、
// **ここが色の正本**になる (先に決めておかないと絵とデバッグ線で色が食い違う)
constexpr uint32_t kToneColors[4] = {
    0x60C0FFFFu, // 0 = 低い / 鈍い音 (水色)
    0xFFFFFFFFu, // 1 = 中庸 (白)
    0xFFA040FFu, // 2 = 硬い音 (橙)
    0xFF60C0FFu, // 3 = 金属質 (桃)
};

// 1 フレームに積む線の上限。**この cap が無いと 128x32x128 の占有で 150 万本になる**
constexpr size_t kMaxOccupancyLines = 24000;
constexpr size_t kMaxFrontierLines = 24000;

constexpr float kOriginArm = 0.35f; // 音源の十字の腕 [m]
constexpr float kListenerArm = 0.25f;

void PushLine(std::vector<DebugLineCmd>& out, float ax, float ay, float az, float bx, float by,
              float bz, uint32_t rgba)
{
    DebugLineCmd c;
    c.ax = ax;
    c.ay = ay;
    c.az = az;
    c.bx = bx;
    c.by = by;
    c.bz = bz;
    c.rgba = rgba;
    out.push_back(c);
}

void PushCross(std::vector<DebugLineCmd>& out, float x, float y, float z, float arm, uint32_t rgba)
{
    PushLine(out, x - arm, y, z, x + arm, y, z, rgba);
    PushLine(out, x, y - arm, z, x, y + arm, z, rgba);
    PushLine(out, x, y, z - arm, x, y, z + arm, rgba);
}

// エネルギー 0..1 を色の明るさに乗せる。**アルファは触らない** (線が消えると
// 「届いていない」と誤読するため — 弱い波は暗い色で描く)
uint32_t ScaleColor(uint32_t rgba, float t)
{
    const float k = std::clamp(t, 0.15f, 1.0f);
    const auto ch = [&](int shift) {
        const uint32_t v = (rgba >> shift) & 0xFFu;
        return static_cast<uint32_t>(static_cast<float>(v) * k) << shift;
    };
    return ch(24) | ch(16) | ch(8) | (rgba & 0xFFu);
}

} // namespace

AcousticDebugFlags& GetAcousticDebugFlags()
{
    static AcousticDebugFlags flags;
    return flags;
}

void BuildAcousticDebugLines(World& world, const AcousticField& field,
                             const AcousticDebugFlags& flags, std::vector<DebugLineCmd>& out)
{
    if (!flags.Any() || !field.HasVolume()) {
        return;
    }
    const AcousticGridDesc& g = field.Grid();
    const float arm = g.cellSize * 0.35f;

    // ---- 占有: 開セルに面している閉セルだけ ----
    // 「内部まで全部描く」と箱が塗り潰されて、肝心の壁の形が見えなくなる
    if (flags.occupancy) {
        size_t drawn = 0;
        for (int32_t cz = 0; cz < g.dimZ && drawn < kMaxOccupancyLines; ++cz) {
            for (int32_t cy = 0; cy < g.dimY && drawn < kMaxOccupancyLines; ++cy) {
                for (int32_t cx = 0; cx < g.dimX && drawn < kMaxOccupancyLines; ++cx) {
                    if (!field.IsSolid(cx, cy, cz)) {
                        continue;
                    }
                    const bool exposed =
                        !field.IsSolid(cx - 1, cy, cz) || !field.IsSolid(cx + 1, cy, cz)
                        || !field.IsSolid(cx, cy - 1, cz) || !field.IsSolid(cx, cy + 1, cz)
                        || !field.IsSolid(cx, cy, cz - 1) || !field.IsSolid(cx, cy, cz + 1);
                    if (!exposed) {
                        continue;
                    }
                    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
                    acoustic::CellToWorldCenter(g, cx, cy, cz, wx, wy, wz);
                    PushCross(out, wx, wy, wz, arm, kOccupancyColor);
                    drawn += 3;
                }
            }
        }
    }

    // ---- 波面と音源 ----
    const std::vector<AcousticField::Wave>& waves = field.Waves();
    for (uint32_t s = 0; s < AcousticField::kMaxWaves; ++s) {
        const AcousticField::Wave& w = waves[s];
        if (w.active == 0) {
            continue;
        }
        const uint32_t color = kToneColors[w.tone & 3u];
        if (flags.waveOrigin) {
            float wx = 0.0f, wy = 0.0f, wz = 0.0f;
            acoustic::CellToWorldCenter(g, w.ox, w.oy, w.oz, wx, wy, wz);
            PushCross(out, wx, wy, wz, kOriginArm, color);
        }
        if (!flags.frontier || w.ring == 0) {
            continue;
        }
        // 直近に確定したリング = [(ring-1)*11, ring*11)。局所ボックスを舐めて拾う。
        // バケットは処理後に空にしているので距離配列から引き直すのが正しい
        const AcousticField::WaveField& f = field.FieldOf(s);
        const uint16_t lo = static_cast<uint16_t>((w.ring - 1) * acoustic::kFaceCost);
        const uint16_t hi = static_cast<uint16_t>(w.ring * acoustic::kFaceCost);
        size_t drawn = 0;
        for (int32_t lz = 0; lz < f.sz && drawn < kMaxFrontierLines; ++lz) {
            for (int32_t ly = 0; ly < f.sy && drawn < kMaxFrontierLines; ++ly) {
                for (int32_t lx = 0; lx < f.sx && drawn < kMaxFrontierLines; ++lx) {
                    const size_t li =
                        static_cast<size_t>((lz * f.sy + ly) * f.sx + lx);
                    const uint16_t d = f.dist[li];
                    if (d < lo || d >= hi) {
                        continue;
                    }
                    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
                    acoustic::CellToWorldCenter(g, lx + f.x0, ly + f.y0, lz + f.z0, wx, wy, wz);
                    const float e = acoustic::EnergyAt(d, f.maxDist, w.amplitude, g.cellSize);
                    const uint32_t c = ScaleColor(color, e);
                    const uint8_t pd = f.parentDir[li];
                    if (pd == AcousticField::kNoParent) {
                        PushLine(out, wx, wy - arm, wz, wx, wy + arm, wz, c);
                    } else {
                        // ★線の向き = 親の方角。「どこから回り込んできたか」が
                        //   そのまま絵になる (M65f の AI が読む向きと同じ 1 バイト)
                        const acoustic::Neighbor& nb = acoustic::kNeighbors[pd];
                        PushLine(out, wx, wy, wz, wx + static_cast<float>(nb.dx) * arm,
                                 wy + static_cast<float>(nb.dy) * arm,
                                 wz + static_cast<float>(nb.dz) * arm, c);
                    }
                    ++drawn;
                }
            }
        }
    }

    // ---- 聴者 (鏡の中身は M65f から。それまでは十字だけが出る) ----
    if (flags.listener) {
        const ComponentTypeId req[] = { AcousticListenerComponent::sTypeId,
                                        WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int li = arch.FindTypeIndex(AcousticListenerComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* lc =
                    static_cast<const AcousticListenerComponent*>(arch.GetPtr(li, row));
                const auto& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const float px = wm.m[3][0], py = wm.m[3][1], pz = wm.m[3][2];
                PushCross(out, px, py, pz, kListenerArm, kListenerColor);
                if (lc->lastHeardTick != 0) {
                    PushLine(out, px, py, pz, lc->lastHeardPos.x, lc->lastHeardPos.y,
                             lc->lastHeardPos.z, kHeardColor);
                }
            }
        });
    }
}

} // namespace mye
