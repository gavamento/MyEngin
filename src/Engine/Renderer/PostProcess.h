#pragma once
#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// HDR ポストプロセス (M16)。シーンを R16G16B16A16F の中間ターゲットへ描画させ、
// 最後にトーンマップ (+ ブルーム / FXAA) でフルスクリーン解決して LDR 出力する。
// 描画専用でワールドハッシュには一切関与しない (RenderQueue と同じく sim 非対象)。
class PostProcess {
public:
    struct Settings {
        int tonemap = 1;             // 0=passthrough 1=ACES 2=Reinhard
        float exposure = 1.0f;       // トーンマップ前の露出倍率
        float bloomThreshold = 1.0f; // ブルーム bright-pass しきい値 (輝度)
        float bloomIntensity = 0.6f; // 合成強度 (0 で無効)
        bool bloom = true;
        bool fxaa = true;
        // linear→sRGB OETF。M16 のシーンは sRGB 未対応 (テクスチャ非デコード) のため既定 OFF。
        // 正しいリニアパイプラインが揃う M17 (sRGB テクスチャ) で ON にする。
        bool applyGamma = false;
    };

    // サイズ別の中間ターゲット群 (フルスクリーン HDR シーン + 半解像度ブルーム ping-pong)。
    struct Target {
        uint64_t key = 0;      // (width<<32)|height
        RenderTexture scene;   // フル解像度 HDR (color のみ。depth はシーン側 target.dsv を共有)
        RenderTexture bloomA;  // 半解像度ブルーム (bright/最終)
        RenderTexture bloomB;  // 半解像度 ping-pong
        RenderTexture ldr;     // FXAA 用の LDR 中間 (トーンマップ結果、fxaa 有効時のみ使用)
    };

    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    bool IsReady() const { return ready_; }

    // このサイズの中間ターゲット群を取得/生成する (サイズ別 LRU キャッシュ)。
    Target* Acquire(GraphicsDevice& device, int width, int height);

    // t.scene を解決して dst へ書く (ブルーム → トーンマップ → FXAA)。dst は LDR。
    void Resolve(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                 ID3D11RenderTargetView* dst, int width, int height, const Settings& s);

private:
    // t.scene から bright-pass → 分離ガウスブラーを行い、結果を t.bloomA (半解像度) に残す。
    void RunBloom(GraphicsDevice& device, ShaderManager& shaders, Target& t, const Settings& s);

    GraphicsDevice* device_ = nullptr;
    bool ready_ = false;

    AssetID tonemapShader_ = {};
    AssetID brightShader_ = {};
    AssetID blurShader_ = {};
    AssetID fxaaShader_ = {};

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;     // PostFx CB (tonemap)
    Microsoft::WRL::ComPtr<ID3D11Buffer> brightCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> blurCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> fxaaCB_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearClamp_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOff_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;

    std::vector<Target> cache_; // サイズ別 (LRU、上限あり)
};

} // namespace mye
