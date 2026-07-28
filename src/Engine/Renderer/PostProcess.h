#pragma once
#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include <DirectXMath.h>

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
        // linear→sRGB OETF。M38a でリニアパイプライン (sRGB テクスチャデコード +
        // authored 色の CPU 変換) が揃ったので既定 ON。
        // 制限: enablePostFx=false (HDR 配管バイパス) 時は OETF も掛からない = 旧来の見た目
        bool applyGamma = true;
        // ---- M32d: 追加ポスト効果 (既定 = 無効 = 従来の見た目) ----
        float chromAberration = 0.0f;   // 色収差 (UV スケール、0=off)
        float vignetteIntensity = 0.0f; // 周辺減光 (0=off)
        float vignetteRadius = 0.75f;   // 減光開始半径 (0..1)
        float saturation = 1.0f;        // 彩度 (1=変化なし)
        float contrast = 1.0f;          // コントラスト (1=変化なし)
        DirectX::XMFLOAT4 colorFilter = { 1.0f, 1.0f, 1.0f, 1.0f }; // 乗算カラーフィルタ
    };

    // サイズ別の中間ターゲット群 (フルスクリーン HDR シーン + 半解像度ブルーム ping-pong)。
    struct Target {
        uint64_t key = 0;      // (width<<32)|height
        RenderTexture scene;   // フル解像度 HDR (color のみ。depth はシーン側 target.dsv を共有)
        RenderTexture bloomA;  // 半解像度ブルーム (bright/最終)
        RenderTexture bloomB;  // 半解像度 ping-pong
        RenderTexture ldr;     // FXAA 用の LDR 中間 (トーンマップ結果、fxaa 有効時のみ使用)
        RenderTexture distort; // M42d: 歪みバッファ (フル解像度 R16G16F、UV オフセット加算先)
    };

    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    bool IsReady() const { return ready_; }

    // このサイズの中間ターゲット群を取得/生成する (サイズ別 LRU キャッシュ)。
    Target* Acquire(GraphicsDevice& device, int width, int height);

    // t.scene を解決して dst へ書く (ブルーム → トーンマップ → FXAA)。dst は LDR。
    // distortionActive (M42d): このフレーム歪みパーティクルが t.distort に描かれたとき true
    // -> トーンマップのシーンサンプル UV に t.distort をオフセット加算する
    void Resolve(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                 ID3D11RenderTargetView* dst, int width, int height, const Settings& s,
                 bool distortionActive = false);

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

struct CameraPostFxComponent;

// カメラ別ポストプロセス (M29e): base (グローバル設定) に CameraPostFxComponent の値を
// 上書きした Settings を返す純関数 (selftest 対象)。applyGamma は base のまま維持する
PostProcess::Settings MergeCameraPostFx(const PostProcess::Settings& base,
                                        const CameraPostFxComponent& comp);

} // namespace mye
