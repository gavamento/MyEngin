/*----
 ProjectEffectRunner.h  プロジェクトポストパスの登録・実行 (M78b)
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/ProjectShaderProperties.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// ユーザーポストの挿入点 (spec §4.1)
enum class PostInsertionPoint : int32_t
{
    BeforeTonemap = 0, // HDR シーンカラーをトーンマップ前に処理
    AfterTonemap  = 1, // LDR (FXAA後) をトーンマップ後に処理
};

// 1 パスの記述子 (C++ デバッグ登録 / sub-03 fxstack から渡される)
struct ProjectPostPassDesc
{
    std::string        shaderName; // "MyTint.post" → ShaderManager::Load("MyTint.post")
    PostInsertionPoint insertion   = PostInsertionPoint::BeforeTonemap;
    int                priority    = 100;
    bool               enabled     = true;
    // プロパティ値 (Inspector / fxstack JSON から流入。sub-03 まで空でも可)
    std::unordered_map<std::string, PropValue> propertyValues;
};

// プロジェクトポストパスのランタイム管理 / 実行クラス (M78b)。
// PostProcess::Resolve から BeforeTonemap / AfterTonemap の 2 点で呼ばれる。
// fxstack JSON 統合は sub-03 で追加する。
class ProjectEffectRunner
{
public:
    // デバッグ登録: C++ から直接パスを追加する (fxstack は sub-03)
    void AddPass(ProjectPostPassDesc desc);
    // 全パスをクリアする
    void ClearPasses();

    // 指定挿入点に enabled なパスが 1 件以上あるか (Resolve の事前判定用)
    bool HasPasses(PostInsertionPoint insertion) const;

    // シェーダのプリロード / キャッシュ更新 (Resolve 前に呼ぶと起動が速い。省略可)
    void PrepareShaders(ShaderManager& shaders);

    // 指定挿入点の enabled なパスを priority 昇順安定ソートした記述子ポインタ配列を返す。
    // RunPasses が使うのと同じ収集・ソートロジックを公開する (SelfTest 用)。
    std::vector<const ProjectPostPassDesc*> CollectSortedPasses(PostInsertionPoint insertion) const;

    // 指定挿入点のパスをすべて実行する。
    //
    // inputSRV  : 最初のパスへの入力 SRV (t.scene 等)
    // pingRTA / pingRTB : ping-pong 用 RenderTexture。フォーマットは挿入点に合わせて渡す
    //   BeforeTonemap → HDR (R16G16B16A16_FLOAT)
    //   AfterTonemap  → LDR (R8G8B8A8_UNORM)
    // finalDstRTV :
    //   BeforeTonemap = nullptr — 最後の書き先は pingRTA/B のどちらかになる (戻り値で判明)
    //   AfterTonemap  = 外部 LDR RTV (dst) に直接書く
    // depthSRV  : ProjectPostCommon.hlsli の gSceneDepth。null 可 (その場合 t1 は null)
    // magentaShaderID : コンパイル失敗時に代替描画するマゼンタシェーダのID
    //
    // 戻り値 (BeforeTonemap): 最終出力の SRV (Resolve が sceneSRV を更新するために使う)
    //        パス 0 件 / AfterTonemap では nullptr
    ID3D11ShaderResourceView* RunPasses(
        PostInsertionPoint        insertion,
        GraphicsDevice&           device,
        ShaderManager&            shaders,
        ID3D11ShaderResourceView* inputSRV,
        RenderTexture&            pingRTA,
        RenderTexture&            pingRTB,
        ID3D11RenderTargetView*   finalDstRTV,
        ID3D11ShaderResourceView* depthSRV,
        int                       width,
        int                       height,
        ID3D11DepthStencilState*  depthDisabled,
        ID3D11BlendState*         blendOff,
        ID3D11RasterizerState*    rasterizer,
        ID3D11SamplerState*       linearClamp,
        AssetID                   magentaShaderID);

private:
    // エンジン共通定数バッファ (ProjectPostCommon.hlsli の b0)
    struct EnginePostCB
    {
        float screenW, screenH;
        float invScreenW, invScreenH;
    };

    // パスのランタイムキャッシュ
    struct CachedPass
    {
        ProjectPostPassDesc                  desc;
        AssetID                              shaderID    = {};
        PropertyParseResult                  schema;
        bool                                 schemaReady = false;
        // ユーザー CB (cbSizeBytes == 0 なら null; spec §4.2 CB 省略)
        Microsoft::WRL::ComPtr<ID3D11Buffer> userCB;
        // エンジン共通 CB (b0)
        Microsoft::WRL::ComPtr<ID3D11Buffer> engineCB;
    };

    // 1 フルスクリーンパスを描画する
    void DrawFullscreen(
        ID3D11DeviceContext*      dc,
        ID3D11VertexShader*       vs,
        ID3D11PixelShader*        ps,
        ID3D11ShaderResourceView* sceneSRV,
        ID3D11ShaderResourceView* depthSRV,
        ID3D11Buffer*             engineCB,
        ID3D11Buffer*             userCB,
        ID3D11RenderTargetView*   dstRTV,
        int                       width,
        int                       height,
        ID3D11DepthStencilState*  depthDisabled,
        ID3D11BlendState*         blendOff,
        ID3D11RasterizerState*    rasterizer,
        ID3D11SamplerState*       linearClamp);

    // pass の schema / CB を最新化する。schemaReady でなければ Load してパース
    void EnsureCached(CachedPass& cp, GraphicsDevice& device, ShaderManager& shaders);

    std::vector<CachedPass> passes_;
};

} // namespace mye
