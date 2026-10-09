//====================================================================================
//                          MeshLodSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          メッシュ LOD (生成・クック・.meta・選択) の自己テストの実装
//====================================================================================
#include "Engine/Engine/Asset/MeshLodSelfTest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Engine/Asset/FbxLoader.h"
#include "Engine/Engine/Asset/MeshLodBuilder.h"
#include "Engine/Engine/Asset/ModelCook.h"
#include "Engine/Engine/Asset/ModelLoader.h"
#include "Engine/Engine/Physics/Collider/MeshColliderLibrary.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Mesh/MeshLod.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {
namespace {

namespace fs = std::filesystem;

constexpr int kSphereRings = 32;
constexpr int kSphereSegments = 64;

// UV 球 (継ぎ目とポールを持つ = 属性付き単純化の実戦的な入力)。極の縮退三角形は作らない
void BuildSphere(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    vertices.clear();
    indices.clear();
    const float pi = 3.14159265358979f;
    for (int r = 0; r <= kSphereRings; ++r) {
        const float phi = pi * static_cast<float>(r) / static_cast<float>(kSphereRings);
        for (int s = 0; s <= kSphereSegments; ++s) {
            const float theta = 2.0f * pi * static_cast<float>(s) / static_cast<float>(kSphereSegments);
            MeshVertex v;
            v.normal = { std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta) };
            v.position = { v.normal.x * 0.5f, v.normal.y * 0.5f, v.normal.z * 0.5f };
            v.uv = { static_cast<float>(s) / static_cast<float>(kSphereSegments),
                     static_cast<float>(r) / static_cast<float>(kSphereRings) };
            vertices.push_back(v);
        }
    }
    for (int r = 0; r < kSphereRings; ++r) {
        for (int s = 0; s < kSphereSegments; ++s) {
            const uint32_t a = static_cast<uint32_t>(r * (kSphereSegments + 1) + s);
            const uint32_t b = a + kSphereSegments + 1;
            if (r != 0) {
                indices.insert(indices.end(), { a, b, a + 1 });
            }
            if (r != kSphereRings - 1) {
                indices.insert(indices.end(), { a + 1, b, b + 1 });
            }
        }
    }
}

// 球を 1 メッシュ 1 プリミティブの GLB にする (ModelLoader のフレッシュパースに通すため)
std::vector<uint8_t> BuildSphereGlb()
{
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    BuildSphere(vertices, indices);

    std::vector<uint8_t> bin;
    auto appendFloats = [&](const float* f, size_t n) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(f);
        bin.insert(bin.end(), p, p + n * sizeof(float));
    };
    const size_t posOffset = bin.size();
    for (const MeshVertex& v : vertices) {
        appendFloats(&v.position.x, 3);
    }
    const size_t nrmOffset = bin.size();
    for (const MeshVertex& v : vertices) {
        appendFloats(&v.normal.x, 3);
    }
    const size_t uvOffset = bin.size();
    for (const MeshVertex& v : vertices) {
        appendFloats(&v.uv.x, 2);
    }
    const size_t idxOffset = bin.size();
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(indices.data());
    bin.insert(bin.end(), ip, ip + indices.size() * sizeof(uint32_t));
    while (bin.size() % 4 != 0) {
        bin.push_back(0);
    }

    char json[2048];
    std::snprintf(
        json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\","
        "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}],"
        "\"buffers\":[{\"byteLength\":%zu}]}",
        vertices.size(), vertices.size(), vertices.size(), indices.size(), posOffset, nrmOffset - posOffset,
        nrmOffset, uvOffset - nrmOffset, uvOffset, idxOffset - uvOffset, idxOffset,
        indices.size() * sizeof(uint32_t), bin.size());
    std::string jsonText = json;
    while (jsonText.size() % 4 != 0) {
        jsonText.push_back(' ');
    }

    std::vector<uint8_t> glb;
    auto appendU32 = [&](uint32_t v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        glb.insert(glb.end(), p, p + 4);
    };
    appendU32(0x46546C67u); // "glTF"
    appendU32(2);
    appendU32(static_cast<uint32_t>(12 + 8 + jsonText.size() + 8 + bin.size()));
    appendU32(static_cast<uint32_t>(jsonText.size()));
    appendU32(0x4E4F534Au); // "JSON"
    glb.insert(glb.end(), jsonText.begin(), jsonText.end());
    appendU32(static_cast<uint32_t>(bin.size()));
    appendU32(0x004E4942u); // "BIN"
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

bool WriteBytes(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

std::string ReadText(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// LOD の段表と index 列が 2 つのメッシュで同一か
bool SameLods(const Mesh& a, const Mesh& b)
{
    return a.lods == b.lods && a.lodIndices == b.lodIndices && a.indices == b.indices
        && a.indexCount == b.indexCount;
}

importmeta::ModelLodSettings MakeSettings(int levels)
{
    importmeta::ModelLodSettings s;
    s.levels = levels;
    s.Normalize();
    return s;
}

// 三角形の頂点が 3 つとも LOD0 の頂点バッファの範囲内か
bool IndicesInRange(const std::vector<uint32_t>& indices, size_t vertexCount)
{
    for (const uint32_t i : indices) {
        if (i >= vertexCount) {
            return false;
        }
    }
    return true;
}

} // namespace

bool RunMeshLodSelfTest()
{
    MYE_LOG_INFO("==== MeshLod self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (1) 選択関数の境界値 (純関数) ----
    {
        // LOD0 / 1 / 2 / 3 の screenSize = 1 / 0.4 / 0.2 / 0.1
        const std::vector<MeshLodLevel> lods = { { 0, 300, 1.0f }, { 300, 150, 0.4f }, { 450, 75, 0.2f },
                                                 { 525, 30, 0.1f } };
        LodSelectParams p;
        check(SelectLod(lods, 0.5f, -1, p) == 0, "select: large on screen -> LOD0");
        check(SelectLod(lods, 0.4f, -1, p) == 0, "select: exactly the threshold stays on the finer stage");
        check(SelectLod(lods, 0.39f, -1, p) == 1, "select: just below the LOD1 threshold -> LOD1");
        check(SelectLod(lods, 0.19f, -1, p) == 2, "select: below the LOD2 threshold -> LOD2");
        check(SelectLod(lods, 0.05f, -1, p) == 3, "select: tiny -> the coarsest stage");
        check(SelectLod(lods, 0.0f, -1, p) == 3, "select: zero size -> the coarsest stage");

        // 履歴あり: 下げる側は 0.9 倍、上げる側は 1.1 倍を越えるまで動かない
        check(SelectLod(lods, 0.39f, 0, p) == 0, "hysteresis: LOD0 holds just below the threshold");
        check(SelectLod(lods, 0.35f, 0, p) == 1, "hysteresis: LOD0 drops once below 0.9x");
        check(SelectLod(lods, 0.41f, 1, p) == 1, "hysteresis: LOD1 holds just above the threshold");
        check(SelectLod(lods, 0.45f, 1, p) == 0, "hysteresis: LOD1 rises once above 1.1x");
        check(SelectLod(lods, 0.19f, 1, p) == 1, "hysteresis: LOD1 holds just below the LOD2 threshold");
        check(SelectLod(lods, 0.17f, 1, p) == 2, "hysteresis: LOD1 drops to LOD2 below 0.9x");
        check(SelectLod(lods, 0.9f, 3, p) == 0, "hysteresis: a big jump crosses several stages at once");
        check(SelectLod(lods, 0.0f, 0, p) == 3, "hysteresis: a collapse crosses several stages downward");

        // 履歴が段数の外 (メッシュが差し替わった) は履歴なし扱い
        check(SelectLod(lods, 0.39f, 9, p) == 1, "history out of range is treated as no history");

        // lodBias は screenSize に掛ける (大きいほど詳細な段を長く使う)
        p.lodBias = 2.0f;
        check(SelectLod(lods, 0.3f, -1, p) == 0, "bias 2: 0.3 is treated as 0.6 -> LOD0");
        p.lodBias = 0.5f;
        check(SelectLod(lods, 0.5f, -1, p) == 1, "bias 0.5: 0.5 is treated as 0.25 -> LOD1");
        p.lodBias = 1.0f;

        // 強制段。無い段は最も粗い段
        p.forcedLod = 2;
        check(SelectLod(lods, 5.0f, -1, p) == 2, "forced stage wins over the screen size");
        p.forcedLod = 7;
        check(SelectLod(lods, 5.0f, -1, p) == 3, "forced stage beyond the table -> the coarsest stage");
        p.forcedLod = 0;
        check(SelectLod(lods, 0.0f, 3, p) == 0, "forced stage 0 beats history");
        p.forcedLod = -1;

        // 段の欠落: 段なし / LOD0 だけ
        check(SelectLod({}, 0.01f, -1, p) == 0, "no table -> LOD0");
        check(SelectLod({ lods[0] }, 0.01f, -1, p) == 0, "LOD0 only -> LOD0");
        p.forcedLod = 3;
        check(SelectLod({ lods[0] }, 0.01f, -1, p) == 0, "forced stage on a single-stage mesh -> LOD0");

        // screen-size の式
        const float persp = LodScreenSize(1.0f, 10.0f, 1.7320508f, false);
        check(std::fabs(persp - 0.17320508f) < 1e-6f, "screen size: perspective is radius * cot(fov/2) / distance");
        check(std::fabs(LodScreenSize(1.0f, 100.0f, 0.5f, true) - 0.5f) < 1e-6f,
              "screen size: orthographic ignores the distance");
        check(LodScreenSize(1.0f, 0.0f, 1.7f, false) > 1000.0f, "screen size: zero distance does not divide by zero");

        // 履歴の世代
        LodHistory history;
        check(history.Get(EntityID{ 3, 1 }) == -1, "history: empty store has no entry");
        history.Set(EntityID{ 3, 1 }, 2);
        check(history.Get(EntityID{ 3, 1 }) == 2, "history: stored stage is returned");
        check(history.Get(EntityID{ 3, 2 }) == -1, "history: a reused index with another generation is a miss");
        history.Clear();
        check(history.Get(EntityID{ 3, 1 }) == -1, "history: Clear drops entries");
    }

    // ---- (2) 生成: 単調減少・決定性・届かない段は作らない ----
    std::vector<MeshVertex> sphereVerts;
    std::vector<uint32_t> sphereIndices;
    BuildSphere(sphereVerts, sphereIndices);
    const size_t lod0Tris = sphereIndices.size() / 3;
    {
        const ModelCook::MeshLodData a = ModelCook::BuildMeshLods("lodtest#sphere", sphereVerts, sphereIndices,
                                                                  MakeSettings(3));
        const ModelCook::MeshLodData b = ModelCook::BuildMeshLods("lodtest#sphere", sphereVerts, sphereIndices,
                                                                  MakeSettings(3));
        check(a.levels.size() == 3, "build: a dense sphere yields all 3 requested stages");
        check(a.levels == b.levels && a.indices == b.indices, "build: two builds are byte-identical");

        bool monotonic = true;
        size_t prevCount = sphereIndices.size();
        float prevScreen = 1.0f;
        size_t expectedStart = sphereIndices.size();
        bool contiguous = true;
        for (const MeshLodLevel& level : a.levels) {
            monotonic = monotonic && level.indexCount < prevCount && level.indexCount % 3 == 0
                     && level.screenSize < prevScreen && level.screenSize > 0.0f;
            contiguous = contiguous && level.indexStart == expectedStart;
            prevCount = level.indexCount;
            prevScreen = level.screenSize;
            expectedStart += level.indexCount;
        }
        check(monotonic, "build: triangle count and screen size strictly decrease per stage");
        check(contiguous && expectedStart == sphereIndices.size() + a.indices.size(),
              "build: stages are laid out contiguously after LOD0");
        check(IndicesInRange(a.indices, sphereVerts.size()), "build: LOD indices point into the LOD0 vertex buffer");
        if (a.levels.size() == 3) {
            check(a.levels[0].indexCount <= static_cast<uint32_t>(lod0Tris * 3 * 0.5 * 1.25)
                      && a.levels[2].indexCount <= static_cast<uint32_t>(lod0Tris * 3 * 0.125 * 1.25),
                  "build: stages land near their target triangle ratios");
        }

        // 段数を減らすと先頭の段は同じ内容になる (段ごとの生成が独立に決定的)
        const ModelCook::MeshLodData one = ModelCook::BuildMeshLods("lodtest#sphere", sphereVerts, sphereIndices,
                                                                    MakeSettings(1));
        check(one.levels.size() == 1 && !a.levels.empty() && one.levels[0] == a.levels[0],
              "build: the first stage does not depend on how many stages follow");

        // 段なし (既定) と、三角形が少なくて作れないメッシュ
        check(ModelCook::BuildMeshLods("lodtest#none", sphereVerts, sphereIndices, MakeSettings(0)).Empty(),
              "build: levels 0 -> no stages");
        const std::vector<MeshVertex> triVerts = { sphereVerts[0], sphereVerts[1], sphereVerts[2] };
        check(ModelCook::BuildMeshLods("lodtest#tri", triVerts, { 0, 1, 2 }, MakeSettings(3)).Empty(),
              "build: a single triangle cannot get stages");
        check(ModelCook::BuildMeshLods("lodtest#bad", sphereVerts, { 0, 1, 99999999 }, MakeSettings(3)).Empty(),
              "build: out-of-range indices are rejected without crashing");

        // 完全に平らな面は目標まで減らせる一方、境界を固定するので開いたメッシュの外周は動かない
        std::vector<MeshVertex> gridVerts;
        std::vector<uint32_t> gridIdx;
        constexpr int kGrid = 16;
        for (int y = 0; y <= kGrid; ++y) {
            for (int x = 0; x <= kGrid; ++x) {
                MeshVertex v;
                v.position = { static_cast<float>(x), 0.0f, static_cast<float>(y) };
                v.uv = { static_cast<float>(x) / kGrid, static_cast<float>(y) / kGrid };
                gridVerts.push_back(v);
            }
        }
        for (int y = 0; y < kGrid; ++y) {
            for (int x = 0; x < kGrid; ++x) {
                const uint32_t i0 = static_cast<uint32_t>(y * (kGrid + 1) + x);
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + kGrid + 1;
                gridIdx.insert(gridIdx.end(), { i0, i2, i1, i1, i2, i2 + 1 });
            }
        }
        const ModelCook::MeshLodData grid = ModelCook::BuildMeshLods("lodtest#grid", gridVerts, gridIdx,
                                                                     MakeSettings(1));
        std::vector<char> used(gridVerts.size(), 0);
        for (const uint32_t i : grid.indices) {
            used[i] = 1;
        }
        const size_t last = static_cast<size_t>(kGrid);
        const bool cornersKept = !grid.levels.empty() && used[0] && used[last] && used[last * (last + 1)]
                              && used[last * (last + 1) + last];
        check(cornersKept, "build: the corners of an open mesh survive simplification (border is locked)");
    }

    // ---- (3) 登録・クック blob: 段表の往復と LOD0 の不変 ----
    {
        RenderResources res;
        ShaderManager sh;
        ModelCook::ModelCookData cook;
        cook.lodSettings = MakeSettings(2);
        const AssetID id = ModelCook::RegisterMeshWithLods(res, &cook, "lodtest#reg", sphereVerts, sphereIndices,
                                                           cook.lodSettings);
        const Mesh* mesh = res.meshes.Get(id);
        check(mesh != nullptr && mesh->lods.size() == 3 && mesh->lods[0].indexCount == sphereIndices.size(),
              "register: the mesh table has LOD0 + 2 stages");
        check(mesh != nullptr && mesh->indices == sphereIndices && mesh->indexCount == sphereIndices.size(),
              "register: the CPU index copy (physics / NavMesh / RT) stays LOD0 only");
        check(mesh != nullptr && mesh->LodRange(1).indexStart == sphereIndices.size()
                  && mesh->LodRange(99).indexStart == mesh->lods.back().indexStart,
              "register: a stage beyond the table falls back to the coarsest one");
        check(cook.meshes.size() == 1 && cook.meshes[0].indices == sphereIndices
                  && cook.meshes[0].lods.size() == 2 && cook.meshes[0].vertices.size() == sphereVerts.size(),
              "cook: the cooked mesh keeps LOD0 as is and carries the stages separately");

        std::vector<uint8_t> blob;
        ModelCook::Serialize(cook, blob);
        ModelCook::ModelCookData back;
        std::vector<uint8_t> reser;
        const bool ok = ModelCook::Deserialize(blob, back);
        if (ok) {
            ModelCook::Serialize(back, reser);
        }
        check(ok && reser == blob, "cook: serialize(deserialize(blob)) round-trips bit-identical with stages");
        check(ok && back.lodSettings == cook.lodSettings && back.meshes[0].lods == cook.meshes[0].lods
                  && back.meshes[0].lodIndices == cook.meshes[0].lodIndices,
              "cook: the settings and the stage table survive the round-trip");

        // Replay が登録した Mesh は、フレッシュパースで登録したものとバイト一致
        RenderResources replayed;
        ModelCook::Replay(replayed, sh, back);
        const Mesh* replayMesh = replayed.meshes.Get(AssetID{ HashStr("lodtest#reg") });
        check(mesh != nullptr && replayMesh != nullptr && SameLods(*mesh, *replayMesh),
              "cook: Replay registers the same stages as the fresh registration");

        // 切り詰めた blob は落ちずに弾く
        bool truncSafe = true;
        for (size_t cut = 0; cut < blob.size(); cut += 997) {
            std::vector<uint8_t> part(blob.begin(), blob.begin() + static_cast<ptrdiff_t>(cut));
            ModelCook::ModelCookData junk;
            truncSafe = truncSafe && !ModelCook::Deserialize(part, junk);
        }
        check(truncSafe, "cook: every truncated blob is rejected without crashing");

        // 矛盾した段表は段なしで登録する (描画は LOD0 のまま続く)
        const std::vector<uint32_t> badTail = { 0, 1, 2 };
        const std::vector<MeshLodLevel> badLevels = { { 12345, 3, 0.5f } };
        const AssetID badId = res.meshes.Register("lodtest#bad", sphereVerts, sphereIndices, badTail, badLevels);
        const Mesh* badMesh = res.meshes.Get(badId);
        check(badMesh != nullptr && badMesh->lods.empty() && badMesh->lodIndices.empty()
                  && badMesh->indexCount == sphereIndices.size(),
              "register: an inconsistent stage table falls back to no stages");

        // 段なしのメッシュは LOD 導入前と同じ登録 (段表も連結 IB も持たない)
        ModelCook::ModelCookData plain;
        const AssetID plainId = ModelCook::RegisterMeshWithLods(res, &plain, "lodtest#plain", sphereVerts,
                                                                sphereIndices, MakeSettings(0));
        const Mesh* plainMesh = res.meshes.Get(plainId);
        check(plainMesh != nullptr && plainMesh->lods.empty() && plainMesh->lodIndices.empty()
                  && plainMesh->indices == sphereIndices && plain.meshes.size() == 1
                  && plain.meshes[0].lodIndices.empty() && plain.meshes[0].lods.empty()
                  && plain.meshes[0].indices == sphereIndices,
              "register: a mesh without stages registers exactly the LOD0 data");
        check(plainMesh != nullptr && plainMesh->LodCount() == 1
                  && plainMesh->LodRange(2).indexCount == sphereIndices.size(),
              "register: a mesh without stages always draws the whole index range");

        // 小さすぎるメッシュ (立方体) は段なしで登録される
        const AssetID cubeRaw = res.meshes.Cube();
        const Mesh* cubeMesh = res.meshes.Get(cubeRaw);
        std::vector<MeshVertex> cubeVerts;
        for (const DirectX::XMFLOAT3& p : cubeMesh->positions) {
            MeshVertex v;
            v.position = p;
            cubeVerts.push_back(v);
        }
        const AssetID smallId = ModelCook::RegisterMeshWithLods(res, nullptr, "lodtest#small", cubeVerts,
                                                                cubeMesh->indices, MakeSettings(3));
        const Mesh* smallMesh = res.meshes.Get(smallId);
        check(smallMesh != nullptr && smallMesh->lods.empty() && smallMesh->indices == cubeMesh->indices,
              "register: a mesh too small for stages registers without them");
    }

    // ---- (3b) GPU: 連結した IB の作成と、デバイス消失の復旧 (M88) での作り直し ----
    {
        GraphicsDevice device;
        if (!device.Init(true)) {
            check(false, "gpu: a WARP device for the LOD buffer test");
        } else {
            RenderResources res;
            res.Init(device);
            ModelCook::ModelCookData cook;
            cook.lodSettings = MakeSettings(2);
            const AssetID id = ModelCook::RegisterMeshWithLods(res, nullptr, "lodtest#gpu", sphereVerts,
                                                               sphereIndices, cook.lodSettings);
            Mesh* mesh = res.meshes.Get(id);
            auto ibIndexCount = [](const Mesh& m) -> uint32_t {
                if (!m.ib) {
                    return 0;
                }
                D3D11_BUFFER_DESC desc = {};
                m.ib->GetDesc(&desc);
                return desc.ByteWidth / static_cast<UINT>(sizeof(uint32_t));
            };
            const uint32_t expected = mesh != nullptr && mesh->lods.size() == 3
                ? mesh->lods[2].indexStart + mesh->lods[2].indexCount
                : 0;
            check(mesh != nullptr && expected != 0 && ibIndexCount(*mesh) == expected,
                  "gpu: the index buffer holds LOD0 followed by every stage");
            const std::vector<MeshLodLevel> lodsBefore = mesh != nullptr ? mesh->lods : std::vector<MeshLodLevel>{};
            res.ReleaseGpu();
            check(mesh != nullptr && !mesh->ib && res.meshes.GetDrawable(id) == nullptr,
                  "gpu: ReleaseGpu drops the buffers and the mesh stops being drawable");
            check(res.RecreateGpu(device) == 0, "gpu: the buffers are recreated");
            mesh = res.meshes.Get(id);
            check(mesh != nullptr && ibIndexCount(*mesh) == expected && mesh->lods == lodsBefore
                      && res.meshes.GetDrawable(id) != nullptr,
                  "gpu: the recreated index buffer holds every stage again with the same table");
        }
    }

    // ---- (4) .meta の読み書き ----
    std::error_code ec;
    const fs::path tempRoot = fs::temp_directory_path(ec) / L"mye_meshlod_selftest";
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot, ec);
    {
        const fs::path metaPath = tempRoot / L"probe.glb.meta";
        AssetMeta m;
        m.guid = 0x1234567890abcdefull;
        m.type = AssetType::Model;
        m.lod.levels = 2;
        m.lod.ratio[0] = 0.6f;
        m.lod.ratio[1] = 0.3f;
        m.lod.screenSize[1] = 0.15f;
        check(AssetDatabase::WriteMeta(metaPath.wstring(), m), ".meta: a model with LOD settings is written");
        AssetMeta back;
        check(AssetDatabase::ReadMeta(metaPath.wstring(), back) && back.lod.levels == 2 && back.lod.ratio[0] == 0.6f
                  && back.lod.ratio[1] == 0.3f && back.lod.screenSize[0] == 0.0f
                  && back.lod.screenSize[1] == 0.15f,
              ".meta: the LOD settings round-trip");

        m.lod = importmeta::ModelLodSettings{};
        AssetDatabase::WriteMeta(metaPath.wstring(), m);
        check(ReadText(metaPath).find("lod") == std::string::npos,
              ".meta: a model without stages writes no \"lod\" key (existing .meta stay byte-identical)");
        AssetMeta noLod;
        check(AssetDatabase::ReadMeta(metaPath.wstring(), noLod) && noLod.lod.levels == 0,
              ".meta: a missing \"lod\" key reads as no stages");

        // 範囲外の値は丸める
        importmeta::ModelLodSettings wild;
        wild.levels = 9;
        wild.ratio[0] = 5.0f;
        wild.ratio[1] = -1.0f;
        wild.Normalize();
        check(wild.levels == 3 && wild.ratio[0] <= 0.95f && wild.ratio[1] >= 0.01f,
              ".meta: levels and ratios are clamped to the valid range");
    }

    // ---- (5) 実ファイル (GLB): クックの決定性・キャッシュ経由とフレッシュパースの一致・.meta の変更で再クック ----
    {
        const fs::path cookDir = tempRoot / L"cooked";
        const fs::path glb = tempRoot / L"lod_sphere.glb";
        const fs::path metaPath = tempRoot / L"lod_sphere.glb.meta";
        check(WriteBytes(glb, BuildSphereGlb()), "glb: the test sphere is written");

        AssetDatabase db;
        auto writeMeta = [&](int levels) {
            AssetMeta m;
            m.guid = 0x5a5a5a5a12345678ull;
            m.type = AssetType::Model;
            m.lod = MakeSettings(levels);
            AssetDatabase::WriteMeta(metaPath.wstring(), m);
        };
        writeMeta(2);
        db.InstallAsKeyResolver();
        CookedCache::Configure(cookDir.wstring(), true);

        const std::wstring path = glb.wstring();
        const std::string meshKey = "guid://5a5a5a5a12345678#mesh0#prim0";
        const AssetID meshId{ HashStr(meshKey) };
        ShaderManager sh;

        RenderResources cold;
        check(ModelLoader::RegisterAssets(cold, sh, path, true), "glb: cold cook succeeds");
        const Mesh* coldMesh = cold.meshes.Get(meshId);
        check(coldMesh != nullptr && coldMesh->lods.size() == 3 && coldMesh->indexCount == lod0Tris * 3,
              "glb: the cold cook builds the stages from the .meta");
        std::vector<uint8_t> blob1;
        check(CookedCache::ReadValidated(path, ModelCook::kModelExt, blob1), "glb: the cook file is valid");

        // クックし直してもバイト一致
        fs::remove(CookedCache::PathFor(path, ModelCook::kModelExt), ec);
        RenderResources cold2;
        ModelLoader::RegisterAssets(cold2, sh, path, true);
        std::vector<uint8_t> blob2;
        check(CookedCache::ReadValidated(path, ModelCook::kModelExt, blob2) && blob1 == blob2,
              "glb: re-cooking is byte-identical");

        // キャッシュ経由 (ウォーム) とフレッシュパース (キャッシュ無効) が一致
        RenderResources warm;
        ModelLoader::RegisterAssets(warm, sh, path, true);
        const Mesh* warmMesh = warm.meshes.Get(meshId);
        check(coldMesh != nullptr && warmMesh != nullptr && SameLods(*coldMesh, *warmMesh),
              "glb: the cache replay matches the fresh parse");
        CookedCache::Configure(L"", false);
        RenderResources fresh;
        ModelLoader::RegisterAssets(fresh, sh, path, true);
        const Mesh* freshMesh = fresh.meshes.Get(meshId);
        check(coldMesh != nullptr && freshMesh != nullptr && SameLods(*coldMesh, *freshMesh),
              "glb: a fresh parse without the cache builds the same stages");
        CookedCache::Configure(cookDir.wstring(), true);

        // 物理コライダー / NavMesh / RT の入力 (MeshColliderData) は LOD0 のまま
        {
            MeshColliderLibrary colliders;
            colliders.Init(&warm);
            const MeshColliderData* data = colliders.Get(meshId);
            check(data != nullptr && static_cast<size_t>(data->TriCount()) == lod0Tris,
                  "glb: the mesh collider (shared by NavMesh and RT) keeps the LOD0 triangle count");
        }

        // .meta を変えると手動削除なしで再クックされる
        writeMeta(1);
        RenderResources one;
        ModelLoader::RegisterAssets(one, sh, path, true);
        const Mesh* oneMesh = one.meshes.Get(meshId);
        std::vector<uint8_t> blob3;
        ModelCook::ModelCookData d3;
        CookedCache::ReadValidated(path, ModelCook::kModelExt, blob3);
        const bool d3ok = ModelCook::Deserialize(blob3, d3);
        check(oneMesh != nullptr && oneMesh->lods.size() == 2 && d3ok && d3.lodSettings.levels == 1,
              "glb: changing the .meta stage count recooks without deleting the cache");
        RenderResources again;
        ModelLoader::RegisterAssets(again, sh, path, true);
        const Mesh* againMesh = again.meshes.Get(meshId);
        check(oneMesh != nullptr && againMesh != nullptr && SameLods(*oneMesh, *againMesh),
              "glb: after the recook the cache hits again with the same stages");

        // 比だけの変更でも再クックされる
        {
            AssetMeta m;
            m.guid = 0x5a5a5a5a12345678ull;
            m.type = AssetType::Model;
            m.lod.levels = 1;
            m.lod.ratio[0] = 0.3f;
            m.lod.Normalize();
            AssetDatabase::WriteMeta(metaPath.wstring(), m);
            RenderResources ratio;
            ModelLoader::RegisterAssets(ratio, sh, path, true);
            const Mesh* ratioMesh = ratio.meshes.Get(meshId);
            check(ratioMesh != nullptr && oneMesh != nullptr && ratioMesh->lods.size() == 2
                      && ratioMesh->lods[1].indexCount < oneMesh->lods[1].indexCount,
                  "glb: changing only the target ratio recooks");
        }

        // 段をやめる (levels 0): LOD0 だけ。blob の設定も段なしへ戻る
        writeMeta(0);
        RenderResources none;
        ModelLoader::RegisterAssets(none, sh, path, true);
        const Mesh* noneMesh = none.meshes.Get(meshId);
        check(noneMesh != nullptr && noneMesh->lods.empty() && noneMesh->indices == coldMesh->indices,
              "glb: removing the stages leaves exactly the LOD0 data");

        AssetDatabase::UninstallKeyResolver();
        CookedCache::Configure(L"", false);
    }

    // ---- (6) 実アセット: 三角形ごとに頂点を持つ FBX (溶接が要る) とスキン付きメッシュ ----
    {
        const std::wstring assetsRoot = FindAssetsRoot();
        struct FbxCase {
            const wchar_t* rel;
            const char* label;
            bool isFbx;
            bool expectStages; // 実アセットの形状から、段が作れるはずのもの
        };
        const FbxCase cases[] = {
            { L"\\models\\player_fp_v01\\Player_Researcher_FP.fbx", "organic fbx", true, true },
            { L"\\models\\skinned_beam.fbx", "skinned fbx (hard surface)", true, false },
            { L"\\models\\CesiumMan.glb", "skinned glb (organic)", false, true },
        };
        const fs::path cookDir = tempRoot / L"cooked_fbx";
        AssetDatabase db;
        db.InstallAsKeyResolver();
        CookedCache::Configure(cookDir.wstring(), true);
        for (const FbxCase& fc : cases) {
            const fs::path src = tempRoot / (std::wstring(L"lodreal_") + std::to_wstring(&fc - cases)
                                             + (fc.isFbx ? L".fbx" : L".glb"));
            fs::copy_file(assetsRoot + fc.rel, src, fs::copy_options::overwrite_existing, ec);
            AssetMeta m;
            m.guid = 0x7b00000000000001ull + static_cast<uint64_t>(&fc - cases);
            m.type = AssetType::Model;
            m.lod = MakeSettings(2);
            AssetDatabase::WriteMeta(src.wstring() + L".meta", m);

            ShaderManager sh;
            RenderResources res;
            const bool loaded = fc.isFbx ? FbxLoader::RegisterAssets(res, sh, src.wstring(), true)
                                         : ModelLoader::RegisterAssets(res, sh, src.wstring(), true);
            std::vector<uint8_t> blob1;
            ModelCook::ModelCookData d;
            const bool cooked = CookedCache::ReadValidated(src.wstring(), ModelCook::kModelExt, blob1)
                             && ModelCook::Deserialize(blob1, d);
            check(loaded && cooked && !d.meshes.empty(), fc.label);

            size_t withStages = 0;
            bool sane = true;
            for (const ModelCook::CookedMesh& cm : d.meshes) {
                const Mesh* mesh = res.meshes.Get(AssetID{ HashStr(cm.key) });
                sane = sane && mesh != nullptr && mesh->indices == cm.indices;
                size_t prevCount = cm.indices.size();
                for (const MeshLodLevel& level : cm.lods) {
                    sane = sane && level.indexCount < prevCount;
                    prevCount = level.indexCount;
                }
                sane = sane && IndicesInRange(cm.lodIndices, cm.vertices.size());
                withStages += cm.lods.empty() ? 0 : 1;
                MYE_LOG_INFO("  [lod] %s %s: LOD0 %zu tris -> %zu stage(s)", fc.label, cm.key.c_str(),
                             cm.indices.size() / 3, cm.lods.size());
            }
            check(sane, "real asset: stages are strictly decreasing and LOD0 is untouched");

            // クックし直してもバイト一致 (溶接・単純化が決定的)
            fs::remove(CookedCache::PathFor(src.wstring(), ModelCook::kModelExt), ec);
            RenderResources res2;
            if (fc.isFbx) {
                FbxLoader::RegisterAssets(res2, sh, src.wstring(), true);
            } else {
                ModelLoader::RegisterAssets(res2, sh, src.wstring(), true);
            }
            std::vector<uint8_t> blob2;
            check(CookedCache::ReadValidated(src.wstring(), ModelCook::kModelExt, blob2) && blob1 == blob2,
                  "real asset: re-cooking with stages is byte-identical");
            if (fc.expectStages) {
                check(withStages > 0, "real asset: an organic mesh gets stages (welding and bone weights work)");
            }
        }
        AssetDatabase::UninstallKeyResolver();
        CookedCache::Configure(L"", false);
    }

    fs::remove_all(tempRoot, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== MeshLod self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== MeshLod self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
