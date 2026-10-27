// 音響・衝突音のショーケース (--acoustic-demo / --modal-demo)。
// DemoContent.cpp から領域ごとの翻訳単位へ分けたもの (M93g)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <cstdio>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Audio/Playback/SoundAsset.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/RayTracing/RtTypes.h" // kRtReflClass* (M67: デモの反射クラス)

namespace mye {

// ---- M65b: 音響ショーケース (--acoustic-demo) ----
// 接頭辞は **adem_**。材質は全ショーケース分が無条件登録されるので、他と名前が被ると
// 先に登録したほうが黙って上書きされる (M54a の申し送りと同じ配慮)。
// 既使用: rdemo_ / tdemo_ / pdemo_ / jdemo_ / fdemo_ / vdemo_
void RegisterAcousticShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();

    // M67: 末尾の reflClass = 反射に映るときの再利用の厳しさ (省略時は 4 = 中立)
    auto makeMat = [&](const char* name, float r, float g, float b,
                       int reflClass = kRtReflClassDefault) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        m.reflectionClass = reflClass;
        return res.materials.Register(name, m);
    };
    // ★暗い。企画は「世界は真っ暗で、音の波だけが世界を描く」なので素の色は沈めてある。
    //   ただし**真っ黒にはしない** — M65e で golden を撮るときに全画素が黒だと
    //   機能が壊れて何も出なくても golden 一致で通る = 回帰検出がゼロになる
    //   (計画 M65e の最大の罠)。壁と床の輪郭が薄く読める程度に留めてある
    makeMat("adem_floor", 0.13f, 0.14f, 0.16f);
    makeMat("adem_wall", 0.20f, 0.20f, 0.23f);
    makeMat("adem_source", 0.85f, 0.72f, 0.30f); // 音源の目印 (黄)
    // ---- M65c: 床材タイル 6 種 ----
    // ★色は**音の大きさの順に明るく**してある。企画は「材質は事前には分からない」だが、
    //   デモは検証用なので「どのタイルで大きい波が出たか」が絵で読めることを優先する
    makeMat("adem_carpet", 0.26f, 0.15f, 0.14f);
    makeMat("adem_wood", 0.34f, 0.24f, 0.13f);
    makeMat("adem_gravel", 0.30f, 0.30f, 0.27f);
    makeMat("adem_water", 0.12f, 0.26f, 0.38f);
    makeMat("adem_metal", 0.46f, 0.48f, 0.52f);
    makeMat("adem_glass", 0.34f, 0.48f, 0.50f);
    makeMat("adem_drop", 0.62f, 0.30f, 0.28f); // 衝撃音の被写体 (落ちてくる箱)
    // ---- M65f: 敵 2 種 ----
    // ★色でセンサーが読めるようにしてある (赤 = 耳 / 緑 = 目)。同じ FSM を回していて
    //   違うのは載っているセンサーだけ、という設計が絵で確かめられる
    makeMat("adem_agent_ear", 0.58f, 0.20f, 0.22f, kRtReflClassCharacter);
    makeMat("adem_agent_eye", 0.22f, 0.52f, 0.26f, kRtReflClassCharacter);
    // ---- M65g: プレイヤーと道具 ----
    // ★設置光 (adem_lamp) だけ明るい。**スクリプトが強度を 0 から育てる**ので、
    //   球そのものが暗いと「置いている最中」が絵から読めない (企画 4-3 はゲージを
    //   出さないと決めているので、光の育ち方が唯一の進行表示になる)
    makeMat("adem_player", 0.30f, 0.44f, 0.58f, kRtReflClassHero);
    makeMat("adem_lamp", 0.95f, 0.86f, 0.62f);
    makeMat("adem_stone", 0.38f, 0.36f, 0.33f);
    makeMat("adem_bottle", 0.30f, 0.52f, 0.44f);
}

// ---- 音響ショーケースの間取り ----
// 12x12 タイル / 1 タイル = 2m → 24x24m。'#' = 壁 / '.' = 通れる。
// **L 字の廊下で 2 部屋を繋ぐ**のが唯一の要件 — 直線で見通せる配置にすると
// 「角を回り込んだ」ことが絵から読めない (シャドウマップでも同じ絵になってしまう)。
//   部屋 A (左上) → 横に伸びる廊下 (row 4) → 縦の廊下 (col 9) → 部屋 B (下)
// 音源は部屋 A に置く。部屋 B は**部屋 A から一直線には見えない**位置にある
static const char* const kAcousticMap[12] = {
    "############", // r0
    "#....#######", // r1  部屋 A (col 1..4)
    "#....#######", // r2
    "#....#######", // r3
    "#..........#", // r4  横の廊下 (col 1..10)
    "#########.##", // r5  縦の廊下 (col 9)
    "#########.##", // r6
    "##.........#", // r7  部屋 B (col 2..10)
    "##.........#", // r8
    "##.........#", // r9
    "##.........#", // r10
    "############", // r11
};
constexpr float kAcousticTile = 2.0f;
constexpr int kAcousticMapN = 12;

// マップ座標 -> ワールド。中心が原点に来るように寄せる
static float AcousticMapToWorld(int i)
{
    return (static_cast<float>(i) - (kAcousticMapN - 1) * 0.5f) * kAcousticTile;
}

// M65b: 音響ショーケース (--acoustic-demo)。
// ★**波が壁を貫通せず L 字を曲がる**ことを見せるためだけのシーン。
//   残光は光パスで絵に出る (M65e) ほか、デバッグ線 (View > 音響) でも見える。
//   replay 7 ペア目の被写体 (= 波のハッシュ被覆) はここにしか無い。
void BuildAcousticShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("acoustic_showcase");
    const AssetID cube = res.meshes.Cube();

    auto box = [&](const char* name, float x, float y, float z, float sx, float sy, float sz,
                   const char* mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(x, y, z);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(mat) };
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox; // ★これが占有ベイクの入力 = 音を遮る実体
        col->halfExtents = { 0.5f, 0.5f, 0.5f }; // スケールが掛かるので単位箱でよい
        return go;
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    // ★間取り全体が 1 枚に入る俯瞰。**L 字の 2 本の腕と 2 部屋が同時に見えること**が
    //   画角の唯一の要件 — 片方の腕が切れていると「回り込んだ」が絵から読めない
    // ★間取りが画面の中で小さいと**残光が壊れても golden の差分画素が少なすぎて埋もれる**。
    //   2 本の腕と 2 部屋が入る限界まで寄せてある
    camera.SetLocalPosition(0.0f, 20.0f, -13.0f);
    camera.SetLocalRotationEuler(56.0f, 0.0f, 0.0f);

    // 弱い環境光。★企画は真っ暗だが、**壁の輪郭が読めない画は golden にならない**
    //   (何も出なくても一致してしまう)。輪郭だけが見える強さに落としてある
    GameObject sun = s.CreateGameObject("Sun");
    auto* sunLight = sun.AddComponent<LightComponent>();
    sunLight->intensity = 0.35f;
    sun.SetLocalRotationEuler(62.0f, 24.0f, 0.0f);

    box("Floor", 0.0f, -0.5f, 0.0f, 26.0f, 1.0f, 26.0f, "adem_floor");

    // ---- 壁 ----
    // 1 タイル = 1 箱。**連結して 1 枚の板にまとめない** — まとめると占有ベイクの
    // 入力が減って速くはなるが、間取りを変えたときに手で貼り直すことになる
    for (int r = 0; r < kAcousticMapN; ++r) {
        for (int c = 0; c < kAcousticMapN; ++c) {
            if (kAcousticMap[r][c] != '#') {
                continue;
            }
            char name[32];
            std::snprintf(name, sizeof(name), "Wall_%02d_%02d", r, c);
            box(name, AcousticMapToWorld(c), 1.5f, AcousticMapToWorld(r), kAcousticTile, 3.0f,
                kAcousticTile, "adem_wall");
        }
    }

    // ---- 音響ボリューム ----
    // 52x6x52 セル / 0.5m = 26x3x26m。間取り (24x24m) を水平に 1m ずつ包んでいる。
    // ★グリッド外は壁扱いなので、包みきれていないと端で波が消えて理由不明のバグに見える。
    // ★★**高さは壁より高くしてはいけない**。中心 1.75 / 6 セル = y[0.25, 3.25] は
    //   壁 (y[0,3]) が全 6 層を塞ぐ高さで、床 (y[-1,0]) はグリッドの外に落ちる。
    //   最初 8 セル (y[-0.5,3.5]) で組んだら**壁の上に 0.5m の隙間が開いて波が壁を
    //   飛び越え**、L 字を曲がらずに直接となりの部屋へ届いていた (実測: 到達セルが
    //   開セルのほぼ全部になり、企画の中核が絵から消えた)。
    //   高さ方向の包み込みは水平方向と逆で、**きつく取る**のが正しい
    {
        GameObject vol = s.CreateGameObject("Acoustic Volume");
        vol.SetLocalPosition(0.0f, 1.75f, 0.0f);
        auto* av = vol.AddComponent<AcousticVolumeComponent>();
        av->dimX = 52;
        av->dimY = 6;
        av->dimZ = 52;
        av->cellSize = 0.5f;
        av->navCellRatio = 2;
    }

    // ---- 音源 (部屋 A) ----
    // ★**生成順の末尾に置く**。デモへ物を足すときに前へ挿すと粒子 RNG のストリームが
    //   ずれる、という既知の罠と同じ理由で、末尾追加を習慣にしておく
    {
        GameObject src = s.CreateGameObject("Wave Source");
        src.SetLocalPosition(AcousticMapToWorld(2), 1.0f, AcousticMapToWorld(2));
        src.SetLocalScale(0.6f, 0.6f, 0.6f);
        auto* mr = src.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("adem_source") };
        auto* em = src.AddComponent<AcousticEmitterComponent>();
        em->ticksPerRing = 2; // 0.5m / 2 tick = 15 m/s。部屋 A から部屋 B まで約 1.6 秒
        em->cooldownTicks = 30;
        // WavePinger (GameLogic.dll) が 150 tick ごとに pendingLoudness を書く。
        // ★DLL が焼けていないと**波が 1 本も出ない** — replay 7 ペア目のハッシュ被覆が
        //   丸ごと消えるので、joints の VehicleDemoDriver と同じ「DLL 依存の被写体」
        AttachScriptIfRegistered(w, src.Id(), "WavePinger");
    }

    // ---- 聴者 (部屋 B) ----
    // 鏡に「いつ・どこから聞こえたか」が入る (M65f)。**デバッグ線の被写体**でもある
    {
        GameObject ear = s.CreateGameObject("Listener");
        // ★部屋 B の**入口寄り**に置く。奥に置くと L 字経路が 32m を超えて
        //   一度も届かず、M65f の聴覚 AI が無反応のまま「壊れていないのに動かない」になる
        ear.SetLocalPosition(AcousticMapToWorld(8), 1.0f, AcousticMapToWorld(8));
        ear.AddComponent<AcousticListenerComponent>();
    }

    // ---- M65c: 床材タイル (横の廊下 row 4 の col 2..7) ----
    // ★**厚み 0.6 / 中心 y=-0.25 = 天面 0.05**。数字の理由:
    //   (a) セル中心 (層 0 = y=0.5) には**届かない**こと — 届くと廊下の床が
    //       占有セルになり、M65b で測った「断面 24 セルの平面波」が変わる。
    //   (b) 床 (天面 y=0) との段差が **CC が登れる高さ**であること。
    //       ★★天面 0.45 にすると **敵 (M65f) が廊下へ入れない**。0.45m の段差は
    //         collide-and-slide では登れず、追跡中の敵がタイルの端で永久に足踏みする
    //         (歩行者はタイルの上で生まれるので一度も跨がず、絵にも出ない)。5cm なら誰でも登れる。
    //   ★(a) の「天面が音響ボリュームの下端 (y=0.25) より上」は**タイルには要らない**。
    //     足音は歩行者の位置 (y=1.35) で鳴るので、タイルの高さは発音位置に無関係。
    //     この制約が効くのは接触点そのものが発音位置になる**衝撃の金属板だけ**で、
    //     あちらは天面 0.45 のまま据え置いてある。
    // ★6 枚を**隙間なく並べる**のも意図的 (端で段差を作らない)
    {
        static const char* const kTileMats[6] = { "carpet", "wood",  "gravel",
                                                  "water",  "metal", "glass" };
        static const char* const kTileVisuals[6] = { "adem_carpet", "adem_wood",  "adem_gravel",
                                                     "adem_water",  "adem_metal", "adem_glass" };
        for (int i = 0; i < 6; ++i) {
            const int col = 2 + i;
            char name[48];
            std::snprintf(name, sizeof(name), "Tile_%s", kTileMats[i]);
            GameObject t = s.CreateGameObject(name);
            t.SetLocalPosition(AcousticMapToWorld(col), -0.25f, AcousticMapToWorld(4));
            t.SetLocalScale(kAcousticTile, 0.6f, kAcousticTile);
            auto* mr = t.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = AssetID{ HashStr(kTileVisuals[i]) };
            auto* col2 = t.AddComponent<ColliderComponent>();
            col2->shape = collidershape::kBox;
            col2->halfExtents = { 0.5f, 0.5f, 0.5f };
            // ★materials 未登録なら AssetID{} = 未割当 = 無音に落ちるだけ (デモは壊れない)
            col2->physMaterial = FindPhysMat(kTileMats[i]);
        }
    }

    // ---- M65c: 歩行者 (足音の被写体) ----
    // ★CC 単体で組む (ソリッド Collider を併用しない) — 足元へ撃つレイが自分に当たると
    //   GroundMaterialUnder が無音を返す仕様なので、併用すると足音が 1 度も出なくなる
    {
        GameObject walker = s.CreateGameObject("Walker");
        // タイル天面は 0.05 (上の床材タイル)。カプセル半長 0.9 で載る高さ 0.95 より 0.4m 上から始まる
        walker.SetLocalPosition(AcousticMapToWorld(2), 1.35f, AcousticMapToWorld(4));
        walker.SetLocalScale(0.6f, 1.0f, 0.6f);
        auto* mr = walker.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("adem_source") };
        auto* cc = walker.AddComponent<CharacterControllerComponent>();
        cc->radius = 0.3f;
        cc->height = 1.8f;
        auto* em = walker.AddComponent<AcousticEmitterComponent>();
        em->autoFootstep = true;
        em->stepDistanceM = 0.9f; // 歩幅。2.0 m/s なら 27 tick に 1 歩
        em->ticksPerRing = 2;
        em->cooldownTicks = 12; // 金属の上で走っても波が詰まらない下限間隔
        AttachScriptIfRegistered(w, walker.Id(), "WaveWalker");
    }

    // ---- M65c: 衝撃音 (部屋 B の金属板へ箱を落とす) ----
    // ★歩行者の通り道から**外して**置く。廊下に置くと CC が剛体に阻まれて止まり、
    //   足音の被写体が丸ごと死ぬ (CC は剛体を押せない)
    {
        GameObject plate = s.CreateGameObject("Impact Plate");
        plate.SetLocalPosition(AcousticMapToWorld(5), 0.15f, AcousticMapToWorld(8));
        plate.SetLocalScale(kAcousticTile, 0.6f, kAcousticTile);
        auto* mr = plate.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("adem_metal") };
        auto* pc = plate.AddComponent<ColliderComponent>();
        pc->shape = collidershape::kBox;
        pc->halfExtents = { 0.5f, 0.5f, 0.5f };
        pc->physMaterial = FindPhysMat("metal");

        GameObject drop = s.CreateGameObject("Impact Box");
        drop.SetLocalPosition(AcousticMapToWorld(5), 4.5f, AcousticMapToWorld(8));
        drop.SetLocalScale(0.7f, 0.7f, 0.7f);
        auto* dmr = drop.AddComponent<MeshRendererComponent>();
        dmr->mesh = cube;
        dmr->material = AssetID{ HashStr("adem_drop") };
        auto* dc = drop.AddComponent<ColliderComponent>();
        dc->shape = collidershape::kBox;
        dc->halfExtents = { 0.5f, 0.5f, 0.5f };
        auto* rb = drop.AddComponent<RigidbodyComponent>();
        rb->mass = 2.0f;
        // ★**反発は 0**。0.5 で組んだら、跳ね返りが減衰しきらずに
        //   「5 tick ごとに力積 1.6 N*s」の極限周期に入り、**900 tick で 151 回**
        //   衝撃音を出し続けた (実測)。反発 + 貫通押し出しが作る古典的なリミットサイクルで、
        //   速度は 0.5 m/s あるのでスリープ閾値にも掛からない。跳ねるたびに音が小さく
        //   なる様子は絵として魅力的だが、**永久音源はデモとして嘘**なので捨てた
        //   (減衰そのものは ImpactGain のセルフテストが固定している)
        rb->restitution = 0.0f;
    }

    // ---- M65e: 設置光 1 個 (企画 §4-3 の「持ち込んだ光」) ----
    // ★**golden を撮るために必須の 1 個**。M65e で初めてピクセルが動くが、企画どおり
    //   真っ暗な画にすると「機能が壊れて残光が 1 画素も出なくても golden と一致する」=
    //   回帰検出がゼロになる (計画 M65e の最大の罠)。弱い環境光 (Sun 0.35) で壁の輪郭を、
    //   この点光源で「音以外の光」を出しておくと、**音の帯が消えたときだけ絵が変わる**。
    // ★置き場所は部屋 A の隅。廊下と部屋 B には届かない範囲にしてあるので、
    //   L 字の向こう側で光っているものは残光しかない = 1 枚で両方を主張できる
    // ★**関数の末尾に足すこと** — 前に挿すと以降の全エンティティの index がずれ、
    //   波スロットの割り当て順まで動く (plans の「デモの生成順は RNG のストリーム」)
    {
        GameObject lamp = s.CreateGameObject("Placed Light");
        lamp.SetLocalPosition(AcousticMapToWorld(3), 1.6f, AcousticMapToWorld(3));
        auto* pl = lamp.AddComponent<LightComponent>();
        pl->type = lighttype::kPoint;
        pl->range = 6.0f;
        pl->intensity = 2.2f;
        pl->color = { 1.0f, 0.86f, 0.62f }; // 携行灯らしい暖色 (残光の寒色と対になる)
    }

    // ---- M65f: 敵 2 種 (同じ FSM / 違うセンサー) ----
    // ★**2 体置くことが企画の主張そのもの**。同じ AgentBrain を回していて、
    //   片方は AcousticListener だけ、もう片方は LightSeeker だけを持つ。
    //   絵の中で「赤は足音のほうへ、緑は設置光のほうへ」動けば、
    //   「1 つの FSM に差し替え可能なセンサーを挿す」という設計が成立している。
    // ★どちらも **CC 単体** (ソリッド Collider を付けない) — 足音のときと同じ理由で、
    //   同じエンティティにコライダを併用すると真下のレイが自分に当たる。
    //   さらに敵にコライダを付けると占有ベイクの署名が毎 tick 変わって全再ベイクが走る
    //   (動くものは遮蔽に入れない、という Sync 側の規約)。
    auto agent = [&](const char* name, int col, int row, const char* mat) {
        GameObject go = s.CreateGameObject(name);
        const float px = AcousticMapToWorld(col);
        const float pz = AcousticMapToWorld(row);
        go.SetLocalPosition(px, 1.35f, pz);
        go.SetLocalScale(0.6f, 1.6f, 0.6f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(mat) };
        auto* cc = go.AddComponent<CharacterControllerComponent>();
        cc->radius = 0.3f;
        cc->height = 1.6f;
        auto* br = go.AddComponent<AgentBrainComponent>();
        // ★home は明示的に焼く。AgentSystem は「原点なら未設定」で拾うが、
        //   それに頼るとデモの巡回先が「たまたま原点」になったときに壊れる
        br->home = { px, 1.35f, pz };
        br->target = br->home;
        return go;
    };
    {
        // 耳の敵: 部屋 B の奥。廊下を往復する歩行者の足音 (材質で 3m〜30m) を聞いて、
        // **音が回り込んできたのと同じ道**を辿って寄ってくる
        GameObject ear = agent("Agent Ear", 9, 9, "adem_agent_ear");
        auto* ls = ear.AddComponent<AcousticListenerComponent>();
        // ★閾値は**逆二乗のスケールで決める**こと。EnergyAt は minD = 1 セル (0.5m) の
        //   逆二乗なので、振幅 1.0 の音でも 10m 先では (0.5/10)^2 = 0.0025 しかない。
        //   0.015 のような「一見ちょうどよさそうな」値だと 3m 以内でしか聞こえず、
        //   デモでは 600 tick 走らせても 1 度も反応しなかった (probe で発見)。
        //   0.0015 = 廊下 (10m) の water / metal / glass は聞こえて gravel は聞こえない境
        ls->threshold = 0.0015f;
    }
    {
        // 目の敵: 部屋 B の入口寄り。**視線判定は持たない** (v1 の割り切り) ので、
        // 壁の向こうの設置光にも引かれる — 企画の「光に寄る」をそのまま素直に実装した形。
        // attractRadius を既定の 12 から広げてあるのは、部屋 B から部屋 A の光までが
        // 直線で 15m あり、既定のままだと 1 度も反応せず絵に出ないため
        GameObject eye = agent("Agent Eye", 4, 8, "adem_agent_eye");
        auto* seek = eye.AddComponent<LightSeekerComponent>();
        seek->attractRadius = 30.0f;
        seek->minIntensity = 0.5f; // 太陽 (0.35) には引かれない (平行光は元々見ない)
    }

    // ---- M65g: プレイヤー (企画のゲームループを通すための薄皮) ----
    // ★**エンジンには 1 行も足していない**。一人称視点 / 光の設置・回収 / 投擲は
    //   すべて GameLogic.dll の C++ スクリプト 3 本で、使っているのは既存 ABI (v15) だけ。
    // ★置き場所は部屋 A の隅。企画 4-2 の「決して消えない開始地点の光」= M65e で置いた
    //   Placed Light がすぐそこにあり、**捕まったときに押し戻される先**でもある。
    //   敵 2 体の経路 (廊下 → 部屋 A の光) から 1.7m (catchRadius) 以上離してあるので、
    //   無入力のスクショ実行で勝手に捕まって絵が動くことはない。
    // ★CC 単体 (ソリッド Collider なし) — 足音のレイが自分に当たると無音になる、という
    //   M65c からの規約。歩行者・敵とまったく同じ組み方。
    // ★**関数の末尾に足す** (以降のエンティティ index を動かさない = 波スロットの
    //   割り当て順と粒子 RNG のストリームを保つ)
    // ★M68a: 耳 (Watcher Ears) を後から**子として**足すために ID だけ持ち出す。
    //   Watcher ブロックの中でエンティティを増やすと以降の index が全部ずれ、
    //   波スロットの割り当て順と粒子 RNG のストリームまで動く (末尾追加の鉄則)
    EntityID watcherId = kNullEntity;
    {
        GameObject player = s.CreateGameObject("Watcher");
        watcherId = player.Id();
        // ★y は敵 2 体と同じ 1.35。**CC の寸法にはスケールが掛かる** ので、
        //   height 1.6 x scale.y 1.6 = 全高 2.56 → 静止時の中心は床天面 + 1.28 になる。
        //   1.0 で置いたら床にめり込んだ状態から始まり、押し出しで浮き上がりながら
        //   歩くという分かりにくい壊れ方をした (probe で発見)
        player.SetLocalPosition(AcousticMapToWorld(1), 1.35f, AcousticMapToWorld(1));
        player.SetLocalScale(0.55f, 1.6f, 0.55f);
        auto* mr = player.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("adem_player") };
        auto* cc = player.AddComponent<CharacterControllerComponent>();
        cc->radius = 0.3f;
        cc->height = 1.6f;
        auto* em = player.AddComponent<AcousticEmitterComponent>();
        // 足音は**床材が大きさを決める** (企画 3-4)。歩幅だけを WatcherFpsCamera が
        // 移動モード (しゃがみ / 歩き / 走り) で書き替える = 企画 3-2 の速度と危険度
        em->autoFootstep = true;
        em->stepDistanceM = 0.9f;
        em->ticksPerRing = 2;
        em->cooldownTicks = 12;
        // ★3 本とも AttachScriptIfRegistered。DLL が焼けていなくてもシーンは成立する
        //   (プレイヤーはただの箱として立っているだけになる)
        AttachScriptIfRegistered(w, player.Id(), "WatcherFpsCamera");
        AttachScriptIfRegistered(w, player.Id(), "WatcherLightTool");
        AttachScriptIfRegistered(w, player.Id(), "WatcherThrowTool");
    }

    // ---- M65g: 携行できる光 3 本 (企画 4-1 の上限そのもの) ----
    // ★★**スクリプトが実行時に生成するのではなく、シーンが用意して使い回す**。
    //   スクリプトから足したコンポーネントは tick 末まで存在しない (EngineAPI.h v14 の明文)
    //   ので、生成した同じ tick に LightComponent へ書いても 1 バイトも入らず、
    //   翌 tick までの 1 フレーム**既定値の平行光** (白 / intensity 1.0) が
    //   シーン全体を照らす = 暗闇のゲームで最悪の閃光になる (probe で実測して設計を変えた)。
    //   使い回しなら構造変更が 1 度も起きず、企画の「同時に持てるのは 3 本」とも一致する。
    // ★床下 (y=-4) に格納しておく。床は y[-1,0] を占めるので俯瞰のスクショには出ず、
    //   intensity 0 なのでライティングにも LightSeeker (minIntensity 0.5) にも寄与しない
    //   = golden は「プレイヤーの箱が増えたぶん」しか動かない
    for (int i = 0; i < 3; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Watcher Lamp %d", i);
        GameObject lamp = s.CreateGameObject(name);
        lamp.SetLocalPosition(0.0f, -4.0f, 0.0f);
        lamp.SetLocalScale(0.3f, 0.3f, 0.3f);
        auto* lmr = lamp.AddComponent<MeshRendererComponent>();
        lmr->mesh = res.meshes.Sphere();
        lmr->material = AssetID{ HashStr("adem_lamp") };
        auto* pl = lamp.AddComponent<LightComponent>();
        pl->type = lighttype::kPoint;
        pl->range = 6.0f;
        pl->intensity = 0.0f; // 消灯 = 手札。設置中にスクリプトが 0 から育てる
        pl->color = { 1.0f, 0.86f, 0.62f }; // 携行灯らしい暖色 (残光の寒色と対になる)
    }

    // ---- M68a: 音響 × 実オーディオ (企画の波の 4 役目 = 耳に届く音) ----
    // ★**関数の末尾に足す**。ここまでの理由と全く同じで、前に挿すと波スロットの
    //   割り当て順と粒子 RNG のストリームが動いて replay 7 ペア目が割れる。
    // ★3 つとも **NoHash / 出力レーン専用** のコンポーネントしか付けない
    //   (AudioListener / AcousticAudio / AudioSource)。だから足しても sim は
    //   1 バイトも変わらない — 「replay も golden も無風」がこの一点に乗っている。
    {
        // 耳は Watcher の**子**として置く。本体に直付けするとアーキタイプが変わり…
        // …とは実は**ならない** (HierarchyComponent は全エンティティが生成時に持つので
        // SetParent は 1 ビットも構造を変えない) のだが、子にしておくと
        // 「頭の高さ」を後から独立に動かせるので、こちらを既定にしてある。
        // 向きは本体の yaw をそのまま継ぐ (WatcherFpsCamera が本体を回す)
        GameObject ears = s.CreateGameObject("Watcher Ears");
        ears.SetParent(GameObject(&w, watcherId));
        ears.SetLocalPosition(0.0f, 0.0f, 0.0f);
        ears.AddComponent<AudioListenerComponent>();
    }
    {
        // 調整卓。既定値のまま = spec §4.2 の表そのもの (耳で詰めるのは M68c の後)
        GameObject tune = s.CreateGameObject("Acoustic Audio");
        auto* ac = tune.AddComponent<AcousticAudioComponent>();
        // M68b: 音色 → .sound.json の名前キー。**GUID を直書きしない**のは Hum と同じ理由で、
        // 名前キーならチェックアウト先に依存しない。physmat の acousticTone の割り当ては
        // 0 = carpet/water/呼吸 / 1 = wood/敵の自発音 / 2 = gravel / 3 = metal/glass
        std::snprintf(ac->toneSound0, sizeof(ac->toneSound0), "step_soft");
        std::snprintf(ac->toneSound1, sizeof(ac->toneSound1), "step_wood");
        std::snprintf(ac->toneSound2, sizeof(ac->toneSound2), "step_hard");
        std::snprintf(ac->toneSound3, sizeof(ac->toneSound3), "step_metal");
    }
    {
        // 部屋 B の持続音。★**部屋 A からは一直線に見えない**位置 (map 6,9) に置くのが
        //   要点で、廊下を進むと「戸口の方向から次第に開いて聞こえる」が体験できる。
        //   MeshRenderer は付けない — 絵が 1 画素も変わらないことが golden 22 枚の条件
        GameObject hum = s.CreateGameObject("Hum");
        hum.SetLocalPosition(AcousticMapToWorld(6), 1.0f, AcousticMapToWorld(9));
        auto* src = hum.AddComponent<AudioSourceComponent>();
        // ★GUID を直書きしない。名前キーで引くのでチェックアウト先にも .meta の作り直しにも
        //   依存しない (M74a 以前のモデル由来サブアセット ID が踏んだ罠と同じ種類を避ける)
        src->sound = AssetID{ ctx.sounds != nullptr ? ctx.sounds->ResolveKey(HashStr("hum")) : 0 };
        src->playOnAwake = true;
    }
}

// M76f: Deep-Modal 衝突音のショーケース (--modal-demo)。参照: RegisterPhysicsShowcaseContent /
// BuildPhysicsShowcaseScene (同じ builtin キューブ + 名前引き physmat の作法)
void RegisterModalShowcaseContent(EngineContext& ctx)
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
    makeMat("mdemo_floor_wood", 0.55f, 0.40f, 0.25f);
    makeMat("mdemo_floor_metal", 0.55f, 0.57f, 0.60f);
    makeMat("mdemo_wood", 0.62f, 0.46f, 0.28f);
    makeMat("mdemo_metal", 0.70f, 0.72f, 0.76f);
    makeMat("mdemo_glass", 0.75f, 0.88f, 0.92f);
    makeMat("mdemo_tile", 0.80f, 0.78f, 0.74f); // sub-10 E: 形状差 (sphere) を見せる 4 個目
    res.meshes.Sphere();
}

void BuildModalShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("modal_showcase");
    const AssetID cube = res.meshes.Cube();
    const AssetID matWood = FindPhysMat("wood");
    const AssetID matMetal = FindPhysMat("metal");
    const AssetID matGlass = FindPhysMat("glass");

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 6.0f, -14.0f);
    camera.SetLocalRotationEuler(18.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    // ---- 物理環境 (シーンに 1 個) ----
    {
        GameObject envGo = s.CreateGameObject("Environment");
        auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
        env->gravity = { 0.0f, -9.81f, 0.0f };
    }

    // ---- 床: 左半分 = 木 / 右半分 = 金属。**AcousticAudio を置かない** (Bypass 経路も踏む) ----
    {
        GameObject floor = s.CreateGameObject("FloorWood");
        floor.SetLocalPosition(-4.0f, -0.5f, 0.0f);
        floor.SetLocalScale(8.0f, 1.0f, 8.0f);
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("mdemo_floor_wood") };
        auto* col = floor.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->physMaterial = matWood;
    }
    {
        GameObject floor = s.CreateGameObject("FloorMetal");
        floor.SetLocalPosition(4.0f, -0.5f, 0.0f);
        floor.SetLocalScale(8.0f, 1.0f, 8.0f);
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("mdemo_floor_metal") };
        auto* col = floor.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->physMaterial = matMetal;
    }

    // ---- 木の箱: 木の床へ低い高さから落ちる (基準の当たり方) ----
    {
        GameObject box = s.CreateGameObject("WoodBox");
        box.SetLocalPosition(-4.0f, 4.0f, 0.0f);
        box.SetLocalScale(1.0f, 1.0f, 1.0f);
        auto* mr = box.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("mdemo_wood") };
        auto* col = box.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->physMaterial = matWood;
        auto* rb = box.AddComponent<RigidbodyComponent>();
        // ★[修正、reviewer round 1 must #1] 旧 `useDensity=true` は 1 m³ の**中実**剛体
        // (wood.physmat.json の density=700 → 700 kg) を作っていた — 「衝突音のショーケース」
        // としては非現実的な質量で、この力積 (J=161〜102164) に合わせて圧縮カーブの指数を
        // 選ぶと現実的な衝突の表現力が犠牲になる (ModalAudio.h の kModalImpulseExponent
        // コメント参照)。小道具サイズの質量を直接指定する — 見た目のサイズは変えない
        // (SetLocalScale はそのまま) が、物理的な重さだけ現実的な範囲へ落とす
        rb->mass = 2.0f; // kg (木箱サイズの小道具相当)
        box.AddComponent<ModalSoundComponent>(); // mesh は空 = MeshRenderer.mesh (Cube) を使う
    }

    // ---- 金属の箱: 金属の床へ低い高さから落ちる (WoodBox と「面で音が変わる」対) ----
    {
        GameObject box = s.CreateGameObject("MetalBox");
        box.SetLocalPosition(3.0f, 4.0f, 0.0f);
        box.SetLocalScale(1.0f, 1.0f, 1.0f);
        auto* mr = box.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("mdemo_metal") };
        auto* col = box.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->physMaterial = matMetal;
        auto* rb = box.AddComponent<RigidbodyComponent>();
        // ★[修正、reviewer round 1 must #1] 旧 `useDensity=true` は 1 m³ 中実 (metal
        // density=7850 → 7,850 kg = 8 トン級) を作っていた。WoodBox と同じ理由で
        // 現実的な質量へ直す (見た目のサイズは不変)
        rb->mass = 4.0f; // kg (金属塊サイズの小道具相当。木より少し重い程度)
        box.AddComponent<ModalSoundComponent>();
    }

    // ---- ガラスの箱: 金属の床へ高い所から強く落ちる (MetalBox と「強く落とすと大きい」対) ----
    {
        GameObject box = s.CreateGameObject("GlassBox");
        box.SetLocalPosition(5.0f, 10.0f, 0.0f);
        box.SetLocalScale(0.8f, 0.8f, 0.8f);
        auto* mr = box.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("mdemo_glass") };
        auto* col = box.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->physMaterial = matGlass;
        auto* rb = box.AddComponent<RigidbodyComponent>();
        // ★[修正、reviewer round 1 must #1] 旧 `useDensity=true` は 0.8³ m³ 中実
        // (glass density=2500 → 約 1,280 kg) を作っていた。ガラスは中身が詰まっていない
        // (置物/瓶サイズ) 想定で軽くする — MetalBox より軽いぶんは、落下高さの差
        // (y=4 → y=10) がもたらす速度差で埋め合わせ、なお「強く落とすと大きい」対になる
        rb->mass = 1.0f; // kg (ガラス製の小物相当)
        box.AddComponent<ModalSoundComponent>();
    }

    // ---- タイルの球: 金属の床へ落ちる (spec sub-10 E、reviewer round 1 指摘 7)。
    //     ★生成順の**末尾**に置く (このリポジトリの流儀: 末尾へ足せば既存の replay/golden に
    //     影響しない)。WoodBox/MetalBox/GlassBox は 3 個とも Cube = 同一メッシュだったため、
    //     「形状で音が変わる」の機構自体は .msfm の解析で確認できても、デモでは見せられなかった ----
    {
        const AssetID sphere = res.meshes.Sphere();
        GameObject ball = s.CreateGameObject("TileSphere");
        ball.SetLocalPosition(-1.0f, 6.0f, 3.0f);
        ball.SetLocalScale(1.0f, 1.0f, 1.0f);
        auto* mr = ball.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("mdemo_tile") };
        auto* col = ball.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->radius = 0.5f;
        col->physMaterial = FindPhysMat("tile");
        auto* rb = ball.AddComponent<RigidbodyComponent>();
        // ★[修正、reviewer round 1 must #1] 半径 0.5m の中実球 (tile density=2400 相当) は
        // useDensity だと約 1,257 kg になる。小道具サイズの質量へ直す (他 3 個と同じ理由)
        rb->mass = 1.5f; // kg (タイル玉サイズの小道具相当)
        auto* modal = ball.AddComponent<ModalSoundComponent>();
        modal->mesh = sphere; // MeshRenderer.mesh と同じだが、球であることを明示しておく
    }
}

} // namespace mye
