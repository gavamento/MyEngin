// ゲームフロー・入力レーン・ネット対戦のショーケース (--flow-demo / --net-demo / --local-demo)。
// DemoContent.cpp から領域ごとの翻訳単位へ分けたもの (M93g)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/UI/UILayout.h" // M75a: 旧 anchor/x/y/w/h → RectTransform
#include "Engine/Engine/UI/UILayoutGroup.h" // M75e: --ui-demo の自動レイアウト
#include "Engine/Platform/Input.h"            // M75f: UiDemoScriptInput
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"

namespace mye {

void RegisterFlowShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();
    res.meshes.Sphere();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("flow_floor", 0.30f, 0.33f, 0.40f);
    makeMat("flow_wall", 0.20f, 0.22f, 0.28f);
    makeMat("flow_ball", 1.00f, 0.55f, 0.10f);
    makeMat("flow_paddle", 0.25f, 0.70f, 0.95f);
    makeMat("flow_deco", 0.60f, 0.35f, 0.85f);
}

namespace {

// flow シーン共通の骨格 (カメラ/太陽/床)。名前キーのマテリアルと builtin メッシュのみ =
// シーンファイルがチェックアウト非依存になる (モデル由来のサブアセット ID は使わない)
void BuildFlowStage(Scene& s, RenderResources& res, bool withColliders)
{
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 6.0f, -14.0f);
    camera.SetLocalRotationEuler(20.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.25f, 0.0f);
    floor.SetLocalScale(16.0f, 0.5f, 16.0f);
    {
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("flow_floor") };
        if (withColliders) {
            auto* col = floor.AddComponent<ColliderComponent>();
            col->shape = collidershape::kBox;
            col->halfExtents = { 0.5f, 0.5f, 0.5f }; // ローカル半径 (スケールは Collider 側で掛かる)
            col->isTrigger = false;
        }
    }
}

// UI テキスト要素 (kind=1)。矩形左上がアンカー点 + オフセットに置かれる (M51e 意味論)。
// ★数値の単位は**キャンバス単位 (基準 1920x1080)** (M70b)。実 px ではない —
//   960x540 の画面では canvasScale がちょうど 0.5 になる
GameObject MakeUiText(Scene& s, const char* name, int anchor, float x, float y, float w, float h,
                      const char* text, float fontScale, int align)
{
    GameObject go = s.CreateGameObject(name);
    // M75a: 配置は RectTransform (旧 9-grid 引数を同じ式で変換)。**UIElement より先に**足す —
    // AddComponent はアーキタイプを移すので、後から足すと ui のポインタが無効になる
    *go.AddComponent<RectTransformComponent>() =
        uilayout::FromLegacyRect(anchor, x, y, w, h, 0, /*hasUiAncestor*/ false);
    auto* ui = go.AddComponent<UIElementComponent>();
    ui->kind = 1;
    ui->fontScale = fontScale;
    ui->align = align;
    std::snprintf(ui->text, sizeof(ui->text), "%s", text);
    return go;
}

// UI ボタン要素 (kind=2、M70c)。**focusable=1 が既定** — パッド/キーのフォーカスナビの
// 候補になるのはこれだけで、押下判定とハイライトはエンジン (UIInteraction) が持つ
GameObject MakeUiButton(Scene& s, const char* name, int anchor, float x, float y, float w,
                        float h, const char* label, float fontScale)
{
    GameObject go = s.CreateGameObject(name);
    *go.AddComponent<RectTransformComponent>() =
        uilayout::FromLegacyRect(anchor, x, y, w, h, 0, /*hasUiAncestor*/ false);
    auto* ui = go.AddComponent<UIElementComponent>();
    ui->kind = 2;
    ui->fontScale = fontScale;
    ui->focusable = 1;
    ui->color = { 0.22f, 0.27f, 0.38f, 1.0f }; // CreateMenu の生成ボタンと同じ配色
    std::snprintf(ui->text, sizeof(ui->text), "%s", label);
    return go;
}

// タイトル/リザルト画面: 装飾 + UI + FlowTitleDriver (C++) + FlowMenu (C#、演出レーン)
void BuildFlowTitleScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("flow_title");
    BuildFlowStage(s, res, /*withColliders=*/false);
    const AssetID cube = res.meshes.Cube();

    // 回転する装飾キューブ (Rotator があれば回る — 無くてもシーンは成立する)
    for (int i = 0; i < 3; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Deco_%d", i);
        GameObject deco = s.CreateGameObject(name);
        deco.SetLocalPosition(-3.0f + 3.0f * i, 1.2f, 2.0f);
        deco.SetLocalRotationEuler(0.0f, 20.0f * i, 0.0f);
        deco.SetLocalScale(1.4f, 1.4f, 1.4f);
        auto* mr = deco.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("flow_deco") };
        AttachScriptIfRegistered(s.GetWorld(), deco.Id(), "Rotator");
    }

    MakeUiText(s, "TitleText", 1, -800.0f, 240.0f, 1600.0f, 160.0f, "MyEngine FLOW DEMO", 6.0f,
               4);
    // ★anchor 7 (下中央) に置く。anchor 4 + y=240 だと真下の TitleBest と**文字が重なる**
    GameObject hint = MakeUiText(s, "TitleHint", 7, -600.0f, -170.0f, 1200.0f, 80.0f,
                                 "D-Pad / Arrows : MOVE   A / Enter : SELECT", 2.4f, 4);
    AttachScriptIfRegistered(s.GetWorld(), hint.Id(), "FlowMenu"); // C# 点滅 (別レーン)
    MakeUiText(s, "TitleBest", 7, -600.0f, -280.0f, 1200.0f, 80.0f, "BEST 0   LAST 0   RUNS 0",
               2.8f, 4);

    // ---- 操作できるメニュー (M70c) ----
    // **エンジンが持つ UI 対話状態の唯一の replay 被覆**。ボタンを 2 個縦に並べるのは、
    // 1 個だと uinav::FindNext が「移動先が無い」で毎回同じ答えを返し、フォーカス移動の
    // 配線が検査にならないため。判定と描画のハイライトは両方エンジン側 (FlowTitleDriver は
    // MyeUIClicked を読むだけ)
    // ★**横に並べる**のは合成入力の都合でもある: SynthLaneInput の D-Pad 分布は
    //   600 tick で Left 9 / Right 5 / Up 12 / Down 2 で、縦に積むと「上端でさらに上」
    //   ばかりになりフォーカスが 1 度も動かない (実測)。横なら Left/Right の 14 回が
    //   そのまま移動になる = 被覆が実際に成立する
    MakeUiButton(s, "TitleStart", 4, -560.0f, 20.0f, 520.0f, 110.0f, "START", 3.0f);
    MakeUiButton(s, "TitleClearBest", 4, 40.0f, 20.0f, 520.0f, 110.0f, "CLEAR BEST", 3.0f);

    GameObject director = s.CreateGameObject("FlowDirector");
    AttachScriptIfRegistered(s.GetWorld(), director.Id(), "FlowTitleDriver");
    s.GetWorld().ApplyStructuralChanges();
}

// ゲーム画面: 弾む球 (Rigidbody + FlowGameDriver) + パドル + HUD
void BuildFlowGameScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("flow_game");
    BuildFlowStage(s, res, /*withColliders=*/true);
    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();

    // 球を囲う壁 (静的コライダー)。転がり出てもデモが破綻しないように
    auto wall = [&](const char* name, float px, float pz, float sx, float sz) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, 1.0f, pz);
        go.SetLocalScale(sx, 2.0f, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("flow_wall") };
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->isTrigger = false;
    };
    wall("WallN", 0.0f, 8.25f, 16.0f, 0.5f);
    wall("WallS", 0.0f, -8.25f, 16.0f, 0.5f);
    wall("WallE", 8.25f, 0.0f, 0.5f, 16.0f);
    wall("WallW", -8.25f, 0.0f, 0.5f, 16.0f);

    // パドル (MoveX 軸で動かせる静的コライダー。リプレイでは軸 0 = 不動)
    GameObject paddle = s.CreateGameObject("Player");
    paddle.SetLocalPosition(0.0f, 0.45f, 0.0f);
    paddle.SetLocalScale(2.4f, 0.4f, 2.4f);
    {
        auto* mr = paddle.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("flow_paddle") };
        auto* col = paddle.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->isTrigger = false;
    }

    // 弾む球 = このシーンの sim 本体。FlowGameDriver がフロー全体を駆動する
    GameObject ball = s.CreateGameObject("Ball");
    ball.SetLocalPosition(1.3f, 5.0f, 0.7f);
    {
        auto* mr = ball.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("flow_ball") };
        auto* col = ball.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->radius = 0.5f;
        col->isTrigger = false;
        auto* rb = ball.AddComponent<RigidbodyComponent>();
        rb->restitution = 0.82f;
    }
    AttachScriptIfRegistered(s.GetWorld(), ball.Id(), "FlowGameDriver");

    MakeUiText(s, "GameScore", 0, 80.0f, 60.0f, 800.0f, 100.0f, "SCORE 0", 4.0f, 0);
    {
        GameObject bar = s.CreateGameObject("GameScoreBar");
        *bar.AddComponent<RectTransformComponent>() =
            uilayout::FromLegacyRect(0, 80.0f, 190.0f, 800.0f, 44.0f, 0, false);
        auto* ui = bar.AddComponent<UIElementComponent>();
        ui->kind = 0;
        ui->color = { 0.30f, 0.85f, 0.45f, 0.9f };
        ui->fillMode = 1; // 水平バー (FlowGameDriver が SetUIFill で書く)
        ui->fillAmount = 0.0f;
    }
    // PAUSED 表示はアルファ 0 で常設し、ドライバが SetUIColor で出し入れする
    GameObject paused = MakeUiText(s, "GamePause", 4, -600.0f, -80.0f, 1200.0f, 160.0f, "PAUSED",
                                   8.0f, 4);
    if (auto* ui = paused.GetComponent<UIElementComponent>()) {
        ui->color = { 1.0f, 0.85f, 0.30f, 0.0f };
    }
    MakeUiText(s, "GameHint", 6, 80.0f, -120.0f, 1800.0f, 80.0f,
               "A/D : MOVE   Esc/Start : PAUSE   J : LOAD SAVE", 2.4f, 0);
    s.GetWorld().ApplyStructuralChanges();
}

} // namespace

void EnsureFlowShowcaseScenes(EngineContext& ctx)
{
    RegisterFlowShowcaseContent(ctx);
    const std::wstring dir = ctx.assetsRoot + L"\\scenes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // ctx.scene に組んで保存 → Clear、を欠けているファイルの分だけ繰り返す。
    // 呼び出し時点の ctx.scene は空 (OnStart のシーンロード前) が前提
    auto ensure = [&](const wchar_t* file, void (*build)(EngineContext&)) {
        const std::wstring path = dir + L"\\" + file;
        if (std::filesystem::exists(path, ec)) {
            return;
        }
        build(ctx);
        if (SceneSerializer::SaveToFile(*ctx.scene, path)) {
            MYE_LOG_INFO("[flow] scene generated: %s", WideToUtf8(path).c_str());
        } else {
            MYE_LOG_ERROR("[flow] scene write failed: %s", WideToUtf8(path).c_str());
        }
        ctx.scene->Clear();
        ctx.scene->GetWorld().ApplyStructuralChanges();
    };
    ensure(L"flow_game.scene.json", &BuildFlowGameScene);
    ensure(L"flow_title.scene.json", &BuildFlowTitleScene);
}

void BuildNetDuelScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("net_duel");
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 9.0f, -12.0f);
    camera.SetLocalRotationEuler(34.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(52.0f, -28.0f, 0.0f);

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.25f, 0.0f);
    floor.SetLocalScale(20.0f, 0.5f, 14.0f);
    {
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("duel_floor") };
    }
    // 得点リング。**当たり判定は持たない** — 判定はスクリプト側の距離計算だけで完結させる
    // (物理を挟むとデモの読み筋が「ネットの話」から離れる)。ここは目印
    GameObject ring = s.CreateGameObject("ScoreRing");
    ring.SetLocalPosition(0.0f, 0.02f, 0.0f);
    ring.SetLocalScale(6.4f, 0.04f, 6.4f);
    {
        auto* mr = ring.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("duel_ring") };
    }

    static const char* kMats[] = { "duel_p0", "duel_p1" };
    for (uint32_t i = 0; i < 2; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Duelist_%u", i + 1);
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(i == 0 ? -6.0f : 6.0f, 0.5f, 0.0f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(kMats[i]) };
        // レーンの結び付けは PlayerInput のミラー (M52g) をそのまま使う。
        // スクリプトは playerIndex を読んで v13 の GetAxisForPlayer を引く
        auto* pi = go.AddComponent<PlayerInputComponent>();
        pi->playerIndex = static_cast<int32_t>(i);
        AttachScriptIfRegistered(s.GetWorld(), go.Id(), "NetDuelDemo");
    }

    // HUD。**ここだけがネット状態 (機種依存の値) を書いてよい場所**
    GameObject hud = MakeUiText(s, "NetHud", 0, 32.0f, 32.0f, 1280.0f, 56.0f, "net", 4.0f, 0);
    AttachScriptIfRegistered(s.GetWorld(), hud.Id(), "NetHudDemo");
}

void RegisterNetDuelContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("duel_floor", 0.24f, 0.26f, 0.30f);
    makeMat("duel_ring", 0.55f, 0.50f, 0.20f);
    makeMat("duel_p0", 0.95f, 0.35f, 0.25f); // P1 = 赤
    makeMat("duel_p1", 0.25f, 0.65f, 0.95f); // P2 = 青
}

void RegisterLocalPlayersContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("mp_floor", 0.26f, 0.28f, 0.34f);
    makeMat("mp_p0", 0.95f, 0.35f, 0.25f); // P1 = 赤
    makeMat("mp_p1", 0.25f, 0.65f, 0.95f); // P2 = 青
    makeMat("mp_p2", 0.35f, 0.85f, 0.40f); // P3 = 緑
    makeMat("mp_p3", 0.90f, 0.80f, 0.25f); // P4 = 黄
}

void BuildLocalPlayersScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("local_players");
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 7.0f, -13.0f);
    camera.SetLocalRotationEuler(24.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(52.0f, -28.0f, 0.0f);

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.25f, 0.0f);
    floor.SetLocalScale(18.0f, 0.5f, 12.0f);
    {
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("mp_floor") };
    }

    // ★**kMaxPlayers 体すべて置く**。--local-players 2 で走らせると 3 体目以降の
    //   PlayerInput ミラーは全ゼロのまま = 「playerCount を超えたレーンは恒常ゼロ」が
    //   ワールドハッシュの上で固定される (動かない 2 体も立派な検査対象)
    static const char* kMats[] = { "mp_p0", "mp_p1", "mp_p2", "mp_p3" };
    for (uint32_t i = 0; i < kMaxPlayers; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Player_%u", i + 1);
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(-6.0f + 4.0f * static_cast<float>(i), 0.5f, 0.0f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(kMats[i]) };
        auto* pi = go.AddComponent<PlayerInputComponent>();
        pi->playerIndex = static_cast<int32_t>(i);
        // 動きは C++ スクリプト側。**エンジンの ABI は 1 本も足していない** —
        // ミラーは v11 の GetComponentField (名前ハッシュの汎用スロット) で読む
        AttachScriptIfRegistered(s.GetWorld(), go.Id(), "LocalPlayerDemo");
    }
}

} // namespace mye
