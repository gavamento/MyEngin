//====================================================================================
//                          Sprite2DRenderer.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D の収集とソートの実装
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"
#include "Engine/Engine/Scene/SortingLayers.h"
#include "Engine/Renderer/Device/GpuResources.h"

using namespace DirectX;

namespace mye {
namespace {

// 内蔵マゼンタの一辺 (画素)。1 ワールド単位 = 画素数 にして、未解決でも 1 単位角で見える大きさにする
constexpr int kMissingTextureSize = 16;
constexpr int kMissingPixelsPerUnit = kMissingTextureSize;

// SortingGroup2D を祖先へ辿る深さの上限 (親リンクの循環で止まらなくなるのを避ける)
constexpr int kMaxGroupWalk = 64;
// タイル 1 方向のピース数の上限 (4096 頂点 = 682 枚で必ず上限に当たるので、それ以上は数えない)
constexpr int kMaxPiecesPerAxis = 700;
// 端のタイルがほぼ丁度のときに薄いタイルを出さないための余裕 (タイル数に対する比)
constexpr float kTileCountEpsilon = 1.0e-4f;

// 1 方向 (x か y) の 1 区間。dest はワールドの小さい側から、src は画素の小さい側から
struct Piece {
    float d0, d1; // ワールド座標 (d0 < d1)
    float s0, s1; // テクスチャ画素 (s0 < s1)
};

// セルの 1 方向を区間に割る。tile = false は区間 1 つへ伸縮、true は原寸 (step = src / ppu) で並べて端を切る。
// anchorMax = タイルの基準を src の大きい側に置く (y は下端から積み、端のタイルは上側が切れる)
void BuildAxis(bool tile, bool anchorMax, float d0, float d1, float s0, float s1, float ppu, std::vector<Piece>& out,
               bool& capped)
{
    out.clear();
    const float destLen = d1 - d0;
    const float srcLen = s1 - s0;
    if (destLen <= 0.0f || srcLen <= 0.0f) {
        return;
    }
    if (!tile) {
        out.push_back({ d0, d1, s0, s1 });
        return;
    }
    const float step = srcLen / ppu;
    int count = static_cast<int>(std::ceil(destLen / step - kTileCountEpsilon));
    if (count < 1) {
        count = 1;
    }
    if (count > kMaxPiecesPerAxis) {
        count = kMaxPiecesPerAxis;
        capped = true;
    }
    for (int i = 0; i < count; ++i) {
        const float p0 = d0 + static_cast<float>(i) * step;
        const float p1 = (std::min)(p0 + step, d1);
        const float len = p1 - p0;
        if (len <= 0.0f) {
            break;
        }
        const float px = len * ppu;
        Piece pc;
        pc.d0 = p0;
        pc.d1 = p1;
        pc.s0 = anchorMax ? s1 - px : s0;
        pc.s1 = anchorMax ? s1 : s0 + px;
        out.push_back(pc);
    }
}

// 祖先 (自分を含む) の最も近い SortingGroup2D。無ければ nullptr
const SortingGroup2DComponent* FindGroup(World& world, EntityID e, EntityID& groupEntity)
{
    EntityID cur = e;
    for (int depth = 0; depth < kMaxGroupWalk && !cur.IsNull(); ++depth) {
        const auto* g = world.GetComponent<SortingGroup2DComponent>(cur);
        if (g != nullptr) {
            groupEntity = cur;
            return g;
        }
        cur = world.GetParent(cur);
    }
    return nullptr;
}

} // namespace

void Sprite2DRenderer::LocalRect(const SpriteDef& def, int texWidth, int texHeight, float out[4])
{
    const float w = def.IsFullTexture() ? static_cast<float>(texWidth) : static_cast<float>(def.rect[2]);
    const float h = def.IsFullTexture() ? static_cast<float>(texHeight) : static_cast<float>(def.rect[3]);
    const float ppu = static_cast<float>((std::max)(def.ppu, 1));
    const float sizeX = w / ppu;
    const float sizeY = h / ppu;
    out[0] = -def.pivot[0] * sizeX;
    out[1] = -def.pivot[1] * sizeY;
    out[2] = (1.0f - def.pivot[0]) * sizeX;
    out[3] = (1.0f - def.pivot[1]) * sizeY;
}

AssetID Sprite2DRenderer::EnsureMissingTexture(RenderResources& resources)
{
    if (missingTexture_.IsNull()) {
        uint8_t pixels[kMissingTextureSize * kMissingTextureSize * 4];
        for (int i = 0; i < kMissingTextureSize * kMissingTextureSize; ++i) {
            pixels[i * 4 + 0] = 255;
            pixels[i * 4 + 1] = 0;
            pixels[i * 4 + 2] = 255;
            pixels[i * 4 + 3] = 255;
        }
        missingTexture_ = resources.textures.CreateFromRgba8("sprite2d_missing", pixels, kMissingTextureSize,
                                                            kMissingTextureSize, false, false);
    }
    return missingTexture_;
}

void Sprite2DRenderer::Push(AssetID texture, bool pointFilter, uint8_t stencil, float alphaCutoff,
                            const Sprite2DVertex* v, uint32_t count)
{
    if (count == 0) {
        return;
    }
    if (list_.batches.empty() || list_.batches.back().texture != texture
        || list_.batches.back().pointFilter != pointFilter || list_.batches.back().stencil != stencil
        || list_.batches.back().alphaCutoff != alphaCutoff) {
        Sprite2DBatch b;
        b.texture = texture;
        b.pointFilter = pointFilter;
        b.stencil = stencil;
        b.alphaCutoff = alphaCutoff;
        b.start = static_cast<uint32_t>(list_.vertices.size());
        list_.batches.push_back(b);
    }
    list_.vertices.insert(list_.vertices.end(), v, v + count);
    list_.batches.back().count += count;
}

bool Sprite2DRenderer::Resolve(AssetID sprite, RenderResources& resources, bool allowMissing, Resolved& out)
{
    TextureLibrary& textures = resources.textures;
    SpriteLibrary* lib = spritelib::Library();
    const SpriteDef* def = (lib != nullptr) ? lib->Get(sprite) : nullptr;
    bool useMissing = (def == nullptr);
    if (def != nullptr) {
        const Texture* tex = textures.Get(def->texture);
        if (tex == nullptr) {
            // 未ロード: 読み込みを頼んで、このフレームは描かない。パスが引けない (ファイルが無い) ものはマゼンタ
            const std::wstring path = assetguid::ResolvePath(def->texture.value);
            if (path.empty()) {
                useMissing = true;
            } else {
                textures.RequestLoadFileAsync(path);
                return false;
            }
        } else if (textures.IsPending(def->texture)) {
            return false; // 白のプレースホルダ (1x1) の間は寸法も絵も嘘になる
        } else {
            out.texW = static_cast<float>((std::max)(tex->width, 1));
            out.texH = static_cast<float>((std::max)(tex->height, 1));
            if (def->IsFullTexture()) {
                out.rect[0] = 0.0f;
                out.rect[1] = 0.0f;
                out.rect[2] = out.texW;
                out.rect[3] = out.texH;
            } else {
                for (int i = 0; i < 4; ++i) {
                    out.rect[i] = static_cast<float>(def->rect[i]);
                }
            }
            for (int i = 0; i < 4; ++i) {
                out.border[i] = static_cast<float>((std::max)(def->border[i], 0));
            }
            out.pivot[0] = def->pivot[0];
            out.pivot[1] = def->pivot[1];
            out.ppu = static_cast<float>((std::max)(def->ppu, 1));
            out.texture = def->texture;
            LocalRect(*def, static_cast<int>(out.texW), static_cast<int>(out.texH), out.local);
            out.pointFilter = (def->filter == 2);
        }
    }
    if (useMissing) {
        if (!allowMissing) {
            return false;
        }
        out.texture = EnsureMissingTexture(resources);
        out.texW = static_cast<float>(kMissingTextureSize);
        out.texH = static_cast<float>(kMissingTextureSize);
        out.rect[0] = 0.0f;
        out.rect[1] = 0.0f;
        out.rect[2] = out.texW;
        out.rect[3] = out.texH;
        out.ppu = static_cast<float>(kMissingPixelsPerUnit);
        constexpr float kHalf = 0.5f * kMissingTextureSize / kMissingPixelsPerUnit;
        out.local[0] = -kHalf;
        out.local[1] = -kHalf;
        out.local[2] = kHalf;
        out.local[3] = kHalf;
        out.missing = true;
    }
    return !out.texture.IsNull();
}

// ローカル矩形 (x0 < x1, y0 < y1、pivot が原点) を 1 枚のクアッドにして scratch_ へ足す。(u0, v0) が左上。
// flip は pivot まわりの鏡映 = 位置の符号を反転するだけで UV はそのまま
void Sprite2DRenderer::AddQuad(const XMFLOAT4X4& m, float flipX, float flipY, const float color[4], float x0,
                               float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
    if (scratch_.size() + 6 > kMaxSpriteVertices) {
        capped_ = true;
        return;
    }
    // 左上, 右上, 右下, 左下 (テクスチャは上が v0)
    const float lx[4] = { x0 * flipX, x1 * flipX, x1 * flipX, x0 * flipX };
    const float ly[4] = { y1 * flipY, y1 * flipY, y0 * flipY, y0 * flipY };
    const float cu[4] = { u0, u1, u1, u0 };
    const float cv[4] = { v0, v0, v1, v1 };
    Sprite2DVertex corner[4];
    for (int i = 0; i < 4; ++i) {
        // ローカル (x, y, 0) × ワールド行列 (行ベクトル規約)
        corner[i].pos[0] = lx[i] * m._11 + ly[i] * m._21 + m._41;
        corner[i].pos[1] = lx[i] * m._12 + ly[i] * m._22 + m._42;
        corner[i].pos[2] = lx[i] * m._13 + ly[i] * m._23 + m._43;
        corner[i].uv[0] = cu[i];
        corner[i].uv[1] = cv[i];
        for (int c = 0; c < 4; ++c) {
            corner[i].color[c] = color[c];
        }
    }
    const Sprite2DVertex quad[6] = { corner[0], corner[1], corner[2], corner[0], corner[2], corner[3] };
    scratch_.insert(scratch_.end(), quad, quad + 6);
}

// Sliced / Tiled: border で 3x3 に割り、角は原寸 (border / ppu)、辺と中央は size に合わせて伸縮 (Sliced) か
// 原寸で繰り返し端を切る (Tiled)。size が角の合計より小さいときは角を縮める
bool Sprite2DRenderer::BuildSliceQuads(const SliceInput& in, std::vector<SliceQuad>& out)
{
    out.clear();
    if (!(in.sizeX > 0.0f) || !(in.sizeY > 0.0f)) {
        return false;
    }
    const float left = -in.pivotX * in.sizeX;
    const float bottom = -in.pivotY * in.sizeY;
    const float right = left + in.sizeX;
    const float top = bottom + in.sizeY;

    // border を矩形に収める (左右・上下の合計が矩形を超えたら右 / 上を削る)
    const float rw = in.rect[2];
    const float rh = in.rect[3];
    const float bl = (std::min)(in.border[0], rw);
    const float br = (std::min)(in.border[2], rw - bl);
    const float bb = (std::min)(in.border[1], rh);
    const float bt = (std::min)(in.border[3], rh - bb);

    // ワールドの角寸法。size を超えるときは比例して縮める
    float cl = bl / in.ppu;
    float cr = br / in.ppu;
    float cb = bb / in.ppu;
    float ct = bt / in.ppu;
    if (cl + cr > in.sizeX) {
        const float k = in.sizeX / (cl + cr);
        cl *= k;
        cr *= k;
    }
    if (cb + ct > in.sizeY) {
        const float k = in.sizeY / (cb + ct);
        cb *= k;
        ct *= k;
    }

    // 列は左 / 中 / 右。行は下 / 中 / 上 (ワールドの y は上向き、src の画素は下向きなので sy は大きい側から)
    const float dx[4] = { left, left + cl, right - cr, right };
    const float sx[4] = { in.rect[0], in.rect[0] + bl, in.rect[0] + rw - br, in.rect[0] + rw };
    const float dy[4] = { bottom, bottom + cb, top - ct, top };
    const float sy[4] = { in.rect[1] + rh, in.rect[1] + rh - bb, in.rect[1] + bt, in.rect[1] };

    bool capped = false;
    std::vector<Piece> xs;
    std::vector<Piece> ys;
    for (int row = 0; row < 3; ++row) {
        BuildAxis(in.tiled && row == 1, true, dy[row], dy[row + 1], sy[row + 1], sy[row], in.ppu, ys, capped);
        for (int col = 0; col < 3; ++col) {
            BuildAxis(in.tiled && col == 1, false, dx[col], dx[col + 1], sx[col], sx[col + 1], in.ppu, xs, capped);
            for (const Piece& py : ys) {
                for (const Piece& px : xs) {
                    if ((out.size() + 1) * 6 > kMaxSpriteVertices) {
                        return true;
                    }
                    out.push_back({ px.d0, py.d0, px.d1, py.d1, px.s0 / in.texW, py.s0 / in.texH, px.s1 / in.texW,
                                    py.s1 / in.texH });
                }
            }
        }
    }
    return capped;
}

void Sprite2DRenderer::EmitSprite(const Item& item, RenderResources& resources)
{
    const Sprite2DComponent& sp = *item.sprite;
    Resolved r;
    if (!Resolve(sp.sprite, resources, true, r)) {
        return;
    }
    const XMFLOAT4X4& m = *item.world;

    scratch_.clear();
    capped_ = false;
    // マゼンタ (未解決) は寸法が無いので常に Simple。border 全 0 の Sliced は size 寸法の 1 枚になる
    if (!r.missing && (sp.drawMode == 1 || sp.drawMode == 2)) {
        SliceInput in;
        std::copy(r.rect, r.rect + 4, in.rect);
        std::copy(r.border, r.border + 4, in.border);
        in.pivotX = r.pivot[0];
        in.pivotY = r.pivot[1];
        in.ppu = r.ppu;
        in.texW = r.texW;
        in.texH = r.texH;
        in.sizeX = sp.size.x;
        in.sizeY = sp.size.y;
        in.tiled = (sp.drawMode == 2);
        capped_ = BuildSliceQuads(in, sliceQuads_);
        const float color[4] = { sp.color.x, sp.color.y, sp.color.z, sp.color.w };
        for (const SliceQuad& q : sliceQuads_) {
            AddQuad(m, sp.flipX ? -1.0f : 1.0f, sp.flipY ? -1.0f : 1.0f, color, q.x0, q.y0, q.x1, q.y1, q.u0, q.v0,
                    q.u1, q.v1);
        }
    } else {
        const float color[4] = { sp.color.x, sp.color.y, sp.color.z, sp.color.w };
        AddQuad(m, sp.flipX ? -1.0f : 1.0f, sp.flipY ? -1.0f : 1.0f, color, r.local[0], r.local[1], r.local[2],
                r.local[3], r.rect[0] / r.texW, r.rect[1] / r.texH, (r.rect[0] + r.rect[2]) / r.texW,
                (r.rect[1] + r.rect[3]) / r.texH);
    }
    if (capped_ && !warnedVertexCap_) {
        warnedVertexCap_ = true;
        MYE_LOG_WARN("[sprite2d] Tiled / Sliced の頂点が上限 (%u) を超えたので切りました。size を小さくするか、"
                     "タイルを大きくしてください",
                     static_cast<unsigned>(kMaxSpriteVertices));
    }

    // マスクとの関係: 描画順でここまでに出たマスクのうち、range にこのスプライトの order が入るものがあるときだけ効く
    uint8_t stencil = sprite2dstencil::kNone;
    if (sp.maskInteraction == 1 || sp.maskInteraction == 2) {
        for (const auto& range : seenMasks_) {
            if (sp.orderInLayer >= range.first && sp.orderInLayer <= range.second) {
                stencil = (sp.maskInteraction == 1) ? sprite2dstencil::kTestInside : sprite2dstencil::kTestOutside;
                break;
            }
        }
    }
    Push(r.texture, r.pointFilter, stencil, 0.0f, scratch_.data(), static_cast<uint32_t>(scratch_.size()));
}

void Sprite2DRenderer::EmitMask(const Item& item, RenderResources& resources)
{
    const SpriteMask2DComponent& mk = *item.mask;
    Resolved r;
    if (!Resolve(mk.sprite, resources, false, r)) {
        return; // スプライトが無い / 未ロードのマスクは何も書かない
    }
    scratch_.clear();
    capped_ = false;
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    AddQuad(*item.world, 1.0f, 1.0f, white, r.local[0], r.local[1], r.local[2], r.local[3], r.rect[0] / r.texW,
            r.rect[1] / r.texH, (r.rect[0] + r.rect[2]) / r.texW, (r.rect[1] + r.rect[3]) / r.texH);
    seenMasks_.emplace_back(mk.rangeBack, mk.rangeFront);
    Push(r.texture, r.pointFilter, sprite2dstencil::kWriteMask, mk.alphaCutoff, scratch_.data(),
         static_cast<uint32_t>(scratch_.size()));
}

const Sprite2DDrawList& Sprite2DRenderer::Collect(World& world, RenderResources& resources)
{
    list_.Clear();
    items_.clear();
    seenMasks_.clear();

    const SortingLayers& layers = SortingLayers::Get();

    // グループがシーンに無ければ祖先を辿らない (多くのシーンはグループを持たない)
    bool hasGroups = false;
    {
        const ComponentTypeId reqGroup[] = { SortingGroup2DComponent::sTypeId };
        world.ForEachArchetype(reqGroup, [&](Archetype& arch) {
            if (arch.Count() > 0) {
                hasGroups = true;
            }
        });
    }

    // 外側 = 自分のキー。グループの子孫なら外側 = グループのキー、内側 = 自分のキー
    auto assignKeys = [&](Item& it, EntityID e, int32_t layerId, int32_t order) {
        it.outerLayer = layers.OrderOf(layerId);
        it.outerOrder = order;
        it.outerIndex = e.index;
        it.innerLayer = 0;
        it.innerOrder = 0;
        it.innerIndex = 0;
        EntityID groupEntity = kNullEntity;
        const SortingGroup2DComponent* g = (hasGroups && !it.isMask) ? FindGroup(world, e, groupEntity) : nullptr;
        if (g != nullptr) {
            it.innerLayer = it.outerLayer;
            it.innerOrder = it.outerOrder;
            it.innerIndex = it.outerIndex;
            it.outerLayer = layers.OrderOf(g->sortingLayerId);
            it.outerOrder = g->orderInLayer;
            it.outerIndex = groupEntity.index;
        }
    };

    {
        const ComponentTypeId req[] = { Sprite2DComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int si = arch.FindTypeIndex(Sprite2DComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                Item it;
                it.sprite = static_cast<const Sprite2DComponent*>(arch.GetPtr(si, row));
                it.world = &static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                assignKeys(it, e, it.sprite->sortingLayerId, it.sprite->orderInLayer);
                items_.push_back(it);
            }
        });
    }
    {
        const ComponentTypeId req[] = { SpriteMask2DComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int mi = arch.FindTypeIndex(SpriteMask2DComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                Item it;
                it.isMask = true;
                it.mask = static_cast<const SpriteMask2DComponent*>(arch.GetPtr(mi, row));
                it.world = &static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                assignKeys(it, e, it.mask->sortingLayerId, it.mask->orderInLayer);
                items_.push_back(it);
            }
        });
    }
    if (items_.empty()) {
        return list_;
    }

    // 浮動小数点を使わない整数キー。entity.index まで入れるので同値は無く、並びはアーキタイプの走査順に依らない
    // (同じエンティティにスプライトとマスクがあるときだけ isMask で決める)
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        return std::tie(a.outerLayer, a.outerOrder, a.outerIndex, a.innerLayer, a.innerOrder, a.innerIndex, a.isMask)
               < std::tie(b.outerLayer, b.outerOrder, b.outerIndex, b.innerLayer, b.innerOrder, b.innerIndex,
                          b.isMask);
    });
    list_.vertices.reserve(items_.size() * 6);
    for (const Item& it : items_) {
        if (it.isMask) {
            EmitMask(it, resources);
        } else {
            EmitSprite(it, resources);
        }
    }
    return list_;
}

} // namespace mye
