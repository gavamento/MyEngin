/*----
 ProjectComputeRunner.cpp  プロジェクトコンピュートパスの登録・実行 (M78d)
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ProjectComputeRunner.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {
namespace {

// 定数バッファのサイズを 16 バイト倍数に切り上げる
inline UINT AlignCb(UINT sz) { return (sz + 15u) & ~15u; }

// スレッドグループ数の計算 (size をカバーするグループ数)
inline UINT CalcGroups(int size, int groupSize)
{
    if (size <= 0 || groupSize <= 0) return 1u;
    return static_cast<UINT>((size + groupSize - 1) / groupSize);
}

} // namespace

// ---------------------------------------------------------------------------
// DispatchPoint 文字列変換
// ---------------------------------------------------------------------------

const char* DispatchPointToString(ComputeDispatchPoint dp)
{
    switch (dp)
    {
    case ComputeDispatchPoint::BeforePost:    return "BeforePost";
    case ComputeDispatchPoint::BeforeTonemap: return "BeforeTonemap";
    case ComputeDispatchPoint::AfterTonemap:  return "AfterTonemap";
    default:                                  return "BeforePost";
    }
}

ComputeDispatchPoint DispatchPointFromString(const std::string& s)
{
    if (s == "BeforeTonemap") return ComputeDispatchPoint::BeforeTonemap;
    if (s == "AfterTonemap")  return ComputeDispatchPoint::AfterTonemap;
    return ComputeDispatchPoint::BeforePost; // 未知の文字列は BeforePost にフォールバック
}

// ---------------------------------------------------------------------------
// 登録 / 照会
// ---------------------------------------------------------------------------

void ProjectComputeRunner::SetPasses(std::vector<ProjectComputePassDesc> newDescs)
{
    // 上限超過は警告して切り捨てる (spec §4.4)
    if (static_cast<int>(newDescs.size()) > kMaxComputePasses)
    {
        MYE_LOG_WARN("ProjectComputeRunner: コンピュートパス数 %d が上限 %d を超えています。超過分を切り捨てます。",
                     static_cast<int>(newDescs.size()), kMaxComputePasses);
        newDescs.resize(static_cast<size_t>(kMaxComputePasses));
    }

    // 同名・同 dispatchPoint の既存パスのキャッシュを再利用しながら一覧を置き換える
    std::vector<CachedComputePass> next;
    next.reserve(newDescs.size());

    for (auto& nd : newDescs)
    {
        bool found = false;
        for (auto& cp : passes_)
        {
            if (cp.desc.shader == nd.shader &&
                cp.desc.dispatchPoint == nd.dispatchPoint)
            {
                // プロパティ値と priority / enabled だけ更新してキャッシュを維持
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
            // 新規: キャッシュなし (EnsureCached が RunDispatch 時に作る)
            CachedComputePass cp;
            cp.desc = std::move(nd);
            next.push_back(std::move(cp));
        }
    }

    passes_ = std::move(next);
}

void ProjectComputeRunner::ClearPasses()
{
    passes_.clear();
}

bool ProjectComputeRunner::HasPasses(ComputeDispatchPoint point) const
{
    for (const auto& cp : passes_)
    {
        if (cp.desc.enabled && cp.desc.dispatchPoint == point)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// シェーダ / スキーマ / CB / UAV テクスチャのキャッシュ更新
// ---------------------------------------------------------------------------

void ProjectComputeRunner::EnsureCached(CachedComputePass& cp, GraphicsDevice& device,
                                        ShaderManager& shaders, int width, int height)
{
    // シェーダ ID の取得 (未取得の場合)
    if (cp.shaderID.IsNull())
    {
        cp.shaderID    = shaders.LoadCompute(cp.desc.shader);
        cp.schemaReady = false;
    }

    // ホットリロードで差し替わったら (世代が変わったら) スキーマと CB を作り直す
    // (ProjectEffectRunner::EnsureCached と同じ規則)
    ShaderProgram* prog = shaders.Get(cp.shaderID);
    if (cp.schemaReady && prog && prog->valid && prog->generation != cp.builtFromGeneration)
    {
        cp.schemaReady = false;
    }

    // スキーマ未取得 → HLSL ソースからパース
    if (!cp.schemaReady)
    {
        if (prog && prog->valid)
        {
            // ソースは ShaderManager と同じ解決規則 (assets 全域の索引 → シェーダルート) で読む
            cp.schema              = shaders.FetchPropertySchema(cp.desc.shader);
            cp.schemaReady         = true;
            cp.builtFromGeneration = prog->generation;

            if (!cp.schema.ok)
            {
                MYE_LOG_ERROR("ProjectComputeRunner: %s の Properties パース失敗: %s",
                              cp.desc.shader.c_str(), cp.schema.errorMessage.c_str());
            }

            // ユーザー CB の生成 (cbSizeBytes > 0 のときのみ; spec §4.2 CB 省略)。
            // スキーマを取り直したときだけ作る (シェーダが無効な間に毎フレーム作り直さない)
            cp.userCB.Reset();
            if (cp.schema.ok && cp.schema.cbSizeBytes > 0)
            {
                gpubuf::CreateConstant(device.Device(),
                                       AlignCb(static_cast<UINT>(cp.schema.cbSizeBytes)),
                                       cp.userCB);
            }
        }

        // エンジン共通 CB の生成 (b0: 画面サイズ)
        if (!cp.engineCB)
        {
            gpubuf::CreateConstant(device.Device(),
                                   AlignCb(static_cast<UINT>(sizeof(EngineComputeCB))),
                                   cp.engineCB);
        }
    }

    // 出力 UAV テクスチャの確保 / リサイズ (サイズ変化なら no-op)
    if (!cp.outputTex.IsValid() || cp.cachedW != width || cp.cachedH != height)
    {
        cp.outputTex.Resize(device, width, height,
                            DXGI_FORMAT_R16G16B16A16_FLOAT,
                            /*withDepth=*/false, /*withUav=*/true);
        cp.cachedW = width;
        cp.cachedH = height;

        if (!cp.outputTex.IsValid() || !cp.outputTex.UAV())
        {
            MYE_LOG_ERROR("ProjectComputeRunner: '%s' の出力 UAV テクスチャ確保に失敗しました。",
                          cp.desc.shader.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// コンピュートパス実行
// ---------------------------------------------------------------------------

void ProjectComputeRunner::RunDispatch(
    ComputeDispatchPoint      point,
    GraphicsDevice&           device,
    ShaderManager&            shaders,
    ID3D11ShaderResourceView* sceneSRV,
    ID3D11ShaderResourceView* depthSRV,
    int                       width,
    int                       height)
{
    // 対象パスを収集して priority 昇順安定ソート (spec §4.1 同一点内は priority 昇順)
    std::vector<CachedComputePass*> active;
    for (auto& cp : passes_)
    {
        if (cp.desc.enabled && cp.desc.dispatchPoint == point)
            active.push_back(&cp);
    }
    if (active.empty())
        return;

    std::stable_sort(active.begin(), active.end(),
                     [](const CachedComputePass* a, const CachedComputePass* b)
                     { return a->desc.priority < b->desc.priority; });

    ID3D11DeviceContext* dc = device.Context();

    for (CachedComputePass* pcp : active)
    {
        CachedComputePass& cp = *pcp;

        // シェーダ / スキーマ / CB / UAV テクスチャのキャッシュ更新
        EnsureCached(cp, device, shaders, width, height);

        // CS の取得と有効性チェック
        ShaderProgram* prog  = shaders.Get(cp.shaderID);
        const bool     valid = prog && prog->valid && prog->cs;

        if (!valid)
        {
            // コンパイル失敗 → このパスをスキップ＋エラー表示 (spec §4.1 失敗時)
            MYE_LOG_ERROR("ProjectComputeRunner: コンピュートシェーダ '%s' が無効。"
                          "Dispatch をスキップします。",
                          cp.desc.shader.c_str());
            continue;
        }

        // 出力 UAV テクスチャが準備できていない場合もスキップ
        if (!cp.outputTex.IsValid() || !cp.outputTex.UAV())
        {
            MYE_LOG_ERROR("ProjectComputeRunner: '%s' の出力 UAV が無効。"
                          "Dispatch をスキップします。",
                          cp.desc.shader.c_str());
            continue;
        }

        // エンジン共通 CB を更新する (b0: 画面サイズ)
        if (cp.engineCB)
        {
            EngineComputeCB ecb  = {};
            ecb.screenW          = static_cast<float>(width);
            ecb.screenH          = static_cast<float>(height);
            ecb.invScreenW       = (width  > 0) ? 1.0f / static_cast<float>(width)  : 0.0f;
            ecb.invScreenH       = (height > 0) ? 1.0f / static_cast<float>(height) : 0.0f;
            gpubuf::UploadCB(dc, cp.engineCB.Get(), ecb);
        }

        // ユーザー CB を更新する (b1: Properties 値のパック)
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

        // リソースバインド
        // b0 = engineCB, b1 = userCB (Properties; null = cbSizeBytes==0)
        ID3D11Buffer* cbs[2] = { cp.engineCB.Get(), cp.userCB.Get() };
        dc->CSSetConstantBuffers(0, 2, cbs);

        // t0 = SceneColor SRV, t1 = SceneDepth SRV
        ID3D11ShaderResourceView* srvs[2] = { sceneSRV, depthSRV };
        dc->CSSetShaderResources(0, 2, srvs);

        // u0 = 出力 UAV テクスチャ
        ID3D11UnorderedAccessView* uavs[1] = { cp.outputTex.UAV() };
        dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        // CS を設定して Dispatch
        dc->CSSetShader(prog->cs.Get(), nullptr, 0);

        // グループ数: 指定があれば使用、なければ画面サイズから自動計算
        const UINT gx = (cp.desc.groupsX > 0)
                        ? cp.desc.groupsX
                        : CalcGroups(width,  kDefaultGroupSize);
        const UINT gy = (cp.desc.groupsY > 0)
                        ? cp.desc.groupsY
                        : CalcGroups(height, kDefaultGroupSize);
        const UINT gz = (cp.desc.groupsZ > 0) ? cp.desc.groupsZ : 1u;
        dc->Dispatch(gx, gy, gz);

        // 必ず unbind する (後続の描画パスが同スロットを安全に使えるようにする)
        // FroxelPass::Inject と同じ理由で省略不可
        dc->CSSetShader(nullptr, nullptr, 0);
        ID3D11UnorderedAccessView* nullUav[1] = { nullptr };
        dc->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        ID3D11ShaderResourceView* nullSrv[2] = { nullptr, nullptr };
        dc->CSSetShaderResources(0, 2, nullSrv);
        ID3D11Buffer* nullCb[2] = { nullptr, nullptr };
        dc->CSSetConstantBuffers(0, 2, nullCb);
    }
}

// ---------------------------------------------------------------------------
// 出力 SRV の取得 / バッファ解放
// ---------------------------------------------------------------------------

ID3D11ShaderResourceView* ProjectComputeRunner::GetOutputSRV(const std::string& shaderName) const
{
    for (const auto& cp : passes_)
    {
        if (cp.desc.shader == shaderName && cp.outputTex.IsValid())
            return cp.outputTex.SRV();
    }
    return nullptr;
}

void ProjectComputeRunner::ReleaseBuffers()
{
    for (auto& cp : passes_)
    {
        cp.outputTex   = RenderTexture{};
        cp.cachedW     = 0;
        cp.cachedH     = 0;
        cp.userCB.Reset();
        cp.engineCB.Reset();
        cp.schemaReady = false;
        cp.shaderID    = {};
    }
}

} // namespace mye
