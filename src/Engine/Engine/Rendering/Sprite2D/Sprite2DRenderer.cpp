//====================================================================================
//                          Sprite2DRenderer.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D の収集とソートの実装
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"
#include "Engine/Engine/Scene/SortingLayers.h"
#include "Engine/Engine/Tilemap/TilemapSystem.h"
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
// 視錐台の逆行列が特異とみなす行列式の下限 (これ未満はチャンクを切らない)
constexpr float kMinFrustumDeterminant = 1.0e-20f;

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

// 正射影のとき、カメラの視錐台 (直方体) を囲むワールドの AABB。透視 / 行列が無い / 特異は false (切らない)
bool Sprite2DRenderer::OrthoWorldBounds(const XMFLOAT4X4* view, const XMFLOAT4X4* proj, XMFLOAT3& outMin, XMFLOAT3& outMax)
{
    // XMMatrixOrthographicLH は _34 = 0、_44 = 1。透視は _34 = 1、_44 = 0
    if (view == nullptr || proj == nullptr || proj->_34 != 0.0f || proj->_44 != 1.0f) {
        return false;
    }
    const XMMATRIX viewProj = XMMatrixMultiply(XMLoadFloat4x4(view), XMLoadFloat4x4(proj));
    XMVECTOR det;
    const XMMATRIX inv = XMMatrixInverse(&det, viewProj);
    if (std::fabs(XMVectorGetX(det)) < kMinFrustumDeterminant) {
        return false;
    }
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        const XMVECTOR ndc = XMVectorSet((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : 0.0f, 1.0f);
        XMFLOAT3 p;
        XMStoreFloat3(&p, XMVector3TransformCoord(ndc, inv));
        if (first) {
            outMin = p;
            outMax = p;
            first = false;
        } else {
            outMin = { (std::min)(outMin.x, p.x), (std::min)(outMin.y, p.y), (std::min)(outMin.z, p.z) };
            outMax = { (std::max)(outMax.x, p.x), (std::max)(outMax.y, p.y), (std::max)(outMax.z, p.z) };
        }
    }
    return true;
}

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

void Sprite2DRenderer::AssignKeys(Item& it, World& world, bool hasGroups, EntityID e, int32_t layerId,
                                  int32_t order) const
{
    const SortingLayers& layers = SortingLayers::Get();
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
}

void Sprite2DRenderer::TileCornerUv(float u0, float v0, float u1, float v1, bool flipX, bool flipY, uint32_t rotation,
                                    float out[4][2])
{
    // 左上, 右上, 右下, 左下
    float c[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
    const auto swap2 = [&c](int a, int b) {
        std::swap(c[a][0], c[b][0]);
        std::swap(c[a][1], c[b][1]);
    };
    if (flipX) {
        swap2(0, 1);
        swap2(3, 2);
    }
    if (flipY) {
        swap2(0, 3);
        swap2(1, 2);
    }
    // 絵を反時計回りに 90 度回すと、左上にあった内容は左下へ、左下は右下へ、右下は右上へ、右上は左上へ移る
    for (uint32_t k = 0; k < (rotation & 3u); ++k) {
        float r[4][2];
        for (int i = 0; i < 4; ++i) {
            const int dst = (i + 3) % 4;
            r[dst][0] = c[i][0];
            r[dst][1] = c[i][1];
        }
        std::copy(&r[0][0], &r[0][0] + 8, &c[0][0]);
    }
    std::copy(&c[0][0], &c[0][0] + 8, &out[0][0]);
}

// チャンクを焼く: 実効セルを読み、tileId → パレット → スプライトを引いて、セルごとに 1 クアッドを原点基準で並べる。
// 非同期ロード中のスプライトがあれば complete = false にして次フレームも焼き直す
void Sprite2DRenderer::BakeTileChunk(const Item& item, World& world, RenderResources& resources, BakedChunk& out)
{
    out.vertices.clear();
    out.runs.clear();
    out.complete = true;
    const TilemapAsset* map = TilemapSystem::AssetOf(world, item.tileEntity);
    const TilePalette* palette = (map != nullptr) ? tilepalette::Resolve(map->palette) : nullptr;
    std::array<uint32_t, kTilemapChunkCells> cells;
    // sim の書き換え (override) を重ねた実効セルで焼く。sim の持ち主が未接続ならアセットの値だけ
    const TilemapSystem* sim = tilemap::System();
    const bool haveCells = (sim != nullptr)
        ? sim->GetChunkCells(world, item.tileEntity, item.tileLayer, item.tileChunk, cells)
        : TilemapSystem::AssetChunkCells(world, item.tileEntity, item.tileLayer, item.tileChunk, cells);
    if (map == nullptr || palette == nullptr || !haveCells) {
        return;
    }
    const float cellW = map->cellSize[0];
    const float cellH = map->cellSize[1];
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::map<uint64_t, Resolved> memo; // 参照するだけ。走査しない
    out.vertices.reserve(kTilemapChunkCells * 6);
    for (int32_t ly = 0; ly < kTilemapChunkSize; ++ly) {
        for (int32_t lx = 0; lx < kTilemapChunkSize; ++lx) {
            const uint32_t value = cells[static_cast<size_t>(ly) * kTilemapChunkSize + static_cast<size_t>(lx)];
            const TileDef* def = palette->Find(tilecell::TileId(value));
            if (def == nullptr || def->sprite.IsNull()) {
                continue; // 空 / パレットに無い id / スプライト無しは描かない
            }
            auto found = memo.find(def->sprite.value);
            if (found == memo.end()) {
                Resolved r;
                if (!Resolve(def->sprite, resources, true, r)) {
                    out.complete = false;
                    continue;
                }
                found = memo.emplace(def->sprite.value, r).first;
            }
            const Resolved& r = found->second;
            float uv[4][2];
            TileCornerUv(r.rect[0] / r.texW, r.rect[1] / r.texH, (r.rect[0] + r.rect[2]) / r.texW,
                         (r.rect[1] + r.rect[3]) / r.texH, (value & tilecell::kFlipX) != 0,
                         (value & tilecell::kFlipY) != 0, tilecell::Rotation(value), uv);
            const float x0 = static_cast<float>(item.tileChunk.cx * kTilemapChunkSize + lx) * cellW;
            const float y0 = static_cast<float>(item.tileChunk.cy * kTilemapChunkSize + ly) * cellH;
            const float x1 = x0 + cellW;
            const float y1 = y0 + cellH;
            // 左上, 右上, 右下, 左下
            const float px[4] = { x0, x1, x1, x0 };
            const float py[4] = { y1, y1, y0, y0 };
            Sprite2DVertex corner[4];
            for (int i = 0; i < 4; ++i) {
                corner[i].pos[0] = px[i];
                corner[i].pos[1] = py[i];
                corner[i].pos[2] = 0.0f;
                corner[i].uv[0] = uv[i][0];
                corner[i].uv[1] = uv[i][1];
                for (int c = 0; c < 4; ++c) {
                    corner[i].color[c] = white[c];
                }
            }
            const Sprite2DVertex quad[6] = { corner[0], corner[1], corner[2], corner[0], corner[2], corner[3] };
            if (out.runs.empty() || out.runs.back().texture != r.texture || out.runs.back().pointFilter != r.pointFilter) {
                TileRun run;
                run.texture = r.texture;
                run.pointFilter = r.pointFilter;
                run.start = static_cast<uint32_t>(out.vertices.size());
                out.runs.push_back(run);
            }
            out.vertices.insert(out.vertices.end(), quad, quad + 6);
            out.runs.back().count += 6;
        }
    }
}

void Sprite2DRenderer::EmitTileChunk(const Item& item, World& world, RenderResources& resources)
{
    TileCacheKey key;
    key.entityIndex = item.tileEntity.index;
    key.entityGeneration = item.tileEntity.generation;
    key.layer = item.tileLayer;
    key.chunk = item.tileChunk;
    BakedChunk& chunk = tileCache_[key];

    TilemapLibrary* maps = tilemap::Library();
    TilePaletteLibrary* palettes = tilepalette::Library();
    const uint64_t mapEpoch = (maps != nullptr) ? maps->Epoch() : 0;
    const uint64_t paletteEpoch = (palettes != nullptr) ? palettes->Epoch() : 0;
    const uint64_t dirtyEpoch = TilemapSystem::DirtyEpoch();
    SpriteLibrary* sprites = spritelib::Library();
    const uint64_t spriteEpoch = (sprites != nullptr) ? sprites->Epoch() : 0;
    if (!chunk.complete || chunk.mapEpoch != mapEpoch || chunk.paletteEpoch != paletteEpoch
        || chunk.dirtyEpoch != dirtyEpoch || chunk.spriteEpoch != spriteEpoch) {
        BakeTileChunk(item, world, resources, chunk);
        chunk.mapEpoch = mapEpoch;
        chunk.paletteEpoch = paletteEpoch;
        chunk.dirtyEpoch = dirtyEpoch;
        chunk.spriteEpoch = spriteEpoch;
    }
    chunk.lastUsed = tileFrame_;

    for (const TileRun& run : chunk.runs) {
        tileScratch_.assign(chunk.vertices.begin() + run.start, chunk.vertices.begin() + run.start + run.count);
        for (Sprite2DVertex& v : tileScratch_) {
            v.pos[0] += item.tileOrigin.x;
            v.pos[1] += item.tileOrigin.y;
            v.pos[2] += item.tileOrigin.z;
        }
        Push(run.texture, run.pointFilter, sprite2dstencil::kNone, 0.0f, tileScratch_.data(),
             static_cast<uint32_t>(tileScratch_.size()));
    }
}

void Sprite2DRenderer::CollectSprites(World& world, bool hasGroups)
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
            AssignKeys(it, world, hasGroups, e, it.sprite->sortingLayerId, it.sprite->orderInLayer);
            items_.push_back(it);
        }
    });
}

void Sprite2DRenderer::CollectMasks(World& world, bool hasGroups)
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
            AssignKeys(it, world, hasGroups, e, it.mask->sortingLayerId, it.mask->orderInLayer);
            items_.push_back(it);
        }
    });
}

// Tilemap のレイヤー x チャンクを 1 項目ずつ。正射影なら画面外のチャンクは項目にしない
// (キャッシュは掃除されないよう lastUsed だけ更新する)
void Sprite2DRenderer::CollectTilemaps(World& world, const XMFLOAT4X4* view, const XMFLOAT4X4* proj)
{
    const ComponentTypeId req[] = { TilemapComponent::sTypeId, WorldMatrixComponent::sTypeId };
    bool any = false;
    world.ForEachArchetype(req, [&](Archetype& arch) {
        if (arch.Count() > 0) {
            any = true;
        }
    });
    if (!any) {
        return;
    }
    bool hasGroups = false;
    {
        const ComponentTypeId reqGroup[] = { SortingGroup2DComponent::sTypeId };
        world.ForEachArchetype(reqGroup, [&](Archetype& arch) {
            if (arch.Count() > 0) {
                hasGroups = true;
            }
        });
    }
    XMFLOAT3 boundsMin = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 boundsMax = { 0.0f, 0.0f, 0.0f };
    const bool culling = OrthoWorldBounds(view, proj, boundsMin, boundsMax);
    const TilemapSystem* sim = tilemap::System();
    std::vector<ChunkKey> chunkKeys;

    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(TilemapComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* tm = static_cast<const TilemapComponent*>(arch.GetPtr(ti, row));
            const TilemapAsset* map = tilemap::Resolve(tm->tilemap);
            if (map == nullptr) {
                continue;
            }
            const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            const XMFLOAT3 origin = { wm._41, wm._42, wm._43 };
            const float chunkW = static_cast<float>(kTilemapChunkSize) * map->cellSize[0];
            const float chunkH = static_cast<float>(kTilemapChunkSize) * map->cellSize[1];
            for (size_t li = 0; li < map->layers.size(); ++li) {
                const TilemapLayer& layer = map->layers[li];
                // override だけで埋まったチャンクも描くため、実効セルの列挙を使う
                if (sim != nullptr) {
                    sim->EnumerateChunks(world, e, static_cast<int32_t>(li), chunkKeys);
                } else {
                    TilemapSystem::AssetEnumerateChunks(world, e, static_cast<int32_t>(li), chunkKeys);
                }
                for (const ChunkKey& key : chunkKeys) {
                    TileCacheKey ck;
                    ck.entityIndex = e.index;
                    ck.entityGeneration = e.generation;
                    ck.layer = static_cast<int32_t>(li);
                    ck.chunk = key;
                    if (culling) {
                        const float minX = origin.x + static_cast<float>(key.cx) * chunkW;
                        const float minY = origin.y + static_cast<float>(key.cy) * chunkH;
                        const bool outside = minX > boundsMax.x || minX + chunkW < boundsMin.x || minY > boundsMax.y
                                             || minY + chunkH < boundsMin.y || origin.z > boundsMax.z
                                             || origin.z < boundsMin.z;
                        if (outside) {
                            const auto cached = tileCache_.find(ck);
                            if (cached != tileCache_.end()) {
                                cached->second.lastUsed = tileFrame_;
                            }
                            continue;
                        }
                    }
                    Item it;
                    it.isTile = true;
                    it.tileEntity = e;
                    it.tileLayer = static_cast<int32_t>(li);
                    it.tileChunk = key;
                    it.tileOrigin = origin;
                    AssignKeys(it, world, hasGroups, e, layer.sortingLayerId, layer.orderInLayer);
                    items_.push_back(it);
                }
            }
        }
    });
}

const Sprite2DDrawList& Sprite2DRenderer::Collect(World& world, RenderResources& resources, const XMFLOAT4X4* view,
                                                  const XMFLOAT4X4* proj)
{
    list_.Clear();
    items_.clear();
    seenMasks_.clear();
    ++tileFrame_;

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
    CollectSprites(world, hasGroups);
    CollectMasks(world, hasGroups);
    CollectTilemaps(world, view, proj);
    if (items_.empty()) {
        tileCache_.clear();
        return list_;
    }

    // 浮動小数点を使わない整数キー。entity.index まで入れるので同値は無く、並びはアーキタイプの走査順に依らない
    // (同じエンティティにスプライトとマスクがあるときだけ isMask で、同じタイルマップのレイヤー / チャンクは添字で決める)
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        return std::tie(a.outerLayer, a.outerOrder, a.outerIndex, a.innerLayer, a.innerOrder, a.innerIndex, a.isMask,
                        a.tileLayer, a.tileChunk.cy, a.tileChunk.cx)
               < std::tie(b.outerLayer, b.outerOrder, b.outerIndex, b.innerLayer, b.innerOrder, b.innerIndex,
                          b.isMask, b.tileLayer, b.tileChunk.cy, b.tileChunk.cx);
    });
    list_.vertices.reserve(items_.size() * 6);
    for (const Item& it : items_) {
        if (it.isTile) {
            EmitTileChunk(it, world, resources);
        } else if (it.isMask) {
            EmitMask(it, resources);
        } else {
            EmitSprite(it, resources);
        }
    }

    // 今回どのタイルマップにも使われなかったチャンク (エンティティ / レイヤー / チャンクの削除) を捨てる
    for (auto it = tileCache_.begin(); it != tileCache_.end();) {
        it = (it->second.lastUsed == tileFrame_) ? std::next(it) : tileCache_.erase(it);
    }
    return list_;
}

} // namespace mye
