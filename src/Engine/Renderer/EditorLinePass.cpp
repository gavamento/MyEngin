#include "Engine/Renderer/EditorLinePass.h"

#include <cmath>

#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {

XMFLOAT4 EditorLinePass::Unpack(uint32_t rgba)
{
    return XMFLOAT4(((rgba >> 24) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f,
                    ((rgba >> 8) & 0xFF) / 255.0f, (rgba & 0xFF) / 255.0f);
}

bool EditorLinePass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("editor_line");

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(XMFLOAT4X4);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // 深度は書かない (シーンを汚さない)
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthOn_.GetAddressOf()))) {
        return false;
    }
    dd.DepthEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthOff_.GetAddressOf()))) {
        return false;
    }

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bld, blend_.GetAddressOf()))) {
        return false;
    }

    ready_ = true;
    return true;
}

void EditorLinePass::Shutdown()
{
    depthTested_.clear();
    onTop_.clear();
    vb_.Reset();
    cb_.Reset();
    raster_.Reset();
    depthOn_.Reset();
    depthOff_.Reset();
    blend_.Reset();
    ready_ = false;
}

void EditorLinePass::Begin()
{
    depthTested_.clear();
    onTop_.clear();
}

void EditorLinePass::AddLine(const XMFLOAT3& a, const XMFLOAT3& b, uint32_t rgba, bool onTop)
{
    const XMFLOAT4 c = Unpack(rgba);
    std::vector<LineVertex>& dst = onTop ? onTop_ : depthTested_;
    dst.push_back({ a, c });
    dst.push_back({ b, c });
}

void EditorLinePass::AddAABB(const XMFLOAT3& lo, const XMFLOAT3& hi, uint32_t rgba, bool onTop)
{
    const XMFLOAT3 p[8] = {
        { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
        { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z },
    };
    const int e[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 },
                           { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    for (auto& seg : e) {
        AddLine(p[seg[0]], p[seg[1]], rgba, onTop);
    }
}

void EditorLinePass::AddWireBox(const XMFLOAT4X4& world, const XMFLOAT3& half, uint32_t rgba,
                                bool onTop)
{
    const XMMATRIX m = XMLoadFloat4x4(&world);
    XMFLOAT3 corners[8];
    int idx = 0;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                const XMVECTOR local = XMVectorSet(sx * half.x, sy * half.y, sz * half.z, 1.0f);
                XMStoreFloat3(&corners[idx++], XMVector3Transform(local, m));
            }
        }
    }
    // corners index: bit0=z, bit1=y, bit2=x
    const int e[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                           { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    for (auto& seg : e) {
        AddLine(corners[seg[0]], corners[seg[1]], rgba, onTop);
    }
}

void EditorLinePass::AddWireSphere(const XMFLOAT3& center, float radius, uint32_t rgba, bool onTop)
{
    constexpr int kSeg = 24;
    constexpr float kTwoPi = 6.28318530718f;
    for (int axis = 0; axis < 3; ++axis) {
        XMFLOAT3 prev = {};
        for (int i = 0; i <= kSeg; ++i) {
            const float t = kTwoPi * static_cast<float>(i) / kSeg;
            const float c = std::cos(t) * radius;
            const float s = std::sin(t) * radius;
            XMFLOAT3 p = center;
            if (axis == 0) { p.x += c; p.y += s; }
            else if (axis == 1) { p.y += c; p.z += s; }
            else { p.x += c; p.z += s; }
            if (i > 0) {
                AddLine(prev, p, rgba, onTop);
            }
            prev = p;
        }
    }
}

void EditorLinePass::AddGrid(int halfCount, float spacing, uint32_t lineRgba, uint32_t axisXRgba,
                             uint32_t axisZRgba)
{
    const float extent = halfCount * spacing;
    for (int i = -halfCount; i <= halfCount; ++i) {
        const float o = i * spacing;
        const uint32_t cz = (i == 0) ? axisZRgba : lineRgba;
        const uint32_t cx = (i == 0) ? axisXRgba : lineRgba;
        AddLine({ o, 0.0f, -extent }, { o, 0.0f, extent }, cz); // Z 方向線
        AddLine({ -extent, 0.0f, o }, { extent, 0.0f, o }, cx); // X 方向線
    }
}

void EditorLinePass::PushVerts(std::vector<LineVertex>& dst, ID3D11DeviceContext* dc,
                               GraphicsDevice& device)
{
    if (dst.empty()) {
        return;
    }
    ID3D11Device* dev = device.Device();
    const uint32_t needed = static_cast<uint32_t>(dst.size());
    if (needed > vbCapacity_) {
        vbCapacity_ = needed + needed / 2 + 256;
        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = vbCapacity_ * sizeof(LineVertex);
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vb_.Reset();
        if (FAILED(dev->CreateBuffer(&vbd, nullptr, vb_.GetAddressOf()))) {
            return;
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(vb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, dst.data(), dst.size() * sizeof(LineVertex));
        dc->Unmap(vb_.Get(), 0);
    }
    const UINT stride = sizeof(LineVertex);
    const UINT offset = 0;
    ID3D11Buffer* vb = vb_.Get();
    dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    dc->Draw(needed, 0);
}

void EditorLinePass::Render(GraphicsDevice& device, ShaderManager& shaders,
                            ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int width,
                            int height, const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    if (!ready_ || (depthTested_.empty() && onTop_.empty())) {
        return;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();

    dc->OMSetRenderTargets(1, &rtv, dsv);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4(&viewProj,
                    XMMatrixTranspose(XMMatrixMultiply(XMLoadFloat4x4(&view), XMLoadFloat4x4(&proj))));
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &viewProj, sizeof(viewProj));
        dc->Unmap(cb_.Get(), 0);
    }
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->VSSetConstantBuffers(0, 1, cbs);
    dc->RSSetState(raster_.Get());
    dc->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFu);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);

    dc->OMSetDepthStencilState(depthOn_.Get(), 0);
    PushVerts(depthTested_, dc, device);
    dc->OMSetDepthStencilState(depthOff_.Get(), 0);
    PushVerts(onTop_, dc, device);
}

} // namespace mye
