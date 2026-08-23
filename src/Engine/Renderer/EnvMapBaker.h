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

// D3D キューブマップ面 (+X,-X,+Y,-Y,+Z,-Z) の基底。
// ★M56e: この表は「ベイクが方向を組む式」と「プローブが 6 面を実描画するカメラ」の
//   **両方**が読む唯一の正本。片方だけ書き換えると焼いた反射が面ごとに 90 度ずれるが、
//   絵からは「なんとなく変」としか見えないので、表を 2 つ持たせないこと。
//   right は必ず up × forward (LH) と一致する — ProbeBakerSelfTest が機械で固定している
struct CubeFaceBasis {
    DirectX::XMFLOAT3 forward, right, up;
};
const CubeFaceBasis& CubeFace(int face); // face は 0..5 (範囲外は 0 に丸める)

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
    // 焼き上がった 1 組 (irradiance + prefiltered)。**テクスチャの所有者**。
    // M56e で public にした — 反射プローブは「キャッシュを通さず、呼び出し側が
    // プローブ 1 個ぶんずつ持つ」ので、キャッシュ内部の型では足りない
    struct BakedEnv {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> irrTex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> irrSrv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> preTex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preSrv;
    };

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
    // M56e: 任意の cubemap SRV から IBL 一式を焼く (**キャッシュを通さない**)。
    // 反射プローブはシーンを 6 面へ実描画した cube を渡す — ソースが「空」ではなく
    // 「その場所から見た景色」になるだけで、プリフィルタの式は 1 行も変わらない
    bool BakeFrom(GraphicsDevice& device, ShaderManager& shaders, ID3D11ShaderResourceView* src,
                  BakedEnv& out);
    // BakedEnv + 共通の BRDF LUT → EnvMaps。**返り値は非所有ポインタ**なので、
    // BakedEnv と EnvMapBaker のどちらかが死ぬと同時にぶら下がる
    EnvMaps MapsFor(const BakedEnv& b) const;
    void Shutdown();

    static constexpr int kSpecSize = 128;
    static constexpr int kSpecMips = 5; // 128..8 (roughness 0..1 を線形マップ)
    static constexpr int kIrrSize = 32;
    static constexpr int kLutSize = 256;

private:
    struct GradientColors {
        DirectX::XMFLOAT3 top, horizon, bottom;
    };

    bool EnsureCommon(GraphicsDevice& device, ShaderManager& shaders); // CB/サンプラ/LUT
    // src == nullptr なら gradient モード (grad の色を使う)
    bool Bake(GraphicsDevice& device, ShaderManager& shaders, ID3D11ShaderResourceView* src,
              const GradientColors& grad, BakedEnv& out);

    bool commonReady_ = false;
    AssetID prefilterShader_ = {};
    AssetID irradianceShader_ = {};
    AssetID lutShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> lutTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lutSrv_;

    std::unordered_map<uint64_t, BakedEnv> cache_; // 上限超過で全クリア (再ベイクは安い)
};

} // namespace mye
