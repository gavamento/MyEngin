#include "Engine/Renderer/HzbPass.h"

#include <cstdint>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {
namespace {

// hzb_reduce.cs.hlsl の HzbCB (b0) と同一レイアウト
struct HzbCB {
    uint32_t srcSize[2];
    uint32_t dstSize[2];
};

using namespace gpubuf;

} // namespace

int HzbMipCount(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return 0;
    }
    int count = 1;
    int w = width;
    int h = height;
    while (w > 1 || h > 1) {
        w = (w > 1) ? (w / 2) : 1;
        h = (h > 1) ? (h / 2) : 1;
        ++count;
    }
    return count;
}

int HzbMipExtent(int base, int level)
{
    if (base <= 0 || level < 0) {
        return 0;
    }
    // floor 半減の繰り返しは floor(base / 2^level) と厳密に同値 (段ごとに丸めても変わらない)
    int v = (level >= 31) ? 0 : (base >> level);
    return (v > 1) ? v : 1;
}

void HzbReduceSpan(int dstIndex, int srcExtent, int dstExtent, int& begin, int& end)
{
    begin = 0;
    end = 0;
    if (srcExtent <= 0 || dstExtent <= 0 || dstIndex < 0 || dstIndex >= dstExtent) {
        return;
    }
    begin = (dstIndex * srcExtent) / dstExtent;
    // 切り上げ側の端 = ceil((i+1)*src/dst) - 1。奇数辺の取りこぼしを潰すのがこの +dst-1
    end = ((dstIndex + 1) * srcExtent + dstExtent - 1) / dstExtent - 1;
    if (end > srcExtent - 1) {
        end = srcExtent - 1;
    }
    if (end < begin) {
        end = begin; // 拡大方向 (src < dst) に使うことは無いが、区間を空にしない
    }
}

bool HzbPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    reduceCS_ = shaders.LoadCompute("hzb_reduce.cs");
    if (!CreateConstant(device.Device(), sizeof(HzbCB), cb_)) {
        return false;
    }
    // 計測できなくても描画は続くので戻り値は見ない (ShadowAtlas と同じ扱い)
    timer_.Init(device);
    return true;
}

void HzbPass::Shutdown()
{
    mipUav_.clear();
    mipSrv_.clear();
    srv_.Reset();
    tex_.Reset();
    cb_.Reset();
    width_ = 0;
    height_ = 0;
    mipCount_ = 0;
}

bool HzbPass::EnsurePyramid(GraphicsDevice& device, int width, int height)
{
    if (tex_ && width == width_ && height == height_) {
        return true;
    }
    mipUav_.clear();
    mipSrv_.clear();
    srv_.Reset();
    tex_.Reset();
    width_ = 0;
    height_ = 0;
    mipCount_ = 0;

    const int mips = HzbMipCount(width, height);
    if (mips <= 0) {
        return false;
    }
    ID3D11Device* dev = device.Device();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = static_cast<UINT>(mips);
    td.ArraySize = 1;
    // R32_FLOAT は FL11_0 で typed UAV ストアが保証されている数少ないフォーマットの 1 つ
    // (RenderTexture.h の withUav コメントと同じ制約)。入力の深度は 24bit なので精度も足りる
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex_.GetAddressOf()))) {
        MYE_LOG_ERROR("HzbPass: pyramid creation failed (%dx%d, %d mips)", width, height, mips);
        return false;
    }
    if (FAILED(dev->CreateShaderResourceView(tex_.Get(), nullptr, srv_.GetAddressOf()))) {
        MYE_LOG_ERROR("HzbPass: SRV creation failed");
        tex_.Reset();
        return false;
    }

    mipSrv_.resize(static_cast<size_t>(mips));
    mipUav_.resize(static_cast<size_t>(mips));
    for (int level = 0; level < mips; ++level) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = static_cast<UINT>(level);
        sd.Texture2D.MipLevels = 1; // ★1 段だけ = 書込中の段と別サブリソースになる
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D.MipSlice = static_cast<UINT>(level);
        if (FAILED(dev->CreateShaderResourceView(tex_.Get(), &sd,
                                                 mipSrv_[static_cast<size_t>(level)].GetAddressOf()))
            || FAILED(dev->CreateUnorderedAccessView(
                tex_.Get(), &ud, mipUav_[static_cast<size_t>(level)].GetAddressOf()))) {
            MYE_LOG_ERROR("HzbPass: mip view creation failed (level %d)", level);
            mipUav_.clear();
            mipSrv_.clear();
            srv_.Reset();
            tex_.Reset();
            return false;
        }
    }

    width_ = width;
    height_ = height;
    mipCount_ = mips;
    return true;
}

bool HzbPass::Build(GraphicsDevice& device, ShaderManager& shaders,
                    ID3D11ShaderResourceView* depthSRV, int width, int height)
{
    if (!depthSRV || width <= 0 || height <= 0 || !cb_) {
        return false;
    }
    ShaderProgram* cs = shaders.Get(reduceCS_);
    if (!cs || !cs->valid || !cs->cs) {
        return false; // コンパイル失敗時は HZB 無しで進む (消費者は SRV()==null で無効化)
    }
    if (!EnsurePyramid(device, width, height)) {
        return false;
    }

    ID3D11DeviceContext* dc = device.Context();
    timer_.Begin(device);
    dc->CSSetShader(cs->cs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->CSSetConstantBuffers(0, 1, cbs);

    for (int level = 0; level < mipCount_; ++level) {
        // 段 0 の入力はシーン深度そのもの = src と dst が同寸 → 縮小式が素通しコピーに
        // 退化する (HzbReduceSpan のコメント参照)。専用のコピー CS を持たずに済む
        const bool fromDepth = (level == 0);
        const int srcW = fromDepth ? width : HzbMipExtent(width, level - 1);
        const int srcH = fromDepth ? height : HzbMipExtent(height, level - 1);
        const int dstW = HzbMipExtent(width, level);
        const int dstH = HzbMipExtent(height, level);

        HzbCB c = {};
        c.srcSize[0] = static_cast<uint32_t>(srcW);
        c.srcSize[1] = static_cast<uint32_t>(srcH);
        c.dstSize[0] = static_cast<uint32_t>(dstW);
        c.dstSize[1] = static_cast<uint32_t>(dstH);
        UploadCB(dc, cb_.Get(), c);

        ID3D11ShaderResourceView* srcSrv[1] = {
            fromDepth ? depthSRV : mipSrv_[static_cast<size_t>(level - 1)].Get()
        };
        dc->CSSetShaderResources(0, 1, srcSrv);
        ID3D11UnorderedAccessView* dstUav[1] = { mipUav_[static_cast<size_t>(level)].Get() };
        dc->CSSetUnorderedAccessViews(0, 1, dstUav, nullptr);

        const UINT gx = static_cast<UINT>((dstW + kHzbThreadGroupSize - 1) / kHzbThreadGroupSize);
        const UINT gy = static_cast<UINT>((dstH + kHzbThreadGroupSize - 1) / kHzbThreadGroupSize);
        dc->Dispatch(gx, gy, 1);

        // ★次の段が「いま書いた面」を SRV で読むので、段ごとに必ず外す。
        //   外さずに次の CSSetShaderResources を撃つと、同じサブリソースが SRV と UAV に
        //   同時に載った瞬間にドライバが片方を暗黙に解除する (RtPasses::UnbindCompute と同じ作法)
        ID3D11ShaderResourceView* nullSrv[1] = {};
        ID3D11UnorderedAccessView* nullUav[1] = {};
        dc->CSSetShaderResources(0, 1, nullSrv);
        dc->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    }

    dc->CSSetShader(nullptr, nullptr, 0);
    timer_.End(device);
    return true;
}

} // namespace mye
