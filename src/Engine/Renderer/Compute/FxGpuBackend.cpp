//====================================================================================
//                          FxGpuBackend.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Fx バッファの GPU 実行 (upload・dispatch・遅延読み戻しリング) の実装
//====================================================================================
#include "Engine/Renderer/Compute/FxGpuBackend.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Renderer/Device/GpuBufferUtil.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye::compute {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t kWaitTimeoutMs = 5000;

} // namespace

void FxGpuBackend::Bind(GraphicsDevice* device, ShaderManager* shaders)
{
    device_ = device;
    shaders_ = shaders;
}

bool FxGpuBackend::Available() const
{
    return device_ != nullptr && shaders_ != nullptr && device_->Device() != nullptr && !released_;
}

bool FxGpuBackend::EnsureGpuBuffer(const BufferIo& io, GpuBuf*& out, uint64_t* uploadedBytes)
{
    ID3D11Device* dev = device_->Device();
    ID3D11DeviceContext* ctx = device_->Context();
    GpuBuf& g = bufs_[io.key];
    const uint32_t padded = std::max(io.count, kMinGpuElements);
    const bool fresh = !g.buf || g.count != io.count || g.stride != io.stride;
    if (fresh) {
        g = GpuBuf{};
        std::vector<uint8_t> init(static_cast<size_t>(padded) * io.stride, 0);
        std::memcpy(init.data(), io.cpu, static_cast<size_t>(io.count) * io.stride);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = padded * io.stride;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = io.stride;
        const D3D11_SUBRESOURCE_DATA sd = { init.data(), 0, 0 };
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = padded;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = padded;
        if (FAILED(dev->CreateBuffer(&bd, &sd, g.buf.GetAddressOf()))
            || FAILED(dev->CreateShaderResourceView(g.buf.Get(), &srvDesc, g.srv.GetAddressOf()))
            || FAILED(dev->CreateUnorderedAccessView(g.buf.Get(), &uavDesc, g.uav.GetAddressOf()))) {
            bufs_.erase(io.key);
            return false;
        }
        g.count = io.count;
        g.stride = io.stride;
        g.paddedCount = padded;
        g.appliedSerial = submitted_; // 作り直し前の読み戻しは無効
        if (uploadedBytes) {
            *uploadedBytes += static_cast<uint64_t>(io.count) * io.stride;
        }
    } else if (io.upload) {
        const D3D11_BOX box = { 0, 0, 0, io.count * io.stride, 1, 1 };
        ctx->UpdateSubresource(g.buf.Get(), 0, &box, io.cpu, 0, 0);
        g.appliedSerial = submitted_; // 古い GPU 結果を CPU ミラーへ戻さない
        if (uploadedBytes) {
            *uploadedBytes += static_cast<uint64_t>(io.count) * io.stride;
        }
    }
    out = &g;
    return true;
}

bool FxGpuBackend::Reflect(const void* bytecode, size_t size, const Request& req, Binding& out)
{
    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(bytecode, size, IID_ID3D11ShaderReflection, reinterpret_cast<void**>(refl.GetAddressOf())))) {
        return false;
    }
    D3D11_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    bool cbFound = false;
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd = {};
        refl->GetResourceBindingDesc(i, &bd);
        const std::string_view name = bd.Name;
        if (bd.Type == D3D_SIT_CBUFFER) {
            out.countBindPoint = bd.BindPoint;
            cbFound = name == "__KernelCount";
            continue;
        }
        int index = -2;
        UINT expectStride = 0;
        if (bd.Type == D3D_SIT_STRUCTURED) {
            if (name == "__kernel_params") {
                index = -1;
                expectStride = req.paramsSize;
            } else {
                for (size_t k = 0; k < req.readOnly.size(); ++k) {
                    if (req.readOnly[k].name == name) {
                        index = static_cast<int>(k);
                        expectStride = req.readOnly[k].stride;
                    }
                }
            }
            if (index != -2) {
                out.srv.emplace_back(bd.BindPoint, index);
            }
        } else if (bd.Type == D3D_SIT_UAV_RWSTRUCTURED) {
            for (size_t k = 0; k < req.readWrite.size(); ++k) {
                if (req.readWrite[k].name == name) {
                    index = static_cast<int>(k);
                    expectStride = req.readWrite[k].stride;
                }
            }
            if (index != -2) {
                out.uav.emplace_back(bd.BindPoint, index);
            }
        }
        // 構造化バッファでは NumSamples が要素の大きさ。ずれていれば struct の詰め方が違う
        if (index == -2 || bd.NumSamples != expectStride) {
            MYE_LOG_ERROR("[compute] reflection mismatch '%s': type %d bind %u stride HLSL %u / expected %u",
                          bd.Name, static_cast<int>(bd.Type), bd.BindPoint, bd.NumSamples, expectStride);
            return false;
        }
    }
    return cbFound;
}

uint64_t FxGpuBackend::Dispatch(const Request& req, uint64_t* uploadedBytes)
{
    if (!Available() || req.readWrite.empty() || req.readWrite.size() > kMaxReadWrite
        || req.readOnly.size() > kMaxReadOnly || req.paramsSize == 0 || (req.paramsSize & 3u) != 0
        || req.elementCount == 0) {
        return 0;
    }
    ID3D11Device* dev = device_->Device();
    ID3D11DeviceContext* ctx = device_->Context();
    const std::string shaderName = std::string(req.kernel) + ".kernel";
    ShaderProgram* prog = shaders_->Get(shaders_->LoadCompute(shaderName));
    if (prog == nullptr || !prog->valid || !prog->cs) {
        MYE_LOG_ERROR("[compute] kernel shader '%s' is not available", shaderName.c_str());
        return 0;
    }
    Binding binding;
    if (!Reflect(prog->csBytecode.data(), prog->csBytecode.size(), req, binding)) {
        return 0;
    }

    if (!countCb_ && !gpubuf::CreateConstant(dev, 16, countCb_)) {
        return 0;
    }

    uint64_t uploaded = 0;
    std::vector<GpuBuf*> ro(req.readOnly.size(), nullptr);
    std::vector<GpuBuf*> rw(req.readWrite.size(), nullptr);
    for (size_t i = 0; i < req.readOnly.size(); ++i) {
        if (!EnsureGpuBuffer(req.readOnly[i], ro[i], &uploaded)) {
            return 0;
        }
    }
    for (size_t i = 0; i < req.readWrite.size(); ++i) {
        if (!EnsureGpuBuffer(req.readWrite[i], rw[i], &uploaded)) {
            return 0;
        }
    }
    // unordered_map は再配置でポインタが動きうるので、全部作り終えてから取り直す
    for (size_t i = 0; i < req.readOnly.size(); ++i) {
        ro[i] = &bufs_[req.readOnly[i].key];
    }
    for (size_t i = 0; i < req.readWrite.size(); ++i) {
        rw[i] = &bufs_[req.readWrite[i].key];
    }

    // params: 読み取り専用なので SRV のみ
    ComPtr<ID3D11Buffer> paramsBuf;
    ComPtr<ID3D11ShaderResourceView> paramsSrv;
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = req.paramsSize;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = req.paramsSize;
        const D3D11_SUBRESOURCE_DATA sd = { req.params, 0, 0 };
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = 1;
        if (FAILED(dev->CreateBuffer(&bd, &sd, paramsBuf.GetAddressOf()))
            || FAILED(dev->CreateShaderResourceView(paramsBuf.Get(), &srvDesc, paramsSrv.GetAddressOf()))) {
            return 0;
        }
    }
    const uint32_t countData[4] = { req.elementCount, 0, 0, 0 };
    gpubuf::UploadCB(ctx, countCb_.Get(), countData);

    // 見つかった資源だけを、リフレクションの BindPoint に置く (未使用で落ちた引数は束縛しない)
    UINT srvCount = 0;
    UINT uavCount = 0;
    for (const auto& e : binding.srv) {
        srvCount = std::max(srvCount, e.first + 1);
    }
    for (const auto& e : binding.uav) {
        uavCount = std::max(uavCount, e.first + 1);
    }
    std::vector<ID3D11ShaderResourceView*> srvs(srvCount, nullptr);
    std::vector<ID3D11UnorderedAccessView*> uavs(uavCount, nullptr);
    for (const auto& e : binding.srv) {
        srvs[e.first] = e.second < 0 ? paramsSrv.Get() : ro[static_cast<size_t>(e.second)]->srv.Get();
    }
    for (const auto& e : binding.uav) {
        uavs[e.first] = rw[static_cast<size_t>(e.second)]->uav.Get();
    }
    ID3D11Buffer* cb = countCb_.Get();
    ctx->CSSetShader(prog->cs.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(binding.countBindPoint, 1, &cb);
    if (srvCount > 0) {
        ctx->CSSetShaderResources(0, srvCount, srvs.data());
    }
    if (uavCount > 0) {
        ctx->CSSetUnorderedAccessViews(0, uavCount, uavs.data(), nullptr);
    }
    ctx->Dispatch((req.elementCount + 63u) / 64u, 1, 1);
    std::vector<ID3D11ShaderResourceView*> nullSrvs(srvCount, nullptr);
    std::vector<ID3D11UnorderedAccessView*> nullUavs(uavCount, nullptr);
    ID3D11Buffer* nullCb = nullptr;
    if (uavCount > 0) {
        ctx->CSSetUnorderedAccessViews(0, uavCount, nullUavs.data(), nullptr);
    }
    if (srvCount > 0) {
        ctx->CSSetShaderResources(0, srvCount, nullSrvs.data());
    }
    ctx->CSSetConstantBuffers(binding.countBindPoint, 1, &nullCb);

    const uint64_t serial = ++submitted_;
    // 結果は RW バッファごとにステージングへ写す。読むのは完了後 (待たない)
    for (GpuBuf* g : rw) {
        const uint32_t slot = g->writeIndex % kStagingRing;
        if (!g->staging[slot]) {
            D3D11_BUFFER_DESC bd = {};
            g->buf->GetDesc(&bd);
            bd.Usage = D3D11_USAGE_STAGING;
            bd.BindFlags = 0;
            bd.MiscFlags = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateBuffer(&bd, nullptr, g->staging[slot].GetAddressOf()))) {
                continue; // この回の結果は返らない (GPU 側の状態は進む)
            }
        }
        ctx->CopyResource(g->staging[slot].Get(), g->buf.Get());
        g->stagingSerial[slot] = serial;
        ++g->writeIndex;
    }
    Pending p;
    p.serial = serial;
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_EVENT;
    if (SUCCEEDED(dev->CreateQuery(&qd, p.query.GetAddressOf()))) {
        ctx->End(p.query.Get());
        pending_.push_back(std::move(p));
    } else {
        completed_ = serial; // 追跡できないので完了扱い (Read が拾う)
    }
    if (uploadedBytes) {
        *uploadedBytes += uploaded;
    }
    return serial;
}

uint64_t FxGpuBackend::Completed()
{
    if (!Available()) {
        return completed_;
    }
    ID3D11DeviceContext* ctx = device_->Context();
    while (!pending_.empty()) {
        BOOL done = FALSE;
        const HRESULT hr = ctx->GetData(pending_.front().query.Get(), &done, sizeof(done), 0);
        if (hr == S_OK && done) {
            completed_ = std::max(completed_, pending_.front().serial);
            pending_.pop_front();
        } else if (FAILED(hr)) {
            // デバイス消失: 完了扱いにして Pending を残さない
            completed_ = std::max(completed_, pending_.back().serial);
            pending_.clear();
        } else {
            break;
        }
    }
    return completed_;
}

bool FxGpuBackend::WaitSerial(uint64_t serial)
{
    if (serial <= completed_) {
        return true;
    }
    if (!Available()) {
        return false;
    }
    device_->Context()->Flush();
    const ULONGLONG start = GetTickCount64();
    while (Completed() < serial) {
        if (GetTickCount64() - start > kWaitTimeoutMs) {
            MYE_LOG_ERROR("[compute] wait for GPU job timed out");
            return false;
        }
        Sleep(0);
    }
    return true;
}

bool FxGpuBackend::HasUnapplied(uint64_t key) const
{
    const auto it = bufs_.find(key);
    if (it == bufs_.end()) {
        return false;
    }
    const GpuBuf& g = it->second;
    for (uint32_t i = 0; i < kStagingRing; ++i) {
        if (g.stagingSerial[i] > g.appliedSerial) {
            return true;
        }
    }
    return false;
}

bool FxGpuBackend::Poll(uint64_t key, uint8_t* cpu, uint32_t count, uint32_t stride, bool wait)
{
    const auto it = bufs_.find(key);
    if (it == bufs_.end() || !Available()) {
        return false;
    }
    GpuBuf& g = it->second;
    if (g.count != count || g.stride != stride) {
        return false;
    }
    // 新しい順に試す。wait なら最新を待ち、そうでなければ終わっているもののうち最新を拾う
    uint32_t order[kStagingRing];
    for (uint32_t i = 0; i < kStagingRing; ++i) {
        order[i] = i;
    }
    std::sort(order, order + kStagingRing, [&](uint32_t a, uint32_t b) { return g.stagingSerial[a] > g.stagingSerial[b]; });
    ID3D11DeviceContext* ctx = device_->Context();
    for (uint32_t n = 0; n < kStagingRing; ++n) {
        const uint32_t slot = order[n];
        if (g.stagingSerial[slot] <= g.appliedSerial || !g.staging[slot]) {
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const UINT flags = (wait && n == 0) ? 0u : D3D11_MAP_FLAG_DO_NOT_WAIT;
        if (FAILED(ctx->Map(g.staging[slot].Get(), 0, D3D11_MAP_READ, flags, &mapped))) {
            continue;
        }
        std::memcpy(cpu, mapped.pData, static_cast<size_t>(count) * stride);
        ctx->Unmap(g.staging[slot].Get(), 0);
        g.appliedSerial = g.stagingSerial[slot];
        return true;
    }
    return false;
}

void FxGpuBackend::Discard(uint64_t key)
{
    const auto it = bufs_.find(key);
    if (it != bufs_.end()) {
        it->second.appliedSerial = submitted_;
    }
}

void FxGpuBackend::Forget(uint64_t key)
{
    bufs_.erase(key);
}

void FxGpuBackend::ReleaseGpu()
{
    bufs_.clear();
    pending_.clear();
    countCb_.Reset();
    completed_ = submitted_;
    released_ = true;
}

void FxGpuBackend::RecreateGpu()
{
    released_ = false;
}

} // namespace mye::compute
