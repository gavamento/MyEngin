#include "Engine/Engine/TerrainSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/RenderTypes.h" // SrgbToLinear (M58d: tint を CB 前にリニアへ)

using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4X4;
using DirectX::XMVECTOR;

namespace mye {

// Renderer 層のミラー定数の突合。層規約 (Renderer は Engine を読めない) で 2 箇所に
// 分かれているので、両方を読める唯一の場所であるここで機械的に止める
static_assert(kTerrainLayerCount == TerrainAsset::kMaxLayers,
              "TerrainPass.h の kTerrainLayerCount と TerrainAsset::kMaxLayers が食い違っている");

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

// レイヤが 1 枚も無い地形の色 (M58c の暫定サーフェスと同値。authored sRGB)。
// ここを変えると「レイヤ未設定の地形」の絵が変わる = M58c の golden とは別物になる
constexpr float kNoLayerColor[3] = { 0.40f, 0.43f, 0.33f };

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

// ==== 純関数: LOD (M58e) ====

void TerrainLodSamples(uint32_t tile0, uint32_t tiles, uint32_t stride, std::vector<uint32_t>& out)
{
    out.clear();
    if (stride == 0) {
        stride = 1;
    }
    if (tiles == 0) {
        out.push_back(tile0);
        return;
    }
    for (uint32_t t = 0; t < tiles; t += stride) {
        out.push_back(tile0 + t);
    }
    // 末尾は必ずチャンクの端。stride で割り切れないぶんは最後のセルが短くなるだけで、
    // 縁の texel 位置は LOD に依らず同じ = 同 LOD の隣接チャンクが縁を共有できる
    out.push_back(tile0 + tiles);
}

uint32_t SelectTerrainLod(float viewZ, float lodDistance)
{
    // NaN も「LOD 0」に倒す (比較を否定形で書いてあるのはそのため)
    if (!(lodDistance > 0.0f) || !(viewZ > 0.0f)) {
        return 0;
    }
    uint32_t lod = 0;
    float threshold = lodDistance;
    while (lod + 1 < kTerrainLodCount && viewZ >= threshold) {
        ++lod;
        threshold *= 2.0f;
    }
    return lod;
}

float ComputeMaxLodEdgeGap(const TerrainAsset::TerrainData& data,
                           const TerrainChunkLayout& layout, uint32_t lodCount)
{
    if (!data.Valid() || layout.chunks.empty() || lodCount <= 1) {
        return 0.0f;
    }
    const uint32_t lods = std::min(lodCount, kTerrainLodCount);
    const uint32_t tilesX = data.heightW - 1;
    const uint32_t tilesZ = data.heightH - 1;

    std::vector<uint32_t> samples;
    std::vector<float> curves[kTerrainLodCount]; // 縁 1 本ぶんの「LOD ごとの高さ列」
    float worst = 0.0f;

    // 縁 1 本 (fixed = 縁の位置 texel、alongZ = 縁が z 方向に走る) について、
    // 全 LOD 対の最大差を worst へ畳む。LOD 0 の列は実高さそのもの
    auto edgeGap = [&](uint32_t fixed, bool alongZ, uint32_t begin, uint32_t count) {
        if (count == 0) {
            return;
        }
        const uint32_t n = count + 1;
        for (uint32_t lod = 0; lod < lods; ++lod) {
            TerrainLodSamples(begin, count, TerrainLodStride(lod), samples);
            curves[lod].assign(n, 0.0f);
            for (size_t k = 0; k + 1 < samples.size(); ++k) {
                const uint32_t a = samples[k];
                const uint32_t b = samples[k + 1];
                const float ha =
                    alongZ ? data.HeightAtTexel(fixed, a) : data.HeightAtTexel(a, fixed);
                const float hb =
                    alongZ ? data.HeightAtTexel(fixed, b) : data.HeightAtTexel(b, fixed);
                for (uint32_t t = a; t <= b; ++t) {
                    const float w = static_cast<float>(t - a) / static_cast<float>(b - a);
                    curves[lod][t - begin] = ha + (hb - ha) * w;
                }
            }
        }
        for (uint32_t la = 0; la < lods; ++la) {
            for (uint32_t lb = la + 1; lb < lods; ++lb) {
                for (uint32_t i = 0; i < n; ++i) {
                    worst = std::max(worst, std::fabs(curves[la][i] - curves[lb][i]));
                }
            }
        }
    };

    for (const TerrainChunk& c : layout.chunks) {
        if (c.tilesX == 0 || c.tilesZ == 0) {
            continue;
        }
        // 内側の縁だけ。隣接チャンクは同じ行/列なので縁に沿った texel 範囲が一致する =
        // 片側から測れば両側ぶんを見たことになる
        if (c.tileX0 > 0) {
            edgeGap(c.tileX0, true, c.tileZ0, c.tilesZ);
        }
        if (c.tileX0 + c.tilesX < tilesX) {
            edgeGap(c.tileX0 + c.tilesX, true, c.tileZ0, c.tilesZ);
        }
        if (c.tileZ0 > 0) {
            edgeGap(c.tileZ0, false, c.tileX0, c.tilesX);
        }
        if (c.tileZ0 + c.tilesZ < tilesZ) {
            edgeGap(c.tileZ0 + c.tilesZ, false, c.tileX0, c.tilesX);
        }
    }
    return worst;
}

void ExpandLayoutForSkirt(TerrainChunkLayout& layout, uint32_t lodCount, float skirtDepth)
{
    layout.lodCount = std::max(1u, std::min(lodCount, kTerrainLodCount));
    layout.skirtDepth = (layout.lodCount > 1 && skirtDepth > 0.0f) ? skirtDepth : 0.0f;
    if (layout.skirtDepth <= 0.0f) {
        return;
    }
    for (TerrainChunk& c : layout.chunks) {
        c.localMin.y -= layout.skirtDepth;
    }
}

// ==== 純関数: メッシュ生成 ====

void BuildChunkMesh(const TerrainAsset::TerrainData& data, const TerrainChunk& chunk,
                    std::vector<MeshVertex>& outVerts, std::vector<uint32_t>& outIndices,
                    uint32_t lod, float skirtDepth)
{
    outVerts.clear();
    outIndices.clear();
    if (!data.Valid() || chunk.tilesX == 0 || chunk.tilesZ == 0) {
        return;
    }
    // M58e: LOD の刻みで頂点格子を間引く。末尾は必ずチャンクの端 (TerrainLodSamples)
    const uint32_t stride = TerrainLodStride(lod);
    std::vector<uint32_t> sampX, sampZ;
    TerrainLodSamples(chunk.tileX0, chunk.tilesX, stride, sampX);
    TerrainLodSamples(chunk.tileZ0, chunk.tilesZ, stride, sampZ);
    const uint32_t vx = static_cast<uint32_t>(sampX.size());
    const uint32_t vz = static_cast<uint32_t>(sampZ.size());
    const float invX = 1.0f / static_cast<float>(data.heightW - 1);
    const float invZ = 1.0f / static_cast<float>(data.heightH - 1);
    const float stepX = data.worldSizeX * invX; // タイル 1 枚のワールド幅
    const float stepZ = data.worldSizeZ * invZ;

    outVerts.resize(static_cast<size_t>(vx) * vz);
    for (uint32_t iz = 0; iz < vz; ++iz) {
        for (uint32_t ix = 0; ix < vx; ++ix) {
            const int32_t tx = static_cast<int32_t>(sampX[ix]);
            const int32_t tz = static_cast<int32_t>(sampZ[iz]);
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
    // ここを裏返すと裏面カリングで地形が丸ごと消える。
    // **セル数は LOD 後の格子** (vx-1, vz-1) — chunk.tilesX を使うと間引いた分だけ
    // index が頂点数を飛び越す (LOD 0 では偶然一致するので気付けない)
    outIndices.reserve(static_cast<size_t>(vx - 1) * (vz - 1) * 6);
    for (uint32_t iz = 0; iz + 1 < vz; ++iz) {
        for (uint32_t ix = 0; ix + 1 < vx; ++ix) {
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

    // ---- スカート (M58e) ----
    // LOD が違う隣接チャンクは縁の高さが食い違う (粗い側は制御点間を直線で結ぶ)。
    // 縁の帯を真下へ伸ばしておくと、その食い違いぶんが必ず面で埋まる = クラックが出ない。
    // **インデックス縫合を採らなかった理由**: 隣の LOD を見て縁の三角形を組み替える方式は
    // 「隣が誰か」をメッシュ生成に持ち込む = チャンクのメッシュがカメラ位置の関数になり、
    // キャッシュが毎フレーム崩れる。スカートはメッシュがカメラから独立したまま済む
    if (skirtDepth <= 0.0f) {
        return;
    }
    const uint32_t tilesX = data.heightW - 1;
    const uint32_t tilesZ = data.heightH - 1;
    const bool hasNegX = chunk.tileX0 > 0;
    const bool hasPosX = chunk.tileX0 + chunk.tilesX < tilesX;
    const bool hasNegZ = chunk.tileZ0 > 0;
    const bool hasPosZ = chunk.tileZ0 + chunk.tilesZ < tilesZ;
    if (!hasNegX && !hasPosX && !hasNegZ && !hasPosZ) {
        return; // 単一チャンクの地形 = 割れる縁が無い
    }

    std::vector<uint32_t> rim;
    // rim = 縁の頂点 index 列。**周回の向きを 4 辺で揃えてある**ので、下の 1 つの
    // 三角形パターンだけで 4 辺すべてが「チャンクの外向き」になる
    // (-X は z 昇順 / +Z は x 昇順 / +X は z 降順 / -Z は x 降順)。
    // 向きを間違えると裏面カリングで消え、**隙間が塞がっていないようにしか見えない**
    auto emitSkirt = [&](const std::vector<uint32_t>& top) {
        if (top.size() < 2) {
            return;
        }
        const uint32_t base = static_cast<uint32_t>(outVerts.size());
        for (uint32_t i : top) {
            MeshVertex v = outVerts[i];
            v.position.y -= skirtDepth;
            outVerts.push_back(v); // 法線 / UV は上の頂点のまま (地表の続きとして陰影が繋がる)
        }
        for (size_t k = 0; k + 1 < top.size(); ++k) {
            const uint32_t t0 = top[k];
            const uint32_t t1 = top[k + 1];
            const uint32_t b0 = base + static_cast<uint32_t>(k);
            outIndices.push_back(t0);
            outIndices.push_back(b0);
            outIndices.push_back(t1);
            outIndices.push_back(b0);
            outIndices.push_back(b0 + 1);
            outIndices.push_back(t1);
        }
    };

    if (hasNegX) {
        rim.clear();
        for (uint32_t iz = 0; iz < vz; ++iz) {
            rim.push_back(iz * vx);
        }
        emitSkirt(rim);
    }
    if (hasPosZ) {
        rim.clear();
        for (uint32_t ix = 0; ix < vx; ++ix) {
            rim.push_back((vz - 1) * vx + ix);
        }
        emitSkirt(rim);
    }
    if (hasPosX) {
        rim.clear();
        for (uint32_t iz = vz; iz-- > 0;) {
            rim.push_back(iz * vx + (vx - 1));
        }
        emitSkirt(rim);
    }
    if (hasNegZ) {
        rim.clear();
        for (uint32_t ix = vx; ix-- > 0;) {
            rim.push_back(ix);
        }
        emitSkirt(rim);
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

// ==== レイヤ解決 (M58d) ====

TerrainSurface BuildTerrainSurface(const TerrainAsset::TerrainData& data,
                                   const std::wstring& srcPath, TextureLibrary& textures)
{
    TerrainSurface s;

    // 平坦法線の 1x1 (128,128,255)。**シェーダに分岐を持たせない**ための受け皿で、
    // 法線マップ未設定のレイヤはこれをサンプルする (*2-1 して ~(0,0,1) = 摂動なし)。
    // 分岐で逃がすと ddx/ddy が非一様フローに入り、PerturbNormal の勾配が壊れる
    const AssetID flatNormal = textures.CreateSolid("terrain://flatnormal", 128, 128, 255, 255);
    const AssetID white = textures.White();

    // スプラットマップは `.mterr` の中にしかない生成物なので生画素から作る。
    // ★名前に**中身のハッシュ**を混ぜる: TextureLibrary は同名先勝ちなので、パスだけを
    //   キーにすると M58f のブラシで焼き直しても古い重みが出続ける
    if (!data.splat.empty()) {
        const uint64_t contentHash = HashBytes(data.splat.data(), data.splat.size());
        char name[64] = {};
        std::snprintf(name, sizeof(name), "terrain://splat/%016llx",
                      static_cast<unsigned long long>(contentHash));
        s.splat = textures.CreateFromRgba8(name, data.splat.data(),
                                           static_cast<int>(data.splatW),
                                           static_cast<int>(data.splatH));
    }

    const uint32_t layerCount =
        std::min<uint32_t>(static_cast<uint32_t>(data.layers.size()), kTerrainLayerCount);
    for (uint32_t i = 0; i < kTerrainLayerCount; ++i) {
        TerrainLayerBinding& b = s.layers[i];
        b.albedo = white;
        b.normal = flatNormal;
        b.tilingU = 1.0f;
        b.tilingV = 1.0f;
        b.tint = { 0.0f, 0.0f, 0.0f, 0.0f }; // a=0 = このレイヤの重みを殺す
        if (i >= layerCount) {
            continue;
        }
        const TerrainAsset::TerrainLayer& l = data.layers[i];
        if (!l.albedo.empty()) {
            // アルベドは sRGB (サンプル時に HW デコード)、法線はデータ系なのでリニア
            const AssetID id = textures.LoadFile(TerrainAsset::ResolveLayerPath(srcPath, l.albedo),
                                                 true);
            if (!id.IsNull()) {
                b.albedo = id;
            }
        }
        if (!l.normal.empty()) {
            const AssetID id = textures.LoadFile(TerrainAsset::ResolveLayerPath(srcPath, l.normal),
                                                 false);
            if (!id.IsNull()) {
                b.normal = id;
            }
        }
        const DirectX::XMFLOAT3 lin = SrgbToLinear(DirectX::XMFLOAT3{ l.tintR, l.tintG, l.tintB });
        b.tint = { lin.x, lin.y, lin.z, 1.0f };
        b.tilingU = l.tilingU;
        b.tilingV = l.tilingV;
    }

    if (layerCount == 0) {
        // レイヤ表が空 = M58c の単色地形に倒す (既定値 = 従来の見た目)
        const DirectX::XMFLOAT3 lin = SrgbToLinear(
            DirectX::XMFLOAT3{ kNoLayerColor[0], kNoLayerColor[1], kNoLayerColor[2] });
        s.layers[0].tint = { lin.x, lin.y, lin.z, 1.0f };
    }
    return s;
}

// ==== ランタイム ====

const TerrainSystem::Entry* TerrainSystem::Acquire(const char* source,
                                                   const TerrainBuildParams& params,
                                                   MeshLibrary& meshes, TextureLibrary& textures,
                                                   const std::wstring& assetsRoot)
{
    if (source == nullptr || source[0] == '\0') {
        return nullptr;
    }
    const uint32_t ct = ClampChunkTiles(params.chunkTiles);
    const uint32_t lodCount = std::max(1u, std::min(params.lodCount, kTerrainLodCount));
    std::wstring abs = assetsRoot;
    if (!abs.empty() && abs.back() != L'\\' && abs.back() != L'/') {
        abs += L'\\';
    }
    abs += Utf8ToWide(source);
    // キーは正規化済み絶対パス + 組み立てパラメータ全部。粒度や LOD/スカートを変えたら
    // 別インスタンス = Inspector でスライダを動かした瞬間に組み直る。
    // ★skirtDepth は**ビットパターン**で混ぜる (to_wstring(float) はロケール依存の桁落ちで
    //   別の値が同じキーに化けうる)
    uint32_t skirtBits = 0;
    std::memcpy(&skirtBits, &params.skirtDepth, sizeof(skirtBits));
    wchar_t suffix[64] = {};
    std::swprintf(suffix, std::size(suffix), L"#%u#%u#%08x", ct, lodCount,
                  static_cast<unsigned>(lodCount > 1 ? skirtBits : 0u));
    std::wstring key = NormalizePathKey(abs) + suffix;

    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return &it->second;
    }

    Entry entry;
    entry.lodCount = lodCount;
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

    // ---- スカート深さの解決 (M58e) ----
    // 0 = 自動: LOD 対が縁に作る最大段差 (+ ラスタライズぶんの余裕) をそのまま使う。
    // 負 = スカート無し (A/B 撮影用の明示 off)。正 = 打った値をそのまま
    float skirt = 0.0f;
    if (lodCount > 1) {
        if (params.skirtDepth > 0.0f) {
            skirt = params.skirtDepth;
        } else if (params.skirtDepth == 0.0f) {
            skirt = ComputeMaxLodEdgeGap(entry.data, entry.layout, lodCount) * kTerrainSkirtMargin;
        }
    }
    ExpandLayoutForSkirt(entry.layout, lodCount, skirt);

    // メッシュ名はパスキーのハッシュから作る。パスをそのまま名前にすると
    // チェックアウト先で AssetID が変わる (= M51j でシーンをコミットできなくした穴と同じ) が、
    // 地形メッシュはランタイム生成物でシーンに保存されないので実害は無い。
    // それでも短く固定長にしておくと、ホットリロードで同名再登録 = 差し替えになる
    const uint64_t pathHash = HashStr(WideToUtf8(key));
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> indices;
    entry.chunkMeshes.assign(entry.layout.chunks.size() * lodCount, AssetID{});
    for (size_t i = 0; i < entry.layout.chunks.size(); ++i) {
        for (uint32_t lod = 0; lod < lodCount; ++lod) {
            BuildChunkMesh(entry.data, entry.layout.chunks[i], verts, indices, lod,
                           entry.layout.skirtDepth);
            if (indices.empty()) {
                continue; // 空チャンク (端数 0) は AssetID を空のまま残す
            }
            char name[96] = {};
            std::snprintf(name, sizeof(name), "terrain://%016llx/%u/l%u",
                          static_cast<unsigned long long>(pathHash), static_cast<unsigned>(i),
                          static_cast<unsigned>(lod));
            entry.chunkMeshes[i * lodCount + lod] = meshes.Register(name, verts, indices);
        }
    }
    entry.surface = BuildTerrainSurface(entry.data, abs, textures); // M58d
    entry.valid = true;
    MYE_LOG_INFO("terrain: %s -> %ux%u chunks (%u tiles each, %u lod, skirt %.3f)", source,
                 entry.layout.countX, entry.layout.countZ, entry.layout.chunkTiles, lodCount,
                 static_cast<double>(entry.layout.skirtDepth));

    auto [pos, inserted] = cache_.emplace(std::move(key), std::move(entry));
    (void)inserted;
    return &pos->second;
}

uint32_t TerrainSystem::Collect(World& world, MeshLibrary& meshes, TextureLibrary& textures,
                                const std::wstring& assetsRoot, const Frustum& frustum,
                                const XMFLOAT4X4& view, std::vector<TerrainDrawItem>& out)
{
    out.clear();
    lastTotal_ = 0;
    lastVisible_ = 0;
    for (uint32_t& n : lastLodCounts_) {
        n = 0;
    }

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
            // M58e: LOD を使うかどうかはコンポーネントの lodDistance だけで決まる。
            // 0 (既定) なら段数 1 = M58d までと同じ 1 本のメッシュ = 絵は 1 画素も変わらない
            TerrainBuildParams params;
            params.chunkTiles = tc->chunkTiles;
            params.lodCount = (tc->lodDistance > 0.0f) ? kTerrainLodCount : 1u;
            params.skirtDepth = tc->skirtDepth;
            const Entry* entry = Acquire(tc->source, params, meshes, textures, assetsRoot);
            if (entry == nullptr || !entry->valid) {
                continue;
            }
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            lastTotal_ += static_cast<uint32_t>(entry->layout.chunks.size());
            CullChunks(entry->layout, frustum, wm->value, visibleScratch_);
            for (uint32_t ci : visibleScratch_) {
                TerrainDrawItem item;
                item.world = wm->value;
                item.terrain = &entry->data;
                item.entity = e;
                item.chunkIndex = ci;
                item.surface = entry->surface; // M58d (地形単位の値をチャンクへ複製)
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
                // M58e: LOD はチャンクの viewZ だけで決まる純関数。**メッシュを選ぶのは
                // ここ 1 箇所**で、描画側 (TerrainPass) は LOD の存在を知らない
                item.lod = std::min(SelectTerrainLod(item.viewZ, tc->lodDistance),
                                    entry->lodCount - 1);
                item.mesh = entry->chunkMeshes[static_cast<size_t>(ci) * entry->lodCount
                                               + item.lod];
                if (item.mesh.IsNull()) {
                    continue;
                }
                ++lastLodCounts_[item.lod];
                out.push_back(item);
            }
        }
    });
    lastVisible_ = static_cast<uint32_t>(out.size());
    return lastTotal_;
}

} // namespace mye
