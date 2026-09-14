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
    ID3D11ShaderResourceView* gbMaterial = nullptr; // M46h: r=metallic g=roughness
    ID3D11ShaderResourceView* skyCube = nullptr;    // skyMode==1 のときのみ
    // M55f: 画面速度 (RT4)。テンポラル蓄積の履歴 UV に使う。null = 前フレーム VP への
    // 射影 (M46d) へ縮退する
    ID3D11ShaderResourceView* gbVelocity = nullptr;
};

// GI パスの出力 (M46d/M46e)。デノイズの段階ごとに参照できるよう 3 つ返す。
// accumulated の a には履歴長、filtered の a には推定分散が入っている
struct RtGiResult {
    ID3D11ShaderResourceView* raw = nullptr;         // 1spp そのまま
    ID3D11ShaderResourceView* accumulated = nullptr; // 蓄積後 (テンポラル off なら raw と同じ)
    ID3D11ShaderResourceView* filtered = nullptr;    // SVGF 後 (off なら accumulated と同じ)
};

// M46h: 反射パスの出力。GI と同じ 2 段 (蓄積 → A-Trous) を通す
struct RtReflResult {
    ID3D11ShaderResourceView* raw = nullptr;      // 1spp そのまま
    ID3D11ShaderResourceView* filtered = nullptr; // デノイズ後 (off なら raw と同じ)
    // M67d: ReSTIR のデバッグ表示 (12 = M / 14 = 反射像側のクラス) が読む面。
    // 実体は **今フレームの面** `slot.set[slot.write]` の rad (a = M) と nrm (rgb = ns, a = cls)
    // = rt_refl がこのフレームに書いた reservoir。spatial は reservoir を書き戻さないので、
    // 「今フレームの reservoir」はここ以外に存在しない。
    // **ReSTIR off なら null** — 消費側 (RenderDebug) は Blit が null で false を返すのに任せる
    ID3D11ShaderResourceView* reservoirM = nullptr;
    ID3D11ShaderResourceView* reservoirCls = nullptr;
};

// レイトレーシングのコンピュートパス群 (M46b: デバッグ表示 / M46c: 拡散 GI /
// M46d: テンポラル蓄積 / M46e: SVGF 空間フィルタ / M46g: 影 / M46h: 反射)。
// Renderer 層 = 生の D3D11 はここに閉じる
class RtPasses {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return inited_; }

    // 拡散 GI を内部解像度で 1spp 計算し、続けてテンポラル蓄積を掛ける。
    // 出力は albedo を掛けない入射放射輝度 (IBL irradiance と同次元)
    RtGiResult RenderGi(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                        const RtFrameInputs& in);

    // M46g: 太陽 (最初の平行光) の可視率をフル解像度で計算する。
    // 戻り値 = 可視率テクスチャ (R8、1 = 照らされる) の SRV。null = 走らせられなかった
    ID3D11ShaderResourceView* RenderShadow(GraphicsDevice& device, ShaderManager& shaders,
                                           const RenderView& view, const RtFrameInputs& in);

    // M46h: 鏡面反射を内部解像度で 1spp 計算し、蓄積 + A-Trous を掛ける。
    // 出力は IBL のプリフィルタ済み放射輝度と同次元 (合成側でそのまま差し替えられる)
    RtReflResult RenderReflection(GraphicsDevice& device, ShaderManager& shaders,
                                  const RenderView& view, const RtFrameInputs& in);

    // デバッグ表示を view.rtv へ上書きする。描いたら true。
    // gi / shadow / refl は、それぞれ rtdebug::NeedsGi / NeedsShadow / NeedsReflection のモードのときだけ使う
    bool RenderDebug(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                     const RtFrameInputs& in, const RtGiResult& gi,
                     ID3D11ShaderResourceView* shadow, const RtReflResult& refl);

    // 直近の GPU 時間 (ProfilerWindow 表示用)
    float DebugGpuMs() const { return debugTimer_.Milliseconds(); }
    float GiGpuMs() const { return giTimer_.Milliseconds(); }
    float TemporalGpuMs() const { return temporalTimer_.Milliseconds(); }
    // M46e: 分散推定 + A-Trous 全反復の合計
    float SvgfGpuMs() const { return svgfTimer_.Milliseconds(); }
    // M46g: 影レイ (フル解像度 1spp) と、その分離型空間フィルタ
    float ShadowGpuMs() const { return shadowTimer_.Milliseconds(); }
    float ShadowFilterGpuMs() const { return shadowFilterTimer_.Milliseconds(); }
    // M46h: 反射レイ (内部解像度 1spp) と、そのデノイズ (蓄積 + 分散推定 + A-Trous)
    float ReflGpuMs() const { return reflTimer_.Milliseconds(); }
    float ReflDenoiseGpuMs() const
    {
        return reflTemporalTimer_.Milliseconds() + reflSvgfTimer_.Milliseconds();
    }
    // M67d: ReSTIR の 2 パス目 (空間再利用 + resolve)。初期 reservoir の書き出しは
    // 反射レイと同じディスパッチなので ReflGpuMs() 側に含まれる (分離できない)
    float RestirGpuMs() const { return restirTimer_.Milliseconds(); }

private:
    // viewKey (0=AssetPreview 1=runtime 2=SceneView 3=GameView) 毎に履歴を分ける。
    // これが無いと SceneView と GameView が互いの履歴を食い合って混線する
    static constexpr int kHistorySlots = 4;

    // テンポラル蓄積の履歴 (ping-pong)。color = rgb 蓄積値 + a 履歴長 /
    // geom = xyz ワールド法線 + w カメラ距離 (再投影の妥当性判定に使う) /
    // moments = x 輝度 μ + y μ² (M46e: SVGF の分散推定)。
    // M46h: GI と反射がそれぞれ独立の組を持つ (片方だけ on でも成立させるため)
    struct RtHistory {
        RenderTexture color[2];
        RenderTexture geom[2];
        RenderTexture moments[2];
        int write = 0; // 今フレームの書き込み先 index (読みは 1-write)
        int w = 0;
        int h = 0;
        uint32_t lastSerial = 0; // 最後に書いたフレームのビュー通番
        bool hasLast = false;
    };

    // M67d: reservoir 1 組 (spec §4.2 の 5 枚)。
    //   pos  = R32G32B32A32 (xyz = xs / w = W)。**fp32 が要る** — 半精度だと遠景で
    //          Jacobian の d² が狂う
    //   rad  = R16G16B16A16 (xyz = Ls / w = M。M は 32 までなので半精度で厳密)
    //   nrm  = R16G16B16A16 (xyz = ns (0 = スカイ) / w = cls)
    //   geom = R16G16B16A16 (xyz = 受け側 N / w = 受け側カメラ距離。RtHistory.geom と同型)
    //   rpos = R32G32B32A32 (xyz = 受け側ワールド座標 P / w = 予備)。temporal の
    //          厳密 Jacobian (U4) が P_from に使う
    struct RtReservoirSet {
        RenderTexture pos;
        RenderTexture rad;
        RenderTexture nrm;
        RenderTexture geom;
        RenderTexture rpos;
    };

    // viewKey 別の reservoir スロット。**RtHistory と同じ ping-pong** (M67f) —
    // rt_refl は set[1-write] (前フレーム) を読んで set[write] へ書き、spatial は
    // set[write] を**読むだけ** (reservoir を書き戻さない)。フレーム末に write を flip。
    // 読む側と書く側が常に別テクスチャなのは変わらない = typed UAV load 不要 (U5)。
    // ★「set[0] 固定 = spatial が書き戻す」にしてはいけない — 近傍の履歴が自画素の履歴へ
    //   混ざり、M の重いクラスのサンプルが 1 フレームに半径ぶんずつ拡散する (sub-06 round 1 で実測)
    struct RtReservoirSlot {
        RtReservoirSet set[2];
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
    // 1spp の結果 (src) に履歴を混ぜる。戻り値の color が null なら走らせていない。
    // hist / maxHistory / timer は信号ごと (GI と反射) に別のものを渡す
    AccumResult Accumulate(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                           const RtFrameInputs& in, int gw, int gh,
                           ID3D11ShaderResourceView* src, RtHistory& hist, float maxHistory,
                           GpuTimer& timer);
    // M46e: 分散推定 + A-Trous ×iterations。戻り値 = 最終出力の SRV (null = 走らせず)。
    // 幾何バッファ (法線 + カメラ距離) が蓄積パスの副産物なので、テンポラル off では動かない。
    // pp は信号ごとに別の ping-pong を渡す (GI の結果はライトパスまで生かす必要があるため)
    ID3D11ShaderResourceView* Denoise(GraphicsDevice& device, ShaderManager& shaders,
                                      const RenderView& view, const AccumResult& acc, int gw,
                                      int gh, RenderTexture (&pp)[2], int iterations,
                                      float sigmaLuma, GpuTimer& timer);
    // src を view.rtv 全面に貼る (mode 1 = a を履歴長 / 2 = a を分散のヒートマップとして表示 /
    // 4 = a を ReflectionClass の色として表示)
    bool Blit(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
              ID3D11ShaderResourceView* src, int mode = 0, float param = 0.0f);
    // M67d: reservoir 5 枚 × 2 組を (必要になった時点で) 確保する。
    // サイズが変わったら hasLast を落とす。false = 確保に失敗 = ReSTIR を諦める
    bool EnsureReservoirs(GraphicsDevice& device, RtReservoirSlot& slot, int gw, int gh);
    // M67d: ReSTIR の 2 パス目。rt_refl が今フレームに書いた面 (t11-t15) を読み、
    // 解決した反射放射輝度を reflRestirRt_ (u0) へ。**reservoir は書き戻さない**
    // (UAV は u0 の 1 本だけ) — 書き戻すと近傍の候補が伝播して Prop が 40 フレームで
    // 画面の 94% を占拠する (M67f 実測、ADR-016)。false = 走らせられなかった
    bool RenderRestirSpatial(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                             const RtFrameInputs& in, RtReservoirSlot& slot, int gw, int gh);

    RenderTexture debugRt_;   // デバッグ CS の出力先 (フル解像度、UAV 付き)
    RenderTexture giRt_;      // GI の出力先 (内部解像度、UAV 付き)
    RenderTexture svgfRt_[2]; // M46e: 分散推定 + A-Trous の ping-pong (内部解像度)
    // M46g: 影の可視率 (フル解像度 R8)。[0] にレイトレ結果、以降フィルタで ping-pong
    RenderTexture shadowRt_[2];
    // M46h: 反射 (内部解像度)。GI の結果はライトパスまで t9 で生きているので
    // ping-pong を共有できない = 専用に持つ
    RenderTexture reflRt_;
    RenderTexture reflSvgfRt_[2];
    // M67d: ReSTIR の resolve 結果 (内部解像度)。**reflRt_ とは別に持つ** —
    // reflRt_ は生の 1spp (デバッグ 10) として同じフレーム内で生きているため
    RenderTexture reflRestirRt_;
    RtHistory giHist_[kHistorySlots];
    RtHistory reflHist_[kHistorySlots];
    // M67d: reservoir (遅延確保。ReSTIR を一度も使わないなら 1 バイトも取らない)
    RtReservoirSlot reservoirs_[kHistorySlots];
    AssetID debugCS_ = {};
    AssetID giCS_ = {};
    AssetID temporalCS_ = {};
    AssetID varianceCS_ = {};
    AssetID atrousCS_ = {};
    AssetID shadowCS_ = {};
    AssetID shadowFilterCS_ = {};
    AssetID reflCS_ = {};
    AssetID restirCS_ = {}; // M67d: rt_refl_restir_spatial.cs
    AssetID blitShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> envCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> debugCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> giCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> temporalCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> varianceCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> atrousCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> shadowCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> shadowFilterCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> reflCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> restirCB_; // M67d (b3、rt_refl と spatial で共有)
    Microsoft::WRL::ComPtr<ID3D11Buffer> blitCB_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearClamp_;
    // M67d: デバッグ表示 (12 / 14) が reservoir を等倍でない画面へ貼るときに使う。
    // クラス番号や M を線形補間すると境界に「存在しないクラスの色」が出るので点サンプル
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointClamp_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    GpuTimer debugTimer_;
    GpuTimer giTimer_;
    GpuTimer temporalTimer_;
    GpuTimer svgfTimer_;
    GpuTimer shadowTimer_;
    GpuTimer shadowFilterTimer_;
    GpuTimer reflTimer_;
    GpuTimer reflTemporalTimer_;
    GpuTimer reflSvgfTimer_;
    GpuTimer restirTimer_; // M67d
    bool inited_ = false;
};

} // namespace mye
