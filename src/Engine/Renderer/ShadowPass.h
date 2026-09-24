#pragma once
#include <DirectXMath.h>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/GpuTimer.h"
#include "Engine/Renderer/MeshInstancing.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
class RenderQueue;
struct RenderResources;
struct WaterDrawData; // WaterPass.h (M79 sub-05: MyEngineWater を影エントリへ渡す)

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
    // viewFrameIndex = M79 sub-03: サーフェスの影エントリが読む gTime (viewFrameIndex/60) の出所。
    // instancing = 非スキン連続 run の一括描画を併用 (M38f)。
    // water = M79 sub-05: MyEngineWater (影エントリ専用 static gWaterTime の代入元)。
    // null / !active = 従来どおり全 0 (水面が無いシーンは 1 ビットも変わらない)
    void Render(GraphicsDevice& device, ShaderManager& shaders, const RenderQueue& queue,
                RenderResources& resources, const DirectX::XMFLOAT4X4* lightViewProjs, int count,
                uint32_t viewFrameIndex, bool instancing = true, const WaterDrawData* water = nullptr);

    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); } // Texture2DArray (R32_FLOAT)
    int Resolution() const { return resolution_; }
    // M54d: 直近 Render の GPU 時間。局所ライトのアトラス (ShadowAtlas) と並べて
    // 「影の総コストのうちどちらが重いか」を ProfilerWindow で読むために足した
    float GpuMs() const { return timer_.Milliseconds(); }

private:
    bool ready_ = false;
    int resolution_ = 0;
    AssetID depthShader_ = {};
    // スキンメッシュ用。掛けないとバインドポーズの生ジオメトリが影に焼かれる
    // (shadow_depth_skinned.hlsl 冒頭に症状)
    AssetID depthSkinnedShader_ = {};
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
    Microsoft::WRL::ComPtr<ID3D11Buffer> boneCB_;   // b3: ボーンパレット (本描画と同じ中身)
    // ---- M79 sub-03: サーフェスの影エントリ用予約 CB。MyEnginePerFrame は全 0 固定で運用する
    //      (ShadowPass::Render は RenderSystem::PrepareEnvironment より前に呼ばれるため、光/霧/IBL
    //      はまだ view に埋まっていない。影エントリは PSMain を呼ばず、位置に効くのは
    //      static (gViewProj/gWorld/gTime/gWaterTime) だけという規約 (spec §4.1) に従う) ----
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfacePerFrameCB_;  // MyEnginePerFrame (常に全 0)
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfaceFrameCB_;     // MyEngineSurfaceFrame (shadowViewProj/gTime)
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfacePerObjectCB_; // MyEnginePerObject
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfaceWaterCB_;     // MyEngineWater (sub-05 まで 0 埋め)
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_; // 深度バイアス付き
    // M79 sub-06: doubleSided なサーフェスの影エントリ用 (同じ深度バイアスで Cull だけ外す)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerCullNone_;
    GpuTimer timer_;                                           // M54d
};

} // namespace mye
