//====================================================================================
//                          Sprite2DDrawList.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D の描画リスト (収集側と描画パスの受け渡し)
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

// 動的 VB の 1 頂点 (36 バイト)。sprite2d.hlsl の VSIn と一致させる
struct Sprite2DVertex {
    float pos[3];   // ワールド座標
    float uv[2];
    float color[4]; // 頂点色 (Sprite2D.color)
};
static_assert(sizeof(Sprite2DVertex) == 36, "sprite2d.hlsl の VSIn と合わせる");

// バッチのステンシル動作。マスクは色を書かず ref 1 を書き、Inside / Outside は書かれた値との一致でテストする
namespace sprite2dstencil {
inline constexpr uint8_t kNone = 0;        // ステンシルを使わない
inline constexpr uint8_t kWriteMask = 1;   // alpha >= cutoff の画素にステンシル 1 を書く (色なし・深度テストなし)
inline constexpr uint8_t kTestInside = 2;  // ステンシル == 1 の画素だけ描く
inline constexpr uint8_t kTestOutside = 3; // ステンシル == 0 の画素だけ描く
} // namespace sprite2dstencil

// 同じテクスチャ・同じサンプラで続く頂点の区間。テクスチャは AssetID で持ち、SRV への解決は描画パスが行う
// (収集側 = Engine 層に D3D の型を出さないため)
struct Sprite2DBatch {
    AssetID texture;
    bool pointFilter = false; // false = linear
    uint8_t stencil = 0;      // Sprite2DStencil
    float alphaCutoff = 0.0f; // stencil == kWriteMask のときだけ使う (alpha がこれ未満の画素は書かない)
    bool lit = false;         // true = ライトバッファを乗算する (属する LightGroup が anyLit のときだけ効く)
    uint32_t start = 0;
    uint32_t count = 0;
};

// ---- 2D ライト ----
// ライトの形状は Engine 側でワールド座標へ展開済み。Renderer 層は頂点を描くだけで sim もシーンも見ない
namespace light2dshape {
inline constexpr uint8_t kSpot = 1;     // 外半径の外接クアッド。放射 / 角度の減衰は PS が計算する
inline constexpr uint8_t kFreeform = 2; // 三角形列。edge (1 = 内側、0 = 外縁) がそのまま明るさ
inline constexpr uint8_t kSprite = 3;   // テクスチャ付きクアッド
} // namespace light2dshape

struct Light2DVertex {
    float pos[2]; // ワールド XY
    float uv[2];
    float edge;   // Freeform の外縁グラデーション (1 = 内側、0 = 外縁)。他の形状は 1
};
static_assert(sizeof(Light2DVertex) == 20, "sprite2d_light.hlsl の VSIn と合わせる");

// ライト 1 個。color は rgb × intensity 済み。Global はここに入らず、群の globalColor へ足し込む
struct Light2DInstance {
    uint8_t shape = 0;
    float color[3] = {};
    float z = 0.0f;           // 描くワールド Z (ライトのエンティティ位置)
    // Spot
    float center[2] = {};
    float dir[2] = { 1.0f, 0.0f };
    float innerRadius = 0.0f;
    float outerRadius = 1.0f;
    float falloff = 1.0f;
    float useAngle = 0.0f;    // 1 = 扇形、0 = 全周
    float cosOuter = -1.0f;
    float invCosRange = 0.0f; // 1 / (cosInner - cosOuter)
    // Freeform / Sprite / Spot の頂点列 (lightVertices の区間)
    uint32_t vertexStart = 0;
    uint32_t vertexCount = 0;
    AssetID texture;          // Sprite
    // 影・法線マップ。center は形状に依らずライトの位置 (法線のライト方向)
    bool shadows = false;
    float shadowStrength = 1.0f;
    float normalMapDistance = 1.0f; // ライトの平面からの高さ (法線の N·L の z)
};

// ---- 2D 影 / 法線マップ ----
// キャスター 1 個: shadowPoints の区間が輪郭 (ワールド XY、向きは任意)。selfShadow == false のときだけ
// shadowFill の区間 (輪郭を三角形に割ったもの) を持ち、影を描いた後に 0 で描き戻して自分を明るく保つ
struct Shadow2DPoint {
    float x = 0.0f;
    float y = 0.0f;
};
struct Shadow2DCaster {
    uint32_t pointStart = 0;
    uint32_t pointCount = 0;
    uint32_t fillStart = 0;  // shadowFill の区間 (3 点 1 組)
    uint32_t fillCount = 0;  // 点の数。selfShadow == true なら 0
};

// 法線 RT へ描くバッチ。頂点は Sprite2DDrawList::normalVertices の区間 (sprite2d の頂点と同じ形)。
// 頂点色は法線の 2x2 変換 (rgba = m11 m12 m21 m22: 回転と flip を畳んだもの)。
// normalMap が空 = 法線マップを持たない lit スプライト (平らな法線 (0,0,1) で描き、奥の法線を隠す)
struct Sprite2DNormalBatch {
    AssetID texture;   // 色のテクスチャ (アルファだけ使う)
    AssetID normalMap;
    bool pointFilter = false;
    uint32_t start = 0;
    uint32_t count = 0;
};

// 同じライト集合で描くバッチの区間。群ごとにライトバッファを作り直す (先頭バッチ = firstBatch、次の群の先頭まで)
struct Sprite2DLightGroup {
    uint32_t firstBatch = 0;
    uint32_t lightStart = 0;   // groupLights の区間
    uint32_t lightCount = 0;
    float globalColor[3] = {}; // 群の Global ライトの合計 (ライトバッファのクリア色)
    bool anyLit = false;       // lit なバッチを含む (含まない群はバッファを作らない)
};

// 描画順に並んだ三角形リスト (1 枚 = 6 頂点)。バッチが 0 件なら描画パスは何もしない。
// lightGroups が空 = シーンに Light2D が無い (presence gate)。従来どおりライトバッファを作らない
struct Sprite2DDrawList {
    std::vector<Sprite2DVertex> vertices;
    std::vector<Sprite2DBatch> batches;
    std::vector<Light2DInstance> lights;
    std::vector<Light2DVertex> lightVertices;
    std::vector<uint32_t> groupLights; // 群が使う lights の添字
    std::vector<Sprite2DLightGroup> lightGroups;
    // 影を落とすライトがあるシーンだけ埋まる (presence gate)。法線マップ付きの lit スプライトがあるシーンだけ
    // normalBatches が空でなくなる
    std::vector<Shadow2DCaster> shadowCasters;
    std::vector<Shadow2DPoint> shadowPoints;
    std::vector<Shadow2DPoint> shadowFill;
    std::vector<Sprite2DVertex> normalVertices;
    std::vector<Sprite2DNormalBatch> normalBatches;

    void Clear()
    {
        vertices.clear();
        batches.clear();
        lights.clear();
        lightVertices.clear();
        groupLights.clear();
        lightGroups.clear();
        shadowCasters.clear();
        shadowPoints.clear();
        shadowFill.clear();
        normalVertices.clear();
        normalBatches.clear();
    }
    bool HasLights() const { return !lightGroups.empty(); }
    bool HasNormals() const { return !normalBatches.empty(); }
    bool Empty() const { return batches.empty(); }
    // ステンシルを使うバッチがあるか (無ければ描画パスはステンシルのクリアも DSV の差し替えもしない)
    bool UsesStencil() const
    {
        for (const Sprite2DBatch& b : batches) {
            if (b.stencil != sprite2dstencil::kNone) {
                return true;
            }
        }
        return false;
    }
};

} // namespace mye
