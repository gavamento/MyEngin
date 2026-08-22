#include "Engine/Engine/TerrainSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Platform/PathUtil.h"

using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4X4;
using DirectX::XMVECTOR;

namespace mye {
namespace {

// texel 座標を地形の範囲へクランプして高さを引く。
// **TerrainData::HeightAtTexel を直接呼ばない**: あちらの引数は uint32_t なので
// x-1 (x==0) が 0xFFFFFFFF に化け、std::min のクランプで「反対側の端」に落ちる。
// 中心差分は必ず境界を跨ぐので、ここを符号付きで受けるのが唯一の正しい入口
float HeightClamped(const TerrainAsset::TerrainData& d, int32_t x, int32_t z)
{
    const int32_t cx = std::clamp(x, 0, static_cast<int32_t>(d.heightW) - 1);
    const int32_t cz = std::clamp(z, 0, static_cast<int32_t>(d.heightH) - 1);
    return d.HeightAtTexel(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz));
}

} // namespace

// ==== 純関数: 分割 ====

const TerrainChunk* TerrainChunkLayout::At(uint32_t cx, uint32_t cz) const
{
    if (cx >= countX || cz >= countZ) {
        return nullptr;
    }
    return &chunks[static_cast<size_t>(cz) * countX + cx];
}

uint32_t ClampChunkTiles(int32_t requested)
{
    const int32_t v = std::clamp(requested, kTerrainMinChunkTiles, kTerrainMaxChunkTiles);
    return static_cast<uint32_t>(v);
}

bool BuildChunkLayout(const TerrainAsset::TerrainData& data, int32_t chunkTiles,
                      TerrainChunkLayout& out)
{
    out = TerrainChunkLayout{};
    if (!data.Valid()) {
        return false;
    }
    const uint32_t ct = ClampChunkTiles(chunkTiles);
    // 頂点数 - 1 = タイル数。Valid() が heightW/H >= 2 を保証しているので 0 にはならない
    const uint32_t tilesX = data.heightW - 1;
    const uint32_t tilesZ = data.heightH - 1;
    out.chunkTiles = ct;
    out.countX = (tilesX + ct - 1) / ct; // 端数チャンクも 1 枚として数える (切り上げ)
    out.countZ = (tilesZ + ct - 1) / ct;
    out.chunks.resize(static_cast<size_t>(out.countX) * out.countZ);

    const float invX = 1.0f / static_cast<float>(tilesX);
    const float invZ = 1.0f / static_cast<float>(tilesZ);

    for (uint32_t cz = 0; cz < out.countZ; ++cz) {
        for (uint32_t cx = 0; cx < out.countX; ++cx) {
            TerrainChunk& c = out.chunks[static_cast<size_t>(cz) * out.countX + cx];
            c.chunkX = cx;
            c.chunkZ = cz;
            c.tileX0 = cx * ct;
            c.tileZ0 = cz * ct;
            c.tilesX = std::min(ct, tilesX - c.tileX0);
            c.tilesZ = std::min(ct, tilesZ - c.tileZ0);

            // 高さの min/max は担当タイルの**頂点**範囲 (= タイル数 + 1) で取る。
            // 1 本足りないと隣接チャンクと共有する縁の頂点が AABB から溢れる
            float hMin = 0.0f;
            float hMax = 0.0f;
            bool first = true;
            for (uint32_t z = c.tileZ0; z <= c.tileZ0 + c.tilesZ; ++z) {
                for (uint32_t x = c.tileX0; x <= c.tileX0 + c.tilesX; ++x) {
                    const float h = data.HeightAtTexel(x, z);
                    hMin = first ? h : std::min(hMin, h);
                    hMax = first ? h : std::max(hMax, h);
                    first = false;
                }
            }
            const float x0 = (static_cast<float>(c.tileX0) * invX - 0.5f) * data.worldSizeX;
            const float x1 =
                (static_cast<float>(c.tileX0 + c.tilesX) * invX - 0.5f) * data.worldSizeX;
            const float z0 = (static_cast<float>(c.tileZ0) * invZ - 0.5f) * data.worldSizeZ;
            const float z1 =
                (static_cast<float>(c.tileZ0 + c.tilesZ) * invZ - 0.5f) * data.worldSizeZ;
            c.localMin = { x0, hMin, z0 };
            c.localMax = { x1, hMax, z1 };
        }
    }
    return true;
}

// ==== 純関数: メッシュ生成 ====

void BuildChunkMesh(const TerrainAsset::TerrainData& data, const TerrainChunk& chunk,
                    std::vector<MeshVertex>& outVerts, std::vector<uint32_t>& outIndices)
{
    outVerts.clear();
    outIndices.clear();
    if (!data.Valid() || chunk.tilesX == 0 || chunk.tilesZ == 0) {
        return;
    }
    const uint32_t vx = chunk.tilesX + 1;
    const uint32_t vz = chunk.tilesZ + 1;
    const float invX = 1.0f / static_cast<float>(data.heightW - 1);
    const float invZ = 1.0f / static_cast<float>(data.heightH - 1);
    const float stepX = data.worldSizeX * invX; // タイル 1 枚のワールド幅
    const float stepZ = data.worldSizeZ * invZ;

    outVerts.resize(static_cast<size_t>(vx) * vz);
    for (uint32_t iz = 0; iz < vz; ++iz) {
        for (uint32_t ix = 0; ix < vx; ++ix) {
            const int32_t tx = static_cast<int32_t>(chunk.tileX0 + ix);
            const int32_t tz = static_cast<int32_t>(chunk.tileZ0 + iz);
            const float u = static_cast<float>(tx) * invX;
            const float v = static_cast<float>(tz) * invZ;
            const float h = HeightClamped(data, tx, tz);

            // 中心差分。境界の texel は片側差分に縮む (クランプの結果) が、
            // それは**地形の外周でだけ**起きる — チャンク境界は地形の内側なので
            // 隣り合うチャンクが同じ texel から同じ法線を出す = 継ぎ目が出ない
            const float hl = HeightClamped(data, tx - 1, tz);
            const float hr = HeightClamped(data, tx + 1, tz);
            const float hb = HeightClamped(data, tx, tz - 1);
            const float hf = HeightClamped(data, tx, tz + 1);
            XMFLOAT3 n = { -(hr - hl) / (2.0f * stepX), 1.0f, -(hf - hb) / (2.0f * stepZ) };
            const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len > 0.0f) {
                n = { n.x / len, n.y / len, n.z / len };
            } else {
                n = { 0.0f, 1.0f, 0.0f };
            }

            MeshVertex& mv = outVerts[static_cast<size_t>(iz) * vx + ix];
            // 中心原点 (組み込みプリミティブと同じ規約)。エンティティのワールド行列が
            // 地形の**中心**を置く = Inspector で位置 0 のまま原点に収まる
            mv.position = { (u - 0.5f) * data.worldSizeX, h, (v - 0.5f) * data.worldSizeZ };
            mv.normal = n;
            // UV は地形全体を [0,1] に張った正規化座標。レイヤごとの繰り返しは
            // シェーダ側で tiling を掛ける (M58d)。MeshVertex に 2 セット目 UV は足さない
            mv.uv = { u, v };
        }
    }

    // 巻き順は builtin plane (0,1,2 / 0,2,3 で法線 +Y) に合わせる。
    // ここを裏返すと裏面カリングで地形が丸ごと消える
    outIndices.reserve(static_cast<size_t>(chunk.tilesX) * chunk.tilesZ * 6);
    for (uint32_t iz = 0; iz < chunk.tilesZ; ++iz) {
        for (uint32_t ix = 0; ix < chunk.tilesX; ++ix) {
            const uint32_t i00 = iz * vx + ix;
            const uint32_t i01 = (iz + 1) * vx + ix;
            const uint32_t i11 = (iz + 1) * vx + ix + 1;
            const uint32_t i10 = iz * vx + ix + 1;
            outIndices.push_back(i00);
            outIndices.push_back(i01);
            outIndices.push_back(i11);
            outIndices.push_back(i00);
            outIndices.push_back(i11);
            outIndices.push_back(i10);
        }
    }
}

// ==== 純関数: カリング ====

size_t CullChunks(const TerrainChunkLayout& layout, const Frustum& frustum,
                  const XMFLOAT4X4& world, std::vector<uint32_t>& outVisible)
{
    outVisible.clear();
    for (size_t i = 0; i < layout.chunks.size(); ++i) {
        const TerrainChunk& c = layout.chunks[i];
        if (AabbInFrustum(frustum, world, c.localMin, c.localMax)) {
            outVisible.push_back(static_cast<uint32_t>(i));
        }
    }
    return outVisible.size();
}

// ==== ランタイム ====

const TerrainSystem::Entry* TerrainSystem::Acquire(const char* source, int32_t chunkTiles,
                                                   MeshLibrary& meshes,
                                                   const std::wstring& assetsRoot)
{
    if (source == nullptr || source[0] == '\0') {
        return nullptr;
    }
    const uint32_t ct = ClampChunkTiles(chunkTiles);
    std::wstring abs = assetsRoot;
    if (!abs.empty() && abs.back() != L'\\' && abs.back() != L'/') {
        abs += L'\\';
    }
    abs += Utf8ToWide(source);
    // キーは正規化済み絶対パス + チャンク粒度。粒度を変えたら別インスタンス =
    // Inspector でスライダを動かした瞬間に組み直る
    std::wstring key = NormalizePathKey(abs) + L"#" + std::to_wstring(ct);

    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return &it->second;
    }

    Entry entry;
    if (!TerrainAsset::Load(abs, entry.data)) {
        MYE_LOG_WARN("terrain: failed to load %s", source);
        cache_.emplace(std::move(key), std::move(entry)); // valid=false のまま覚える
        return nullptr;
    }
    if (!BuildChunkLayout(entry.data, static_cast<int32_t>(ct), entry.layout)) {
        MYE_LOG_WARN("terrain: chunk layout failed for %s", source);
        cache_.emplace(std::move(key), std::move(entry));
        return nullptr;
    }

    // メッシュ名はパスキーのハッシュから作る。パスをそのまま名前にすると
    // チェックアウト先で AssetID が変わる (= M51j でシーンをコミットできなくした穴と同じ) が、
    // 地形メッシュはランタイム生成物でシーンに保存されないので実害は無い。
    // それでも短く固定長にしておくと、ホットリロードで同名再登録 = 差し替えになる
    const uint64_t pathHash = HashStr(WideToUtf8(key));
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> indices;
    entry.chunkMeshes.resize(entry.layout.chunks.size());
    for (size_t i = 0; i < entry.layout.chunks.size(); ++i) {
        BuildChunkMesh(entry.data, entry.layout.chunks[i], verts, indices);
        if (indices.empty()) {
            continue; // 空チャンク (端数 0) は AssetID を空のまま残す
        }
        char name[96] = {};
        std::snprintf(name, sizeof(name), "terrain://%016llx/%u",
                      static_cast<unsigned long long>(pathHash), static_cast<unsigned>(i));
        entry.chunkMeshes[i] = meshes.Register(name, verts, indices);
    }
    entry.valid = true;
    MYE_LOG_INFO("terrain: %s -> %ux%u chunks (%u tiles each)", source, entry.layout.countX,
                 entry.layout.countZ, entry.layout.chunkTiles);

    auto [pos, inserted] = cache_.emplace(std::move(key), std::move(entry));
    (void)inserted;
    return &pos->second;
}

uint32_t TerrainSystem::Collect(World& world, MeshLibrary& meshes, const std::wstring& assetsRoot,
                                const Frustum& frustum, const XMFLOAT4X4& view,
                                std::vector<TerrainDrawItem>& out)
{
    out.clear();
    lastTotal_ = 0;
    lastVisible_ = 0;

    const DirectX::XMMATRIX v = DirectX::XMLoadFloat4x4(&view);
    const ComponentTypeId req[] = { TerrainComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(TerrainComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* tc = static_cast<const TerrainComponent*>(arch.GetPtr(ti, row));
            const Entry* entry = Acquire(tc->source, tc->chunkTiles, meshes, assetsRoot);
            if (entry == nullptr || !entry->valid) {
                continue;
            }
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            lastTotal_ += static_cast<uint32_t>(entry->layout.chunks.size());
            CullChunks(entry->layout, frustum, wm->value, visibleScratch_);
            for (uint32_t ci : visibleScratch_) {
                if (entry->chunkMeshes[ci].IsNull()) {
                    continue;
                }
                TerrainDrawItem item;
                item.mesh = entry->chunkMeshes[ci];
                item.world = wm->value;
                item.terrain = &entry->data;
                item.entity = e;
                item.chunkIndex = ci;
                // viewZ はチャンク AABB 中心のカメラ空間深度 (RenderItem と同じ規約)。
                // 地形はエンティティ原点が全チャンクで共通なので、原点を使うと
                // どのチャンクも同じ深度になり LOD (M58e) もソートも成立しない
                const TerrainChunk& c = entry->layout.chunks[ci];
                const DirectX::XMVECTOR centerLS =
                    DirectX::XMVectorSet((c.localMin.x + c.localMax.x) * 0.5f,
                                         (c.localMin.y + c.localMax.y) * 0.5f,
                                         (c.localMin.z + c.localMax.z) * 0.5f, 1.0f);
                const DirectX::XMMATRIX w = DirectX::XMLoadFloat4x4(&wm->value);
                const DirectX::XMVECTOR centerVS =
                    DirectX::XMVector3TransformCoord(centerLS, DirectX::XMMatrixMultiply(w, v));
                item.viewZ = DirectX::XMVectorGetZ(centerVS);
                out.push_back(item);
            }
        }
    });
    lastVisible_ = static_cast<uint32_t>(out.size());
    return lastTotal_;
}

} // namespace mye
