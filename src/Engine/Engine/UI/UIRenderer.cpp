#include "Engine/Engine/UI/UIRenderer.h"

#include "Engine/Engine/UI/UIInteraction.h" // M70c: hover/press の正本

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/UI/UIGeometry.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UILayoutGroup.h" // M75e: LayoutScratch
#include "Engine/Engine/UI/UITextLayout.h"
#include "Engine/Engine/UI/UITextMetrics.h" // M75d: 行高の一致を固定するだけ
#include "Engine/Engine/UI/UIWidgets.h"     // M75f: Selectable の色 / 画像と Toggle のチェックマーク
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {

// sim 側の計測表 (uitext) は D3D 非依存に保つため FontAtlas.h を include せず行高を自前で持つ。
// 描画の行高とずれると Layout の箱の高さと描いた文字の行送りが食い違うので、ここで機械照合する
static_assert(uitext::kLineH == FontAtlas::kUILineH,
              "uitext::kLineH must equal FontAtlas::kUILineH");
namespace {

// 解決済み矩形 (float px) → シザー矩形。RT 外へはみ出す分は D3D が切るので clamp 不要
D3D11_RECT ToScissor(const uilayout::UIRect& r)
{
    D3D11_RECT s;
    s.left = static_cast<LONG>(std::lroundf(r.x));
    s.top = static_cast<LONG>(std::lroundf(r.y));
    s.right = static_cast<LONG>(std::lroundf(r.x + r.w));
    s.bottom = static_cast<LONG>(std::lroundf(r.y + r.h));
    return s;
}

bool SameScissor(const D3D11_RECT& a, const D3D11_RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

} // namespace

bool UIRenderer::Init(GraphicsDevice& device, ShaderManager& shaders,
                      const std::wstring& assetsRoot, bool fontEmbedded)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("ui");

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(XMFLOAT4); // (invW, invH, pad, pad)
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf()))) {
        return false;
    }

    D3D11_SAMPLER_DESC smp = {};
    // 画像/8x8 ビットマップフォントは POINT でにじまず crisp に
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&smp, sampler_.GetAddressOf()))) {
        return false;
    }
    // TTF フォントはベイク 32px → 表示 10px 級の縮小になるので LINEAR (M34)
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(dev->CreateSamplerState(&smp, samplerLinear_.GetAddressOf()))) {
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

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE; // UI は最前面 (深度テスト無し)
    if (FAILED(dev->CreateDepthStencilState(&dd, depthOff_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE; // M51e: clipChildren。クリップ無し要素は RT 全域シザー
    if (FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        return false;
    }

    font_.Init(device, assetsRoot, fontEmbedded); // 失敗しても UI 自体は動く (テキストが出ないだけ)

    ready_ = true;
    return true;
}

void UIRenderer::Shutdown()
{
    cb_.Reset();
    vb_.Reset();
    sampler_.Reset();
    samplerLinear_.Reset();
    blend_.Reset();
    depthOff_.Reset();
    raster_.Reset();
    font_.Shutdown();
    verts_.clear();
    batches_.clear();
    ready_ = false;
}

void UIRenderer::PushQuad(ID3D11ShaderResourceView* srv, bool linear, float x, float y, float w,
                          float h, float u0, float v0, float u1, float v1, const XMFLOAT4& col)
{
    if (batches_.empty() || batches_.back().srv != srv || batches_.back().linear != linear
        || !SameScissor(batches_.back().scissor, curScissor_)) {
        Batch b;
        b.srv = srv;
        b.linear = linear;
        b.scissor = curScissor_;
        b.start = static_cast<uint32_t>(verts_.size());
        b.count = 0;
        batches_.push_back(b);
    }
    UIVertex q[6] = {
        { { x, y }, { u0, v0 }, col },     { { x + w, y }, { u1, v0 }, col },
        { { x + w, y + h }, { u1, v1 }, col }, { { x, y }, { u0, v0 }, col },
        { { x + w, y + h }, { u1, v1 }, col }, { { x, y + h }, { u0, v1 }, col },
    };
    if (hasXform_) {
        // M75a: 回転/スケール要素だけ頂点を変換する (テキストのグリフも 1 枚ずつ回る)
        for (UIVertex& v : q) {
            const float px = v.pos.x;
            const float py = v.pos.y;
            v.pos.x = xf_[0] * px + xf_[2] * py + xf_[4];
            v.pos.y = xf_[1] * px + xf_[3] * py + xf_[5];
        }
    }
    for (const UIVertex& v : q) {
        verts_.push_back(v);
    }
    batches_.back().count += 6;
}

void UIRenderer::PushTextLine(const char* begin, const char* end, float x, float y, float scale,
                              const XMFLOAT4& col)
{
    if (!begin || !font_.IsReady() || !font_.SRV()) {
        return;
    }
    const float k = font_.GlyphScale(scale); // ベイク px → 画面 px
    const float ascent = font_.AscentPx();
    const bool linear = font_.IsTtf();
    const FontGlyphInfo* fallback = font_.Find(static_cast<uint32_t>('?'));
    float penX = x;
    const char* p = begin;
    while (p < end) {
        const uint32_t cp = fontgeom::Utf8Next(p);
        if (cp == 0) {
            break;
        }
        if (cp < 0x20) {
            continue; // 行分割は textlayout 済み — '\n' もここには来ない前提だが無害
        }
        const FontGlyphInfo* gp = font_.Find(cp);
        if (!gp) {
            gp = fallback;
        }
        if (!gp) {
            continue;
        }
        const FontGlyphInfo& g = *gp;
        // 可視グリフのみクアッド化 (空白は advance だけ)。ベースライン = topY + ascent
        if (g.w > 0.0f && g.h > 0.0f) {
            const float gx = penX + g.xoff * k;
            const float gy = y + (ascent + g.yoff) * k;
            PushQuad(font_.SRV(), linear, gx, gy, g.w * k, g.h * k, g.u0, g.v0, g.u1, g.v1, col);
        }
        penX += g.advance * k;
    }
}

void UIRenderer::PushTextInRect(const char* s, float rx, float ry, float rw, float rh, float scale,
                                const XMFLOAT4& col, int align, bool wrap)
{
    if (!s || !font_.IsReady() || !font_.SRV()) {
        return;
    }
    const float k = font_.GlyphScale(scale);
    const float lineH = LineHeight(scale);
    std::vector<textlayout::Line> lines;
    textlayout::LayoutText(font_.Glyphs(), s, k, wrap, rw, lines);
    if (lines.empty()) {
        return;
    }
    const float totalH = static_cast<float>(lines.size()) * lineH;
    float lineY = ry + textlayout::AlignY(align, totalH, rh);
    for (const textlayout::Line& ln : lines) {
        PushTextLine(ln.begin, ln.end, rx + textlayout::AlignX(align, ln.width, rw), lineY, scale,
                     col);
        lineY += lineH;
    }
}

void UIRenderer::Render(World& world, GraphicsDevice& device, ShaderManager& shaders,
                        RenderResources& resources, ID3D11RenderTargetView* rtv, int width,
                        int height, const UIInteractionState* ui,
                        const uilayout::UIWorldContext* worldCtx)
{
    if (!ready_ || width <= 0 || height <= 0) {
        return;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid) {
        return;
    }

    // ---- 収集 (canvas.sortOrder → order → entity.index の明示キーで安定ソート) ----
    // M75c: キーの先頭は属する Canvas の sortOrder (既定キャンバスは 0)。
    // HitTest の「最前面」と同じキーなので、手前に見えている要素が押せる
    struct Item {
        int32_t sortOrder;
        int32_t order;
        EntityID e;
        EntityID canvas; // 属する Canvas (kNullEntity = 既定キャンバス)
        const UIElementComponent* el;
    };
    std::vector<Item> items;
    const ComponentTypeId req[] = { UIElementComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(UIElementComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* el = static_cast<const UIElementComponent*>(arch.GetPtr(ci, row));
            const EntityID canvasE = uilayout::FindCanvas(world, e);
            items.push_back({ uilayout::CanvasSortOrder(world, canvasE), el->order, e, canvasE, el });
        }
    });
    if (items.empty()) {
        return;
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.sortOrder != b.sortOrder) {
            return a.sortOrder < b.sortOrder;
        }
        if (a.order != b.order) {
            return a.order < b.order;
        }
        return a.e.index < b.e.index;
    });

    // ---- フェーズ 1: 全テキストのグリフを焼成 (アトラス成長はここだけで起きる) ----
    for (const Item& it : items) {
        if (it.el->kind == 1 || it.el->kind == 2) {
            font_.EnsureText(it.el->text);
        }
    }

    // ---- フェーズ 2: クアッド構築 (SRV はもう安定) ----
    verts_.clear();
    batches_.clear();
    Texture* whiteTex = resources.textures.Get(resources.textures.White());
    whiteSrv_ = whiteTex ? whiteTex->srv.Get() : nullptr;
    if (!whiteSrv_) {
        return;
    }

    // ---- キャンバス (M70b) ----
    // 矩形解決は**キャンバス単位**で行い、実 px へは最後に一様スケールを掛けるだけにする。
    // 解く側 (uilayout) に px を持ち込まないので、ヒットテスト / フォーカスナビ
    // (EngineApiTable、キャンバス単位) と描画が同じ数値の上で一致する。
    // ★16:9 ならキャンバスは厳密に 1920x1080 で、960x540 なら canvasScale はちょうど 0.5。
    //   資産側の数値を 2 倍してあるので、この経路は IEEE754 で元の絵とビット一致する
    const uilayout::CanvasInfo canvas = uilayout::CanvasSize(width, height);
    const int canvasW = canvas.w;
    const int canvasH = canvas.h;
    const float defaultScale = canvas.scale;

    const D3D11_RECT fullScissor = { 0, 0, width, height };
    // M75e: 自動レイアウトのメモはこのフレームの描画 1 回ぶん (結果は変えない。描画中に World は動かない)
    uilayout::LayoutScratch layoutScratch;
    // M75f: Selectable の遷移 (色 / 画像) と off の Toggle のチェックマーク。ウィジェットの無いシーンでは空で、
    // 下の color は el.color の複写のまま
    std::vector<uiwidgets::VisualOverride> overrides;
    uiwidgets::CollectVisualOverrides(world, ui, overrides);
    for (const Item& it : items) {
        const UIElementComponent& el = *it.el;
        uiwidgets::VisualOverride vis;
        if (!overrides.empty()) {
            vis = uiwidgets::MergedOverrideFor(overrides, it.e);
            if ((vis.flags & uiwidgets::kVisHidden) != 0) {
                continue;
            }
        }
        XMFLOAT4 color = el.color;
        if ((vis.flags & uiwidgets::kVisTint) != 0) {
            // Unity の ColorBlock と同じく targetGraphic の色に掛ける (倍率で 1 を超えた分は切る)
            color.x = std::clamp(el.color.x * vis.tint.x, 0.0f, 1.0f);
            color.y = std::clamp(el.color.y * vis.tint.y, 0.0f, 1.0f);
            color.z = std::clamp(el.color.z * vis.tint.z, 0.0f, 1.0f);
            color.w = std::clamp(el.color.w * vis.tint.w, 0.0f, 1.0f);
        }
        // M75c: この要素のキャンバス単位 → 実 px。明示 Canvas は既定キャンバス単位への倍率を
        // 先に掛ける (UILayout.h の CanvasOfEntity)。Canvas の無い要素は 1.0f * defaultScale =
        // defaultScale とビット一致
        const float canvasScale =
            uilayout::CanvasOfEntity(world, it.canvas, canvasW, canvasH).scale * defaultScale;
        // 矩形解決 (M51e: 親子/クリップは UILayout — UIFocusNav と共有)。クリップ祖先が
        // 無い要素は RT 全域シザー = 同じバッチにまとまる。
        // ワールド追従要素はここで射影され、背面 (クランプ OFF) は visible=false で消える。
        // res.scale (距離スケール) は矩形に折り込み済み — テキストのグリフ倍率にだけ手で掛ける
        const uilayout::UIResolved res =
            uilayout::Resolve(world, it.e, canvasW, canvasH, worldCtx, &layoutScratch);
        if (!res.visible) {
            continue;
        }
        // キャンバス単位 → 実 px。**ここが唯一の変換点**で、以降の PushQuad / PushText は
        // 実 px を積む (シザーもフォーカス枠も同じ土俵に乗る)
        const uilayout::UIRect rect = { res.rect.x * canvasScale, res.rect.y * canvasScale,
                                        res.rect.w * canvasScale, res.rect.h * canvasScale };
        const float rx = rect.x;
        const float ry = rect.y;
        // M75a: 回転/スケールはキャンバス単位の xform を px 系へ (線形部はそのまま、
        // 平行移動だけ canvasScale を掛ける: s*(A q + t) = A (s q) + s t)
        hasXform_ = res.hasXform;
        if (res.hasXform) {
            xf_[0] = res.xform.a;
            xf_[1] = res.xform.b;
            xf_[2] = res.xform.c;
            xf_[3] = res.xform.d;
            xf_[4] = res.xform.tx * canvasScale;
            xf_[5] = res.xform.ty * canvasScale;
        }
        const float textScale = el.fontScale * res.scale * canvasScale;
        curScissor_ = fullScissor;
        {
            const uilayout::UIRect c = uilayout::ResolveClipRect(world, it.e, canvasW, canvasH,
                                                                 worldCtx, &layoutScratch);
            if (c.w <= 0.0f || c.h <= 0.0f) {
                continue; // 祖先クリップで完全に隠れている
            }
            const uilayout::UIRect clip = { c.x * canvasScale, c.y * canvasScale,
                                            c.w * canvasScale, c.h * canvasScale };
            if (clip.x > 0.0f || clip.y > 0.0f || clip.x + clip.w < static_cast<float>(width)
                || clip.y + clip.h < static_cast<float>(height)) {
                curScissor_ = ToScissor(clip);
            }
        }

        if (el.kind == 1) {
            // テキスト (背景無し)。M51e: 矩形 (w,h) 内で整列 + 折返し (既定 0/0 = 左上)
            PushTextInRect(el.text, rx, ry, rect.w, rect.h, textScale, color, el.align,
                           el.wrap != 0);
        } else if (el.kind == 2) {
            // ボタン: 背景 + hover/press ハイライト + 中央ラベル。
            // ★M70c: 判定はエンジンが tick 中に確定した状態を読むだけ (自前で矩形と
            //   マウスを比べない) = 「光っている要素」と「押したことになる要素」が
            //   構造的に同じものになる
            XMFLOAT4 bg = color;
            const uint32_t bits = ui ? uiinteract::BitsFor(*ui, it.e) : 0u;
            // M75f: Selectable を持つボタン / 遷移の対象になったボタンはハードコードのハイライトを当てない
            const bool legacyHighlight =
                (vis.flags & (uiwidgets::kVisTint | uiwidgets::kVisNoLegacyHighlight)) == 0;
            if (legacyHighlight && (bits & uiinteract::kHovered)) {
                // press で暗く、hover で明るく。pressed は「掴んだ要素」なので、
                // 押したまま外へ出ているあいだは hovered が落ちて暗くならない
                const float k = (bits & uiinteract::kPressed) ? 0.8f : 1.25f;
                bg.x = std::min(1.0f, bg.x * k);
                bg.y = std::min(1.0f, bg.y * k);
                bg.z = std::min(1.0f, bg.z * k);
            }
            PushQuad(whiteSrv_, false, rx, ry, rect.w, rect.h, 0, 0, 1, 1, bg);
            const XMFLOAT4 label = { 1, 1, 1, 1 };
            PushTextInRect(el.text, rx, ry, rect.w, rect.h, textScale, label, 4, false);
        } else {
            // パネル / 画像 (M35: 9-slice / fillAmount 対応)
            ID3D11ShaderResourceView* srv = whiteSrv_;
            float texW = 0.0f, texH = 0.0f;
            // M75f: Sprite Swap はその状態の画像へ差し替える (無ければ元のテクスチャ)
            const AssetID texture =
                ((vis.flags & uiwidgets::kVisSprite) != 0) ? vis.sprite : el.texture;
            if (texture.value != 0) {
                Texture* t = resources.textures.Get(texture);
                if (t && t->srv) {
                    srv = t->srv.Get();
                    texW = static_cast<float>(t->width);
                    texH = static_cast<float>(t->height);
                }
            }
            if (el.sliced != 0 && texW > 0.0f) {
                // 9-slice の境界は距離スケール対象外 (枠の太さは見た目の一貫性を優先) だが、
                // **キャンバススケールは掛ける** — sliceBorder は他の x/y/w/h と同じ
                // キャンバス単位でオーサリングされているので、ここだけ実 px 扱いにすると
                // 解像度を変えた瞬間に枠だけ太さが合わなくなる (M70b)
                uigeom::UIQuad quads[9];
                const int n = uigeom::Build9Slice(
                    rx, ry, rect.w, rect.h, el.sliceBorder.x * canvasScale,
                    el.sliceBorder.y * canvasScale, el.sliceBorder.z * canvasScale,
                    el.sliceBorder.w * canvasScale, texW, texH, quads);
                for (int i = 0; i < n; ++i) {
                    const uigeom::UIQuad& q = quads[i];
                    PushQuad(srv, false, q.x, q.y, q.w, q.h, q.u0, q.v0, q.u1, q.v1, color);
                }
            } else if (el.fillMode != 0) {
                const uigeom::UIQuad q =
                    uigeom::BuildFillQuad(rx, ry, rect.w, rect.h, el.fillMode, el.fillAmount);
                if (q.w > 0.0f && q.h > 0.0f) {
                    PushQuad(srv, false, q.x, q.y, q.w, q.h, q.u0, q.v0, q.u1, q.v1, color);
                }
            } else {
                PushQuad(srv, false, rx, ry, rect.w, rect.h, 0, 0, 1, 1, color);
            }
        }

        // フォーカス枠 (M35): focusable かつ focused (スクリプトが書く表示専用状態) の要素に
        // 白枠を重ねる。kind 問わず矩形 (x,y,w,h) 基準
        if (el.focusable != 0 && el.focused != 0) {
            // M70b: リング幅もキャンバス単位 (基準解像度で 2 px)。実 px 固定にすると
            // 4K で髪の毛のように細くなる — UI 層のものは一様に伸縮させる
            const float kRing = 2.0f * canvasScale;
            const XMFLOAT4 ring = { 1.0f, 1.0f, 1.0f, 0.9f };
            PushQuad(whiteSrv_, false, rx - kRing, ry - kRing, rect.w + kRing * 2, kRing, 0, 0, 1,
                     1, ring); // 上
            PushQuad(whiteSrv_, false, rx - kRing, ry + rect.h, rect.w + kRing * 2, kRing, 0, 0, 1,
                     1, ring); // 下
            PushQuad(whiteSrv_, false, rx - kRing, ry, kRing, rect.h, 0, 0, 1, 1, ring); // 左
            PushQuad(whiteSrv_, false, rx + rect.w, ry, kRing, rect.h, 0, 0, 1, 1, ring); // 右
        }
    }

    if (verts_.empty()) {
        return;
    }

    // ---- VB 確保 + アップロード ----
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();
    const uint32_t needed = static_cast<uint32_t>(verts_.size());
    if (needed > vbCapacity_) {
        vbCapacity_ = needed + needed / 2 + 512;
        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = vbCapacity_ * sizeof(UIVertex);
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
        memcpy(mapped.pData, verts_.data(), verts_.size() * sizeof(UIVertex));
        dc->Unmap(vb_.Get(), 0);
    }

    // 定数バッファ (1/screen)
    const XMFLOAT4 inv(1.0f / width, 1.0f / height, 0.0f, 0.0f);
    if (SUCCEEDED(dc->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &inv, sizeof(inv));
        dc->Unmap(cb_.Get(), 0);
    }

    // ---- 描画 ----
    dc->OMSetRenderTargets(1, &rtv, nullptr); // 深度なし
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    const UINT stride = sizeof(UIVertex);
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
    dc->OMSetDepthStencilState(depthOff_.Get(), 0);
    dc->RSSetState(raster_.Get());

    for (const Batch& b : batches_) {
        ID3D11SamplerState* samps[1] = { b.linear ? samplerLinear_.Get() : sampler_.Get() };
        dc->PSSetSamplers(0, 1, samps);
        ID3D11ShaderResourceView* srvs[1] = { b.srv };
        dc->PSSetShaderResources(0, 1, srvs);
        dc->RSSetScissorRects(1, &b.scissor); // ラスタライザは常時 ScissorEnable (M51e)
        dc->Draw(b.count, b.start);
    }
}

} // namespace mye
