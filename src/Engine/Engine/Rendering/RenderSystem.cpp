#include "Engine/Engine/Rendering/RenderSystem.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <system_error>

#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Renderer/PostFx/FxStackAsset.h"  // M78c: fxstack ロード
#include "Engine/Renderer/Compute/ProjectComputeRunner.h" // M78d: DispatchPointFromString
#include "Engine/Renderer/PostFx/ProjectFxStackPolicy.h" // M78 §4.1 CameraOverride
#include "Engine/Core/Diagnostics/Profiler.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Acoustic/AcousticField.h" // M65d: 残光ボリュームの転送元
#include "Engine/Engine/Disturbance/DisturbanceRender.h" // M96: 場の 2D 層の転送指示
#include "Engine/Engine/Physics/Fracture/FractureSystem.h" // M80g: root proxy の可視規則を FractureSystem と共有
#include "Engine/Engine/Navigation/NavDebugDraw.h" // M82e: ナビメッシュの塗り・輪郭
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/Rendering/DistanceCull.h"
#include "Engine/Engine/Physics/Ragdoll/Ragdoll.h" // M60g1: 剛体が骨を駆動しているときのパレット
#include "Engine/Engine/Animation/SkinningSystem.h" // M18 追補: クロスフェード込みのポーズ評価
#include "Engine/Engine/RayTracing/RtScope.h" // RT の適用範囲 (個別設定 / タグ規則)
#include "Engine/Engine/Vfx/VfxRenderer.h"
#include "Engine/Platform/PathUtil.h" // WideToUtf8 (fxstack のパスをログへ)
#include "Engine/Renderer/Pipeline/FrustumCull.h"
#include "Engine/Renderer/Pipeline/PixelPerfect2D.h" // M94v
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/PostFx/PostFxMath.h" // M55b: camerajitter
#include "Engine/Renderer/Pipeline/RenderPath.h"
#include "Engine/Renderer/Passes/SkyResolve.h"

using namespace DirectX;

namespace mye {
namespace {

// M94c: 手触りの FOV キックの結果をこの範囲へ収める (0 や 180 度近くは射影が壊れる)
constexpr float kFeelMinFovDeg = 5.0f;
constexpr float kFeelMaxFovDeg = 170.0f;

// M94c: 画面効果のパルスを実効のポスト設定へ加算する。露出は倍率への加算 (0.5 = 1.5 倍)、
// フラッシュ色は乗算フィルタを重みぶん flashColor へ寄せる。どれも負にならないようにする
void ApplyFeelPostFxPulse(PostProcess::Settings& s, const FeelFrameOutput& feel)
{
    s.vignetteIntensity = std::max(0.0f, s.vignetteIntensity + feel.vignette);
    s.chromAberration = std::max(0.0f, s.chromAberration + feel.chromAberration);
    s.exposure = std::max(0.0f, s.exposure * (1.0f + feel.exposure));
    s.saturation = std::max(0.0f, s.saturation + feel.saturation);
    const float w = std::clamp(feel.flashWeight, 0.0f, 1.0f);
    s.colorFilter.x += (feel.flashColor[0] - s.colorFilter.x) * w;
    s.colorFilter.y += (feel.flashColor[1] - s.colorFilter.y) * w;
    s.colorFilter.z += (feel.flashColor[2] - s.colorFilter.z) * w;
}

// フラスタムカリング候補 (M25: 収集は直列、可視判定 + viewZ を並列、キュー構築は直列)。
struct CullCand {
    EntityID e;
    AssetID mesh;
    AssetID material;
    XMFLOAT4X4 world;
    const Mesh* meshPtr;
    float viewZ;
    uint8_t visible;
    uint8_t skinned;
    // M79 sub-06: サーフェスマテリアルの視錐台余白 [m] (.mat.json boundsPadding、非サーフェスは 0)。
    // MaterialLibrary の横テーブル参照はステージ 1 (直列) で解決しておく — ステージ 2 は
    // ジョブ並列の純関数なので、その中でテーブルを引くと並列化の前提 (要素独立) が崩れる
    float boundsPadding = 0.0f;
    // 視錐台判定に使うローカル AABB。非スキンはメッシュの AABB、スキンは全姿勢を包む保守的 AABB (SkinBounds.h)
    XMFLOAT3 boundsMin = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 boundsMax = { 0.0f, 0.0f, 0.0f };
    // 箱で判定できない (ラグドール作動中・スキンの箱を求められない) = 常に可視
    uint8_t alwaysVisible = 0;
    // メッシュ LOD の段 (段のあるメッシュだけ。ステージ 2 で決める)
    uint8_t lod = 0;
    // スキンの画面での大きさ (外接球の高さ比、全姿勢を包む箱から。ステージ 2 で決める)。URO の間隔の出所。
    // 求められない (カメラ無し) ときは FLT_MAX = 間引かない
    float uroSize = FLT_MAX;
    // 距離カリング: 実効の最大描画距離 (0 = 判定しない。ステージ 1 で物ごと・タグ・既定から決める) と、その判定の結果
    float maxDrawDistance = 0.0f;
    uint8_t tooFar = 0;
};

constexpr size_t kCullGrain = 256; // これ未満は直列 (スレッド起動コスト回避)

// 2 つの時刻の差 [ms]
float ElapsedMs(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
    return std::chrono::duration<float, std::milli>(to - from).count();
}

// ---- M65d: `--acoustic-dump N` の読み戻し検査 ----
//
// ★「転送が正しい」を数値で主張できる唯一の手段。GPU から読み戻したバイト列を
//   **CPU 側の残光配列とバイト単位で**突き合わせる — RowPitch と DepthPitch を取り違えると Z がずれた絵になり、
//   しかも絵は普通に出るので目視では絶対に見つからない。
// 併せて「残光が閉セルに入っていないこと」も数える。閉セルは波が絶対に訪れないので、
// ここが 0 でなければ伝播か転送のどちらかが壁を越えている。
void DumpAcousticVolume(GraphicsDevice& device, const AcousticVolumePass& pass,
                        const AcousticField& field)
{
    std::vector<uint8_t> gpu;
    if (!pass.Volume().ReadbackBytes(device, gpu)) {
        MYE_LOG_ERROR("[acoustic-dump] readback failed (R8_UNORM の Texture3D が読めない)");
        return;
    }
    const std::vector<uint8_t>& cpu = field.Glow();
    const AcousticGridDesc& g = field.Grid();
    if (gpu.size() != cpu.size()) {
        MYE_LOG_ERROR("[acoustic-dump] size mismatch: gpu %zu vs cpu %zu", gpu.size(), cpu.size());
        return;
    }
    size_t mismatch = 0;
    int32_t firstX = -1, firstY = -1, firstZ = -1;
    size_t nonZero = 0, solidLit = 0;
    uint8_t maxV = 0;
    for (int32_t z = 0; z < g.dimZ; ++z) {
        for (int32_t y = 0; y < g.dimY; ++y) {
            for (int32_t x = 0; x < g.dimX; ++x) {
                const size_t i = static_cast<size_t>(acoustic::CellIndex(g, x, y, z));
                if (gpu[i] != cpu[i]) {
                    if (mismatch == 0) {
                        firstX = x;
                        firstY = y;
                        firstZ = z;
                    }
                    ++mismatch;
                }
                if (cpu[i] != 0) {
                    ++nonZero;
                    maxV = (cpu[i] > maxV) ? cpu[i] : maxV;
                    if (field.IsSolid(x, y, z)) {
                        ++solidLit;
                    }
                }
            }
        }
    }
    uint32_t activeWaves = 0;
    for (const AcousticField::Wave& w : field.Waves()) {
        activeWaves += (w.active != 0) ? 1u : 0u;
    }
    MYE_LOG_INFO("[acoustic-dump] grid %dx%dx%d (%zu cells, %.1f KB) / active waves %u / "
                 "upload %.3f ms",
                 g.dimX, g.dimY, g.dimZ, cpu.size(),
                 static_cast<double>(cpu.size()) / 1024.0, activeWaves,
                 static_cast<double>(pass.LastUploadMs()));
    MYE_LOG_INFO("[acoustic-dump] glow: %zu non-zero (%.2f%%), max %u (energy %.5f)", nonZero,
                 cpu.empty() ? 0.0 : 100.0 * static_cast<double>(nonZero) / static_cast<double>(cpu.size()),
                 static_cast<unsigned>(maxV), static_cast<double>(acoustic::DecodeGlow(maxV)));
    if (mismatch != 0) {
        MYE_LOG_ERROR("[acoustic-dump] GPU/CPU MISMATCH: %zu cells (first at %d,%d,%d: gpu %u vs "
                      "cpu %u) — RowPitch/DepthPitch を疑うこと",
                      mismatch, firstX, firstY, firstZ,
                      static_cast<unsigned>(gpu[static_cast<size_t>(acoustic::CellIndex(g, firstX, firstY, firstZ))]),
                      static_cast<unsigned>(cpu[static_cast<size_t>(acoustic::CellIndex(g, firstX, firstY, firstZ))]));
    } else {
        MYE_LOG_INFO("[acoustic-dump] GPU readback matches the CPU field byte for byte");
    }
    if (solidLit != 0) {
        MYE_LOG_ERROR("[acoustic-dump] %zu solid cells are lit — 波が壁の中へ入っている",
                      solidLit);
    } else {
        MYE_LOG_INFO("[acoustic-dump] no solid cell is lit (壁の向こうは完全にゼロ)");
    }

    // ---- 2026-09-12「描画だけ円」: 残光の byte と解析的な波面のモデル値を同じセルで並べる ----
    // シェーダ (acoustic_common.hlsli AcousticFront) と同じ式を CPU で引き、原点から +x へ
    // 1 セルずつ「残光 / 円」を出す。両者が大きくずれたら減衰モデルか半径の取り方が違う
    const float keepRaw = field.GlowKeepPerTick();
    const float keep = (keepRaw > 0.0f && keepRaw < 1.0f) ? keepRaw : acoustic::kGlowDecayPerTick;
    const AcousticField::FrontWave* fws = field.FrontWaves();
    const std::vector<uint32_t>& mask = field.FrontMask();
    for (uint32_t s = 0; s < AcousticField::kMaxWaves; ++s) {
        const AcousticField::FrontWave& fw = fws[s];
        if (fw.active == 0) {
            continue;
        }
        int32_t ox = 0, oy = 0, oz = 0;
        if (!acoustic::WorldToCell(g, fw.ox, fw.oy, fw.oz, ox, oy, oz)) {
            continue;
        }
        MYE_LOG_INFO("[acoustic-dump] front slot %u: origin cell (%d,%d,%d) R=%.2fm amp=%.2f "
                     "maxD=%.2fm tick/m=%.1f extraAge=%.0f keep=%.4f",
                     s, ox, oy, oz, static_cast<double>(fw.radiusM), static_cast<double>(fw.amplitude),
                     static_cast<double>(fw.maxDistM), static_cast<double>(fw.ticksPerMetre),
                     static_cast<double>(fw.extraAgeTicks), static_cast<double>(keep));
        std::string row;
        for (int32_t k = 0; ox + k < g.dimX && k <= 24; ++k) {
            const size_t ci = static_cast<size_t>(acoustic::CellIndex(g, ox + k, oy, oz));
            const unsigned glow = cpu[ci];
            const bool bit = !mask.empty() && (mask[ci] & (1u << s)) != 0;
            const float d = static_cast<float>(k) * g.cellSize;
            float model = 0.0f;
            if (bit && d < fw.radiusM && d < fw.maxDistM) {
                const float ratio = g.cellSize / (std::max)(d, g.cellSize);
                float t = std::sqrt(std::sqrt((std::min)(fw.amplitude * ratio * ratio, 1.0f)));
                t *= (std::min)(1.0f, (std::max)(0.0f, (fw.radiusM - d) / (0.5f * g.cellSize)));
                const float age = (fw.radiusM - d) * fw.ticksPerMetre + fw.extraAgeTicks;
                const float vStar = 1.0f / (std::max)(1.0f - keep, 1e-4f);
                float v = t * 255.0f;
                float n = age;
                if (v > vStar) {
                    const float n1 = (std::min)(n, std::log(vStar / v) / std::log(keep));
                    v = v * std::exp(n1 * std::log(keep)) - 0.5f * n1;
                    n -= n1;
                }
                v -= n;
                model = (std::max)(v, 0.0f);
            }
            char buf[48];
            snprintf(buf, sizeof(buf), " %u/%.0f%s", glow, static_cast<double>(model), bit ? "" : "-");
            row += buf;
        }
        MYE_LOG_INFO("[acoustic-dump]   +x: glow/circle per cell (- = no line of sight):%s", row.c_str());
        // 原点の層と、床の画素がサンプルする層 (y = 0 + 法線押し出し) を上から見た見通しビットの
        // 地図 (# = 円を描く / . = 残光だけ / X = 壁)。自由空間で # の縁が凸でなければ
        // 見通し判定の閾値が低すぎる
        int32_t fx = 0, fy = oy, fz = 0;
        (void)acoustic::WorldToCell(g, fw.ox, 0.75f * g.cellSize, fw.oz, fx, fy, fz);
        const int32_t layers[2] = { oy, fy };
        for (int li = 0; li < (fy == oy ? 1 : 2); ++li) {
            const int32_t ly = layers[li];
            MYE_LOG_INFO("[acoustic-dump]   layer y=%d (%s):", ly, li == 0 ? "origin" : "floor sample");
            const int32_t rad = 20;
            for (int32_t z = oz - rad; z <= oz + rad; ++z) {
                if (z < 0 || z >= g.dimZ) {
                    continue;
                }
                std::string line;
                for (int32_t x = ox - rad; x <= ox + rad; ++x) {
                    if (x < 0 || x >= g.dimX) {
                        line += ' ';
                        continue;
                    }
                    const size_t ci = static_cast<size_t>(acoustic::CellIndex(g, x, ly, z));
                    if (field.IsSolid(x, ly, z)) {
                        line += 'X';
                    } else if (x == ox && z == oz) {
                        line += 'O';
                    } else if (!mask.empty() && (mask[ci] & (1u << s)) != 0) {
                        line += '#';
                    } else {
                        line += (cpu[ci] != 0) ? '.' : ' ';
                    }
                }
                MYE_LOG_INFO("[acoustic-dump]   |%s|", line.c_str());
            }
        }
    }
}


// 前 tick → 現 tick のワールド行列を成分 lerp する (M36b 描画補間)。
// 平行移動は厳密 lerp、回転 3x3 は成分 lerp — 1/60s の姿勢差では非直交化は不可視。
// render-only なので float 誤差は sim/hash に無関係
XMFLOAT4X4 LerpWorld(const XMFLOAT4X4& a, const XMFLOAT4X4& b, float t)
{
    XMFLOAT4X4 o;
    const float* pa = &a._11;
    const float* pb = &b._11;
    float* po = &o._11;
    for (int i = 0; i < 16; ++i) {
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
    }
    return o;
}

// 車輪の見た目 (M60i)。**描画専用の合成で、ワールドハッシュは 1 バイトも増えない**。
// ソルバは車輪の LocalTransform を書かない (書くとハッシュ対象が増える) ので、
// 転がり・切れ角・サスの伸縮は出力フィールドを読んでここで絵にするしかない
// — ラグドールのボーンパレット (M60g1) とまったく同じ「決定論の面積を広げない」判断。
//
// エンティティの**ローカル空間**で ①車軸 (ローカル X) まわりに転がし ②切れ角ぶん
// ローカル Y まわりに回し ③サスの伸びぶんローカル -Y へ下げる、の順に掛ける。
// 車輪の取り付け点は「サスの上端」なので、③が無いと車輪が宙に浮いて見える。
//
// ★ローカル空間 = **スケールが掛かる前**。したがって非一様スケールの車輪は回すと歪む
//   (v1 の制限。車輪の寸法は Wheel の radius が正本なので、メッシュ側に焼くのが本筋)。
// ★**子エンティティへは伝播しない** — 車輪メッシュは車輪エンティティ自身に置くこと。
XMFLOAT4X4 ApplyWheelVisual(const XMFLOAT4X4& world, const WheelComponent& wc)
{
    const XMMATRIX m = XMMatrixRotationX(wc.rotationAngle) * XMMatrixRotationY(wc.steerAngle)
                       * XMMatrixTranslation(0.0f, wc.compression - wc.restLength, 0.0f);
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixMultiply(m, XMLoadFloat4x4(&world)));
    return out;
}

// 平行光の view-proj (行ベクトル規約 world*view*proj)。シーン AABB にフィットした正射影。
// 戻り値は非転置 (ShadowPass が world と合成、RenderView 用に別途転置する)。
XMFLOAT4X4 ComputeDirectionalLightVP(const XMFLOAT3& lightDir, const XMFLOAT3& sceneMin,
                                     const XMFLOAT3& sceneMax)
{
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDir));
    const XMFLOAT3 cf = { (sceneMin.x + sceneMax.x) * 0.5f, (sceneMin.y + sceneMax.y) * 0.5f,
                          (sceneMin.z + sceneMax.z) * 0.5f };
    const XMVECTOR center = XMLoadFloat3(&cf);
    const XMFLOAT3 ext = { sceneMax.x - sceneMin.x, sceneMax.y - sceneMin.y,
                           sceneMax.z - sceneMin.z };
    const float radius = 0.5f * std::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z);
    const XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(dir, radius * 2.0f + 1.0f));
    const bool nearlyVertical = std::fabs(XMVectorGetY(dir)) > 0.99f;
    const XMVECTOR up = nearlyVertical ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    const XMMATRIX lightView = XMMatrixLookToLH(eye, dir, up);

    const XMFLOAT3 corners[8] = {
        { sceneMin.x, sceneMin.y, sceneMin.z }, { sceneMax.x, sceneMin.y, sceneMin.z },
        { sceneMin.x, sceneMax.y, sceneMin.z }, { sceneMax.x, sceneMax.y, sceneMin.z },
        { sceneMin.x, sceneMin.y, sceneMax.z }, { sceneMax.x, sceneMin.y, sceneMax.z },
        { sceneMin.x, sceneMax.y, sceneMax.z }, { sceneMax.x, sceneMax.y, sceneMax.z },
    };
    float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
    for (const XMFLOAT3& c : corners) {
        const XMVECTOR vv = XMVector3TransformCoord(XMLoadFloat3(&c), lightView);
        const float x = XMVectorGetX(vv), y = XMVectorGetY(vv), z = XMVectorGetZ(vv);
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
        minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
    }
    const float margin = radius * 0.05f + 0.5f;
    const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(minX - margin, maxX + margin,
                                                               minY - margin, maxY + margin,
                                                               std::max(0.05f, minZ - 1.0f),
                                                               maxZ + 1.0f);
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixMultiply(lightView, lightProj));
    return out;
}

// スポットライトの view-proj (M54c、行ベクトル規約 world*view*proj)。戻り値は非転置。
// 円錐をちょうど包む透視 1 面。fov は外角の 2 倍 + 余白 —
// ★余白が要る理由: 3x3 PCF はタイル境界の外へタップを伸ばす。円錐の縁が
//   ぴったり枠に接していると、縁だけ「タイル外 = 影なし」に落ちて輪郭が硬くなる。
// near は 0.05 固定 (これ以上手前は減衰式でもほぼ寄与しない)、far は減衰半径。
// far を range に合わせるのは深度精度のため — シーン全体に合わせると近傍が潰れる。
XMFLOAT4X4 ComputeSpotLightVP(const XMFLOAT3& position, const XMFLOAT3& direction, float cosOuter,
                              float range)
{
    const XMVECTOR eye = XMLoadFloat3(&position);
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&direction));
    const bool nearlyVertical = std::fabs(XMVectorGetY(dir)) > 0.99f;
    const XMVECTOR up = nearlyVertical ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    const XMMATRIX lightView = XMMatrixLookToLH(eye, dir, up);

    const float clamped = std::min(std::max(cosOuter, -0.99f), 0.9999f);
    const float halfAngle = std::acos(clamped);
    // 余白 6 度。上限 85 度 (= fov 170 度) は透視行列が発散しない範囲
    const float fovY = std::min(2.0f * (halfAngle + XMConvertToRadians(6.0f)),
                                XMConvertToRadians(170.0f));
    const float farZ = std::max(range, 1.0f);
    const XMMATRIX lightProj = XMMatrixPerspectiveFovLH(fovY, 1.0f, 0.05f, farZ);

    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixMultiply(lightView, lightProj));
    return out;
}

// D3D cubemap 面順 (+X,-X,+Y,-Y,+Z,-Z) の forward/up 基底 (M54d)。
// **EnvMapBaker.cpp の kFaces と同一表を意図的に複製している** — あちらは IBL ベイクの
// 匿名 namespace にあり、公開すると「環境マップの都合」がシャドウ側の依存になる。
// HLSL 側 common.hlsli の CubeFaceIndex もこの順番。3 者がずれると絵は出るが合わない
struct CubeFaceBasis {
    XMFLOAT3 forward;
    XMFLOAT3 up;
};
constexpr CubeFaceBasis kCubeFaces[6] = {
    { { 1, 0, 0 }, { 0, 1, 0 } },  // +X
    { { -1, 0, 0 }, { 0, 1, 0 } }, // -X
    { { 0, 1, 0 }, { 0, 0, -1 } }, // +Y
    { { 0, -1, 0 }, { 0, 0, 1 } }, // -Y
    { { 0, 0, 1 }, { 0, 1, 0 } },  // +Z
    { { 0, 0, -1 }, { 0, 1, 0 } }, // -Z
};

// 点光源 1 面ぶんの lightViewProj (M54d)。face は kCubeFaces の添字。
// ★fov はちょうど 90 度ではなく「タイル境界に PCF 用の余白を marginTexels 取る」ぶんだけ
//   広い。CubeFaceIndex はちょうど 90 度で面を切り替えるので、90 度で焼くと境界画素の
//   3x3 タップがタイルの外へ出る → SampleShadowAtlas の clamp が働いて自分の深度でなく
//   縁の深度を舐め、面の継ぎ目に沿って影の線が走る。tan(fov/2) を 1+2m/S にすると
//   90 度境界がタイル内側 m テクセルへ寄る (実測 S=1024 / m=2 で継ぎ目が消える)
XMFLOAT4X4 ComputePointLightFaceVP(const XMFLOAT3& position, int face, float range, int tileSize)
{
    const CubeFaceBasis& b = kCubeFaces[(face < 0 || face > 5) ? 0 : face];
    const XMVECTOR eye = XMLoadFloat3(&position);
    const XMMATRIX lightView =
        XMMatrixLookToLH(eye, XMLoadFloat3(&b.forward), XMLoadFloat3(&b.up));
    constexpr float kMarginTexels = 2.0f;
    const float s = static_cast<float>(std::max(tileSize, 16));
    const float fovY = 2.0f * std::atan(1.0f + 2.0f * kMarginTexels / s);
    const float farZ = std::max(range, 1.0f);
    const XMMATRIX lightProj = XMMatrixPerspectiveFovLH(fovY, 1.0f, 0.05f, farZ);

    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixMultiply(lightView, lightProj));
    return out;
}

// CSM のカスケード VP 列 (M38d)。カメラの部分フラスタム 8 隅をライトビューへ射影して
// フィットした ortho を作る。practical split (λ=0.5)、影距離は kShadowMaxDist まで。
// テクセルスナップで安定化。ライト方向のキャスター (フラスタム外) を拾うため
// 近平面はシーン AABB のライト空間 z まで引き戻す。
// 非 perspective (エディタ Ortho ビュー) は従来のシーン全体フィットを全カスケードに複製。
constexpr float kShadowMaxDist = 60.0f;

void ComputeCascadeVPs(const XMFLOAT3& lightDir, const XMFLOAT3& sceneMin,
                       const XMFLOAT3& sceneMax, const RenderView& view, int resolution,
                       XMFLOAT4X4* outVPs, float* outSplits, int count)
{
    const XMMATRIX camView = XMLoadFloat4x4(&view.view);
    // M55b: カスケードのフィットは非ジッタ側で行う。ジッタ付きだとカメラフラスタムの
    // 8 隅がサブピクセル分毎フレーム動き、テクセルスナップで殺したはずの影の揺れが戻る
    const XMMATRIX camProj = XMLoadFloat4x4(&view.projNoJitter);
    XMFLOAT4X4 pj;
    XMStoreFloat4x4(&pj, camProj);
    const bool perspective = std::fabs(pj._34 - 1.0f) < 1e-3f; // LH perspective は _34 == 1

    if (!perspective || resolution <= 0) {
        const XMFLOAT4X4 whole = ComputeDirectionalLightVP(lightDir, sceneMin, sceneMax);
        for (int c = 0; c < count; ++c) {
            outVPs[c] = whole;
            outSplits[c] = kShadowMaxDist;
        }
        return;
    }

    // 射影行列から near/far を復元 (行ベクトル規約: zNdc = P33 + P43/viewZ)
    const float nearZ = (std::fabs(pj._33) > 1e-6f) ? (-pj._43 / pj._33) : 0.1f;
    const float rawFar = (std::fabs(1.0f - pj._33) > 1e-6f) ? (pj._43 / (1.0f - pj._33)) : 1000.0f;
    const float farZ = std::min(std::max(rawFar, nearZ + 1.0f), kShadowMaxDist);
    ComputeCascadeSplits(nearZ, farZ, count, 0.5f, outSplits);

    // 共有ライトビュー (原点はシーン中心)
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDir));
    const XMFLOAT3 cf = { (sceneMin.x + sceneMax.x) * 0.5f, (sceneMin.y + sceneMax.y) * 0.5f,
                          (sceneMin.z + sceneMax.z) * 0.5f };
    const XMFLOAT3 ext = { sceneMax.x - sceneMin.x, sceneMax.y - sceneMin.y,
                           sceneMax.z - sceneMin.z };
    const float radius = 0.5f * std::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z);
    const XMVECTOR eye =
        XMVectorSubtract(XMLoadFloat3(&cf), XMVectorScale(dir, radius * 2.0f + 1.0f));
    const bool nearlyVertical = std::fabs(XMVectorGetY(dir)) > 0.99f;
    const XMVECTOR up = nearlyVertical ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    const XMMATRIX lightView = XMMatrixLookToLH(eye, dir, up);

    // シーン AABB のライト空間 z 範囲 (フラスタム外キャスターの取りこぼし防止)
    float sceneMinZ = FLT_MAX, sceneMaxZ = -FLT_MAX;
    for (int i = 0; i < 8; ++i) {
        const XMFLOAT3 c = { (i & 1) ? sceneMax.x : sceneMin.x, (i & 2) ? sceneMax.y : sceneMin.y,
                             (i & 4) ? sceneMax.z : sceneMin.z };
        const float z = XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&c), lightView));
        sceneMinZ = std::min(sceneMinZ, z);
        sceneMaxZ = std::max(sceneMaxZ, z);
    }

    const XMMATRIX invVP = XMMatrixInverse(nullptr, XMMatrixMultiply(camView, camProj));
    auto ndcZ = [&](float viewZ) { return pj._33 + pj._43 / viewZ; };
    float splitNear = nearZ;
    for (int c = 0; c < count; ++c) {
        const float splitFar = outSplits[c];
        // 部分フラスタムの 8 隅 (NDC → ワールド)
        float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
        float minZ = FLT_MAX, maxZ = -FLT_MAX;
        for (int i = 0; i < 8; ++i) {
            const float nx = (i & 1) ? 1.0f : -1.0f;
            const float ny = (i & 2) ? 1.0f : -1.0f;
            const float nz = (i & 4) ? ndcZ(splitFar) : ndcZ(splitNear);
            const XMVECTOR w = XMVector3TransformCoord(XMVectorSet(nx, ny, nz, 0), invVP);
            const XMVECTOR lv = XMVector3TransformCoord(w, lightView);
            const float x = XMVectorGetX(lv), y = XMVectorGetY(lv), z = XMVectorGetZ(lv);
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
        }
        // テクセルスナップ (カメラ移動でのシャドウエッジのちらつき防止)
        const float margin = 1.0f;
        minX -= margin; maxX += margin;
        minY -= margin; maxY += margin;
        const float texelX = (maxX - minX) / static_cast<float>(resolution);
        const float texelY = (maxY - minY) / static_cast<float>(resolution);
        if (texelX > 0.0f && texelY > 0.0f) {
            minX = std::floor(minX / texelX) * texelX;
            minY = std::floor(minY / texelY) * texelY;
            maxX = std::floor(maxX / texelX) * texelX;
            maxY = std::floor(maxY / texelY) * texelY;
        }
        // 近平面はシーン AABB まで引き戻す (ライト方向の手前にいるキャスターを含める)
        const float zNear = std::min(minZ, sceneMinZ) - 1.0f;
        const float zFar = std::min(maxZ + 1.0f, sceneMaxZ + 1.0f) + 1.0f;
        const XMMATRIX lightProj =
            XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, zNear, std::max(zFar, zNear + 1.0f));
        XMStoreFloat4x4(&outVPs[c], XMMatrixMultiply(lightView, lightProj));
        splitNear = splitFar;
    }
}

// 最初の平行光の添字 (CSM の光)。無ければ -1
int FindDirectionalLight(const SceneLightData& lights)
{
    for (int i = 0; i < lights.count; ++i) {
        if (lights.lights[i].type == lighttype::kDirectional) {
            return i;
        }
    }
    return -1;
}

// 影を投げる局所ライト (シャドウアトラスの担当) が 1 つでも選ばれているか
bool HasLocalShadowLight(const LightSelection& selection)
{
    for (int i = 0; i < selection.count; ++i) {
        if (selection.lights[i].shadowSlot >= 0) {
            return true;
        }
    }
    return false;
}

} // namespace

Texture* LoadTextureRememberingFailure(TextureLibrary& textures, AssetID id, const std::wstring& path,
                                       bool srgb, FailedTextureLoad& failed)
{
    if (Texture* loaded = textures.Get(id)) {
        return loaded;
    }
    std::error_code stampEc;
    const auto writeTime = std::filesystem::last_write_time(path, stampEc);
    const int64_t stamp = stampEc ? 0 : static_cast<int64_t>(writeTime.time_since_epoch().count());
    if (!ShouldRetryTextureLoad(failed, id.value, stamp)) {
        return nullptr; // 前回と同じ中身で失敗済み (エラーログは初回の 1 回だけ)
    }
    textures.LoadFile(path, srgb);
    Texture* tex = textures.Get(id);
    failed = tex ? FailedTextureLoad{} : FailedTextureLoad{ id.value, stamp };
    return tex;
}

// 霧のノイズのマップ (M96) を AssetID から解決する。LUT と同じ遅延ロードで、ノイズはデータなので srgb=false。
// 解決できなければ null = そのノイズのマップは掛からない (数値のノイズと霧そのものは描かれる)
void ResolveDisturbanceNoiseMap(TextureLibrary& textures, DisturbanceNoiseUpload& noise, FailedTextureLoad& failed)
{
    noise.mapSRV = nullptr;
    if (noise.mapAsset == 0 || !(noise.mapStrength > 0.0f)) {
        return;
    }
    const AssetID id{ noise.mapAsset };
    Texture* tex = textures.Get(id);
    if (tex == nullptr) {
        const std::wstring path = assetguid::ResolvePath(id.value);
        if (!path.empty()) {
            tex = LoadTextureRememberingFailure(textures, id, path, /*srgb=*/false, failed);
        }
    }
    if (tex != nullptr && tex->srv) {
        noise.mapSRV = tex->srv.Get();
    }
}

// 霧のノイズの時計 [s]。水面と同じく sim の tick の時刻を描画の補間で割る (描画フレーム数では進めない =
// 60 FPS 以外でも同じ速さで流れ、決定的撮影で同じ絵になる)。tick 0 (編集中) は 0 = 模様は止まって見える
double DisturbanceNoiseTime(uint64_t simTick, float interpAlpha)
{
    if (simTick == 0) {
        return 0.0;
    }
    return (static_cast<double>(simTick - 1) + static_cast<double>(interpAlpha)) * (1.0 / 60.0);
}

void CollectEnvironment(World& world, RenderView& view)
{
    // 最初 (entity.index 最小) の active な Skybox
    uint32_t bestSky = 0xFFFFFFFFu;
    const ComponentTypeId skyReq[] = { SkyboxComponent::sTypeId };
    world.ForEachArchetype(skyReq, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SkyboxComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (e.index >= bestSky || !IsEntityActive(world, e)) {
                continue;
            }
            bestSky = e.index;
            const auto* sb = static_cast<const SkyboxComponent*>(arch.GetPtr(si, row));
            // M38b / M76: cubemap / panoramic 実装 — SRV 解決は RenderSystem 側 (この関数は純データのまま)
            view.skyMode = sb->mode;
            view.skyCubemapId = sb->cubemapTexture;
            view.skyTop = { sb->topColor.x, sb->topColor.y, sb->topColor.z };
            view.skyHorizon = { sb->horizonColor.x, sb->horizonColor.y, sb->horizonColor.z };
            view.skyBottom = { sb->bottomColor.x, sb->bottomColor.y, sb->bottomColor.z };
            // 2026-09-14: 星空と環境光の切り離し (純パススルー。丸めは SkyboxPass / IBL の判定側)
            view.skyStarDensity = sb->starDensity;
            view.skyStarBrightness = sb->starBrightness;
            view.skyStarTwinkle = sb->starTwinkle;
            view.skyStarCells = sb->starCells;
            view.skyLighting = sb->lighting;
        }
    });

    // 最初の active な Fog
    uint32_t bestFog = 0xFFFFFFFFu;
    const ComponentTypeId fogReq[] = { FogComponent::sTypeId };
    world.ForEachArchetype(fogReq, [&](Archetype& arch) {
        const int fi = arch.FindTypeIndex(FogComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (e.index >= bestFog || !IsEntityActive(world, e)) {
                continue;
            }
            bestFog = e.index;
            const auto* fog = static_cast<const FogComponent*>(arch.GetPtr(fi, row));
            view.fogMode = (fog->mode >= 0 && fog->mode <= 2) ? fog->mode : 0;
            view.fogColor = { fog->color.x, fog->color.y, fog->color.z };
            view.fogDensity = fog->density;
            view.fogStart = fog->start;
            view.fogEnd = fog->end;
            view.fogHeightFalloff = fog->heightFalloff; // M43a (純パススルー)
            view.fogBaseHeight = fog->baseHeight;
            view.fogInscatterIntensity = fog->inscatterIntensity;
            view.fogInscatterPower = fog->inscatterPower;
        }
    });
}

// RenderSystem::Render の段をまたぐ 1 フレームぶんの値。段の関数が上から順に埋め、後の段が読む
struct RenderSystem::FrameContext {
    RenderView view;                                   // この 1 回の描画の指示 (パスへ渡す)
    PostProcess::Target* hdr = nullptr;                // HDR 中間 (M16)。null = 直描き
    float aspectRatio = 1.0f;
    bool cameraFound = false;
    EntityID camEntity = kNullEntity;                  // シーンカメラの実体 (CameraPostFx 参照用、M29e)
    bool froxelOn = false;                             // M57c: この描画でフロクセルを回すか
    FroxelSettings effectiveFroxel;                    // グローバル設定 + シーンカメラの CameraPostFx
    Frustum frustum = {};                              // メッシュとライトのカリング (カメラがある時のみ)
    bool cullEnabled = false;                          // カメラがある (LOD・URO・地形・ライト選別の元栓)
    bool frustumDrop = false;                          // 視錐台の外のメッシュを落とす (cullEnabled && enableFrustumCulling)
    SceneLightData lights;                             // 選別後のライト。view.lights がこの実体を指す
    XMFLOAT3 sceneMin = { FLT_MAX, FLT_MAX, FLT_MAX };    // 不透明キャスターの world AABB (影のフィット、M17)
    XMFLOAT3 sceneMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool hasScene = false;
    bool distortionActive = false;                     // M42d: このフレーム歪みバッファを使ったか
    const std::vector<OcclusionDebugBox>* occlusionBoxes = nullptr; // --hzb-debug-max: パスが集めた (パスの寿命内だけ有効)
    // M94v: ピクセルパーフェクトカメラ (描画専用)
    float orthoSizeOverride = 0.0f;                    // > 0: 主カメラの orthoSize の代わりに使う
    bool pixelPerfect = false;                         // TAA ジッタと FXAA を止める (PixelPerfectCamera2D が有効な間は常に)
    float pixelSnapUnit = 0.0f;                        // > 0: カメラ位置をこの格子 (ワールド単位) へ丸める
};

namespace {

// 主カメラのピクセルパーフェクト設定から求めた描画計画 (ResolveCamera と同じ規則でカメラを選ぶ)
struct PixelPerfectPlan {
    bool active = false;             // 主カメラが正射影で PixelPerfectCamera2D を持つ
    bool snapping = false;
    float snapUnit = 0.0f;           // 1 / assetsPPU (ワールド単位)
    pixelperfect2d::Result result;
};

PixelPerfectPlan PlanPixelPerfect(World& world, int screenW, int screenH)
{
    PixelPerfectPlan plan;
    bool found = false;
    EntityID camEntity = kNullEntity;
    bool orthographic = false;
    const ComponentTypeId req[] = { CameraComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(CameraComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* c = static_cast<const CameraComponent*>(arch.GetPtr(ci, row));
            if (!found || c->isPrimary) {
                orthographic = c->orthographic;
                camEntity = arch.EntityAt(row);
                found = true;
                if (c->isPrimary) {
                    return;
                }
            }
        }
    });
    if (!found || !orthographic) {
        return plan;
    }
    const auto* pp = world.GetComponent<PixelPerfectCamera2DComponent>(camEntity);
    if (pp == nullptr) {
        return plan;
    }
    pixelperfect2d::Settings s;
    s.assetsPPU = pp->assetsPPU;
    s.refResolutionX = pp->refResolutionX;
    s.refResolutionY = pp->refResolutionY;
    s.upscaleRT = pp->upscaleRT;
    plan.active = true;
    plan.snapping = pp->pixelSnapping;
    plan.snapUnit = 1.0f / static_cast<float>(std::max(pp->assetsPPU, 1));
    plan.result = pixelperfect2d::Compute(screenW, screenH, s);
    return plan;
}

} // namespace

bool RenderSystem::Render(World& world, GraphicsDevice& device, IRenderPath& path,
                          ShaderManager& shaders, RenderResources& resources,
                          const FrameTarget& screenTarget, const CameraOverride* cameraOverride,
                          ParticleSystem* particles, VfxRenderer* vfx)
{
    prof::SetRenderStatsView(screenTarget.viewKey);
    if (!frameTimerInit_) {
        frameTimerInit_ = true;
        frameTimer_.Init(device); // 失敗しても計測が 0 になるだけ
    }
    frameTimer_.Begin(device);

    // ---- M94v: ピクセルパーフェクト。エディタ視界 (cameraOverride) は対象外 ----
    // PixelPerfectCamera2D の無いシーンは PlanPixelPerfect が active=false を返すだけで、以降の経路は従来と同じ
    PixelPerfectPlan ppPlan;
    if (!cameraOverride) {
        ppPlan = PlanPixelPerfect(world, screenTarget.width, screenTarget.height);
        lastPixelPerfectValid = ppPlan.active;
        lastPixelPerfect = ppPlan.result;
    }
    FrameTarget lowTarget;
    bool drawLowRes = false;
    if (ppPlan.active && ppPlan.result.upscale) {
        pixelPerfectRt_.Resize(device, ppPlan.result.rtW, ppPlan.result.rtH);
        if (pixelPerfectRt_.IsValid()) {
            lowTarget = screenTarget;
            lowTarget.rtv = pixelPerfectRt_.RTV();
            lowTarget.dsv = pixelPerfectRt_.DSV();
            lowTarget.width = pixelPerfectRt_.Width();
            lowTarget.height = pixelPerfectRt_.Height();
            lowTarget.depthSRV = pixelPerfectRt_.DepthSRV();
            lowTarget.dsvReadOnly = pixelPerfectRt_.DSVReadOnly();
            drawLowRes = true;
        } else {
            // RT を作れなければ解像度そのままで描く (見える範囲は同じ。orthoSize だけ画面基準にする)
            ppPlan.result.orthoSize = ppPlan.result.viewOrthoSize;
        }
    }
    const FrameTarget& target = drawLowRes ? lowTarget : screenTarget;
    if (ppPlan.active && !pixelPerfectLogged_) {
        pixelPerfectLogged_ = true;
        if (drawLowRes) {
            MYE_LOG_INFO("RenderSystem: PixelPerfectCamera2D zoom %d, RT %dx%d -> %dx%d (TAA / FXAA are turned off)",
                         ppPlan.result.zoom, ppPlan.result.rtW, ppPlan.result.rtH, screenTarget.width,
                         screenTarget.height);
        } else {
            MYE_LOG_INFO("RenderSystem: PixelPerfectCamera2D zoom %d, drawn at %dx%d without a low-res RT (TAA / FXAA are turned off)",
                         ppPlan.result.zoom, screenTarget.width, screenTarget.height);
        }
    }

    // ★後の段は前の段が view / lights に書いた値を読む
    FrameContext f;
    if (ppPlan.active) {
        f.orthoSizeOverride = ppPlan.result.orthoSize;
        f.pixelPerfect = true; // 低解像度 RT を使わない (zoom 1 / upscaleRT = false) ときもドットを保つ
        f.pixelSnapUnit = ppPlan.snapping ? ppPlan.snapUnit : 0.0f;
    }
    sprite2dRenderer_.SetPixelSnapUnit(f.pixelSnapUnit);
    sprite2dRenderer_.SetFrustumCulling(enableFrustumCulling);
    BeginView(device, shaders, target, f);
    ResolveCamera(world, cameraOverride, f);
    DecideTaaAndFroxel(world, path, target, cameraOverride, f);
    CollectLights(world, f);
    // 草 (サーフェスシェーダの VS) が影エントリでも同じ場を読むので、カスケード影より前に束縛する
    UpdateDisturbanceField(world, device, resources, f);
    UpdateDisturbanceVolumes(world, device, resources, f); // フロクセルの注入 (UpdateFroxel) が読む
    CollectDrawables(world, resources, target, f);
    RenderCascadeShadows(world, device, shaders, resources, f);
    AllocateShadowAtlas(world, device, shaders, resources, f);
    PrepareEnvironment(world, device, shaders, resources, target, cameraOverride, f);
    UpdateRtScene(device, shaders, resources, target, f);
    UpdateFroxel(device, shaders, target, f);
    UpdateAcousticVolume(device, target, f);

    queue_.Sort();
    path.Render(device, f.view, queue_, f.lights, resources, shaders);
    // M55d: 画面速度 (GBuffer RT4) はパスが所有する — 描いた後でないと SRV が無い。
    // TAA (ポスプロ) がこの後で読む。Forward は null = TAA は自然に不成立になる
    f.view.velocitySRV = path.VelocitySRV();
    // M56c: HZB の GPU 時間を写す。Forward は既定の 0 を返すので、パスを切り替えると
    // 行が消えるのではなく 0.000 ms になる (「計っていない」と「速い」の区別は
    // ProfilerWindow が hzbDebugMip で行を出し分けることで付けている)
    hzbGpuMs_ = path.HzbGpuMs();
    ssrGpuMs_ = path.SsrGpuMs(); // M56d (同上)
    gbufferGpuMs_ = path.GbufferGpuMs();             // M90a (同上)
    forwardOpaqueGpuMs_ = path.ForwardOpaqueGpuMs(); // M90a (同上)
    gbufferCpuMs_ = path.GbufferCpuMs();             // M90f (同上)
    occlusionGpuMs_ = path.OcclusionGpuMs();         // (同上)
    f.occlusionBoxes = (hzbDebugMip != 0 && hzbDebugMax) ? &path.OcclusionDebugBoxes() : nullptr;

    DrawParticlesAndDebug(world, device, shaders, resources, target, cameraOverride, particles, vfx, f);
    if (groundFogBound_) {
        disturbancePass_.UnbindGroundFog(device); // 地面の霧の束縛は他のビューの描画へ残さない
        groundFogBound_ = false;
    }
    ResolvePost(world, device, shaders, resources, path, target, cameraOverride, f);
    if (drawLowRes) {
        // 低解像度 RT を整数倍で画面へ。深度は呼び出し側の後段 (ギズモ等) のために 1 へ戻しておく
        if (!pixelPerfectBlit_.IsReady()) {
            pixelPerfectBlit_.Init(device, shaders);
        }
        if (screenTarget.dsv != nullptr) {
            device.Context()->ClearDepthStencilView(screenTarget.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
        pixelPerfectBlit_.Blit(device, shaders, pixelPerfectRt_.SRV(), lowTarget.width, lowTarget.height,
                               screenTarget.rtv, screenTarget.width, screenTarget.height, ppPlan.result.zoom);
        // ワールド追従 UI は画面に見えている範囲 (RT の端を切った後) の射影で置く
        if (f.cameraFound) {
            const float viewHalfH = ppPlan.result.viewOrthoSize;
            const float screenAspect = (screenTarget.height > 0)
                ? static_cast<float>(screenTarget.width) / static_cast<float>(screenTarget.height) : 1.0f;
            const XMMATRIX screenProj = XMMatrixOrthographicLH(2.0f * viewHalfH * screenAspect, 2.0f * viewHalfH,
                                                               f.view.nearZ, f.view.farZ);
            XMStoreFloat4x4(&lastViewProjNoJitter, XMLoadFloat4x4(&f.view.view) * screenProj);
        }
    }
    frameTimer_.End(device);
    return f.cameraFound;
}

// 描画先から RenderView を初期化し、HDR 中間 (M16) を確保する
void RenderSystem::BeginView(GraphicsDevice& device, ShaderManager& shaders, const FrameTarget& target,
                             FrameContext& f)
{
    RenderView& view = f.view;
    PostProcess::Target*& hdr = f.hdr;
    view.dsv = target.dsv;
    view.width = target.width;
    view.height = target.height;
    memcpy(view.clearColor, target.clearColor, sizeof(view.clearColor));
    view.depthSRV = target.depthSRV;       // M42a: null なら深度読み系効果は自然無効
    view.dsvReadOnly = target.dsvReadOnly;

    // シーン + パーティクルを HDR 中間 (R16F, color のみ) へ描き、最後にトーンマップ解決で
    // target.rtv へ書く。depth は target.dsv を共有するため、解決後にエディタが重ねる
    // ギズモ/線 (rt_.DSV() 利用) の深度テストはそのまま成立する。
    if (!postFx_.IsReady()) {
        postFx_.Init(device, shaders);
    }
    if (enablePostFx && postFx_.IsReady()) {
        hdr = postFx_.Acquire(device, target.width, target.height);
    }
    view.rtv = (hdr != nullptr) ? hdr->scene.RTV() : target.rtv; // 確保失敗時は従来直描きにフォールバック
    if (hdr != nullptr) {
        // M38a: 背景クリア色も authored 色 → リニアへ (トーンマップ + OETF と対)。
        // 直描き (postFx 無効) 経路は OETF が掛からないので変換しない
        for (int i = 0; i < 3; ++i) {
            view.clearColor[i] = SrgbToLinear(view.clearColor[i]);
        }
    }

    f.aspectRatio = (target.height > 0)
        ? static_cast<float>(target.width) / static_cast<float>(target.height) : 1.0f;
}

// カメラ: CameraOverride (エディタ視界) か、シーンの isPrimary カメラ (無ければ最初のカメラ)
void RenderSystem::ResolveCamera(World& world, const CameraOverride* cameraOverride, FrameContext& f)
{
    RenderView& view = f.view;
    const float aspectRatio = f.aspectRatio;
    bool& cameraFound = f.cameraFound;
    EntityID& camEntity = f.camEntity;
    if (cameraOverride) {
        view.view = cameraOverride->view;
        if (cameraOverride->hasProj) {
            // M55b: 呼び出し側 (SceneView) が既に組んである行列をそのまま使う。
            // ここで組み直すと Ortho トグルが描画へ届かず、ギズモ/ピッキングの行列
            // (SceneViewWindow::lastProj_) と食い違ったままになる
            view.proj = cameraOverride->proj;
        } else {
            const XMMATRIX p = XMMatrixPerspectiveFovLH(
                XMConvertToRadians(cameraOverride->fovYDeg), aspectRatio,
                cameraOverride->nearZ, cameraOverride->farZ);
            XMStoreFloat4x4(&view.proj, p);
        }
        view.cameraPos = cameraOverride->position;
        view.debugViewMode = cameraOverride->debugViewMode; // M40b (SceneView のみ非 0)
        view.nearZ = cameraOverride->nearZ; // M42a: 深度線形化用
        view.farZ = cameraOverride->farZ;
        cameraFound = true;
    } else {
        const ComponentTypeId req[] = { CameraComponent::sTypeId, WorldMatrixComponent::sTypeId };
        XMFLOAT4X4 camWorld = {};
        CameraComponent cam = {};
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(CameraComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* c = static_cast<const CameraComponent*>(arch.GetPtr(ci, row));
                if (!cameraFound || c->isPrimary) {
                    cam = *c;
                    camWorld = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                    camEntity = arch.EntityAt(row); // M29e: CameraPostFx 参照用
                    cameraFound = true;
                    if (c->isPrimary) {
                        return;
                    }
                }
            }
        });
        if (cameraFound) {
            // M36b: カメラも補間 (スクリプト駆動カメラの tick 刻みを消す)
            if (prevWorld && interpAlpha < 1.0f) {
                if (const XMFLOAT4X4* pw = prevWorld->Get(camEntity)) {
                    camWorld = LerpWorld(*pw, camWorld, interpAlpha);
                }
            }
            // M94c: 手触りのカメラの揺れ・キック (ADR-035)。補間の後・逆行列の前に、カメラ局所の
            // 回転 + 平行移動として掛ける。ここより後 (ジッタの一元化・lastViewProjNoJitter・TAA の
            // 再投影) は揺れたカメラを「本当に動いたカメラ」として一貫して扱う
            float fovYDeg = cam.fovYDeg;
            if (feel.cameraActive) {
                const XMMATRIX local = XMMatrixRotationRollPitchYaw(XMConvertToRadians(feel.camRotDeg[0]),
                                                                     XMConvertToRadians(feel.camRotDeg[1]),
                                                                     XMConvertToRadians(feel.camRotDeg[2]))
                    * XMMatrixTranslation(feel.camPos[0], feel.camPos[1], feel.camPos[2]);
                XMStoreFloat4x4(&camWorld, local * XMLoadFloat4x4(&camWorld));
                fovYDeg = std::clamp(fovYDeg + feel.fovDeg, kFeelMinFovDeg, kFeelMaxFovDeg);
            }
            // M94v: ピクセルスナップ中はカメラの XY を 1/PPU の格子へ丸める (スプライトの頂点と同じ格子)
            if (f.pixelSnapUnit > 0.0f) {
                camWorld._41 = std::round(camWorld._41 / f.pixelSnapUnit) * f.pixelSnapUnit;
                camWorld._42 = std::round(camWorld._42 / f.pixelSnapUnit) * f.pixelSnapUnit;
            }
            const XMMATRIX w = XMLoadFloat4x4(&camWorld);
            const XMMATRIX v = XMMatrixInverse(nullptr, w);
            // M94v: PixelPerfectCamera2D があれば orthoSize はそちらの有効値 (CameraComponent.orthoSize は無視)
            const float orthoSize = (f.orthoSizeOverride > 0.0f) ? f.orthoSizeOverride : cam.orthoSize;
            const XMMATRIX p = cam.orthographic
                ? XMMatrixOrthographicLH(2.0f * orthoSize * aspectRatio, 2.0f * orthoSize,
                                         cam.nearZ, cam.farZ)
                : XMMatrixPerspectiveFovLH(XMConvertToRadians(fovYDeg), aspectRatio,
                                           cam.nearZ, cam.farZ);
            XMStoreFloat4x4(&view.view, v);
            XMStoreFloat4x4(&view.proj, p);
            view.cameraPos = { camWorld._41, camWorld._42, camWorld._43 };
            view.nearZ = cam.nearZ; // M42a: 深度線形化用
            view.farZ = cam.farZ;
        } else {
            XMStoreFloat4x4(&view.view, XMMatrixIdentity());
            XMStoreFloat4x4(&view.proj, XMMatrixIdentity());
        }
    }
}

// ジッタ前の射影の保存 (M55b)、TAA の有効判定 (M55d)、フロクセルの有効判定 (M57c)、ジッタの適用
void RenderSystem::DecideTaaAndFroxel(World& world, IRenderPath& path, const FrameTarget& target,
                                      const CameraOverride* cameraOverride, FrameContext& f)
{
    RenderView& view = f.view;
    const bool cameraFound = f.cameraFound;
    const EntityID camEntity = f.camEntity;
    const PostProcess::Target* hdr = f.hdr;
    bool& froxelOn = f.froxelOn;
    FroxelSettings& effectiveFroxel = f.effectiveFroxel;
    // ---- M55b: カメラジッタの一元化 ----
    // 射影の組み立ては上の 2 経路 (CameraOverride / シーンカメラ) に分かれているが、
    // ジッタを載せるのは **ここ 1 箇所だけ**。projNoJitter が「ジッタ前」の正本で、
    // 再投影 (prevVP_ の保存 / モーションブラー / RT テンポラル)、シャドウのカスケード
    // フィット、視錐台カリング、太陽の画面位置はすべてそちらを読む
    // (混ぜると「カメラが毎フレーム半ピクセル動いた」ことになり履歴が毎回外れる)。
    // viewKey==0 (AssetPreview) は履歴も TAA も持たないので常に非ジッタ。
    view.projNoJitter = view.proj;
    // ワールド追従 UI 用: 補間済みカメラのジッタ無し view×proj を公開 (UIRenderer が
    // この Render の直後に読む)。ジッタ入りを渡すと UI が毎フレーム半ピクセル揺れる
    lastCamValid = cameraFound;
    if (cameraFound) {
        XMStoreFloat4x4(&lastViewProjNoJitter,
                        XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.projNoJitter));
    }
    view.viewKey = target.viewKey; // M55d: TAA の履歴スロット
    view.viewFrameIndex = (target.viewKey < 4) ? viewSerial_[target.viewKey] : 0u;
    // ---- M55d: TAA の有効判定 ----
    // ★ジッタと TAA は**必ず同じ条件**で on/off する。TAA 抜きでジッタだけ載せると
    //   画面が毎フレーム半ピクセル揺れるだけになるので、「velocity を書かないパス
    //   (Forward)」「HDR 配管なし」「AssetPreview」はジッタごと落とす。
    // 設定の出所はポスプロと同じ規則 (グローバル設定 → シーンカメラの CameraPostFx で上書き)
    // だが、判定はマージ (Resolve 直前) より前に要るのでここで先に引く
    {
        bool taaOn = postFxSettings.taaOn != 0;
        if (!cameraOverride && !camEntity.IsNull()) {
            if (const auto* pfx = world.GetComponent<CameraPostFxComponent>(camEntity)) {
                taaOn = pfx->taaOn;
            }
        }
        // M94v: PixelPerfectCamera2D がある間はジッタも TAA も止める (ドットが滲む)
        view.taaEnabled = (taaOn && cameraFound && hdr != nullptr && path.WritesVelocity()
                           && target.viewKey > 0 && target.viewKey < 4 && !f.pixelPerfect)
            ? 1 : 0;
    }
    // ---- M57c: フロクセルの有効判定とパラメータ ----
    // 設定の出所は TAA / ポスプロと同じ規則 (グローバル設定 → シーンカメラの
    // CameraPostFx があればそちらが勝つ)。**判定だけここで先に引く** — 実際の
    // ディスパッチはシャドウアトラスと環境の収集が終わったあと (path.Render の直前)。
    // ★SceneView のエディタカメラ (CameraOverride) には CameraPostFx が効かない
    //   規約なので、そちらは --froxel / Rendering メニューのグローバル設定だけで動く
    froxelOn = enableFroxel;
    effectiveFroxel = froxelSettings;
    if (!cameraOverride && !camEntity.IsNull()) {
        if (const auto* pfx = world.GetComponent<CameraPostFxComponent>(camEntity)) {
            froxelOn = pfx->froxelOn;
            effectiveFroxel.density = pfx->froxelDensity;
            effectiveFroxel.anisotropy = pfx->froxelAnisotropy;
        }
    }
    froxelOn = froxelOn && cameraFound;
    if (view.taaEnabled != 0 && jitterAmplitude > 0.0f) {
        float px = 0.0f;
        float py = 0.0f;
        camerajitter::Sample(view.viewFrameIndex, px, py);
        px *= jitterAmplitude;
        py *= jitterAmplitude;
        float ndcX = 0.0f;
        float ndcY = 0.0f;
        camerajitter::PixelsToNdc(px, py, view.width, view.height, ndcX, ndcY);
        view.jitterPixels[0] = px;
        view.jitterPixels[1] = py;
        view.jitterNdc[0] = ndcX;
        view.jitterNdc[1] = ndcY;
        view.proj = camerajitter::ApplyToProj(view.proj, ndcX, ndcY);
    }
}

// 視錐台 (M16。メッシュとライトのカリング用。カメラがある時のみ。描画専用でハッシュ非対象) と、
// ライトの候補収集 → SelectLights (M54b)
void RenderSystem::CollectLights(World& world, FrameContext& f)
{
    RenderView& view = f.view;
    Frustum& frustum = f.frustum;
    SceneLightData& lights = f.lights;
    f.cullEnabled = f.cameraFound;
    f.frustumDrop = f.cameraFound && enableFrustumCulling;
    const bool cullEnabled = f.cullEnabled;
    if (cullEnabled) {
        XMFLOAT4X4 vp;
        // M55b: カリングは非ジッタ側。サブピクセルで可視判定が反転すると
        // 境界の物体がフレーム毎に出入りして TAA の履歴に穴が空く
        XMStoreFloat4x4(&vp, XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.projNoJitter));
        frustum = BuildFrustum(vp);
    }
    view.cullFrustum = f.frustumDrop ? &frustum : nullptr; // GPU パーティクル / VFX が描画だけを落とす判定に使う

    // ---- ライト (Directional/Point/Spot) ----
    // M54b: ここは候補を集めるだけで、カリング / 決定論ソート / 上限 kMaxLights の適用は
    // LightSelection.cpp の純関数が行う (M54c のシャドウアトラスが「影を投げるライトの列」の
    // frame 間安定性を要求するため、順序を決める場所を 1 箇所に閉じた)。
    {
        const ComponentTypeId req[] = { LightComponent::sTypeId, WorldMatrixComponent::sTypeId };
        bool ambientSet = false;
        std::vector<LightCandidate> cands;
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int li = arch.FindTypeIndex(LightComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue; // 無効化されたライトは寄与しない
                }
                const auto* l = static_cast<const LightComponent*>(arch.GetPtr(li, row));
                const auto* w = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                LightCandidate& c = cands.emplace_back();
                c.sortKey = e.index; // 決定論キー (アーキタイプの並び順に依存しない)
                c.castShadow = l->castShadow;
                GpuLight& g = c.light;
                // ワールド行列の第 3 行 = ローカル +Z の向き、第 4 行 = 位置
                XMVECTOR dir = XMVector3Normalize(
                    XMVectorSet(w->value._31, w->value._32, w->value._33, 0));
                XMStoreFloat3(&g.direction, dir);
                g.position = { w->value._41, w->value._42, w->value._43 };
                g.color = SrgbToLinear(l->color); // M38a: authored 色をリニアへ
                g.intensity = l->intensity;
                g.type = l->type;
                g.range = l->range;
                g.cosInner = std::cos(XMConvertToRadians(l->spotInnerDeg));
                g.cosOuter = std::cos(XMConvertToRadians(l->spotOuterDeg));
                if (!ambientSet) {
                    // アンビエントは最初のライトの値を全体に使う (M38a: リニアへ)。
                    // ★選別後の先頭ではなく**走査順の先頭** — 選別の順に替えると ambient の出どころが変わり絵が動く
                    lights.ambient = SrgbToLinear(l->ambient);
                    ambientSet = true;
                }
            }
        });
        lightSelection = SelectLights(cands.data(), static_cast<int>(cands.size()),
                                      cullEnabled ? &frustum : nullptr);
        lights.count = lightSelection.count;
        for (int i = 0; i < lightSelection.count; ++i) {
            lights.lights[i] = lightSelection.lights[i].light;
        }
        // カリングで意図せずライトが消えていないかを目視するための 1 行。
        // 毎フレーム出すとログが埋まるので、初めて見る組み合わせのときだけ出す
        {
            auto pack = [](int v) { return static_cast<uint64_t>((std::min)((std::max)(v, 0), 0xFFFF)); };
            const uint64_t key = pack(lightSelection.count) | (pack(lightSelection.culled) << 16)
                | (pack(lightSelection.overflow) << 32) | (pack(lightSelection.shadowCount) << 48);
            bool seen = false;
            for (int i = 0; i < lightLogSeenCount_; ++i) {
                seen = seen || lightLogSeen_[i] == key;
            }
            if (!seen && lightLogSeenCount_ < static_cast<int>(std::size(lightLogSeen_))) {
                lightLogSeen_[lightLogSeenCount_++] = key;
                MYE_LOG_INFO("Lights: %d selected (culled %d, dropped %d, shadow casters %d)",
                             lightSelection.count, lightSelection.culled, lightSelection.overflow,
                             lightSelection.shadowCount);
            }
        }
    }
}

// パレット評価 1 件。評価はジョブ間で互いに素なエントリ (palette) にだけ書く
struct RenderSystem::PaletteJob {
    EntityID entity;
    const SkinnedMeshComponent* sm = nullptr;
    const SkinnedModel* model = nullptr;
    SkinPaletteCache::Entry* entry = nullptr;
    float alpha = 1.0f;
    size_t rawBoneCount = 0; // kMaxBones で切る前の骨の数 (超過の WARN 用)
};

// 収集: 前フレーム world のストアを進め (M55c)、地形 (M58c) / デカール (M56a) / メッシュを集めてキューへ。
// メッシュはカリング (並列) → スキンのパレット → 不透明 / 半透明の振り分けと、影のフィット用 AABB の集約
void RenderSystem::CollectDrawables(World& world, RenderResources& resources, const FrameTarget& target,
                                    FrameContext& f)
{
    const auto collectStart = std::chrono::steady_clock::now();
    RenderView& view = f.view;
    const Frustum& frustum = f.frustum;
    const bool cullEnabled = f.cullEnabled;
    const bool frustumDrop = f.frustumDrop;
    XMFLOAT3& sceneMin = f.sceneMin;
    XMFLOAT3& sceneMax = f.sceneMax;
    bool& hasScene = f.hasScene;
    // ---- M55c: 「前フレームに実際に描いた world 行列」のストアを今フレームへ進める ----
    // viewKey==0 (AssetPreview) は履歴を持たない = velocity は常に 0 に落ちる。
    // viewSerial_ はこの Render の末尾で +1 されるので、ここでの値が「今フレームの通番」
    const uint32_t prevRenderKey = (target.viewKey > 0 && target.viewKey < 4) ? target.viewKey : 0u;
    PrevRenderWorldStore* prevRender = (prevRenderKey != 0) ? &prevRender_[prevRenderKey] : nullptr;
    if (prevRender != nullptr) {
        prevRender->Begin(viewSerial_[prevRenderKey], target.width, target.height);
    }

    // ---- 収集 ----
    queue_.Clear();
    // sim の tick が戻った = 巻き戻し・Play の開始や停止。前の履歴 (LOD の段・パレット) は別の時間のもの
    if (simTick < lastSimTick_) {
        ResetRenderHistory();
    }
    lastSimTick_ = simTick;
    skinPaletteCache_.BeginFrame();
    // 影のキャスター候補 (カメラの視錐台で落とす前の全不透明)。CSM (平行光) とシャドウアトラス (局所光) が読む。
    // どちらの影も描かないフレームは集めない
    shadowCasters_.clear();
    offscreenCasters_.clear();
    offscreenScreenSize_.clear();
    const bool collectShadowCasters = enableShadows
        && (FindDirectionalLight(f.lights) >= 0 || (enableLocalShadows && HasLocalShadowLight(lightSelection)));
    // スキンのパレット: 直列段で必要なエントリを確保して評価を積み、並列段で評価し、キューの項目へ繋ぐ
    std::vector<PaletteJob> paletteJobs;
    struct PaletteBind {
        bool transparent;
        size_t index;
        SkinPaletteCache::Entry* entry;
    };
    std::vector<PaletteBind> paletteBinds;
    int paletteReused = 0; // キャッシュを使えたパレットの数 (統計)
    const XMMATRIX v = XMLoadFloat4x4(&view.view);
    // RT のどれかのレーン (か RT デバッグ表示) が on か。off のフレームは適用範囲を 1 回も
    // 判定しない (祖先を辿らない) — 既定の経路のコストと絵を RT 導入前のまま保つ
    const bool rtAnyLane =
        rtDebugMode != rtdebug::kOff || enableRtGi || enableRtShadow || enableRtRefl; // M46b/f/g/h
    // ---- 地形 (M58c): メッシュとは別レーンで収集する ----
    // TerrainComponent は kComponentNoHash = 描画専用。ここで作るのは
    // 「可視チャンクの描画指示」だけで sim には 1 バイトも触れない。
    // ★カメラが無いフレーム (cullEnabled==false) は収集しない — 視錐台がゼロ行列のままで
    //   CullChunks に渡しても意味のある結果にならない (メッシュ側がカリングを
    //   丸ごと飛ばしているのと同じ扱い)。
    // ★assetsRoot が空 = AssetPreviewCache の専用 RenderSystem。サムネイルに地形を
    //   混ぜないための元栓はここ 1 箇所 (「配線の 2 箇所目」で毎回漏れるところ)
    terrainList_.items.clear();
    if (cullEnabled && !assetsRoot.empty()) {
        terrainSystem_.Collect(world, resources.meshes, resources.textures, assetsRoot,
                               frustum, view.view, terrainScratch_);
        terrainList_.items.reserve(terrainScratch_.size());
        for (const TerrainDrawItem& t : terrainScratch_) {
            TerrainRenderItem it;
            it.mesh = t.mesh;
            it.world = t.world;
            it.viewZ = t.viewZ;
            it.surface = t.surface; // M58d: スプラット + 4 レイヤの bind
            // 地形も RT を受ける面の判定はメッシュと同じ規則 (ResolveRtScope)。
            // RT が全レーン off なら 1 固定 = G-Buffer が RT 適用範囲の導入前とビット一致
            it.rtReceiver = (!rtAnyLane || ResolveRtScope(world, t.entity, rtTagRules).receiver.on)
                ? 1.0f
                : 0.0f;
            terrainList_.items.push_back(it);
        }
        // 近い順 (early-z が効く順)。比較規則の正本は TerrainPass.h の
        // TerrainDrawOrderLess (規則 7 のタイブレークつき。TerrainSelfTest が検査する)
        std::sort(terrainList_.items.begin(), terrainList_.items.end(),
                  TerrainDrawOrderLess);
    }
    view.terrain = &terrainList_;

    // ---- デカール (M56a): メッシュとは別レーンで収集する (地形と同じ流儀) ----
    // DecalComponent は kComponentNoHash = 描画専用。ここで作るのは「投影ボックス 1 個
    // ぶんの描画指示」だけで sim には 1 バイトも触れない。
    // 投影ボックス (単位立方体 [-0.5,0.5]^3 を world で変換した箱) が視錐台の外なら落とす。
    // 箱の外の受け面は PS の OBB 判定が捨てるので、落としても絵は変わらない。
    // ★AssetPreviewCache の専用 RenderSystem 用の元栓は要らない — プレビュー世界には
    //   デカールが 1 個も居ないので、下の収集が空リストを作り view.decals が
    //   「空 = 何もしない」に落ちる (地形は共有ワールドを見るので元栓が要った)
    decalList_.items.clear();
    int decalsCulled = 0;
    {
        const ComponentTypeId req[] = { DecalComponent::sTypeId,
                                        WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int di = arch.FindTypeIndex(DecalComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue; // 無効化されたデカールは貼られない
                }
                const auto* d = static_cast<const DecalComponent*>(arch.GetPtr(di, row));
                if (d->color.w <= 0.0f) {
                    continue; // 完全に透明 = 描いても 1 画素も変わらない
                }
                const auto* w =
                    static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                DecalRenderItem it;
                if (!FillDecalTransform(w->value, it)) {
                    continue; // スケール 0 等で逆行列が作れない
                }
                if (frustumDrop
                    && !AabbInFrustum(frustum, w->value, { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f })) {
                    ++decalsCulled;
                    continue;
                }
                it.color = SrgbToLinear(XMFLOAT3(d->color.x, d->color.y, d->color.z));
                it.opacity = d->color.w;
                it.angleFadeCos = DecalAngleFadeCos(d->angleFadeDeg);
                it.uvScale[0] = d->uvScale.x;
                it.uvScale[1] = d->uvScale.y;
                it.uvOffset[0] = d->uvOffset.x;
                it.uvOffset[1] = d->uvOffset.y;
                it.texture = d->texture;
                it.sortOrder = d->sortOrder;
                it.sortKey = e.index; // 決定論キー (アーキタイプの並び順に依存しない)
                // M56b: 強度は [0,1] に丸めてから渡す。**そのままハードウェアの
                // ブレンド係数になる**ので、1 を超えると dst 側の係数 (1-src) が負に
                // なって法線が反転する (Inspector のスライダは止めるがスクリプト経由は素通り)
                it.normalTexture = d->normalTex;
                it.normalStrength = DecalStrength01(d->normalStrength);
                it.roughness = DecalStrength01(d->roughness);
                it.roughnessStrength = DecalStrength01(d->roughnessStrength);
                decalList_.items.push_back(it);
            }
        });
        // 比較規則の正本は RenderTypes.h の DecalDrawOrderLess
        // (規則 7 のタイブレークつき。DecalSelfTest が検査する)
        std::sort(decalList_.items.begin(), decalList_.items.end(), DecalDrawOrderLess);
    }
    if (decalsCulled != 0) {
        prof::RenderStats delta;
        delta.culledNonMesh = decalsCulled;
        prof::AddRenderStats(delta);
    }
    view.decals = &decalList_;

    // ---- 水面 (M69): メッシュとは別レーンで収集 (地形/デカールと同じ流儀) ----
    // シーン内で entity.index 最小かつ active/enabled な WaterWaveComponent を解決 (PhysicsSystem と同一規則)
    view.water = nullptr;
    waterData_.active = false;
    waterData_.useSurfaceRoute = false;
    {
        const ComponentTypeId req[] = { WaterWaveComponent::sTypeId };
        uint32_t bestIndex = UINT32_MAX;
        const WaterWaveComponent* bestWave = nullptr;
        EntityID bestEntity = kNullEntity;

        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int wi = arch.FindTypeIndex(WaterWaveComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (e.index >= bestIndex) {
                    continue;
                }
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                const auto* wave = static_cast<const WaterWaveComponent*>(arch.GetPtr(wi, row));
                if (!wave->enabled) {
                    continue;
                }
                bestIndex = e.index;
                bestWave = wave;
                bestEntity = e;
            }
        });

        if (bestWave != nullptr) {
            waterData_.active = true;
            // 水面の時刻 = 浮力と同じ tick の時計 (WaterWave.timeTicks)。描画フレーム数では進めない
            // (60 FPS 以外で見た目の波と浮き物の水位がずれ、SceneView と GameView でも別の位相になった)。
            // timeTicks は「直近の tick の状態」の時刻なので、メッシュと同じく前 tick → 直近 tick を
            // interpAlpha で補間する (Edit 中 / 決定的撮影は alpha=1 = 直近 tick そのもの)。
            // Edit 中はエディタの水面プレビューの経過秒だけを足す (waterPreviewSeconds、描画専用)
            const float ticks = static_cast<float>(bestWave->timeTicks - 1) + interpAlpha;
            const float t = (ticks * (1.0f / 60.0f) + waterPreviewSeconds) * bestWave->timeScale;
            // 速度エントリの「前」= このビューが前フレームに描いた時刻 (履歴が無いビューは今と同じ = 速度 0)
            const uint32_t waterVk = (target.viewKey > 0 && target.viewKey < 4) ? target.viewKey : 0u;
            const float tPrev = (waterVk != 0 && prevWaterTimeValid_[waterVk]) ? prevWaterTime_[waterVk] : t;
            if (waterVk != 0) {
                prevWaterTime_[waterVk] = t;
                prevWaterTimeValid_[waterVk] = true;
            }

            // ワールド行列の取得 (WorldMatrixComponent があればそれを使う)
            if (const auto* wm = world.GetComponent<WorldMatrixComponent>(bestEntity)) {
                waterData_.world = wm->value;
            } else if (const auto* lt = world.GetComponent<LocalTransform>(bestEntity)) {
                XMVECTOR pos = XMLoadFloat3(&lt->position);
                XMVECTOR rot = XMLoadFloat4(&lt->rotation);
                XMVECTOR scl = XMLoadFloat3(&lt->scale);
                XMMATRIX m = XMMatrixAffineTransformation(scl, XMVectorZero(), rot, pos);
                XMStoreFloat4x4(&waterData_.world, m);
            } else {
                waterData_.world = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            }

            // WaterMaterialCB への詰め込み (authored 色は sRGB -> Linear 変換)
            waterData_.material.deepColor = SrgbToLinear(bestWave->deepColor);
            waterData_.material.shallowColor = SrgbToLinear(bestWave->shallowColor);
            waterData_.material.waveParams0 = { bestWave->wave0Amplitude, bestWave->wave0Wavelength, bestWave->wave0Speed, bestWave->wave0DirAngle };
            waterData_.material.waveParams1 = { bestWave->wave1Amplitude, bestWave->wave1Wavelength, bestWave->wave1Speed, bestWave->wave1DirAngle };
            waterData_.material.waveParams2 = { bestWave->wave2Amplitude, bestWave->wave2Wavelength, bestWave->wave2Speed, bestWave->wave2DirAngle };
            waterData_.material.waveParams3 = { bestWave->wave3Amplitude, bestWave->wave3Wavelength, bestWave->wave3Speed, bestWave->wave3DirAngle };
            waterData_.material.waveSteepness = { bestWave->wave0Steepness, bestWave->wave1Steepness, bestWave->wave2Steepness, bestWave->wave3Steepness };
            waterData_.material.waterSettings = { bestWave->baseHeight, bestWave->overallScale, t, bestWave->foamStrength };
            waterData_.material.waterOptics = { bestWave->fresnelPower, bestWave->smoothness, 0.0f, 0.0f };
            // 浮力 (PhysicsSystem) とサーフェス水面 (下の surfaceCb) と同じ本数で波を打ち切る
            waterData_.material.waveCount = { bestWave->ClampedWaveCount(), 0, 0, 0 };

            // ---- M79 sub-05: MyEngineWater CB (予約 CB。全サーフェスシェーダへ名前で張る) ----
            // 水面自身の描画経路 (surfaceMaterial の有無) に関係なく埋める — 他メッシュの
            // サーフェスシェーダ (例: 船体) が水面レベルを読めるようにするため (spec §4.1)
            waterData_.curWaterTime = t;
            waterData_.prevWaterTime = tPrev;
            waterData_.surfaceCb.enabled = 1;
            waterData_.surfaceCb.baseHeight = bestWave->baseHeight;
            waterData_.surfaceCb.overallScale = bestWave->overallScale;
            waterData_.surfaceCb.waveCount = bestWave->ClampedWaveCount(); // 浮力と同じ規則
            bestWave->ExtractWaves(waterData_.surfaceCb.waves, WaterWaveComponent::kMaxWaves);
            waterData_.surfaceCb.deepColor = waterData_.material.deepColor;
            waterData_.surfaceCb.shallowColor = waterData_.material.shallowColor;

            view.water = &waterData_;

            // ---- M79 sub-05: surfaceMaterial が解決できれば、水面を 1 個の RenderItem として
            //      通常のメッシュ経路 (Forward 不透明/透明、Deferred サーフェス段/透明段、CSM 影) に
            //      乗せる。WaterPass::Render はこのフレーム何も描かない (useSurfaceRoute)。
            //      未設定 / GUID が解決できない場合は従来どおり WaterPass が描く ----
            if (!bestWave->surfaceMaterial.IsNull()) {
                const Material* smat = resources.materials.Get(bestWave->surfaceMaterial);
                if (smat != nullptr) {
                    waterData_.useSurfaceRoute = true;
                    RenderItem item;
                    item.mesh = resources.meshes.WaterPlane();
                    item.material = bestWave->surfaceMaterial;
                    item.entity = bestEntity;
                    item.world = waterData_.world;
                    // 水面プレートは動かない前提 (sub-05.md) — velocity はシェーダの gWaterTime
                    // 変位 (前後 2 回評価の差) だけに由来させる
                    item.prevWorld = waterData_.world;
                    const XMVECTOR posWS = XMVectorSet(waterData_.world._41, waterData_.world._42,
                                                       waterData_.world._43, 1);
                    item.viewZ = XMVectorGetZ(XMVector3TransformCoord(posWS, v));
                    // ★sceneMin/sceneMax (CSM のカスケードフィットに使う AABB) には加えない —
                    //   巨大な水面プレートを混ぜるとカスケードが不必要に広がり、陸上の影の
                    //   解像度が落ちる (従来の WaterPass も一貫してシャドウフィットの対象外)
                    if (smat->transparent != 0) {
                        queue_.transparent.push_back(item);
                    } else {
                        if (collectShadowCasters) {
                            ShadowCaster sc; // 箱を持たない = 全カスケードへ (従来どおり常に影を落とす)
                            sc.queueIndex = static_cast<int32_t>(queue_.opaque.size());
                            shadowCasters_.push_back(sc);
                        }
                        queue_.opaque.push_back(item);
                    }
                }
            }
        }
    }

    int culledCount = 0;
    int culledDistanceCount = 0;
    // 不透明キャスターの world AABB を集約 → シャドウ範囲のフィットに使う (M17)
    sceneMin = { FLT_MAX, FLT_MAX, FLT_MAX };
    sceneMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    hasScene = false;

    // ---- ステージ 1 (直列): 候補を収集 (順序 = ForEachArchetype/row = 決定的) ----
    const auto stage1Start = std::chrono::steady_clock::now();
    std::vector<CullCand> cullCands;
    const bool interp = prevWorld != nullptr && interpAlpha < 1.0f; // M36b
    // M80f: root proxy の描画規則。破壊物の無いシーンに追加コストが出ないよう、
    // まず Destructible の有無だけを 1 回スキャンする (存在ゲート。以後は車輪と同じ
    // 「アーキタイプごとに型 index を 1 回引く」経路に落ちる)
    bool anyDestructibles = false;
    {
        const ComponentTypeId destructibleReq[] = { DestructibleComponent::sTypeId };
        world.ForEachArchetype(destructibleReq,
                               [&](Archetype& a) { anyDestructibles = anyDestructibles || a.Count() != 0; });
    }
    // 距離カリングを掛けるビューか。タグ別の距離が 1 つも無ければタグの列は引かない
    const bool distanceOn = cullEnabled && enableDistanceCulling
        && distancecull::AppliesToView(target.viewKey, cullSceneView);
    const bool anyTagDistance = distanceOn
        && std::any_of(std::begin(cullTagDistance), std::end(cullTagDistance), [](float d) { return d > 0.0f; });
    // 祖先を辿るのは、RenderCulling がワールドに 1 つでもあるか、タグ別の距離が 1 つでもあるときだけ (存在の門)
    bool anyRenderCulling = false;
    if (distanceOn) {
        const ComponentTypeId rcReq[] = { RenderCullingComponent::sTypeId };
        world.ForEachArchetype(rcReq,
                               [&](Archetype& a) { anyRenderCulling = anyRenderCulling || a.Count() != 0; });
    }
    const bool resolveDistance = distanceOn && (anyRenderCulling || anyTagDistance);
    const ComponentTypeId req[] = { MeshRendererComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int mi = arch.FindTypeIndex(MeshRendererComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        // M60i: 車輪の見た目回転。**アーキタイプごとに 1 回引くだけ**なので、
        // 車輪を持たないシーンは 1 命令も余計に走らない (存在ゲートと同じ効き)
        const int whi = arch.FindTypeIndex(WheelComponent::sTypeId);
        // M80f: root proxy。dsi/fpi はそのエンティティ自身が Destructible/FracturePiece を
        // 持つ速い経路。`_cap` はどちらも持たない直子なので hi 経由で親 (Frag<i>) の
        // FracturePiece を見て同じ規則に従わせる
        const int dsi = anyDestructibles ? arch.FindTypeIndex(DestructibleComponent::sTypeId) : -1;
        const int fpi = anyDestructibles ? arch.FindTypeIndex(FracturePieceComponent::sTypeId) : -1;
        const int hi = anyDestructibles ? arch.FindTypeIndex(HierarchyComponent::sTypeId) : -1;
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue; // 無効エンティティは描画しない (M10)
            }
            if (anyDestructibles) {
                if (dsi >= 0) {
                    // 割れたら元メッシュは描かない (破片側が描く)
                    if (static_cast<const DestructibleComponent*>(arch.GetPtr(dsi, row))->broken) {
                        continue;
                    }
                } else if (fpi >= 0) {
                    // 破片 (Frag<i>) は割れる前は描かない。可視規則は FractureSystem と共有
                    // (ShouldHideUnbrokenFracturePiece)
                    const auto* fp = static_cast<const FracturePieceComponent*>(arch.GetPtr(fpi, row));
                    const auto* d = world.GetComponent<DestructibleComponent>(fp->root);
                    if (ShouldHideUnbrokenFracturePiece(d)) {
                        continue;
                    }
                } else if (hi >= 0) {
                    const auto* h = static_cast<const HierarchyComponent*>(arch.GetPtr(hi, row));
                    if (const auto* pfp = world.GetComponent<FracturePieceComponent>(h->parent)) {
                        const auto* d = world.GetComponent<DestructibleComponent>(pfp->root);
                        if (ShouldHideUnbrokenFracturePiece(d)) {
                            continue;
                        }
                    }
                }
            }
            const auto* mr = static_cast<const MeshRendererComponent*>(arch.GetPtr(mi, row));
            if (mr->mesh.IsNull() || mr->material.IsNull()) {
                continue;
            }
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            XMFLOAT4X4 worldMat = wm->value;
            if (interp) {
                // M36b: 前 tick との補間 (新規 spawn は prev 無し → 現在値)
                if (const XMFLOAT4X4* pw = prevWorld->Get(e)) {
                    worldMat = LerpWorld(*pw, wm->value, interpAlpha);
                }
            }
            if (whi >= 0) {
                // 補間**後**に掛ける。補間の対象は sim が書いたワールド行列だけで、
                // 出力フィールドから作る見た目はその上に載る
                worldMat = ApplyWheelVisual(
                    worldMat, *static_cast<const WheelComponent*>(arch.GetPtr(whi, row)));
            }
            // M79 sub-06: 余白はここ (直列ステージ) で MaterialLibrary の横テーブルから解決する。
            // 非サーフェスマテリアルは 0 を返すので既存の判定と 1 ビットも変わらない
            const Mesh* meshPtr = resources.meshes.Get(mr->mesh);
            const auto* skinComp = world.GetComponent<SkinnedMeshComponent>(e);
            CullCand cand{ e, mr->mesh, mr->material, worldMat, meshPtr, 0.0f, 1,
                           skinComp != nullptr ? uint8_t{ 1 } : uint8_t{ 0 },
                           resources.materials.GetSurfaceBoundsPadding(mr->material) };
            if (meshPtr != nullptr) {
                cand.boundsMin = meshPtr->aabbMin;
                cand.boundsMax = meshPtr->aabbMax;
            }
            if (skinComp != nullptr) {
                // スキンはバインドポーズの AABB が姿勢を包まないので、全姿勢を包む箱で判定する。
                // ラグドールは骨がアニメと無関係に動く / 箱を求められないモデルは、従来どおり常に可視
                const auto* rag = world.GetComponent<RagdollComponent>(e);
                const SkinnedLocalAabb* box = (rag != nullptr && rag->active)
                    ? nullptr
                    : skinBounds_.Get(resources.skinnedModels, skinComp->model, resources.meshes, mr->mesh);
                if (box != nullptr) {
                    cand.boundsMin = box->min;
                    cand.boundsMax = box->max;
                } else {
                    cand.alwaysVisible = 1;
                }
            }
            if (resolveDistance) {
                cand.maxDrawDistance = distancecull::ResolveMaxDrawDistance(
                    world, e, anyTagDistance, cullTagDistance, cullDefaultMaxDrawDistance);
            } else if (distanceOn) {
                cand.maxDrawDistance = cullDefaultMaxDrawDistance > 0.0f ? cullDefaultMaxDrawDistance : 0.0f;
            }
            cullCands.push_back(cand);
        }
    });

    // ---- ステージ 2 (並列): 視錐台テスト + viewZ + LOD の段 (要素独立・純関数、M25) ----
    // 履歴は読むだけ (書き込みはステージ 3)。履歴の無いビュー (viewKey 0) は距離だけで決まる
    const auto stage2Start = std::chrono::steady_clock::now();
    const LodSelectParams lodParams{ lodBias, static_cast<int32_t>(lodForcedStage) };
    const LodHistory* lodHistoryRead = (prevRenderKey != 0) ? &lodHistory_[prevRenderKey] : nullptr;
    const bool orthographic = view.proj._44 != 0.0f; // 透視は _44 == 0
    jobs::System().ParallelRanges(cullCands.size(), kCullGrain, [&](size_t a, size_t b) {
        for (size_t i = a; i < b; ++i) {
            CullCand& c = cullCands[i];
            if (cullEnabled && c.meshPtr && c.meshPtr->lods.size() > 1) {
                // 画面に占める外接球の大きさ。画面外のキャスターも同じ式で段を決める (影はカメラ基準の段)
                XMFLOAT3 lo, hi;
                WorldAabb(c.world, c.meshPtr->aabbMin, c.meshPtr->aabbMax, lo, hi, 0.0f);
                const float screenSize = BoxScreenSize(lo, hi, view.cameraPos, view.proj._22, orthographic);
                const int32_t prevLod = (lodHistoryRead != nullptr) ? lodHistoryRead->Get(c.e) : -1;
                c.lod = static_cast<uint8_t>(SelectLod(c.meshPtr->lods, screenSize, prevLod, lodParams));
            }
            if (cullEnabled && c.skinned != 0 && c.meshPtr && c.alwaysVisible == 0) {
                // URO の大きさは全姿勢を包む箱から (姿勢が箱の外へ出ないので、遠いと判定した物は本当に小さい)。
                // 箱を求められない物は FLT_MAX のまま = 間引かない
                XMFLOAT3 lo, hi;
                WorldAabb(c.world, c.boundsMin, c.boundsMax, lo, hi, 0.0f);
                c.uroSize = BoxScreenSize(lo, hi, view.cameraPos, view.proj._22, orthographic);
            }
            // viewZ は画面外の影キャスターのソートキーにも使うので、落とす物にも先に求める
            const XMVECTOR posWS = XMVectorSet(c.world._41, c.world._42, c.world._43, 1);
            c.viewZ = XMVectorGetZ(XMVector3TransformCoord(posWS, v));
            // スキンの箱は全クリップの全姿勢を包む (別メッシュの手・指がアニメ中に消えない)
            if (cullEnabled && c.meshPtr
                && !RenderableInFrustum(frustum, c.world, c.boundsMin, c.boundsMax, c.alwaysVisible != 0,
                                        c.boundsPadding)) {
                c.visible = 0;
            }
            // 距離は視錐台の内外に関わらず求める (視錐台 OFF でも遠い物は落とす / 視錐台の外の遠い物は影も落とす)。
            // 箱を持てない物 (alwaysVisible) は落とさない
            if (c.maxDrawDistance > 0.0f && c.meshPtr && c.alwaysVisible == 0) {
                XMFLOAT3 lo, hi;
                WorldAabb(c.world, c.boundsMin, c.boundsMax, lo, hi, c.boundsPadding);
                c.tooFar = distancecull::ExceedsMaxDistance(view.cameraPos, lo, hi, c.maxDrawDistance) ? 1 : 0;
            }
        }
    });
    const auto stage2End = std::chrono::steady_clock::now();

    // 計測専用: 視錐台判定だけを同じ粒度で並列に回し直す (結果は捨てる。出力に使う値は 1 つも書かない)。
    // stage2 の直後なので候補はキャッシュに載っており、stage2 内の判定よりやや速く出る
    float frustumMs = 0.0f;
    if (cullTimingProbe && cullEnabled) {
        std::vector<uint8_t> probeVisible(cullCands.size(), 1);
        const auto probeStart = std::chrono::steady_clock::now();
        jobs::System().ParallelRanges(cullCands.size(), kCullGrain, [&](size_t a, size_t b) {
            for (size_t i = a; i < b; ++i) {
                const CullCand& c = cullCands[i];
                probeVisible[i] = (c.meshPtr
                                   && !RenderableInFrustum(frustum, c.world, c.boundsMin, c.boundsMax,
                                                           c.alwaysVisible != 0, c.boundsPadding))
                    ? 0
                    : 1;
            }
        });
        frustumMs = ElapsedMs(probeStart, std::chrono::steady_clock::now());
        // 最適化で判定が消えないよう、結果の総和をメンバへ残す
        cullProbeSink_ = std::accumulate(probeVisible.begin(), probeVisible.end(), size_t{ 0 });
    }

    // ---- ステージ 3 (直列): 可視候補をキュー化 (スキン/AABB/キューは順序依存で直列) ----
    const auto stage3Start = std::chrono::steady_clock::now();
    rtInstances_.clear();
    for (const CullCand& c : cullCands) {
        if (prevRenderKey != 0 && c.meshPtr != nullptr && c.meshPtr->lods.size() > 1) {
            lodHistory_[prevRenderKey].Set(c.e, c.lod); // 次フレームのヒステリシスの出所
        }
        // RT の適用範囲 (個別設定 → タグ規則 → 既定 OFF)。RT が off のフレームは引かない
        const RtScope rtScope = rtAnyLane ? ResolveRtScope(world, c.e, rtTagRules) : RtScope{};
        // M46b: レイトレ用の収集はフラスタムカリングしない (画面外の物体も
        // 反射や GI には効くため)。v1 制限: スキンメッシュ (CPU 頂点がバインドポーズ
        // のままなので姿勢が反映できない) と半透明は BVH に入れない。
        // 適用範囲が OFF の物も入れない (= 反射に映らず RT の影も落とさない)
        if (rtAnyLane && rtScope.inScene.on
            && world.GetComponent<SkinnedMeshComponent>(c.e) == nullptr) {
            const Material* rtMat = resources.materials.Get(c.material);
            if (!rtMat || rtMat->transparent == 0) {
                rtInstances_.push_back({ c.mesh, c.material, c.world });
            }
        }
        // 視錐台の外。スイッチが OFF のときは落とさず描くが、影の候補・CSM のフィット・前フレーム行列の記録は
        // ON と同じ規則にする (OFF が絵を変えないための不変量)
        const bool outsideFrustum = !c.visible;
        if (outsideFrustum) {
            if (frustumDrop) {
                ++culledCount;
            }
            // 画面外でも影は画面へ落ちる: 不透明のキャスターは影の候補に残す (描くかはカスケード別の判定)。
            // 距離で落とす物は影も落とすので入れない
            const bool shadowCandidate = collectShadowCasters && c.meshPtr && !c.tooFar;
            const Material* offMat = shadowCandidate ? resources.materials.Get(c.material) : nullptr;
            if (shadowCandidate && !(offMat && offMat->transparent != 0)) {
                RenderItem it;
                it.mesh = c.mesh;
                it.material = c.material;
                it.entity = c.e;
                it.world = c.world;
                it.viewZ = c.viewZ;
                it.lod = c.lod;
                ShadowCaster sc;
                sc.offscreenIndex = static_cast<int32_t>(offscreenCasters_.size());
                WorldAabb(c.world, c.boundsMin, c.boundsMax, sc.aabbMin, sc.aabbMax, c.boundsPadding);
                sc.bounded = 1;
                offscreenCasters_.push_back(it);
                offscreenScreenSize_.push_back(c.uroSize);
                shadowCasters_.push_back(sc);
                if (c.skinned != 0) {
                    // スキンは以前は常に可視 = フィット AABB に入っていた。フィットを変えないために入れ続ける
                    XMFLOAT3 bindMin, bindMax;
                    WorldAabb(c.world, c.meshPtr->aabbMin, c.meshPtr->aabbMax, bindMin, bindMax, c.boundsPadding);
                    sceneMin = { std::min(sceneMin.x, bindMin.x), std::min(sceneMin.y, bindMin.y),
                                 std::min(sceneMin.z, bindMin.z) };
                    sceneMax = { std::max(sceneMax.x, bindMax.x), std::max(sceneMax.y, bindMax.y),
                                 std::max(sceneMax.z, bindMax.z) };
                    hasScene = true;
                }
            }
            if (frustumDrop) {
                continue;
            }
        }
        // 距離の外 (視錐台の判定を通った物、または視錐台 OFF の物)。キュー・影・CSM のフィットに入れない
        if (c.tooFar) {
            ++culledDistanceCount;
            continue;
        }
        RenderItem item;
        item.mesh = c.mesh;
        item.material = c.material;
        item.entity = c.e;
        item.world = c.world;
        item.viewZ = c.viewZ;
        item.lod = c.lod;
        // RT が off のフレームは 1 固定 = G-Buffer が適用範囲の導入前とビット一致
        item.rtReceiver = (!rtAnyLane || rtScope.receiver.on) ? 1.0f : 0.0f;
        // M55c: velocity 用に「前フレームに実際に描いた行列」を載せる。履歴が無い
        // (初回 / リサイズ / 前フレームは視錐台の外だった / 生成直後) ときは現在値と
        // 同値を入れる = 画面速度が厳密に 0 = カメラ再投影のみへ縮退する。
        // Lookup は Record より先 (同じスロットを読んでから上書きする)
        item.prevWorld = c.world;
        if (prevRender != nullptr && !outsideFrustum) {
            if (const XMFLOAT4X4* pr = prevRender->Lookup(c.e)) {
                item.prevWorld = *pr;
            }
            prevRender->Record(c.e, c.world);
        }

        // スキンメッシュ (M18): ポーズ評価の要否をここで決め、ボーンパレットはループの後で item に繋ぐ。
        // ポーズは描画専用 (SkinnedMeshComponent は kComponentNoHash)
        SkinPaletteCache::Entry* paletteEntry = nullptr;
        if (auto* sm = world.GetComponent<SkinnedMeshComponent>(c.e)) {
            if (const SkinnedModel* model = resources.skinnedModels.Get(sm->model)) {
                paletteEntry = AcquireSkinPalette(world, c.e, *sm, *model, c.uroSize, paletteJobs, paletteReused);
            }
        }

        const Material* mat = resources.materials.Get(c.material);
        if (mat && mat->transparent != 0) {
            if (paletteEntry != nullptr) {
                paletteBinds.push_back({ true, queue_.transparent.size(), paletteEntry });
            }
            queue_.transparent.push_back(item);
        } else {
            if (collectShadowCasters && c.meshPtr && !outsideFrustum) {
                // 影の候補は箱が持てれば AABB (ワールド) で、持てなければ全カスケードへ入れる
                ShadowCaster sc;
                sc.queueIndex = static_cast<int32_t>(queue_.opaque.size());
                if (c.alwaysVisible == 0) {
                    WorldAabb(c.world, c.boundsMin, c.boundsMax, sc.aabbMin, sc.aabbMax, c.boundsPadding);
                    sc.bounded = 1;
                }
                shadowCasters_.push_back(sc);
            }
            if (c.meshPtr) {
                XMFLOAT3 wmin, wmax;
                // M79 sub-06: CSM のフィット AABB もカリングと同じ余白で広げる。
                // 0 のときは従来と同じ AABB。スキンもバインドポーズの AABB のまま (フィットを変えない)
                WorldAabb(c.world, c.meshPtr->aabbMin, c.meshPtr->aabbMax, wmin, wmax,
                         c.boundsPadding);
                // GPU オクルージョンの判定箱。スキンは姿勢が AABB に収まる保証が無いので載せない (常に描く)
                if (c.skinned == 0) {
                    item.worldAabbMin = wmin;
                    item.worldAabbMax = wmax;
                    item.hasWorldAabb = 1;
                }
                if (!outsideFrustum) {
                    sceneMin = { std::min(sceneMin.x, wmin.x), std::min(sceneMin.y, wmin.y),
                                 std::min(sceneMin.z, wmin.z) };
                    sceneMax = { std::max(sceneMax.x, wmax.x), std::max(sceneMax.y, wmax.y),
                                 std::max(sceneMax.z, wmax.z) };
                    hasScene = true;
                }
            }
            if (paletteEntry != nullptr) {
                paletteBinds.push_back({ false, queue_.opaque.size(), paletteEntry });
            }
            queue_.opaque.push_back(item);
        }
    }
    prof::AddCulled(culledCount);
    if (culledDistanceCount != 0) {
        prof::RenderStats culledDelta;
        culledDelta.culledDistance = culledDistanceCount;
        prof::AddRenderStats(culledDelta);
    }
    const auto stage3End = std::chrono::steady_clock::now();

    // ---- パレットの評価 (並列) と、キューの項目への接続 (直列、キューの添字は固定) ----
    RunPaletteJobs(world, paletteJobs);
    for (const PaletteBind& bind : paletteBinds) {
        RenderItem& item = bind.transparent ? queue_.transparent[bind.index] : queue_.opaque[bind.index];
        item.bones = bind.entry->palette.data();
        item.boneCount = static_cast<int32_t>(bind.entry->palette.size());
    }
    if (!paletteJobs.empty() || paletteReused != 0) {
        prof::RenderStats delta;
        delta.paletteEvaluated = static_cast<int>(paletteJobs.size());
        delta.paletteReused = paletteReused;
        prof::AddRenderStats(delta);
    }

    CullCpuMs sample;
    sample.total = ElapsedMs(collectStart, std::chrono::steady_clock::now());
    sample.stage1 = ElapsedMs(stage1Start, stage2Start);
    sample.stage2 = ElapsedMs(stage2Start, stage2End);
    sample.frustum = frustumMs;
    sample.stage3 = ElapsedMs(stage3Start, stage3End);
    cullCpuSamples_[cullCpuNext_] = sample;
    cullCpuNext_ = (cullCpuNext_ + 1) % kCullCpuWindow;
    cullCpuCount_ = (std::min)(cullCpuCount_ + 1, kCullCpuWindow);
}

RenderSystem::CullCpuMs RenderSystem::CullCpuTimes() const
{
    CullCpuMs avg;
    if (cullCpuCount_ == 0) {
        return avg;
    }
    for (int i = 0; i < cullCpuCount_; ++i) {
        avg.total += cullCpuSamples_[i].total;
        avg.stage1 += cullCpuSamples_[i].stage1;
        avg.stage2 += cullCpuSamples_[i].stage2;
        avg.frustum += cullCpuSamples_[i].frustum;
        avg.stage3 += cullCpuSamples_[i].stage3;
    }
    const float inv = 1.0f / static_cast<float>(cullCpuCount_);
    avg.total *= inv;
    avg.stage1 *= inv;
    avg.stage2 *= inv;
    avg.frustum *= inv;
    avg.stage3 *= inv;
    return avg;
}

namespace {

// スキンメッシュ 1 体のボーンパレット。ポーズは描画専用 (SkinnedMeshComponent は kComponentNoHash)。
// 入力は ECS の状態と alpha だけの純関数で、world は読むだけ (ジョブから呼べる)。
// 戻り値は kMaxBones で切る前の骨の数。alpha = 描画補間 (1 = 補間しない)
size_t EvaluateSkinPalette(World& world, EntityID e, const SkinnedMeshComponent& sm, const SkinnedModel& model,
                           float alpha, std::vector<XMFLOAT4X4>& palette)
{
    const float timeSec = static_cast<float>(sm.timeTicks) / 60.0f;
    // M60g1: ラグドールが作動中なら、骨の姿勢はアニメではなく**部位の
    // LocalTransform (= 剛体が置いた値)** から組む。
    // M18 追補: クロスフェード中は 2 クリップを混ぜた局所行列から組む。
    // M89a: ポーズプログラムが書かれていれば同じく局所行列から組む。
    // どちらでもない間は M18 の経路をそのまま通す (golden が動かない)
    if (const auto* rag = world.GetComponent<RagdollComponent>(e); rag && rag->active) {
        std::vector<XMMATRIX> locals;
        SampleSkinnedLocals(model, sm, locals);
        ragdoll::BuildBonePaletteFromLocals(world, e, model, locals, palette);
    } else if (UsesLocalsPath(sm)) {
        // M89f: ポーズプログラムの時刻は、ワールド行列と同じ条件 (interp) で前 tick と補間する。
        // ラグドールの枝は補間しない = 剛体が置いた部位 (tick 境界の値) と骨がずれないように
        std::vector<XMMATRIX> locals;
        SampleSkinnedLocalsInterpolated(model, sm, alpha, locals);
        ComputeBonePaletteWithOverrides(model, locals, {}, {}, palette);
    } else {
        ComputeBonePalette(model, sm.clip, timeSec, palette);
    }
    const size_t rawBoneCount = palette.size();
    if (rawBoneCount > static_cast<size_t>(kMaxBones)) {
        // 上限超過は切り捨て (シェーダの定数バッファが kMaxBones 固定のため)。WARN は呼び出し側 (直列) が出す
        palette.resize(static_cast<size_t>(kMaxBones));
    }
    return rawBoneCount;
}

} // namespace

SkinPaletteCache::Entry* RenderSystem::AcquireSkinPalette(World& world, EntityID e, const SkinnedMeshComponent& sm,
                                                          const SkinnedModel& model, float uroScreenSize,
                                                          std::vector<PaletteJob>& jobs, int& reused)
{
    const auto* rag = world.GetComponent<RagdollComponent>(e);
    const bool ragdollActive = rag != nullptr && rag->active;
    SkinPaletteCache::Request req;
    req.entity = e;
    req.model = sm.model;
    req.sm = &sm;
    req.alpha = (prevWorld != nullptr && interpAlpha < 1.0f) ? interpAlpha : 1.0f;
    req.simTick = simTick;
    // 剛体が置いた部位から組むパレットは ECS のポーズ入力だけでは決まらないので、キャッシュに使わない
    req.cacheable = !ragdollActive;
    req.interval = (enableAnimUro && UroEligible(sm, ragdollActive)) ? UroUpdateInterval(uroScreenSize) : 1u;
    if (SkinPaletteCache::Entry* hit = skinPaletteCache_.Find(req)) {
        ++reused;
        return hit;
    }
    SkinPaletteCache::Entry* entry = skinPaletteCache_.Claim(req);
    PaletteJob job;
    job.entity = e;
    job.sm = &sm;
    job.model = &model;
    job.entry = entry;
    job.alpha = req.alpha;
    jobs.push_back(job);
    return entry;
}

void RenderSystem::RunPaletteJobs(World& world, std::vector<PaletteJob>& jobs)
{
    // 数個では起動コストの方が高い。パレット 1 体の評価は骨数 x クリップのサンプルで、それなりに重い
    constexpr size_t kPaletteGrain = 4;
    jobs::System().ParallelRanges(jobs.size(), kPaletteGrain, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            PaletteJob& job = jobs[i];
            job.rawBoneCount = EvaluateSkinPalette(world, job.entity, *job.sm, *job.model, job.alpha,
                                                   job.entry->palette);
        }
    });
    // 黙って切ると姿勢が壊れた原因が追えないので model 毎に 1 回だけ WARN (並列段では書かない)
    for (const PaletteJob& job : jobs) {
        if (job.rawBoneCount <= static_cast<size_t>(kMaxBones)) {
            continue;
        }
        if (std::find(boneOverflowWarned_.begin(), boneOverflowWarned_.end(), job.sm->model.value)
            == boneOverflowWarned_.end()) {
            boneOverflowWarned_.push_back(job.sm->model.value);
            MYE_LOG_WARN("skinned model has %zu bones, clamped to %d "
                         "(MYE_MAX_BONES): pose will be wrong for the extra bones",
                         job.rawBoneCount, kMaxBones);
        }
    }
}

void RenderSystem::ApplyCullingSettings(const CullingSettings& settings)
{
    enableFrustumCulling = settings.frustum;
    enableOcclusionCulling = settings.occlusion;
    enableDistanceCulling = settings.distance;
    enableSizeCulling = settings.size;
    cullMinScreenSize = settings.minScreenSize;
    cullDefaultMaxDrawDistance = settings.defaultMaxDrawDistance;
    std::copy(std::begin(settings.tagDistance), std::end(settings.tagDistance), std::begin(cullTagDistance));
}

void RenderSystem::ResetRenderHistory()
{
    for (LodHistory& history : lodHistory_) {
        history.Clear();
    }
    skinPaletteCache_.Clear();
}

// 平行光の CSM (M17 / M38d): 最初の平行光でシーンにフィットした 3 カスケードを描く
void RenderSystem::RenderCascadeShadows(World& world, GraphicsDevice& device, ShaderManager& shaders,
                                        RenderResources& resources, FrameContext& f)
{
    RenderView& view = f.view;
    SceneLightData& lights = f.lights;
    const XMFLOAT3& sceneMin = f.sceneMin;
    const XMFLOAT3& sceneMax = f.sceneMax;
    const bool hasScene = f.hasScene;
    if (!shadowPass_.IsReady()) {
        shadowPass_.Init(device, shaders);
    }
    view.shadowSRV = nullptr;
    if (enableShadows && shadowPass_.IsReady() && hasScene) {
        const int dirIdx = FindDirectionalLight(lights);
        if (dirIdx >= 0) {
            // M38d: カメラフィットの 3 カスケード (非 perspective はシーン全体×3 に縮退)
            XMFLOAT4X4 lightVPs[ShadowPass::kCascades];
            float splits[ShadowPass::kCascades];
            ComputeCascadeVPs(lights.lights[dirIdx].direction, sceneMin, sceneMax, view,
                              shadowPass_.Resolution(), lightVPs, splits,
                              ShadowPass::kCascades);

            // カスケードごとのキャスター判定: そのカスケードのライト直交視錐台と交わる物だけを描く。
            // 候補はカメラの視錐台で落とす前の全不透明 (画面外の物の影も画面へ落ちる)。
            // ライト側の奥行きは切らない: 近平面を除く 5 面で判定し、描画は深度クランプ (ShadowPass)
            Frustum cascadeFrustum[ShadowPass::kCascades];
            for (int c = 0; c < ShadowPass::kCascades; ++c) {
                cascadeFrustum[c] = BuildFrustum(lightVPs[c]);
            }
            // 判定はキャスターごとに独立 (入力は不変、出力は自分の要素だけ) = 並列
            shadowCasterMask_.assign(shadowCasters_.size(), 0);
            constexpr size_t kCasterGrain = 256;
            jobs::System().ParallelRanges(shadowCasters_.size(), kCasterGrain, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    const ShadowCaster& sc = shadowCasters_[i];
                    shadowCasterMask_[i] = CascadeCasterMask(cascadeFrustum, ShadowPass::kCascades, sc.bounded != 0,
                                                             sc.aabbMin, sc.aabbMax);
                }
            });
            // 画面外のスキンは、どれかのカスケードに入ったときだけパレットを評価する。
            // 直列でエントリを確保 → 並列で評価 → 直列で項目へ繋ぐ (本描画のパレットと同じキャッシュを使う)
            std::vector<PaletteJob> paletteJobs;
            std::vector<std::pair<size_t, SkinPaletteCache::Entry*>> offscreenBinds;
            int paletteReused = 0;
            for (size_t i = 0; i < shadowCasters_.size(); ++i) {
                const ShadowCaster& sc = shadowCasters_[i];
                if (sc.offscreenIndex < 0 || !OffscreenCasterNeedsPalette(shadowCasterMask_[i])) {
                    continue;
                }
                const size_t offscreenIndex = static_cast<size_t>(sc.offscreenIndex);
                const RenderItem& item = offscreenCasters_[offscreenIndex];
                const auto* sm = world.GetComponent<SkinnedMeshComponent>(item.entity);
                const SkinnedModel* model = sm != nullptr ? resources.skinnedModels.Get(sm->model) : nullptr;
                if (model != nullptr) {
                    offscreenBinds.emplace_back(
                        offscreenIndex, AcquireSkinPalette(world, item.entity, *sm, *model,
                                                           offscreenScreenSize_[offscreenIndex], paletteJobs,
                                                           paletteReused));
                }
            }
            RunPaletteJobs(world, paletteJobs);
            for (const auto& bind : offscreenBinds) {
                RenderItem& item = offscreenCasters_[bind.first];
                item.bones = bind.second->palette.data();
                item.boneCount = static_cast<int32_t>(bind.second->palette.size());
            }
            // 並びは本描画のキューと同じキー (material → mesh → 深度 → entity)。連続する同一メッシュを
            // BuildInstanceRuns がインスタンシングする。深度だけなので並びは結果に効かない
            prof::RenderStats delta;
            delta.shadowCasterCandidates = static_cast<int>(shadowCasters_.size());
            delta.paletteEvaluated = static_cast<int>(paletteJobs.size());
            delta.paletteReused = paletteReused;
            for (int c = 0; c < ShadowPass::kCascades; ++c) {
                std::vector<RenderItem>& list = cascadeQueues_[c].opaque;
                cascadeQueues_[c].Clear();
                for (size_t i = 0; i < shadowCasters_.size(); ++i) {
                    if ((shadowCasterMask_[i] & (1u << c)) == 0) {
                        continue;
                    }
                    const ShadowCaster& sc = shadowCasters_[i];
                    list.push_back(sc.queueIndex >= 0 ? queue_.opaque[static_cast<size_t>(sc.queueIndex)]
                                                      : offscreenCasters_[static_cast<size_t>(sc.offscreenIndex)]);
                }
                cascadeQueues_[c].Sort();
                delta.shadowCascadeCasters[c] = static_cast<int>(list.size());
            }
            prof::AddRenderStats(delta);

            const RenderQueue* cascadeQueuePtrs[ShadowPass::kCascades] = { &cascadeQueues_[0], &cascadeQueues_[1],
                                                                          &cascadeQueues_[2] };
            shadowPass_.Render(device, shaders, cascadeQueuePtrs, resources, lightVPs,
                               ShadowPass::kCascades, view.viewFrameIndex, enableInstancing,
                               view.water); // M79 sub-05: MyEngineWater (影エントリ用)
            for (int c = 0; c < ShadowPass::kCascades; ++c) {
                XMStoreFloat4x4(&view.lightViewProj[c],
                                XMMatrixTranspose(XMLoadFloat4x4(&lightVPs[c])));
                view.cascadeSplits[c] = splits[c];
            }
            view.cascadeCount = ShadowPass::kCascades;
            view.shadowSRV = shadowPass_.SRV();
            view.shadowTexelSize = 1.0f / static_cast<float>(shadowPass_.Resolution());
        }
    }
}

// 局所ライトのシャドウアトラス (M54c: スポット 1 面 / M54d: 点光源 6 面): 影を投げるライトへ枠を前詰めで割り当てて描く
void RenderSystem::AllocateShadowAtlas(World& world, GraphicsDevice& device, ShaderManager& shaders,
                                       RenderResources& resources, FrameContext& frame) // f は下の面番号ループが使う名前
{
    RenderView& view = frame.view;
    SceneLightData& lights = frame.lights;
    const XMFLOAT3& sceneMin = frame.sceneMin;
    const XMFLOAT3& sceneMax = frame.sceneMax;
    const bool hasScene = frame.hasScene;
    // 枠は「M54b の決定論キーで並んだライト順に前詰め」= シーンが変わらなければ
    // frame をまたいでも同じライトが同じ枠に落ちる (割当が揺れると影がポップする)。
    // ★点光源は 6 枚を**連番**で取る — シェーダは shadowTile + 面番号で引くので、
    //   途中に他のライトの枠が挟まると隣の深度を読んでしまう
    view.shadowAtlasSRV = nullptr;
    view.shadowTileCount = 0;
    shadowAtlasFaceCulled_ = 0;
    if (enableShadows && enableLocalShadows && hasScene) {
        int tileCount = 0;
        int culledFaces = 0;
        int casters = 0;
        // 面カリングの判定は、全タイルが決まってから (画面外のキャスターの箱も判定に入れるため)
        Frustum tileFrustums[kMaxShadowTiles];
        bool tileTouchesScene[kMaxShadowTiles] = {};
        for (int i = 0; i < lightSelection.count && i < lights.count; ++i) {
            if (lightSelection.lights[i].shadowSlot < 0) {
                continue;
            }
            const GpuLight& g = lights.lights[i];
            const int faces = (g.type == lighttype::kPoint) ? 6 : ((g.type == lighttype::kSpot) ? 1 : 0);
            if (faces == 0) {
                continue; // 平行光の影は CSM (ShadowPass) の担当
            }
            // アトラス未生成ならここで初めて作る (64MB。影を使わないシーンでは払わない)
            if (!shadowAtlas_.IsReady() && !shadowAtlas_.Init(device, shaders)) {
                break;
            }
            // ★入り切らないライトは break ではなく skip。6 枚要る点光源が入らなくても
            //   後ろに並ぶ 1 枚のスポットはまだ入る。選別順に前詰めという規則は
            //   保たれるので、割当は決定論のまま
            if (tileCount + faces > shadowAtlas_.TileCapacity()) {
                continue;
            }
            const int base = tileCount;
            for (int f = 0; f < faces; ++f) {
                ShadowTile& tile = view.shadowTiles[base + f];
                shadowAtlas_.FillTileRect(base + f, tile);
                tile.lightViewProj = (faces == 6)
                    ? ComputePointLightFaceVP(g.position, f, g.range, shadowAtlas_.TileSize())
                    : ComputeSpotLightVP(g.position, g.direction, g.cosOuter, g.range);
                // 定数バイアスはラスタライザ側 (傾斜依存) を主役にしているので極小。
                // 0 にすると自己遮蔽の縞が出る距離帯が残る (実測で詰めた値)
                tile.depthBias = 0.00015f;
                tileFrustums[base + f] = BuildFrustum(tile.lightViewProj);
                tileTouchesScene[base + f] = WorldAabbInFrustum(tileFrustums[base + f], sceneMin, sceneMax);
            }
            lights.lights[i].shadowTile = base;
            lights.lights[i].shadowFaces = faces;
            tileCount += faces;
            ++casters;
        }
        if (tileCount > 0) {
            // 画面外のキャスター (視錐台で落とした不透明) のうち、どれかのタイルの視錐台に入るもの。
            // bit t = タイル t。添字は offscreenCasters_ と同じ
            static_assert(kMaxShadowTiles <= 16, "offscreenTileMask is 16 bits");
            std::vector<uint16_t> offscreenTileMask(offscreenCasters_.size(), 0);
            jobs::System().ParallelRanges(shadowCasters_.size(), 256, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    const ShadowCaster& sc = shadowCasters_[i];
                    if (sc.offscreenIndex < 0) {
                        continue;
                    }
                    uint16_t mask = 0;
                    for (int t = 0; t < tileCount; ++t) {
                        if (WorldAabbInFrustum(tileFrustums[t], sc.aabbMin, sc.aabbMax)) {
                            mask = static_cast<uint16_t>(mask | (1u << t));
                        }
                    }
                    offscreenTileMask[static_cast<size_t>(sc.offscreenIndex)] = mask;
                }
            });
            uint16_t tilesWithOffscreen = 0;
            for (const uint16_t mask : offscreenTileMask) {
                tilesWithOffscreen = static_cast<uint16_t>(tilesWithOffscreen | mask);
            }
            // ★面カリング (M54d): この面の視錐台がシーン AABB にも画面外のキャスターにも触れないなら描かない。
            //   タイルは**確保したまま** pixelSize=0 にする — 連番を詰めると
            //   shadowTile + 面番号の対応が崩れる。描かれないタイルはクリア値
            //   1.0 (最遠) のままなので、サンプルしても「影なし」に落ちる
            for (int t = 0; t < tileCount; ++t) {
                if (!tileTouchesScene[t] && (tilesWithOffscreen & (1u << t)) == 0) {
                    view.shadowTiles[t].pixelSize = 0;
                    ++culledFaces;
                }
            }

            // 描くのは「画面内 + どれかのタイルに入る画面外のキャスター」。視錐台スイッチが OFF のときは
            // 画面外の物も queue_ に入っているので、queue_ ではなく影の候補から組む (ON と描く集合を同じにする)
            const RenderQueue* atlasQueue = &queue_;
            if (!offscreenCasters_.empty()) {
                // 画面外のスキンは、どれかのタイルが必要とするときだけパレットを評価する
                std::vector<PaletteJob> paletteJobs;
                std::vector<std::pair<size_t, SkinPaletteCache::Entry*>> offscreenBinds;
                int paletteReused = 0;
                atlasQueue_.Clear();
                for (const ShadowCaster& sc : shadowCasters_) {
                    if (sc.queueIndex >= 0 || offscreenTileMask[static_cast<size_t>(sc.offscreenIndex)] == 0) {
                        continue;
                    }
                    const size_t offscreenIndex = static_cast<size_t>(sc.offscreenIndex);
                    const RenderItem& item = offscreenCasters_[offscreenIndex];
                    const auto* sm = world.GetComponent<SkinnedMeshComponent>(item.entity);
                    const SkinnedModel* model = sm != nullptr ? resources.skinnedModels.Get(sm->model) : nullptr;
                    if (model != nullptr) {
                        offscreenBinds.emplace_back(
                            offscreenIndex, AcquireSkinPalette(world, item.entity, *sm, *model,
                                                               offscreenScreenSize_[offscreenIndex], paletteJobs,
                                                               paletteReused));
                    }
                }
                RunPaletteJobs(world, paletteJobs);
                for (const auto& bind : offscreenBinds) {
                    RenderItem& item = offscreenCasters_[bind.first];
                    item.bones = bind.second->palette.data();
                    item.boneCount = static_cast<int32_t>(bind.second->palette.size());
                }
                if (!paletteJobs.empty() || paletteReused != 0) {
                    prof::RenderStats delta;
                    delta.paletteEvaluated = static_cast<int>(paletteJobs.size());
                    delta.paletteReused = paletteReused;
                    prof::AddRenderStats(delta);
                }
                for (const ShadowCaster& sc : shadowCasters_) {
                    if (sc.queueIndex >= 0) {
                        atlasQueue_.opaque.push_back(queue_.opaque[static_cast<size_t>(sc.queueIndex)]);
                    } else if (offscreenTileMask[static_cast<size_t>(sc.offscreenIndex)] != 0) {
                        atlasQueue_.opaque.push_back(offscreenCasters_[static_cast<size_t>(sc.offscreenIndex)]);
                    }
                }
                atlasQueue = &atlasQueue_;
            }
            shadowAtlas_.Render(device, shaders, *atlasQueue, resources, view.shadowTiles,
                                tileCount, enableInstancing);
            view.shadowAtlasSRV = shadowAtlas_.SRV();
            view.shadowAtlasTexel =
                1.0f / static_cast<float>(shadowAtlas_.Resolution());
            view.shadowTileCount = tileCount;
        }
        shadowAtlasFaceCulled_ = culledFaces;
        // M54d: 割当の実測をログへ。ヘッドレス撮影では ProfilerWindow が見えないので、
        // 「6 面が連番で取れたか / 面カリングが効いたか」はここでしか確認できない。
        // M54b のライト選別ログと同じ「出たことのある組み合わせを 1 回ずつ」方式
        // (SceneView と GameView で結果が食い違うと毎フレーム 2 行出続けるため)
        if (tileCount > 0) {
            auto pack = [](int v) { return static_cast<uint64_t>(v & 0xFFFF); };
            const uint64_t key = pack(tileCount) | (pack(culledFaces) << 16)
                | (pack(casters) << 32) | (pack(shadowAtlas_.DrawCalls()) << 48);
            bool seen = false;
            for (int i = 0; i < shadowLogSeenCount_; ++i) {
                seen = seen || shadowLogSeen_[i] == key;
            }
            if (!seen && shadowLogSeenCount_ < static_cast<int>(std::size(shadowLogSeen_))) {
                shadowLogSeen_[shadowLogSeenCount_++] = key;
                MYE_LOG_INFO("ShadowAtlas: %d tiles for %d local casters (%d faces culled, "
                             "%d draws, %d draws culled)",
                             tileCount, casters, culledFaces, shadowAtlas_.DrawCalls(),
                             shadowAtlas_.CulledDraws());
            }
        }
    }
}

// 描画フラグと環境: SSAO / SSR / プローブ、Skybox / Fog の収集と色のリニア化、太陽、IBL、前フレームの viewProj
void RenderSystem::PrepareEnvironment(World& world, GraphicsDevice& device, ShaderManager& shaders,
                                      RenderResources& resources, const FrameTarget& target,
                                      const CameraOverride* cameraOverride, FrameContext& f)
{
    RenderView& view = f.view;
    const EntityID camEntity = f.camEntity;
    SceneLightData& lights = f.lights;
    view.ssaoEnabled = enableSsao ? 1 : 0; // M38e (Deferred のみ消費)
    view.instancingEnabled = enableInstancing ? 1 : 0; // M38f
    // GPU オクルージョン (Deferred / Forward の不透明のみ)。viewKey 0 (AssetPreview) は履歴を持たないので常に off
    view.occlusionEnabled = (enableOcclusionCulling && target.viewKey > 0 && target.viewKey < 4) ? 1 : 0;
    view.frustumCulling = enableFrustumCulling ? 1 : 0; // CPU パーティクルのプール判定が読む
    view.velocityDebug = velocityDebugMode;            // M55c (Deferred のみ消費)
    view.hzbDebug = hzbDebugMip;                       // M56c (Deferred のみ消費。hzbDebugMax は Forward も)
    view.hzbDebugMax = hzbDebugMax ? 1 : 0;
    view.occlusionStatsWait = occlusionStatsWait ? 1 : 0;
    view.ssrEnabled = enableSsr ? 1 : 0;               // M56d (Deferred のみ消費)
    // M56f: 焼いたプローブ束をそのまま指す (Deferred のみ消費)。ベイクした所有者が
    // このポインタを立てるまで null = 1 命令も増えない。**トグルを設けていない**のは、
    // 「焼いていない = 無い」で十分だから (焼く操作そのものが明示 opt-in)
    view.probes = reflectionProbes;
    // M40d: シーンカメラの CameraPostFx から SSAO パラメータ (override = エディタ視界は既定)
    if (!cameraOverride && !camEntity.IsNull()) {
        if (const auto* pfx = world.GetComponent<CameraPostFxComponent>(camEntity)) {
            view.ssaoRadius = pfx->ssaoRadius;
            view.ssaoIntensity = pfx->ssaoIntensity;
            // M56d: SSR はグローバル設定をシーンカメラが上書きする (TAA と同じ規則)。
            // SceneView (cameraOverride あり) はグローバル設定のまま = エディタ視界は不変
            view.ssrEnabled = pfx->ssrOn ? 1 : 0;
            view.ssrMaxRoughness = pfx->ssrMaxRoughness;
            view.ssrIntensity = pfx->ssrIntensity;
        }
    }
    CollectEnvironment(world, view); // M29d: Skybox/Fog を view に反映
    // M38a: 環境の authored 色をリニアへ (CollectEnvironment 自体は純パススルーのまま =
    // selftest 不変)。スカイ/フォグは HDR 中間に描かれ、トーンマップ後の OETF と対になる
    view.skyTop = SrgbToLinear(view.skyTop);
    view.skyHorizon = SrgbToLinear(view.skyHorizon);
    view.skyBottom = SrgbToLinear(view.skyBottom);
    view.fogColor = SrgbToLinear(view.fogColor);
    // M43a: 太陽 = 最初の平行光 (CSM の dirIdx と同じ規則)。色はリニア・強度込み。
    // 平行光が無いシーンではインスキャッタ無効化 (黒い太陽へ lerp して暗転する事故を防ぐ)
    {
        int sunIdx = -1;
        for (int i = 0; i < lights.count; ++i) {
            if (lights.lights[i].type == lighttype::kDirectional) {
                sunIdx = i;
                break;
            }
        }
        if (sunIdx >= 0) {
            const GpuLight& g = lights.lights[sunIdx];
            view.sunDirection = g.direction;
            view.sunColor = { g.color.x * g.intensity, g.color.y * g.intensity,
                              g.color.z * g.intensity };
        } else {
            view.fogInscatterIntensity = 0.0f;
        }
    }
    // M38b / M76: cubemap / panoramic スカイの SRV 解決 (未ロードなら遅延ロード、不正なら gradient にフォールバック)
    // ★Skybox 無し (-1) は -1 のまま (ResolveSkyMode)。-1 まで 0 に落とすと、Skybox を置いていない
    //   シーンにグラデーション空と IBL が出て背景とライティングが変わる
    {
        bool hasTexture = false;
        bool isCube = false;
        if (view.skyMode > 0 && !view.skyCubemapId.IsNull()) {
            Texture* tex = resources.textures.Get(view.skyCubemapId);
            if (!tex) {
                const std::wstring texPath = assetguid::ResolvePath(view.skyCubemapId.value);
                if (!texPath.empty()) {
                    tex = LoadTextureRememberingFailure(resources.textures, view.skyCubemapId, texPath,
                                                        /*srgb=*/true, skyLoadFailed_);
                }
            }
            if (tex && tex->srv && tex->tex) {
                view.skyCubemap = tex->srv.Get();
                D3D11_TEXTURE2D_DESC td = {};
                tex->tex->GetDesc(&td);
                hasTexture = true;
                isCube = (td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0 || td.ArraySize == 6;
            }
        }
        view.skyMode = ResolveSkyMode(view.skyMode, hasTexture, isCube);
    }
    // M38c: スカイがあるなら IBL 環境マップを取得 (初回のみ GPU ベイク、以後キャッシュ)。
    // gradient も同じベイクに通す — シェーダ側は「IBL on/off」の 2 択で済む。
    // ベイクは RT/シェーダ状態を触るが、この後の path.Render が全て再設定するので安全
    // 2026-09-14: Skybox.lighting = 0 なら焼かない = IBL の SRV が null のまま = 各パスは定数アンビエントへ
    //   落ちる (ForwardPath / DeferredPath / SsrPass の判定はどれも「3 枚そろったら IBL」の 1 本)
    const bool skyLights = (view.skyLighting != 0);
    if ((view.skyMode == 1 || view.skyMode == 2) && view.skyCubemap != nullptr && skyLights) {
        const bool isPano = (view.skyMode == 2);
        const EnvMaps em =
            envBaker_.GetForCubemap(device, shaders, view.skyCubemapId, view.skyCubemap, isPano);
        view.iblIrradiance = em.irradiance;
        view.iblPrefiltered = em.prefiltered;
        view.iblBrdfLut = em.brdfLut;
        view.iblSpecMips = em.specMips;
    } else if (view.skyMode == 0 && skyLights) {
        const EnvMaps em = envBaker_.GetForGradient(device, shaders, view.skyTop, view.skyHorizon,
                                                    view.skyBottom); // リニア変換済みの色
        view.iblIrradiance = em.irradiance;
        view.iblPrefiltered = em.prefiltered;
        view.iblBrdfLut = em.brdfLut;
        view.iblSpecMips = em.specMips;
    }
    // M44d: 前フレームの viewProj / カメラ位置 (viewKey 毎)。初フレーム/リサイズは invalid。
    // モーションブラー (ポスプロ) と RT のテンポラル再投影 (M46d) の共通の出所なので、
    // path.Render より前で埋める
    if (target.viewKey > 0 && target.viewKey < 4) {
        const PrevViewProj& p = prevVP_[target.viewKey];
        if (p.valid && p.w == target.width && p.h == target.height) {
            view.prevViewProj = p.m;
            view.prevCameraPos = p.pos;
            view.prevViewProjValid = 1;
        }
    }
}

// レイトレ用シーン (M46b) を GPU へ上げて view へ配線し、SSR / プローブが要る環境 BRDF LUT を焼く
void RenderSystem::UpdateRtScene(GraphicsDevice& device, ShaderManager& shaders, RenderResources& resources,
                                 const FrameTarget& target, FrameContext& f)
{
    RenderView& view = f.view;
    // M46b: レイトレ用シーン (BLAS 連結 + TLAS + インスタンス) を GPU へ。
    // デバッグ表示・GI 合成・RT 影のどれも off なら収集自体が空なので、
    // この節はまるごと従来経路と同じになる
    if ((rtDebugMode != rtdebug::kOff || enableRtGi || enableRtShadow || enableRtRefl)
        && !rtInstances_.empty()) {
        if (!rtPasses_.IsReady()) {
            rtPasses_.Init(device, shaders);
        }
        rtScene_.Init(device);
        // M67f: クラス上書き (--rt-class-override / チューニング UI) は**インスタンスを
        // 組むこの 1 か所**で効かせる。デバッグ 13 (一次ヒットのクラス色) も同じ
        // RtInstance を読むので、上書きした瞬間に画面全体が 1 色になるのが期待どおり
        rtScene_.Update(rtInstances_, resources, rtReflRestirParams.classOverride);
        if (rtPasses_.IsReady() && rtScene_.Bindings().IsValid()) {
            view.rtDebugMode = rtDebugMode;
            view.rtScene = &rtScene_.Bindings();
            view.rtPasses = &rtPasses_;
            view.rtResolutionScale = rtResolutionScale;
            view.rtBounces = rtBounces;
            // freeze 中は毎フレーム同じ乱数列 = 決定的なスクリーンショットが撮れる
            view.rtFrameIndex = rtFreezeSeed ? 0u : rtFrameCounter_;
            ++rtFrameCounter_;
            // M46d: テンポラル蓄積。履歴は viewKey 別 (SceneView と GameView が混線しない)
            view.rtTemporal = rtTemporal ? 1 : 0;
            view.rtViewKey = (target.viewKey < 4) ? target.viewKey : 0u;
            view.rtViewSerial = viewSerial_[view.rtViewKey];
            // M46e: SVGF。凍結中はテンポラル分散が 0 に潰れるので空間推定へ落とす合図も渡す
            view.rtSvgf = rtSvgf ? 1 : 0;
            view.rtFreezeSeed = rtFreezeSeed ? 1 : 0;
            // M46f: 最終画像への合成 (ライトパスの拡散環境項を GI で置換)
            view.rtGiEnabled = enableRtGi ? 1 : 0;
            // M46g: 平行光のシャドウ係数を CSM でなくレイトレの可視率で作る。
            // 「Shadows」トグルは影全体の元栓なので、off なら RT 影も出さない
            view.rtShadowEnabled = (enableRtShadow && enableShadows) ? 1 : 0;
            // M46h: スペキュラ環境項をレイトレ反射で置換
            view.rtReflEnabled = enableRtRefl ? 1 : 0;
            // M67d: ReSTIR。デバッグ 12 (reservoir の M) / 14 (反射像側のクラス) は
            // reservoir そのものを映すので、トグルが off でも強制的に立てる。
            // ★条件の出所は RtRestirEffective() 1 本 (M67h) — エディタのチューニング UI も
            //   同じ関数で有効/無効を決める
            view.rtReflRestir = RtRestirEffective() ? 1 : 0;
            view.rtReflRestirParams = rtReflRestirParams;
            // 合成は split-sum なので環境 BRDF LUT (t7) が要る。IBL は「スカイがある」
            // ことが条件だが LUT 自体はスカイに依らない純関数なので、スカイ無しの
            // シーンでも反射のためにベイクしておく。irradiance/prefiltered は
            // null のままなので pf.iblEnabled は false に留まる = IBL は有効化されない
            if (enableRtRefl && view.iblBrdfLut == nullptr) {
                view.iblBrdfLut = envBaker_.GetBrdfLut(device, shaders);
            }
        }
    }
    // M56d: SSR も split-sum なので環境 BRDF LUT が要る。RT 反射 (すぐ上) と同じ理屈で、
    // **スカイの無いシーンでも LUT だけは焼く** — LUT はスカイに依らない純関数で、
    // irradiance / prefiltered は null のままなので pf.iblEnabled は false に留まる。
    // ★これを忘れると LUT が null → SampleLevel が 0 を返す → 環境 BRDF が 0 →
    //   **SSR を on にしても絵が 1 ピクセルも変わらない** (--render-demo にはスカイが無い)
    // M56f: 反射プローブも split-sum で合成する = 同じ理由で LUT が要る。
    // ★これを忘れると「プローブを焼いたのに絵が 1 画素も変わらない」になる
    //   (--render-demo にはスカイが無いので LUT が誰にも焼かれない)
    if ((view.ssrEnabled != 0 || (view.probes != nullptr && view.probes->count > 0))
        && view.iblBrdfLut == nullptr) {
        view.iblBrdfLut = envBaker_.GetBrdfLut(device, shaders);
    }
}

// フロクセル (M57b-M57d): 注入 → テンポラル → 前方積分。結果は光パスが読む
void RenderSystem::UpdateFroxel(GraphicsDevice& device, ShaderManager& shaders, const FrameTarget& target,
                                FrameContext& f)
{
    RenderView& view = f.view;
    SceneLightData& lights = f.lights;
    FroxelSettings& effectiveFroxel = f.effectiveFroxel;
    // M96: 場だけのモード。カメラ (または Scene View のグローバル設定) のフロクセルが off でも、霧を出す autoFroxel の
    // Volume があればフロクセルを回し、Volume の霧だけを足す。ユーザー設定は書き換えず、このフレームの実効値だけ決める。
    // Volume の束 (view.disturbanceVolumes) は Decide より後に作られるので、判定はここ
    const bool fieldOnly = !f.froxelOn && f.cameraFound && view.disturbanceVolumes != nullptr
        && view.disturbanceVolumes->wantsAutoFroxel;
    if (fieldOnly) {
        effectiveFroxel.density = 0.0f; // グローバルの媒質は 0 (高さの霧も density に掛かるので 0)
    }
    view.froxelFieldOnly = fieldOnly;
    if (target.viewKey < 4 && froxelFieldOnly_[target.viewKey] != fieldOnly) {
        froxelFieldOnly_[target.viewKey] = fieldOnly;
        MYE_LOG_INFO(fieldOnly ? "Froxel: view %u entered field-only mode (a disturbance Volume fog needs it)"
                               : "Froxel: view %u left field-only mode",
                     static_cast<unsigned>(target.viewKey));
    }
    const bool froxelOn = f.froxelOn || fieldOnly;
    // ★置き場所はここしかない: 上流に CollectEnvironment (高度フォグのパラメータ) と
    //   シャドウアトラス (SampleShadowAtlas の入力) が要り、下流の path.Render より
    //   前でないと消費側 (光パス) が積分結果を読めない。
    // ★**積分が走らなかったフレームは SRV が null のまま** =
    //   光パス側のゲートで ApplyFog へ落ちる (正射影ビュー / シェーダ未ロードなど)
    if (froxelOn && (froxelPass_.IsReady() || froxelPass_.Init(device, shaders))) {
        view.froxelSRV = froxelPass_.Render(device, shaders, view, lights, effectiveFroxel);
        view.froxelNearZ = froxelPass_.GridNearZ();
        view.froxelFarZ = froxelPass_.GridFarZ();
        view.froxelSlices = froxelPass_.GridSliceCount();
        // 「そのビューの N 回目の描画」= 決定的撮影モードでは frame 番号と一致する
        // (viewSerial_ はこの Render の末尾で +1 される = ここでの値が今フレームの通番)
        const uint32_t serial = (target.viewKey < 4) ? viewSerial_[target.viewKey] : 0u;
        if (froxelDumpFrame >= 0 && static_cast<uint32_t>(froxelDumpFrame) == serial) {
            froxelPass_.DebugDumpAB(device, shaders, view, lights, effectiveFroxel);
            // DebugDumpAB は検査のために注入をもう 2 回走らせてボリュームを塗り替える。
            // その状態の scatter_ を積分し直さないまま光パスへ渡すと「ダンプした
            // フレームだけ絵が違う」になるので、確定した設定でもう一度回して戻す
            view.froxelSRV = froxelPass_.Render(device, shaders, view, lights, effectiveFroxel);
        }
    }
}

// かき乱しフィールド (M96) の 2D 層を GPU のテクスチャ配列へ上げ、view へ配線する。
// 地面の霧は各ピクセルが読む (ground_fog.hlsli) ので、フロクセルの on / off に依らず霧のある層があれば転送し、
// PS の固定スロットへ張る (剥がすのは描画の最後)。霧のある層が無ければ view.disturbance は null のまま = 従来と同じ絵
void RenderSystem::UpdateDisturbanceField(World& world, GraphicsDevice& device, RenderResources& resources,
                                          FrameContext& f)
{
    f.view.disturbance = nullptr;
    if (disturbanceWorld == nullptr) {
        return;
    }
    // 状態を読めるのは、状態を持つシーン自身の World を描くときだけ
    const DisturbanceState* state = (&world == disturbanceWorld) ? disturbance : nullptr;
    DisturbanceFieldUpload up;
    if (BuildDisturbanceFieldUpload(world, state, up) <= 0) {
        return;
    }
    up.noiseTime = DisturbanceNoiseTime(simTick, interpAlpha);
    for (int32_t i = 0; i < up.layerCount; ++i) {
        ResolveDisturbanceNoiseMap(resources.textures, up.layers[i].noise, noiseMapLoadFailed_[i]);
    }
    // 霧のある層、状態を持つ層 (流れを草が読む。MyeFieldSample / 波紋を水面が読む) があれば転送する。
    // 状態の無い層 (編集中) は流れが無いので、霧が無ければ従来どおり転送しない
    bool anyFog = false;
    for (int32_t i = 0; i < up.layerCount; ++i) {
        const DisturbanceLayerUpload& l = up.layers[i];
        anyFog = anyFog || (l.hasDensity && l.fogDensity > 0.0f) || l.hasState;
    }
    if (!anyFog || !disturbancePass_.Upload(device, up)) {
        return;
    }
    f.view.disturbance = &disturbancePass_.Gpu();

    GroundFogViewParams gf;
    gf.camPos[0] = f.view.cameraPos.x;
    gf.camPos[1] = f.view.cameraPos.y;
    gf.camPos[2] = f.view.cameraPos.z;
    gf.lights = &f.lights;
    gf.ambient[0] = f.lights.ambient.x;
    gf.ambient[1] = f.lights.ambient.y;
    gf.ambient[2] = f.lights.ambient.z;
    {
        const XMMATRIX vp = XMMatrixMultiply(XMLoadFloat4x4(&f.view.view), XMLoadFloat4x4(&f.view.proj));
        XMFLOAT4X4 inv;
        XMStoreFloat4x4(&inv, XMMatrixTranspose(XMMatrixInverse(nullptr, vp)));
        std::memcpy(gf.invViewProj, &inv, sizeof(gf.invViewProj));
    }
    const int sun = FindDirectionalLight(f.lights);
    if (sun >= 0) {
        const GpuLight& g = f.lights.lights[sun];
        gf.hasSun = true;
        gf.sunDir[0] = g.direction.x;
        gf.sunDir[1] = g.direction.y;
        gf.sunDir[2] = g.direction.z;
        gf.sunColor[0] = g.color.x * g.intensity;
        gf.sunColor[1] = g.color.y * g.intensity;
        gf.sunColor[2] = g.color.z * g.intensity;
    }
    groundFogBound_ = disturbancePass_.BindGroundFog(device, f.view.disturbance, gf);
}

// かき乱しフィールドの Volume (M96g、3D) を Texture3D へ上げ、view へ配線する。読むのはフロクセルの注入だけなので
// フロクセルの on / off に依らず転送するが、霧を出す Volume が無ければ view.disturbanceVolumes は null のまま = 従来と同じ絵
void RenderSystem::UpdateDisturbanceVolumes(World& world, GraphicsDevice& device, RenderResources& resources,
                                            FrameContext& f)
{
    f.view.disturbanceVolumes = nullptr;
    if (disturbanceWorld == nullptr) {
        return;
    }
    const DisturbanceState* state = (&world == disturbanceWorld) ? disturbance : nullptr;
    DisturbanceVolumeUpload up;
    if (BuildDisturbanceVolumeUpload(world, state, up) <= 0) {
        return;
    }
    up.noiseTime = DisturbanceNoiseTime(simTick, interpAlpha);
    for (int32_t i = 0; i < up.count; ++i) {
        ResolveDisturbanceNoiseMap(resources.textures, up.volumes[i].noise,
                                   noiseMapLoadFailed_[kDisturbanceMaxLayers + i]);
    }
    if (disturbanceVolumePass_.Upload(device, up)) {
        f.view.disturbanceVolumes = &disturbanceVolumePass_.Gpu();
    }
}

// 音響の残光ボリューム (M65d) と解析的な波面: CPU の値を 3D テクスチャへ上げて view へ配線する
// (読むのは M65e の光パス。転送の正しさは `--acoustic-dump` の読み戻しで数値として確かめる)
void RenderSystem::UpdateAcousticVolume(GraphicsDevice& device, const FrameTarget& target, FrameContext& f)
{
    RenderView& view = f.view;
    // ★三重のゲート: ポインタが null / ボリュームが無い / 一度も光っていない、の
    //   どれかなら SRV は null のまま = 既存の絵は 1 ビットも動かない
    acousticSupplied_ = false;
    if (acousticField != nullptr && acousticField->HasVolume() && acousticField->VisualActive()) {
        const AcousticGridDesc& ag = acousticField->Grid();
        const std::vector<uint8_t>& glow = acousticField->Glow();
        if (static_cast<int64_t>(glow.size()) == ag.CellCount()) {
            AcousticVolumeUpload up;
            up.cells = glow.data();
            up.dimX = ag.dimX;
            up.dimY = ag.dimY;
            up.dimZ = ag.dimZ;
            up.serial = acousticField->VisualSerial();
            if (acousticPass_.Upload(device, up)) {
                view.acousticSRV = acousticPass_.SRV();
                view.acousticGridMin[0] = ag.minX;
                view.acousticGridMin[1] = ag.minY;
                view.acousticGridMin[2] = ag.minZ;
                // 1 / (dim * cellSize) = ワールド差分をテクスチャ座標 [0,1] へ写す係数。
                // ★**dim を掛けるのが要点** — cellSize だけで割るとセル単位の座標になる
                view.acousticInvSize[0] = 1.0f / (static_cast<float>(ag.dimX) * ag.cellSize);
                view.acousticInvSize[1] = 1.0f / (static_cast<float>(ag.dimY) * ag.cellSize);
                view.acousticInvSize[2] = 1.0f / (static_cast<float>(ag.dimZ) * ag.cellSize);
                // M65h: ボリューム側の明るさ (glowIntensity) と全体係数の積。
                // 既定はどちらも 1.0 = 既存の絵と 1 ビットも変わらない
                view.acousticIntensity = acousticIntensity * acousticField->GlowIntensity();
                // 強い残光に面の色を混ぜる割合。既定 0 = 既存の絵と 1 ビットも変わらない
                view.acousticAlbedoMix = acousticField->GlowAlbedoMix();
                // 閉セルは波が絶対に訪れない = 壁面の残光は開セル側にしかない。
                // 法線方向へ 0.75 セル押し出して開セル側を引くのが
                // 「壁に当たった面だけが光る」の正体 (計画 判断 5)
                view.acousticNormalPush = 0.75f * ag.cellSize;
                acousticSupplied_ = true;
                // 2026-09-12「描画だけ円」: 見通しビットと波の表。マスクの転送に失敗したら
                // SRV が null のまま = 従来 (残光だけ) の絵に静かに戻る
                const std::vector<uint32_t>& front = acousticField->FrontMask();
                if (acousticFront && acousticField->FrontActive()
                    && static_cast<int64_t>(front.size()) == ag.CellCount()
                    && acousticPass_.UploadFront(device, front.data(), ag.dimX, ag.dimY, ag.dimZ,
                                                 acousticField->FrontSerial())) {
                    view.acousticFrontSRV = acousticPass_.FrontSRV();
                    const AcousticField::FrontWave* waves = acousticField->FrontWaves();
                    int n = 0;
                    for (uint32_t s = 0; s < AcousticField::kMaxWaves && s < RenderView::kAcousticWaveSlots; ++s) {
                        // スロット番号 = マスクのビット番号なので、空きも位置を保って詰めない
                        RenderView::AcousticWaveGpu& g = view.acousticWaves[s];
                        g = RenderView::AcousticWaveGpu{};
                        if (waves[s].active == 0) {
                            continue;
                        }
                        g.origin[0] = waves[s].ox;
                        g.origin[1] = waves[s].oy;
                        g.origin[2] = waves[s].oz;
                        g.radiusM = waves[s].radiusM;
                        g.amplitude = waves[s].amplitude;
                        g.maxDistM = waves[s].maxDistM;
                        g.ticksPerMetre = waves[s].ticksPerMetre;
                        g.extraAgeTicks = waves[s].extraAgeTicks;
                        n = static_cast<int>(s) + 1;
                    }
                    view.acousticWaveCount = n;
                    const float keep = acousticField->GlowKeepPerTick();
                    view.acousticKeepPerTick = (keep > 0.0f && keep < 1.0f) ? keep : acoustic::kGlowDecayPerTick;
                    view.acousticCellSize = ag.cellSize;
                }
            }
        }
        // 「そのビューの N 回目の描画」= 決定的撮影モードでは frame 番号と一致する
        const uint32_t acSerial = (target.viewKey < 4) ? viewSerial_[target.viewKey] : 0u;
        if (acousticDumpFrame >= 0 && static_cast<uint32_t>(acousticDumpFrame) == acSerial
            && acousticSupplied_) {
            DumpAcousticVolume(device, acousticPass_, *acousticField);
        }
    }
}

// パスの後段: VFX (M29c) → パーティクル (M42 / M63d) → スクリプトの DebugDrawLine (M37)
void RenderSystem::DrawParticlesAndDebug(World& world, GraphicsDevice& device, ShaderManager& shaders,
                                         RenderResources& resources, const FrameTarget& target,
                                         const CameraOverride* cameraOverride, ParticleSystem* particles,
                                         VfxRenderer* vfx, FrameContext& f)
{
    RenderView& view = f.view;
    SceneLightData& lights = f.lights;
    PostProcess::Target* hdr = f.hdr;
    bool& distortionActive = f.distortionActive;
    // M94: Sprite2D。VFX の直前 (整数ソートキーで並べた 2D スプライト)。Sprite2D が無ければ
    // 描画リストが空のままパスを Init もせず、D3D の状態も触らない
    {
        const Sprite2DDrawList& sprites = sprite2dRenderer_.Collect(world, resources, &view.view, &view.proj, simTick);
        if (!sprites.Empty()) {
            if (!sprite2dPass_.IsReady()) {
                sprite2dPass_.Init(device, shaders);
            }
            sprite2dPass_.Render(device, shaders, resources.textures, resources.materials, sprites, view.view, view.proj, view.dsv);
        }
    }
    // VFX (M29c): Sprite/Trail/TextMesh をメッシュ (不透明+透明) の後・パーティクルの前に
    // 重ねる。HDR 中間へ描かれ postfx を通る。RT はパスがバインドしたまま
    if (vfx) {
        vfx->Render(world, device, shaders, resources, view);
    }

    // M63d: パーティクルへライトを渡す。**パスの外で描かれる**ので forward_lit /
    // deferred_light が使う CB には相乗りできず、RenderView 経由が唯一の口になる。
    // ★ここで初めて配線するのは、パーティクル (と VFX) より前を 1 行も変えないため。
    //   非所有ポインタなので lights の寿命 (この関数のローカル) より長生きしない
    view.lights = &lights;

    // パーティクルは常に Forward 後段 (どのレンダリングパスでも共通)。HDR 中間へ加算される
    distortionActive = false;
    if (particles) {
        // M42d: blendMode=2 (distortion) のエミッタが存在するときだけ歪みバッファを
        // クリアして配線する (HDR 経路限定。バッファ未使用フレームのクリアコストを避ける)
        if (hdr != nullptr && hdr->distort.IsValid()) {
            const ComponentTypeId req2[] = { ParticleEmitterComponent::sTypeId };
            bool hasDistortion = false;
            world.ForEachArchetype(req2, [&](Archetype& arch) {
                const int pi = arch.FindTypeIndex(ParticleEmitterComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const auto* p =
                        static_cast<const ParticleEmitterComponent*>(arch.GetPtr(pi, row));
                    if (p->blendMode == 2) {
                        hasDistortion = true;
                        return;
                    }
                }
            });
            if (hasDistortion) {
                const float zero[4] = { 0, 0, 0, 0 };
                device.Context()->ClearRenderTargetView(hdr->distort.RTV(), zero);
                view.distortionRTV = hdr->distort.RTV();
                distortionActive = true;
            }
        }
        // M42a: パーティクル系は深度書込みしない (WriteMask=ZERO) ので read-only DSV に
        // 差し替える。これで深度 SRV (view.depthSRV) との同時バインドが合法になり、
        // ソフトパーティクル (M42b) 等が深度を読める。read-only ビューが無ければ従来どおり
        if (view.dsvReadOnly != nullptr) {
            device.Context()->OMSetRenderTargets(1, &view.rtv, view.dsvReadOnly);
        }
        particles->Render(device, view, shaders, resources);

        // M42e: GPU 深度衝突へシーンカメラの深度を供給する。次 tick の sim が使う =
        // 衝突相手は前フレームの深度 (仕様)。SceneView エディタカメラ (cameraOverride) は
        // 衝突源にしない — ゲームカメラの見た目だけが物理感を持つ
        if (!cameraOverride && view.depthSRV != nullptr) {
            particles->Gpu().SetSceneDepth(view.depthSRV, view.view, view.proj, view.width,
                                           view.height, view.nearZ, view.farZ);
        }
        // M96k: GPU 粒子が読むかき乱しフィールドのテクスチャ。深度と同じく、エディタカメラの描画では更新しない
        // (別の World を描くビューで場が null になり、ゲームビューの値を消すのを避ける)
        if (!cameraOverride) {
            particles->Gpu().SetFieldGpu(view.disturbance, view.disturbanceVolumes);
        }
    }

    // ナビメッシュの半透明の塗り (M82e)。線より先 (塗りの上に輪郭と経路線が乗る)。
    // 空のシーンではパスを Init もしない
    if (navView != nullptr && !navView->FillVertices().empty()) {
        if (!navFillPass_.IsReady()) {
            navFillPass_.Init(device, shaders);
        }
        static_assert(sizeof(DebugFillVertex) == NavFillPass::kVertexStride);
        navFillPass_.Render(device, shaders, navView->FillVertices().data(),
                            static_cast<uint32_t>(navView->FillVertices().size()), navView->FillSerial(), view.rtv,
                            view.dsv, target.width, target.height, view.view, view.proj);
    }

    // スクリプトの DebugDrawLine (v7、M37): シーン空間の線を深度テスト付きで重ねる。
    // ポスプロ解決前 = HDR 中間 (直描き時は最終 RT) に描く。ナビメッシュの輪郭線も同じ線パスで描く
    const bool hasNavLines = navView != nullptr && !navView->Lines().empty();
    const bool hasOccBoxes = f.occlusionBoxes != nullptr && !f.occlusionBoxes->empty();
    if ((debugLines != nullptr && !debugLines->empty()) || hasNavLines || hasOccBoxes) {
        if (!linePass_.IsReady()) {
            linePass_.Init(device, shaders); // 遅延 Init (postFx_ 前例)
        }
        if (linePass_.IsReady()) {
            linePass_.Begin();
            if (hasNavLines) {
                for (const DebugLineCmd& l : navView->Lines()) {
                    linePass_.AddLine({ l.ax, l.ay, l.az }, { l.bx, l.by, l.bz }, l.rgba, false);
                }
            }
            if (debugLines != nullptr) {
                for (const DebugLineCmd& l : *debugLines) {
                    linePass_.AddLine({ l.ax, l.ay, l.az }, { l.bx, l.by, l.bz }, l.rgba, false);
                }
            }
            if (hasOccBoxes) {
                // 隠れていると判定された物なので手前に出す (R=1 G=0.2 B=0.2 A=1)
                constexpr uint32_t kOccludedBoxRgba = 0xFF3333FFu;
                for (const OcclusionDebugBox& b : *f.occlusionBoxes) {
                    linePass_.AddAABB({ b.bmin[0], b.bmin[1], b.bmin[2] }, { b.bmax[0], b.bmax[1], b.bmax[2] },
                                      kOccludedBoxRgba, true);
                }
            }
            linePass_.Render(device, shaders, view.rtv, view.dsv, target.width, target.height,
                             view.view, view.proj);
        }
    }
}

// HDR → LDR 解決 (トーンマップ、CameraPostFx の上書きマージ) と、次フレーム用の viewProj / 描画通番の保存
void RenderSystem::ResolvePost(World& world, GraphicsDevice& device, ShaderManager& shaders,
                               RenderResources& resources, IRenderPath& path, const FrameTarget& target,
                               const CameraOverride* cameraOverride, FrameContext& f)
{
    RenderView& view = f.view;
    PostProcess::Target* hdr = f.hdr;
    const EntityID camEntity = f.camEntity;
    const bool distortionActive = f.distortionActive;
    // HDR → LDR 解決 (トーンマップ)。HDR 中間を使った時のみ。
    // シーンカメラに CameraPostFx があれば上書きマージ (M29e。CameraOverride 経路は
    // エディタ視界なのでグローバル設定のまま)
    if (hdr != nullptr) {
        PostProcess::Settings effective = postFxSettings;
        if (!cameraOverride && !camEntity.IsNull()) {
            if (const auto* pfx = world.GetComponent<CameraPostFxComponent>(camEntity)) {
                effective = MergeCameraPostFx(postFxSettings, *pfx);
            }
        }
        // M94v: PixelPerfectCamera2D がある間は FXAA を掛けない (ドットの輪郭が滲む)
        if (f.pixelPerfect) {
            effective.fxaa = false;
        }
        // M94c: 手触りの画面効果のパルス (ADR-035)。シーンカメラだけ、マージ後の実効値へ加算する
        // (ユーザー設定と CameraPostFx の値は書き換えない)
        if (!cameraOverride && feel.flashActive) {
            ApplyFeelPostFxPulse(effective, feel);
        }
        // M44d: 前フレーム viewProj は path.Render 前に充填済み (view.prevViewProj)。
        // SceneView (cameraOverride) はエディタ操作中のスミアが UX を阻害するため強制 off
        if (cameraOverride) {
            effective.motionBlurIntensity = 0.0f;
        }
        // M57d: フォグ三重計上の 3 つめを降ろす。ゴッドレイ (postfx_godray_mask/blur) は
        // 「遮蔽マスクが空だけ」の**スクリーンスペース近似**で、フロクセルはその上位互換
        // (深度を持つ全ての遮蔽物 + 局所ライト + シャドウアトラス) にあたる。
        // 両方走らせると太陽まわりの散乱を 2 回足すことになるので、フロクセルが
        // 実際に絵へ出たフレームだけ自動で降ろす。**ユーザー設定は書き換えない** —
        // ここで潰すのは「このフレームで使う実効値」だけなので、froxel を切れば戻る
        // ★条件に path.AppliesFroxel() が要る — 積分結果を読まないパスで SRV の有無だけで
        //   判定すると「ゴッドレイだけ消えて霧が増えない」になる
        // 場だけのモードはフロクセルに太陽の散乱が無い (Volume の霧だけ) ので降ろさない
        if (view.froxelSRV != nullptr && path.AppliesFroxel() && !view.froxelFieldOnly) {
            effective.godrayIntensity = 0.0f;
        }
        // M44a: LUT の SRV 解決 (MaterialLibrary の GUID→パス解決と同じ流儀)。
        // 未ロードなら遅延ロード — LUT はデータなので srgb=false (デコード禁止)。
        // 解決できなければ lutSRV=null のまま = Resolve 側で強制 off
        if (effective.lutIntensity > 0.0f && !effective.lutTexture.IsNull()) {
            Texture* lut = resources.textures.Get(effective.lutTexture);
            if (!lut) {
                const std::wstring lutPath = assetguid::ResolvePath(effective.lutTexture.value);
                if (!lutPath.empty()) {
                    lut = LoadTextureRememberingFailure(resources.textures, effective.lutTexture, lutPath,
                                                        /*srgb=*/false, lutLoadFailed_);
                }
            }
            if (lut && lut->srv) {
                effective.lutSRV = lut->srv.Get();
            }
        }
        // M78c/d: fxStack からこのビューのユーザーポスト／コンピュートを更新する。
        // ランナーは viewKey 毎 (Scene View と Game View が互いのパスを消し合わない)。
        // 実効 fxStack が null (Scene View / fxStack を持たないカメラ / 解決不能) ならパスを消す
        const uint32_t fxKey = (target.viewKey < 4) ? target.viewKey : 0u;
        ProjectEffectRunner&  effectRunner  = projectEffectRunner_[fxKey];
        ProjectComputeRunner& computeRunner = projectComputeRunner_[fxKey];
        const CameraPostFxComponent* camPostFx =
            camEntity.IsNull() ? nullptr : world.GetComponent<CameraPostFxComponent>(camEntity);
        const AssetID requestedFx = camPostFx ? camPostFx->fxStack : AssetID{};
        const std::wstring fxPath =
            requestedFx.IsNull() ? std::wstring() : assetguid::ResolvePath(requestedFx.value);
        const AssetID fxId = EffectiveFxStack(cameraOverride != nullptr, camPostFx != nullptr,
                                              requestedFx, !fxPath.empty());
        if (fxId.IsNull()) {
            if (!loadedFxStackId_[fxKey].IsNull()) {
                effectRunner.ClearPasses();
                computeRunner.ClearPasses();
                loadedFxStackId_[fxKey]    = {};
                loadedFxStackStamp_[fxKey] = 0;
            }
        } else {
            // Tex2D リゾルバをフレームごとに設定 (resources の参照は ResolvePost が生きている間有効)
            effectRunner.SetTextureResolver(
                [&resources, &computeRunner](const std::string& name) -> ID3D11ShaderResourceView*
                {
                    // M78d: コンピュートパス出力 SRV を shader 名で先引き (§3 GetOutputSRV 接続)
                    // 例: fxstack.json で _Mask = "MySim.cs" と書いた場合、MySim.cs の出力 UAV が
                    // そのまま SRV としてポストの t2 以降に渡る (fill CS → ポスト表示の手動手順参照)。
                    if (!name.empty())
                    {
                        if (ID3D11ShaderResourceView* srv = computeRunner.GetOutputSRV(name))
                            return srv;
                    }
                    // ビルトイン名またはフォールバック → white
                    auto getWhite = [&]() -> ID3D11ShaderResourceView* {
                        Texture* t = resources.textures.Get(resources.textures.White());
                        return t ? t->srv.Get() : nullptr;
                    };
                    if (name.empty() || name == "white") {
                        return getWhite();
                    }
                    if (name == "gray" || name == "black" || name == "bump") {
                        // spec §4.1: 専用 SRV 未整備時は white ＋ WARN (docs/project-shaders-tex2d-defaults.md)
                        static bool warnedGray = false;
                        static bool warnedBlack = false;
                        static bool warnedBump = false;
                        if (name == "gray" && !warnedGray) {
                            warnedGray = true;
                            MYE_LOG_WARN(
                                "RenderSystem: Tex2D 既定 'gray' は専用 SRV 未整備のため white にフォールバックします");
                        } else if (name == "black" && !warnedBlack) {
                            warnedBlack = true;
                            MYE_LOG_WARN(
                                "RenderSystem: Tex2D 既定 'black' は専用 SRV 未整備のため white にフォールバックします");
                        } else if (name == "bump" && !warnedBump) {
                            warnedBump = true;
                            MYE_LOG_WARN(
                                "RenderSystem: Tex2D 既定 'bump' は専用 SRV 未整備のため white にフォールバックします");
                        }
                        return getWhite();
                    }
                    // GUID hex 文字列として解決
                    char* endp = nullptr;
                    const unsigned long long val = strtoull(name.c_str(), &endp, 16);
                    if (val != 0 && endp && *endp == '\0') {
                        Texture* t = resources.textures.Get(AssetID{ static_cast<uint64_t>(val) });
                        if (t && t->srv) {
                            return t->srv.Get();
                        }
                        // 未ロードなら非同期ロードをキック
                        const std::wstring texPath = assetguid::ResolvePath(static_cast<uint64_t>(val));
                        if (!texPath.empty()) {
                            resources.textures.RequestLoadFileAsync(texPath);
                        }
                    }
                    return getWhite();
                });

            // fxstack.json は ID が変わったかファイルが書き換わったときだけ読む
            // (エディタで保存した値は更新時刻の変化で拾う。同期読み込みを毎フレーム走らせない)
            std::error_code stampEc;
            const auto writeTime = std::filesystem::last_write_time(fxPath, stampEc);
            const int64_t stamp = stampEc ? 0 : static_cast<int64_t>(writeTime.time_since_epoch().count());
            if (NeedsFxStackReload(loadedFxStackId_[fxKey], loadedFxStackStamp_[fxKey], fxId, stamp)) {
                FxStackAsset fx;
                std::string errMsg;
                if (LoadFxStack(fxPath, fx, &errMsg)) {
                    // M78c: Post パスを ProjectEffectRunner へ流す
                    std::vector<ProjectPostPassDesc> postDescs;
                    postDescs.reserve(fx.passes.size());
                    // M78d: Compute パスを ProjectComputeRunner へ流す
                    std::vector<ProjectComputePassDesc> compDescs;
                    compDescs.reserve(fx.passes.size());

                    for (const auto& entry : fx.passes) {
                        if (entry.kind == FxStackKind::Post) {
                            ProjectPostPassDesc d;
                            d.shaderName     = entry.shader;
                            d.insertion      = entry.insertion;
                            d.priority       = entry.priority;
                            d.enabled        = entry.enabled;
                            d.propertyValues = entry.properties;
                            postDescs.push_back(std::move(d));
                        } else if (entry.kind == FxStackKind::Compute) {
                            ProjectComputePassDesc d;
                            d.shader        = entry.shader;
                            d.dispatchPoint = DispatchPointFromString(entry.dispatchPoint);
                            d.priority      = entry.priority;
                            d.enabled       = entry.enabled;
                            d.propertyValues = entry.properties;
                            compDescs.push_back(std::move(d));
                        }
                    }
                    // SetPasses は同名・同挿入点のキャッシュを保つので、値だけの変更では CB も UAV も作り直さない
                    effectRunner.SetPasses(std::move(postDescs));
                    computeRunner.SetPasses(std::move(compDescs));
                } else {
                    MYE_LOG_WARN("RenderSystem: fxstack ロード失敗 (%s): %s",
                                 WideToUtf8(fxPath).c_str(), errMsg.c_str());
                    effectRunner.ClearPasses();
                    computeRunner.ClearPasses();
                }
                loadedFxStackId_[fxKey]    = fxId;
                loadedFxStackStamp_[fxKey] = stamp;
            }
        }

        ProjectEffectRunner*  injectPostFx = nullptr;
        ProjectComputeRunner* injectComputeFx = nullptr;
        if (ShouldInjectProjectFxStack(cameraOverride != nullptr)) {
            injectPostFx     = &effectRunner;
            injectComputeFx  = &computeRunner;
        }

        // M78d: BeforePost コンピュートを Resolve 前に実行する (HDR 描画完了直後)
        if (injectComputeFx != nullptr
            && computeRunner.HasPasses(ComputeDispatchPoint::BeforePost))
        {
            computeRunner.RunDispatch(ComputeDispatchPoint::BeforePost,
                                      device, shaders,
                                      hdr->scene.SRV(), view.depthSRV,
                                      target.width, target.height);
        }

        postFx_.Resolve(device, shaders, *hdr, target.rtv, target.width, target.height,
                        effective, view, distortionActive,
                        injectPostFx,     // M78c: ユーザーポスト (Scene View は nullptr)
                        injectComputeFx); // M78d: ユーザーコンピュート (Scene View は nullptr)
    }
    // M44d: 次フレームのモーションブラー用に viewProj を保存 (viewKey=0 = AssetPreview は対象外)。
    // M46d: カメラ位置と描画通番も同じ場所で更新する (再投影とテンポラル履歴の連続性判定)。
    // ★M55b: ここに保存するのは **非ジッタ側** (projNoJitter)。この 1 箇所を
    // RtPasses (M46d) と PostProcess::RunMotionBlur (M44d) の **両方** が読むので、
    // ジッタ付きを入れると RT テンポラルとモーションブラーが同時に壊れる
    if (target.viewKey > 0 && target.viewKey < 4) {
        PrevViewProj& p = prevVP_[target.viewKey];
        XMStoreFloat4x4(&p.m, XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.projNoJitter));
        p.pos = view.cameraPos;
        p.w = target.width;
        p.h = target.height;
        p.valid = true;
        ++viewSerial_[target.viewKey];
    }
}

void RenderSystem::ReleaseGpu()
{
    // 遅延 Init のパス群は既定構築した実体へ差し替える = Init 前の状態へ戻り、ComPtr が解放される。
    // 設定値 (public のフィールド) と CPU 側の履歴 (prevVP_ 等) は触らない。ただし LOD の段とパレットの履歴は捨てる
    postFx_ = PostProcess();
    shadowPass_ = ShadowPass();
    shadowAtlas_ = ShadowAtlas();
    frameTimer_ = GpuTimer();
    frameTimerInit_ = false;
    froxelPass_ = FroxelPass();
    acousticPass_ = AcousticVolumePass();
    disturbancePass_ = DisturbanceFieldPass();
    disturbanceVolumePass_ = DisturbanceVolumePass();
    linePass_ = EditorLinePass();
    navFillPass_ = NavFillPass();
    sprite2dPass_ = Sprite2DPass();
    pixelPerfectRt_ = RenderTexture();
    pixelPerfectBlit_ = PixelPerfectBlitPass();
    envBaker_ = EnvMapBaker();
    rtScene_ = RtScene();
    rtPasses_ = RtPasses();
    terrainSystem_.Clear();
    terrainList_ = TerrainDrawList();
    waterData_ = WaterDrawData();
    decalList_ = DecalDrawList();
    for (ProjectEffectRunner& r : projectEffectRunner_) {
        r = ProjectEffectRunner();
    }
    for (ProjectComputeRunner& r : projectComputeRunner_) {
        r = ProjectComputeRunner();
    }
    for (int i = 0; i < 4; ++i) {
        loadedFxStackId_[i] = {};
        loadedFxStackStamp_[i] = 0;
    }
    skyLoadFailed_ = FailedTextureLoad();
    lutLoadFailed_ = FailedTextureLoad();
    ResetRenderHistory();
}

} // namespace mye
