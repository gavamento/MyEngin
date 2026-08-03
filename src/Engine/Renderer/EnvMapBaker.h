#pragma once
#include <cstdint>
#include <unordered_map>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// IBL 用の環境マップ一式 (M38c)。EnvMapBaker が所有する SRV への非所有ポインタ
struct EnvMaps {
    ID3D11ShaderResourceView* irradiance = nullptr;  // 32² cube (平均入射色に正規化)
    ID3D11ShaderResourceView* prefiltered = nullptr; // 128² cube + mips (GGX プリフィルタ)
    ID3D11ShaderResourceView* brdfLut = nullptr;     // 256² R16G16F (split-sum 第 2 項)
    float specMips = 0.0f;                           // prefiltered の最終 mip index
};

// スカイ環境から IBL マップを GPU ベイクする (M38c)。ロード時 lazy + キャッシュ、
// ディスク書き出しは無し (数 ms 級)。ソースは cubemap SRV または gradient 3 色 (リニア) —
// gradient も同じベイクを通すことで ApplyLighting 側の分岐を IBL on/off の 2 択に保つ。
// 描画専用 (sim/hash 非干渉)。レイヤ規約: 生 D3D11 はこの Renderer 層に閉じる。
class EnvMapBaker {
public:
    // cubemap ソース (キー = AssetID)。初回はベイク、以後キャッシュ
    EnvMaps GetForCubemap(GraphicsDevice& device, ShaderManager& shaders, AssetID id,
                          ID3D11ShaderResourceView* src);
    // gradient ソース (キー = 3 色のビットハッシュ)。色は SrgbToLinear 済みで渡すこと
    EnvMaps GetForGradient(GraphicsDevice& device, ShaderManager& shaders,
                           const DirectX::XMFLOAT3& top, const DirectX::XMFLOAT3& horizon,
                           const DirectX::XMFLOAT3& bottom);
    // M46h: split-sum 第 2 項 (環境 BRDF) だけを取り出す。スカイに依存しない純関数なので、
    // スカイの無いシーンでも RT 反射の合成に使える。失敗時は null (初回のみベイク)
    ID3D11ShaderResourceView* GetBrdfLut(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();

    static constexpr int kSpecSize = 128;
    static constexpr int kSpecMips = 5; // 128..8 (roughness 0..1 を線形マップ)
    static constexpr int kIrrSize = 32;
    static constexpr int kLutSize = 256;

private:
    struct Baked {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> irrTex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> irrSrv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> preTex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preSrv;
    };
    struct GradientColors {
        DirectX::XMFLOAT3 top, horizon, bottom;
    };

    bool EnsureCommon(GraphicsDevice& device, ShaderManager& shaders); // CB/サンプラ/LUT
    // src == nullptr なら gradient モード (grad の色を使う)
    bool Bake(GraphicsDevice& device, ShaderManager& shaders, ID3D11ShaderResourceView* src,
              const GradientColors& grad, Baked& out);
    EnvMaps Result(const Baked& b) const;

    bool commonReady_ = false;
    AssetID prefilterShader_ = {};
    AssetID irradianceShader_ = {};
    AssetID lutShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> lutTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lutSrv_;

    std::unordered_map<uint64_t, Baked> cache_; // 上限超過で全クリア (再ベイクは安い)
};

} // namespace mye
