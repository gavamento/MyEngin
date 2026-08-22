#pragma once
#include <DirectXMath.h>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/GpuTimer.h"
#include "Engine/Renderer/MeshInstancing.h"
#include "Engine/Renderer/RenderTypes.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
class RenderQueue;
struct RenderResources;

// 局所ライト (スポット/点) のシャドウアトラス (M54c)。
// 平行光の CSM (ShadowPass) が Texture2DArray なのに対し、こちらは **1 枚の大きな
// 深度テクスチャを正方タイルに割る**。理由は 2 つ:
//   ・ライト 1 本あたりの面数が違う (スポット=1 / 点光源=6、M54d) ので配列スライスでは
//     数が合わない。タイルなら「必要な枚数だけ取る」が自然に書ける。
//   ・サンプル側の SRV が 1 本で済む。統合契約 (plans/radiant-shimmering-lumen.md 付録
//     予約 2) が M54 に許した SRV スロットは Deferred t12 / Forward t6 の 1 本きり。
//
// 描画自体は ShadowPass と同じ最小 VS (shadow_depth / shadow_depth_instanced) を使う —
// あれは pos しか読まないので、正射影の lightVP を透視の lightVP に差し替えるだけで通る。
//
// ★ShadowPass と決定的に違うのは**深度バイアス**。ShadowPass の DepthBias=800 /
//   SlopeScaled=2.5 は正射影 (深度がビュー z に線形) 用に調整された値で、透視の非線形深度に
//   そのまま当てるとアクネかピーターパンのどちらかが必ず出る。ここは専用のラスタライザ
//   ステートを持ち、定数項をほぼ捨てて傾斜依存に寄せてある (実測は Init のコメント)。
class ShadowAtlas {
public:
    static constexpr int kDefaultResolution = 4096;
    static constexpr int kDefaultTileSize = 1024;

    // resolution / tileSize は 2 冪を前提 (割り切れないと端のタイルが欠ける)。
    // 生成する深度テクスチャは R32_TYPELESS 1 枚 = 4096^2 * 4B = 64MB。
    // **影を投げる局所ライトが 1 本も無いシーンでは呼ばれない** (RenderSystem が遅延 Init)
    bool Init(GraphicsDevice& device, ShaderManager& shaders, int resolution = kDefaultResolution,
              int tileSize = kDefaultTileSize);
    bool IsReady() const { return ready_; }

    int Resolution() const { return resolution_; }
    int TileSize() const { return tileSize_; }
    // 実際に配れるタイル数 = (resolution/tileSize)^2 を kMaxShadowTiles で頭打ちにした値
    int TileCapacity() const { return capacity_; }

    // タイル index → アトラス内の矩形 + UV 変換 (純関数。Init 前でも既定値で答えられるよう
    // resolution/tileSize を引数に取る形にはせず、Init 後に呼ぶ前提)。
    // 並びは行優先 (左上から右へ) — **決定論キーで並べたライト順にそのまま前詰めする**ので、
    // シーンが変わらなければ frame をまたいで同じライトが同じ枠に落ちる (影のポップ防止)
    void FillTileRect(int index, ShadowTile& tile) const;

    // tiles[0..count) の lightViewProj で不透明キューをそれぞれのタイルへ描く。
    // アトラス全体を 1 回クリアしてからタイル毎に RSSetViewports する
    // (タイル外は深度 1.0 = 「一番遠い」= サンプルしても影にならない)。
    void Render(GraphicsDevice& device, ShaderManager& shaders, const RenderQueue& queue,
                RenderResources& resources, const ShadowTile* tiles, int count,
                bool instancing = true);

    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); } // Texture2D (R32_FLOAT)

    // ---- 直近 Render の計測 (M54d、ProfilerWindow 行)。描画専用の統計 ----
    float GpuMs() const { return timer_.Milliseconds(); }
    int DrawnTiles() const { return drawnTiles_; }   // 実際に描いたタイル数 (面カリング後)
    int DrawCalls() const { return drawCalls_; }     // 発行した DrawIndexed(Instanced) 数
    int CulledDraws() const { return culledDraws_; } // タイル毎の視錐台カリングで省いた分

private:
    bool ready_ = false;
    int resolution_ = 0;
    int tileSize_ = 0;
    int tilesPerRow_ = 0;
    int capacity_ = 0;
    AssetID depthShader_ = {};
    AssetID depthInstancedShader_ = {};
    // インスタンシング (M38f と同じ流儀)。run はタイル間で共通なので充填は 1 回だけ
    MeshInstanceBuffer instanceBuf_;
    std::vector<uint8_t> canInstance_;
    std::vector<MeshInstanceRun> runs_;
    std::vector<DirectX::XMFLOAT4X4> worlds_;
    // ★タイル毎の視錐台カリング用 world AABB キャッシュ (M54d)。
    //   点光源 1 本 = 6 面なので、タイル数は M54c の数倍まで増える。キューを毎タイル
    //   舐め直すのは避けられないが、AABB の再計算 (mesh 取得 + 絶対値 3x3) までタイル数倍に
    //   するのは無駄なので 1 回だけ作って使い回す。run は「全要素が枠外なら run ごと飛ばす」
    //   ための合併 AABB を持つ (途中だけ落とすと run が分割できず描画がむしろ増える)
    std::vector<DirectX::XMFLOAT3> itemMin_;
    std::vector<DirectX::XMFLOAT3> itemMax_;
    std::vector<DirectX::XMFLOAT3> runMin_;
    std::vector<DirectX::XMFLOAT3> runMax_;
    GpuTimer timer_;
    int drawnTiles_ = 0;
    int drawCalls_ = 0;
    int culledDraws_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> objectCB_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
};

} // namespace mye
