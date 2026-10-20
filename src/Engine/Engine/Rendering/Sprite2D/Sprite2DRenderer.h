//====================================================================================
//                          Sprite2DRenderer.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D の収集とソート (描画リストを作る)
//====================================================================================
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Renderer/Passes/Sprite2DDrawList.h"

namespace mye {

class World;
struct RenderResources;
struct Sprite2DComponent;
struct SpriteMask2DComponent;
struct SpriteDef;

// World の active な Sprite2D / SpriteMask2D を集め、SpriteDef を解決して三角形リストに展開し、
// 整数のソートキーで並べた描画リストを作る。描画は Renderer 層の Sprite2DPass。
// ソートキー = (外側の層順, order, index, 内側の層順, order, index) の辞書式で、安定ソート。viewZ は見ない。
// SortingGroup2D の子孫は外側 = 最も近い祖先グループ、内側 = 自分。グループ無しは外側 = 自分、内側 = 0。
// 描画専用で sim の入力ではない (ハッシュ対象外)。メインスレッド専用
class Sprite2DRenderer {
public:
    // Sprite2D が無ければ空の描画リストを返す。返した参照は次の Collect まで有効
    const Sprite2DDrawList& Collect(World& world, RenderResources& resources);

    // pivot を原点にしたローカル矩形 {left, bottom, right, top} (ワールド単位、flip 適用前)。
    // 描画と Scene View のギズモが同じ寸法を使うための 1 本。texWidth / texHeight は全面スプライトのときだけ使う
    static void LocalRect(const SpriteDef& def, int texWidth, int texHeight, float out[4]);

    // Tiled / Sliced が 1 スプライトに出す頂点数の上限 (超えた分は切って警告を 1 回出す)
    static constexpr uint32_t kMaxSpriteVertices = 4096;

    // Sliced / Tiled の分割結果 1 枚。ローカル座標 (pivot が原点、y 上向き) と正規化 UV ((u0, v0) が左上)
    struct SliceQuad {
        float x0, y0, x1, y1;
        float u0, v0, u1, v1;
    };
    // 分割への入力。rect / border は画素 (rect = x, y, w, h 左上原点、border = 左, 下, 右, 上)
    struct SliceInput {
        float rect[4] = {};
        float border[4] = {};
        float pivotX = 0.5f;
        float pivotY = 0.5f;
        float ppu = 100.0f;
        float texW = 1.0f;
        float texH = 1.0f;
        float sizeX = 1.0f; // ワールド寸法
        float sizeY = 1.0f;
        bool tiled = false; // false = Sliced
    };
    // 3x3 に割ったクアッド列を作る (GPU に依らない純関数)。頂点数が kMaxSpriteVertices を超える分は切り、切ったら true
    static bool BuildSliceQuads(const SliceInput& in, std::vector<SliceQuad>& out);

private:
    struct Item {
        int32_t outerLayer = 0;
        int32_t outerOrder = 0;
        uint32_t outerIndex = 0;
        int32_t innerLayer = 0;
        int32_t innerOrder = 0;
        uint32_t innerIndex = 0;
        bool isMask = false;
        const Sprite2DComponent* sprite = nullptr;
        const SpriteMask2DComponent* mask = nullptr;
        const DirectX::XMFLOAT4X4* world = nullptr;
    };

    // 解決済みの 1 スプライト (SpriteDef + テクスチャ寸法)。矩形・border は画素、左上原点
    struct Resolved {
        AssetID texture;
        float texW = 1.0f;
        float texH = 1.0f;
        float rect[4] = {}; // x, y, w, h
        float border[4] = {}; // 左, 下, 右, 上
        float pivot[2] = { 0.5f, 0.5f };
        float ppu = 100.0f;
        float local[4] = {}; // Simple の pivot 原点ローカル矩形 {left, bottom, right, top}
        bool pointFilter = false;
        bool missing = false;
    };

    AssetID EnsureMissingTexture(RenderResources& resources);
    bool Resolve(AssetID sprite, RenderResources& resources, bool allowMissing, Resolved& out);
    void EmitSprite(const Item& item, RenderResources& resources);
    void EmitMask(const Item& item, RenderResources& resources);
    void AddQuad(const DirectX::XMFLOAT4X4& m, float flipX, float flipY, const float color[4], float x0, float y0,
                 float x1, float y1, float u0, float v0, float u1, float v1);
    void Push(AssetID texture, bool pointFilter, uint8_t stencil, float alphaCutoff, const Sprite2DVertex* v,
              uint32_t count);

    AssetID missingTexture_ = {}; // 内蔵 16x16 マゼンタ (未解決のスプライト用。必要になったときに作る)
    bool warnedVertexCap_ = false;
    bool capped_ = false; // いま組んでいるスプライトが上限に当たった
    std::vector<Item> items_;
    std::vector<SliceQuad> sliceQuads_;
    std::vector<Sprite2DVertex> scratch_; // 1 スプライト分の頂点 (上限 kMaxSpriteVertices)
    std::vector<std::pair<int32_t, int32_t>> seenMasks_; // 描画順でここまでに出たマスクの range
    Sprite2DDrawList list_;
};

} // namespace mye
