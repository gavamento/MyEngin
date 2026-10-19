//====================================================================================
//                          Sprite2DRenderer.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D の収集とソートの実装
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h"

#include <algorithm>
#include <tuple>

#include "Engine/Core/Asset/AssetGuidResolver.h"
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

// 1 スプライトの描画に使う解決済みの値
struct ResolvedSprite {
    AssetID texture;
    float uv0[2]; // 左上の UV
    float uv1[2]; // 右下の UV
    float local[4];     // pivot 原点のローカル矩形 {left, bottom, right, top}
    bool pointFilter = false;
};

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

void Sprite2DRenderer::Push(AssetID texture, bool pointFilter, const Sprite2DVertex* v, uint32_t count)
{
    if (list_.batches.empty() || list_.batches.back().texture != texture
        || list_.batches.back().pointFilter != pointFilter) {
        Sprite2DBatch b;
        b.texture = texture;
        b.pointFilter = pointFilter;
        b.start = static_cast<uint32_t>(list_.vertices.size());
        list_.batches.push_back(b);
    }
    list_.vertices.insert(list_.vertices.end(), v, v + count);
    list_.batches.back().count += count;
}

void Sprite2DRenderer::Emit(const Item& item, RenderResources& resources)
{
    const Sprite2DComponent& sp = *item.sprite;
    TextureLibrary& textures = resources.textures;

    ResolvedSprite r;
    SpriteLibrary* lib = spritelib::Library();
    const SpriteDef* def = (lib != nullptr) ? lib->Get(sp.sprite) : nullptr;
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
                return;
            }
        } else if (textures.IsPending(def->texture)) {
            return; // 白のプレースホルダ (1x1) の間は寸法も絵も嘘になる
        } else {
            const float texW = static_cast<float>((std::max)(tex->width, 1));
            const float texH = static_cast<float>((std::max)(tex->height, 1));
            float rx = static_cast<float>(def->rect[0]);
            float ry = static_cast<float>(def->rect[1]);
            float rw = static_cast<float>(def->rect[2]);
            float rh = static_cast<float>(def->rect[3]);
            if (def->IsFullTexture()) {
                rx = 0.0f;
                ry = 0.0f;
                rw = texW;
                rh = texH;
            }
            r.texture = def->texture;
            r.uv0[0] = rx / texW;
            r.uv0[1] = ry / texH;
            r.uv1[0] = (rx + rw) / texW;
            r.uv1[1] = (ry + rh) / texH;
            LocalRect(*def, static_cast<int>(texW), static_cast<int>(texH), r.local);
            r.pointFilter = (def->filter == 2);
        }
    }
    if (useMissing) {
        r.texture = EnsureMissingTexture(resources);
        r.uv0[0] = 0.0f;
        r.uv0[1] = 0.0f;
        r.uv1[0] = 1.0f;
        r.uv1[1] = 1.0f;
        constexpr float kHalf = 0.5f * kMissingTextureSize / kMissingPixelsPerUnit;
        r.local[0] = -kHalf;
        r.local[1] = -kHalf;
        r.local[2] = kHalf;
        r.local[3] = kHalf;
    }
    if (r.texture.IsNull()) {
        return;
    }

    // ローカルの矩形 (pivot が原点)。flip は pivot まわりの鏡映 = 位置の符号を反転するだけで UV はそのまま
    const float left = r.local[0];
    const float bottom = r.local[1];
    const float right = r.local[2];
    const float top = r.local[3];
    const float sx = sp.flipX ? -1.0f : 1.0f;
    const float sy = sp.flipY ? -1.0f : 1.0f;

    // 左上, 右上, 右下, 左下 (テクスチャは上が v0)
    const float lx[4] = { left * sx, right * sx, right * sx, left * sx };
    const float ly[4] = { top * sy, top * sy, bottom * sy, bottom * sy };
    const float cu[4] = { r.uv0[0], r.uv1[0], r.uv1[0], r.uv0[0] };
    const float cv[4] = { r.uv0[1], r.uv0[1], r.uv1[1], r.uv1[1] };

    const XMFLOAT4X4& m = *item.world;
    Sprite2DVertex corner[4];
    for (int i = 0; i < 4; ++i) {
        // ローカル (x, y, 0) × ワールド行列 (行ベクトル規約)
        corner[i].pos[0] = lx[i] * m._11 + ly[i] * m._21 + m._41;
        corner[i].pos[1] = lx[i] * m._12 + ly[i] * m._22 + m._42;
        corner[i].pos[2] = lx[i] * m._13 + ly[i] * m._23 + m._43;
        corner[i].uv[0] = cu[i];
        corner[i].uv[1] = cv[i];
        corner[i].color[0] = sp.color.x;
        corner[i].color[1] = sp.color.y;
        corner[i].color[2] = sp.color.z;
        corner[i].color[3] = sp.color.w;
    }
    const Sprite2DVertex quad[6] = { corner[0], corner[1], corner[2], corner[0], corner[2], corner[3] };
    Push(r.texture, r.pointFilter, quad, 6);
}

const Sprite2DDrawList& Sprite2DRenderer::Collect(World& world, RenderResources& resources)
{
    list_.Clear();
    items_.clear();

    const ComponentTypeId req[] = { Sprite2DComponent::sTypeId, WorldMatrixComponent::sTypeId };
    const SortingLayers& layers = SortingLayers::Get();
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
            it.layerOrder = layers.OrderOf(it.sprite->sortingLayerId);
            it.orderInLayer = it.sprite->orderInLayer;
            it.entityIndex = e.index;
            items_.push_back(it);
        }
    });
    if (items_.empty()) {
        return list_;
    }

    // 浮動小数点を使わない整数キー。entity.index まで入れるので同値は無く、並びはアーキタイプの走査順に依らない
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        return std::tie(a.layerOrder, a.orderInLayer, a.groupKey, a.entityIndex)
               < std::tie(b.layerOrder, b.orderInLayer, b.groupKey, b.entityIndex);
    });
    list_.vertices.reserve(items_.size() * 6);
    for (const Item& it : items_) {
        Emit(it, resources);
    }
    return list_;
}

} // namespace mye
