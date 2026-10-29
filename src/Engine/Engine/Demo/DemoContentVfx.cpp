// 粒子・VFX・霧のショーケース (--particle-demo / --fog-demo)。
// DemoContent.cpp から領域ごとの翻訳単位へ分けたもの (M93g)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"

namespace mye {

void RegisterFogShowcaseContent(EngineContext& ctx)
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
    // 接頭辞は **fdemo_**。材質は全ショーケース分が無条件登録されるので、他と名前が
    // 被ると先に登録したほうが黙って上書きされる (M54a の申し送りと同じ配慮)
    // ★明るめに焼いてある。霧は「元の色をフォグ色へ寄せる」効果なので、素の色が暗いと
    //   掛かっているのかどうかが絵から読めない (最初に暗い材質で組んで失敗した)
    makeMat("fdemo_ground", 0.42f, 0.46f, 0.40f);
    makeMat("fdemo_pillar", 0.78f, 0.79f, 0.82f); // 距離帯の物差し (奥ほど霧に沈む)
}

// ---- M63a: パーティクル表現ショーケース (--particle-demo) ----
// 材質 2 つ + **手続き生成のテクスチャ 2 枚**。テクスチャをコードで焼くのは
// RegisterJointShowcaseContent の車輪メッシュと同じ流儀 (checkout 先に依存しない生成物)。
//
// ★テクスチャが要るのは被覆のため。既定の procedural ソフト円は**点対称**なので、
//   回転させても絵が 1 画素も変わらない = golden が C1 の回帰を検出できない。
//   ストレッチ (C2) も同じ理由で、向きの分かる絵でないと「伸びたか」しか読めない。
void RegisterParticleShowcaseContent(EngineContext& ctx)
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
    // 接頭辞は **vdemo_** (VFX)。★pdemo_ は --physics-demo が使用済み — 材質は全ショーケース
    // 分が無条件登録されるので、被ると先に登録したほうが黙って上書きされる (M54a の申し送り)
    makeMat("vdemo_ground", 0.34f, 0.36f, 0.40f);
    makeMat("vdemo_block", 0.72f, 0.70f, 0.66f); // 影を落とす箱 (C4 の被写体)

    // (1) 向きの分かるスプライト: +X を指す矢羽根。64x64 RGBA8。
    //     ★点対称にしないことだけが要件。回転角がそのまま絵の向きになるので、
    //       C1 が壊れると golden が必ず動く
    {
        constexpr int kN = 64;
        std::vector<uint8_t> px(static_cast<size_t>(kN) * kN * 4, 0);
        for (int y = 0; y < kN; ++y) {
            for (int x = 0; x < kN; ++x) {
                // ローカル座標 [-1,1]。+X が「前」、|v| が幅
                const float u = (static_cast<float>(x) + 0.5f) / kN * 2.0f - 1.0f;
                const float v = (static_cast<float>(y) + 0.5f) / kN * 2.0f - 1.0f;
                // 先端 (u=+1) で幅 0、尾 (u=-1) で最大幅の三角形。尾を少しえぐって矢羽根に
                const float halfWidth = 0.42f * (1.0f - u) * 0.5f;
                const float notch = 0.30f * (-u - 0.55f); // u<-0.55 でくびれる
                float a = 0.0f;
                if (u <= 1.0f && std::fabs(v) <= halfWidth && std::fabs(v) >= std::max(0.0f, notch)) {
                    // 縁を 1 画素ぶんだけ滑らかに (WARP でもエイリアスが暴れないように)
                    const float edge = (halfWidth - std::fabs(v)) * static_cast<float>(kN) * 0.5f;
                    a = std::clamp(edge, 0.0f, 1.0f);
                }
                const size_t o = (static_cast<size_t>(y) * kN + x) * 4;
                const uint8_t c = static_cast<uint8_t>(std::lround(a * 255.0f));
                px[o + 0] = c;
                px[o + 1] = c;
                px[o + 2] = c;
                px[o + 3] = c;
            }
        }
        res.textures.CreateFromRgba8("vdemo_arrow", px.data(), kN, kN, false, true);
    }

    // (2) フリップブックアトラス 4x4 (256x256)。コマ番号が絵から読めるように、
    //     リングの半径をコマごとに変え、切り欠きの角度もコマごとに回す。
    //     ★「隣り合うコマが十分に違う」ことが C3 (フレーム間ブレンド / ランダム開始) の
    //       被覆条件 — 似たコマだと補間しても開始をずらしても絵が変わらない
    {
        constexpr int kTiles = 4;
        constexpr int kTile = 64;
        constexpr int kN = kTiles * kTile;
        std::vector<uint8_t> px(static_cast<size_t>(kN) * kN * 4, 0);
        for (int ty = 0; ty < kTiles; ++ty) {
            for (int tx = 0; tx < kTiles; ++tx) {
                const int frame = ty * kTiles + tx; // 0..15
                const float t = static_cast<float>(frame) / 15.0f;
                const float radius = 0.25f + 0.65f * t;      // 広がるリング
                const float thick = 0.30f - 0.18f * t;       // 薄くなる
                const float notchAngle = t * 6.28318531f;    // 切り欠きが 1 周する
                for (int y = 0; y < kTile; ++y) {
                    for (int x = 0; x < kTile; ++x) {
                        const float u = (static_cast<float>(x) + 0.5f) / kTile * 2.0f - 1.0f;
                        const float v = (static_cast<float>(y) + 0.5f) / kTile * 2.0f - 1.0f;
                        const float r = std::sqrt(u * u + v * v);
                        float a = std::clamp(1.0f - std::fabs(r - radius) / thick, 0.0f, 1.0f);
                        // 切り欠き (コマごとに位置が回る = 隣接コマが必ず違う絵になる)
                        const float ang = std::atan2(v, u);
                        float d = std::fabs(ang - (notchAngle - 3.14159265f));
                        d = std::min(d, 6.28318531f - d);
                        if (d < 0.45f) {
                            a = 0.0f;
                        }
                        const size_t o =
                            (static_cast<size_t>(ty * kTile + y) * kN + (tx * kTile + x)) * 4;
                        const uint8_t c = static_cast<uint8_t>(std::lround(a * 255.0f));
                        px[o + 0] = c;
                        px[o + 1] = c;
                        px[o + 2] = c;
                        px[o + 3] = c;
                    }
                }
            }
        }
        // ★mips は作らない。4x4 アトラスは縮小時に隣のコマが滲み込む (タイル境界を
        //   跨ぐ mip はコマ同士を混ぜる) ので、フリップブックでは常に off が正しい
        res.textures.CreateFromRgba8("vdemo_flipbook", px.data(), kN, kN, false, false);
    }
}

// M63a: パーティクル表現ショーケース (--particle-demo)。golden 16/17 枚目の被写体。
//
// ★存在理由は被覆の穴埋め。M63 の 5 機能 (回転 / 速度ストレッチ / フリップブック /
//   ライティング / 深度衝突) は**全部既定 off** なので、絵に出さないと回帰検出がゼロになる。
//   既存デモに足すのでは駄目で、既定デモ (demo_forward/deferred = **CI 対象**) を動かすと
//   無関係な変更のレビューで毎回赤くなり、--fog-demo に足すと「フロクセル 2 分岐 ×
//   中間キー × VFX」の密な被覆を潰す (DemoContent.cpp の fog 節に明文の警告がある)。
// ★**CPU と GPU の 2 枚を撮る**のが要点。C1〜C3 は CPU インスタンス経路と GPU VS 経路の
//   2 実装を持つのに、既存 15 枚には両者を同じ被写体で突き合わせる golden が 1 枚も無い。
//   2 枚あれば「片方だけ直した」が必ず赤くなる。
void BuildParticleShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("particle_showcase");
    const AssetID cube = res.meshes.Cube();
    // ★手続き生成テクスチャの ID は **HashStr(生成名)** (CreateFromRgba8 と同じ式)。
    //   IdForFile は NormalizePathKey を通すので**別の値**になる — 取り違えると
    //   テクスチャ未解決で procedural 円へ落ち、回転が絵に出ないまま golden が焼ける
    const AssetID arrowTex{ HashStr("vdemo_arrow") };
    const AssetID flipTex{ HashStr("vdemo_flipbook") };

    auto box = [&](const char* name, float x, float y, float z, float sx, float sy, float sz,
                   const char* mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(x, y, z);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(mat) };
        return go;
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    // 6 本のエミッタ (x = -16..+10) を 1 枚に収める。少し見下ろして床との衝突が読めるように。
    // ★左端の mode=1 ライティングエミッタ (M63d) は x_px 230 より左の空白に置いてある。
    //   他のエミッタを動かすと C1〜C3/C5 の被覆が「位置が変わっただけ」で全部赤くなり、
    //   レビューで本物の回帰が埋もれる
    camera.SetLocalPosition(0.5f, 3.6f, -15.0f);
    camera.SetLocalRotationEuler(8.0f, 0.0f, 0.0f);

    // 平行光 (C4 の CSM 影の光源)。仰角を寝かせて箱の影を長く伸ばす —
    // 「影の中を煙が通る」が絵で読めるのが M63d の見どころ
    GameObject sun = s.CreateGameObject("Sun");
    {
        auto* l = sun.AddComponent<LightComponent>();
        l->intensity = 1.6f;
        l->ambient = { 0.10f, 0.11f, 0.13f }; // 暗めのアンビエント = 受光の差が読める
        sun.SetLocalRotationEuler(28.0f, -25.0f, 0.0f);
    }

    box("Ground", 0.0f, -0.5f, 8.0f, 60.0f, 1.0f, 60.0f, "vdemo_ground");
    // C4 の影を作る箱。ライティングエミッタ (x=+4) の手前に置いて影を跨がせる
    box("ShadowCaster", 6.6f, 1.5f, 3.2f, 0.9f, 3.0f, 0.9f, "vdemo_block");
    // M63d: mode=1 側 (x=-16) にも**同じ相対位置**で箱を置く。emitter との差 (+2.0, z=3.2)
    // まで揃えてあるのは、2 つのライティングモードを**同じ幾何条件で並べる**ため —
    // 「粒子単位は粒子まるごと影に入る/出るのでパッと切り替わる」「画素単位は煙の中を
    // 影の角柱が通る」という違いだけが絵に残る
    box("ShadowCaster2", -14.0f, 1.5f, 3.2f, 0.9f, 3.0f, 0.9f, "vdemo_block");

    // 点光源 (C4)。**局所ライトが煙を照らす**のが「完全 unlit」を直したことの主張なので、
    // 平行光だけでは足りない。範囲は隣のエミッタへ漏れない程度に絞る
    GameObject lamp = s.CreateGameObject("Lamp");
    {
        auto* l = lamp.AddComponent<LightComponent>();
        l->type = lighttype::kPoint;
        l->color = { 1.0f, 0.62f, 0.28f };
        l->intensity = 6.0f;
        l->range = 5.0f;
        lamp.SetLocalPosition(4.6f, 1.3f, 5.0f);
    }

    // M63d: mode=1 側の点光源。**色も強度も範囲も Lamp と同一**にして、
    // 2 つのライティングモードの A/B が光源の違いに汚されないようにする
    // (エミッタとの相対位置 (同 x, +1.0 上, 1.0 手前) も揃えてある)
    GameObject lamp2 = s.CreateGameObject("Lamp2");
    {
        auto* l = lamp2.AddComponent<LightComponent>();
        l->type = lighttype::kPoint;
        l->color = { 1.0f, 0.62f, 0.28f };
        l->intensity = 6.0f;
        l->range = 5.0f;
        lamp2.SetLocalPosition(-16.0f, 1.3f, 5.0f);
    }

    // ---- (1) C1 回転: 矢羽根が粒子ごとに違う速さで回る ----
    // ★角速度を**非対称な範囲**にするのが要点。全部同じ速さだと KillDead の swap 漏れ
    //   (粒子が死ぬたびに隣へ他人の回転が飛び移る) が絵に出ない
    {
        GameObject go = s.CreateGameObject("P1_Rotation");
        go.SetLocalPosition(-9.5f, 0.3f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63001u;
        e->blendMode = 1; // alpha (向きが読めるように不透明寄り)
        e->rate = 15.0f;
        e->texture = arrowTex;
        e->shape = 2; // cone
        e->coneAngleDeg = 26.0f;
        e->speedMin = 1.1f;
        e->speedMax = 1.9f;
        e->lifetimeMin = 1.6f;
        e->lifetimeMax = 2.4f;
        e->sizeMin = 0.34f;
        e->sizeMax = 0.50f;
        e->gravity = { 0.0f, 0.35f, 0.0f };
        e->colorBegin = { 1.0f, 0.92f, 0.70f, 1.0f };
        e->colorEnd = { 0.9f, 0.45f, 0.20f, 0.0f };
        e->sizeEndScale = 0.9f;
        e->rotationMin = -3.14159f;   // 初期角はばらばら
        e->rotationMax = 3.14159f;
        e->rotationSpeedMin = -4.5f;  // 逆回転も混ぜる (符号規約の被覆)
        e->rotationSpeedMax = 6.0f;
    }

    // ---- (2) C2 速度ストレッチ: 火花が進行方向へ伸びる (実装は M63b) ----
    {
        GameObject go = s.CreateGameObject("P2_Stretch");
        go.SetLocalPosition(-4.8f, 0.3f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63002u;
        e->blendMode = 0; // additive (火花)
        e->rate = 90.0f;
        e->texture = arrowTex;
        e->shape = 2;
        e->coneAngleDeg = 42.0f;
        e->speedMin = 4.0f; // 速いほど伸びる = ストレッチが読める
        e->speedMax = 8.0f;
        e->lifetimeMin = 0.8f;
        e->lifetimeMax = 1.3f;
        e->sizeMin = 0.10f;
        e->sizeMax = 0.16f;
        e->gravity = { 0.0f, -5.0f, 0.0f }; // 放物線 = 速度の向きが刻々変わる
        e->colorBegin = { 1.0f, 0.85f, 0.45f, 1.0f };
        e->colorEnd = { 1.0f, 0.25f, 0.05f, 0.0f };
        e->stretchScale = 0.22f;
        e->stretchMax = 5.0f;
    }

    // ---- (3) C3 フリップブック: コマ送り + 補間 + 開始位相のばらつき (実装は M63c) ----
    {
        GameObject go = s.CreateGameObject("P3_Flipbook");
        go.SetLocalPosition(0.0f, 0.4f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63003u;
        e->blendMode = 1;
        e->rate = 14.0f; // 少なめ = 1 粒ずつのコマが読める
        e->texture = flipTex;
        e->flipTilesX = 4;
        e->flipTilesY = 4;
        e->flipCycles = 1.0f;
        e->flipFps = 11.0f;      // 寿命に依らない固定 fps
        e->flipBlend = 1;        // コマ間補間
        e->flipRandomStart = 1;  // ★同 tick 湧きが同じコマになる問題そのものの被覆
        e->shape = 1;            // sphere
        e->shapeRadius = 0.5f;
        e->speedMin = 0.4f;
        e->speedMax = 0.9f;
        e->lifetimeMin = 1.8f;
        e->lifetimeMax = 2.6f;
        e->sizeMin = 0.55f;
        e->sizeMax = 0.80f;
        e->gravity = { 0.0f, 0.5f, 0.0f };
        e->colorBegin = { 0.65f, 0.85f, 1.0f, 1.0f };
        e->colorEnd = { 0.30f, 0.55f, 0.95f, 0.0f };
        e->sizeEndScale = 1.2f;
    }

    // ---- (4) C4 ライティング: 点光源に照らされ、箱の影を通る煙 (実装は M63d) ----
    {
        GameObject go = s.CreateGameObject("P4_Lit");
        go.SetLocalPosition(4.6f, 0.3f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63004u;
        e->blendMode = 1; // alpha (加算だと受光の差が飽和して読めない)
        e->rate = 26.0f;
        e->shape = 1;
        e->shapeRadius = 0.45f;
        e->speedMin = 0.5f;
        e->speedMax = 1.1f;
        e->lifetimeMin = 2.2f;
        e->lifetimeMax = 3.0f;
        e->sizeMin = 0.55f;
        e->sizeMax = 0.85f;
        e->gravity = { -0.15f, 0.75f, 0.0f }; // 影の側へ流す
        // ★素の色は**白に近いグレー**。着色しておくと点光源の橙が乗ったか読めない
        e->colorBegin = { 0.82f, 0.84f, 0.86f, 0.60f };
        e->colorEnd = { 0.60f, 0.62f, 0.66f, 0.0f };
        e->sizeEndScale = 1.8f;
        e->lightingMode = 2; // 画素単位 (球面法線)
        e->lightWrap = 0.55f;
        e->lightIntensity = 1.0f;
        e->lightReceiveShadow = 1;
    }

    // ---- (4b) C4 ライティング mode=1: **粒子単位** (受光を VS で色へ畳む) ----
    // ★P4_Lit と **lightingMode 以外は 1 フィールドも違わない**。光源も箱も相対位置まで
    //   ミラーしてあるので、2 枚の絵の違いは「粒子単位か画素単位か」だけになる。
    // ★これが golden における mode=1 の**唯一の被覆**。VS ステージへの CB / CSM /
    //   比較サンプラのバインドを 1 本でも落とすと、CSM が未バインド SRV から 0 を返して
    //   dirShadow=0 になり**この 1 本だけ暗く沈む** — mode=2 側は無傷なので、
    //   並べてあることが検出そのものになっている
    {
        GameObject go = s.CreateGameObject("P4b_LitVertex");
        go.SetLocalPosition(-16.0f, 0.3f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63006u;
        e->blendMode = 1;
        e->rate = 26.0f;
        e->shape = 1;
        e->shapeRadius = 0.45f;
        e->speedMin = 0.5f;
        e->speedMax = 1.1f;
        e->lifetimeMin = 2.2f;
        e->lifetimeMax = 3.0f;
        e->sizeMin = 0.55f;
        e->sizeMax = 0.85f;
        e->gravity = { -0.15f, 0.75f, 0.0f };
        e->colorBegin = { 0.82f, 0.84f, 0.86f, 0.60f };
        e->colorEnd = { 0.60f, 0.62f, 0.66f, 0.0f };
        e->sizeEndScale = 1.8f;
        e->lightingMode = 1; // 粒子単位 (VS で畳む) ← ここだけが P4_Lit との差
        e->lightWrap = 0.55f;
        e->lightIntensity = 1.0f;
        e->lightReceiveShadow = 1;
    }

    // ---- (5) C5 深度衝突: 床で跳ねて滑る破片 (GPU 限定。実装は M63e) ----
    // ★CPU バックエンドでは衝突しない (spec 7.5 の例外) ので、**2 枚の golden が
    //   意図的に食い違う唯一の場所**。差が出ること自体が仕様の可視化になっている
    {
        GameObject go = s.CreateGameObject("P5_Collide");
        go.SetLocalPosition(10.0f, 3.4f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63005u;
        e->blendMode = 1;
        e->rate = 60.0f;
        e->shape = 3; // box
        e->boxExtents = { 0.9f, 0.05f, 0.9f };
        e->emitFrom = 1;
        e->speedMin = 0.2f;
        e->speedMax = 0.7f;
        e->lifetimeMin = 2.0f;
        e->lifetimeMax = 3.2f;
        e->sizeMin = 0.12f;
        e->sizeMax = 0.20f;
        e->gravity = { 0.0f, -7.0f, 0.0f }; // 床へ叩きつける
        e->colorBegin = { 0.55f, 0.95f, 0.75f, 0.95f };
        e->colorEnd = { 0.25f, 0.70f, 0.55f, 0.0f };
        e->depthCollision = 1;
        e->collisionBounce = 0.35f;
        e->collisionFriction = 0.4f;
        e->collisionLifeLoss = 0.12f;
        e->collisionFloor = 1;
        e->collisionFloorY = 0.0f;
    }
}

void BuildFogShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("fog_showcase");
    const AssetID cube = res.meshes.Cube();

    auto box = [&](const char* name, float x, float y, float z, float sx, float sy, float sz,
                   const char* mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(x, y, z);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(mat) };
        return go;
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    // 手前 8m から奥 70m までを 1 枚に収める画角。**フロクセルのグリッド端 (既定 64m) が
    // 画の中に入っていること**が要点 — 受け持ちの切り替わりが絵に出ていないと、
    // 「解析フォグとフロクセルの分担」が壊れても golden が気づけない
    camera.SetLocalPosition(0.0f, 4.0f, -16.0f);
    camera.SetLocalRotationEuler(5.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    // ★太陽をカメラの正面側 (奥) へ振る — 太陽インスキャッタ (M43a) は「視線が太陽へ
    //   向くほどフォグ色が太陽色へ寄る」効果なので、背後から照らすと絵に 1 画素も出ない。
    //   VFX が ApplyFog へ寄ったこと (コミット②) を絵で見るための配置。
    //   仰角は地面が読める程度に上げてある (寝かせると全部逆光のシルエットになる)
    sun.SetLocalRotationEuler(42.0f, 6.0f, 0.0f);

    box("Ground", 0.0f, -0.5f, 25.0f, 120.0f, 1.0f, 200.0f, "fdemo_ground");

    // ---- 距離帯の物差し (グリッドの内と外を跨ぐ) ----
    // 10 / 25 / 45 はフロクセルのグリッド内、70 は外。**同じ形・同じ材質**を並べるので、
    // 霧の掛かり方の違いがそのまま距離の関数として読める
    const float pillarZ[] = { 10.0f, 25.0f, 45.0f, 70.0f };
    for (int i = 0; i < 4; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Pillar_%d", i);
        const float x = (i % 2 == 0) ? -6.0f : 6.0f;
        box(name, x, 3.0f, pillarZ[i], 1.6f, 7.0f, 1.6f, "fdemo_pillar");
    }

    // ---- 霧 (--render-demo と同じ値) ----
    // 同じ設定にしてあるのは、あちらで確認済みの「メッシュがどう霧むか」を基準として
    // 使うため。粒子と VFX がこれと食い違っていないかが本ショーケースの見どころ
    {
        GameObject fog = s.CreateGameObject("Fog");
        auto* f = fog.AddComponent<FogComponent>();
        f->mode = 2; // Exp2
        f->color = { 0.11f, 0.13f, 0.17f, 1.0f };
        // ★density は --render-demo (0.011) より濃い 0.020。あちらは被写体が 44〜88m に
        //   あるが、こちらは粒子と VFX が 28〜38m の近距離に居るので、同じ濃度だと
        //   f が 0.12 程度にしかならず「霧が適用されているか」が絵から読めない。
        //   0.020 なら 32m で f≈0.34 / 61m で f≈0.78 / 86m で f≈0.95 と広い範囲に散る
        f->density = 0.020f;
        f->heightFalloff = 0.035f;
        f->baseHeight = 0.0f;
        f->inscatterIntensity = 0.30f;
        f->inscatterPower = 10.0f;
    }

    // ---- パーティクル: 加算と alpha を両方置く ----
    // ★フロクセル合成の 2 分岐 (加算は透過率だけ / alpha は scene·T + inscatter) を
    //   **両方とも絵に出す**のが目的。片方だけだと、取り違えても golden が気づけない
    {
        GameObject fire = s.CreateGameObject("Fire_Additive");
        fire.SetLocalPosition(-3.0f, 0.2f, 16.0f);
        auto* e = fire.AddComponent<ParticleEmitterComponent>();
        e->blendMode = 0; // additive
        e->rate = 120.0f;
        e->seed = 20250830u;
    }
    {
        GameObject smoke = s.CreateGameObject("Smoke_Alpha");
        smoke.SetLocalPosition(3.0f, 0.2f, 16.0f);
        auto* e = smoke.AddComponent<ParticleEmitterComponent>();
        e->blendMode = 1; // alpha
        e->rate = 90.0f;
        e->seed = 777u;
        e->colorBegin = { 0.75f, 0.78f, 0.82f, 0.85f };
        e->colorEnd = { 0.55f, 0.58f, 0.62f, 0.0f };
        e->sizeMin = 0.35f;
        e->sizeMax = 0.60f;
        e->speedMin = 1.0f;
        e->speedMax = 1.8f;
        // ★M42追補: **中間キーの CPU/GPU 一致をピクセルで担保するための被覆**。
        //   golden にこの 2 本が写っている限り、片方のバックエンドが中間キーを無視すると赤くなる。
        //   ★値は「begin→end の線形補間から**明確に外れる**」ように選ぶこと —
        //     線上に置くと中間キーを無視しても絵が変わらず、被覆にならない。
        //   立ち上がりで炎に照らされた暖色へ寄せ (alpha も線形より高い)、その後に冷えて散る絵。
        e->colorMid1 = { 0.92f, 0.60f, 0.34f, 0.72f };
        e->colorMidT1 = 0.25f;
        //   サイズは 1.0 → 1.4 → 0.25 (sizeEndScale の既定) の山なり。t=0.35 の 2 点線形は
        //   0.74 なので約 2 倍 = 中間キーを無視すれば一目で分かる差。★2.2 まで太らせると
        //   煙が TrailTip のリボンを半分隠し、M57追補 が作った VFX のピクセル被覆
        //   (シアン画素 151 → 83) を削ってしまう — 被覆を足すために別の被覆を潰さないこと
        e->sizeMidScale = 1.4f;
        e->sizeMidT = 0.35f;
    }

    // ---- VFX (Sprite / Trail / TextMesh) ----
    // ★**リポジトリで唯一この 3 種が写る被写体** (無いと VfxRenderer が壊れても golden が全部緑のまま)
    // ★スプライト 2 枚は **「霧の量だけが違う 2 枚」になるように配置してある**:
    //   どちらもカメラの目線高さ (y=4) に置き、遠いほうは距離比 (68/28) だけ大きく焼く。
    //   こうすると 2 枚は画面上で同じ大きさ・左右対称の同じ高さに出るので、
    //   **見え方の差が霧の量そのもの**になる。片方だけ霧が抜けたら一目で分かる。
    //   ★寸法や z を動かすときは必ずこの比 (size ∝ 距離) を保つこと — 崩すと
    //     「大きさが違うから暗いのか、霧で暗いのか」が絵から切り分けられなくなる
    {
        GameObject spriteNear = s.CreateGameObject("Sprite_Near");
        spriteNear.SetLocalPosition(-10.0f, 4.0f, 12.0f); // カメラから 28m
        auto* sp = spriteNear.AddComponent<SpriteRendererComponent>();
        sp->color = { 1.0f, 0.55f, 0.25f, 1.0f };
        sp->size = { 2.0f, 2.0f };
    }
    {
        GameObject spriteFar = s.CreateGameObject("Sprite_Far");
        spriteFar.SetLocalPosition(24.29f, 4.0f, 52.0f); // カメラから 68m (28m の 2.43 倍)
        auto* sp = spriteFar.AddComponent<SpriteRendererComponent>();
        sp->color = { 1.0f, 0.55f, 0.25f, 1.0f };
        sp->size = { 4.857f, 4.857f }; // 2.0 * 2.43 = 画面上では近い板と同じ大きさ
    }
    {
        // ★Trail は「点が 2 つ以上溜まって初めて」リボンになる (TrailStore)。静止した
        //   エンティティでは 1 本も出ないので、Rotator (GameLogic.dll) で親を Y 軸回転させて
        //   子のワールド位置を動かす。**DLL が焼けていないとリボンが消えて golden が
        //   静かに変わる** (joints の VehicleDemoDriver と同じ依存)。
        //   Rotator は 30 deg/s なので、frame 120 (2.0 秒) までに 60 度ぶんの弧が溜まる
        GameObject spinner = s.CreateGameObject("Spinner");
        spinner.SetLocalPosition(0.0f, 3.2f, 20.0f);
        AttachScriptIfRegistered(w, spinner.Id(), "Rotator");
        GameObject tip = s.CreateGameObject("TrailTip");
        tip.SetParent(spinner);
        tip.SetLocalPosition(4.5f, 0.0f, 0.0f);
        auto* tr = tip.AddComponent<TrailRendererComponent>();
        tr->duration = 2.0f; // 撮影 frame (120 = 2.0 秒) と揃える = 弧が最長になる
        tr->width = 0.55f;
        tr->colorBegin = { 0.35f, 0.85f, 1.0f, 1.0f };
        tr->colorEnd = { 0.10f, 0.35f, 0.65f, 0.0f };
        tr->minVertexDistance = 0.05f;
    }
    {
        GameObject label = s.CreateGameObject("Label");
        label.SetLocalPosition(0.0f, 5.2f, 30.0f);
        auto* tm = label.AddComponent<TextMeshComponent>();
        std::snprintf(tm->text, sizeof(tm->text), "FOG");
        // fontScale 1.0 で行高 ≈ 0.3 ワールド単位。30m 先で読める大きさに留める —
        // 大きくしすぎると板が画面を覆って、奥の柱 (霧の物差し) が隠れる
        tm->fontScale = 6.0f;
        tm->color = { 0.95f, 0.95f, 0.80f, 1.0f };
    }

    w.ApplyStructuralChanges();
}

} // namespace mye
