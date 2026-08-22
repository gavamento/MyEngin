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

    // グリッドへ密度と局所ライトの散乱を注入する。戻り値 = 実際にディスパッチしたか。
    // false になるのは「シェーダが無い」「ビューが 0 サイズ」「正射影」のいずれか
    // (正射影の視錐台はスラブなので、この深度スライスの定義が成り立たない)
    bool Inject(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                const SceneLightData& lights, const FroxelSettings& settings);

    // 注入結果。M57c のテンポラルと積分が読む (今は --froxel-dump だけ)
    ID3D11ShaderResourceView* ScatterSRV() const { return scatter_.SRV(); }
    const VolumeTexture& Volume() const { return scatter_; }

    // 直近のディスパッチの GPU 時間 (ProfilerWindow 表示用)
    float InjectGpuMs() const { return timer_.Milliseconds(); }
    int CellCount() const { return scatter_.Width() * scatter_.Height() * scatter_.Depth(); }

    // 全セルを読み戻して集計する (デバッグ専用。GPU を完全に待たせる)。
    // rawOut を渡すと読み戻した生の rgba 列もそこへ残す (A/B の差分用)
    bool ReadbackStats(GraphicsDevice& device, FroxelVolumeStats& out,
                       std::vector<float>* rawOut = nullptr) const;

    // `--froxel-dump N`: 影あり / 影なしを同じ実行の中で 2 回注入して読み戻し、
    // 統計と差分をログへ出す。**このサブでグリッドの中身を確かめられる唯一の口**
    // (積分も合成もまだ無いので、絵には 1 画素も出てこない)
    void DebugDumpAB(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                     const SceneLightData& lights, const FroxelSettings& settings);

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    // SampleShadowAtlas (common.hlsli) が要求する比較サンプラ。
    // ★統合契約 予約 2 の「サンプラは増やさない」はピクセルシェーダのスロットの話で、
    //   CS は別のバインド空間 (M55f が rt_temporal の t7 で確認済みの理屈と同じ)。
    //   ここで s0 を取っても Deferred 光パス / Forward の s0 とは一切干渉しない
    Microsoft::WRL::ComPtr<ID3D11SamplerState> shadowSampler_;
    VolumeTexture scatter_;
    GpuTimer timer_;
    AssetID injectCS_ = {};
    bool inited_ = false;
    bool orthoWarned_ = false; // 正射影のビューで毎フレーム WARN を出さないため
};

} // namespace mye
