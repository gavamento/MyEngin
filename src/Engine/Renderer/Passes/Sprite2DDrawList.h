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
    uint32_t start = 0;
    uint32_t count = 0;
};

// 描画順に並んだ三角形リスト (1 枚 = 6 頂点)。バッチが 0 件なら描画パスは何もしない
struct Sprite2DDrawList {
    std::vector<Sprite2DVertex> vertices;
    std::vector<Sprite2DBatch> batches;

    void Clear()
    {
        vertices.clear();
        batches.clear();
    }
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
