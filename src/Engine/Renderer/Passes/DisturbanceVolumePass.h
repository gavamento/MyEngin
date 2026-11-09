//====================================================================================
//                          DisturbanceVolumePass.h
//  MyEngin/ 秋田蓮音                                                       11/09/2026
//                                          かき乱しフィールド (M96) の Volume (3D) を GPU の Texture3D へ転送する
//====================================================================================
#pragma once
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Renderer/Passes/DisturbanceNoise.h"

namespace mye {

class GraphicsDevice;

// 同時に描ける Volume の数 (assets\shaders\disturbance_volume.hlsli の MYE_DFLD_VOL_MAX と一致。規則 9)。
// Texture3D は配列にできないので、1 個ごとに別のテクスチャを持つ
constexpr int kDisturbanceMaxVolumes = 4;
// Volume の CB 全体のバイト数 (同 MYE_DFLD_VOL_CB_BYTES と一致。規則 9)
constexpr int kDisturbanceVolumeCbBytes = 464;
// フロクセル注入 (CS) の束縛番号。HLSL の MYE_DFLD_VOL_SRV (先頭。続く 3 本が連番) / MYE_DFLD_VOL_CB と一致 (規則 9)
constexpr int kDisturbanceVolumeSrvSlot = 3;
constexpr int kDisturbanceVolumeCbSlot = 2;
// 霧のノイズのマップ (Volume ごとに 1 枚、4 本連番) と、繰り返して敷くサンプラ (wrap)。フロクセル注入 (CS) の番号。
// HLSL の MYE_DFLD_VOL_SRV_NOISE_MAP / MYE_DFLD_VOL_NOISE_SAMPLER と一致 (規則 9)
constexpr int kDisturbanceVolumeSrvNoiseMapSlot = 7;
constexpr int kDisturbanceVolumeNoiseSamplerSlot = 2;

// Volume 1 個の転送指示。Engine 層が sim の状態 (または編集中の設定) から詰める。
// ★DisturbanceState を直接受け取らない (DisturbanceLayerUpload と同じ。Renderer から Engine を参照しない)
struct DisturbanceVolumeEntryUpload {
    // 状態が無い (編集中・Play 前) とき false。配列は読まず、restDensity の定数として描く
    bool hasState = false;
    int32_t cellsX = 0;
    int32_t cellsY = 0;
    int32_t cellsZ = 0;
    // 各 cellsX * cellsY * cellsZ 個、index = x + cellsX * (z + cellsZ * y)。hasState のときだけ読む
    const float* flowX = nullptr;
    const float* flowY = nullptr;
    const float* flowZ = nullptr;
    const float* density = nullptr; // null = 濃さチャネルなし
    int32_t originCell[3] = {};
    float cellSize = 0.5f;
    float restDensity = 1.0f;
    bool hasDensity = true;
    float fogDensity = 2.0f;
    float fogColor[3] = { 1.0f, 1.0f, 1.0f };
    float fogBrightness = 1.0f;
    bool fogSunLight = false; // true: 平行光をこの Volume の霧に当てる (影なし)
    bool autoFroxel = true;   // true: カメラのフロクセルが off でもこの霧のために回す
    DisturbanceNoiseUpload noise; // 霧のノイズ (見た目だけ。既定 = 掛けない)
    uint32_t entityIndex = 0;
    uint32_t entityGeneration = 0;
};

struct DisturbanceVolumeUpload {
    int32_t count = 0;
    DisturbanceVolumeEntryUpload volumes[kDisturbanceMaxVolumes];
    // sim の通番。前回と同じで構成も同じなら転送しない (sim は 60 Hz、描画はそれより速く回る)
    uint64_t serial = 0;
    // ノイズの時計 [s] (DisturbanceFieldUpload::noiseTime と同じ値)
    double noiseTime = 0.0;
};

// HLSL の DisturbanceVolume (float4 x 7 = 4 + ノイズの 3) と同じ並び
struct DisturbanceVolumeEntryCB {
    float originSize[4] = {}; // xyz = 原点セル (整数), w = cellSize [m]
    float extent[4] = {};     // xyz = cellsX/Y/Z, w = 1: テクスチャを読む / 0: restDensity の定数
    float look[4] = {};       // x = fogDensity [1/m], y = fogBrightness, z = restDensity, w = 1: 濃さチャネルあり
    float fogColor[4] = {};   // rgb = 霧の色, w = 1: 平行光を当てる (fogSunLight)
    DisturbanceNoiseCB noise; // 霧のノイズ (float4 x 3。DisturbanceNoise.h)
};

// HLSL の cbuffer DisturbanceVolumeCB と同じ並び
struct DisturbanceVolumeCB {
    uint32_t header[4] = {}; // x = 有効な Volume の数
    DisturbanceVolumeEntryCB volumes[kDisturbanceMaxVolumes];
};
static_assert(sizeof(DisturbanceVolumeCB) == kDisturbanceVolumeCbBytes, "HLSL の DisturbanceVolumeCB と一致させること");

// 描き手 (フロクセル注入) へ渡す束。SRV は null のことがある (編集中の定数の Volume)。
// 読む側は CB の個数と、各 Volume の extent.w (テクスチャを読むか) を見る
struct DisturbanceVolumeGpu {
    ID3D11ShaderResourceView* srv[kDisturbanceMaxVolumes] = {}; // Texture3D RGBA16F (vx, vy, vz, 濃さ)。幅 = cellsX, 高さ = cellsZ, 奥行き = cellsY
    DisturbanceVolumeCB cb;
    // 霧を出す (濃さチャネルあり && fogDensity > 0) autoFroxel な Volume が 1 つでもあるか。
    // true なら、カメラのフロクセルが off のビューでも「場だけのモード」で回す
    bool wantsAutoFroxel = false;
    // Volume ごとの霧のノイズのマップ (null = その Volume はマップなし。CB の noise.c[3] と対応)
    ID3D11ShaderResourceView* noiseMapSRV[kDisturbanceMaxVolumes] = {};
};

// Volume の Texture3D の所有者。GPU は読むだけ — sim の状態は CPU (決定論) が持つ
class DisturbanceVolumePass {
public:
    // 転送して束を作る。戻り値 = 束が有効か (false = Volume が 0 個、またはテクスチャを作れなかった)
    bool Upload(GraphicsDevice& device, const DisturbanceVolumeUpload& src);
    void Release();

    const DisturbanceVolumeGpu& Gpu() const { return gpu_; }
    // 直近の転送にかかった CPU 時間 [ms]。転送を省いたフレームは 0
    float LastUploadMs() const { return uploadMs_; }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture3D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int32_t width = 0;  // cellsX
        int32_t height = 0; // cellsZ
        int32_t depth = 0;  // cellsY
        // 最後に転送した内容の鍵
        uint32_t entityIndex = 0;
        uint32_t entityGeneration = 0;
        uint64_t serial = 0;
        bool uploaded = false;
        bool createFailed = false; // 同じ寸法で一度落ちたら毎フレーム再試行してログを溢れさせない
    };
    bool EnsureTexture(GraphicsDevice& device, Slot& slot, int32_t width, int32_t height, int32_t depth);

    Slot slots_[kDisturbanceMaxVolumes];
    DisturbanceVolumeGpu gpu_;
    float uploadMs_ = 0.0f;
};

} // namespace mye
