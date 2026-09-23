//====================================================================================
//                          SurfaceShaderTypes.h
//  MyEngin/ 秋田蓮音                                                     09/24/2026
//                                          サーフェスシェーダー予約 CB の C++ 正本
//====================================================================================
#pragma once
#include <cstdint>

#include <DirectXMath.h>

#include "Engine/Core/WaveMath.h"
#include "Engine/Renderer/RenderTypes.h"

namespace mye {

// ---- MyEnginePerFrame ----
// 内容は forward_lit.hlsl / deferred_gbuffer.hlsl の PerFrame と同じ (カメラ位置・環境光・
// ライト配列・CSM・霧・IBL・シャドウアトラス・フロクセル・音響)。
// ★先頭の gViewProj (フレームの camera VP) だけは持たない —
//   サーフェス側は位置に効く行列を include の static gViewProj (MyEngineSurfaceEntries.hlsli
//   が生成エントリごとに代入する) が担うため、同名で cbuffer に置くと再宣言エラーになる。
//   それ以外のフィールドは既存 PerFrame と同順・同型 (ForwardPath.cpp の PerFrameCB 参照)。
// このため既存 PerFrameCB とはバイト単位で同一ではない (先頭 64 バイトが無い) —
// 「使い回し」は GPU バッファの共有ではなく、内容 (RenderView から詰める式) の共有を指す。
struct MyEnginePerFrameCB {
    DirectX::XMFLOAT3 cameraPos;
    int32_t lightCount;
    DirectX::XMFLOAT3 ambient;
    float pad0;
    GpuLight lights[kMaxLights];
    DirectX::XMFLOAT4X4 shadowVP; // transpose(lightView*lightProj) カスケード 0
    float shadowTexel;
    int32_t shadowEnabled;
    float pad1[2];
    DirectX::XMFLOAT3 fogColor;
    int32_t fogMode; // -1=無効
    float fogDensity;
    float fogStart;
    float fogEnd;
    float fogPad;
    int32_t iblEnabled;
    float iblSpecMips;
    float iblPad[2];
    DirectX::XMFLOAT4X4 shadowVP12[2]; // カスケード 1,2
    float cascadeInfo[4];
    float fogHeightFalloff;
    float fogBaseHeight;
    float fogInscatterIntensity;
    float fogInscatterPower;
    DirectX::XMFLOAT3 sunDirection;
    float fogPad2;
    DirectX::XMFLOAT3 sunColor;
    float fogPad3;
    int32_t shadowAtlasEnabled;
    float shadowAtlasTexel;
    float atlasPad[2];
    ShadowTileCB shadowTiles[kMaxShadowTiles];
    FroxelForwardCB froxel;
    AcousticCB acoustic;
};

// ---- MyEngineSurfaceFrame ----
// TAA の速度・CSM 影のために生成エントリが static (gViewProj/gWorld/gTime/gWaterTime) へ
// 代入する値の出所。今/前の 2 時刻を両方持つのは速度エントリが VSMain を 2 回評価するため
struct MyEngineSurfaceFrameCB {
    DirectX::XMFLOAT4X4 curViewProj;    // ジッタ込み (色・速度の「今」)
    DirectX::XMFLOAT4X4 prevViewProj;   // 非ジッタ (速度の「前」)
    DirectX::XMFLOAT4X4 shadowViewProj; // CSM: 描画中カスケードのライト VP (影エントリ専用)
    float curTime;
    float prevTime;
    float curWaterTime;
    float prevWaterTime;
    DirectX::XMFLOAT2 jitterNdc;
    DirectX::XMFLOAT2 screenSize;
    int32_t historyValid; // 0 = 前フレーム履歴なし (速度エントリは 0 を書く)
    float pad[3];
};

// ---- MyEnginePerObject ----
struct MyEnginePerObjectCB {
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4X4 prevWorld;
    DirectX::XMFLOAT4 baseColor; // マテリアル baseColor のリニア値
};

// ---- MyEngineWater (sub-05 で配線。CB の形だけここで確定させる) ----
// WaterWaveComponent (Components.h) の波パラメータをそのまま渡す。無効時は全 0
struct MyEngineWaterCB {
    int32_t enabled = 0;
    float baseHeight = 0.0f;
    float overallScale = 0.0f;
    int32_t waveCount = 0;
    GerstnerWave waves[4];
    DirectX::XMFLOAT4 deepColor;
    DirectX::XMFLOAT4 shallowColor;
};

// ---- 予約名 (D3DReflect の名前解決に使う。cbuffer 名は宣言側 (MyEngineSurface.hlsli) と
//      1 対 1 で一致させること) ----
namespace surface {
inline constexpr const char* kPerFrameCB = "MyEnginePerFrame";
inline constexpr const char* kSurfaceFrameCB = "MyEngineSurfaceFrame";
inline constexpr const char* kPerObjectCB = "MyEnginePerObject";
inline constexpr const char* kWaterCB = "MyEngineWater";
inline constexpr const char* kPerMaterialCB = "MyEnginePerMaterial"; // 作者宣言 (register なし)
} // namespace surface

} // namespace mye
