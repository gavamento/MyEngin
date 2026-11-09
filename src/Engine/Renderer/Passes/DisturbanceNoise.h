//====================================================================================
//                          DisturbanceNoise.h
//  MyEngin/ 秋田蓮音                                                       11/09/2026
//                                          かき乱しフィールドの霧のノイズ (数値 / マップ) の転送指示と CB の詰め方
//====================================================================================
#pragma once
#include <cstdint>

#include <d3d11.h>

namespace mye {

// 数値のノイズの格子の周期 [格子]。HLSL の MYE_DFLD_NOISE_PERIOD と一致 (規則 9)。
// 座標のずらし (流れ × 時刻) をこの周期で折り返すので、長く回しても float の桁が落ちない
constexpr int kDisturbanceNoisePeriod = 256;
// 重ね数の上限 (HLSL のループの上限 MYE_DFLD_NOISE_MAX_OCTAVES と一致。規則 9)
constexpr int kDisturbanceNoiseMaxOctaves = 6;

// 霧のノイズの設定 (DisturbanceFieldComponent の noise* をそのまま写したもの)。見た目だけで sim は読まない。
// 2D 層と Volume で同じものを使う
struct DisturbanceNoiseUpload {
    float strength = 0.0f;   // 数値のノイズの強さ [0,1] (0 = 掛けない)
    float scale = 4.0f;      // [m] 模様の大きさ
    int32_t octaves = 3;     // 重ね数 [1, kDisturbanceNoiseMaxOctaves]
    float evolve = 0.0f;     // [1/s] 形が変わる速さ
    float scroll[3] = {};    // [m/s] 模様が流れる速度
    float mapStrength = 0.0f; // マップのノイズの強さ [0,1] (0 = 掛けない)
    uint64_t mapAsset = 0;    // マップの AssetID (0 = なし)。RenderSystem が mapSRV へ解決する
    float mapScale = 8.0f;    // [m] マップ 1 枚が覆う辺
    float mapScroll[2] = {};  // [m/s] マップが流れる速度 (x, z)
    ID3D11ShaderResourceView* mapSRV = nullptr; // 解決できなければ null (マップのノイズは掛からない)
};

// HLSL の DisturbanceNoise (float4 x 3) と同じ並び。層の CB と Volume の CB の末尾に 1 個ずつ載る
struct DisturbanceNoiseCB {
    float a[4] = {}; // xyz = 数値のノイズの座標のずらし [格子] (流れ × 時刻 / 大きさ、周期で折り返す), w = 1 / 大きさ [1/m]
    float b[4] = {}; // x = 数値のノイズの強さ (0 = 掛けない), y = 重ね数, z = 形の変化の位相 [格子], w = マップの強さ (0 = 掛けない)
    float c[4] = {}; // xy = マップの UV のずらし ([0,1) へ折り返す), z = 1 / マップの大きさ [1/m], w = 1: マップを張った
};
static_assert(sizeof(DisturbanceNoiseCB) == 48, "HLSL の DisturbanceNoise と一致させること");

// 設定と時刻 [s] から CB を組む (純粋関数。同じ入力から同じ値)。非有限・範囲外の値は既定へ戻すか切る。
// 強さが 0 (または非有限) のノイズは CB でも 0 = シェーダが読まない = 従来の絵とビット一致する。
// マップは mapSRV が null なら強さ 0 として扱う
DisturbanceNoiseCB PackDisturbanceNoise(const DisturbanceNoiseUpload& n, double timeSeconds);

} // namespace mye
