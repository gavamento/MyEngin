/*----
 ProjectEffectRunner.cpp  プロジェクトポストパスの登録・実行 (M78b)
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ProjectEffectRunner.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {
namespace {

// CB サイズを 16 バイト倍数に切り上げる
inline UINT AlignCb(UINT sz) { return (sz + 15u) & ~15u; }

} // namespace

// ---------------------------------------------------------------------------
// 登録 / 照会
// ---------------------------------------------------------------------------

void ProjectEffectRunner::AddPass(ProjectPostPassDesc desc)
{
    CachedPass cp;
    cp.desc = std::move(desc);
    passes_.push_back(std::move(cp));
}

void ProjectEffectRunner::ClearPasses()
{
    passes_.clear();
}

void ProjectEffectRunner::SetPasses(std::vector<ProjectPostPassDesc> newDescs)
{
    // 上限超過は警告して切り捨てる (spec §4.4)
    if (static_cast<int>(newDescs.size()) > kMaxPostPasses)
    {
        MYE_LOG_WARN("ProjectEffectRunner: ポストパス数 %d が上限 %d を超えています。超過分を切り捨てます。",
                     static_cast<int>(newDescs.size()), kMaxPostPasses);
        newDescs.resize(static_cast<size_t>(kMaxPostPasses));
    }

    // 同名・同挿入点のパスのキャッシュを再利用しながら一覧を置き換える (M78c)。
    // 新規パスはキャッシュ空で追加; 既存パスは propertyValues だけ更新して CB 等を維持。
    std::vector<CachedPass> next;
    next.reserve(newDescs.size());

    for (auto& nd : newDescs)
    {
        // 同名・同挿入点の既存パスを探す
        bool found = false;
        for (auto& cp : passes_)
        {
            if (cp.desc.shaderName == nd.shaderName &&
                cp.desc.insertion  == nd.insertion)
            {
                // プロパティ値と priority/enabled だけ更新してキャッシュを維持
                cp.desc.propertyValues = std::move(nd.propertyValues);
                cp.desc.priority       = nd.priority;
                cp.desc.enabled        = nd.enabled;
                next.push_back(std::move(cp));
                found = true;
                break;
            }
        }
        if (!found)
        {
            // 新規: キャッシュなし (EnsureCached が次回 RunPasses で作る)
            CachedPass cp;
            cp.desc = std::move(nd);
            next.push_back(std::move(cp));
        }
    }

    passes_ = std::move(next);
}

bool ProjectEffectRunner::HasPasses(PostInsertionPoint insertion) const
{
    for (const auto& cp : passes_)
    {
        if (cp.desc.enabled && cp.desc.insertion == insertion)
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// シェーダ・スキーマのキャッシュ更新
// ---------------------------------------------------------------------------

void ProjectEffectRunner::EnsureCached(CachedPass& cp, GraphicsDevice& device, ShaderManager& shaders)
{
    // シェーダ ID を取得 (未取得 or 名前が変わった場合)
    if (cp.shaderID.IsNull())
    {
        cp.shaderID     = shaders.Load(cp.desc.shaderName);
        cp.schemaReady  = false;
    }

    // ホットリロードで差し替わったら (世代が変わったら) スキーマと CB を作り直す。
    // 失敗したリロードは旧プログラム・旧世代のままなので、動いているバイトコードとスキーマが必ず揃う
    ShaderProgram* prog = shaders.Get(cp.shaderID);
    if (cp.schemaReady && prog && prog->valid && prog->generation != cp.builtFromGeneration)
    {
        cp.schemaReady = false;
    }

    // スキーマ未取得 → シェーダのソースからパース
    if (!cp.schemaReady)
    {
        if (prog && prog->valid)
        {
            // ソースは ShaderManager と同じ解決規則 (assets 全域の索引 → シェーダルート) で読む。
            // ShaderDirs 直下だけを見ると、assets 内の任意フォルダに置いたポストは空スキーマになり
            // b1 が張られない (Inspector には Properties が出るのに実行時は真っ黒)
            cp.schema             = shaders.FetchPropertySchema(cp.desc.shaderName);
            cp.schemaReady        = true;
            cp.builtFromGeneration = prog->generation;

            if (!cp.schema.ok)
            {
                MYE_LOG_ERROR("ProjectEffectRunner: %s の Properties パース失敗: %s",
                              cp.desc.shaderName.c_str(), cp.schema.errorMessage.c_str());
            }

            // ユーザー CB の生成 / 再生成 (スキーマを取り直したときだけ。シェーダが無効な間に
            // 毎フレーム作り直さない)
            cp.userCB.Reset();
            if (cp.schema.ok && cp.schema.cbSizeBytes > 0)
            {
                gpubuf::CreateConstant(device.Device(),
                                       AlignCb(static_cast<UINT>(cp.schema.cbSizeBytes)),
                                       cp.userCB);
            }
        }

        // エンジン共通 CB の生成 (b0)
        if (!cp.engineCB)
        {
            gpubuf::CreateConstant(device.Device(),
                                   AlignCb(static_cast<UINT>(sizeof(EnginePostCB))),
                                   cp.engineCB);
        }
    }
}

void ProjectEffectRunner::PrepareShaders(ShaderManager& shaders)
{
    // GraphicsDevice が不明なので EnsureCached は呼べない; Load だけ先行
    for (auto& cp : passes_)
    {
        if (cp.shaderID.IsNull())
        {
            cp.shaderID    = shaders.Load(cp.desc.shaderName);
            cp.schemaReady = false;
        }
    }
}

std::vector<const ProjectPostPassDesc*>
ProjectEffectRunner::CollectSortedPasses(PostInsertionPoint insertion) const
{
    // RunPasses と同じ収集・ソートロジック (SelfTest から観測するための公開 API)
    std::vector<const CachedPass*> active;
    for (const auto& cp : passes_)
    {
        if (cp.desc.enabled && cp.desc.insertion == insertion)
        {
            active.push_back(&cp);
        }
    }
    std::stable_sort(active.begin(), active.end(),
                     [](const CachedPass* a, const CachedPass* b)
                     { return a->desc.priority < b->desc.priority; });

    std::vector<const ProjectPostPassDesc*> result;
    result.reserve(active.size());
    for (const auto* cp : active)
    {
        result.push_back(&cp->desc);
    }
    return result;
}

// ---------------------------------------------------------------------------
// フルスクリーン描画ヘルパ
// ---------------------------------------------------------------------------

void ProjectEffectRunner::DrawFullscreen(
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
    ID3D11SamplerState*       linearClamp,
    const std::vector<ID3D11ShaderResourceView*>& userTexSRVs)
{
    // SRV バインド中の RTV を外す (同じテクスチャの SRV/RTV 同時バインド防止)
    dc->OMSetRenderTargets(0, nullptr, nullptr);

    // ステート設定
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthDisabled, 0);
    dc->OMSetBlendState(blendOff, nullptr, 0xFFFFFFFFu);
    dc->RSSetState(rasterizer);

    // ビューポート
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(width);
    vp.Height   = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    // 定数バッファ: b0 = エンジン共通, b1 = ユーザー Properties
    ID3D11Buffer* cbs[2] = { engineCB, userCB };
    dc->PSSetConstantBuffers(0, 2, cbs);

    // テクスチャ: t0 = SceneColor, t1 = Depth
    ID3D11ShaderResourceView* srvs[2] = { sceneSRV, depthSRV };
    dc->PSSetShaderResources(0, 2, srvs);

    // ユーザー Tex2D: t2, t3, ... (スキーマ宣言順)
    if (!userTexSRVs.empty())
    {
        dc->PSSetShaderResources(2, static_cast<UINT>(userTexSRVs.size()),
                                 userTexSRVs.data());
    }

    // サンプラ: s0 = linearClamp
    dc->PSSetSamplers(0, 1, &linearClamp);

    // 描画
    dc->OMSetRenderTargets(1, &dstRTV, nullptr);
    dc->VSSetShader(vs, nullptr, 0);
    dc->PSSetShader(ps, nullptr, 0);
    dc->Draw(3, 0);

    // バインド解除 (深度 SRV は DSV bind 対策のため必ず外す)
    ID3D11ShaderResourceView* nullSrv[2] = { nullptr, nullptr };
    dc->PSSetShaderResources(0, 2, nullSrv);
    // ユーザー Tex2D スロットも解除
    if (!userTexSRVs.empty())
    {
        std::vector<ID3D11ShaderResourceView*> nullTex(userTexSRVs.size(), nullptr);
        dc->PSSetShaderResources(2, static_cast<UINT>(nullTex.size()), nullTex.data());
    }
    dc->OMSetRenderTargets(0, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// メイン実行
// ---------------------------------------------------------------------------

ID3D11ShaderResourceView* ProjectEffectRunner::RunPasses(
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
    AssetID                   magentaShaderID)
{
    // enabled な対象パスを収集し priority 昇順で安定ソート (spec §4.1)
    std::vector<CachedPass*> active;
    for (auto& cp : passes_)
    {
        if (cp.desc.enabled && cp.desc.insertion == insertion)
        {
            active.push_back(&cp);
        }
    }
    if (active.empty())
    {
        return nullptr;
    }
    std::stable_sort(active.begin(), active.end(),
                     [](const CachedPass* a, const CachedPass* b)
                     { return a->desc.priority < b->desc.priority; });

    ID3D11DeviceContext* dc = device.Context();
    ShaderProgram* magenta  = shaders.Get(magentaShaderID);

    // ping-pong の状態: 現在の読み取り SRV と書き先インデックス (0=RTA, 1=RTB)
    ID3D11ShaderResourceView* curSRV = inputSRV;
    int                       pingIdx = 0; // 次に書くバッファのインデックス

    const int n = static_cast<int>(active.size());
    for (int i = 0; i < n; ++i)
    {
        CachedPass& cp = *active[i];
        const bool  isLast = (i == n - 1);

        // シェーダ / スキーマのキャッシュ更新
        EnsureCached(cp, device, shaders);

        // 書き先 RTV の決定
        ID3D11RenderTargetView* dstRTV = nullptr;
        if (isLast && finalDstRTV != nullptr)
        {
            // AfterTonemap: 最終パスは外部 dst へ
            dstRTV = finalDstRTV;
        }
        else
        {
            // pingRTA (pingIdx==0) または pingRTB (pingIdx==1) に書く
            dstRTV = (pingIdx == 0) ? pingRTA.RTV() : pingRTB.RTV();
        }

        // シェーダの取得
        ShaderProgram* prog = shaders.Get(cp.shaderID);
        const bool     valid = prog && prog->valid && prog->vs && prog->ps;

        if (!valid)
        {
            // コンパイル失敗 → マゼンタ (spec §4.1)
            MYE_LOG_ERROR("ProjectEffectRunner: シェーダ '%s' が無効。マゼンタで代替描画します。",
                          cp.desc.shaderName.c_str());
            if (magenta && magenta->valid && magenta->vs && magenta->ps)
            {
                DrawFullscreen(dc, magenta->vs.Get(), magenta->ps.Get(),
                               curSRV, depthSRV,
                               cp.engineCB.Get(), nullptr,
                               dstRTV, width, height,
                               depthDisabled, blendOff, rasterizer, linearClamp);
            }
        }
        else
        {
            // ユーザー CB を更新する (PackProperties の結果を Map/Unmap で転送)
            if (cp.userCB && cp.schema.ok && cp.schema.cbSizeBytes > 0)
            {
                std::vector<uint8_t> cbData;
                if (PackProperties(cp.schema, cp.desc.propertyValues, cbData) &&
                    !cbData.empty())
                {
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (SUCCEEDED(dc->Map(cp.userCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                    {
                        memcpy(mapped.pData, cbData.data(), cbData.size());
                        dc->Unmap(cp.userCB.Get(), 0);
                    }
                }
            }

            // エンジン共通 CB を更新する (b0: 画面サイズ)
            if (cp.engineCB)
            {
                EnginePostCB ecb = {};
                ecb.screenW    = static_cast<float>(width);
                ecb.screenH    = static_cast<float>(height);
                ecb.invScreenW = (width  > 0) ? 1.0f / static_cast<float>(width)  : 0.0f;
                ecb.invScreenH = (height > 0) ? 1.0f / static_cast<float>(height) : 0.0f;
                gpubuf::UploadCB(dc, cp.engineCB.Get(), ecb);
            }

            // Tex2D SRV を収集する (スキーマ宣言順 → t2, t3, ...)
            std::vector<ID3D11ShaderResourceView*> texSRVs;
            if (texResolver_ && cp.schema.ok)
            {
                for (const auto& prop : cp.schema.properties)
                {
                    if (prop.type != PropType::Tex2D) {
                        continue;
                    }
                    // JSON 値があれば使い、なければスキーマの既定値
                    std::string name = prop.defaultTex.empty() ? "white" : prop.defaultTex;
                    auto it = cp.desc.propertyValues.find(prop.name);
                    if (it != cp.desc.propertyValues.end())
                    {
                        if (const std::string* sp = std::get_if<std::string>(&it->second)) {
                            name = *sp;
                        }
                    }
                    ID3D11ShaderResourceView* srv = texResolver_(name);
                    texSRVs.push_back(srv); // null もそのまま (スロット飛ばし不可)
                }
            }

            DrawFullscreen(dc, prog->vs.Get(), prog->ps.Get(),
                           curSRV, depthSRV,
                           cp.engineCB.Get(), cp.userCB.Get(),
                           dstRTV, width, height,
                           depthDisabled, blendOff, rasterizer, linearClamp,
                           texSRVs);
        }

        // 次パスの入力を更新 (AfterTonemap の最終パスで finalDstRTV に書いた場合は curSRV 不要)
        if (!(isLast && finalDstRTV != nullptr))
        {
            curSRV  = (pingIdx == 0) ? pingRTA.SRV() : pingRTB.SRV();
            pingIdx = 1 - pingIdx; // 次回は反対のバッファへ
        }
    }

    // BeforeTonemap: 最終出力の SRV を返す
    // (AfterTonemap / finalDstRTV あり は nullptr)
    if (finalDstRTV == nullptr)
    {
        return curSRV;
    }
    return nullptr;
}

} // namespace mye
