#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
struct ShaderProgram;
struct RenderResources;
struct RenderView;

// 地形の描画パス (M58c、spec §6.5)。
//
// ★**なぜ独立したパスなのか**: Deferred の不透明パスは `material->shader` を**見ない**
//   (`DeferredPath.cpp` が GBuffer シェーダ固定 3 種を bind している。Forward は見る)。
//   つまり地形を「マテリアルを持つ MeshRenderer」として通しても、Deferred では永久に
//   deferred_gbuffer で描かれてしまう。加えて地形はテクスチャを 8 枚使う予定 (M58d の
//   4 レイヤ x albedo+normal) で `Material` の 2 枚枠に収まらない。**専用パス一択**。
//
// ★**CB は b4 を使う** (下の kTerrainObjectCbSlot)。b0 = ホストパスが張った PerFrame を
//   そのまま読み、b1-b3 (PerObject / MaterialParams / ボーンパレット) には触らない。
//   触ると「地形の後に描かれる透明メッシュが地形の CB を読む」形で静かに壊れる。
//   この方針のおかげでホスト側は terrain の描画後に何も張り直さなくてよい。

// 地形固有 CB のスロット。**HLSL 側 (deferred_terrain.hlsl / forward_terrain.hlsl) の
// `register(b4)` と必ず一致させること。**
inline constexpr uint32_t kTerrainObjectCbSlot = 4;

// ★**地形パス専用の SRV スロット (M58d)。** t0-t7 はホスト (Deferred 光パス / Forward) の
//   持ち物で、t12-t15 / t6-t7 は他のマイルストーンの予約席 (計画の付録「予約 2」)。
//   地形は**誰とも隣り合わない t20 以降**へ逃がす — こうしておけば
//   「統合で番号がぶつかったが *コンパイルは通る*」という一番静かな壊れ方が起きない。
//   描画後に必ず null で剥がす (剥がさないと後段のパスが読まないだけの残留になるが、
//   RT との二重バインド警告の温床になる)。**HLSL の register(t20/t21/t25) と一致必須**
inline constexpr uint32_t kTerrainSplatSrvSlot = 20;      // スプラット (RGBA8 = 4 レイヤの重み)
inline constexpr uint32_t kTerrainAlbedoSrvSlot = 21;     // レイヤ albedo x4 (t21..t24)
inline constexpr uint32_t kTerrainNormalSrvSlot = 25;     // レイヤ normal x4 (t25..t28)

// スプラットは RGBA8 の 4 チャンネル = レイヤ 4 枚が構造的な上限。
// **`TerrainAsset::kMaxLayers` の Renderer 層ミラー** (Renderer は Engine のヘッダを読めない —
// 層規約)。食い違いは TerrainSystem.cpp の static_assert が止める
inline constexpr uint32_t kTerrainLayerCount = 4;

// 地表レイヤ 1 枚の bind (M58d)。テクスチャは `Material` に載せない —
// `Material` はテクスチャ 2 枚までで 4 レイヤ x (albedo + normal) = 8 枚が入らないため
// (拡張すると ParseMaterialJson とシーン互換に波及する)。
struct TerrainLayerBinding {
    AssetID albedo = {}; // null 不可 (TerrainSystem が 1x1 白で埋める)
    AssetID normal = {}; // null 不可 (TerrainSystem が 1x1 平坦法線で埋める)
    // リニア済みの色味。rgb = albedo への乗算、a = レイヤ有効フラグ (0 = 重みを殺す)。
    // **フラグを tint の a に相乗りさせている**のは CB を 16 バイト境界に保つため
    DirectX::XMFLOAT4 tint = { 1.0f, 1.0f, 1.0f, 1.0f };
    float tilingU = 8.0f; // 地形全幅あたりの繰り返し回数
    float tilingV = 8.0f;
};

// 地形 1 枚ぶんのサーフェス (= チャンク間で共通の材質)。
struct TerrainSurface {
    AssetID splat = {}; // null 不可 (TerrainSystem が「レイヤ 0 が 100%」の 1x1 で埋める)
    TerrainLayerBinding layers[kTerrainLayerCount];
    float metallic = 0.0f;
    float roughness = 0.92f; // 土/草は完全な拡散面に近い
};

// 可視チャンク 1 枚の描画指示 (Renderer 層の純データ)。
// Engine 層の `TerrainDrawItem` (TerrainSystem.h) を RenderSystem がここへ写す —
// Renderer は Engine のヘッダを読めない (層規約) ので型を分けてある。
struct TerrainRenderItem {
    AssetID mesh = {};
    DirectX::XMFLOAT4X4 world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float viewZ = 0.0f; // ソート用 (カメラ空間深度)。RenderItem::viewZ と同じ規約
    // ★サーフェスは**チャンクごとに複製して持つ**。同じ地形のチャンクは全部同じ値だが、
    //   ソートで並びが変わるので「地形単位の表を index で引く」形にすると
    //   index の付け替えが要る。1 件 ~200 バイト x 可視チャンク数 (既定デモで 16) は
    //   毎フレーム作り直しても無視できる
    TerrainSurface surface;
};

// 地形チャンクの描画順 (近い順 = early-z が効く順)。
// ★**タイブレークを AssetID にしてあるのが本体**: 同じ viewZ のチャンクが並んだとき、
//   比較が「元の並び」に依存すると描画順が収集順の揺れで変わる (規則 7 が禁じるのはまさに
//   それ)。ポインタ比較も禁止。純関数なので TerrainSelfTest が直接検査する
inline bool TerrainDrawOrderLess(const TerrainRenderItem& a, const TerrainRenderItem& b)
{
    if (a.viewZ != b.viewZ) {
        return a.viewZ < b.viewZ;
    }
    return a.mesh.value < b.mesh.value;
}

// このフレームの地形描画リスト。RenderSystem が所有し RenderView から指す。
// 空 / null = 地形なし = 従来と完全に同じ絵 (AssetPreview の RenderSystem は常に空)
struct TerrainDrawList {
    std::vector<TerrainRenderItem> items;
};

class TerrainPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();

    // Deferred のジオメトリパスから呼ぶ。RT (GBuffer 4 枚 + 深度) / ビューポート /
    // ラスタライザ / 深度ステート / ブレンドは**呼び出し側の状態をそのまま使う**
    // (Wireframe (M40b) もそのまま効く)
    void RenderGBuffer(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                       RenderResources& resources);
    // Forward の不透明段の直後から呼ぶ。t1 (CSM) / t3-t5 (IBL) / s1 / s2 は
    // ForwardPath がフレーム頭で張ったものを読む
    void RenderForward(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                       RenderResources& resources);

private:
    void Draw(GraphicsDevice& device, ShaderProgram* prog, const RenderView& view,
              RenderResources& resources);

    AssetID deferredShader_ = {};
    AssetID forwardShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> objectCB_;
};

} // namespace mye
