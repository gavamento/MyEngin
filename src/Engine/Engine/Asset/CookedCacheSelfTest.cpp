#include "Engine/Engine/Asset/CookedCacheSelfTest.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Engine/Asset/ModelCook.h"
#include "Engine/Engine/Asset/TerrainAsset.h"
#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ImageWrite.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/Skeleton.h"

namespace mye {
namespace {

namespace fs = std::filesystem;

bool WriteFileBytes(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

// SkinnedModel の全 float / 文字列 / 構造の bitwise 一致
bool SkinEqual(const SkinnedModel& a, const SkinnedModel& b)
{
    if (a.joints.size() != b.joints.size() || a.clips.size() != b.clips.size()) {
        return false;
    }
    for (size_t j = 0; j < a.joints.size(); ++j) {
        const SkeletonJoint& x = a.joints[j];
        const SkeletonJoint& y = b.joints[j];
        if (x.parent != y.parent || x.name != y.name
            || memcmp(&x.inverseBind, &y.inverseBind, sizeof(x.inverseBind)) != 0
            || memcmp(&x.bindT, &y.bindT, sizeof(x.bindT)) != 0
            || memcmp(&x.bindR, &y.bindR, sizeof(x.bindR)) != 0
            || memcmp(&x.bindS, &y.bindS, sizeof(x.bindS)) != 0) {
            return false;
        }
    }
    for (size_t c = 0; c < a.clips.size(); ++c) {
        const SkeletalClip& x = a.clips[c];
        const SkeletalClip& y = b.clips[c];
        if (x.name != y.name || memcmp(&x.duration, &y.duration, sizeof(float)) != 0
            || x.tracks.size() != y.tracks.size()) {
            return false;
        }
        auto vecEq = [](const auto& u, const auto& v) {
            using T = std::remove_reference_t<decltype(u[0])>;
            return u.size() == v.size()
                   && (u.empty() || memcmp(u.data(), v.data(), u.size() * sizeof(T)) == 0);
        };
        for (size_t t = 0; t < x.tracks.size(); ++t) {
            const JointTrack& p = x.tracks[t];
            const JointTrack& q = y.tracks[t];
            if (!vecEq(p.tTimes, q.tTimes) || !vecEq(p.tVals, q.tVals) || !vecEq(p.rTimes, q.rTimes)
                || !vecEq(p.rVals, q.rVals) || !vecEq(p.sTimes, q.sTimes)
                || !vecEq(p.sVals, q.sVals)) {
                return false;
            }
        }
    }
    return true;
}

// blob の登録内容が resources のライブラリに bitwise 一致で入っていること。
// Mesh の CPU コピーは position/normal/uv とインデックスを持つ (boneWeights は
// GPU 専用なので blob 往復テスト側で被覆する)
bool LibrariesMatchBlob(RenderResources& res, const ModelCook::ModelCookData& d,
                        const char* who)
{
    for (const ModelCook::CookedMesh& m : d.meshes) {
        Mesh* mesh = res.meshes.Get(AssetID{ HashStr(m.key) });
        if (!mesh || mesh->positions.size() != m.vertices.size()
            || mesh->indices.size() != m.indices.size()) {
            MYE_LOG_ERROR("  mesh missing or size mismatch (%s): %s", who, m.key.c_str());
            return false;
        }
        for (size_t i = 0; i < m.vertices.size(); ++i) {
            if (memcmp(&mesh->positions[i], &m.vertices[i].position, sizeof(float) * 3) != 0
                || memcmp(&mesh->normals[i], &m.vertices[i].normal, sizeof(float) * 3) != 0
                || memcmp(&mesh->uvs[i], &m.vertices[i].uv, sizeof(float) * 2) != 0) {
                MYE_LOG_ERROR("  vertex bits differ (%s): %s [%zu]", who, m.key.c_str(), i);
                return false;
            }
        }
        if (!m.indices.empty()
            && memcmp(mesh->indices.data(), m.indices.data(),
                      m.indices.size() * sizeof(uint32_t)) != 0) {
            MYE_LOG_ERROR("  index bits differ (%s): %s", who, m.key.c_str());
            return false;
        }
    }
    for (const ModelCook::CookedMaterial& m : d.materials) {
        Material* mat = res.materials.Get(AssetID{ HashStr(m.key) });
        if (!mat || memcmp(mat, &m.mat, sizeof(Material)) != 0) {
            MYE_LOG_ERROR("  material missing or bits differ (%s): %s", who, m.key.c_str());
            return false;
        }
    }
    for (const ModelCook::CookedSkin& s : d.skins) {
        const SkinnedModel* model = res.skinnedModels.Get(AssetID{ HashStr(s.key) });
        if (!model || !SkinEqual(*model, s.model)) {
            MYE_LOG_ERROR("  skin missing or bits differ (%s): %s", who, s.key.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

bool RunCookedCacheSelfTest()
{
    MYE_LOG_INFO("==== CookedCache self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    std::error_code ec;
    const fs::path tempRoot = fs::temp_directory_path(ec) / L"mye_cook_selftest";
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot, ec);
    const fs::path cookDir = tempRoot / L"cooked";

    // ---- (0) 未設定時は全 API が no-op (エンジン外での誤作動防止) ----
    {
        std::vector<uint8_t> payload;
        check(!CookedCache::Enabled(), "unconfigured: Enabled() is false");
        check(!CookedCache::ReadValidated(L"C:\\fake\\x.bin", L".tst", payload),
              "unconfigured: ReadValidated is a no-op");
    }

    CookedCache::Configure(cookDir.wstring(), true);

    // ---- (1) 実アセット 2 種 (glTF / FBX): クック決定論 + 形式往復 + ウォーム再生の内容一致 ----
    const std::wstring assetsRoot = FindAssetsRoot();
    struct ModelCase {
        const wchar_t* rel;
        bool isFbx;
        const char* label;
    };
    const ModelCase cases[] = {
        { L"\\models\\CesiumMan.glb", false, "glTF CesiumMan" },
        { L"\\models\\skinned_beam.fbx", true, "FBX skinned_beam" },
    };
    for (const ModelCase& mc : cases) {
        const std::wstring src = assetsRoot + mc.rel;
        auto registerOnce = [&](RenderResources& res, ShaderManager& sh) {
            return mc.isFbx ? FbxLoader::RegisterAssets(res, sh, src, /*logErrors=*/true)
                            : ModelLoader::RegisterAssets(res, sh, src, /*logErrors=*/true);
        };

        RenderResources resCold;
        ShaderManager shCold;
        check(registerOnce(resCold, shCold), mc.label);
        std::vector<uint8_t> payload1;
        check(CookedCache::ReadValidated(src, ModelCook::kModelExt, payload1),
              "cold run wrote a valid cook file");

        // パース決定論: クックし直しても blob がビット一致する
        fs::remove(CookedCache::PathFor(src, ModelCook::kModelExt), ec);
        RenderResources resCold2;
        ShaderManager shCold2;
        registerOnce(resCold2, shCold2);
        std::vector<uint8_t> payload2;
        check(CookedCache::ReadValidated(src, ModelCook::kModelExt, payload2) && payload1 == payload2,
              "re-cook is bit-identical (parse determinism)");

        ModelCook::ModelCookData d;
        check(ModelCook::Deserialize(payload1, d), "blob deserializes");
        std::vector<uint8_t> reser;
        ModelCook::Serialize(d, reser);
        check(reser == payload1, "serialize(deserialize(blob)) round-trips bit-identical");
        check(!d.meshes.empty() && !d.materials.empty() && !d.skins.empty(),
              "blob contains meshes, materials and skins");

        RenderResources resWarm;
        ShaderManager shWarm;
        check(registerOnce(resWarm, shWarm), "warm run succeeds");
        check(LibrariesMatchBlob(resCold, d, "cold"), "fresh parse matches the blob bit-exactly");
        check(LibrariesMatchBlob(resWarm, d, "warm"), "warm replay matches the blob bit-exactly");
    }

    // ---- (2) ウォーム経路の識別力: blob を改竄すると登録内容に現れる = 再生経路が使われた証明 ----
    // (フレッシュパースならソース由来の値が入るので、この検査は「本当にキャッシュから
    //  再生された」ことを識別できる)
    {
        const fs::path tamperSrc = tempRoot / L"tamper.fbx";
        fs::copy_file(assetsRoot + L"\\models\\skinned_beam.fbx", tamperSrc, ec);
        RenderResources resCold;
        ShaderManager shCold;
        check(FbxLoader::RegisterAssets(resCold, shCold, tamperSrc.wstring(), true),
              "tamper: cold cook of the copied fbx");
        std::vector<uint8_t> payload;
        ModelCook::ModelCookData d;
        const bool loaded =
            CookedCache::ReadValidated(tamperSrc.wstring(), ModelCook::kModelExt, payload)
            && ModelCook::Deserialize(payload, d) && !d.meshes.empty()
            && !d.meshes[0].vertices.empty();
        check(loaded, "tamper: blob is readable");
        if (loaded) {
            const float sentinel = 12345.0f;
            d.meshes[0].vertices[0].position.x = sentinel;
            std::vector<uint8_t> tampered;
            ModelCook::Serialize(d, tampered);
            CookedCache::Write(tamperSrc.wstring(), ModelCook::kModelExt, tampered.data(),
                               tampered.size(), d.ExternalDeps());
            RenderResources resWarm;
            ShaderManager shWarm;
            FbxLoader::RegisterAssets(resWarm, shWarm, tamperSrc.wstring(), true);
            Mesh* mesh = resWarm.meshes.Get(AssetID{ HashStr(d.meshes[0].key) });
            check(mesh && !mesh->positions.empty()
                      && memcmp(&mesh->positions[0].x, &sentinel, sizeof(float)) == 0,
                  "tamper: warm run served the tampered blob (replay path is really used)");
        }
    }

    // ---- (3) 無効化マトリクス (合成ソース + 合成 payload、パーサは介さない) ----
    {
        const fs::path src = tempRoot / L"src.bin";
        const std::vector<uint8_t> contentA = { 1, 2, 3, 4, 5, 6, 7, 8 };
        std::vector<uint8_t> contentB = contentA;
        contentB[3] = 99; // 同サイズ・別内容
        const std::vector<uint8_t> payload = { 10, 20, 30, 40 };
        std::vector<uint8_t> got;

        // ★書いた**後に mtime を明示的に固定する**。ReadValidated は「サイズ同一 かつ
        //   mtime 同一」なら内容を読まずに hit する高速路を持つ (M51b の設計意図)。
        //   NTFS の最終更新時刻は約 14 ms 刻みなので、2 回の書き込みが同じ刻みに入ると
        //   mtime が動かず、同サイズ書換でも**正当に** hit してしまう = 時計運で落ちる
        //   テストになる (M52b の CI が実際にこれで赤くなった)。
        //   刻みをまたぐ保証を「速く書けば動く」に委ねず、秒単位で離して確定させる
        auto writeAt = [&](const std::vector<uint8_t>& bytes, int secondsAgo) {
            WriteFileBytes(src, bytes);
            fs::last_write_time(
                src, fs::file_time_type::clock::now() - std::chrono::seconds(secondsAgo), ec);
        };

        writeAt(contentA, 300);
        check(CookedCache::Write(src.wstring(), L".tst", payload.data(), payload.size()),
              "invalidation: write succeeds");
        check(CookedCache::ReadValidated(src.wstring(), L".tst", got) && got == payload,
              "invalidation: unchanged source hits");

        writeAt(contentB, 200); // 同サイズの内容改変 + mtime も違う → ハッシュで検出
        check(!CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "invalidation: same-size content change misses (hash check)");

        writeAt(contentA, 100); // 内容を戻す = mtime だけが違う状態
        check(CookedCache::ReadValidated(src.wstring(), L".tst", got) && got == payload,
              "invalidation: mtime-only change hits via content hash (self-heal)");
        check(CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "invalidation: still hits after self-heal");

        std::vector<uint8_t> bigger = contentA;
        bigger.push_back(0);
        WriteFileBytes(src, bigger); // サイズ変化 → 即 miss
        check(!CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "invalidation: size change misses");
        WriteFileBytes(src, contentA);

        // 移動 → パスキー (およびパスハッシュ GUID) が変わるので miss
        const fs::path moved = tempRoot / L"src_moved.bin";
        fs::rename(src, moved, ec);
        check(!CookedCache::ReadValidated(moved.wstring(), L".tst", got),
              "invalidation: moved source misses (path key)");
        fs::rename(moved, src, ec);

        // 外部依存 (テクスチャ相当) の消失 → miss、復活 → hit
        const fs::path dep = tempRoot / L"dep.png";
        WriteFileBytes(dep, contentA);
        CookedCache::Write(src.wstring(), L".tst", payload.data(), payload.size(),
                           { dep.wstring() });
        check(CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "invalidation: dep present hits");
        fs::remove(dep, ec);
        check(!CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "invalidation: missing dep misses");
        WriteFileBytes(dep, contentB); // 内容は問わない (リプレイ時に実ファイルを読み直す)
        check(CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "invalidation: restored dep hits again");

        // 破損耐性: マジック破壊 / 末尾切り詰め — miss になるだけで落ちない
        const fs::path cookFile = CookedCache::PathFor(src.wstring(), L".tst");
        std::vector<uint8_t> cookBytes;
        {
            std::ifstream f(cookFile, std::ios::binary);
            cookBytes.assign(std::istreambuf_iterator<char>(f), {});
        }
        std::vector<uint8_t> corrupted = cookBytes;
        corrupted[0] ^= 0xFF;
        WriteFileBytes(cookFile, corrupted);
        check(!CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "corruption: bad magic misses without crashing");
        corrupted = cookBytes;
        corrupted.pop_back();
        WriteFileBytes(cookFile, corrupted);
        check(!CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "corruption: truncated file misses without crashing");

        // 完全なゴミの Deserialize も安全に false
        ModelCook::ModelCookData junkOut;
        check(!ModelCook::Deserialize(contentB, junkOut), "corruption: junk blob deserialize fails");
    }

    // ---- (3b) 封印キャッシュ (M51j、配布ビルド): kSealedMarker でソース検証が跳ぶ ----
    // 配布物は移設で pathKey/mtime が必ずズレ、DDS 一括クック後は元画像自体が無い。
    // 封印中は magic/version/guid だけ見て再生し、マーカーを外せば通常の無効化に戻る
    {
        const fs::path src = tempRoot / L"sealed_src.bin";
        const fs::path dep = tempRoot / L"sealed_dep.png";
        const std::vector<uint8_t> content = { 9, 8, 7, 6, 5 };
        const std::vector<uint8_t> payload = { 42, 43, 44 };
        std::vector<uint8_t> got;
        WriteFileBytes(src, content);
        WriteFileBytes(dep, content);
        CookedCache::Write(src.wstring(), L".tst", payload.data(), payload.size(),
                           { dep.wstring() });

        // 対照実験: 封印前は内容改変 + 依存消失で miss (通常の無効化が生きている)
        WriteFileBytes(src, { 9, 8, 7, 6, 4 });
        fs::remove(dep, ec);
        check(!CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "sealed: without marker, changed source misses (control)");

        WriteFileBytes(cookDir / CookedCache::kSealedMarker, { 1 });
        CookedCache::Configure(cookDir.wstring(), true); // エンジン同様、起動時判定を再現
        check(CookedCache::Sealed(), "sealed: marker detected by Configure");
        check(CookedCache::ReadValidated(src.wstring(), L".tst", got) && got == payload,
              "sealed: changed source + missing dep still hit (validation skipped)");
        fs::remove(src, ec); // DDS 一括クック後の「元画像なし」相当
        check(CookedCache::ReadValidated(src.wstring(), L".tst", got) && got == payload,
              "sealed: even a deleted source replays");

        // 破損はやはり弾く (magic/version/guid は封印中も見る)
        const fs::path cookFile = CookedCache::PathFor(src.wstring(), L".tst");
        std::vector<uint8_t> cookBytes;
        {
            std::ifstream f(cookFile, std::ios::binary);
            cookBytes.assign(std::istreambuf_iterator<char>(f), {});
        }
        cookBytes[0] ^= 0xFF;
        WriteFileBytes(cookFile, cookBytes);
        check(!CookedCache::ReadValidated(src.wstring(), L".tst", got),
              "sealed: corrupt magic still misses");

        fs::remove(cookDir / CookedCache::kSealedMarker, ec);
        CookedCache::Configure(cookDir.wstring(), true);
        check(!CookedCache::Sealed(), "sealed: marker removal restores live validation");
    }

    // ---- (4) 形式往復: エッジな float ビットパターン (NaN / -0.0 / 非正規化数) を保つ ----
    {
        ModelCook::ModelCookData d;
        ModelCook::CookedMesh mesh;
        mesh.key = "edge#mesh0";
        MeshVertex v;
        uint32_t nanBits = 0x7FC00000u, negZero = 0x80000000u, denorm = 0x00000001u;
        memcpy(&v.position.x, &nanBits, 4);
        memcpy(&v.position.y, &negZero, 4);
        memcpy(&v.position.z, &denorm, 4);
        v.boneIndices[0] = 255;
        v.boneWeights = { 0.25f, 0.25f, 0.25f, 0.25f };
        mesh.vertices = { v };
        mesh.indices = { 0, 0, 0 };
        d.meshes.push_back(mesh);
        ModelCook::CookedSkin skin;
        skin.key = "edge#skin0";
        SkeletonJoint j;
        j.name = "骨"; // 非 ASCII 名も往復すること
        memcpy(&j.bindT.x, &negZero, 4);
        skin.model.joints = { j };
        SkeletalClip clip;
        clip.name = "clip";
        clip.duration = 1.0f;
        JointTrack tr;
        tr.tTimes = { 0.0f, 0.5f };
        tr.tVals = { { 0, 0, 0 }, { 1, 2, 3 } };
        clip.tracks = { tr };
        skin.model.clips = { clip };
        d.skins.push_back(skin);

        std::vector<uint8_t> blob;
        ModelCook::Serialize(d, blob);
        ModelCook::ModelCookData back;
        const bool ok = ModelCook::Deserialize(blob, back);
        check(ok && back.meshes.size() == 1
                  && memcmp(back.meshes[0].vertices.data(), d.meshes[0].vertices.data(),
                            sizeof(MeshVertex)) == 0,
              "edge floats: NaN / -0.0 / denormal survive the round-trip bit-exactly");
        check(ok && back.skins.size() == 1 && SkinEqual(back.skins[0].model, d.skins[0].model),
              "edge skin: non-ASCII joint name and tracks round-trip");
    }

    // ---- (5) .ogg PCM クック (.mpcm): 合成クリップの往復 ----
    {
        const fs::path fakeOgg = tempRoot / L"fake.ogg";
        WriteFileBytes(fakeOgg, { 0x4F, 0x67, 0x67, 0x53, 0, 1, 2, 3 }); // 検証はヘッダ stat のみ
        AudioClip clip;
        clip.sampleRate = 48000;
        clip.channels = 2;
        clip.samples.resize(1000);
        for (size_t i = 0; i < clip.samples.size(); ++i) {
            clip.samples[i] = static_cast<int16_t>((i * 131) % 65536 - 32768);
        }
        clip.samples[0] = INT16_MIN;
        clip.samples[1] = INT16_MAX;
        SaveCookedClip(fakeOgg.wstring(), clip);
        AudioClip back;
        const bool hit = LoadCookedClip(fakeOgg.wstring(), back);
        check(hit && back.sampleRate == clip.sampleRate && back.channels == clip.channels
                  && back.samples == clip.samples,
              "pcm: synthetic clip round-trips bit-exactly");
    }

    // ---- (6) 地形クック (.mterr、M58a) ----
    // 地形は描画専用レーンだが、M59 の地形コリジョンがこの blob をハッシュレーンへ持ち込む。
    // その前提条件が「クックがバイト決定論」なので、ここで先に固定しておく
    {
        using TerrainAsset::TerrainData;
        const fs::path terrDir = tempRoot / L"terr";
        fs::create_directories(terrDir, ec);
        auto writeText = [&](const fs::path& p, const std::string& s) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write(s.data(), static_cast<std::streamsize>(s.size()));
        };

        // (6a) 手続き生成のソース (画像を 1 枚も要求しない経路)
        const fs::path procSrc = terrDir / L"proc.terrain.json";
        writeText(procSrc,
                  R"({"type":"terrain","version":1,"worldSize":[64.0,48.0],)"
                  R"("heightRes":[33,17],"splatRes":[16,8],"heightBase":-2.0,"heightScale":10.0,)"
                  R"("procedural":{"seed":7,"octaves":4,"frequency":2.5,"lacunarity":2.0,"gain":0.5},)"
                  R"("layers":[{"name":"a","albedo":"../t.png","tiling":[4.0,4.0],"tint":[0.25,0.5,0.75]},)"
                  R"({"name":"b","normal":"../n.png"},{"name":"c"},{"name":"d"},{"name":"ignored"}]})");
        TerrainData d1;
        check(TerrainAsset::CookFromSource(procSrc.wstring(), d1) && d1.Valid(),
              "terrain: procedural source cooks and validates");
        check(d1.heightW == 33 && d1.heightH == 17 && d1.splatW == 16 && d1.splatH == 8
                  && d1.heights.size() == 33u * 17u && d1.splat.size() == 16u * 8u * 4u,
              "terrain: resolutions and buffer sizes come from the source");
        check(d1.layers.size() == TerrainAsset::kMaxLayers,
              "terrain: layer count is clamped to the splat channel count");
        // M58d: tint は指定があれば読み、無ければ白 (= 恒等 = albedo をそのまま出す)
        check(d1.layers[0].tintR == 0.25f && d1.layers[0].tintG == 0.5f
                  && d1.layers[0].tintB == 0.75f && d1.layers[1].tintR == 1.0f
                  && d1.layers[1].tintG == 1.0f && d1.layers[1].tintB == 1.0f,
              "terrain: layer tint defaults to white and is read when authored");

        // 高さが定数でない = ノイズが本当に走っている (恒等な生成器を PASS にしない)
        bool varies = false;
        for (uint16_t h : d1.heights) {
            varies = varies || h != d1.heights[0];
        }
        check(varies, "terrain: procedural heights are not constant");

        // スプラットの不変量: 1 テクセルの重み合計 = kSplatWeightSum
        bool splatSums = true;
        for (size_t i = 0; i + 3 < d1.splat.size(); i += 4) {
            const uint32_t sum = static_cast<uint32_t>(d1.splat[i]) + d1.splat[i + 1]
                + d1.splat[i + 2] + d1.splat[i + 3];
            splatSums = splatSums && sum == TerrainAsset::kSplatWeightSum;
        }
        check(splatSums, "terrain: every splat texel sums to the normalized weight total");

        // (6b) 形式往復 + クック決定論
        std::vector<uint8_t> blob1;
        TerrainAsset::Serialize(d1, blob1);
        TerrainData back;
        std::vector<uint8_t> blob2;
        const bool rt = TerrainAsset::Deserialize(blob1, back);
        TerrainAsset::Serialize(back, blob2);
        check(rt && blob1 == blob2, "terrain: serialize(deserialize(blob)) round-trips bit-identical");
        check(rt && back.heights == d1.heights && back.splat == d1.splat
                  && back.layers.size() == d1.layers.size() && back.layers[0].name == "a"
                  && back.layers[0].albedo == "../t.png" && back.layers[1].normal == "../n.png"
                  && back.layers[0].tintR == 0.25f && back.layers[0].tintB == 0.75f,
              "terrain: layer table survives the round-trip");

        TerrainData d2;
        std::vector<uint8_t> blob3;
        TerrainAsset::CookFromSource(procSrc.wstring(), d2);
        TerrainAsset::Serialize(d2, blob3);
        check(blob3 == blob1, "terrain: re-cook is bit-identical (cook determinism)");

        // (6c) キャッシュ経路: 1 回目で .mterr が書かれ、2 回目は同じバイトで再生される
        TerrainData l1;
        check(TerrainAsset::Load(procSrc.wstring(), l1), "terrain: cold Load cooks");
        std::vector<uint8_t> payload;
        check(CookedCache::ReadValidated(procSrc.wstring(), TerrainAsset::kTerrainExt, payload)
                  && payload == blob1,
              "terrain: the cook file payload is the same bytes as a fresh cook");
        TerrainData l2;
        check(TerrainAsset::Load(procSrc.wstring(), l2) && l2.heights == l1.heights
                  && l2.splat == l1.splat,
              "terrain: warm Load replays the cached blob");

        // (6d) 画像取り込み (.r16 生ハイトマップ + PNG スプラット)
        const fs::path rawHeight = terrDir / L"h.r16";
        std::vector<uint8_t> rawBytes(9u * 9u * 2u);
        for (size_t i = 0; i < 81; ++i) {
            const uint16_t v = static_cast<uint16_t>(i * 701u);
            rawBytes[i * 2] = static_cast<uint8_t>(v & 0xFF);
            rawBytes[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
        }
        WriteFileBytes(rawHeight, rawBytes);
        std::vector<uint8_t> splatPixels(8u * 8u * 4u);
        for (size_t i = 0; i < 64; ++i) {
            splatPixels[i * 4 + 0] = static_cast<uint8_t>(i * 3);
            splatPixels[i * 4 + 1] = static_cast<uint8_t>(255 - i * 3);
            splatPixels[i * 4 + 2] = 0;
            splatPixels[i * 4 + 3] = 0; // 合計は 255 にならない = 正規化が効くこと自体が試験
        }
        const fs::path splatPng = terrDir / L"s.png";
        check(WritePngRGBA(splatPng.wstring(), splatPixels.data(), 8, 8, 8 * 4),
              "terrain: test splat png written");
        const fs::path imgSrc = terrDir / L"img.terrain.json";
        writeText(imgSrc,
                  R"({"type":"terrain","version":1,"worldSize":[16.0,16.0],"heightRes":[9,9],)"
                  R"("heightmap":"h.r16","splatmap":"s.png","heightScale":8.0,)"
                  R"("layers":[{"name":"only"}]})");
        TerrainData img;
        const bool imgOk = TerrainAsset::CookFromSource(imgSrc.wstring(), img);
        check(imgOk && img.heightW == 9 && img.heightH == 9 && img.splatW == 8 && img.splatH == 8,
              "terrain: image source drives the resolution");
        bool rawMatches = imgOk && img.heights.size() == 81;
        for (size_t i = 0; rawMatches && i < 81; ++i) {
            rawMatches = img.heights[i] == static_cast<uint16_t>(i * 701u);
        }
        check(rawMatches, "terrain: raw .r16 heights are ingested verbatim (little endian)");
        bool imgSplatSums = imgOk;
        for (size_t i = 0; imgOk && i + 3 < img.splat.size(); i += 4) {
            const uint32_t sum = static_cast<uint32_t>(img.splat[i]) + img.splat[i + 1]
                + img.splat[i + 2] + img.splat[i + 3];
            imgSplatSums = imgSplatSums && sum == TerrainAsset::kSplatWeightSum;
        }
        check(imgSplatSums, "terrain: an un-normalized splat png is normalized at cook time");

        // ★焼き込んだ画像だけを差し替える。.terrain.json の stat は動かないので CookedCache は
        //   ヒットしてしまう (deps は存在しか見ない) — TerrainSourceImage の内容ハッシュが
        //   唯一の防波堤で、ここが抜けると「PNG を直したのに絵が変わらない」になる
        TerrainData warm;
        check(TerrainAsset::Load(imgSrc.wstring(), warm) && warm.heights == img.heights,
              "terrain: image-backed cold Load cooks");
        std::vector<uint8_t> tweaked = rawBytes;
        tweaked[0] = static_cast<uint8_t>(tweaked[0] ^ 0xFF); // 同サイズ・別内容
        WriteFileBytes(rawHeight, tweaked);
        TerrainData restale;
        check(TerrainAsset::Load(imgSrc.wstring(), restale)
                  && restale.heights[0] != img.heights[0],
              "terrain: changing only the heightmap image invalidates the cook");
        WriteFileBytes(rawHeight, rawBytes);

        // (6e) 破損 blob の境界検査 — 落ちずに false であること
        TerrainData junkOut;
        check(!TerrainAsset::Deserialize(std::vector<uint8_t>{}, junkOut),
              "terrain corruption: empty blob is rejected");
        check(!TerrainAsset::Deserialize({ 1, 2, 3, 4, 5, 6, 7, 8 }, junkOut),
              "terrain corruption: junk blob is rejected");
        std::vector<uint8_t> badMagic = blob1;
        badMagic[0] = static_cast<uint8_t>(badMagic[0] ^ 0xFF);
        check(!TerrainAsset::Deserialize(badMagic, junkOut),
              "terrain corruption: bad magic is rejected");
        std::vector<uint8_t> tail = blob1;
        tail.push_back(0);
        check(!TerrainAsset::Deserialize(tail, junkOut),
              "terrain corruption: trailing bytes are rejected");
        bool truncSafe = true;
        for (size_t cut = 0; cut < blob1.size(); cut += 13) {
            std::vector<uint8_t> part(blob1.begin(), blob1.begin() + static_cast<ptrdiff_t>(cut));
            truncSafe = truncSafe && !TerrainAsset::Deserialize(part, junkOut);
        }
        check(truncSafe, "terrain corruption: every truncation is rejected without crashing");

        // 巨大な要素数: resize する前に残量で検算していないと bad_alloc で即死する。
        // レイヤ 0 本 / 相対パス空の blob を作ればカウント位置は固定計算できる
        TerrainData tiny;
        tiny.heightW = tiny.heightH = 4;
        tiny.splatW = tiny.splatH = 2;
        tiny.worldSizeX = tiny.worldSizeZ = 10.0f;
        tiny.heightScale = 1.0f;
        tiny.heights.assign(16, 100);
        tiny.splat.assign(2u * 2u * 4u, 0);
        for (size_t i = 0; i < tiny.splat.size(); i += 4) {
            tiny.splat[i] = 255;
        }
        std::vector<uint8_t> tinyBlob;
        TerrainAsset::Serialize(tiny, tinyBlob);
        check(TerrainAsset::Deserialize(tinyBlob, junkOut), "terrain: minimal blob is accepted");
        // magic+version(8) + 解像度 4x u32(16) + float 4 本(16) + src 2 本((4+8+8) x2 = 40)
        // + procedural(20) + layerCount(4) = 104
        constexpr size_t kHeightCountOff = 104;
        uint64_t seen = 0;
        std::memcpy(&seen, tinyBlob.data() + kHeightCountOff, sizeof(seen));
        check(seen == tiny.heights.size(),
              "terrain: blob layout matches what the corruption test patches");
        const uint64_t absurd = 0x00FFFFFFFFFFFFFFull;
        std::memcpy(tinyBlob.data() + kHeightCountOff, &absurd, sizeof(absurd));
        check(!TerrainAsset::Deserialize(tinyBlob, junkOut),
              "terrain corruption: an absurd element count is rejected before allocating");

        // (6f) 重み量子化の純関数検査 (M58d のブレンドがこの不変量に乗る)
        uint8_t q[4] = { 0, 0, 0, 0 };
        const float even[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        TerrainAsset::QuantizeSplatWeights(even, q);
        check(q[0] + q[1] + q[2] + q[3] == 255 && q[0] == 64 && q[3] == 63,
              "terrain: equal weights quantize to 255 with a deterministic remainder");
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        TerrainAsset::QuantizeSplatWeights(zero, q);
        check(q[0] == 255 && q[1] == 0 && q[2] == 0 && q[3] == 0,
              "terrain: an all-zero texel falls back to layer 0 (no divide by zero downstream)");
        const float nan4[4] = { std::numeric_limits<float>::quiet_NaN(), -1.0f, 3.0f, 1.0f };
        TerrainAsset::QuantizeSplatWeights(nan4, q);
        check(q[0] + q[1] + q[2] + q[3] == 255 && q[0] == 0 && q[1] == 0,
              "terrain: NaN / negative weights are dropped and the rest still sums to 255");

        // (6g) リポジトリ同梱のデモ地形が実際に焼けること (--package が同梱する現物)
        const fs::path demo = fs::path(assetsRoot) / L"terrain" / L"demo.terrain.json";
        if (fs::exists(demo, ec)) {
            TerrainData a, b;
            std::vector<uint8_t> ba, bb;
            const bool okA = TerrainAsset::CookFromSource(demo.wstring(), a);
            const bool okB = TerrainAsset::CookFromSource(demo.wstring(), b);
            TerrainAsset::Serialize(a, ba);
            TerrainAsset::Serialize(b, bb);
            check(okA && okB && ba == bb && a.Valid(),
                  "terrain: the bundled demo terrain cooks deterministically");
        } else {
            check(false, "terrain: bundled demo terrain is present");
        }
    }

    // ---- 後始末: 以降のテスト/実行に影響を残さない ----
    CookedCache::Configure(L"", false);
    fs::remove_all(tempRoot, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== CookedCache self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== CookedCache self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
