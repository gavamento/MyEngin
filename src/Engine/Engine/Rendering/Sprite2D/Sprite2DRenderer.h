//====================================================================================
//                          Sprite2DRenderer.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D の収集とソート (描画リストを作る)
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Renderer/Passes/Sprite2DDrawList.h"

namespace mye {

class World;
struct RenderResources;
struct Sprite2DComponent;
struct SpriteDef;

// World の active な Sprite2D を集め、SpriteDef を解決して 4 頂点 (6 頂点の三角形リスト) に展開し、
// 整数のソートキーで並べた描画リストを作る。描画は Renderer 層の Sprite2DPass。
// ソートキー = (ソート層の描画順, orderInLayer, グループキー, entity.index) の辞書式で、安定ソート。viewZ は見ない。
// 描画専用で sim の入力ではない (ハッシュ対象外)。メインスレッド専用
class Sprite2DRenderer {
public:
    // Sprite2D が無ければ空の描画リストを返す。返した参照は次の Collect まで有効
    const Sprite2DDrawList& Collect(World& world, RenderResources& resources);

    // pivot を原点にしたローカル矩形 {left, bottom, right, top} (ワールド単位、flip 適用前)。
    // 描画と Scene View のギズモが同じ寸法を使うための 1 本。texWidth / texHeight は全面スプライトのときだけ使う
    static void LocalRect(const SpriteDef& def, int texWidth, int texHeight, float out[4]);

private:
    struct Item {
        int32_t layerOrder = 0;
        int32_t orderInLayer = 0;
        int32_t groupKey = 0; // SortingGroup2D (sub-06) が入れる。今は常に 0
        uint32_t entityIndex = 0;
        const Sprite2DComponent* sprite = nullptr;
        const DirectX::XMFLOAT4X4* world = nullptr;
    };

    AssetID EnsureMissingTexture(RenderResources& resources);
    void Emit(const Item& item, RenderResources& resources);
    void Push(AssetID texture, bool pointFilter, const Sprite2DVertex* v, uint32_t count);

    AssetID missingTexture_ = {}; // 内蔵 16x16 マゼンタ (未解決のスプライト用。必要になったときに作る)
    std::vector<Item> items_;
    Sprite2DDrawList list_;
};

} // namespace mye
