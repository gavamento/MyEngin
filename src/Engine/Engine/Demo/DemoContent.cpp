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
#include "Engine/Engine/Physics/Rigid/PhysMatLibrary.h"
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
    // ---- スクリプト間メッセージ (v30、M93e) をリプレイ被覆に入れる ----
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

} // namespace mye
