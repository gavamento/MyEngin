#include "Engine/Renderer/ShadowAtlas.h"

#include <algorithm>
#include <cfloat>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

// shadow_depth.hlsl / shadow_depth_instanced.hlsl の b0。ShadowPass と同一レイアウト
// (同じシェーダを共有しているので当然だが、CB 構造体まで共有すると
//  ShadowPass.cpp の匿名 namespace を公開することになるのでこちらで持つ)
struct ShadowObjectCB {
    XMFLOAT4X4 mvp; // transpose(world * lightViewProj)
    int32_t instanceBase;
    float instPad[3];
};

template <typename T>
void UploadCB(ID3D11DeviceContext* dc, ID3D11Buffer* cb, const T& data)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &data, sizeof(T));
        dc->Unmap(cb, 0);
    }
}

} // namespace

bool ShadowAtlas::Init(GraphicsDevice& device, ShaderManager& shaders, int resolution, int tileSize)
{
    resolution_ = (std::max)(resolution, 1);
    tileSize_ = (std::max)((std::min)(tileSize, resolution_), 1);
    tilesPerRow_ = resolution_ / tileSize_;
    capacity_ = (std::min)(tilesPerRow_ * tilesPerRow_, kMaxShadowTiles);
    ID3D11Device* dev = device.Device();

    depthShader_ = shaders.Load("shadow_depth");
    depthInstancedShader_ = shaders.Load("shadow_depth_instanced");

    // ShadowPass.cpp と同じ「TYPELESS で作って DSV(D32_FLOAT) と SRV(R32_FLOAT) を張る」手順。
    // 違いは配列でなく 1 枚であることだけ
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(resolution_);
    td.Height = static_cast<UINT>(resolution_);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex_.GetAddressOf()))) {
        MYE_LOG_ERROR("ShadowAtlas: depth texture creation failed (%dx%d)", resolution_,
                      resolution_);
        return false;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
    dvd.Format = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateDepthStencilView(tex_.Get(), &dvd, dsv_.GetAddressOf()))) {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = DXGI_FORMAT_R32_FLOAT;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(tex_.Get(), &svd, srv_.GetAddressOf()))) {
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ShadowObjectCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, objectCB_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthState_.GetAddressOf()))) {
        return false;
    }

    // ★透視専用の深度バイアス。ShadowPass (正射影 CSM) の DepthBias=800 とは単位が違う。
    // D32_FLOAT の DepthBias は「プリミティブ内の最大深度の指数から決まる ULP の整数倍」
    // なので、深度が 1.0 近くに寄る透視では 1 単位 ≒ 2^-23。800 だと NDC で約 1e-4 =
    // ライトから 20 単位離れた床で world 1.5 単位ぶんの浮き (= 派手なピーターパン) になる。
    // 定数項は量子化 1〜2 ULP を消す程度に絞り、アクネ対策は傾斜依存 (深度勾配に比例 =
    // 距離が変わっても自己調整する) に寄せる。clamp を入れているのは、円錐の縁で
    // 勾配が発散したときにバイアスが無限に伸びて影が消えるのを止めるため
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    rd.DepthBias = 120;
    rd.SlopeScaledDepthBias = 3.0f;
    rd.DepthBiasClamp = 0.002f;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizer_.GetAddressOf()))) {
        return false;
    }

    timer_.Init(device); // M54d: 失敗しても計測が 0 になるだけなので戻り値は見ない

    ready_ = true;
    MYE_LOG_INFO("ShadowAtlas: %dx%d, tile %d, capacity %d", resolution_, resolution_, tileSize_,
                 capacity_);
    return true;
}

void ShadowAtlas::FillTileRect(int index, ShadowTile& tile) const
{
    if (index < 0 || index >= capacity_ || tilesPerRow_ <= 0) {
        tile.pixelSize = 0;
        return;
    }
    const int tx = index % tilesPerRow_;
    const int ty = index / tilesPerRow_;
    tile.pixelX = tx * tileSize_;
    tile.pixelY = ty * tileSize_;
    tile.pixelSize = tileSize_;
    const float inv = 1.0f / static_cast<float>(resolution_);
    const float s = static_cast<float>(tileSize_) * inv;
    tile.uvScale[0] = s;
    tile.uvScale[1] = s;
    tile.uvOffset[0] = static_cast<float>(tile.pixelX) * inv;
    tile.uvOffset[1] = static_cast<float>(tile.pixelY) * inv;
}

void ShadowAtlas::Render(GraphicsDevice& device, ShaderManager& shaders, const RenderQueue& queue,
                         RenderResources& resources, const ShadowTile* tiles, int count,
                         bool instancing)
{
    ShaderProgram* prog = shaders.Get(depthShader_);
    drawnTiles_ = 0;
    drawCalls_ = 0;
    culledDraws_ = 0;
    if (!ready_ || !prog || !prog->valid || tiles == nullptr || count <= 0) {
        return;
    }
    count = (std::min)(count, capacity_);
    ID3D11DeviceContext* dc = device.Context();

    // run 検出は ShadowPass::Render と同じ (タイル間で共通なので 1 回だけ)
    runs_.clear();
    worlds_.clear();
    ShaderProgram* instProg = shaders.Get(depthInstancedShader_);
    if (instancing && instProg && instProg->valid) {
        canInstance_.resize(queue.opaque.size());
        for (size_t i = 0; i < queue.opaque.size(); ++i) {
            const RenderItem& it = queue.opaque[i];
            canInstance_[i] = (it.bones == nullptr && resources.meshes.Get(it.mesh)) ? 1 : 0;
        }
        BuildInstanceRuns(queue.opaque, canInstance_, runs_, worlds_);
        if (worlds_.empty() || !instanceBuf_.Upload(device, worlds_)) {
            runs_.clear();
        }
    }

    // ---- タイル毎カリング用の world AABB を 1 回だけ作る (M54d) ----
    // ★これを入れないと「点光源 1 個 = 6 タイル × 不透明キュー全件」が素通しで積まれる。
    //   M54c はスポット 2 本 = 2 パスだったので問題にならなかったが、点光源が入ると
    //   一気に 6 倍になり、WARP 撮影が計測不能に遅くなる (計画 M54d の★罠)
    const size_t itemCount = queue.opaque.size();
    itemMin_.assign(itemCount, XMFLOAT3{ 0.0f, 0.0f, 0.0f });
    itemMax_.assign(itemCount, XMFLOAT3{ 0.0f, 0.0f, 0.0f });
    for (size_t i = 0; i < itemCount; ++i) {
        const RenderItem& it = queue.opaque[i];
        const Mesh* mesh = resources.meshes.Get(it.mesh);
        if (!mesh) {
            continue; // 描画ループ側で弾かれる (AABB は使われない)
        }
        WorldAabb(it.world, mesh->aabbMin, mesh->aabbMax, itemMin_[i], itemMax_[i]);
    }
    // run は合併 AABB。「一部だけ枠外」で run を割ると描画数がむしろ増えるので、
    // run 全体が外にあるときだけ丸ごと飛ばす (保守的)
    runMin_.assign(runs_.size(), XMFLOAT3{ 0.0f, 0.0f, 0.0f });
    runMax_.assign(runs_.size(), XMFLOAT3{ 0.0f, 0.0f, 0.0f });
    for (size_t r = 0; r < runs_.size(); ++r) {
        const MeshInstanceRun& run = runs_[r];
        XMFLOAT3 lo = { FLT_MAX, FLT_MAX, FLT_MAX };
        XMFLOAT3 hi = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (uint32_t k = 0; k < run.count && run.first + k < itemCount; ++k) {
            const XMFLOAT3& a = itemMin_[run.first + k];
            const XMFLOAT3& b = itemMax_[run.first + k];
            lo = { (std::min)(lo.x, a.x), (std::min)(lo.y, a.y), (std::min)(lo.z, a.z) };
            hi = { (std::max)(hi.x, b.x), (std::max)(hi.y, b.y), (std::max)(hi.z, b.z) };
        }
        runMin_[r] = lo;
        runMax_[r] = hi;
    }

    timer_.Begin(device);

    // アトラスが SRV に残ったままだと DSV へ束ねられない。
    // ★ShadowPass は t0..t7 しか解除していないが、こちらは **t12 (Deferred 光パスの
    //   アトラススロット)** に自分自身が刺さっている可能性があるので t0..t12 を落とす
    ID3D11ShaderResourceView* nullSrvs[13] = {};
    dc->PSSetShaderResources(0, 13, nullSrvs);

    ID3D11RenderTargetView* noRtv[1] = { nullptr };
    dc->OMSetRenderTargets(1, noRtv, dsv_.Get());
    // 全面を 1.0 (最遠) でクリア。未使用タイルをサンプルしても影にならない値
    dc->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    dc->OMSetDepthStencilState(depthState_.Get(), 0);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    dc->RSSetState(rasterizer_.Get());
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(nullptr, nullptr, 0); // 深度のみ
    ID3D11Buffer* cbs[1] = { objectCB_.Get() };
    dc->VSSetConstantBuffers(0, 1, cbs);
    if (!runs_.empty()) {
        ID3D11ShaderResourceView* isrv = instanceBuf_.SRV();
        dc->VSSetShaderResources(0, 1, &isrv);
    }

    uint64_t boundShader = depthShader_.value;
    for (int t = 0; t < count; ++t) {
        const ShadowTile& tile = tiles[t];
        if (tile.pixelSize <= 0) {
            continue;
        }
        // ビューポートでタイルを切る (D3D11 はビューポート矩形でラスタライズをクリップする
        // ので、タイル外へはみ出したポリゴンが隣の枠を汚すことは無い)
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = static_cast<float>(tile.pixelX);
        vp.TopLeftY = static_cast<float>(tile.pixelY);
        vp.Width = static_cast<float>(tile.pixelSize);
        vp.Height = static_cast<float>(tile.pixelSize);
        vp.MaxDepth = 1.0f;
        dc->RSSetViewports(1, &vp);
        ++drawnTiles_;

        const XMMATRIX lvp = XMLoadFloat4x4(&tile.lightViewProj);
        // タイル毎の視錐台 (= このライト面の錐台)。lightViewProj は非転置 = 行ベクトル規約
        // なので BuildFrustum にそのまま渡せる
        const Frustum tileFrustum = BuildFrustum(tile.lightViewProj);
        uint64_t boundMesh = 0;
        size_t nextRun = 0;
        for (size_t idx = 0; idx < queue.opaque.size(); ++idx) {
            const RenderItem& item = queue.opaque[idx];
            Mesh* mesh = resources.meshes.Get(item.mesh);
            if (!mesh) {
                continue;
            }
            if (nextRun < runs_.size() && runs_[nextRun].first == idx) {
                const MeshInstanceRun& run = runs_[nextRun];
                if (!WorldAabbInFrustum(tileFrustum, runMin_[nextRun], runMax_[nextRun])) {
                    idx += run.count - 1; // run ごと飛ばす
                    ++nextRun;
                    ++culledDraws_;
                    continue;
                }
                ++nextRun;
                if (depthInstancedShader_.value != boundShader) {
                    dc->IASetInputLayout(instProg->inputLayout.Get());
                    dc->VSSetShader(instProg->vs.Get(), nullptr, 0);
                    boundShader = depthInstancedShader_.value;
                }
                ShadowObjectCB cb = {};
                XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(lvp)); // world はインスタンス側
                cb.instanceBase = static_cast<int32_t>(run.base);
                UploadCB(dc, objectCB_.Get(), cb);
                if (item.mesh.value != boundMesh) {
                    const UINT stride = sizeof(MeshVertex);
                    const UINT offset = 0;
                    ID3D11Buffer* vb = mesh->vb.Get();
                    dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                    dc->IASetIndexBuffer(mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
                    boundMesh = item.mesh.value;
                }
                dc->DrawIndexedInstanced(mesh->indexCount, run.count, 0, 0, 0);
                ++drawCalls_;
                idx += run.count - 1;
                continue;
            }
            if (!WorldAabbInFrustum(tileFrustum, itemMin_[idx], itemMax_[idx])) {
                ++culledDraws_;
                continue;
            }
            if (depthShader_.value != boundShader) {
                dc->IASetInputLayout(prog->inputLayout.Get());
                dc->VSSetShader(prog->vs.Get(), nullptr, 0);
                boundShader = depthShader_.value;
            }
            ShadowObjectCB cb = {};
            XMStoreFloat4x4(&cb.mvp,
                            XMMatrixTranspose(XMMatrixMultiply(XMLoadFloat4x4(&item.world), lvp)));
            UploadCB(dc, objectCB_.Get(), cb);
            if (item.mesh.value != boundMesh) {
                const UINT stride = sizeof(MeshVertex);
                const UINT offset = 0;
                ID3D11Buffer* vb = mesh->vb.Get();
                dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                dc->IASetIndexBuffer(mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
                boundMesh = item.mesh.value;
            }
            dc->DrawIndexed(mesh->indexCount, 0, 0);
            ++drawCalls_;
        }
    }

    timer_.End(device);

    if (!runs_.empty()) {
        ID3D11ShaderResourceView* nullVsSrv = nullptr;
        dc->VSSetShaderResources(0, 1, &nullVsSrv);
    }
    // SRV として読む前に DSV バインドを外す (同一リソースの DSV/SRV 同時バインドは禁止)
    dc->OMSetRenderTargets(1, noRtv, nullptr);
}

} // namespace mye
