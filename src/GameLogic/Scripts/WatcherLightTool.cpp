// 企画 (三校企画.md) §4 の「光を置く / 回収する」(M65g)。**エンジンには 1 行も足していない** —
// 光は既存の `LightComponent` の強度を上げ下げするだけで、M65f の `LightSeekerComponent` は
// **それをそのまま見つける** (新しい「光」概念を作らない、という M65 の判断 7 がここで報われる)。
//
// ★★**光は実行時に生成しない**。シーンが 3 個 (企画 4-1 の携行上限) を床下に用意していて、
//   設置は「それを目の前へ動かして強度を 0 から育てる」だけ。理由は 2 つ:
//   (a) スクリプトから足したコンポーネントは**tick 末まで存在しない** (EngineAPI.h v14 の
//       明文。probe でも実測した) ので、生成した同じ tick に LightComponent へ書いても
//       1 バイトも入らない。翌 tick に書くまでの 1 フレーム、**既定値の平行光**
//       (白 / intensity 1.0 / ambient 付き) がシーン全体を照らす = 暗闇のゲームで最悪の閃光。
//   (b) 使い回しなら構造変更が 1 度も起きない。企画の「同時に持てるのは最大 3 本」は
//       そもそも固定プールそのものなので、設計としても素直。
// ★**ゲージを出さない** (企画 4-3)。進行は「光そのものが徐々に強くなる」ことだけで表す。
//   おかげで「育っている途中の弱い光にも光の敵は反応を始める」が、
//   LightSeeker.minIntensity の閾値をまたぐ瞬間として**自動的に**成立する。
// ★時間は整数 tick で数える。秒の float 累積は加算順で割れる (規則 2)。
// ★設置中に動くと中断する (企画 4-3)。**中断しても光は失われない**。
// ★捕まると光を 1 本失って開始地点へ戻される (企画 7 の 7 段目)。敵と光は名前で引いている —
//   デモ専用の割り切りで、ゲームとして作るならタグ検索が要る。
#include <cmath>

#include "Shared/ScriptAPI.h"

namespace {
constexpr uint8_t kVkF = 0x46; // 設置 / 回収 (押しっぱなし)

const uint64_t kCompLight = MyeNameHash("Light");
const uint64_t kFieldIntensity = MyeNameHash("intensity");

// ★**登録フィールドは 16 本まで** (ScriptAPI.h の MYE_SF_FOREACH)。動かない調整値は
//   ここへ置く — フィールドにすると snapshot / ハッシュ / DLL リロードの復元に載る
constexpr float kReachM = 2.4f;      // 回収に近づく必要のある距離
constexpr float kPlaceAheadM = 1.2f; // 足元ではなく少し前に置く (自分の箱に埋まらない)
constexpr float kLampDropM = 1.1f;   // 中心から足元まで (CC の全高はスケール込みで 2.56m)
constexpr float kStowY = -4.0f;      // 手札の格納高さ。床 (y[-1,0]) の下 = 見えない
constexpr float kCatchRadiusM = 1.7f;
constexpr int32_t kGraceTicks = 120; // 押し戻された直後に連続で捕まらないための猶予

// ランプの状態
constexpr int32_t kLampCarried = 0;
constexpr int32_t kLampPlaced = 1;
constexpr int32_t kLampLost = 2; // 捕まって失った (企画 7 の 7 段目)

// 距離の 2 乗 (水平のみ)。高さを混ぜると、床に置いた光と目線の差だけで届かなくなる
float Dist2XZ(const MyeVec3& a, const MyeVec3& b)
{
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}
} // namespace

struct WatcherLightTool : Script<WatcherLightTool> {
    // ---- 時間 (整数 tick) ----
    int32_t placeTicks = 150;    // 2.5 秒 (企画 4-3)
    int32_t retrieveTicks = 300; // 5.0 秒 (企画 4-4: 設置より長い)
    int32_t mode = 0;            // 0=待機 1=設置中 2=回収中
    int32_t progress = 0;
    int32_t busyIdx = -1; // 設置 / 回収の対象 (ランプの添字)

    // ---- 携行できる光 3 本 (企画 4-1)。シーンが用意した実体を使い回す ----
    MyeEntityId lamp0 = {};
    MyeEntityId lamp1 = {};
    MyeEntityId lamp2 = {};
    int32_t state0 = kLampCarried;
    int32_t state1 = kLampCarried;
    int32_t state2 = kLampCarried;
    float lightIntensity = 2.2f;

    // ---- 失敗 (企画 7 の 7 段目) ----
    // ★startPos は「厳密に原点なら未取得」で代用する (M65f の AgentBrain.home と同じ手)。
    //   間取りの原点 (0,0,0) は壁の中なので、プレイヤーがそこに立つことはない
    MyeVec3 startPos = {};
    int32_t caughtGrace = 0;
    MyeEntityId agent0 = {};
    MyeEntityId agent1 = {};

    MyeEntityId* Lamp(int32_t i)
    {
        MyeEntityId* l[3] = { &lamp0, &lamp1, &lamp2 };
        return (i >= 0 && i < 3) ? l[i] : nullptr;
    }
    int32_t* State(int32_t i)
    {
        int32_t* s[3] = { &state0, &state1, &state2 };
        return (i >= 0 && i < 3) ? s[i] : nullptr;
    }
    int32_t CarriedCount()
    {
        int32_t n = 0;
        for (int32_t i = 0; i < 3; ++i) {
            n += (*State(i) == kLampCarried) ? 1 : 0;
        }
        return n;
    }

    void Update(MyeUpdateContext& ctx)
    {
        const MyeEngineApi* api = ctx.api;
        MyeGameObject self = MyeSelf(ctx);
        const MyeVec3 pos = self.GetLocalPosition();
        if (startPos.x == 0.0f && startPos.y == 0.0f && startPos.z == 0.0f) {
            startPos = pos; // 開始地点 = 企画 4-2 の「決して消えない光」がある場所
        }
        if (MyeEntityIdIsNull(lamp0) && MyeEntityIdIsNull(lamp1) && MyeEntityIdIsNull(lamp2)) {
            // ★1 度だけ引く (見つからなくても諦める)。このスクリプトが音響ショーケース
            //   以外に付いたときに毎 tick 全走査するのを避ける
            lamp0 = api->FindByName(api->engine, "Watcher Lamp 0");
            lamp1 = api->FindByName(api->engine, "Watcher Lamp 1");
            lamp2 = api->FindByName(api->engine, "Watcher Lamp 2");
            agent0 = api->FindByName(api->engine, "Agent Ear");
            agent1 = api->FindByName(api->engine, "Agent Eye");
        }

        // ---- 捕まる → 光を 1 本失い、開始地点まで押し戻される ----
        if (caughtGrace > 0) {
            --caughtGrace;
        } else {
            const MyeEntityId agents[2] = { agent0, agent1 };
            for (const MyeEntityId& a : agents) {
                if (MyeEntityIdIsNull(a) || !api->IsAlive(api->engine, a)) {
                    continue;
                }
                MyeVec3 ap = {};
                api->GetLocalPosition(api->engine, a, &ap);
                if (Dist2XZ(pos, ap) > kCatchRadiusM * kCatchRadiusM) {
                    continue;
                }
                LoseOneLight(ctx);
                Abort(ctx);
                self.SetLocalPosition(startPos);
                caughtGrace = kGraceTicks;
                MyeLogf(ctx, "watcher: caught - carried=%d", CarriedCount());
                return;
            }
        }

        // ---- 設置 / 回収 ----
        const bool held = api->KeyDown(api->engine, kVkF) != 0;
        const float ax = MyeAxis(ctx, "MoveX");
        const float ay = MyeAxis(ctx, "MoveY");
        const bool moving = (ax < -0.05f || ax > 0.05f) || (ay < -0.05f || ay > 0.05f);
        if (!held || moving) {
            Abort(ctx); // ★移動で中断。**光は失われない** (失うのは時間だけ)
            return;
        }
        if (mode == 0) {
            const int32_t nearIdx = NearestPlaced(ctx, pos);
            if (nearIdx >= 0) {
                mode = 2;
                busyIdx = nearIdx;
                progress = 0;
            } else {
                const int32_t freeIdx = FirstWithState(kLampCarried);
                if (freeIdx < 0) {
                    return; // 手札が無い
                }
                busyIdx = freeIdx;
                mode = 1;
                progress = 0;
                PlaceAt(ctx, busyIdx, pos); // 目の前へ動かして強度 0 から始める
            }
        }
        MyeEntityId* lamp = Lamp(busyIdx);
        if (lamp == nullptr || MyeEntityIdIsNull(*lamp) || !api->IsAlive(api->engine, *lamp)) {
            Abort(ctx);
            return;
        }
        ++progress;
        if (mode == 1) {
            // 育つ = 進行の唯一の表示 (企画 4-3: 画面にゲージは出さない)
            const float t = (placeTicks > 0)
                ? (static_cast<float>(progress) / static_cast<float>(placeTicks))
                : 1.0f;
            MyeSetField(ctx, *lamp, kCompLight, kFieldIntensity, lightIntensity * t);
            if (progress >= placeTicks) {
                *State(busyIdx) = kLampPlaced;
                mode = 0;
                progress = 0;
                busyIdx = -1;
                MyeLogf(ctx, "watcher: light placed - carried=%d", CarriedCount());
            }
            return;
        }
        const float t = (retrieveTicks > 0)
            ? (static_cast<float>(progress) / static_cast<float>(retrieveTicks))
            : 1.0f;
        MyeSetField(ctx, *lamp, kCompLight, kFieldIntensity, lightIntensity * (1.0f - t));
        if (progress >= retrieveTicks) {
            Stow(ctx, busyIdx);
            *State(busyIdx) = kLampCarried;
            mode = 0;
            progress = 0;
            busyIdx = -1;
            MyeLogf(ctx, "watcher: light retrieved - carried=%d", CarriedCount());
        }
    }

    // 中断。**完成前なら手札のまま**なので、失うのは時間だけ (企画 4-4)
    void Abort(MyeUpdateContext& ctx)
    {
        if (mode == 1 && busyIdx >= 0) {
            Stow(ctx, busyIdx); // 育ちかけは床下へ戻す (置いた扱いにしない)
        } else if (mode == 2 && busyIdx >= 0) {
            // 回収を諦めたら強度を戻す (途中で暗いまま放置しない)
            MyeEntityId* lamp = Lamp(busyIdx);
            if (lamp != nullptr && !MyeEntityIdIsNull(*lamp)
                && ctx.api->IsAlive(ctx.api->engine, *lamp)) {
                MyeSetField(ctx, *lamp, kCompLight, kFieldIntensity, lightIntensity);
            }
        }
        mode = 0;
        progress = 0;
        busyIdx = -1;
    }

    int32_t FirstWithState(int32_t want)
    {
        for (int32_t i = 0; i < 3; ++i) {
            if (*State(i) == want && !MyeEntityIdIsNull(*Lamp(i))) {
                return i;
            }
        }
        return -1;
    }

    // 置いた光のうち kReachM 以内で最も近いもの。**同点は添字の小さい方** (決定論)
    int32_t NearestPlaced(MyeUpdateContext& ctx, const MyeVec3& pos)
    {
        int32_t best = -1;
        float bestD2 = kReachM * kReachM;
        for (int32_t i = 0; i < 3; ++i) {
            if (*State(i) != kLampPlaced || MyeEntityIdIsNull(*Lamp(i))) {
                continue;
            }
            MyeVec3 lp = {};
            ctx.api->GetLocalPosition(ctx.api->engine, *Lamp(i), &lp);
            const float d2 = Dist2XZ(pos, lp);
            if (d2 < bestD2) {
                bestD2 = d2;
                best = i;
            }
        }
        return best;
    }

    // 1 本失う。置いてあるものが先 (**添字の小さい方から** = 決定論)、無ければ手札から
    void LoseOneLight(MyeUpdateContext& ctx)
    {
        int32_t idx = FirstWithState(kLampPlaced);
        if (idx < 0) {
            idx = FirstWithState(kLampCarried);
        }
        if (idx < 0) {
            return; // もう 1 本も無い
        }
        Stow(ctx, idx);
        *State(idx) = kLampLost;
    }

    // 床下へ戻して消灯する (構造変更は 1 度も起きない)
    void Stow(MyeUpdateContext& ctx, int32_t i)
    {
        MyeEntityId* lamp = Lamp(i);
        if (lamp == nullptr || MyeEntityIdIsNull(*lamp)
            || !ctx.api->IsAlive(ctx.api->engine, *lamp)) {
            return;
        }
        MyeVec3 p = {};
        ctx.api->GetLocalPosition(ctx.api->engine, *lamp, &p);
        p.y = kStowY;
        ctx.api->SetLocalPosition(ctx.api->engine, *lamp, p);
        MyeSetField(ctx, *lamp, kCompLight, kFieldIntensity, 0.0f);
    }

    // 目の前の床へ移す。前方は**体の回転から導く** (角度を持つのは WatcherFpsCamera だけ)
    void PlaceAt(MyeUpdateContext& ctx, int32_t i, const MyeVec3& pos)
    {
        const MyeEngineApi* api = ctx.api;
        MyeEntityId* lamp = Lamp(i);
        if (lamp == nullptr || MyeEntityIdIsNull(*lamp) || !api->IsAlive(api->engine, *lamp)) {
            return;
        }
        // fwd = (2(xz+wy), 2(yz-wx), 1-2(x^2+y^2)) の水平成分
        MyeQuat q = {};
        api->GetLocalRotation(api->engine, ctx.self, &q);
        float fx = 2.0f * (q.x * q.z + q.w * q.y);
        float fz = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        const float len2 = fx * fx + fz * fz;
        if (len2 > 1e-6f) {
            // 正規化の sqrt は IEEE-754 で正しく丸められる = 構成に依らない
            const float inv = 1.0f / std::sqrt(len2);
            fx *= inv;
            fz *= inv;
        } else {
            fx = 0.0f;
            fz = 1.0f;
        }
        const MyeVec3 p = { pos.x + fx * kPlaceAheadM, pos.y - kLampDropM,
                            pos.z + fz * kPlaceAheadM };
        api->SetLocalPosition(api->engine, *lamp, p);
        MyeSetField(ctx, *lamp, kCompLight, kFieldIntensity, 0.0f); // ここから育つ
    }
};
REGISTER_SCRIPT(WatcherLightTool,
                FIELDS(placeTicks, retrieveTicks, mode, progress, busyIdx, lamp0, lamp1, lamp2,
                       state0, state1, state2, lightIntensity, startPos, caughtGrace, agent0,
                       agent1));
