#pragma once
#include <cstring>

#include <d3d11.h>
#include <wrl/client.h>

// GPU バッファ生成の共通ヘルパ (M46a)。
// GpuParticleBackend / PostProcess / DeferredPath に同型の定義が三重化していたものを集約した。
// D3D11 の素の呼び出しを畳むだけの薄い層で、状態は持たない (ヘッダオンリー)。
namespace mye::gpubuf {

// 構造化バッファ + (任意で) UAV / SRV。uav/srv に nullptr を渡せばそのビューは作らない。
// uavFlags には D3D11_BUFFER_UAV_FLAG_APPEND / _COUNTER を渡す (0 = 素の UAV)。
// initData が null なら未初期化 (D3D11_USAGE_DEFAULT)
inline bool CreateStructured(ID3D11Device* dev, UINT elemSize, UINT count, const void* initData,
                             UINT uavFlags, Microsoft::WRL::ComPtr<ID3D11Buffer>& buf,
                             Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>* uav,
                             Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* srv)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = elemSize * count;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = elemSize;
    D3D11_SUBRESOURCE_DATA init = { initData, 0, 0 };
    if (FAILED(dev->CreateBuffer(&bd, initData ? &init : nullptr, buf.GetAddressOf()))) {
        return false;
    }
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = count;
        ud.Buffer.Flags = uavFlags;
        if (FAILED(dev->CreateUnorderedAccessView(buf.Get(), &ud, uav->GetAddressOf()))) {
            return false;
        }
    }
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = count;
        if (FAILED(dev->CreateShaderResourceView(buf.Get(), &sd, srv->GetAddressOf()))) {
            return false;
        }
    }
    return true;
}

// 定数バッファ (DYNAMIC + CPU_ACCESS_WRITE)。size は 16 の倍数であること
inline bool CreateConstant(ID3D11Device* dev, UINT size, Microsoft::WRL::ComPtr<ID3D11Buffer>& out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out.GetAddressOf()));
}

// 定数バッファへの全書き換え (WRITE_DISCARD)。Map 失敗時は前の内容が残る
template <typename T>
void UploadCB(ID3D11DeviceContext* dc, ID3D11Buffer* cb, const T& data)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &data, sizeof(T));
        dc->Unmap(cb, 0);
    }
}

} // namespace mye::gpubuf
