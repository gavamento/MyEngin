/*----
 ProjectComputeRunner.h  プロジェクトコンピュートパスの登録・実行 (M78d)
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

// コンピュートシェーダの起動タイミング (spec §4.1)
enum class ComputeDispatchPoint : int32_t
{
    BeforePost    = 0, // ポストプロセス開始前 (HDR 描画直後)
    BeforeTonemap = 1, // TAA/DoF/MB の後、トーンマップ前
    AfterTonemap  = 2, // トーンマップ/FXAA の後
};

// コンピュートパスの記述子 (fxstack エントリから変換される)
struct ProjectComputePassDesc
{
    std::string          shader;        // LoadCompute に渡す名前 (例: "MySim.cs")
    ComputeDispatchPoint dispatchPoint  = ComputeDispatchPoint::BeforePost;
    int                  priority       = 50;
    bool                 enabled        = true;
    std::unordered_map<std::string, PropValue> propertyValues;
    // Dispatch グループ数の固定値 (0 = 画面サイズから自動計算: ceil(w/8), ceil(h/8), 1)
    uint32_t             groupsX        = 0;
    uint32_t             groupsY        = 0;
    uint32_t             groupsZ        = 0;
};

// "BeforePost" 等の文字列 ↔ ComputeDispatchPoint の相互変換
const char*          DispatchPointToString(ComputeDispatchPoint dp);
ComputeDispatchPoint DispatchPointFromString(const std::string& s); // 未知は BeforePost

// プロジェクトコンピュートパスのランタイム管理・実行クラス (M78d)。
// PostProcess::Resolve および RenderSystem から 3 つの dispatchPoint で呼ばれる。
// スクリプト所有バッファの ABI は ComputeAbiRunner (寿命とバインド単位が別)。
class ProjectComputeRunner
{
public:
    // スタック駆動パスの上限 (spec §4.4 性能制約)
    static constexpr int kMaxComputePasses = 8;
    // デフォルトのスレッドグループサイズ (HLSL の [numthreads(8,8,1)] と対応)
    static constexpr int kDefaultGroupSize = 8;

    // パス一覧を一括更新する (同名・同点の既存キャッシュは再利用)
    void SetPasses(std::vector<ProjectComputePassDesc> descs);
    // 全パスをクリアする
    void ClearPasses();
    // 指定 dispatchPoint に enabled なパスが 1 件以上あるか (事前判定用)
    bool HasPasses(ComputeDispatchPoint point) const;

    // 指定起動点の enabled なパスを実行する
    // sceneSRV: t0 へバインドする入力 SRV (null 可)
    // depthSRV: t1 へバインドする深度 SRV (null 可)
    // 実行後は必ず UAV / SRV / CB スロットを unbind する
    void RunDispatch(
        ComputeDispatchPoint      point,
        GraphicsDevice&           device,
        ShaderManager&            shaders,
        ID3D11ShaderResourceView* sceneSRV,
        ID3D11ShaderResourceView* depthSRV,
        int                       width,
        int                       height);

    // パスの出力 SRV を shaderName で取得する
    // (後続ポストが SRV として参照するための公開 API)
    // 対応パスが無い / UAV 未確保の場合は nullptr
    ID3D11ShaderResourceView* GetOutputSRV(const std::string& shaderName) const;

    // 出力テクスチャ / CB を全解放する (解像度変更やシャットダウン前に呼ぶ)
    void ReleaseBuffers();

private:
    // エンジン共通定数バッファ (b0: 画面サイズ)
    struct EngineComputeCB
    {
        float screenW, screenH;
        float invScreenW, invScreenH;
    };

    // パスのランタイムキャッシュ
    struct CachedComputePass
    {
        ProjectComputePassDesc               desc;
        AssetID                              shaderID    = {};
        PropertyParseResult                  schema;
        bool                                 schemaReady = false;
        // ユーザー Properties の定数バッファ (cbSizeBytes==0 なら null; spec §4.2 CB 省略)
        Microsoft::WRL::ComPtr<ID3D11Buffer> userCB;
        // エンジン共通定数バッファ (b0: screenW/H など)
        Microsoft::WRL::ComPtr<ID3D11Buffer> engineCB;
        // 出力 UAV テクスチャ (R16G16B16A16_FLOAT, フル解像度, withUav=true)
        RenderTexture                        outputTex;
        int                                  cachedW     = 0;
        int                                  cachedH     = 0;
    };

    // シェーダ / スキーマ / CB / UAV テクスチャを最新化する
    void EnsureCached(CachedComputePass& cp, GraphicsDevice& device, ShaderManager& shaders,
                      int width, int height);

    std::vector<CachedComputePass> passes_;
};

} // namespace mye
