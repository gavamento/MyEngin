#pragma once
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Renderer/GpuTimer.h"
#include "Engine/Renderer/RenderTexture.h"
#include "Engine/Renderer/RenderTypes.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// RtScene (Engine 層) が用意した GPU バッファ一式。
// これを介することで Renderer 層は ECS / メッシュライブラリを知らずに済む
struct RtSceneBindings {
    ID3D11ShaderResourceView* nodes = nullptr;     // 全 BLAS 連結のノード配列
    ID3D11ShaderResourceView* tris = nullptr;      // 同上 (三角形)
    ID3D11ShaderResourceView* attrs = nullptr;     // 同上 (頂点属性)
    ID3D11ShaderResourceView* tlas = nullptr;      // TLAS (root = 0)
    ID3D11ShaderResourceView* instances = nullptr; // TLAS の葉順に並んだインスタンス
    ID3D11ShaderResourceView* materials = nullptr;
    int32_t instanceCount = 0;

    bool IsValid() const
    {
        return nodes && tris && attrs && tlas && instances && materials && instanceCount > 0;
    }
};

// フレーム毎の入力 (G-Buffer とライト)。DeferredPath が組んで渡す
struct RtFrameInputs {
    const RtSceneBindings* scene = nullptr;
    const SceneLightData* lights = nullptr;
    ID3D11ShaderResourceView* gbNormal = nullptr;   // ワールド法線 (*0.5+0.5)
    ID3D11ShaderResourceView* gbPosition = nullptr; // ワールド座標
    ID3D11ShaderResourceView* gbAlbedo = nullptr;   // a = ジオメトリ有りマーク
    ID3D11ShaderResourceView* skyCube = nullptr;    // skyMode==1 のときのみ
};

// レイトレーシングのコンピュートパス群 (M46b: デバッグ表示 / M46c: 拡散 GI)。
// Renderer 層 = 生の D3D11 はここに閉じる
class RtPasses {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return inited_; }

    // 拡散 GI を内部解像度で 1spp 計算する。戻り値 = 結果の SRV (null = 計算していない)。
    // 出力は albedo を掛けない入射放射輝度 (IBL irradiance と同次元)
    ID3D11ShaderResourceView* RenderGi(GraphicsDevice& device, ShaderManager& shaders,
                                       const RenderView& view, const RtFrameInputs& in);

    // デバッグ表示を view.rtv へ上書きする。描いたら true。
    // giSrv は rtDebugMode==4 (生 GI 表示) のときだけ使う
    bool RenderDebug(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                     const RtFrameInputs& in, ID3D11ShaderResourceView* giSrv);

    // 直近の GPU 時間 (ProfilerWindow 表示用)
    float DebugGpuMs() const { return debugTimer_.Milliseconds(); }
    float GiGpuMs() const { return giTimer_.Milliseconds(); }

private:
    // t0-t6 / b0-b1 / s0 (シーン + 環境) をコンピュートステージへバインドする
    void BindCommon(GraphicsDevice& device, const RenderView& view, const RtFrameInputs& in);
    void UnbindCompute(GraphicsDevice& device);
    // src を view.rtv 全面に貼る
    bool Blit(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
              ID3D11ShaderResourceView* src);

    RenderTexture debugRt_; // デバッグ CS の出力先 (フル解像度、UAV 付き)
    RenderTexture giRt_;    // GI の出力先 (内部解像度、UAV 付き)
    AssetID debugCS_ = {};
    AssetID giCS_ = {};
    AssetID blitShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> envCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> debugCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> giCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> blitCB_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearClamp_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    GpuTimer debugTimer_;
    GpuTimer giTimer_;
    bool inited_ = false;
};

} // namespace mye
