#pragma once
#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Renderer/GpuTimer.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/VolumeTexture.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// M57b: フロクセルへの注入パラメータ。
// M57c で `CameraPostFxComponent` の froxelDensity / froxelAnisotropy から供給される
// (**このサブではまだコンポーネントを触らない** — 統合契約 予約 4 が M57c の枠と
// 決めているので、先回りして末尾 append すると M56 と番号を取り合う)。
// 既定値は予約表に書かれた値と同じにしてある = M57c で意味が変わらない
struct FroxelSettings {
    float density = 0.02f;      // 基準の消散係数 σ_t [1/m] (高度スケール前)
    float anisotropy = 0.3f;    // HG 位相関数の g (>0 = 前方散乱)
    float scatterAlbedo = 0.9f; // σ_s / σ_t。1 = 吸収なし (煙は 0.9 前後、霧はほぼ 1)
    // グリッドの奥行きの上限 [m]。カメラの far (既定 1000m) をそのまま使うと
    // 64 スライスが遠景に食われて近景が粗くなる。**視認できる霧の距離**で切る
    float maxDistance = 64.0f;
    // false = シャドウアトラスを読まない (影なし)。`--froxel-dump` の A/B 専用で、
    // 通常経路は常に true。「ビームが本当にアトラス由来か」を数値で示す唯一の口
    bool useLocalShadows = true;
    // ---- M57c ----
    // テンポラル蓄積 (深度スライスジッタ + 履歴の再投影)。
    // ★ジッタと履歴は**必ずセット**で切り替える。ジッタだけ入れると霧が毎フレーム
    //   奥行き方向に脈打つだけになる (M55d のカメラジッタと TAA の関係と同じ)。
    //   false のときは代表点が厳密に 0.5 = M57b とビット一致する
    bool temporal = true;
    float temporalFeedback = 0.9f; // 履歴の残し率 [0, froxel::kMaxTemporalFeedback]
};

// 注入結果の統計 (`--froxel-dump N` の読み戻し 1 回ぶんを CPU で集計したもの)。
// 消費者 (積分 = M57c、合成 = M57e) がまだ居ないサブなので、
// **グリッドに何が入ったかを機械で言える口はこれしかない**
struct FroxelVolumeStats {
    int cells = 0; // グリッドの総セル数
    // 局所ライトが届いたセル。判定は「**ボリューム内の最小値**より明るいか」で、
    // 0 との比較ではない — アンビエントの等方散乱が全セルに乗っているので、
    // 0 と比べると常に 100% になって何も測れない。
    // ★ハイトフォグ (fogHeightFalloff > 0) のシーンでは密度が場所ごとに違い、
    //   アンビエントの床も一定でなくなるのでこの指標は飽和する (実測 99.98%)。
    //   そういうシーンで意味があるのは DebugDumpAB の「影で暗くなったセル数」のほう
    int litCells = 0;
    float minInscatter = 0.0f; // = アンビエントの床 (一様密度なら全セル同値)
    float maxInscatter = 0.0f;
    double sumInscatter = 0.0; // 平均は sum/cells (0 セルでも壊れないよう double)
    float minExtinction = 0.0f;
    float maxExtinction = 0.0f;
    // 光っているセルの外接箱 (セル座標)。lit が 0 のときは min > max のまま
    int litMin[3] = { 0, 0, 0 };
    int litMax[3] = { -1, -1, -1 };
};

// M57b: 密度注入 + 局所ライト散乱注入のコンピュートパス。
//
// このサブの時点では **1 パスしか無く、書いた結果を読む者も居ない**。
// 積分 (M57c) と最終画像への合成 (M57e) が入るまで、絵は 1 ビットも変わらない。
// それでも実体をここに作るのは、「注入のコストが WARP で許容範囲か」が
// M57c の設計 (テンポラルを入れるか / golden を CI に載せるか) の入力になるため。
//
// ★VolumeTexture (M57a) の Create/Resize/Release の作法をそのまま使う。
//   Resize は同寸なら no-op なので毎フレーム呼んでよい (RenderTexture と同じ流儀)。
// ★遅延 Init: `RenderSystem::enableFroxel` が立つまで 7MB のボリュームも
//   シェーダも作らない (ShadowAtlas の 64MB と同じ理由)
class FroxelPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return inited_; }

    // M57c: 1 フレームぶん (注入 → テンポラル → 前方積分) をまわす。
    // 戻り値 = 積分結果の SRV。null = 走らなかった (呼び出し側は霧なしで進む)。
    // ★M57c の時点でこの SRV を読む者はまだ居ない (合成は M57d/M57e)。
    //   絵は 1 ビットも変わらない — 中身の検査は `--froxel-dump` の読み戻しが担当する
    ID3D11ShaderResourceView* Render(GraphicsDevice& device, ShaderManager& shaders,
                                     const RenderView& view, const SceneLightData& lights,
                                     const FroxelSettings& settings);

    // グリッドへ密度と局所ライトの散乱を注入する。戻り値 = 実際にディスパッチしたか。
    // false になるのは「シェーダが無い」「ビューが 0 サイズ」「正射影」のいずれか
    // (正射影の視錐台はスラブなので、この深度スライスの定義が成り立たない)。
    // sliceJitter = セル中心のスライス方向オフセット [0,1)。0.5 = ジッタ無し
    bool Inject(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                const SceneLightData& lights, const FroxelSettings& settings, float sliceJitter);

    // 注入結果 (単位長あたりの rgb = 内向き散乱 / a = 消散係数)
    ID3D11ShaderResourceView* ScatterSRV() const { return scatter_.SRV(); }
    const VolumeTexture& Volume() const { return scatter_; }
    // M57c: 前方積分の結果 (rgb = 積算した内向き散乱 / a = そこまでの透過率)。
    // 格納規約はテクセル z = 「スライス z の**奥端**まで」— サンプルは
    // froxel::IntegratedSampleW で半テクセル手前を指すこと
    ID3D11ShaderResourceView* IntegratedSRV() const { return integrated_.SRV(); }
    const VolumeTexture& Integrated() const { return integrated_; }

    // 直近のディスパッチの GPU 時間 (ProfilerWindow 表示用)
    float InjectGpuMs() const { return timer_.Milliseconds(); }
    float TemporalGpuMs() const { return temporalTimer_.Milliseconds(); }
    float IntegrateGpuMs() const { return integrateTimer_.Milliseconds(); }
    int CellCount() const { return scatter_.Width() * scatter_.Height() * scatter_.Depth(); }
    // 直近フレームでテンポラルが履歴を混ぜられたか / そのとき使ったジッタ
    bool LastHistoryValid() const { return lastHistValid_; }
    float LastSliceJitter() const { return lastJitter_; }

    // 全セルを読み戻して集計する (デバッグ専用。GPU を完全に待たせる)。
    // rawOut を渡すと読み戻した生の rgba 列もそこへ残す (A/B の差分用)
    bool ReadbackStats(GraphicsDevice& device, FroxelVolumeStats& out,
                       std::vector<float>* rawOut = nullptr) const;

    // M57c: 履歴を捨てる (次のテンポラルが histValid=0 で走る)。
    // リサイズとビュー切り替えのほか、`--froxel-dump` の検査が「1 フレーム目」を
    // 何度も作り直すのに使う
    void ResetHistory();

    // `--froxel-dump N`: 影あり / 影なしを同じ実行の中で 2 回注入して読み戻し、
    // 統計と差分をログへ出す。**このサブでグリッドの中身を確かめられる唯一の口**
    // (積分も合成もまだ無いので、絵には 1 画素も出てこない)
    void DebugDumpAB(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                     const SceneLightData& lights, const FroxelSettings& settings);

private:
    // M57c: テンポラルの 1 ディスパッチ。戻り値 = 出力先ボリューム (null = 走らなかった)。
    // prevViewProj / prevValid を**引数で受け取る**のは、`--froxel-dump` の検査が
    // 「カメラが 1 ミリも動いていない前フレーム」を明示的に作って再投影の同一性を
    // 測れるようにするため (view.prevViewProj は実際のカメラ運動に依存してしまう)
    VolumeTexture* Temporal(GraphicsDevice& device, ShaderManager& shaders,
                            const RenderView& view, const FroxelSettings& settings,
                            float sliceJitter, uint32_t frameSerial,
                            const DirectX::XMFLOAT4X4& prevViewProj, bool prevValid,
                            ID3D11ShaderResourceView* currentSRV);
    // M57c: 前方積分の 1 ディスパッチ (src の Z 列を手前から舐めて integrated_ へ)
    bool Integrate(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                   ID3D11ShaderResourceView* srcSRV);
    // 積分 / テンポラルが共有する CB を組む (グリッドの幾何は Inject が確定させた値を使う)
    void FillPostCB(const RenderView& view, float sliceJitter, float feedback, bool histValid,
                    const DirectX::XMFLOAT4X4& prevViewProj, void* out) const;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> postCb_; // M57c: temporal / integrate 共有
    // SampleShadowAtlas (common.hlsli) が要求する比較サンプラ。
    // ★統合契約 予約 2 の「サンプラは増やさない」はピクセルシェーダのスロットの話で、
    //   CS は別のバインド空間 (M55f が rt_temporal の t7 で確認済みの理屈と同じ)。
    //   ここで s0 を取っても Deferred 光パス / Forward の s0 とは一切干渉しない
    Microsoft::WRL::ComPtr<ID3D11SamplerState> shadowSampler_;
    // M57c: 履歴の再投影サンプラ (LINEAR / CLAMP)。CLAMP でよいのは、グリッドの外は
    // シェーダ側で先に弾いていて端の値を引く経路が無いため
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearClamp_;
    VolumeTexture scatter_;
    // M57c: 前方積分の出力。**ビュー間で共有する** — 消費者 (M57d/M57e) は同じ
    // Render の中で読み切るので、SceneView と GameView が上書きし合っても問題ない。
    // 履歴と違って「前フレームの自分」を要求しないので viewKey 別に持つ意味が無い
    VolumeTexture integrated_;
    // M57c: viewKey (0=AssetPreview 1=runtime 2=SceneView 3=GameView) 毎の履歴。
    // ★(w,h) キーではなく viewKey キーで持つ — SceneView と GameView が同寸のときに
    //   履歴を食い合って混線する (TaaPass / RtPasses::kHistorySlots と同じ理由。3 度目)。
    //   グリッド寸法は固定 (froxel::kGrid*) なのでリサイズによる破棄は起きない
    static constexpr int kHistorySlots = 4;
    struct History {
        VolumeTexture vol[2];   // ping-pong。write = 今フレームの書き込み先
        int write = 0;
        uint32_t lastSerial = 0;
        bool hasLast = false;
    };
    History hist_[kHistorySlots];
    GpuTimer timer_;
    GpuTimer temporalTimer_;
    GpuTimer integrateTimer_;
    AssetID injectCS_ = {};
    AssetID temporalCS_ = {};
    AssetID integrateCS_ = {};
    // Inject が確定させたグリッドの深度範囲。テンポラル / 積分 / CPU 参照実装が
    // **同じ値**を使わないと、スライスの厚みが食い違って透過率がずれる
    float gridNearZ_ = 0.0f;
    float gridFarZ_ = 0.0f;
    float lastJitter_ = 0.5f;
    bool lastHistValid_ = false;
    bool inited_ = false;
    bool orthoWarned_ = false; // 正射影のビューで毎フレーム WARN を出さないため
};

} // namespace mye
