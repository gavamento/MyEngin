#pragma once
#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/GpuTimer.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
struct RenderView; // M43b: Resolve へのシーン情報供給口 (RenderTypes.h)

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
        // ---- M43b: スクリーンスペースゴッドレイ (既定 = 無効 = 従来の見た目) ----
        float godrayIntensity = 0.0f; // 空マスクの明るさ倍率 (0=off)
        float godrayDecay = 0.95f;    // 放射ブラーのタップ毎減衰 (0..1)
        // ---- M44a: カラーグレーディング LUT (既定 = 無効)。256x16 ストリップ (16 スライス)。
        //      lutSRV は render-only: RenderSystem がマージ後に lutTexture から解決して埋める
        //      (シリアライズ/比較対象は AssetID のみ) ----
        AssetID lutTexture = {};
        float lutIntensity = 0.0f; // 0=off / 1=LUT 全適用
        ID3D11ShaderResourceView* lutSRV = nullptr;
        // ---- M44b: 自動露出 (既定 = 無効)。輝度ヒストグラム CS → 露出倍率 (GPU 内完結) ----
        int autoExposure = 0;  // 0=off 1=on
        float aeSpeed = 3.0f;  // 適応速度 (1/s)
        float aeMin = 0.25f;   // 露出倍率の下限
        float aeMax = 4.0f;    // 上限
        // Settings 専用 (Merge は base 維持 — applyGamma と同じ扱い)。
        // EngineLoop が --replay-verify/--replay-record/--screenshot 時に true にし、
        // 適応を 1 フレーム収束させて決定的スクショを成立させる
        bool aeInstant = false;
        // ---- M44c: 被写界深度 (既定 = 無効)。CoC + 半解像度 gather ----
        float dofFocusDistance = 10.0f; // 焦点距離 (ビュー空間 z)
        float dofFocusRange = 5.0f;     // 焦点面から CoC が最大に達するまでの距離
        float dofMaxRadius = 0.0f;      // 最大ボケ半径 (フル解像度 px、0=off)
        // ---- M44d: カメラモーションブラー (既定 = 無効)。深度再投影方式 ----
        float motionBlurIntensity = 0.0f; // ブラー量スケール (0=off、SceneView は強制 0)
        float mbMaxPixels = 16.0f;        // 速度クランプ (px、スミアの暴走防止)
    };

    // サイズ別の中間ターゲット群 (フルスクリーン HDR シーン + 半解像度ブルーム ping-pong)。
    struct Target {
        uint64_t key = 0;      // (width<<32)|height
        RenderTexture scene;   // フル解像度 HDR (color のみ。depth はシーン側 target.dsv を共有)
        RenderTexture bloomA;  // 半解像度ブルーム (bright/最終)
        RenderTexture bloomB;  // 半解像度 ping-pong
        RenderTexture ldr;     // FXAA 用の LDR 中間 (トーンマップ結果、fxaa 有効時のみ使用)
        RenderTexture distort; // M42d: 歪みバッファ (フル解像度 R16G16F、UV オフセット加算先)
        RenderTexture godA;    // M43b: ゴッドレイ (半解像度 FP16。bloomA/B は bloom 結果保持中のため流用不可)
        RenderTexture godB;    // M43b: 放射ブラー ping-pong
        RenderTexture sceneB;  // M44c: DoF (/M44d MB) の書き先 — 実行後は bloom/tonemap がこちらを読む
        RenderTexture dofA;    // M44c: プリフィルタ結果 (半解像度、rgb=色 a=符号付き CoC)
        RenderTexture dofB;    // M44c: ギャザー結果 (半解像度)
        // ---- M44b: 自動露出 (ヒストグラム 256 bin + 露出倍率 1 要素、GPU 常駐) ----
        // exposureBuf[0] はフレームを跨いで持ち越す適応状態 (初期値 1.0、リサイズで再生成 = リセット)
        Microsoft::WRL::ComPtr<ID3D11Buffer> histBuf;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> histUAV;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> histSRV;
        Microsoft::WRL::ComPtr<ID3D11Buffer> exposureBuf;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> exposureUAV;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> exposureSRV;
    };

    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    bool IsReady() const { return ready_; }

    // このサイズの中間ターゲット群を取得/生成する (サイズ別 LRU キャッシュ)。
    Target* Acquire(GraphicsDevice& device, int width, int height);

    // t.scene を解決して dst へ書く (ブルーム → ゴッドレイ → トーンマップ → FXAA)。dst は LDR。
    // view (M43b): depthSRV/太陽/view/proj の供給口 (M44 の DoF/モーションブラーもここから取る)。
    // distortionActive (M42d): このフレーム歪みパーティクルが t.distort に描かれたとき true
    // -> トーンマップのシーンサンプル UV に t.distort をオフセット加算する
    void Resolve(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                 ID3D11RenderTargetView* dst, int width, int height, const Settings& s,
                 const RenderView& view, bool distortionActive = false);

    // M44d: 直近の Resolve の GPU 時間 (ms)。ProfilerWindow の "postfx" 行が表示する。
    // 複数ビュー解決時は最後に完了した計測値
    float ResolveGpuMs() const { return resolveTimer_.Milliseconds(); }

private:
    // sceneSRV から bright-pass → 分離ガウスブラーを行い、結果を t.bloomA (半解像度) に残す。
    // sceneSRV は t.scene または DoF 実行後の t.sceneB (M44c)
    void RunBloom(GraphicsDevice& device, ShaderManager& shaders, Target& t, const Settings& s,
                  ID3D11ShaderResourceView* sceneSRV);
    // M44c: プリフィルタ → 円盤ギャザー → 合成で t.sceneB を作る。
    // 実行しなかった (radius 0 / depthSRV 無し / シェーダ未コンパイル) 場合は false
    bool RunDof(GraphicsDevice& device, ShaderManager& shaders, Target& t, const Settings& s,
                const RenderView& view);
    // M43b: 空マスク → 太陽へ向けた放射ブラー ×2 を行い、結果を t.godA (半解像度) に残す。
    // 実行しなかった (intensity 0 / 太陽が背面・画面外 / depthSRV 無し等) 場合は false
    bool RunGodray(GraphicsDevice& device, ShaderManager& shaders, Target& t, const Settings& s,
                   const RenderView& view);
    // M44b: 輝度ヒストグラム収集 → 縮約 CS で t.exposureBuf[0] を更新する。
    // 実行しなかった (off / CS 未コンパイル / バッファ無し) 場合は false
    bool RunAutoExposure(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                         const Settings& s, ID3D11ShaderResourceView* sceneSRV);
    // M44d: 深度再投影モーションブラー (inputSRV → dst)。
    // 実行しなかった (off / prevViewProj 無効 / depthSRV 無し) 場合は false
    bool RunMotionBlur(GraphicsDevice& device, ShaderManager& shaders, const Settings& s,
                       const RenderView& view, ID3D11ShaderResourceView* inputSRV,
                       RenderTexture& dst);

    GraphicsDevice* device_ = nullptr;
    bool ready_ = false;

    AssetID tonemapShader_ = {};
    AssetID brightShader_ = {};
    AssetID blurShader_ = {};
    AssetID fxaaShader_ = {};
    AssetID godrayMaskShader_ = {}; // M43b
    AssetID godrayBlurShader_ = {};
    AssetID histCS_ = {}; // M44b
    AssetID histReduceCS_ = {};
    AssetID dofPrefilterShader_ = {}; // M44c
    AssetID dofGatherShader_ = {};
    AssetID dofCompositeShader_ = {};
    AssetID motionBlurShader_ = {}; // M44d

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;     // PostFx CB (tonemap)
    Microsoft::WRL::ComPtr<ID3D11Buffer> brightCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> blurCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> fxaaCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> godrayMaskCB_; // M43b
    Microsoft::WRL::ComPtr<ID3D11Buffer> godrayBlurCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> histCB_; // M44b
    Microsoft::WRL::ComPtr<ID3D11Buffer> aeReduceCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> dofCB_; // M44c (3 パス共通)
    Microsoft::WRL::ComPtr<ID3D11Buffer> mbCB_;  // M44d
    GpuTimer resolveTimer_; // M44d: Resolve 全体の GPU 時間
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
