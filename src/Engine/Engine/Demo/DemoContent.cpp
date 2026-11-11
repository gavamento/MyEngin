#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Animation/Animation.h"
#include "Engine/Engine/Animation/AnimatorController.h"
#include "Engine/Engine/Audio/Playback/AudioMixer.h"
#include "Engine/Engine/Audio/Playback/AudioSystem.h"
#include "Engine/Engine/Audio/Synth/ImpactSoundAsset.h" // .impact.json (手続き生成サウンド)
#include "Engine/Engine/Audio/Playback/SoundAsset.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Asset/FbxLoader.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Asset/ModelLoader.h"
#include "Engine/Engine/Physics/Fracture/FractureBake.h"    // M80f: --fracture-demo の焼き
#include "Engine/Engine/Physics/Fracture/FractureLibrary.h" // M80f: メモリ上焼きの登録口
#include "Engine/Engine/Physics/Fracture/FractureSkinBake.h" // M80j: スキン破壊の骨割り当て
#include "Engine/Core/Asset/AssetKeyResolver.h" // M94w: 2D デモのスプライトのサブアセットキー
#include "Engine/Engine/Animation/SpriteAnim.h"
#include "Engine/Engine/Physics/Collider/Shape2DLibrary.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Engine/Feel/FeelLibrary.h"
#include "Engine/Engine/Physics/Rigid/PhysMatLibrary.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteShape.h"
#include "Engine/Engine/Navigation/NavFilterLibrary.h"
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {

void RegisterDemoContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    const AssetID white = res.textures.White();
    res.meshes.Cube();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    // 地面はファイルテクスチャ (assets\textures\test.png) — テクスチャホットリロードの実演用
    {
        Material m;
        m.shader = shader;
        const AssetID groundTex =
            res.textures.LoadFile(ctx.assetsRoot + L"\\textures\\test.png", true); // アルベド=sRGB (M38a)
        m.texture = groundTex.IsNull() ? white : groundTex;
        m.baseColor = groundTex.IsNull() ? DirectX::XMFLOAT4{ 0.45f, 0.47f, 0.50f, 1.0f }
                                         : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
        res.materials.Register("mat_ground", m);
    }
    makeMat("mat_arm", 0.85f, 0.55f, 0.20f);
    makeMat("mat_red", 0.85f, 0.30f, 0.28f);
    makeMat("mat_green", 0.35f, 0.75f, 0.40f);
    makeMat("mat_blue", 0.30f, 0.50f, 0.85f);
    makeMat("mat_yellow", 0.90f, 0.80f, 0.30f);
    // モデル (.glb など) はここでは登録しない — RegisterAssetLibraries の起動走査が
    // 全モデルを RegisterAssets でヘッドレス登録する (呼び出し側は常に対)
}

void BuildDemoScene(EngineContext& ctx, float perfRate, int perfMax)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    const AssetID cube = res.meshes.Cube();
    const AssetID matGround = AssetID{ HashStr("mat_ground") };
    const AssetID matArm = AssetID{ HashStr("mat_arm") };
    const AssetID palette[4] = {
        AssetID{ HashStr("mat_red") },
        AssetID{ HashStr("mat_green") },
        AssetID{ HashStr("mat_blue") },
        AssetID{ HashStr("mat_yellow") },
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 7.0f, -16.0f);
    camera.SetLocalRotationEuler(18.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    GameObject ground = s.CreateGameObject("Ground");
    ground.SetLocalPosition(0.0f, -1.0f, 0.0f);
    ground.SetLocalScale(40.0f, 0.5f, 40.0f);
    {
        auto* mr = ground.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = matGround;
    }

    GameObject spinner = s.CreateGameObject("Spinner");
    spinner.SetLocalPosition(0.0f, 1.5f, 0.0f);
    for (int a = 0; a < 20; ++a) {
        char name[32];
        snprintf(name, sizeof(name), "Arm_%02d", a);
        GameObject arm = s.CreateGameObject(name);
        arm.SetParent(spinner);
        arm.SetLocalRotationEuler(0.0f, a * (360.0f / 20.0f), 0.0f);
        arm.SetLocalScale(0.4f, 0.4f, 0.4f);
        {
            auto* mr = arm.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = matArm;
        }
        for (int j = 0; j < 25; ++j) {
            snprintf(name, sizeof(name), "Cube_%02d_%02d", a, j);
            GameObject leaf = s.CreateGameObject(name);
            leaf.SetParent(arm);
            const float dist = (2.0f + 0.9f * j) / 0.4f;
            const float wave = 1.0f + 0.8f * ((j % 5) - 2) * 0.3f;
            leaf.SetLocalPosition(dist, wave, 0.0f);
            leaf.SetLocalScale(0.75f, 0.75f, 0.75f);
            auto* mr = leaf.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = palette[(a + j) % 4];
        }
    }

    GameObject model = ModelLoader::Load(s, res, *ctx.shaders,
                                         ctx.assetsRoot + L"\\models\\BoxTextured.glb");
    if (model) {
        model.SetLocalPosition(0.0f, 1.5f, 0.0f);
        model.SetLocalScale(2.0f, 2.0f, 2.0f);
    }


    // ---- パーティクルエミッタ (M5 デモ: 中央の炎) ----
    GameObject fire = s.CreateGameObject("Fire");
    fire.SetLocalPosition(4.0f, 0.0f, 0.0f);
    auto* emitter = fire.AddComponent<ParticleEmitterComponent>(); // 既定値 = 上向きコーン炎
    if (perfRate > 0.0f) {
        emitter->rate = perfRate;
        emitter->maxParticles = (perfMax > 0) ? perfMax : 100000;
        emitter->lifetimeMin = 1.2f;
        emitter->lifetimeMax = 1.8f;
        emitter->speedMin = 3.0f;
        emitter->speedMax = 8.0f;
        emitter->coneAngleDeg = 60.0f;
    }

    // ---- GameLogic.dll のスクリプトをアタッチ (DLL 未ロードならスキップ) ----
    const ComponentTypeId rotator = ComponentRegistry::Get().FindByName("Rotator");
    if (rotator != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(spinner.Id(), rotator);
    }
    const ComponentTypeId player = ComponentRegistry::Get().FindByName("PlayerController");
    if (player != kInvalidComponentType && model) {
        s.GetWorld().AddComponentRaw(model.Id(), player);
        auto* col = model.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->radius = 1.2f;
        // M7 以来の OnTrigger 用検知球。既定が bool 化でソリッドに変わったため明示
        col->isTrigger = true;
    }
    const ComponentTypeId spawner = ComponentRegistry::Get().FindByName("Spawner");
    if (spawner != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(fire.Id(), spawner);
    }

    // ---- 汎用フィールド ABI (v11、M50d) をリプレイ被覆に入れる ----
    // Health (assets\schemas 由来のスキーマ型) を Spinner に載せ、probe スクリプトが
    // 毎 tick GetComponentField → 減衰 → SetComponentField で書き戻す
    // (詳細は SchemaHealthDemo.cpp)。スキーマ未登録 (ヘッドレス selftest) や DLL 未ロード
    // では対ごとスキップ — Health 単独でも probe 単独でも被覆にならないため 2 つで 1 対
    const ComponentTypeId health = ComponentRegistry::Get().FindByName("Health");
    const ComponentTypeId healthDemo = ComponentRegistry::Get().FindByName("SchemaHealthDemo");
    if (health != kInvalidComponentType && healthDemo != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(spinner.Id(), health);
        s.GetWorld().AddComponentRaw(spinner.Id(), healthDemo);
    }
    // ---- スクリプト間メッセージ (v33、M93e) をリプレイ被覆に入れる ----
    // Spinner に MessageProbe を載せ、7 tick ごとに自分宛てへ送って同じ tick に受け取り、回数を
    // sim 状態へ書く (詳細は MessageProbe.cpp)。DLL 未ロードではスキップ
    const ComponentTypeId messageProbe = ComponentRegistry::Get().FindByName("MessageProbe");
    if (messageProbe != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(spinner.Id(), messageProbe);
    }
}

void RegisterAssetLibraries(EngineContext& ctx)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(ctx.assetsRoot, ec)) {
        return;
    }
    // 起動時アセット走査の所要時間 (M51b のクック効果を含む診断値。sim には無関係)
    const auto scanStart = std::chrono::steady_clock::now();
    std::vector<std::wstring> audioFiles; // 走査後にまとめて判定する (M45f。下の注記参照)
    std::vector<std::wstring> impactFiles; // .impact.json (手続き生成。走査後にまとめて生成)
    for (const auto& e : std::filesystem::recursive_directory_iterator(ctx.assetsRoot, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const std::wstring p = e.path().wstring();
        if (PrefabLibrary::IsComposePath(p)) { // .actor.json / .prefab.json (M48d)
            if (ctx.prefabs) {
                ctx.prefabs->LoadFromFile(p);
            }
        } else if (p.size() >= 10 && p.compare(p.size() - 10, 10, L".anim.json") == 0) {
            if (ctx.anims) {
                ctx.anims->LoadFromFile(p);
            }
        } else if (p.size() >= 9 && p.compare(p.size() - 9, 9, L".mat.json") == 0) {
            if (ctx.resources) {
                ctx.resources->materials.LoadFromFile(p, ctx.resources->textures, ctx.assetsRoot);
            }
        } else if (p.size() >= 16 && p.compare(p.size() - 16, 16, L".controller.json") == 0) {
            if (ctx.controllers) {
                ctx.controllers->LoadFromFile(p); // M22: Animator Controller
            }
        } else if (p.size() >= 11 && p.compare(p.size() - 11, 11, L".sound.json") == 0) {
            if (ctx.sounds) {
                ctx.sounds->LoadFromFile(p); // M45c: サウンドアセット
            }
        } else if (p.size() >= 12 && p.compare(p.size() - 12, 12, L".impact.json") == 0) {
            impactFiles.push_back(p); // ImpactSynth: PCM 生成は走査の後 (順序を名前で固定する)
        } else if (p.size() >= 11 && p.compare(p.size() - 11, 11, L".mixer.json") == 0) {
            if (ctx.mixers) {
                ctx.mixers->LoadFromFile(p); // M45d: ミキサー (適用は走査後にまとめて)
            }
        } else if (p.size() >= 15 && p.compare(p.size() - 15, 15, L".navfilter.json") == 0) {
            // M84c: ナビのエリアのフィルタ。所有は EngineLoop、navfilter:: で注入済み
            if (NavFilterLibrary* nf = navfilter::Library()) {
                nf->LoadFromFile(p);
            }
        } else if (p.size() >= 8 && p.compare(p.size() - 8, 8, L".bt.json") == 0) {
            // M85: ビヘイビアツリー。所有は EngineLoop / HeadlessSim、behaviortree:: で注入済み
            if (BehaviorTreeLibrary* bt = behaviortree::Library()) {
                bt->LoadFromFile(p);
            }
        } else if (p.size() >= 8 && p.compare(p.size() - 8, 8, L".bb.json") == 0) {
            // M85: ブラックボード。同じく blackboard:: で注入済み
            if (BlackboardLibrary* bb = blackboard::Library()) {
                bb->LoadFromFile(p);
            }
        } else if (p.size() >= 13 && p.compare(p.size() - 13, 13, L".shape2d.json") == 0) {
            // M94h: 2D 衝突形状。所有は EngineLoop / HeadlessSim、shape2d:: で注入済み
            if (ShapeLibrary2D* s2 = shape2d::Library()) {
                s2->LoadFromFile(p);
            }
        } else if (p.size() >= 17 && p.compare(p.size() - 17, 17, L".tilepalette.json") == 0) {
            // M94j: タイルパレット。所有は EngineLoop / HeadlessSim、tilepalette:: で注入済み
            if (TilePaletteLibrary* tp = tilepalette::Library()) {
                tp->LoadFromFile(p);
            }
        } else if (p.size() >= 17 && p.compare(p.size() - 17, 17, L".spriteshape.json") == 0) {
            // M94s: スプライトシェイプのプロファイル。spriteshape:: で注入済み
            if (SpriteShapeLibrary* ss = spriteshape::Library()) {
                ss->LoadFromFile(p);
            }
        } else if (p.size() >= 13 && p.compare(p.size() - 13, 13, L".tilemap.json") == 0) {
            // M94j: タイルマップ。tilemap:: で注入済み
            if (TilemapLibrary* tm = tilemap::Library()) {
                tm->LoadFromFile(p);
            }
        } else if (p.size() >= 16 && p.compare(p.size() - 16, 16, L".spriteanim.json") == 0) {
            // M94: スプライト列クリップ。所有は EngineLoop / HeadlessSim、spriteanim:: で注入済み
            if (SpriteAnimLibrary* sa = spriteanim::Library()) {
                sa->LoadFromFile(p);
            }
        } else if (p.size() >= 10 && p.compare(p.size() - 10, 10, L".feel.json") == 0) {
            // M94d: 手触りのプリセット。所有は EngineLoop / HeadlessSim、feellib:: で注入済み
            if (FeelLibrary* fl = feellib::Library()) {
                fl->LoadFromFile(p);
            }
        } else if (p.size() >= 13 && p.compare(p.size() - 13, 13, L".physmat.json") == 0) {
            // M59a1: 物理マテリアル。所有は EngineLoop、ここへは physmat:: で注入済み
            // (EngineContext を汚さない meshcol:: 流儀)
            if (PhysMatLibrary* pm = physmat::Library()) {
                pm->LoadFromFile(p);
            }
        } else {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            // 素の音声ファイルは走査中には展開せず、候補として貯めるだけにする。
            // **.sound.json より先に .wav を見ることがある**ので、「BGM 専用かどうか」は
            // 全部読み終えてからでないと判定できない (M45f)
            if (ext == L".wav" || ext == L".ogg") {
                audioFiles.push_back(p);
            } else if (AssetDatabase::ClassifyPath(p) == AssetType::Texture) {
                // M94: スプライト (.meta の sprites)。GPU テクスチャのロードは参照側が行う
                if (SpriteLibrary* sprites = spritelib::Library()) {
                    sprites->Register(p);
                }
            } else if (ctx.resources != nullptr && ctx.shaders != nullptr
                       && (ext == L".glb" || ext == L".gltf" || ext == L".fbx")) {
                // M50a: メッシュ / マテリアル / スキンを丸ごとヘッドレス登録する。
                // 保存済みシーンをロードする経路 (ModelLoader::Load を通らない) では、ここで
                // 登録しないと MeshRenderer.mesh / .material の実体が誰にも登録されず、モデルが描画されない。Editor / Runtime 共通
                const bool ok = (ext == L".fbx")
                    ? FbxLoader::RegisterAssets(*ctx.resources, *ctx.shaders, p, false)
                    : ModelLoader::RegisterAssets(*ctx.resources, *ctx.shaders, p, false);
                if (ok) {
                    MYE_LOG_INFO("[assets] model assets registered: %s", WideToUtf8(p).c_str());
                }
            }
        }
    }

    // M45c: 素の音声ファイルは PCM を AudioSystem へ展開しつつ、**ファイル名 stem を
    // 名前キーに登録**する (LoadWav = M19 互換シム)。既存スクリプトの
    // PlaySound("beep") がハードコードロード無しで引き続き解決できる経路がこれ。
    // --no-audio / デバイス無しでは LoadClipFile が早期 return するので何も起きない。
    // ★M45f: **stream の .sound.json からしか参照されていないファイルは展開しない** —
    //   数分の BGM を PCM 化すると数十 MB になり、ストリーミングの意味が無くなる。
    //   展開しない = 名前キーも張られないので、その素材は PlaySound(名前) では鳴らない
    //   (BGM を一発ものとして鳴らす経路は元々無いので、失うものは無い)
    for (const std::wstring& p : audioFiles) {
        if (ctx.audio == nullptr) {
            break;
        }
        if (ctx.sounds != nullptr &&
            ctx.sounds->UsageOfClip(AudioSystem::IdForFile(p).value) == ClipUsage::StreamOnly) {
            MYE_LOG_INFO("[audio] stream-only (not preloaded): %s", WideToUtf8(p).c_str());
            continue;
        }
        ctx.audio->LoadWav(WideToUtf8(std::filesystem::path(p).stem().wstring()), p);
    }

    // ImpactSynth (計画 ImpactSoundDesign §18): 手続き生成サウンドは**起動時にここで 1 回だけ**
    // PCM を作って登録する。衝突のたびに合成しない。RegisterClip はデバイス無しでも通るので
    // --no-audio / selftest でも名前キーは解決できる (鳴らないだけ)。
    // ★ディレクトリ反復順に依存しないよう名前順で回す (spec 11.2 規則 7)
    if (ctx.audio != nullptr && ctx.sounds != nullptr && !impactFiles.empty()) {
        std::sort(impactFiles.begin(), impactFiles.end());
        const auto t0 = std::chrono::steady_clock::now();
        int registered = 0;
        for (const std::wstring& p : impactFiles) {
            if (LoadImpactSoundFile(*ctx.audio, *ctx.sounds, p) != 0) {
                ++registered;
            }
        }
        MYE_LOG_INFO("[impact] %d/%d procedural sounds registered in %.1f ms", registered,
                     static_cast<int>(impactFiles.size()),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                         .count());
    }

    // M45d: バスグラフはグローバルに 1 つなので、走査後に「どれを鳴らすか」を 1 本決める。
    // 反復順に依存しないよう PickStartupMixer が名前で選ぶ (default 優先 → 名前順の先頭)。
    // .mixer.json が 1 本も無ければ AudioSystem の既定構成 (Master/BGM/SE/UI) のまま
    if (ctx.mixers != nullptr && ctx.audio != nullptr) {
        const uint64_t hash = ctx.mixers->PickStartupMixer();
        if (const MixerAsset* m = ctx.mixers->Get(hash)) {
            ctx.mixers->SetActive(hash);
            ctx.audio->ApplyMixer(*m);
            MYE_LOG_INFO("[mixer] active mixer: %s (%d buses)", m->name.c_str(),
                         static_cast<int>(m->buses.size()));
        }
    }

    const double scanMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - scanStart)
                              .count();
    MYE_LOG_INFO("[assets] startup asset scan: %.1f ms", scanMs);
}

// ---- 複数のショーケースが共有する小さなヘルパー (宣言は DemoContentInternal.h) ----

void AttachScriptIfRegistered(World& w, EntityID e, const char* name)
{
    const ComponentTypeId t = ComponentRegistry::Get().FindByName(name);
    if (t != kInvalidComponentType) {
        w.AddComponentRaw(e, t);
    }
}

// 名前で .physmat を引く (プリセットは起動走査で登録済み)。
// **絶対パスのハッシュを直接組まない** — チェックアウト先で値が変わるうえ、
// 同伴 .meta の GUID が優先されるので一致しない (M59a1 の申し送り 1)
AssetID FindPhysMat(const char* name)
{
    PhysMatLibrary* lib = physmat::Library();
    if (!lib) {
        return AssetID{};
    }
    for (const PhysMatEntry& e : lib->Enumerate()) { // 名前昇順 = 決定論
        if (e.name == name) {
            return AssetID{ e.hash };
        }
    }
    return AssetID{}; // 未登録なら未割当 = 既存フィールドで動く (デモは壊れない)
}

// ---- M94w: --2d-demo (横スクロール 2D のショーケース) ----
// 2D レーンの全部品を 1 枚に載せる: タイルマップ (背景 / 地面 / 前景の 3 層、ルール・アニメ・一方通行・壊れるブロック) /
// 主人公 (CC plane2D、スプライト列、ZTwoDHero) / 箱 3 個 + ベルト (Effector2D) / kinematic の移動床 / 敵 (GridAgent2D + BT) /
// 丘 (SpriteShape2D) / ロープ (Line2D) / 視差背景 2 枚 / Camera2DFollow + PixelPerfectCamera2D (320x180、PPU 16)。
// night = true で Light2D (Global 暗 + スポット 2 灯 + 影) を足す (--2d-night。昼夜の差は末尾の数エンティティだけ)。
// stress = true で負荷計測用に Sprite2D 2000 枚と 100 x 50 x 3 層のタイルを画面へ詰める (--2d-stress)。
// 資産は assets\2d\ (tools\gen_2d_assets.py が生成)。GUID は同スクリプトの G_* と同じ値。
namespace {

constexpr uint64_t kTwoDTilesGuid = 0x2D00000000000001ull;
constexpr uint64_t kTwoDHeroGuid = 0x2D00000000000002ull;
constexpr uint64_t kTwoDCrateGuid = 0x2D00000000000003ull;
constexpr uint64_t kTwoDBgFarGuid = 0x2D00000000000004ull;
constexpr uint64_t kTwoDBgNearGuid = 0x2D00000000000005ull;
constexpr uint64_t kTwoDHeroNormalGuid = 0x2D00000000000006ull;
constexpr uint64_t kTwoDControllerGuid = 0x2D00000000000012ull;
constexpr uint64_t kTwoDPaletteGuid = 0x2D00000000000013ull;
constexpr uint64_t kTwoDTilemapGuid = 0x2D00000000000014ull;
constexpr uint64_t kTwoDHillProfileGuid = 0x2D00000000000015ull;
constexpr uint64_t kTwoDChaseTreeGuid = 0x2D00000000000016ull;

// tiles.png のスライス番号 (tools\gen_2d_assets.py の tile_names の並び)
constexpr uint32_t kTilePlankSlice = 5;

// SpriteLibrary::SliceId と同じ式。ここは AssetDatabase のキー解決が無くても組めるよう GUID から直接引く
AssetID TwoDSliceId(uint64_t textureGuid, uint32_t sliceNo)
{
    char key[64];
    std::snprintf(key, sizeof(key), "%s%016llx#sprite%u", assetkey::kSubAssetKeyScheme.data(),
                  static_cast<unsigned long long>(textureGuid), sliceNo);
    return AssetID{ HashStr(key) };
}

// 負荷計測 (--2d-stress) の追加分。Sprite2D 2000 枚 + 100 x 50 x 3 層のタイル (計 15000 セル) を、画面 (20 x 11.25 ワールド単位)
// の中へ全部詰める (視錐台で切られない最悪の形)。タイルマップはファイルを作らずメモリ上へ登録する (衝突なし = 物理には載らない)
void AddTwoDStress(Scene& scene)
{
    constexpr int32_t kMapWidth = 100;
    constexpr int32_t kMapHeight = 50;
    constexpr float kStressCellSize = 0.2f; // 100 x 50 セルがちょうど 20 x 10 ワールド単位
    constexpr uint32_t kStressTiles[] = { 4, 6, 4 }; // 壁 / 草 / 壁。層ごとに別のタイルで 3 層を埋める
    constexpr int32_t kSpriteColumns = 50;
    constexpr int32_t kSpriteRows = 40;
    constexpr float kSpriteScale = 0.35f;
    constexpr int32_t kOrderStressBase = -60;

    if (TilemapLibrary* maps = tilemap::Library()) {
        TilemapAsset map;
        map.palette = kTwoDPaletteGuid;
        map.cellSize[0] = kStressCellSize;
        map.cellSize[1] = kStressCellSize;
        for (size_t layerIndex = 0; layerIndex < std::size(kStressTiles); ++layerIndex) {
            TilemapLayer layer;
            layer.name = "Stress";
            layer.orderInLayer = kOrderStressBase + static_cast<int32_t>(layerIndex);
            layer.collision = false;
            for (int32_t y = 0; y < kMapHeight; ++y) {
                for (int32_t x = 0; x < kMapWidth; ++x) {
                    layer.SetCell(x, y, tilecell::Make(kStressTiles[layerIndex], false, false, 0));
                }
            }
            map.layers.push_back(std::move(layer));
        }
        GameObject tiles = scene.CreateGameObject("Stress Tiles");
        tiles.SetLocalPosition(0.5f, 0.5f, 0.0f);
        tiles.AddComponent<TilemapComponent>()->tilemap = AssetID{ maps->Register(L"stress://2d_stress.tilemap.json", std::move(map)) };
    }
    for (int32_t i = 0; i < kSpriteColumns * kSpriteRows; ++i) {
        GameObject sprite = scene.CreateGameObject("Stress Sprite");
        sprite.SetLocalPosition(0.7f + static_cast<float>(i % kSpriteColumns) * 0.4f,
                                0.3f + static_cast<float>(i / kSpriteColumns) * 0.25f, 0.0f);
        sprite.SetLocalScale(kSpriteScale, kSpriteScale, 1.0f);
        auto* sp = sprite.AddComponent<Sprite2DComponent>();
        sp->sprite = AssetID{ kTwoDCrateGuid };
        sp->orderInLayer = kOrderStressBase + 10;
    }
}

} // namespace

void BuildTwoDShowcaseScene(EngineContext& ctx, bool night, bool stress){
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    s.SetName("twod_showcase");

    // 手で決めた配置。単位はワールド (= タイル 1 セル = 16 px)。床の天面が y = 2
    constexpr float kGroundTop = 2.0f;
    constexpr float kCcHalfHeight = 0.9f; // CC の既定の高さ 1.8 の半分
    constexpr float kRefHeightPx = 180.0f;
    constexpr float kPixelsPerUnit = 16.0f;
    constexpr float kOrthoSize = kRefHeightPx / (2.0f * kPixelsPerUnit); // 5.625
    constexpr int32_t kOrderHero = 20;
    constexpr int32_t kOrderEnemy = 18;
    constexpr int32_t kOrderProp = 10;
    constexpr int32_t kOrderRope = 12;

    const AssetID heroSprite = TwoDSliceId(kTwoDHeroGuid, 1);
    const AssetID plankSprite = TwoDSliceId(kTwoDTilesGuid, kTilePlankSlice);

    // ---- カメラ (主カメラ。ピクセルパーフェクト + 追従。色の契約: tonemapMode 0 = 元の画素がそのまま出る) ----
    GameObject camera = s.CreateGameObject("Main Camera");
    camera.SetLocalPosition(10.5f, 5.625f, -10.0f);
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->orthographic = true;
        cam->orthoSize = kOrthoSize;
    }
    {
        auto* fx = camera.AddComponent<CameraPostFxComponent>();
        fx->tonemapMode = 0;
        fx->bloomOn = false;
        fx->fxaaOn = false;
    }
    {
        auto* pp = camera.AddComponent<PixelPerfectCamera2DComponent>();
        pp->assetsPPU = static_cast<int32_t>(kPixelsPerUnit);
        pp->refResolutionX = 320;
        pp->refResolutionY = static_cast<int32_t>(kRefHeightPx);
        pp->upscaleRT = true;
    }
    camera.AddComponent<Camera2DFollowComponent>();

    {
        GameObject envGo = s.CreateGameObject("Environment");
        envGo.AddComponent<PhysicsEnvironmentComponent>();
    }

    // ---- タイルマップ (背景 / 地面 / 前景)。衝突は Ground レイヤー ----
    GameObject level = s.CreateGameObject("Level");
    level.AddComponent<TilemapComponent>()->tilemap = AssetID{ kTwoDTilemapGuid };

    // ---- 主人公 ----
    GameObject hero = s.CreateGameObject("Hero");
    hero.SetLocalPosition(10.5f, 5.0f + kCcHalfHeight + 0.02f, 0.0f); // 一方通行板 (天面 y = 5) の上
    {
        auto* cc = hero.AddComponent<CharacterControllerComponent>();
        cc->plane2D = true;
        cc->pushPower = 3.0f;
    }
    hero.AddComponent<PlayerInputComponent>();
    hero.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ kTwoDControllerGuid };
    AttachScriptIfRegistered(w, hero.Id(), "ZTwoDHero");
    {
        // 足元 (CC の下端) が絵の下端になるよう、pivot が足元のスプライトを CC の半分だけ下げて置く
        GameObject body = s.CreateGameObject("HeroSprite");
        body.SetParent(hero);
        body.SetLocalPosition(0.0f, -kCcHalfHeight, 0.0f);
        auto* sp = body.AddComponent<Sprite2DComponent>();
        sp->sprite = heroSprite;
        sp->normalMap = AssetID{ kTwoDHeroNormalGuid };
        sp->orderInLayer = kOrderHero;
    }
    {
        // 夜の影。主人公の胴の箱 (幅 0.6 x 高さ 1.6)。CC は Box でも Sphere でもないので points で与える
        GameObject caster = s.CreateGameObject("HeroShadow");
        caster.SetParent(hero);
        auto* sc = caster.AddComponent<ShadowCaster2DComponent>();
        sc->source = shadowcaster2dsource::kPoints;
        sc->pointCount = 4;
        sc->points[0] = { -0.3f, -0.8f };
        sc->points[1] = { 0.3f, -0.8f };
        sc->points[2] = { 0.3f, 0.8f };
        sc->points[3] = { -0.3f, 0.8f };
    }

    // ---- 敵 (GridAgent2D + BT の MoveTo で主人公を追う。段差と一方通行板を越えて板へ上がる) ----
    GameObject enemy = s.CreateGameObject("Enemy");
    enemy.SetLocalPosition(17.5f, kGroundTop + kCcHalfHeight + 0.02f, 0.0f);
    {
        auto* cc = enemy.AddComponent<CharacterControllerComponent>();
        cc->plane2D = true;
        cc->pushPower = 3.0f; // 箱の山を押しのけて進む (経路探索は動く物を見ない)
    }
    {
        auto* agent = enemy.AddComponent<GridAgent2DComponent>();
        agent->tilemap = level.Id();
        agent->speed = 3.0f;
        agent->maxJumpHeight = 2;
        agent->maxJumpLength = 3;
        agent->jumpSpeed = 9.0f;
    }
    {
        auto* bt = enemy.AddComponent<BehaviorTreeComponent>();
        bt->tree = AssetID{ kTwoDChaseTreeGuid };
        std::snprintf(bt->bbEntityKey[0], kBtEntityKeyBytes, "%s", "target");
        bt->bbEntityValue[0] = hero.Id();
    }
    enemy.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ kTwoDControllerGuid };
    {
        GameObject body = s.CreateGameObject("EnemySprite");
        body.SetParent(enemy);
        body.SetLocalPosition(0.0f, -kCcHalfHeight, 0.0f);
        auto* sp = body.AddComponent<Sprite2DComponent>();
        sp->sprite = heroSprite;
        sp->color = { 1.0f, 0.55f, 0.5f, 1.0f };
        sp->orderInLayer = kOrderEnemy;
    }

    // ---- 箱 3 個 (plane2D の動的剛体)。1 個はベルトの上、2 個は積んで置く ----
    auto addCrate = [&](const char* name, float x, float y) {
        GameObject crate = s.CreateGameObject(name);
        crate.SetLocalPosition(x, y, 0.0f);
        {
            auto* rb = crate.AddComponent<RigidbodyComponent>();
            rb->plane2D = true;
        }
        {
            auto* col = crate.AddComponent<ColliderComponent>();
            col->shape = collidershape::kBox;
            col->halfExtents = { 0.5f, 0.5f, 0.5f };
        }
        {
            auto* sp = crate.AddComponent<Sprite2DComponent>();
            sp->sprite = AssetID{ kTwoDCrateGuid };
            sp->orderInLayer = kOrderProp;
        }
        crate.AddComponent<ShadowCaster2DComponent>(); // source 0 = 同じエンティティの Collider (Box)
        return crate;
    };
    addCrate("Crate Stack A", 19.5f, kGroundTop + 0.5f);
    addCrate("Crate Stack B", 19.5f, kGroundTop + 1.5f);
    // ベルト (Surface は接触面を動かす。乗せる物は接地状態から始める)。板は厚み 0.25 (CC の段差 0.3 以内)
    constexpr float kBeltHalfThickness = 0.125f;
    constexpr float kBeltHalfLength = 2.0f;
    {
        GameObject belt = s.CreateGameObject("Belt");
        belt.SetLocalPosition(24.0f, kGroundTop + kBeltHalfThickness, 0.0f);
        {
            auto* col = belt.AddComponent<ColliderComponent>();
            col->shape = collidershape::kBox;
            col->halfExtents = { kBeltHalfLength, kBeltHalfThickness, 0.5f };
        }
        {
            auto* eff = belt.AddComponent<Effector2DComponent>();
            eff->type = effector2dtype::kSurface;
            eff->surfaceSpeed = 2.0f;
        }
        for (int i = 0; i < 4; ++i) {
            GameObject plank = s.CreateGameObject("BeltPlank");
            plank.SetParent(belt);
            // 板のタイルは上 6 px だけ描いてある。絵の上端を天面へ揃える (タイル中心 = 天面 - 0.5)
            plank.SetLocalPosition(-kBeltHalfLength + 0.5f + static_cast<float>(i), kBeltHalfThickness - 0.5f, 0.0f);
            auto* sp = plank.AddComponent<Sprite2DComponent>();
            sp->sprite = plankSprite;
            sp->color = { 1.0f, 0.72f, 0.5f, 1.0f };
            sp->orderInLayer = kOrderProp - 1;
        }
    }
    addCrate("Crate Belt", 23.0f, kGroundTop + 2.0f * kBeltHalfThickness + 0.5f);

    // ---- 動く床 (kinematic の plane2D。velocity を ZTwoDMover が書く) ----
    {
        constexpr float kPlatformHalfThickness = 0.1875f; // 板のタイルの絵 (6 px) と同じ厚み
        GameObject platform = s.CreateGameObject("Moving Platform");
        platform.SetLocalPosition(32.0f, 5.5f, 0.0f);
        {
            auto* rb = platform.AddComponent<RigidbodyComponent>();
            rb->isKinematic = true;
            rb->plane2D = true;
            rb->gravityScale = 0.0f;
        }
        {
            auto* col = platform.AddComponent<ColliderComponent>();
            col->shape = collidershape::kBox;
            col->halfExtents = { 1.5f, kPlatformHalfThickness, 0.5f };
        }
        platform.AddComponent<ShadowCaster2DComponent>();
        AttachScriptIfRegistered(w, platform.Id(), "ZTwoDMover");
        for (int i = 0; i < 3; ++i) {
            GameObject plank = s.CreateGameObject("PlatformPlank");
            plank.SetParent(platform);
            plank.SetLocalPosition(-1.0f + static_cast<float>(i), kPlatformHalfThickness - 0.5f, 0.0f);
            auto* sp = plank.AddComponent<Sprite2DComponent>();
            sp->sprite = plankSprite;
            sp->color = { 0.7f, 0.85f, 1.0f, 1.0f };
            sp->orderInLayer = kOrderProp - 1;
        }
    }

    // ---- 丘 (SpriteShape2D。閉じた smooth の輪郭 + 縁 3 + 塗り。衝突あり) ----
    {
        GameObject hill = s.CreateGameObject("Hill");
        hill.SetLocalPosition(3.5f, kGroundTop, 0.0f);
        auto* shape = hill.AddComponent<SpriteShape2DComponent>();
        shape->profile = AssetID{ kTwoDHillProfileGuid };
        shape->closed = true;
        shape->smooth = true;
        shape->collider = true;
        shape->orderInLayer = -5;
        const DirectX::XMFLOAT2 points[] = { { -2.5f, 0.0f }, { -1.5f, 0.7f }, { 0.0f, 1.3f },
                                             { 1.5f, 0.7f },  { 2.5f, 0.0f },  { 0.0f, -0.6f } };
        shape->pointCount = static_cast<int32_t>(std::size(points));
        std::copy(std::begin(points), std::end(points), shape->points);
    }

    // ---- ロープ (Line2D。端を固定した放物線のたるみ) ----
    {
        GameObject rope = s.CreateGameObject("Rope");
        rope.SetLocalPosition(10.5f, 8.6f, 0.0f);
        auto* line = rope.AddComponent<Line2DComponent>();
        constexpr int32_t kRopePoints = 13;
        constexpr float kRopeHalfSpan = 4.0f;
        constexpr float kRopeSag = 1.0f;
        line->pointCount = kRopePoints;
        for (int32_t i = 0; i < kRopePoints; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kRopePoints - 1) * 2.0f - 1.0f; // -1..1
            line->points[i] = { t * kRopeHalfSpan, -kRopeSag * (1.0f - t * t) };
        }
        line->width = 0.12f;
        line->color = { 0.58f, 0.36f, 0.2f, 1.0f };
        line->orderInLayer = kOrderRope;
    }

    // ---- 視差背景 2 枚 (遠景 / 近景。正射影の表示範囲を覆うまで横に繰り返す) ----
    {
        GameObject farBg = s.CreateGameObject("Parallax Far");
        farBg.SetLocalPosition(10.0f, 6.0f, 0.0f);
        {
            auto* sp = farBg.AddComponent<Sprite2DComponent>();
            sp->sprite = AssetID{ kTwoDBgFarGuid };
            sp->orderInLayer = -100;
        }
        {
            auto* pr = farBg.AddComponent<Parallax2DComponent>();
            pr->factor = { 0.15f, 0.15f };
            pr->repeatX = true;
        }
        GameObject nearBg = s.CreateGameObject("Parallax Near");
        nearBg.SetLocalPosition(10.0f, 3.5f, 0.0f);
        {
            auto* sp = nearBg.AddComponent<Sprite2DComponent>();
            sp->sprite = AssetID{ kTwoDBgNearGuid };
            sp->orderInLayer = -90;
        }
        {
            auto* pr = nearBg.AddComponent<Parallax2DComponent>();
            pr->factor = { 0.5f, 0.5f };
            pr->repeatX = true;
        }
    }

    // ---- 夜 (--2d-night): Global 暗 + スポット 2 灯 (吊りランプ / 主人公のランタン)。影は箱・移動床・主人公 ----
    if (night) {
        GameObject global = s.CreateGameObject("Night Global");
        {
            auto* l = global.AddComponent<Light2DComponent>();
            l->type = light2dtype::kGlobal;
            l->color = { 0.16f, 0.2f, 0.42f, 1.0f };
            l->intensity = 1.0f;
        }
        GameObject lamp = s.CreateGameObject("Night Lamp");
        lamp.SetLocalPosition(10.5f, 7.0f, 0.0f);
        {
            auto* l = lamp.AddComponent<Light2DComponent>();
            l->type = light2dtype::kSpot;
            l->color = { 1.0f, 0.82f, 0.5f, 1.0f };
            l->intensity = 1.3f;
            l->innerRadius = 1.0f;
            l->outerRadius = 8.0f;
            l->shadows = true;
        }
        GameObject lantern = s.CreateGameObject("Hero Lantern");
        lantern.SetParent(hero);
        lantern.SetLocalPosition(0.4f, 0.4f, 0.0f);
        {
            auto* l = lantern.AddComponent<Light2DComponent>();
            l->type = light2dtype::kSpot;
            l->color = { 0.6f, 0.85f, 1.0f, 1.0f };
            l->intensity = 1.1f;
            l->innerRadius = 0.5f;
            l->outerRadius = 6.0f;
            l->shadows = true;
        }
    }

    if (stress) {
        AddTwoDStress(s);
    }

    // ---- 配線 ----
    {
        auto* follow = camera.GetComponent<Camera2DFollowComponent>();
        follow->target = hero.Id();
        follow->offset = { 0.0f, 1.0f };
        follow->deadZone = { 1.0f, 0.75f };
        follow->damping = { 0.12f, 0.12f };
        follow->lookAheadX = 1.0f;
        follow->useBounds = true;
        follow->boundsFromTilemap = level.Id();
    }
    w.ApplyStructuralChanges();
}

// ---- M99e: --culling-2d-demo (2D の視錐台カリングの検証) ----
// 正射影カメラ (原点、orthoSize 5 = 高さ 10 / 16:9 で幅約 17.8) の画面内外に、Sprite2D (回転・拡大つきを含む) / Sliced / Tiled /
// Line2D / SpriteShape2D / タイルマップ / Parallax2D / SpriteMask2D を置く。外すと困る物 (画面端に掛かる回転スプライト、
// 両端が画面外の長い線、視差の繰り返し、マスク) と、落ちてよい物 (遠くの同種) を並べる。資産は --2d-demo と同じ
void BuildCulling2DShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    s.SetName("culling_2d_showcase");

    constexpr float kOrthoSize = 5.0f;
    constexpr float kRollDeg = 45.0f;
    const AssetID crate{ kTwoDCrateGuid };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.SetLocalPosition(0.0f, 0.0f, -10.0f);
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->orthographic = true;
        cam->orthoSize = kOrthoSize;
    }
    {
        auto* fx = camera.AddComponent<CameraPostFxComponent>();
        fx->tonemapMode = 0;
        fx->bloomOn = false;
        fx->fxaaOn = false;
    }

    // 色を変えた箱を 1 個置く (位置・拡大・回転・order)。orderInLayer は重なりの順を固定する
    int32_t order = 0;
    const auto addCrate = [&](const char* name, float x, float y, float scaleX, float scaleY, float rollDeg,
                              const DirectX::XMFLOAT4& color) {
        GameObject g = s.CreateGameObject(name);
        g.SetLocalPosition(x, y, 0.0f);
        g.SetLocalScale(scaleX, scaleY, 1.0f);
        g.SetLocalRotationEuler(0.0f, 0.0f, rollDeg);
        auto* sp = g.AddComponent<Sprite2DComponent>();
        sp->sprite = crate;
        sp->color = color;
        sp->orderInLayer = order++;
        return g;
    };

    // ---- 視差背景 (画面の外に置く。factor 0.5 で繰り返しが画面を覆う = 落とさない) ----
    {
        GameObject bg = s.CreateGameObject("Parallax Far");
        bg.SetLocalPosition(40.0f, 3.0f, 0.0f);
        auto* sp = bg.AddComponent<Sprite2DComponent>();
        sp->sprite = AssetID{ kTwoDBgFarGuid };
        sp->orderInLayer = -100;
        auto* pr = bg.AddComponent<Parallax2DComponent>();
        pr->factor = { 0.5f, 0.5f };
        pr->repeatX = true;
    }

    // ---- タイルマップ (チャンク 16 x 16。左の 2 枚が画面に掛かり、右は外) ----
    {
        GameObject level = s.CreateGameObject("Level");
        level.SetLocalPosition(-8.0f, -5.0f, 0.0f);
        level.AddComponent<TilemapComponent>()->tilemap = AssetID{ kTwoDTilemapGuid };
    }

    // ---- Sprite2D: 画面内 / 外 ----
    addCrate("In Center", 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
    addCrate("In Right", 5.0f, 2.0f, 1.0f, 1.0f, 0.0f, { 1.0f, 0.6f, 0.6f, 1.0f });
    addCrate("Out Right", 30.0f, 0.0f, 1.0f, 1.0f, 0.0f, { 0.6f, 1.0f, 0.6f, 1.0f });
    addCrate("Out Top", 0.0f, 14.0f, 1.0f, 1.0f, 0.0f, { 0.6f, 0.6f, 1.0f, 1.0f });
    addCrate("Out Bottom Left", -25.0f, -12.0f, 1.0f, 1.0f, 0.0f, { 1.0f, 1.0f, 0.4f, 1.0f });
    // 回転 45 度・拡大 3 倍 (一辺 3 の菱形)。中心は画面の右の外 (x 10.7) だが、尖った角は画面へ入る。
    // ローカル軸のままの外接矩形 (x 9.2 以上) で判定すると画面の外になって落としてしまう = 回転を見ている証拠
    addCrate("Rotated Edge", 10.7f, 0.0f, 3.0f, 3.0f, kRollDeg, { 1.0f, 0.8f, 0.3f, 1.0f });
    // 同じ菱形をもっと遠くへ (角も画面の外 = 落ちる)
    addCrate("Rotated Far", 14.0f, 0.0f, 3.0f, 3.0f, kRollDeg, { 0.5f, 0.9f, 1.0f, 1.0f });
    // 細長い箱を回して左下の角へ掛ける
    addCrate("Rotated Corner", -9.0f, -4.6f, 4.0f, 1.5f, 30.0f, { 0.8f, 0.5f, 1.0f, 1.0f });

    // ---- Sliced / Tiled ----
    {
        GameObject g = addCrate("Sliced In", 4.0f, -3.0f, 1.0f, 1.0f, 0.0f, { 0.5f, 1.0f, 0.8f, 1.0f });
        auto* sp = g.GetComponent<Sprite2DComponent>();
        sp->drawMode = 1;
        sp->size = { 4.0f, 2.0f };
    }
    {
        GameObject g = addCrate("Sliced Out", 40.0f, 3.0f, 1.0f, 1.0f, 0.0f, { 0.5f, 1.0f, 0.8f, 1.0f });
        auto* sp = g.GetComponent<Sprite2DComponent>();
        sp->drawMode = 1;
        sp->size = { 4.0f, 2.0f };
    }
    {
        GameObject g = addCrate("Tiled Out", -40.0f, 0.0f, 1.0f, 1.0f, 0.0f, { 1.0f, 0.7f, 0.9f, 1.0f });
        auto* sp = g.GetComponent<Sprite2DComponent>();
        sp->drawMode = 2;
        sp->size = { 6.0f, 6.0f };
    }
    // flip + pivot: 画面の端 (左) に半分だけ掛かる
    {
        GameObject g = addCrate("Flipped Edge", -9.2f, 3.5f, 2.0f, 2.0f, 0.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
        auto* sp = g.GetComponent<Sprite2DComponent>();
        sp->flipX = true;
        sp->flipY = true;
    }

    // ---- Line2D ----
    const auto addLine = [&](const char* name, float x, float y, float x0, float y0, float x1, float y1, float width,
                             const DirectX::XMFLOAT4& color) {
        GameObject g = s.CreateGameObject(name);
        g.SetLocalPosition(x, y, 0.0f);
        auto* line = g.AddComponent<Line2DComponent>();
        line->pointCount = 2;
        line->points[0] = { x0, y0 };
        line->points[1] = { x1, y1 };
        line->width = width;
        line->color = color;
        line->orderInLayer = order++;
    };
    addLine("Line In", 0.0f, -3.5f, -3.0f, 0.0f, 3.0f, 0.5f, 0.2f, { 1.0f, 0.4f, 0.2f, 1.0f });
    addLine("Line Out", 0.0f, 20.0f, -3.0f, 0.0f, 3.0f, 0.0f, 0.2f, { 0.2f, 1.0f, 0.4f, 1.0f });
    // 両端が画面の外で、真ん中が画面を横切る (点が 1 つも画面に無い)
    addLine("Line Across", 0.0f, 4.0f, -30.0f, 0.0f, 30.0f, 0.0f, 0.15f, { 0.3f, 0.6f, 1.0f, 1.0f });
    addLine("Line Far Left", -40.0f, 0.0f, -2.0f, 0.0f, 2.0f, 1.0f, 0.1f, { 1.0f, 1.0f, 1.0f, 1.0f });

    // ---- SpriteShape2D (丘) ----
    const auto addHill = [&](const char* name, float x, float y) {
        GameObject hill = s.CreateGameObject(name);
        hill.SetLocalPosition(x, y, 0.0f);
        auto* shape = hill.AddComponent<SpriteShape2DComponent>();
        shape->profile = AssetID{ kTwoDHillProfileGuid };
        shape->closed = true;
        shape->smooth = true;
        shape->orderInLayer = order++;
        const DirectX::XMFLOAT2 points[] = { { -2.5f, 0.0f }, { -1.5f, 0.7f }, { 0.0f, 1.3f },
                                             { 1.5f, 0.7f },  { 2.5f, 0.0f },  { 0.0f, -0.6f } };
        shape->pointCount = static_cast<int32_t>(std::size(points));
        std::copy(std::begin(points), std::end(points), shape->points);
    };
    addHill("Hill In", -3.0f, 1.5f);
    addHill("Hill Edge", 9.5f, -2.0f); // 画面の右端に半分掛かる
    addHill("Hill Out", 60.0f, 0.0f);

    // ---- SpriteMask2D (マスクは落とさない。画面の外のマスクも描画リストへ載る) ----
    {
        GameObject mask = s.CreateGameObject("Mask In");
        mask.SetLocalPosition(-6.0f, -1.0f, 0.0f);
        mask.SetLocalScale(2.0f, 2.0f, 1.0f);
        auto* mk = mask.AddComponent<SpriteMask2DComponent>();
        mk->sprite = crate;
        mk->orderInLayer = order++;
        GameObject masked = addCrate("Masked In", -6.0f, -1.0f, 4.0f, 4.0f, 0.0f, { 1.0f, 0.3f, 0.3f, 1.0f });
        masked.GetComponent<Sprite2DComponent>()->maskInteraction = 1; // VisibleInside = マスクの内側だけ
    }
    {
        GameObject mask = s.CreateGameObject("Mask Out");
        mask.SetLocalPosition(35.0f, 5.0f, 0.0f);
        auto* mk = mask.AddComponent<SpriteMask2DComponent>();
        mk->sprite = crate;
        mk->orderInLayer = order++;
        GameObject masked = addCrate("Masked Out", 35.0f, 5.0f, 4.0f, 4.0f, 0.0f, { 1.0f, 0.3f, 0.3f, 1.0f });
        masked.GetComponent<Sprite2DComponent>()->maskInteraction = 1;
    }

    w.ApplyStructuralChanges();
}

} // namespace mye
