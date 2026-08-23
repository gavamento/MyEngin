#pragma once
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/GpuTimer.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// HZB = 階層 Z バッファ (min-Z ピラミッド)。M56c。
//
// 「ミップ n のテクセル 1 個 = 対応する画面領域の**最も手前**の深度」を持つ縮小列で、
// SSR (M56d) の光線行進が「この領域には何も無い」を 1 サンプルで判定して大股で進むための
// 加速構造。消費者が現れるのは M56d なので、このサブでは**デバッグ表示だけが唯一の目視口**
// になる (M55c の velocity と同じ立ち位置)。
//
// ★**`RenderTexture` にミップを足していない**。`RenderTexture::Create` の `MipLevels = 1` は
//   GBuffer 5 枚 / postfx 中間 / SceneView RT / RT パスが全部使う共有クラスの固定値で、
//   触ると影響範囲が全描画になる。ここは RTV も DSV も要らない (CS が UAV で書き SRV で読む
//   だけ) ので、素の `ID3D11Texture2D` をこのクラスが自前で持つ方が安く安全。
//
// ★**段ごとに SRV と UAV を 1 枚ずつ作る**。同じテクスチャの「ミップ n-1 を SRV で読み
//   ミップ n を UAV で書く」は、ビューが**別サブリソース**を指していれば D3D11 で合法。
//   全ミップを覆う `srv_` を読み書き中に張ると同一サブリソースの二重バインドになるので、
//   縮小中は必ず `mipSrv_[n-1]` の方を張ること。
//
// 深度の素性: 入力は `RenderView::depthSRV` (R24_UNORM_X8、**非線形のデバイス深度** [0,1]、
// クリア値 1.0 = 最遠)。ピラミッドも同じ非線形深度をそのまま持つ — 線形化は消費者の仕事で、
// `common.hlsli` の共有 `LinearizeDepth` を near/far と一緒に呼べばよい。
// **min を取る向きが正しいのは深度が「小さいほど手前」だから** (このリポジトリは reversed-Z を
// 使っていない。全 `ClearDepthStencilView` が 1.0 でクリアしているのが根拠)。
class HzbPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();

    // depthSRV から min-Z ピラミッドを 1 段ずつ作る。
    // 戻り値 false = 作れなかった (SRV が null / CS 未コンパイル / 確保失敗) —
    // **消費者は SRV() の null で自然に無効化される**ので、呼び出し側でログを撒かないこと
    // (毎フレーム走るパスなので失敗が続くとログが溢れる)。
    bool Build(GraphicsDevice& device, ShaderManager& shaders, ID3D11ShaderResourceView* depthSRV,
               int width, int height);

    // ピラミッド全段の SRV (null = まだ 1 度も作れていない)
    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }
    int MipCount() const { return mipCount_; }
    // 直近の Build の GPU 時間 (ProfilerWindow 表示用)。作っていないフレームは前の値が残る
    float GpuMs() const { return timer_.Milliseconds(); }

private:
    bool EnsurePyramid(GraphicsDevice& device, int width, int height);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_; // 全ミップ (消費者用)
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> mipSrv_; // 段ごと (読む側)
    std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUav_; // 段ごと (書く側)
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    AssetID reduceCS_ = {};
    GpuTimer timer_;
    int width_ = 0;
    int height_ = 0;
    int mipCount_ = 0;
};

// ---- 以下は純関数 (GPU も device も要らない)。HzbSelfTest が直接検査する ----

// hzb_reduce.cs.hlsl の numthreads と同一。**HLSL 側の MYE_HZB_TG と必ず一致させること** —
// 食い違うと画面の右端・下端だけが縮小されずに前フレームの値が残る (絵は出るのに間違っている)。
// tools\check_rules.ps1 の規則 9 が一致を静的に検査する
constexpr int kHzbThreadGroupSize = 8;

// width x height のピラミッドの段数 (1x1 まで数える)。0 以下なら 0。
// D3D11 が自動で作るミップ列の長さ (floor(log2(max(w,h))) + 1) と**必ず同じ**になる形で
// 数えている — ここがずれると `CreateTexture2D` の `MipLevels` と段ごとのビューが食い違う
int HzbMipCount(int width, int height);

// 段 level の 1 辺の長さ。D3D のミップ規則そのもの (floor 半減・最小 1)
int HzbMipExtent(int base, int level);

// 縮小の被覆区間: 出力テクセル dstIndex が読むべき入力テクセルの範囲 [begin, end] (両端含む)。
//
// ★**これが HZB で一番静かに壊れるところ**。奇数辺 (例 15 → 7) を素朴に 2x2 で畳むと
//   最後の 1 行 / 1 列が**どの出力テクセルにも入らない**。「そこに壁があるのに HZB が
//   空だと言う」= SSR の光線が壁を貫通する、という形でしか現れず、絵を見ても原因が分からない。
//   区間を [i*src/dst, (i+1)*src/dst) の切り上げ側へ広げて、取りこぼしゼロを保証する
//   (代償は奇数段での 3 テクセル読み = 重なり。min なので重なっても結果は変わらない)。
//
// src == dst (= ミップ 0 への素通しコピー) なら 1 テクセルに退化する — おかげで
// 「コピー用の CS」を別に持たずに 1 本のシェーダで全段を回せる
void HzbReduceSpan(int dstIndex, int srcExtent, int dstExtent, int& begin, int& end);

} // namespace mye
