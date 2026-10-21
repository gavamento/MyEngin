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
#include "Engine/Engine/Physics/Collider/Shape2DLibrary.h"
#include "Engine/Engine/Rendering/Sprite2D/Line2DGeometry.h"
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
// 法線を回すワールド行列の軸の長さの下限 (これ未満の軸は回さない)
constexpr float kMinAxisLength = 1.0e-6f;

// Parallax2D の繰り返し: 1 スプライトの複製の上限、間隔の下限、添字の絶対値の上限 (int への変換を安全にする)
constexpr int32_t kParallaxMaxCopies = 64;
constexpr float kMinRepeatPeriod = 1.0e-4f;
constexpr double kMaxRepeatIndex = 1.0e6;

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

void Sprite2DRenderer::NormalTransform(const XMFLOAT4X4& m, bool flipX, bool flipY, float out[4])
{
    const float xLen = std::sqrt(m._11 * m._11 + m._12 * m._12);
    const float yLen = std::sqrt(m._21 * m._21 + m._22 * m._22);
    const float sx = flipX ? -1.0f : 1.0f;
    const float sy = flipY ? -1.0f : 1.0f;
    out[0] = (xLen > kMinAxisLength ? m._11 / xLen : 1.0f) * sx;
    out[1] = (xLen > kMinAxisLength ? m._12 / xLen : 0.0f) * sx;
    out[2] = (yLen > kMinAxisLength ? m._21 / yLen : 0.0f) * sy;
    out[3] = (yLen > kMinAxisLength ? m._22 / yLen : 1.0f) * sy;
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

void Sprite2DRenderer::Push(AssetID texture, bool pointFilter, uint8_t stencil, float alphaCutoff, bool lit,
                            const Sprite2DVertex* v, uint32_t count, AssetID material)
{
    if (count == 0) {
        return;
    }
    // ライト群の切り替え直後は、同じテクスチャでも群の境界でバッチを割る (群ごとにライトバッファが違う)。
    // マテリアルが違えばシェーダが違うのでバッチを割る
    if (forceNewBatch_ || list_.batches.empty() || list_.batches.back().texture != texture
        || list_.batches.back().pointFilter != pointFilter || list_.batches.back().stencil != stencil
        || list_.batches.back().alphaCutoff != alphaCutoff || list_.batches.back().lit != lit
        || list_.batches.back().material != material) {
        forceNewBatch_ = false;
        Sprite2DBatch b;
        b.texture = texture;
        b.material = material;
        b.pointFilter = pointFilter;
        b.stencil = stencil;
        b.alphaCutoff = alphaCutoff;
        b.lit = lit;
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

// scratch_ に組んだ 1 枚目の外接矩形と正射影の表示範囲から、複製するオフセットの添字範囲 [first, last] を求める。
// 繰り返しの無い軸は 0..0。表示範囲が取れない (透視 / 行列なし) ときも 0..0 で、1 枚だけ描く
void Sprite2DRenderer::ParallaxRepeatRange(const Parallax2DComponent& px, int32_t& firstX, int32_t& lastX,
                                           int32_t& firstY, int32_t& lastY, float& periodX, float& periodY)
{
    firstX = lastX = firstY = lastY = 0;
    periodX = periodY = 0.0f;
    if ((!px.repeatX && !px.repeatY) || !hasOrtho_ || scratch_.empty()) {
        return;
    }
    float lo[2] = { scratch_[0].pos[0], scratch_[0].pos[1] };
    float hi[2] = { lo[0], lo[1] };
    for (const Sprite2DVertex& v : scratch_) {
        for (int a = 0; a < 2; ++a) {
            lo[a] = (std::min)(lo[a], v.pos[a]);
            hi[a] = (std::max)(hi[a], v.pos[a]);
        }
    }
    // 両軸を繰り返すときは 1 辺 8 枚 (8 x 8 = 上限 64)
    const int32_t axisCap = (px.repeatX && px.repeatY) ? kParallaxMaxCopies / 8 : kParallaxMaxCopies;
    const auto range = [&](int axis, bool enable, float repeatSize, float viewLo, float viewHi, int32_t& first,
                           int32_t& last, float& period) {
        if (!enable) {
            return;
        }
        const float p = repeatSize > 0.0f ? repeatSize : (hi[axis] - lo[axis]);
        if (!(p > kMinRepeatPeriod)) {
            return;
        }
        const double a = std::ceil(static_cast<double>(viewLo - hi[axis]) / p);
        const double b = std::floor(static_cast<double>(viewHi - lo[axis]) / p);
        first = static_cast<int32_t>((std::max)(-kMaxRepeatIndex, (std::min)(kMaxRepeatIndex, a)));
        last = static_cast<int32_t>((std::max)(-kMaxRepeatIndex, (std::min)(kMaxRepeatIndex, b)));
        period = p;
        if (last - first + 1 > axisCap) {
            // 上限を超えるときは範囲の中央付近だけ描く (端は欠ける)
            const int32_t center = (first + last) / 2;
            first = center - axisCap / 2;
            last = first + axisCap - 1;
            if (!warnedParallaxCap_) {
                warnedParallaxCap_ = true;
                MYE_LOG_WARN("[sprite2d] Parallax2D の繰り返しが上限 (%d 枚) を超えたので端を省きました。"
                             "repeatSize を大きくするか、スプライトを大きくしてください",
                             kParallaxMaxCopies);
            }
        }
    };
    range(0, px.repeatX, px.repeatSize.x, orthoMin_.x, orthoMax_.x, firstX, lastX, periodX);
    range(1, px.repeatY, px.repeatSize.y, orthoMin_.y, orthoMax_.y, firstY, lastY, periodY);
}

void Sprite2DRenderer::EmitSprite(const Item& item, RenderResources& resources)
{
    const Sprite2DComponent& sp = *item.sprite;
    Resolved r;
    if (!Resolve(sp.sprite, resources, true, r)) {
        return;
    }
    // 視差: 描画位置だけをずらす (回転・スケール・当たりは変えない)
    XMFLOAT4X4 shifted = *item.world;
    if (item.parallax != nullptr && hasCamera_) {
        shifted._41 += (cameraXY_.x - shifted._41) * (1.0f - item.parallax->factor.x);
        shifted._42 += (cameraXY_.y - shifted._42) * (1.0f - item.parallax->factor.y);
    }
    const XMFLOAT4X4& m = shifted;

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
    const bool lit = !lightMeta_.empty() && sp.lit;
    const AssetID normalTex = (lit && anyNormalMap_) ? ResolveNormalMap(sp.normalMap, resources) : AssetID{};
    float xform[4] = {};
    if (lit && anyNormalMap_) {
        anyNormalReady_ = anyNormalReady_ || !normalTex.IsNull();
        NormalTransform(m, sp.flipX, sp.flipY, xform);
    }
    // 視差の繰り返し: 1 枚目 (scratch_ のまま) の後ろに、間隔ぶんずらした複製を足す
    int32_t firstX = 0;
    int32_t lastX = 0;
    int32_t firstY = 0;
    int32_t lastY = 0;
    float periodX = 0.0f;
    float periodY = 0.0f;
    if (item.parallax != nullptr) {
        ParallaxRepeatRange(*item.parallax, firstX, lastX, firstY, lastY, periodX, periodY);
    }
    for (int32_t ky = firstY; ky <= lastY; ++ky) {
        for (int32_t kx = firstX; kx <= lastX; ++kx) {
            const Sprite2DVertex* verts = scratch_.data();
            const uint32_t count = static_cast<uint32_t>(scratch_.size());
            if (kx != 0 || ky != 0) {
                repeatScratch_.assign(scratch_.begin(), scratch_.end());
                for (Sprite2DVertex& v : repeatScratch_) {
                    v.pos[0] += periodX * static_cast<float>(kx);
                    v.pos[1] += periodY * static_cast<float>(ky);
                }
                verts = repeatScratch_.data();
            }
            Push(r.texture, r.pointFilter, stencil, 0.0f, lit, verts, count, sp.material);
            if (lit && anyNormalMap_) {
                // 法線 RT へ同じ頂点列を重ねる。法線マップが無いスプライトも平らな法線で描き、奥の法線を隠す
                PushNormal(r.texture, normalTex, r.pointFilter, xform, verts, count);
            }
        }
    }
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
    Push(r.texture, r.pointFilter, sprite2dstencil::kWriteMask, mk.alphaCutoff, false, scratch_.data(),
         static_cast<uint32_t>(scratch_.size()));
}

void Sprite2DRenderer::EmitLine(const Item& item, RenderResources& resources)
{
    const Line2DComponent& ln = *item.line;
    const int n = std::clamp(ln.pointCount, 0, kLine2DMaxPoints);
    const bool stroke = ln.width > 0.0f && n >= 2;
    const bool fill = ln.fill && n >= 3;
    if (!stroke && !fill) {
        return; // 描くものが無いときはテクスチャの読み込みも頼まない
    }
    // texture が空 = 白。指定があって未ロードなら読み込みを頼んで、このフレームは描かない (パスが引けないものはマゼンタ)
    TextureLibrary& textures = resources.textures;
    AssetID texture = textures.White();
    if (!ln.texture.IsNull()) {
        if (textures.Get(ln.texture) != nullptr) {
            texture = ln.texture;
        } else {
            const std::wstring path = assetguid::ResolvePath(ln.texture.value);
            if (path.empty()) {
                texture = EnsureMissingTexture(resources);
            } else {
                textures.RequestLoadFileAsync(path);
                return;
            }
        }
    }

    const XMFLOAT4X4& m = *item.world;
    scratch_.clear();
    const auto addVertex = [&](float x, float y, float u, float v) {
        Sprite2DVertex vert;
        vert.pos[0] = x * m._11 + y * m._21 + m._41;
        vert.pos[1] = x * m._12 + y * m._22 + m._42;
        vert.pos[2] = x * m._13 + y * m._23 + m._43;
        vert.uv[0] = u;
        vert.uv[1] = v;
        vert.color[0] = ln.color.x;
        vert.color[1] = ln.color.y;
        vert.color[2] = ln.color.z;
        vert.color[3] = ln.color.w;
        scratch_.push_back(vert);
    };
    // 塗りを先に積む (帯が上に重なる)。UV = ローカル座標 / 1 単位 (テクスチャの上 = +y なので v は符号を反転)
    if (fill) {
        line2d::Triangulate(ln.points, n, lineFill_);
        for (const uint32_t idx : lineFill_) {
            const XMFLOAT2& p = ln.points[idx];
            addVertex(p.x, p.y, p.x, -p.y);
        }
    }
    if (stroke) {
        line2d::BuildStroke(ln.points, n, ln.width, ln.loop, lineStroke_);
        for (const line2d::StrokeVertex& sv : lineStroke_) {
            addVertex(sv.x, sv.y, sv.u, sv.v);
        }
    }
    if (scratch_.empty()) {
        return;
    }

    // Line2D には lit の欄が無い。ライトがあるシーンでは Tilemap と同じく常に lit (法線 RT には平らな法線で載る)
    const bool lit = !lightMeta_.empty();
    const uint32_t count = static_cast<uint32_t>(scratch_.size());
    Push(texture, false, sprite2dstencil::kNone, 0.0f, lit, scratch_.data(), count);
    if (lit && anyNormalMap_) {
        const float flat[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        PushNormal(texture, AssetID{}, false, flat, scratch_.data(), count);
    }
}

// SpriteShape2D: 折れ線をワールド XY へ移してから塗りと縁を作る (向きの正規化・格子の切りをワールドで行うため)。
// 頂点の z はエンティティ原点の z で一定 (xy 平面の形状が前提)。profile / スプライトが無ければ何も描かない
void Sprite2DRenderer::EmitShape(const Item& item, RenderResources& resources)
{
    const SpriteShape2DComponent& sh = *item.shape;
    const int n = std::clamp(sh.pointCount, 0, kSpriteShape2DMaxPoints);
    const SpriteShapeProfile* profile = spriteshape::Resolve(sh.profile);
    if (profile == nullptr || n < 2) {
        return;
    }
    spriteshape::EvaluateSpline(sh.points, n, sh.closed, sh.smooth, shapeLocal_);
    const XMFLOAT4X4& m = *item.world;
    shapeWorld_.resize(shapeLocal_.size());
    for (size_t i = 0; i < shapeLocal_.size(); ++i) {
        const XMFLOAT2& p = shapeLocal_[i];
        shapeWorld_[i] = { p.x * m._11 + p.y * m._21 + m._41, p.x * m._12 + p.y * m._22 + m._42 };
    }
    const float z = m._43;
    const bool closed = spriteshape::IsClosedOutline(n, sh.closed);
    const bool lit = !lightMeta_.empty();
    const auto addVertex = [&](float x, float y, float u, float v) {
        Sprite2DVertex vert;
        vert.pos[0] = x;
        vert.pos[1] = y;
        vert.pos[2] = z;
        vert.uv[0] = u;
        vert.uv[1] = v;
        vert.color[0] = 1.0f;
        vert.color[1] = 1.0f;
        vert.color[2] = 1.0f;
        vert.color[3] = 1.0f;
        scratch_.push_back(vert);
    };
    // scratch_ を 1 バッチとして積む (ライトのあるシーンでは法線 RT へ平らな法線でも)
    const auto flush = [&](const Resolved& r) {
        if (scratch_.empty()) {
            return;
        }
        const uint32_t count = static_cast<uint32_t>(scratch_.size());
        Push(r.texture, r.pointFilter, sprite2dstencil::kNone, 0.0f, lit, scratch_.data(), count);
        if (lit && anyNormalMap_) {
            const float flat[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
            PushNormal(r.texture, AssetID{}, r.pointFilter, flat, scratch_.data(), count);
        }
    };
    bool capped = false;

    // 塗り (縁の下)。スプライト 1 枚ぶんの寸法の格子で繰り返す
    if (closed && profile->fill != 0) {
        Resolved r;
        if (Resolve(AssetID{ profile->fill }, resources, true, r)) {
            capped = spriteshape::BuildFill(shapeWorld_, r.rect[2] / r.ppu, r.rect[3] / r.ppu, r.rect[0] / r.texW,
                                            r.rect[1] / r.texH, (r.rect[0] + r.rect[2]) / r.texW,
                                            (r.rect[1] + r.rect[3]) / r.texH, kMaxSpriteVertices, shapeFill_)
                     || capped;
            scratch_.clear();
            for (const spriteshape::FillVertex& fv : shapeFill_) {
                addVertex(fv.x, fv.y, fv.u, fv.v);
            }
            flush(r);
        }
    }

    // 縁。profile.edges の添字順に、同じ縁のクアッドを 1 バッチへまとめる
    if (!profile->edges.empty()) {
        std::vector<Resolved> resolved(profile->edges.size());
        std::vector<spriteshape::EdgeSpriteInfo> infos(profile->edges.size());
        for (size_t i = 0; i < profile->edges.size(); ++i) {
            const AssetID id{ profile->edges[i].sprite };
            if (id.IsNull() || !Resolve(id, resources, true, resolved[i])) {
                continue;
            }
            const Resolved& r = resolved[i];
            infos[i].width = r.rect[2] / r.ppu;
            infos[i].u0 = r.rect[0] / r.texW;
            infos[i].v0 = r.rect[1] / r.texH;
            infos[i].u1 = (r.rect[0] + r.rect[2]) / r.texW;
            infos[i].v1 = (r.rect[1] + r.rect[3]) / r.texH;
        }
        capped = spriteshape::BuildEdgeQuads(shapeWorld_, sh.closed && closed, profile->edges, infos, profile->height,
                                             kMaxSpriteVertices / 6, shapeQuads_)
                 || capped;
        static constexpr int kTriOrder[6] = { 0, 1, 2, 0, 2, 3 };
        for (size_t e = 0; e < profile->edges.size(); ++e) {
            scratch_.clear();
            for (const spriteshape::EdgeQuad& q : shapeQuads_) {
                if (q.edge != static_cast<int>(e)) {
                    continue;
                }
                for (const int k : kTriOrder) {
                    addVertex(q.x[k], q.y[k], q.u[k], q.v[k]);
                }
            }
            flush(resolved[e]);
        }
    }
    if (capped && !warnedShapeCap_) {
        warnedShapeCap_ = true;
        MYE_LOG_WARN("[spriteshape] too many fill / edge pieces: the rest of the outline is not drawn");
    }
}

void Sprite2DRenderer::AssignKeys(Item& it, World& world, bool hasGroups, EntityID e, int32_t layerId,
                                  int32_t order) const
{
    const SortingLayers& layers = SortingLayers::Get();
    it.layerId = layerId;
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
        it.layerId = g->sortingLayerId;
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

TileNeighbors Sprite2DRenderer::TileNeighborsAt(const TileChunkAround& around, int32_t lx, int32_t ly)
{
    static constexpr int32_t kDx[kTileNeighborCount] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    static constexpr int32_t kDy[kTileNeighborCount] = { 1, 1, 1, 0, 0, -1, -1, -1 };
    TileNeighbors out{};
    for (size_t i = 0; i < kTileNeighborCount; ++i) {
        const int32_t x = lx + kDx[i];
        const int32_t y = ly + kDy[i];
        const int32_t chunkX = (x < 0) ? 0 : (x >= kTilemapChunkSize ? 2 : 1);
        const int32_t chunkY = (y < 0) ? 0 : (y >= kTilemapChunkSize ? 2 : 1);
        const TileChunkCells& cells = around[static_cast<size_t>(chunkY * 3 + chunkX)];
        out[i] = tilecell::TileId(cells[static_cast<size_t>(TilemapFloorMod(y, kTilemapChunkSize) * kTilemapChunkSize
                                                            + TilemapFloorMod(x, kTilemapChunkSize))]);
    }
    return out;
}

// チャンクを焼く: 実効セルを読み、tileId → パレット → スプライトを引いて、セルごとに 1 クアッドを原点基準で並べる。
// 絵は anim > rule (同じレイヤーの隣接 8 マス。チャンク境界は隣のチャンクを読む) > sprite の順で選ぶ。
// 非同期ロード中のスプライトがあれば complete = false にして次フレームも焼き直す
void Sprite2DRenderer::BakeTileChunk(const Item& item, World& world, RenderResources& resources, BakedChunk& out)
{
    out.vertices.clear();
    out.runs.clear();
    out.anims.clear();
    out.complete = true;
    const TilemapAsset* map = TilemapSystem::AssetOf(world, item.tileEntity);
    const TilePalette* palette = (map != nullptr) ? tilepalette::Resolve(map->palette) : nullptr;
    // sim の書き換え (override) を重ねた実効セルで焼く。sim の持ち主が未接続ならアセットの値だけ
    const TilemapSystem* sim = tilemap::System();
    const auto readChunk = [&](ChunkKey key, std::array<uint32_t, kTilemapChunkCells>& cells) {
        return (sim != nullptr) ? sim->GetChunkCells(world, item.tileEntity, item.tileLayer, key, cells)
                                : TilemapSystem::AssetChunkCells(world, item.tileEntity, item.tileLayer, key, cells);
    };
    std::array<uint32_t, kTilemapChunkCells> cells;
    if (map == nullptr || palette == nullptr || !readChunk(item.tileChunk, cells)) {
        return;
    }
    // 隣接を見るのはルールタイルがあるパレットだけ。3x3 チャンクの実効セル (添字 = (dy + 1) * 3 + (dx + 1)、中央は cells)
    const bool needNeighbors = std::any_of(palette->tiles.begin(), palette->tiles.end(),
                                           [](const TileDef& t) { return t.hasRule; });
    TileChunkAround around{};
    if (needNeighbors) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                auto& dst = around[static_cast<size_t>((dy + 1) * 3 + (dx + 1))];
                if (dx == 0 && dy == 0) {
                    dst = cells;
                } else if (!readChunk(ChunkKey{ item.tileChunk.cy + dy, item.tileChunk.cx + dx }, dst)) {
                    dst.fill(0);
                }
            }
        }
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
            if (def == nullptr) {
                continue; // 空 / パレットに無い id は描かない
            }
            const TileNeighbors neighbors = def->hasRule ? TileNeighborsAt(around, lx, ly) : TileNeighbors{};
            if (def->hasAnim && !def->animFrames.empty()) {
                const auto stamp = std::lower_bound(out.anims.begin(), out.anims.end(), def->id,
                                                    [](const AnimStamp& a, uint32_t id) { return a.tileId < id; });
                if (stamp == out.anims.end() || stamp->tileId != def->id) {
                    out.anims.insert(stamp, AnimStamp{ def->id, def->ticksPerFrame,
                                                       static_cast<uint32_t>(def->animFrames.size()),
                                                       TileAnimFrameIndex(def->ticksPerFrame, def->animFrames.size(),
                                                                          simTick_) });
                }
            }
            const AssetID sprite = ResolveTileSprite(*palette, *def, neighbors, simTick_);
            if (sprite.IsNull()) {
                continue; // スプライト無しは描かない
            }
            auto found = memo.find(sprite.value);
            if (found == memo.end()) {
                Resolved r;
                if (!Resolve(sprite, resources, true, r)) {
                    out.complete = false;
                    continue;
                }
                found = memo.emplace(sprite.value, r).first;
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
    // アニメーションタイルは、含むコマのどれかが今の tick で変わったときだけ焼き直す
    const bool animChanged = std::any_of(chunk.anims.begin(), chunk.anims.end(), [this](const AnimStamp& a) {
        return TileAnimFrameIndex(a.ticksPerFrame, a.frameCount, simTick_) != a.frame;
    });
    if (!chunk.complete || chunk.mapEpoch != mapEpoch || chunk.paletteEpoch != paletteEpoch
        || chunk.dirtyEpoch != dirtyEpoch || chunk.spriteEpoch != spriteEpoch || animChanged) {
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
        // タイルマップには lit の欄が無い。ライトがあるシーンでは常に lit
        Push(run.texture, run.pointFilter, sprite2dstencil::kNone, 0.0f, !lightMeta_.empty(), tileScratch_.data(),
             static_cast<uint32_t>(tileScratch_.size()));
        if (!lightMeta_.empty() && anyNormalMap_) {
            const float flat[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
            PushNormal(run.texture, AssetID{}, run.pointFilter, flat, tileScratch_.data(),
                       static_cast<uint32_t>(tileScratch_.size()));
        }
    }
}

void Sprite2DRenderer::CollectSprites(World& world, bool hasGroups)
{
    // Parallax2D がシーンに無ければ、スプライトごとの引き当てをしない
    bool anyParallax = false;
    {
        const ComponentTypeId reqParallax[] = { Parallax2DComponent::sTypeId };
        world.ForEachArchetype(reqParallax, [&](Archetype& arch) { anyParallax = anyParallax || arch.Count() > 0; });
    }
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
            if (it.sprite->lit && !it.sprite->normalMap.IsNull()) {
                anyNormalMap_ = true;
            }
            if (anyParallax) {
                it.parallax = world.GetComponent<Parallax2DComponent>(e);
                hasParallaxItem_ = hasParallaxItem_ || it.parallax != nullptr;
            }
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

void Sprite2DRenderer::CollectLines(World& world, bool hasGroups)
{
    const ComponentTypeId req[] = { Line2DComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int li = arch.FindTypeIndex(Line2DComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            Item it;
            it.isLine = true;
            it.line = static_cast<const Line2DComponent*>(arch.GetPtr(li, row));
            it.world = &static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            AssignKeys(it, world, hasGroups, e, it.line->sortingLayerId, it.line->orderInLayer);
            items_.push_back(it);
        }
    });
}

void Sprite2DRenderer::CollectShapes(World& world, bool hasGroups)
{
    const ComponentTypeId req[] = { SpriteShape2DComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SpriteShape2DComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            Item it;
            it.isShape = true;
            it.shape = static_cast<const SpriteShape2DComponent*>(arch.GetPtr(si, row));
            it.world = &static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            AssignKeys(it, world, hasGroups, e, it.shape->sortingLayerId, it.shape->orderInLayer);
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

// ---- 2D ライト ----

namespace {
// 度 → ラジアン
constexpr float kDegToRad = 0.017453292519943295f;
// 扇形の内外の cos 差の下限 (内外の角度が等しいときの 0 割りを避け、境界をほぼ段差にする)
constexpr float kMinCosRange = 1.0e-4f;
// 外角がこの値以上なら全周 (点光源)
constexpr float kFullAngleDeg = 360.0f;
// Freeform の外縁を広げるときの miter 係数の下限 (鋭角で無限に伸びないようにする)
constexpr float kMinMiterDot = 0.25f;
// 面積がこれ以下の多角形は Freeform として描かない
constexpr float kMinPolygonArea = 1.0e-8f;
// 法線の N·L が 0 にならないよう、ライトの高さ (normalMapDistance) に持たせる下限
constexpr float kMinNormalMapDistance = 1.0e-3f;
// Collider が球のシャドウキャスターを近似する多角形の角数
constexpr int kShadowCircleSegments = 16;

// ライトのエンティティが向く方向 (ワールド行列のローカル X 軸の XY 成分)。ゼロなら +X
void LightDirection(const XMFLOAT4X4& m, float out[2])
{
    const float len = std::sqrt(m._11 * m._11 + m._12 * m._12);
    if (len < 1.0e-6f) {
        out[0] = 1.0f;
        out[1] = 0.0f;
        return;
    }
    out[0] = m._11 / len;
    out[1] = m._12 / len;
}

void AddLightQuad(std::vector<Light2DVertex>& out, const float x[4], const float y[4], const float u[4],
                  const float v[4])
{
    // 左上, 右上, 右下, 左下
    const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int k : order) {
        out.push_back({ { x[k], y[k] }, { u[k], v[k] }, 1.0f });
    }
}
} // namespace

void Sprite2DRenderer::BuildSpotLight(const Light2DComponent& lc, const XMFLOAT4X4& m, Light2DInstance& out)
{
    out.shape = light2dshape::kSpot;
    out.center[0] = m._41;
    out.center[1] = m._42;
    LightDirection(m, out.dir);
    out.outerRadius = (std::max)(lc.outerRadius, 0.0f);
    out.innerRadius = (std::min)((std::max)(lc.innerRadius, 0.0f), out.outerRadius);
    out.falloff = (std::max)(lc.falloff, 0.0f);
    const float outerAngle = (std::max)(lc.outerAngleDeg, 0.0f);
    const float innerAngle = (std::min)((std::max)(lc.innerAngleDeg, 0.0f), outerAngle);
    out.useAngle = (outerAngle < kFullAngleDeg) ? 1.0f : 0.0f;
    const float cosInner = std::cos(innerAngle * 0.5f * kDegToRad);
    out.cosOuter = std::cos(outerAngle * 0.5f * kDegToRad);
    out.invCosRange = 1.0f / (std::max)(cosInner - out.cosOuter, kMinCosRange);
    // 外半径の外接クアッド (PS が距離と角度で減衰させる)
    const float r = out.outerRadius;
    const float x[4] = { m._41 - r, m._41 + r, m._41 + r, m._41 - r };
    const float y[4] = { m._42 + r, m._42 + r, m._42 - r, m._42 - r };
    const float u[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    const float v[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    out.vertexStart = static_cast<uint32_t>(list_.lightVertices.size());
    AddLightQuad(list_.lightVertices, x, y, u, v);
    out.vertexCount = 6;
}

bool Sprite2DRenderer::BuildFreeformLight(const Light2DComponent& lc, const XMFLOAT4X4& m, Light2DInstance& out)
{
    const int count = (std::min)((std::max)(lc.pointCount, 0), kLight2DMaxPoints);
    if (count < 3) {
        return false;
    }
    std::vector<XMFLOAT2> pts(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float lx = lc.points[i].x;
        const float ly = lc.points[i].y;
        pts[static_cast<size_t>(i)] = { lx * m._11 + ly * m._21 + m._41, lx * m._12 + ly * m._22 + m._42 };
    }
    float area2 = 0.0f; // 符号付き面積の 2 倍 (正 = 反時計回り)
    for (int i = 0; i < count; ++i) {
        const XMFLOAT2& a = pts[static_cast<size_t>(i)];
        const XMFLOAT2& b = pts[static_cast<size_t>((i + 1) % count)];
        area2 += a.x * b.y - b.x * a.y;
    }
    if (std::fabs(area2) < kMinPolygonArea) {
        return false;
    }
    std::vector<uint32_t> tris;
    if (!TriangulatePolygon2D(pts, tris)) {
        return false;
    }
    out.shape = light2dshape::kFreeform;
    out.vertexStart = static_cast<uint32_t>(list_.lightVertices.size());
    for (uint32_t idx : tris) {
        const XMFLOAT2& p = pts[idx];
        list_.lightVertices.push_back({ { p.x, p.y }, { 0.0f, 0.0f }, 1.0f });
    }
    // 外縁: 各辺を外向き法線へ falloffWidth だけ押し出し、edge 1 → 0 のグラデーションにする
    const float width = (std::max)(lc.falloffWidth, 0.0f);
    if (width > 0.0f) {
        const float sign = (area2 > 0.0f) ? 1.0f : -1.0f; // 反時計回りなら辺の右側が外
        std::vector<XMFLOAT2> offsets(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const XMFLOAT2& prev = pts[static_cast<size_t>((i + count - 1) % count)];
            const XMFLOAT2& cur = pts[static_cast<size_t>(i)];
            const XMFLOAT2& next = pts[static_cast<size_t>((i + 1) % count)];
            const auto edgeNormal = [sign](const XMFLOAT2& a, const XMFLOAT2& b) {
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                const float len = (std::max)(std::sqrt(dx * dx + dy * dy), 1.0e-12f);
                return XMFLOAT2{ sign * dy / len, -sign * dx / len };
            };
            const XMFLOAT2 n0 = edgeNormal(prev, cur);
            const XMFLOAT2 n1 = edgeNormal(cur, next);
            float ax = n0.x + n1.x;
            float ay = n0.y + n1.y;
            const float alen = (std::max)(std::sqrt(ax * ax + ay * ay), 1.0e-12f);
            ax /= alen;
            ay /= alen;
            const float miter = (std::max)(ax * n0.x + ay * n0.y, kMinMiterDot);
            offsets[static_cast<size_t>(i)] = { ax * width / miter, ay * width / miter };
        }
        for (int i = 0; i < count; ++i) {
            const int j = (i + 1) % count;
            const XMFLOAT2& a = pts[static_cast<size_t>(i)];
            const XMFLOAT2& b = pts[static_cast<size_t>(j)];
            const Light2DVertex ia = { { a.x, a.y }, { 0.0f, 0.0f }, 1.0f };
            const Light2DVertex ib = { { b.x, b.y }, { 0.0f, 0.0f }, 1.0f };
            const Light2DVertex oa = { { a.x + offsets[static_cast<size_t>(i)].x, a.y + offsets[static_cast<size_t>(i)].y },
                                       { 0.0f, 0.0f }, 0.0f };
            const Light2DVertex ob = { { b.x + offsets[static_cast<size_t>(j)].x, b.y + offsets[static_cast<size_t>(j)].y },
                                       { 0.0f, 0.0f }, 0.0f };
            const Light2DVertex quad[6] = { ia, ib, ob, ia, ob, oa };
            list_.lightVertices.insert(list_.lightVertices.end(), quad, quad + 6);
        }
    }
    out.vertexCount = static_cast<uint32_t>(list_.lightVertices.size()) - out.vertexStart;
    return true;
}

bool Sprite2DRenderer::BuildSpriteLight(const Light2DComponent& lc, const XMFLOAT4X4& m, RenderResources& resources,
                                        Light2DInstance& out)
{
    Resolved r;
    if (!Resolve(lc.sprite, resources, false, r)) {
        return false; // スプライトが無い / 未ロードのライトは何も足さない
    }
    out.shape = light2dshape::kSprite;
    out.texture = r.texture;
    const float lx[4] = { r.local[0], r.local[2], r.local[2], r.local[0] };
    const float ly[4] = { r.local[3], r.local[3], r.local[1], r.local[1] };
    float x[4];
    float y[4];
    for (int i = 0; i < 4; ++i) {
        x[i] = lx[i] * m._11 + ly[i] * m._21 + m._41;
        y[i] = lx[i] * m._12 + ly[i] * m._22 + m._42;
    }
    const float u0 = r.rect[0] / r.texW;
    const float v0 = r.rect[1] / r.texH;
    const float u1 = (r.rect[0] + r.rect[2]) / r.texW;
    const float v1 = (r.rect[1] + r.rect[3]) / r.texH;
    const float u[4] = { u0, u1, u1, u0 };
    const float v[4] = { v0, v0, v1, v1 };
    out.vertexStart = static_cast<uint32_t>(list_.lightVertices.size());
    AddLightQuad(list_.lightVertices, x, y, u, v);
    out.vertexCount = 6;
    return true;
}

void Sprite2DRenderer::CollectLights(World& world, RenderResources& resources)
{
    struct Entry {
        EntityID entity;
        LightMeta meta;
        Light2DInstance inst;
    };
    std::vector<Entry> found;
    const ComponentTypeId req[] = { Light2DComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int li = arch.FindTypeIndex(Light2DComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* lc = static_cast<const Light2DComponent*>(arch.GetPtr(li, row));
            const XMFLOAT4X4& m = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            Entry en;
            en.entity = e;
            const float k = (std::max)(lc->intensity, 0.0f);
            en.meta.targetMask = lc->targetLayerMask;
            en.meta.color[0] = lc->color.x * k;
            en.meta.color[1] = lc->color.y * k;
            en.meta.color[2] = lc->color.z * k;
            en.meta.global = (lc->type == light2dtype::kGlobal);
            en.inst.z = m._43;
            en.inst.center[0] = m._41;
            en.inst.center[1] = m._42;
            en.inst.shadows = lc->shadows && !en.meta.global; // Global には影が無い
            en.inst.shadowStrength = (std::min)((std::max)(lc->shadowStrength, 0.0f), 1.0f);
            en.inst.normalMapDistance = (std::max)(lc->normalMapDistance, kMinNormalMapDistance);
            std::copy(en.meta.color, en.meta.color + 3, en.inst.color);
            // 形状を作れないライト (半径 0、頂点不足、スプライト未解決) は数えない = ライトが 0 個のシーンと同じ扱い
            bool ok = true;
            if (lc->type == light2dtype::kSpot) {
                ok = lc->outerRadius > 0.0f;
                if (ok) {
                    BuildSpotLight(*lc, m, en.inst);
                }
            } else if (lc->type == light2dtype::kFreeform) {
                ok = BuildFreeformLight(*lc, m, en.inst);
            } else if (lc->type == light2dtype::kSprite) {
                ok = BuildSpriteLight(*lc, m, resources, en.inst);
            } else if (!en.meta.global) {
                ok = false; // 未知の種類
            }
            if (ok) {
                found.push_back(en);
            }
        }
    });
    // 加算なので順序は結果に効かないが、アーキタイプの走査順に依らないよう entity.index で並べる
    std::stable_sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
        return std::tie(a.entity.index, a.entity.generation) < std::tie(b.entity.index, b.entity.generation);
    });
    // 並べ替えで頂点区間の順は変わるが、各ライトは自分の区間を指しているので問題ない
    for (Entry& en : found) {
        if (!en.meta.global) {
            en.meta.instance = static_cast<uint32_t>(list_.lights.size());
            list_.lights.push_back(en.inst);
            anyShadowLight_ = anyShadowLight_ || en.inst.shadows;
        }
        lightMeta_.push_back(en.meta);
    }
}

bool Sprite2DRenderer::BuildShadowPolygon(World& world, EntityID entity, const ShadowCaster2DComponent& caster,
                                          const XMFLOAT4X4& m, std::vector<XMFLOAT2>& out)
{
    out.clear();
    // ローカル座標 (x, y, z) → ワールド XY (行ベクトル規約)
    const auto toWorld = [&m](float x, float y, float z) {
        return XMFLOAT2{ x * m._11 + y * m._21 + z * m._31 + m._41, x * m._12 + y * m._22 + z * m._32 + m._42 };
    };
    if (caster.source == shadowcaster2dsource::kPoints) {
        const int count = (std::min)((std::max)(caster.pointCount, 0), kShadowCaster2DMaxPoints);
        for (int i = 0; i < count; ++i) {
            out.push_back(toWorld(caster.points[i].x, caster.points[i].y, 0.0f));
        }
    } else if (const auto* col = world.GetComponent<ColliderComponent>(entity)) {
        if (col->shape == collidershape::kBox) {
            // 箱の XY 断面 (collider の回転と中心を含む)
            const XMVECTOR q = XMLoadFloat4(&col->rotation);
            const float hx = col->halfExtents.x;
            const float hy = col->halfExtents.y;
            const float cornerX[4] = { -hx, hx, hx, -hx };
            const float cornerY[4] = { -hy, -hy, hy, hy };
            for (int i = 0; i < 4; ++i) {
                XMFLOAT3 p;
                XMStoreFloat3(&p, XMVector3Rotate(XMVectorSet(cornerX[i], cornerY[i], 0.0f, 0.0f), q));
                out.push_back(toWorld(p.x + col->center.x, p.y + col->center.y, p.z + col->center.z));
            }
        } else if (col->shape == collidershape::kSphere) {
            for (int i = 0; i < kShadowCircleSegments; ++i) {
                const float a = 2.0f * XM_PI * static_cast<float>(i) / static_cast<float>(kShadowCircleSegments);
                out.push_back(toWorld(col->center.x + col->radius * std::cos(a), col->center.y + col->radius * std::sin(a),
                                      col->center.z));
            }
        } else if (col->shape == collidershape::kPolygon2D) {
            if (const Shape2D* shape = shape2d::Resolve(col->meshAsset)) {
                for (const XMFLOAT2& p : shape->points) {
                    out.push_back(toWorld(p.x, p.y, 0.0f));
                }
            }
        }
    }
    if (out.size() < 3) {
        out.clear();
        return false;
    }
    return true;
}

void Sprite2DRenderer::CollectShadowCasters(World& world)
{
    struct Found {
        EntityID entity;
        bool selfShadow = false;
        std::vector<XMFLOAT2> points;
    };
    std::vector<Found> found;
    const ComponentTypeId req[] = { ShadowCaster2DComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(ShadowCaster2DComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* sc = static_cast<const ShadowCaster2DComponent*>(arch.GetPtr(ci, row));
            if (!sc->castsShadows) {
                continue;
            }
            Found f;
            f.entity = e;
            f.selfShadow = sc->selfShadow;
            if (BuildShadowPolygon(world, e, *sc, static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value,
                                   f.points)) {
                found.push_back(std::move(f));
            }
        }
    });
    // 影は和集合 (描画順に依らない) だが、アーキタイプの走査順に依らないよう entity 順に並べる
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return std::tie(a.entity.index, a.entity.generation) < std::tie(b.entity.index, b.entity.generation);
    });
    for (const Found& f : found) {
        Shadow2DCaster c;
        c.pointStart = static_cast<uint32_t>(list_.shadowPoints.size());
        c.pointCount = static_cast<uint32_t>(f.points.size());
        for (const XMFLOAT2& p : f.points) {
            list_.shadowPoints.push_back({ p.x, p.y });
        }
        c.fillStart = static_cast<uint32_t>(list_.shadowFill.size());
        if (!f.selfShadow) {
            std::vector<uint32_t> tris;
            if (TriangulatePolygon2D(f.points, tris)) {
                for (uint32_t idx : tris) {
                    list_.shadowFill.push_back({ f.points[idx].x, f.points[idx].y });
                }
            }
        }
        c.fillCount = static_cast<uint32_t>(list_.shadowFill.size()) - c.fillStart;
        list_.shadowCasters.push_back(c);
    }
}

AssetID Sprite2DRenderer::ResolveNormalMap(AssetID normalMap, RenderResources& resources)
{
    if (normalMap.IsNull()) {
        return AssetID{};
    }
    TextureLibrary& textures = resources.textures;
    if (textures.Get(normalMap) == nullptr) {
        const std::wstring path = assetguid::ResolvePath(normalMap.value);
        if (!path.empty()) {
            textures.RequestLoadFileAsync(path);
        }
        return AssetID{};
    }
    // 白のプレースホルダ (1x1) の間は法線として使えない
    return textures.IsPending(normalMap) ? AssetID{} : normalMap;
}

void Sprite2DRenderer::PushNormal(AssetID texture, AssetID normalMap, bool pointFilter, const float xform[4],
                                  const Sprite2DVertex* v, uint32_t count)
{
    if (count == 0) {
        return;
    }
    if (list_.normalBatches.empty() || list_.normalBatches.back().texture != texture
        || list_.normalBatches.back().normalMap != normalMap || list_.normalBatches.back().pointFilter != pointFilter) {
        Sprite2DNormalBatch b;
        b.texture = texture;
        b.normalMap = normalMap;
        b.pointFilter = pointFilter;
        b.start = static_cast<uint32_t>(list_.normalVertices.size());
        list_.normalBatches.push_back(b);
    }
    // 頂点色は法線の変換に使う (法線 RT のアルファはテクスチャから取る)
    const size_t first = list_.normalVertices.size();
    list_.normalVertices.insert(list_.normalVertices.end(), v, v + count);
    for (size_t i = first; i < list_.normalVertices.size(); ++i) {
        std::copy(xform, xform + 4, list_.normalVertices[i].color);
    }
    list_.normalBatches.back().count += count;
}

uint32_t Sprite2DRenderer::LightSetOf(int32_t layerId)
{
    // 表に無い id は描画順と同じく Default として扱う
    const int32_t layer = (layerId < 0 || layerId > 31 || SortingLayers::Get().IndexOfId(layerId) < 0)
                              ? SortingLayers::kDefaultId
                              : layerId;
    if (layerLightSet_[layer] >= 0) {
        return static_cast<uint32_t>(layerLightSet_[layer]);
    }
    std::vector<uint32_t> set;
    for (uint32_t i = 0; i < lightMeta_.size(); ++i) {
        if (((lightMeta_[i].targetMask >> layer) & 1u) != 0u) {
            set.push_back(i);
        }
    }
    // 同じ集合は同じ id (層ごとに別の id になると、集合が同じでも群を割ってしまう)
    for (size_t s = 0; s < lightSets_.size(); ++s) {
        if (lightSets_[s] == set) {
            layerLightSet_[layer] = static_cast<int32_t>(s);
            return static_cast<uint32_t>(s);
        }
    }
    lightSets_.push_back(std::move(set));
    layerLightSet_[layer] = static_cast<int32_t>(lightSets_.size() - 1);
    return static_cast<uint32_t>(lightSets_.size() - 1);
}

void Sprite2DRenderer::SwitchLightGroup(int32_t layerId, bool lit)
{
    if (list_.lightGroups.empty()) {
        Sprite2DLightGroup g;
        g.firstBatch = static_cast<uint32_t>(list_.batches.size());
        list_.lightGroups.push_back(g);
        groupSetIds_.push_back(0);
        forceNewBatch_ = true;
    }
    if (!lit) {
        return; // lit でない項目 (マスク / lit=false) は今の群にそのまま入る
    }
    const uint32_t setId = LightSetOf(layerId);
    const auto fill = [&](Sprite2DLightGroup& g) {
        g.anyLit = true;
        g.lightStart = static_cast<uint32_t>(list_.groupLights.size());
        for (uint32_t li : lightSets_[setId]) {
            const LightMeta& meta = lightMeta_[li];
            if (meta.global) {
                for (int c = 0; c < 3; ++c) {
                    g.globalColor[c] += meta.color[c];
                }
            } else {
                list_.groupLights.push_back(meta.instance);
            }
        }
        g.lightCount = static_cast<uint32_t>(list_.groupLights.size()) - g.lightStart;
    };
    if (!list_.lightGroups.back().anyLit) {
        groupSetIds_.back() = setId;
        fill(list_.lightGroups.back());
        return;
    }
    if (groupSetIds_.back() == setId) {
        return;
    }
    Sprite2DLightGroup g;
    g.firstBatch = static_cast<uint32_t>(list_.batches.size());
    list_.lightGroups.push_back(g);
    groupSetIds_.push_back(setId);
    fill(list_.lightGroups.back());
    forceNewBatch_ = true;
}

const Sprite2DDrawList& Sprite2DRenderer::Collect(World& world, RenderResources& resources, const XMFLOAT4X4* view,
                                                  const XMFLOAT4X4* proj, uint64_t simTick)
{
    simTick_ = simTick;
    list_.Clear();
    items_.clear();
    seenMasks_.clear();
    lightMeta_.clear();
    lightSets_.clear();
    groupSetIds_.clear();
    std::fill(std::begin(layerLightSet_), std::end(layerLightSet_), -1);
    forceNewBatch_ = false;
    anyShadowLight_ = false;
    anyNormalMap_ = false;
    anyNormalReady_ = false;
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
    hasParallaxItem_ = false;
    hasCamera_ = false;
    hasOrtho_ = false;
    CollectSprites(world, hasGroups);
    if (hasParallaxItem_ && view != nullptr) {
        // 視差の基準 = カメラのワールド位置 (view の逆行列の平行移動)
        XMVECTOR det;
        const XMMATRIX inv = XMMatrixInverse(&det, XMLoadFloat4x4(view));
        if (std::fabs(XMVectorGetX(det)) >= kMinFrustumDeterminant) {
            XMFLOAT4X4 invView;
            XMStoreFloat4x4(&invView, inv);
            cameraXY_ = { invView._41, invView._42 };
            hasCamera_ = true;
        }
        hasOrtho_ = OrthoWorldBounds(view, proj, orthoMin_, orthoMax_);
    }
    CollectMasks(world, hasGroups);
    CollectLines(world, hasGroups);
    CollectShapes(world, hasGroups);
    CollectTilemaps(world, view, proj);
    if (items_.empty()) {
        tileCache_.clear();
        return list_;
    }

    // 浮動小数点を使わない整数キー。entity.index まで入れるので同値は無く、並びはアーキタイプの走査順に依らない
    // (同じエンティティにスプライトとマスクがあるときだけ isMask で、同じタイルマップのレイヤー / チャンクは添字で決める)
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        return std::tie(a.outerLayer, a.outerOrder, a.outerIndex, a.innerLayer, a.innerOrder, a.innerIndex, a.isMask,
                        a.isLine, a.isShape, a.tileLayer, a.tileChunk.cy, a.tileChunk.cx)
               < std::tie(b.outerLayer, b.outerOrder, b.outerIndex, b.innerLayer, b.innerOrder, b.innerIndex,
                          b.isMask, b.isLine, b.isShape, b.tileLayer, b.tileChunk.cy, b.tileChunk.cx);
    });
    list_.vertices.reserve(items_.size() * 6);
    CollectLights(world, resources);
    if (anyShadowLight_) {
        CollectShadowCasters(world);
    }
    for (const Item& it : items_) {
        if (!lightMeta_.empty()) {
            SwitchLightGroup(it.layerId, it.isTile || it.isLine || it.isShape || (!it.isMask && it.sprite->lit));
        }
        if (it.isTile) {
            EmitTileChunk(it, world, resources);
        } else if (it.isMask) {
            EmitMask(it, resources);
        } else if (it.isLine) {
            EmitLine(it, resources);
        } else if (it.isShape) {
            EmitShape(it, resources);
        } else {
            EmitSprite(it, resources);
        }
    }

    // 使える法線マップが 1 枚も無ければ法線 RT を作らない (平らな法線だけのバッチは捨てる)
    if (!anyNormalReady_) {
        list_.normalVertices.clear();
        list_.normalBatches.clear();
    }

    // 今回どのタイルマップにも使われなかったチャンク (エンティティ / レイヤー / チャンクの削除) を捨てる
    for (auto it = tileCache_.begin(); it != tileCache_.end();) {
        it = (it->second.lastUsed == tileFrame_) ? std::next(it) : tileCache_.erase(it);
    }
    return list_;
}

} // namespace mye
