#include "Engine/Renderer/MeshInstancing.h"

#include <algorithm>

#include "Engine/Renderer/GraphicsDevice.h"

using namespace DirectX;

namespace mye {

bool MeshInstanceBuffer::Upload(GraphicsDevice& device, const std::vector<XMFLOAT4X4>& worlds)
{
    if (worlds.empty()) {
        return false;
    }
    const uint32_t total = static_cast<uint32_t>(worlds.size());

    // 容量拡張 (CpuParticleBackend と同じ成長則)
    if (capacity_ < total) {
        capacity_ = std::max(total, capacity_ * 2 + 256);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = capacity_ * sizeof(XMFLOAT4X4);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(XMFLOAT4X4);
        if (FAILED(device.Device()->CreateBuffer(&bd, nullptr,
                                                 buffer_.ReleaseAndGetAddressOf()))) {
            capacity_ = 0;
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = capacity_;
        if (FAILED(device.Device()->CreateShaderResourceView(
                buffer_.Get(), &sd, srv_.ReleaseAndGetAddressOf()))) {
            buffer_.Reset();
            capacity_ = 0;
            return false;
        }
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ID3D11DeviceContext* dc = device.Context();
    if (FAILED(dc->Map(buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    memcpy(mapped.pData, worlds.data(), sizeof(XMFLOAT4X4) * worlds.size());
    dc->Unmap(buffer_.Get(), 0);
    return true;
}

} // namespace mye
