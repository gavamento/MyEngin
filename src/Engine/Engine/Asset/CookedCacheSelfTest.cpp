#include "Engine/Engine/Asset/CookedCacheSelfTest.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Engine/Asset/ModelCook.h"
#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
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
