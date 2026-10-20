//====================================================================================
//                          Sprite2DRenderer.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D の収集とソート (描画リストを作る)
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Renderer/Passes/Sprite2DDrawList.h"

namespace mye {

class World;
struct RenderResources;
struct Sprite2DComponent;
struct SpriteMask2DComponent;
struct SpriteDef;

// World の active な Sprite2D / SpriteMask2D / Tilemap を集め、SpriteDef を解決して三角形リストに展開し、
// 整数のソートキーで並べた描画リストを作る。描画は Renderer 層の Sprite2DPass。
// ソートキー = (外側の層順, order, index, 内側の層順, order, index) の辞書式で、安定ソート。viewZ は見ない。
// SortingGroup2D の子孫は外側 = 最も近い祖先グループ、内側 = 自分。グループ無しは外側 = 自分、内側 = 0。
// Tilemap はレイヤーごと・チャンクごとに 1 項目で、同じキーの Sprite2D と並ぶ (同値は entity.index → レイヤー → チャンク)。
// 描画専用で sim の入力ではない (ハッシュ対象外)。メインスレッド専用
class Sprite2DRenderer {
public:
    // Sprite2D / Tilemap が無ければ空の描画リストを返す。返した参照は次の Collect まで有効。
    // view / proj を渡し、proj が正射影ならタイルのチャンクを画面外で切る (透視 / 省略は切らない)
    const Sprite2DDrawList& Collect(World& world, RenderResources& resources,
                                    const DirectX::XMFLOAT4X4* view = nullptr,
                                    const DirectX::XMFLOAT4X4* proj = nullptr);

    // 正射影のとき、カメラの視錐台 (直方体) を囲むワールドの AABB (view / proj は行ベクトル規約の行列)。
    // 透視 / 行列が無い / 特異は false (タイルを切らない)
    static bool OrthoWorldBounds(const DirectX::XMFLOAT4X4* view, const DirectX::XMFLOAT4X4* proj,
                                 DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax);

    // タイル 1 枚の 4 隅の UV。corner = 左上, 右上, 右下, 左下 (テクスチャの (u0, v0) が左上)。
    // flip を先に掛け、その後 rotation (0..3 = 反時計回りに 90 度ずつ) で絵を回す。ジオメトリは変えない
    static void TileCornerUv(float u0, float v0, float u1, float v1, bool flipX, bool flipY, uint32_t rotation,
                             float out[4][2]);

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
        // タイルのチャンク (isTile)。同じキーの中ではレイヤー添字 → チャンク (cy, cx) の順
        bool isTile = false;
        int32_t tileLayer = 0;
        ChunkKey tileChunk;
        EntityID tileEntity = kNullEntity;
        DirectX::XMFLOAT3 tileOrigin = { 0.0f, 0.0f, 0.0f };
        const Sprite2DComponent* sprite = nullptr;
        const SpriteMask2DComponent* mask = nullptr;
        const DirectX::XMFLOAT4X4* world = nullptr;
    };

    // 焼いたチャンク 1 枚。頂点はタイルマップの原点基準 (z = 0) で、描画のたびに原点を足して流す
    struct TileRun {
        AssetID texture;
        bool pointFilter = false;
        uint32_t start = 0;
        uint32_t count = 0;
    };
    struct TileCacheKey {
        uint32_t entityIndex = 0;
        uint32_t entityGeneration = 0;
        int32_t layer = 0;
        ChunkKey chunk;
        auto operator<=>(const TileCacheKey&) const = default;
    };
    struct BakedChunk {
        uint64_t mapEpoch = 0;     // 焼いたときのライブラリ / 全体 dirty の版。違えば焼き直す
        uint64_t paletteEpoch = 0;
        uint64_t dirtyEpoch = 0;
        uint64_t spriteEpoch = 0;  // SpriteLibrary の版 (スプライトエディタの Apply で UV が変わる)
        bool complete = false;     // false = 非同期ロード中のスプライトがあった (次フレームも焼き直す)
        uint64_t lastUsed = 0;
        std::vector<Sprite2DVertex> vertices;
        std::vector<TileRun> runs;
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
    void EmitTileChunk(const Item& item, World& world, RenderResources& resources);
    void BakeTileChunk(const Item& item, World& world, RenderResources& resources, BakedChunk& out);
    // 外側 = 自分のキー。グループの子孫なら外側 = グループのキー、内側 = 自分のキー
    void AssignKeys(Item& it, World& world, bool hasGroups, EntityID e, int32_t layerId, int32_t order) const;
    void CollectSprites(World& world, bool hasGroups);
    void CollectMasks(World& world, bool hasGroups);
    void CollectTilemaps(World& world, const DirectX::XMFLOAT4X4* view, const DirectX::XMFLOAT4X4* proj);
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
    std::map<TileCacheKey, BakedChunk> tileCache_;       // 走査は挿入 / 削除だけ (描画順には使わない)
    uint64_t tileFrame_ = 0;                             // Collect の通し番号 (使われなくなったチャンクの掃除用)
    std::vector<Sprite2DVertex> tileScratch_;            // 原点を足した 1 run 分の頂点
    Sprite2DDrawList list_;
};

} // namespace mye
