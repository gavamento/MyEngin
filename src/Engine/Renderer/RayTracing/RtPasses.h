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

// GI パスの出力 (M46d/M46e)。デノイズの段階ごとに参照できるよう 3 つ返す。
// accumulated の a には履歴長、filtered の a には推定分散が入っている
struct RtGiResult {
    ID3D11ShaderResourceView* raw = nullptr;         // 1spp そのまま
    ID3D11ShaderResourceView* accumulated = nullptr; // 蓄積後 (テンポラル off なら raw と同じ)
    ID3D11ShaderResourceView* filtered = nullptr;    // SVGF 後 (off なら accumulated と同じ)
};

// レイトレーシングのコンピュートパス群 (M46b: デバッグ表示 / M46c: 拡散 GI /
// M46d: テンポラル蓄積 / M46e: SVGF 空間フィルタ)。Renderer 層 = 生の D3D11 はここに閉じる
class RtPasses {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return inited_; }

    // 拡散 GI を内部解像度で 1spp 計算し、続けてテンポラル蓄積を掛ける。
    // 出力は albedo を掛けない入射放射輝度 (IBL irradiance と同次元)
    RtGiResult RenderGi(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                        const RtFrameInputs& in);

    // デバッグ表示を view.rtv へ上書きする。描いたら true。
    // gi は rtDebugMode>=4 (GI 系表示) のときだけ使う
    bool RenderDebug(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                     const RtFrameInputs& in, const RtGiResult& gi);

    // 直近の GPU 時間 (ProfilerWindow 表示用)
    float DebugGpuMs() const { return debugTimer_.Milliseconds(); }
    float GiGpuMs() const { return giTimer_.Milliseconds(); }
    float TemporalGpuMs() const { return temporalTimer_.Milliseconds(); }
    // M46e: 分散推定 + A-Trous 全反復の合計
    float SvgfGpuMs() const { return svgfTimer_.Milliseconds(); }

private:
    // viewKey (0=AssetPreview 1=runtime 2=SceneView 3=GameView) 毎に履歴を分ける。
    // これが無いと SceneView と GameView が互いの履歴を食い合って混線する
    static constexpr int kHistorySlots = 4;

    // テンポラル蓄積の履歴 (ping-pong)。color = rgb 蓄積 GI + a 履歴長 /
    // geom = xyz ワールド法線 + w カメラ距離 (再投影の妥当性判定に使う) /
    // moments = x 輝度 μ + y μ² (M46e: SVGF の分散推定)
    struct GiHistory {
        RenderTexture color[2];
        RenderTexture geom[2];
        RenderTexture moments[2];
        int write = 0; // 今フレームの書き込み先 index (読みは 1-write)
        int w = 0;
        int h = 0;
        uint32_t lastSerial = 0; // 最後に書いたフレームのビュー通番
        bool hasLast = false;
    };

    // Accumulate が今フレーム書き込んだ面 (SVGF の入力)。null = 蓄積を走らせなかった
    struct AccumResult {
        ID3D11ShaderResourceView* color = nullptr;
        ID3D11ShaderResourceView* geom = nullptr;
        ID3D11ShaderResourceView* moments = nullptr;
    };

    // t0-t6 / b0-b1 / s0 (シーン + 環境) をコンピュートステージへバインドする
    void BindCommon(GraphicsDevice& device, const RenderView& view, const RtFrameInputs& in);
    void UnbindCompute(GraphicsDevice& device);
    // 1spp の結果に履歴を混ぜる。戻り値の color が null なら走らせていない
    AccumResult Accumulate(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                           const RtFrameInputs& in, int gw, int gh);
    // M46e: 分散推定 + A-Trous ×kRtAtrousIterations。戻り値 = 最終出力の SRV (null = 走らせず)。
    // 幾何バッファ (法線 + カメラ距離) が蓄積パスの副産物なので、テンポラル off では動かない
    ID3D11ShaderResourceView* Denoise(GraphicsDevice& device, ShaderManager& shaders,
                                      const RenderView& view, const AccumResult& acc, int gw,
                                      int gh);
    // src を view.rtv 全面に貼る (mode 1 = a を履歴長 / 2 = a を分散のヒートマップとして表示)
    bool Blit(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
              ID3D11ShaderResourceView* src, int mode = 0, float param = 0.0f);

    RenderTexture debugRt_;   // デバッグ CS の出力先 (フル解像度、UAV 付き)
    RenderTexture giRt_;      // GI の出力先 (内部解像度、UAV 付き)
    RenderTexture svgfRt_[2]; // M46e: 分散推定 + A-Trous の ping-pong (内部解像度)
    GiHistory giHist_[kHistorySlots];
    AssetID debugCS_ = {};
    AssetID giCS_ = {};
    AssetID temporalCS_ = {};
    AssetID varianceCS_ = {};
    AssetID atrousCS_ = {};
    AssetID blitShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> envCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> debugCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> giCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> temporalCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> varianceCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> atrousCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> blitCB_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearClamp_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    GpuTimer debugTimer_;
    GpuTimer giTimer_;
    GpuTimer temporalTimer_;
    GpuTimer svgfTimer_;
    bool inited_ = false;
};

} // namespace mye
