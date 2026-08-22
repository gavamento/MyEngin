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

// 可視チャンク 1 枚の描画指示 (Renderer 層の純データ)。
// Engine 層の `TerrainDrawItem` (TerrainSystem.h) を RenderSystem がここへ写す —
// Renderer は Engine のヘッダを読めない (層規約) ので型を分けてある。
struct TerrainRenderItem {
    AssetID mesh = {};
    DirectX::XMFLOAT4X4 world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float viewZ = 0.0f; // ソート用 (カメラ空間深度)。RenderItem::viewZ と同じ規約
    // M58c v1 の暫定サーフェス。**authored (sRGB) 色**で持ち、CB へ載せる直前に
    // リニアへ変換する (他パスと同じ規約)。M58d の 4 レイヤスプラットが置き換える
    DirectX::XMFLOAT4 baseColor = { 0.40f, 0.43f, 0.33f, 1.0f };
    float metallic = 0.0f;
    float roughness = 0.92f; // 土/草は完全な拡散面に近い
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
