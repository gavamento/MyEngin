#pragma once
#include "Engine/Renderer/RenderTypes.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
struct RenderResources;

// レンダリングパスの差し替え点 (M1 で ForwardPath、M6.5 で DeferredPath)。
// Engine 層はこのインターフェースのみに依存する。
// パスは「ソート済みキューを受け取り提出する」だけで、収集は RenderSystem の仕事。
class IRenderPath {
public:
    virtual ~IRenderPath() = default;
    virtual const char* Name() const = 0;
    virtual bool Init(GraphicsDevice& device, ShaderManager& shaders) = 0;
    virtual void Shutdown() = 0;

    // view.rtv/dsv へ描画する (クリア込み)。queue はソート済みであること
    virtual void Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                        const SceneLightData& lights, RenderResources& resources,
                        ShaderManager& shaders) = 0;

    // ---- M55c/M55d: 画面速度バッファ (GBuffer RT4) ----
    // このパスが velocity を書くか。**Render を呼ぶ前に**知る必要がある —
    // TAA を有効化するかの判定 (= カメラジッタを載せるかの判定) がこれで決まるため。
    // false のパスでジッタだけ載せると、画面が毎フレーム半ピクセル揺れるだけになる
    virtual bool WritesVelocity() const { return false; }
    // 直近の Render が書いた velocity の SRV (null = 未生成 / このパスは書かない)。
    // 消費側 (TAA / モーションブラー v2 / RT) が自分でバインドする —
    // 「光パスの t0-t11 の並びは 1 つも動かさない」が M55c からの約束
    virtual ID3D11ShaderResourceView* VelocitySRV() const { return nullptr; }

    // ---- M57d: フロクセルの積分結果 (view.froxelSRV) をこのパスが合成するか ----
    // **Render を呼ぶ前に**知る必要がある: フロクセルが霧を持つフレームでは
    // ゴッドレイを自動で降ろす (フォグ三重計上の解消) ので、「降ろしたのに誰も霧を
    // 足さない」= 霧が減るだけ、という状態を作らないための門になる。
    // v1 は Deferred だけが true。Forward への配線 (t7) は M57e の枠
    virtual bool AppliesFroxel() const { return false; }
};

} // namespace mye
