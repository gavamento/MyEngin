#pragma once
#include <unordered_set>
#include <vector>
#include <wrl/client.h>

#include "Engine/Renderer/Device/GpuTimer.h"
#include "Engine/Renderer/Mesh/MeshInstancing.h"
#include "Engine/Renderer/Pipeline/RenderPath.h"
#include "Engine/Renderer/Passes/HzbDebugPass.h"
#include "Engine/Renderer/Passes/OcclusionCullPass.h"
#include "Engine/Renderer/Passes/SkyboxPass.h"
#include "Engine/Renderer/Passes/TerrainPass.h"
#include "Engine/Renderer/Passes/WaterPass.h"

namespace mye {

struct Material;             // GpuResources.h
struct Mesh;                 // GpuResources.h
struct SurfaceMaterialState; // GpuResources.h (M79 sub-02)

// Forward レンダリング (engine_spec.md 6.1 Option A)。
// opaque を手前順 → transparent を奥順で 1 パス描画する
class ForwardPath : public IRenderPath {
public:
    const char* Name() const override { return "Forward"; }
    bool Init(GraphicsDevice& device, ShaderManager& shaders) override;
    void Shutdown() override;
    void Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                const SceneLightData& lights, RenderResources& resources,
                ShaderManager& shaders) override;
    // M57e: Forward も t7 でフロクセルを合成する (不透明 / 透明 / 地形 + スカイ)。
    // ★合成を外すなら false に戻すこと — true のまま合成が無いと「ゴッドレイだけ消えて
    //   霧が増えない」= 霧が減るだけになる
    bool AppliesFroxel() const override { return true; }
    // M90a: 不透明メッシュ + 地形の GPU 時間
    float ForwardOpaqueGpuMs() const override { return opaqueTimer_.Milliseconds(); }
    // GPU オクルージョンの判定 + max-Z ピラミッド構築の GPU 時間 (フェーズ 1/2 の描画は含まない)
    float OcclusionGpuMs() const override { return occlusion_.GpuMs(); }
    const std::vector<OcclusionDebugBox>& OcclusionDebugBoxes() const override { return occDebugBoxes_; }
    // selftest 用: オクルージョンのリソース作成失敗を模擬する (描画は従来の経路で続く)
    void InjectOcclusionFailureForTest(bool on) { occlusion_.InjectCreateFailureForTest(on); }
    bool OcclusionDisabled() const { return occlusion_.IsDisabled(); }
    OcclusionStats OcclusionStatsForTest(uint32_t viewKey) const { return occlusion_.Stats(viewKey); }

private:
    // forward_lit が前提にする固定バインド一式 (VS/PS b0-b2・PS t1-t9・s0-s2・VS t0/t1・トポロジ・
    // ラスタライザ) を張り直す。他のパスや名前解決のサーフェスがスロットを張り替えた直後に呼ぶ
    // (水面の後・サーフェスアイテムの後)。instSrv = VS t0 に張るインスタンスバッファ (無ければ null)、
    // remapSrv = VS t1 に張るオクルージョンの remap (無ければ null)
    void BindForwardLitFixed(ID3D11DeviceContext* dc, const RenderView& view,
                             ID3D11ShaderResourceView* instSrv,
                             ID3D11ShaderResourceView* remapSrv = nullptr);

    // 1 回の描画単位。PlanItems が items から組み、従来の描画と GPU オクルージョンの 2 フェーズが
    // 同じ列を使う (スキップ規則・サーフェス判定はここ 1 箇所)
    struct DrawUnit {
        enum class Kind { Run, Single, Surface };
        Kind kind = Kind::Single;
        size_t first = 0;                     // items の先頭項目
        uint32_t count = 1;                   // 項目数 (Run なら run.count)
        const MeshInstanceRun* run = nullptr; // Run のみ
        Material* mat = nullptr;
        Mesh* mesh = nullptr;
        SurfaceMaterialState* surf = nullptr; // Surface のみ
        AssetID shaderId = {};                // Single のみ (スキンならスキニング版)
        bool skinned = false;
        int32_t cmdIndex = -1;                // オクルージョンのコマンド番号。-1 = 判定対象外 (Surface)
    };
    void PlanItems(GraphicsDevice& device, const std::vector<RenderItem>& items,
                   RenderResources& resources, ShaderManager& shaders,
                   const std::vector<MeshInstanceRun>* runs, std::vector<DrawUnit>& out);
    // units を描く。phase: -1 = 従来 (CPU が数を決めて直接描く) / 0 = フェーズ 1 / 1 = フェーズ 2
    // (どちらも間接描画。フェーズ 1 でサーフェスなど判定対象外の単位も描き、フェーズ 2 は間接描画のみ)
    void DrawUnits(GraphicsDevice& device, const std::vector<DrawUnit>& units,
                   const std::vector<RenderItem>& items, const RenderView& view,
                   RenderResources& resources, ShaderManager& shaders, int phase);
    // M79 sub-02: shader が "*.surface" のアイテムを 1 個描く (色エントリのみ。
    // 深度書き込み/ブレンドは呼び出し元が既に設定済みの opaque/transparent ステートに従う)。
    // 描画後に IA/VS/PS/CB/SRV/サンプラの一部が forward_lit の前提と食い違うので、
    // 呼び出し側 (DrawUnits) が続けて RestoreFixedBindings 相当を行うこと
    void DrawSurfaceItem(GraphicsDevice& device, const RenderItem& item, const Material& mat,
                         const Mesh& mesh, SurfaceMaterialState& surf, ShaderManager& shaders,
                         RenderResources& resources, const RenderView& view);

    Microsoft::WRL::ComPtr<ID3D11Buffer> perFrameCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perObjectCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> materialCB_; // PBR パラメータ (metallic/roughness)
    Microsoft::WRL::ComPtr<ID3D11Buffer> boneCB_;     // ボーンパレット (b3、スキニング、M18)
    AssetID skinnedShader_ = {};                      // forward_skinned (スキンメッシュ用に差替)
    // ---- インスタンシング (M38f)。forward_lit マテリアルの opaque 連続 run のみ対象 ----
    AssetID litShader_ = {};          // forward_lit (run 判定: mat->shader がこれと一致する時のみ)
    AssetID litInstancedShader_ = {}; // forward_lit_instanced
    MeshInstanceBuffer instanceBuf_;
    std::vector<uint8_t> canInstance_;        // フレーム毎スクラッチ
    std::vector<MeshInstanceRun> runs_;
    std::vector<DirectX::XMFLOAT4X4> worlds_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> shadowSampler_; // 比較サンプラ (PCF)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> iblSampler_;    // LINEAR/CLAMP (s2、M38c)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerWire_; // SceneView Wireframe (M40b)
    // M79 sub-06: doubleSided なサーフェス用 (Cull None、Solid)。DrawSurfaceItem が描画直前に
    // 張り、直後に restoreForwardLitBindings が rasterizer_/rasterizerWire_ へ戻す
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerCullNone_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOpaque_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthTransparent_; // 書き込みなし
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
    // ---- M79 sub-02: サーフェスシェーダーの予約 CB (forward_lit の b0-b2 とは別バッファ) ----
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfacePerFrameCB_;   // MyEnginePerFrame
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfaceFrameCB_;      // MyEngineSurfaceFrame
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfacePerObjectCB_;  // MyEnginePerObject
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfaceWaterCB_;      // MyEngineWater (sub-05 まで 0 埋め)
    AssetID surfaceErrorId_ = {}; // "surface_error" (失敗時のマゼンタ代替)
    std::unordered_set<uint64_t> skinnedSurfaceWarned_; // スキン+サーフェスの WARN はマテリアル毎に 1 回
    GpuTimer opaqueTimer_; // M90a: 不透明メッシュ + 地形
    // GPU オクルージョン (2 フェーズ)。viewKey ごとの履歴を内部に持つ
    OcclusionCuller occlusion_;
    HzbDebugPass hzbDebug_; // --hzb-debug-max の max-Z 全画面表示
    std::vector<DrawUnit> opaqueUnits_; // フレーム毎スクラッチ
    std::vector<DrawUnit> transparentUnits_;
    std::vector<OcclusionItemIn> occItems_;
    std::vector<OcclusionCmdIn> occCmds_;
    std::vector<OcclusionDebugBox> occDebugBoxes_;
    SkyboxPass skybox_; // 不透明後・透明前に空を塗る (M29d)
    // 地形 (M58c)。不透明メッシュの直後・スカイボックスの前に描く (深度を書くため)
    TerrainPass terrain_;
    // 水面 (スカイボックス後・透明メッシュ前)
    WaterPass water_;
};

} // namespace mye
