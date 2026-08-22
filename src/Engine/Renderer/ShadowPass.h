#pragma once
#include <DirectXMath.h>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/MeshInstancing.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
class RenderQueue;
struct RenderResources;

// 平行光シャドウマップ (M17 単一 → M38d CSM 3 カスケード)。ライト視点から不透明
// ジオメトリの深度のみを Texture2DArray の各スライスへ描き、SRV として本描画の
// ライティングへ渡す (PCF 比較 + 範囲ベースのカスケード選択)。描画専用でハッシュ非対象。
class ShadowPass {
public:
    // シェーダの SampleShadowCSM (common.hlsli) の vps[] と対。
    // tools\check_rules.ps1 の規則 9 が配列長の一致を静的に検査する (M55a で登録)
    static constexpr int kCascades = 3;

    bool Init(GraphicsDevice& device, ShaderManager& shaders, int resolution = 2048);
    bool IsReady() const { return ready_; }

    // 不透明キューを各カスケードの lightViewProj (非転置、行ベクトル規約 world*view*proj) で
    // シャドウ深度 (スライス c) へ描く。count は kCascades 以下。
    // instancing = 非スキン連続 run の一括描画を併用 (M38f)
    void Render(GraphicsDevice& device, ShaderManager& shaders, const RenderQueue& queue,
                RenderResources& resources, const DirectX::XMFLOAT4X4* lightViewProjs, int count,
                bool instancing = true);

    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); } // Texture2DArray (R32_FLOAT)
    int Resolution() const { return resolution_; }

private:
    bool ready_ = false;
    int resolution_ = 0;
    AssetID depthShader_ = {};
    // ---- インスタンシング (M38f)。run はカスケード間で共通 (充填は 1 回) ----
    AssetID depthInstancedShader_ = {};
    MeshInstanceBuffer instanceBuf_;
    std::vector<uint8_t> canInstance_; // フレーム毎スクラッチ
    std::vector<MeshInstanceRun> runs_;
    std::vector<DirectX::XMFLOAT4X4> worlds_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_; // ArraySize = kCascades
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_[kCascades]; // スライス毎
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> objectCB_; // 1 オブジェクトあたり transpose(world*lightVP)
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_; // 深度バイアス付き
};

} // namespace mye
