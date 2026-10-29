// 描画系ショーケース (--rt-demo / --render-demo / --terrain-demo / --render-bench-demo / --render-bench-unique-demo)。
// DemoContent.cpp から領域ごとの翻訳単位へ分けたもの (M93g)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <array>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Core/Asset/AssetKeyResolver.h" // render_bench: LOD 付きモデルのメッシュキー
#include "Engine/Engine/Asset/ModelLoader.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/RayTracing/RtTypes.h" // kRtReflClass* (M67: デモの反射クラス)
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {

void RegisterRtShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();
    res.meshes.Sphere();

    // マテリアルはこのシーン専用 (既定デモの mat_* とは名前を分ける)。
    // 二次ヒットのシェーディングはマテリアル定数のみ = テクスチャ非対応 (M46 v1 制限) なので、
    // ショーケースは意図的に全て無地にしてある — ラスタとレイトレで同じ色が出る
    auto makeMat = [&](const char* name, float r, float g, float b, float metallic,
                       float roughness, float emissive) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        m.metallic = metallic;
        m.roughness = roughness;
        m.emissiveIntensity = emissive;
        return res.materials.Register(name, m);
    };
    makeMat("rt_white", 0.80f, 0.79f, 0.76f, 0.0f, 0.85f, 0.0f);
    makeMat("rt_red", 0.75f, 0.15f, 0.12f, 0.0f, 0.85f, 0.0f);
    makeMat("rt_green", 0.18f, 0.65f, 0.20f, 0.0f, 0.85f, 0.0f);
    // 発光パネル。ラスタでは gbMaterial.b 経由でこの面自体が明るく描かれ、
    // レイトレでは RtMaterial.emissive としてバウンス先の光源になる (同じ強度から両方が出る)
    makeMat("rt_lamp", 1.00f, 0.95f, 0.85f, 0.0f, 1.0f, 6.0f);
    makeMat("rt_mirror", 0.95f, 0.93f, 0.88f, 1.0f, 0.05f, 0.0f);
    // 粗さは 0.18 にしてある: roughness 0.3〜0.6 の帯は 1spp の GGX ローブが
    // 小さな発光パネルを引く分散が大きく、v1 のデノイザではファイアフライが残るため (M46h v1 制限)
    makeMat("rt_brushed", 0.72f, 0.45f, 0.20f, 1.0f, 0.18f, 0.0f);
}

void BuildRtShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    RegisterRtShowcaseContent(ctx); // 単独で呼ばれても実体が揃うようにしておく
    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();
    const AssetID matWhite = AssetID{ HashStr("rt_white") };
    const AssetID matRed = AssetID{ HashStr("rt_red") };
    const AssetID matGreen = AssetID{ HashStr("rt_green") };
    const AssetID matLamp = AssetID{ HashStr("rt_lamp") };
    const AssetID matMirror = AssetID{ HashStr("rt_mirror") };
    const AssetID matBrushed = AssetID{ HashStr("rt_brushed") };

    auto box = [&](const char* name, float px, float py, float pz, float sx, float sy, float sz,
                   AssetID mat, float yawDeg = 0.0f) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(sx, sy, sz);
        if (yawDeg != 0.0f) {
            go.SetLocalRotationEuler(0.0f, yawDeg, 0.0f);
        }
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = mat;
        return go;
    };
    auto ball = [&](const char* name, float px, float py, float pz, float diameter, AssetID mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(diameter, diameter, diameter); // Sphere() は半径 0.5 = 直径 1
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = mat;
        return go;
    };

    // カメラは箱の内側に置く (前面の壁はカメラの背後なので視界を塞がない)。
    // 箱を閉じておくことで、拡散 GI のバウンスが外へ逃げずコーネル箱らしい色移りが出る
    GameObject camera = s.CreateGameObject("Main Camera");
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->fovYDeg = 55.0f; // 既定 60 はやや広角。天井の発光パネルが画に入る下限
    }
    camera.SetLocalPosition(0.0f, 4.2f, -4.3f);
    camera.SetLocalRotationEuler(4.0f, 0.0f, 0.0f); // わずかに見下ろして床を画に入れる

    // ---- 箱 (内寸 10 x 8 x 14、壁厚 0.4)。奥行きを取ってあるのは、
    //      被写体をカメラから離して画角に収めるため ----
    box("Floor", 0.0f, -0.2f, 2.0f, 10.8f, 0.4f, 14.4f, matWhite);
    box("Ceiling", 0.0f, 8.2f, 2.0f, 10.8f, 0.4f, 14.4f, matWhite);
    box("WallBack", 0.0f, 4.0f, 9.2f, 10.8f, 8.8f, 0.4f, matWhite);
    box("WallFront", 0.0f, 4.0f, -5.2f, 10.8f, 8.8f, 0.4f, matWhite); // カメラの背後
    box("WallLeft", -5.2f, 4.0f, 2.0f, 0.4f, 8.8f, 14.4f, matRed);
    box("WallRight", 5.2f, 4.0f, 2.0f, 0.4f, 8.8f, 14.4f, matGreen);

    // ---- 天井の面光源 (このシーン唯一の光源) ----
    box("LightPanel", 0.0f, 7.85f, 2.5f, 4.5f, 0.15f, 5.0f, matLamp);

    // ---- 被写体 ----
    ball("MirrorSphere", -2.1f, 1.5f, 4.2f, 3.0f, matMirror);  // 鏡面 = 箱が映り込む
    ball("BrushedSphere", 2.8f, 1.1f, 1.8f, 2.2f, matBrushed); // 金属 = 引き締まった映り込み
    box("TallBox", 2.2f, 2.2f, 6.2f, 2.2f, 4.4f, 2.2f, matWhite, 20.0f);
    box("ShortBox", -3.0f, 0.9f, 1.4f, 1.8f, 1.8f, 1.8f, matWhite, -18.0f);

    // 2 個目の発光体は **ファイルマテリアル** (.mat.json の "emissive" フィールド) から引く。
    // これで「JSON → MaterialLibrary → ラスタ + レイトレ」の経路がショーケースを起動するだけで
    // 通る (エディタで .mat.json を書き換えればホットリロードで光り方が変わるのも確認できる)。
    // ファイルが無ければ静かに省略する — このシーンの成立に必須ではない
    {
        const AssetID matFile = MaterialLibrary::HashForPath(
            ctx.assetsRoot + L"\\materials\\demo_emissive.mat.json");
        if (res.materials.Get(matFile) != nullptr) {
            box("EmissiveBar", -4.6f, 3.4f, 6.4f, 0.5f, 2.6f, 0.5f, matFile);
        }
    }
}

void RegisterRenderShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();
    res.meshes.Sphere();

    // 名前は **rdemo_ 接頭辞**。既定デモ (mat_*) / RT (rt_*) / parts / flow / mp_ / duel_ の
    // どれとも衝突させない — 材質は全ショーケース分が無条件登録されるので、名前が被ると
    // 「後勝ちで別のシーンの色に化ける」が静かに起きる
    // M67: 末尾の reflClass は「この材質の面が反射に**映るとき**の再利用の厳しさ」。
    // 省略時は 4 (中立) = 従来と同じ扱いなので、既存の呼び出しは書き換えない
    auto makeMat = [&](const char* name, float r, float g, float b, float metallic,
                       float roughness, float emissive,
                       int reflClass = kRtReflClassDefault) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        m.metallic = metallic;
        m.roughness = roughness;
        m.emissiveIntensity = emissive;
        m.reflectionClass = reflClass;
        return res.materials.Register(name, m);
    };
    makeMat("rdemo_ground", 0.34f, 0.35f, 0.38f, 0.0f, 0.90f, 0.0f);
    // 柱は小物 (Prop) — 反射像の中で最も面積を占めるので、ここが積極再利用で崩れないかが
    // ReSTIR の A/B の主戦場になる
    makeMat("rdemo_pillar_a", 0.72f, 0.70f, 0.66f, 0.0f, 0.75f, 0.0f, kRtReflClassProp);
    makeMat("rdemo_pillar_b", 0.60f, 0.34f, 0.28f, 0.0f, 0.65f, 0.0f, kRtReflClassProp);
    makeMat("rdemo_pillar_c", 0.28f, 0.42f, 0.56f, 0.0f, 0.55f, 0.0f, kRtReflClassProp);
    // 反射床パッチ。M56d (SSR) と M56f (反射プローブ) の被写体そのものなので、
    // **粗さは 0.1 以下**にしておく (SSR は粗さでフェードするため、粗いと画に出ない)
    makeMat("rdemo_mirror", 0.85f, 0.87f, 0.90f, 0.90f, 0.10f, 0.0f);
    // 回転体は唯一の動く被写体 = 主役 (Hero)。ゴーストが出るなら真っ先にここに出る
    makeMat("rdemo_spin", 0.95f, 0.58f, 0.14f, 0.0f, 0.45f, 0.0f, kRtReflClassHero);
    makeMat("rdemo_far", 0.42f, 0.45f, 0.52f, 0.0f, 0.85f, 0.0f);
    // ライト位置の目印。**発光だけ**でライティングには寄与しない (エンジンに面光源は無い) —
    // 「どこにスポット/点光源があるか」が絵の上で分かると M54c/M54d の A/B が読みやすい
    makeMat("rdemo_lamp_warm", 1.00f, 0.82f, 0.55f, 0.0f, 1.0f, 5.0f);
    makeMat("rdemo_lamp_cool", 0.70f, 0.85f, 1.00f, 0.0f, 1.0f, 5.0f);
}

void BuildRenderShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    RegisterRenderShowcaseContent(ctx); // 単独で呼ばれても実体が揃うようにしておく
    s.SetName("render_showcase");
    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();
    const AssetID matGround = AssetID{ HashStr("rdemo_ground") };
    const AssetID matMirror = AssetID{ HashStr("rdemo_mirror") };
    const AssetID matSpin = AssetID{ HashStr("rdemo_spin") };
    const AssetID matFar = AssetID{ HashStr("rdemo_far") };
    const AssetID pillarMats[3] = {
        AssetID{ HashStr("rdemo_pillar_a") },
        AssetID{ HashStr("rdemo_pillar_b") },
        AssetID{ HashStr("rdemo_pillar_c") },
    };

    auto box = [&](const char* name, float px, float py, float pz, float sx, float sy, float sz,
                   AssetID mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = mat;
        return go;
    };
    auto ball = [&](const char* name, float px, float py, float pz, float diameter, AssetID mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(diameter, diameter, diameter); // Sphere() は半径 0.5 = 直径 1
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = mat;
        return go;
    };

    // ---- カメラ ----
    // shot_verify は 960x540 (16:9) で撮る。この位置/画角で「柱の群れ + 反射パッチ +
    // 地平線 + 遠景」が 1 枚に収まる。俯角 14° は「床を広く見せつつ空も残す」下限
    GameObject camera = s.CreateGameObject("Main Camera");
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->fovYDeg = 55.0f;
    }
    camera.SetLocalPosition(0.0f, 8.0f, -22.0f);
    camera.SetLocalRotationEuler(13.0f, 0.0f, 0.0f);

    // ---- 太陽 (平行光) ----
    // **必ず残す** — CSM (M54c 以降が拡張する既存のシャドウパス) の被覆がここでしか取れない。
    // 強度は既定の 1.0 から落としてある: 平行光が支配的だと局所ライトの寄与が絵に出ず、
    // M54c/M54d の A/B が「差が見えない」で終わってしまう。アンビエントも同じ理由で低め
    // (アンビエントは **最初のライト**のものが全体に使われるので、Sun を最初に作る)
    GameObject sun = s.CreateGameObject("Sun");
    {
        auto* l = sun.AddComponent<LightComponent>();
        l->intensity = 0.85f;
        l->color = { 1.00f, 0.96f, 0.88f };
        l->ambient = { 0.13f, 0.14f, 0.17f };
    }
    sun.SetLocalRotationEuler(46.0f, -34.0f, 0.0f);

    // ---- 床 200x200 ----
    // 大スケールにしてあるのは CSM のカスケード分割 (カメラフィット、kShadowMaxDist=60) を
    // 実際に働かせるため。M58 の地形もこのスケール感になる
    box("Ground", 0.0f, -0.5f, 0.0f, 200.0f, 1.0f, 200.0f, matGround);

    // ---- 反射床パッチ (M56d SSR / M56f 反射プローブの被写体) ----
    // 床とほぼ同じ高さに薄く敷く。柱がこの上に立つので「柱が映り込む」絵になる
    box("MirrorPatch", 0.0f, 0.05f, 8.0f, 18.0f, 0.1f, 12.0f, matMirror);

    // ---- 柱 20 本 ----
    // ★座標は**固定表**。rand() はもちろん、trig を使った「それらしいばらつき」も入れない
    //   (規則 2)。並びの意図は 3 つ:
    //     ・スポット 2 本の光錐に入る柱を作る            (M54c の透視シャドウが画に出る)
    //     ・点光源 2 個を囲む柱を作る                    (M54d のキューブ 6 面の影が交差する)
    //     ・反射パッチの上/縁に立つ柱を作る              (M56d の SSR が画に出る)
    struct Pillar {
        float x;
        float z;
        float height;
        float width;
        int mat;
    };
    static const Pillar kPillars[] = {
        { -16.0f, -4.0f, 3.5f, 1.6f, 0 }, { -9.0f, -6.0f, 2.4f, 1.2f, 1 },
        { -3.0f, -5.0f, 4.5f, 1.4f, 2 },  { 4.0f, -6.0f, 3.0f, 1.8f, 0 },
        { 11.0f, -4.0f, 5.0f, 1.4f, 1 },  { 18.0f, -2.0f, 2.8f, 1.6f, 2 },
        { -19.0f, 4.0f, 4.2f, 1.5f, 1 },  { -12.0f, 3.0f, 6.0f, 1.3f, 2 },
        { -6.0f, 6.0f, 3.2f, 1.7f, 0 },   { 0.0f, 3.0f, 2.2f, 2.2f, 1 },
        { 7.0f, 5.0f, 5.5f, 1.4f, 2 },    { 14.0f, 7.0f, 3.6f, 1.6f, 0 },
        { -15.0f, 12.0f, 5.2f, 1.5f, 2 }, { -8.0f, 14.0f, 3.0f, 1.3f, 0 },
        { -1.0f, 12.0f, 6.5f, 1.5f, 1 },  { 6.0f, 15.0f, 4.0f, 1.8f, 2 },
        { 13.0f, 13.0f, 2.6f, 1.4f, 1 },  { -11.0f, 22.0f, 7.0f, 1.6f, 0 },
        { 2.0f, 24.0f, 5.0f, 1.5f, 2 },   { 10.0f, 21.0f, 6.2f, 1.4f, 1 },
    };
    for (int i = 0; i < static_cast<int>(std::size(kPillars)); ++i) {
        const Pillar& p = kPillars[i];
        char name[32];
        std::snprintf(name, sizeof(name), "Pillar_%02d", i);
        box(name, p.x, p.height * 0.5f, p.z, p.width, p.height, p.width, pillarMats[p.mat]);
    }

    // ---- スポットライト 2 本 (M54c: 透視 1 面のシャドウ) ----
    // 光の向きはエンティティのローカル +Z (RenderSystem がワールド行列の第 3 行を使う)。
    // **柱を斜めから照らす**向きに置いてある — 真上から当てると影が柱の足元に潰れて、
    // シャドウが入ったかどうかが絵で判別できない
    auto spot = [&](const char* name, float px, float py, float pz, float pitchDeg, float yawDeg,
                    float r, float g, float b, float intensity, AssetID markerMat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalRotationEuler(pitchDeg, yawDeg, 0.0f);
        auto* l = go.AddComponent<LightComponent>();
        l->type = lighttype::kSpot;
        l->color = { r, g, b };
        l->intensity = intensity;
        l->range = 42.0f;
        l->spotInnerDeg = 18.0f;
        l->spotOuterDeg = 30.0f;
        // M54e: 局所影を立てる (局所影の golden 被覆)
        l->castShadow = 1;
        // 目印 (発光する小球)。ライトエンティティの子にすると位置が自動で追従する
        GameObject marker = s.CreateGameObject((std::string(name) + "_Marker").c_str());
        marker.SetParent(go);
        marker.SetLocalScale(0.5f, 0.5f, 0.5f);
        auto* mr = marker.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = markerMat;
        return go;
    };
    spot("SpotLeft", -12.0f, 10.0f, 4.0f, 44.0f, 34.0f, 1.00f, 0.88f, 0.70f, 9.0f,
         AssetID{ HashStr("rdemo_lamp_warm") });
    spot("SpotRight", 12.0f, 10.0f, 6.0f, 44.0f, -34.0f, 0.72f, 0.86f, 1.00f, 9.0f,
         AssetID{ HashStr("rdemo_lamp_cool") });

    // ---- 点光源 2 個 (M54d: キューブ 6 面のシャドウ) ----
    // **柱の間**に低く置く。全方位に影を落とすので、周囲の柱の影が床の上で交差する =
    // 6 面ぶんのアトラスが正しく張られたかが 1 枚の絵で分かる
    auto point = [&](const char* name, float px, float py, float pz, float r, float g, float b,
                     float intensity, float range, AssetID markerMat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        auto* l = go.AddComponent<LightComponent>();
        l->type = lighttype::kPoint;
        l->color = { r, g, b };
        l->intensity = intensity;
        l->range = range;
        l->castShadow = 1; // M54e: 点光源はキューブ 6 面 (M54d) をアトラスへ焼く
        GameObject marker = s.CreateGameObject((std::string(name) + "_Marker").c_str());
        marker.SetParent(go);
        marker.SetLocalScale(0.4f, 0.4f, 0.4f);
        auto* mr = marker.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = markerMat;
        return go;
    };
    point("PointWarm", -5.0f, 2.6f, 6.0f, 1.00f, 0.55f, 0.22f, 6.0f, 18.0f,
          AssetID{ HashStr("rdemo_lamp_warm") });
    point("PointCool", 5.5f, 2.6f, 13.0f, 0.35f, 0.65f, 1.00f, 6.0f, 18.0f,
          AssetID{ HashStr("rdemo_lamp_cool") });

    // ---- デカール 2 枚 (M56a / M56b) ----
    // ★**golden に載せるために置いている。** 1 枚も置かないと「デカールが壊れても
    //   demo_render_deferred は緑のまま」= 回帰の被覆がゼロになる (M54a でこのシーンを
    //   作った理由そのもの)。置いた代償として demo_render_deferred / demo_render_taa の
    //   2 枚が M56a で動く (demo_render_forward が**動かない**ことが v1 の Forward 非対応の証明)。
    // ★albedo テクスチャは付けない (null = 白 = color がそのまま出る)。AssetRef はシーン
    //   JSON へ 64bit の AssetID をそのまま書く仕様で、テクスチャの AssetID は正規化絶対
    //   パスのハッシュ = チェックアウト依存になる (M51j で踏んだ穴)。**このシーンは
    //   コードから毎回組み直す生成物 (cache\render_showcase.scene.json は gitignore) なので
    //   それでも構わない** — 実際 M56b では法線マップをファイルから読んでいる。
    // 向きは **ローカル +Z が投影方向** (LightComponent と同じ規約)。
    auto decal = [&](const char* name, float px, float py, float pz, float pitchDeg, float yawDeg,
                     float sx, float sy, float sz, float r, float g, float b, float a,
                     float angleFadeDeg, int32_t sortOrder) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalRotationEuler(pitchDeg, yawDeg, 0.0f);
        go.SetLocalScale(sx, sy, sz);
        auto* d = go.AddComponent<DecalComponent>();
        d->color = { r, g, b, a };
        d->angleFadeDeg = angleFadeDeg;
        d->sortOrder = sortOrder;
        return go;
    };
    // 1 枚目: 床へ真下投影 (pitch 90 = ローカル +Z が世界の -Y)。箱は
    // x ∈ [-12,-4] / z ∈ [-9,-3] / y ∈ [-1,3] で、Pillar_01 (x=-9 z=-6 高さ 2.4) を
    // すっぽり含む位置にしてある。**床と柱の天面には乗り、柱の側面には乗らない**
    // (側面は法線が投影方向と直角 = 角度フェード 0) — スクリーンスペースの貼り絵ではなく
    // 面へ投影していることが 1 枚の絵で分かる
    GameObject decalGround = decal("DecalGround", -8.0f, 1.0f, -6.0f, 90.0f, 0.0f, 8.0f, 6.0f,
                                   4.0f, 0.95f, 0.42f, 0.12f, 0.85f, 90.0f, 0);
    // ---- M56b: この 1 枚だけ法線 / roughness も上描きする ----
    // ★**2 枚のうち 1 枚だけ**にしてあるのが要点。DecalPillar は強度 0 のまま =
    //   「M56b を足しても albedo だけのデカールは 1 ビットも動かない」が同じ 1 枚の絵で
    //   検証できる (img-diff の差分が DecalGround の footprint に閉じるはず)。
    // ★法線マップは既にリポジトリに居る assets\textures\demo_normal.png (256^2、
    //   平坦 128,128,255 に凹凸)。**リニアで読む** (法線マップに sRGB を掛けると凹凸が歪む)。
    //   平坦な床と柱の天面に貼るので、法線を書き換えないと絵が 1 画素も変わらない
    //   (投影方向 = -Y = 受け面の法線そのものなので、法線マップ無しの「平坦なデカール」は
    //   定義上なにもしない) — つまり**このテクスチャが無いと M56b の被覆が作れない**。
    //   assetsRoot にファイルが無い環境では null が返り、平坦 = 恒等に落ちる。
    if (auto* dg = decalGround.GetComponent<DecalComponent>()) {
        dg->normalTex = res.textures.LoadFile(ctx.assetsRoot + L"\\textures\\demo_normal.png",
                                              /*srgb=*/false);
        dg->normalStrength = 1.0f;
        // 濡れた路面のように鏡面を立てる。床 (rdemo_ground) の粗さは 0.90 なので
        // 0.15 との差はスポット 2 本と点光源 2 個のハイライトとしてはっきり出る
        dg->roughness = 0.15f;
        dg->roughnessStrength = 1.0f;
    }
    // 2 枚目: 柱の側面へ水平投影 (回転なし = ローカル +Z が世界の +Z = カメラの奥向き)。
    // Pillar_03 (x=4, z=-6, 幅 1.8, 高さ 3.0) の手前の面だけを覆う箱で、床 (y=0) は
    // 箱の下端 y=0.5 の外なので掛からない = 「箱の外は捨てる」が角度フェードとは
    // 独立に効いていることの目印になる
    decal("DecalPillar", 4.0f, 1.5f, -5.0f, 0.0f, 0.0f, 2.5f, 2.0f, 4.0f,
          0.20f, 0.75f, 0.95f, 0.90f, 70.0f, 1);

    // ---- 反射プローブ 1 個 (M56f) ----
    // ★**置いても絵は 1 ビットも変わらない** — ベイクは常に明示指示 (`--probe-bake-all` /
    //   `View > Rendering > すべての反射プローブをベイク`) なので、golden はプローブを
    //   足しても不変のまま。デカール (M56a) が golden 2 枚を動かしたのとはここが違う。
    // 箱は反射パッチ MirrorPatch (中心 (0,0.05,8) / 18 x 12) をちょうど覆う大きさで、
    // 撮影点は床から 3 の高さ = 柱が水平 4 面にしっかり写る位置。**視差補正 (ボックス投影)
    // が効いているかは、床の映り込みが箱の壁で切り替わるかで読める**
    {
        GameObject probe = s.CreateGameObject("ReflectionProbe");
        probe.SetLocalPosition(0.0f, 3.0f, 8.0f);
        auto* rp = probe.AddComponent<ReflectionProbeComponent>();
        rp->extents = { 11.0f, 6.0f, 8.0f };
        rp->blendDistance = 1.5f;
    }

    // ---- 回転する物体 (M55c の velocity / M55e のモーションブラー) ----
    // 既定デモの Spinner (DemoContent.cpp の Rotator) と同じ流儀。腕を付けてあるのは
    // 「回っていること」が静止画 1 枚でも分かるようにするため
    GameObject spinner = s.CreateGameObject("Spinner");
    spinner.SetLocalPosition(0.0f, 3.4f, -4.0f);
    for (int a = 0; a < 3; ++a) {
        char name[32];
        std::snprintf(name, sizeof(name), "SpinArm_%d", a);
        GameObject arm = s.CreateGameObject(name);
        arm.SetParent(spinner);
        arm.SetLocalRotationEuler(0.0f, a * 120.0f, 0.0f);
        GameObject blade = s.CreateGameObject((std::string(name) + "_Blade").c_str());
        blade.SetParent(arm);
        blade.SetLocalPosition(0.0f, 0.0f, 2.4f);
        blade.SetLocalScale(0.5f, 0.5f, 3.2f);
        auto* mr = blade.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = matSpin;
    }
    AttachScriptIfRegistered(s.GetWorld(), spinner.Id(), "Rotator");

    // ---- 遠景 (DoF / フォグ / M58e の LOD) ----
    // CSM の届く範囲 (kShadowMaxDist=60) の外に置いてある。影が付かない距離帯が
    // 画にあること自体が M54 の「どこまで影が出るか」の目視材料になる
    box("Far_0", -30.0f, 6.0f, 60.0f, 12.0f, 12.0f, 8.0f, matFar);
    box("Far_1", 25.0f, 8.0f, 72.0f, 16.0f, 16.0f, 10.0f, matFar);
    box("Far_2", 0.0f, 10.0f, 88.0f, 20.0f, 20.0f, 14.0f, matFar);
    box("Far_3", -55.0f, 7.0f, 78.0f, 14.0f, 14.0f, 12.0f, matFar);
    box("Far_4", 48.0f, 5.0f, 55.0f, 10.0f, 10.0f, 7.0f, matFar);
    ball("FarDome", -38.0f, 3.0f, 44.0f, 6.0f, matFar);

    // ---- 高さフォグ (M57d のフロクセルが置き換える対象) ----
    // 色は既定の clearColor (0.08,0.09,0.11) 近傍にしてある — 背景にはフォグが掛からない
    // (ApplyFog はマテリアルシェーダの中) ので、離れた色を選ぶと地平線に段が出る
    GameObject fog = s.CreateGameObject("Fog");
    {
        auto* f = fog.AddComponent<FogComponent>();
        f->mode = 2; // Exp2
        f->color = { 0.11f, 0.13f, 0.17f, 1.0f };
        f->density = 0.011f;
        f->heightFalloff = 0.035f; // 高いところほど薄い = 柱の頭が抜けて見える
        f->baseHeight = 0.0f;
        f->inscatterIntensity = 0.30f; // 太陽方向にだけ僅かに明るい
        f->inscatterPower = 10.0f;
    }

    s.GetWorld().ApplyStructuralChanges();

    // RT の適用範囲は既定 OFF なので、描画物すべてを明示的に ON にする
    // (golden の demo_render_rtrefl / rtgi / rtrefl_restir がこのシーンの RT を撮る)。
    // ★RT レーンが off の撮影には効かない (RenderSystem は RT off のフレームで判定しない)
    {
        World& w = s.GetWorld();
        std::vector<EntityID> meshes;
        const std::array<ComponentTypeId, 1> req = { MeshRendererComponent::sTypeId };
        w.ForEachArchetype(req, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                meshes.push_back(arch.EntityAt(row));
            }
        });
        for (EntityID e : meshes) {
            if (auto* rt = w.AddComponent<RayTracingComponent>(e)) {
                rt->inScene = kRtScopeOn;
                rt->receiver = kRtScopeOn;
            }
        }
        w.ApplyStructuralChanges();
    }
}

void RegisterTerrainShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();

    // 名前は **tdemo_ 接頭辞** (rdemo_ / mat_ / rt_ / parts / flow / mp_ / duel_ と衝突させない —
    // 材質は全ショーケース分が無条件登録されるので、被ると後勝ちで別の色に化ける)
    Material m;
    m.shader = shader;
    m.texture = white;
    m.baseColor = { 0.86f, 0.36f, 0.20f, 1.0f };
    m.metallic = 0.0f;
    m.roughness = 0.55f;
    // 参照用の柱。**地形の起伏に対する縦のスケール**を絵の中に置くためだけの存在で、
    // 「地形と通常メッシュが同じ深度バッファを共有しているか」を 1 枚の絵で見分ける材料になる
    // (地形だけだと前後関係のバグが画に出ない)
    res.materials.Register("tdemo_marker", m);
}

void BuildTerrainShowcaseScene(EngineContext& ctx, float lodDistance, float skirtDepth)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    RegisterTerrainShowcaseContent(ctx); // 単独で呼ばれても実体が揃うようにしておく
    s.SetName("terrain_showcase");
    const AssetID cube = res.meshes.Cube();
    const AssetID matMarker = AssetID{ HashStr("tdemo_marker") };

    // ---- カメラ ----
    // 地形は 256x256 m / 高さ -4..+32 m (assets\terrain\demo.terrain.json)。
    // 960x540 (16:9) の shot_verify で「起伏の陰影 + 地平線 + 空」が入る位置に置く。
    //
    // ★**手前の縁 (z=-128) を画面の下に追い出す**位置合わせが要る。地形は「面」であって
    //   「塊」ではないので、視線が縁の高さより下を通ると**裏面カリングで地形が消え、
    //   画面の下端に空色の帯が出る** (最初の試写で実際に出た。バグにしか見えない)。
    //   条件は atan((camY - 地形の最大高さ) / 縁までの距離) > 俯角 + 縦画角の半分:
    //   camY=50 / 縁まで 14 m / 最大高さ 32 m → 52° > 17 + 27.5 = 44.5° (余裕 7.5°)。
    //   カメラを引くほどこの余裕が減る — 引きたくなったら高さも一緒に上げること
    GameObject camera = s.CreateGameObject("Main Camera");
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->fovYDeg = 55.0f;
        cam->farZ = 800.0f; // 奥の縁 (約 270 m 先) まで確実に入る
    }
    camera.SetLocalPosition(0.0f, 50.0f, -142.0f);
    camera.SetLocalRotationEuler(17.0f, 0.0f, 0.0f);

    // ---- 太陽 (平行光) ----
    // 斜めから当てて尾根と谷にコントラストを付ける。真上だと起伏が飛んで
    // 「法線が正しいか」が絵で判別できなくなる (M58b の継ぎ目検査の目視版)
    GameObject sun = s.CreateGameObject("Sun");
    {
        auto* l = sun.AddComponent<LightComponent>();
        l->intensity = 1.15f;
        l->color = { 1.00f, 0.95f, 0.86f };
        l->ambient = { 0.16f, 0.18f, 0.22f };
    }
    sun.SetLocalRotationEuler(38.0f, -28.0f, 0.0f);

    // ---- 空 (グラデーション) ----
    // 地形の輪郭が空との境界で出る = 「チャンクが 1 枚欠けた」が絵で分かる
    GameObject sky = s.CreateGameObject("Sky");
    {
        auto* sk = sky.AddComponent<SkyboxComponent>();
        sk->topColor = { 0.20f, 0.38f, 0.72f, 1.0f };
        sk->horizonColor = { 0.72f, 0.80f, 0.88f, 1.0f };
        sk->bottomColor = { 0.30f, 0.28f, 0.24f, 1.0f };
    }

    // ---- 地形 ----
    // source は **assets\ 相対**で持つ (M58b: AssetID にすると Inspector の参照ピッカーが
    // 引くべきランタイムライブラリが無い。相対パスならシーン JSON がチェックアウト先に
    // 依存しない = M51j の絶対パスハッシュ問題も踏まない)
    GameObject terrain = s.CreateGameObject("Terrain");
    {
        auto* t = terrain.AddComponent<TerrainComponent>();
        std::snprintf(t->source, sizeof(t->source), "terrain/demo.terrain.json");
        t->chunkTiles = 32; // 128 タイル / 32 = 4x4 = 16 チャンク
        // M58e: 既定 0 = LOD 無効。--terrain-lod のときだけ点く。
        // ★このカメラ (y=50 / z=-142 / 俯角 17°) だとチャンク行の viewZ はおおよそ
        //   57 / 118 / 179 / 241 m。lodDistance=80 で 3 段すべてが 1 枚の絵に入る =
        //   LOD 境界が 2 本できるので、クラックが出るなら必ず映る
        t->lodDistance = lodDistance;
        t->skirtDepth = skirtDepth;
    }

    // ---- 参照用の柱 ----
    // 手前側に等間隔で並べる。y は地形の高さを問い合わせずに固定値で置いてある —
    // 高さ問い合わせ API は M59 (地形コリジョン) の話で、今は ECS にも ABI にも無い。
    // 柱の足元が地面に埋まるか浮くかは golden の主張ではない (縦の目盛りとしてだけ使う)
    static const float kMarkerX[] = { -60.0f, -20.0f, 20.0f, 60.0f };
    for (int i = 0; i < static_cast<int>(std::size(kMarkerX)); ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Marker_%d", i);
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(kMarkerX[i], 16.0f, -70.0f);
        go.SetLocalScale(3.0f, 32.0f, 3.0f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = matMarker;
    }

    s.GetWorld().ApplyStructuralChanges();
}

void BuildRenderBenchScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("render_bench");

    // .meta の GUID (assets\anims\anim_test.controller.json.meta)。--anim-demo と同じコントローラ
    constexpr uint64_t kAnimTestControllerGuid = 0xb44c659f992d4a83ull;

    const AssetID shader = AssetID{ HashStr("forward_lit") };
    auto makeMaterial = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = res.textures.White();
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    const AssetID gridMats[3] = {
        makeMaterial("rbench_grid_a", 0.80f, 0.45f, 0.35f),
        makeMaterial("rbench_grid_b", 0.40f, 0.70f, 0.45f),
        makeMaterial("rbench_grid_c", 0.40f, 0.50f, 0.85f),
    };
    const AssetID matGround = makeMaterial("rbench_ground", 0.32f, 0.33f, 0.36f);
    const AssetID matWall = makeMaterial("rbench_wall", 0.75f, 0.75f, 0.72f);
    const AssetID matTower = makeMaterial("rbench_tower", 0.55f, 0.35f, 0.30f);
    const AssetID matFar = makeMaterial("rbench_far", 0.60f, 0.65f, 0.80f);

    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();
    // メッシュ LOD の検証用に、高ポリの球を .meta で 3 段にしたモデルを使う (assets\models\lod_sphere.glb.meta)。
    // 読めなければ組込みの球で代用する (LOD の統計だけが出なくなる)
    AssetID lodSphere = sphere;
    {
        const std::wstring lodPath = ctx.assetsRoot + L"\\models\\lod_sphere.glb";
        if (ModelLoader::RegisterAssets(res, *ctx.shaders, lodPath, /*logErrors=*/true)) {
            lodSphere = AssetID{ HashStr(assetkey::SubAssetKeyPrefix(lodPath) + "#mesh0#prim0") };
        } else {
            MYE_LOG_ERROR("[render-bench] lod_sphere.glb could not be loaded (run tools\\gen_lod_test_gltf.ps1)");
        }
    }
    auto place = [&](const char* name, AssetID mesh, AssetID mat, float px, float py, float pz, float sx, float sy,
                     float sz) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = mesh;
        mr->material = mat;
        return go;
    };

    // ---- カメラ (固定。+Z 向き) ----
    // 後ろ (-Z) に視錐台の外のキャスター (Tower) を置くので、遠い側を広く取るために farZ を伸ばす
    GameObject camera = s.CreateGameObject("Main Camera");
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->fovYDeg = 60.0f;
        cam->farZ = 400.0f;
    }
    camera.SetLocalPosition(0.0f, 7.0f, -14.0f);
    camera.SetLocalRotationEuler(10.0f, 0.0f, 0.0f);

    // ---- 太陽 ----
    // 光は +Z 向きに 50 度で落ちる。Tower (カメラの背後) の影が画面の手前の床に伸びる
    GameObject sun = s.CreateGameObject("Sun");
    {
        auto* l = sun.AddComponent<LightComponent>();
        l->intensity = 0.9f;
        l->ambient = { 0.15f, 0.16f, 0.19f };
    }
    sun.SetLocalRotationEuler(50.0f, 0.0f, 0.0f);

    place("Ground", cube, matGround, 0.0f, -0.5f, 100.0f, 500.0f, 1.0f, 500.0f);

    // ---- 遮蔽する壁 ----
    // カメラ (y=7) から見て、壁の背後の中央の円錐にあるグリッドの物が隠れる
    place("Wall", cube, matWall, 0.0f, 8.0f, 14.0f, 36.0f, 16.0f, 1.5f);

    // ---- 視錐台の外の影キャスター ----
    // カメラの背後にあり、本体は画面に入らない。影だけが手前の床へ落ちる (影のカリング用)
    place("Tower", cube, matTower, 0.0f, 40.0f, -48.0f, 10.0f, 80.0f, 10.0f);

    // ---- 不透明グリッド 61 x 60 = 3660 個 ----
    // ★座標・高さ・材質はすべて添字の整数式 (乱数も三角関数も使わない = 毎回同じ並び)
    constexpr int kGridX = 61;
    constexpr int kGridZ = 60;
    constexpr float kGridStep = 3.0f;
    for (int iz = 0; iz < kGridZ; ++iz) {
        for (int ix = 0; ix < kGridX; ++ix) {
            const float height = 1.0f + static_cast<float>((ix * 7 + iz * 13) % 5) * 0.5f;
            const float x = (static_cast<float>(ix) - 30.0f) * kGridStep;
            const float z = 22.0f + static_cast<float>(iz) * kGridStep;
            char name[32];
            std::snprintf(name, sizeof(name), "Bench_%02d_%02d", ix, iz);
            const bool isCube = ((ix + iz) % 2) == 0;
            place(name, isCube ? cube : sphere, gridMats[(ix * 3 + iz) % 3], x, height * 0.5f, z, 1.6f, height,
                  1.6f);
        }
    }

    // ---- 遠景 (大きな球) ----
    for (int i = 0; i < 8; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "Far_%d", i);
        place(name, lodSphere, matFar, static_cast<float>(i - 4) * 40.0f + 20.0f, 15.0f, 300.0f, 30.0f, 30.0f,
              30.0f);
    }

    // ---- スキンのキャラ ----
    // 近景 2 / 遠景 2 (壁の円錐の外) / 壁の裏 2 / 画面外 (カメラの背後) 1
    struct ActorSpec {
        float x;
        float z;
    };
    const ActorSpec actors[] = {
        { -2.0f, 0.0f },  { 2.5f, 4.0f },    // 近景
        { 55.0f, 60.0f }, { -70.0f, 120.0f }, // 遠景
        { -6.0f, 30.0f }, { 8.0f, 40.0f },    // 壁の裏
        { 3.0f, -30.0f },                     // カメラの背後 (画面外)
    };
    int actorIndex = 0;
    for (const ActorSpec& a : actors) {
        GameObject actor = ModelLoader::Load(s, res, *ctx.shaders, ctx.assetsRoot + L"\\models\\anim_test.glb");
        if (!actor) {
            MYE_LOG_ERROR("[render-bench] anim_test.glb could not be loaded (run tools\\gen_anim_test_gltf.ps1)");
            break;
        }
        actor.SetLocalPosition(a.x, 0.0f, a.z);
        actor.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ kAnimTestControllerGuid };
        ++actorIndex;
    }
    // ---- LOD の見本 ----
    // 半径 3 の球を、カメラから 14 / 34 / 43 / 65 m に左右 1 組ずつ置く (壁 x = ±18 の外側)。
    // 画面に占める大きさで、近景は LOD0、遠ざかるにつれ LOD1 → LOD2 → LOD3 になる
    // (切り替えの境目から余白を取ってある。ヒステリシスの帯に入ると前フレームの段に留まるため)
    const ActorSpec lodSpheres[] = {
        { -6.0f, -2.0f },  { 6.0f, -2.0f },  // LOD0
        { -24.0f, 10.0f }, { 24.0f, 10.0f }, // LOD1
        { -24.0f, 22.0f }, { 24.0f, 22.0f }, // LOD2
        { -24.0f, 46.0f }, { 24.0f, 46.0f }, // LOD3
    };
    int lodIndex = 0;
    for (const ActorSpec& a : lodSpheres) {
        char name[16];
        std::snprintf(name, sizeof(name), "Lod_%02d", lodIndex);
        place(name, lodSphere, gridMats[lodIndex % 3], a.x, 3.0f, a.z, 6.0f, 6.0f, 6.0f);
        ++lodIndex;
    }
    MYE_LOG_INFO("[render-bench] %d grid objects, %d skinned actors", kGridX * kGridZ, actorIndex);
}

void BuildRenderBenchUniqueScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("render_bench_unique");

    constexpr uint64_t kAnimTestControllerGuid = 0xb44c659f992d4a83ull; // BuildRenderBenchScene と同じ
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    const AssetID cube = res.meshes.Cube();
    const Mesh* cubeMesh = res.meshes.Get(cube);
    if (cubeMesh == nullptr) {
        return;
    }

    // ---- カメラ・太陽・床・壁は render_bench と同じ配置。遠景のキャラを映すため farZ を伸ばす ----
    GameObject camera = s.CreateGameObject("Main Camera");
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->fovYDeg = 60.0f;
        cam->farZ = 1500.0f;
    }
    camera.SetLocalPosition(0.0f, 7.0f, -14.0f);
    camera.SetLocalRotationEuler(10.0f, 0.0f, 0.0f);
    GameObject sun = s.CreateGameObject("Sun");
    {
        auto* l = sun.AddComponent<LightComponent>();
        l->intensity = 0.9f;
        l->ambient = { 0.15f, 0.16f, 0.19f };
    }
    sun.SetLocalRotationEuler(50.0f, 0.0f, 0.0f);

    auto makeMaterial = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = res.textures.White();
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    auto place = [&](const char* name, AssetID mesh, AssetID mat, float px, float py, float pz, float sx, float sy,
                     float sz) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = mesh;
        mr->material = mat;
        return go;
    };
    place("Ground", cube, makeMaterial("rbu_ground", 0.32f, 0.33f, 0.36f), 0.0f, -0.5f, 100.0f, 500.0f, 1.0f, 500.0f);
    place("Wall", cube, makeMaterial("rbu_wall", 0.75f, 0.75f, 0.72f), 0.0f, 8.0f, 14.0f, 36.0f, 16.0f, 1.5f);

    // ---- メッシュも材質もすべて別のグリッド 50 x 30 = 1500 個 ----
    // 同じメッシュ・材質が 1 つも無いのでインスタンシングの run が全部 1 個になり、GPU オクルージョンのフェーズ 2 が
    // run ごとに状態設定と CB 更新を出し直す最悪の形になる (CPU の提出コストの計測用)。
    // メッシュは立方体の頂点を添字ごとの倍率で変えた複製。座標・色は添字の整数式のみ (毎回同じ)
    constexpr int kGridX = 50;
    constexpr int kGridZ = 30;
    constexpr float kGridStep = 3.0f;
    std::vector<MeshVertex> vertices(cubeMesh->positions.size());
    for (int iz = 0; iz < kGridZ; ++iz) {
        for (int ix = 0; ix < kGridX; ++ix) {
            const int index = iz * kGridX + ix;
            const float scale = 1.0f + static_cast<float>(index % 7) * 0.02f;
            for (size_t v = 0; v < vertices.size(); ++v) {
                vertices[v].position = { cubeMesh->positions[v].x * scale, cubeMesh->positions[v].y * scale,
                                         cubeMesh->positions[v].z * scale };
                vertices[v].normal = cubeMesh->normals[v];
                vertices[v].uv = cubeMesh->uvs[v];
            }
            char name[32];
            std::snprintf(name, sizeof(name), "rbu_mesh_%04d", index);
            const AssetID mesh = res.meshes.Register(name, vertices, cubeMesh->indices);
            std::snprintf(name, sizeof(name), "rbu_mat_%04d", index);
            const AssetID mat = makeMaterial(name, 0.3f + static_cast<float>(index % 5) * 0.15f,
                                             0.3f + static_cast<float>(index % 7) * 0.1f,
                                             0.3f + static_cast<float>(index % 3) * 0.25f);
            const float height = 1.0f + static_cast<float>((ix * 7 + iz * 13) % 5) * 0.5f;
            std::snprintf(name, sizeof(name), "BenchU_%02d_%02d", ix, iz);
            place(name, mesh, mat, (static_cast<float>(ix) - 24.5f) * kGridStep, height * 0.5f,
                  22.0f + static_cast<float>(iz) * kGridStep, 1.6f, height, 1.6f);
        }
    }

    // ---- スキンのキャラ (URO の確認用): 近景 1 体と、画面で次第に小さくなる遠景 ----
    // 画面での大きさの目安 (外接球の高さ比): z=2 で約 1、z=100 で約 0.07、200 で 0.035、400 で 0.017、800 で 0.009、1000 で 0.007
    const float actorZ[] = { 2.0f, 100.0f, 200.0f, 400.0f, 800.0f, 1000.0f };
    int actorIndex = 0;
    for (const float z : actorZ) {
        GameObject actor = ModelLoader::Load(s, res, *ctx.shaders, ctx.assetsRoot + L"\\models\\anim_test.glb");
        if (!actor) {
            MYE_LOG_ERROR("[render-bench] anim_test.glb could not be loaded (run tools\\gen_anim_test_gltf.ps1)");
            break;
        }
        actor.SetLocalPosition(actorIndex == 0 ? 2.5f : -30.0f + 12.0f * static_cast<float>(actorIndex), 0.0f, z);
        actor.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ kAnimTestControllerGuid };
        ++actorIndex;
    }
    MYE_LOG_INFO("[render-bench-unique] %d unique mesh/material objects, %d skinned actors", kGridX * kGridZ,
                 actorIndex);
}

} // namespace mye
