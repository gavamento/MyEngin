#include "Engine/Engine/Vfx/VfxRenderer.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/UI/UIRenderer.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {

// ---- TrailStore (D3D 非依存 — ヘッドレス selftest 対象) ----

void TrailStore::Update(World& world, uint64_t tick)
{
    // 現存する {TrailRenderer + WorldMatrix} エンティティを index 昇順で収集
    struct Cur {
        EntityID e;
        const TrailRendererComponent* tr;
        XMFLOAT3 pos;
    };
    std::vector<Cur> cur;
    const ComponentTypeId req[] = { TrailRendererComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(TrailRendererComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue; // 無効化されたトレイルはバッファごと消える (再有効化で最初から)
            }
            const auto* tr = static_cast<const TrailRendererComponent*>(arch.GetPtr(ti, row));
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            cur.push_back({ e, tr, { wm->value._41, wm->value._42, wm->value._43 } });
        }
    });
    std::sort(cur.begin(), cur.end(),
              [](const Cur& a, const Cur& b) { return a.e.index < b.e.index; });

    // 同期: 現存エンティティのバッファだけを引き継ぐ (消えた owner は落ちる)
    std::vector<Buffer> next;
    next.reserve(cur.size());
    for (const Cur& c : cur) {
        Buffer nb;
        nb.owner = c.e;
        for (Buffer& old : buffers_) {
            if (old.owner.index == c.e.index && old.owner.generation == c.e.generation) {
                nb.pts = std::move(old.pts);
                break;
            }
        }
        // 寿命失効 (先頭=最古から)。duration 秒 → tick 換算
        const float lifeTicks = (std::max)(1.0f, c.tr->duration * 60.0f);
        size_t expire = 0;
        while (expire < nb.pts.size()
               && static_cast<float>(tick - nb.pts[expire].tick) > lifeTicks) {
            ++expire;
        }
        if (expire > 0) {
            nb.pts.erase(nb.pts.begin(), nb.pts.begin() + static_cast<ptrdiff_t>(expire));
        }
        // 追加 (emitting 時のみ)。minVertexDistance 未満の移動は追加しない
        if (c.tr->emitting != 0) {
            bool add = nb.pts.empty();
            if (!add) {
                const XMFLOAT3& last = nb.pts.back().pos;
                const float dx = c.pos.x - last.x;
                const float dy = c.pos.y - last.y;
                const float dz = c.pos.z - last.z;
                const float minD = c.tr->minVertexDistance;
                add = (dx * dx + dy * dy + dz * dz) >= minD * minD;
            }
            if (add) {
                nb.pts.push_back({ c.pos, tick });
                if (nb.pts.size() > static_cast<size_t>(kMaxPoints)) {
                    nb.pts.erase(nb.pts.begin());
                }
            }
        }
        next.push_back(std::move(nb));
    }
    buffers_ = std::move(next);
}

// ---- VfxRenderer ----

bool VfxRenderer::Init(GraphicsDevice& device, ShaderManager& shaders, UIRenderer* ui)
{
    ui_ = ui;
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("vfx_sprite");

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(XMFLOAT4X4); // transpose(view*proj)
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf()))) {
        return false;
    }

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&smp, samplerLinear_.GetAddressOf()))) {
        return false;
    }
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // フォントアトラス用 (crisp + ブリード防止)
    if (FAILED(dev->CreateSamplerState(&smp, samplerPoint_.GetAddressOf()))) {
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

    // 深度テストあり (シーンに隠れる)・書き込みなし (透明同士の順序は自前ソート)
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthReadOnly_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // ビルボード/リボンは両面
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        return false;
    }

    ready_ = true;
    return true;
}

void VfxRenderer::Shutdown()
{
    cb_.Reset();
    vb_.Reset();
    samplerLinear_.Reset();
    samplerPoint_.Reset();
    blend_.Reset();
    depthReadOnly_.Reset();
    raster_.Reset();
    trails_.Reset();
    verts_.clear();
    batches_.clear();
    scratch_.clear();
    ready_ = false;
}

void VfxRenderer::PushVerts(ID3D11ShaderResourceView* srv, bool pointSample, const VfxVertex* v,
                            int count)
{
    if (count <= 0) {
        return;
    }
    if (batches_.empty() || batches_.back().srv != srv
        || batches_.back().pointSample != pointSample) {
        Batch b;
        b.srv = srv;
        b.start = static_cast<uint32_t>(verts_.size());
        b.count = 0;
        b.pointSample = pointSample;
        batches_.push_back(b);
    }
    verts_.insert(verts_.end(), v, v + count);
    batches_.back().count += static_cast<uint32_t>(count);
}

void VfxRenderer::Render(World& world, GraphicsDevice& device, ShaderManager& shaders,
                         RenderResources& resources, const RenderView& view)
{
    if (!ready_ || view.width <= 0 || view.height <= 0) {
        return;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid) {
        return;
    }

    // カメラ基底 (particle_render と同じ規約: view 行列の列)
    const XMFLOAT4X4& vm = view.view;
    const XMFLOAT3 camRight = { vm._11, vm._21, vm._31 };
    const XMFLOAT3 camUp = { vm._12, vm._22, vm._32 };
    auto viewZOf = [&vm](const XMFLOAT3& p) {
        return p.x * vm._13 + p.y * vm._23 + p.z * vm._33 + vm._43;
    };

    // ---- 収集 (viewZ 降順 back-to-front、tie は entity.index 昇順) ----
    struct Item {
        float viewZ;
        uint32_t index;
        int kind; // 0=sprite 1=trail 2=text
        const void* comp;
        const XMFLOAT4X4* world;
        const TrailStore::Buffer* trail;
    };
    std::vector<Item> items;

    const ComponentTypeId spReq[] = { SpriteRendererComponent::sTypeId,
                                      WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(spReq, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SpriteRendererComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* sp = static_cast<const SpriteRendererComponent*>(arch.GetPtr(si, row));
            const auto* wmc = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            const XMFLOAT3 pos = { wmc->value._41, wmc->value._42, wmc->value._43 };
            items.push_back({ viewZOf(pos), e.index, 0, sp, &wmc->value, nullptr });
        }
    });

    const bool haveFont = (ui_ != nullptr) && (ui_->FontSRV() != nullptr);
    if (haveFont) {
        const ComponentTypeId txReq[] = { TextMeshComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(txReq, [&](Archetype& arch) {
            const int ti = arch.FindTypeIndex(TextMeshComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                const auto* tm = static_cast<const TextMeshComponent*>(arch.GetPtr(ti, row));
                const auto* wmc = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                const XMFLOAT3 pos = { wmc->value._41, wmc->value._42, wmc->value._43 };
                items.push_back({ viewZOf(pos), e.index, 2, tm, &wmc->value, nullptr });
            }
        });
    }

    for (const TrailStore::Buffer& tb : trails_.Buffers()) {
        if (tb.pts.size() < 2 || !world.IsAlive(tb.owner)) {
            continue;
        }
        const auto* tr = world.GetComponent<TrailRendererComponent>(tb.owner);
        if (!tr || !IsEntityActive(world, tb.owner)) {
            continue;
        }
        items.push_back({ viewZOf(tb.pts.back().pos), tb.owner.index, 1, tr, nullptr, &tb });
    }

    if (items.empty()) {
        return;
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.viewZ != b.viewZ) {
            return a.viewZ > b.viewZ; // 遠い順 (back-to-front)
        }
        return a.index < b.index;
    });

    // ---- 頂点構築 (SRV 切替でバッチ分割) ----
    verts_.clear();
    batches_.clear();
    Texture* whiteTex = resources.textures.Get(resources.textures.White());
    ID3D11ShaderResourceView* whiteSrv = whiteTex ? whiteTex->srv.Get() : nullptr;
    if (!whiteSrv) {
        return;
    }
    VfxGlyph glyphs[128];
    if (haveFont) {
        ui_->CopyGlyphTable(glyphs);
    }

    // 現フレームの tick は TrailStore の最新点 tick で近似不要 — age は寿命比の表示なので
    // buffers の最新 tick を now として使う (UpdateTrails と同じ時計)
    uint64_t nowTick = 0;
    for (const TrailStore::Buffer& tb : trails_.Buffers()) {
        if (!tb.pts.empty() && tb.pts.back().tick > nowTick) {
            nowTick = tb.pts.back().tick;
        }
    }

    for (const Item& it : items) {
        if (it.kind == 0) {
            const auto& sp = *static_cast<const SpriteRendererComponent*>(it.comp);
            XMFLOAT3 right, up;
            vfx::BillboardBasis(sp.billboardMode, *it.world, camRight, camUp, view.cameraPos,
                                right, up);
            const XMFLOAT3 pos = { it.world->_41, it.world->_42, it.world->_43 };
            const float hx = sp.size.x * 0.5f;
            const float hy = sp.size.y * 0.5f;
            const XMFLOAT3 rx = { right.x * hx, right.y * hx, right.z * hx };
            const XMFLOAT3 uy = { up.x * hy, up.y * hy, up.z * hy };
            const XMFLOAT3 tl = { pos.x - rx.x + uy.x, pos.y - rx.y + uy.y, pos.z - rx.z + uy.z };
            const XMFLOAT3 tr2 = { pos.x + rx.x + uy.x, pos.y + rx.y + uy.y, pos.z + rx.z + uy.z };
            const XMFLOAT3 bl = { pos.x - rx.x - uy.x, pos.y - rx.y - uy.y, pos.z - rx.z - uy.z };
            const XMFLOAT3 br = { pos.x + rx.x - uy.x, pos.y + rx.y - uy.y, pos.z + rx.z - uy.z };
            const VfxVertex q[6] = {
                { tl, { 0, 0 }, sp.color }, { tr2, { 1, 0 }, sp.color },
                { br, { 1, 1 }, sp.color }, { tl, { 0, 0 }, sp.color },
                { br, { 1, 1 }, sp.color }, { bl, { 0, 1 }, sp.color },
            };
            ID3D11ShaderResourceView* srv = whiteSrv;
            if (sp.texture.value != 0) {
                Texture* t = resources.textures.Get(sp.texture);
                if (t && t->srv) {
                    srv = t->srv.Get();
                }
            }
            PushVerts(srv, false, q, 6);
        } else if (it.kind == 1) {
            const auto& tr = *static_cast<const TrailRendererComponent*>(it.comp);
            const float lifeTicks = (std::max)(1.0f, tr.duration * 60.0f);
            scratch_.clear();
            vfx::BuildTrailRibbon(it.trail->pts.data(), static_cast<int>(it.trail->pts.size()),
                                  nowTick, lifeTicks, tr.width, tr.colorBegin, tr.colorEnd,
                                  view.cameraPos, scratch_);
            PushVerts(whiteSrv, false, scratch_.data(), static_cast<int>(scratch_.size()));
        } else {
            const auto& tm = *static_cast<const TextMeshComponent*>(it.comp);
            scratch_.clear();
            if (vfx::BuildTextQuadsLocal(tm.text, glyphs, tm.fontScale, tm.color, scratch_) > 0) {
                XMFLOAT3 right, up;
                vfx::BillboardBasis(tm.billboardMode, *it.world, camRight, camUp, view.cameraPos,
                                    right, up);
                const XMFLOAT3 pos = { it.world->_41, it.world->_42, it.world->_43 };
                for (VfxVertex& v : scratch_) {
                    const float lx = v.pos.x;
                    const float ly = v.pos.y;
                    v.pos = { pos.x + right.x * lx + up.x * ly, pos.y + right.y * lx + up.y * ly,
                              pos.z + right.z * lx + up.z * ly };
                }
                PushVerts(ui_->FontSRV(), true, scratch_.data(),
                          static_cast<int>(scratch_.size()));
            }
        }
    }

    if (verts_.empty()) {
        return;
    }

    // ---- VB 確保 + アップロード (UIRenderer と同パターン) ----
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();
    const uint32_t needed = static_cast<uint32_t>(verts_.size());
    if (needed > vbCapacity_) {
        vbCapacity_ = needed + needed / 2 + 512;
        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = vbCapacity_ * sizeof(VfxVertex);
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
        memcpy(mapped.pData, verts_.data(), verts_.size() * sizeof(VfxVertex));
        dc->Unmap(vb_.Get(), 0);
    }
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, XMMatrixTranspose(XMMatrixMultiply(XMLoadFloat4x4(&view.view),
                                                            XMLoadFloat4x4(&view.proj))));
    if (SUCCEEDED(dc->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &vp, sizeof(vp));
        dc->Unmap(cb_.Get(), 0);
    }

    // ---- 描画。RT/ビューポートはパスがバインド済み (particles と同じ前提) ----
    const UINT stride = sizeof(VfxVertex);
    const UINT offset = 0;
    ID3D11Buffer* vb = vb_.Get();
    dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->VSSetConstantBuffers(0, 1, cbs);
    dc->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFu);
    dc->OMSetDepthStencilState(depthReadOnly_.Get(), 0);
    dc->RSSetState(raster_.Get());

    for (const Batch& b : batches_) {
        ID3D11SamplerState* samps[1] = { b.pointSample ? samplerPoint_.Get()
                                                       : samplerLinear_.Get() };
        dc->PSSetSamplers(0, 1, samps);
        ID3D11ShaderResourceView* srvs[1] = { b.srv };
        dc->PSSetShaderResources(0, 1, srvs);
        dc->Draw(b.count, b.start);
    }
    // ブレンド/深度は次段 (particles / postfx) が自前で設定するので復元不要
}

} // namespace mye
