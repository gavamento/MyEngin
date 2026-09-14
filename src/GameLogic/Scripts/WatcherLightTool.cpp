// 企画 (三校企画.md) §4 の「光を置く / 回収する」(M65g)。**エンジンには 1 行も足していない** —
// 光は既存の `LightComponent` の強度を上げ下げするだけで、M65f の `LightSeekerComponent` は
// **それをそのまま見つける** (新しい「光」概念を作らない = M65 の判断 7)。
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
// ★捕まると最後に設置した残存光を消費し、その設置時の足場へ復活する。敵と光は名前で引いている —
//   デモ専用の割り切りで、ゲームとして作るならタグ検索が要る。
#include <cmath>

#include "Shared/ScriptAPI.h"

namespace {
const uint64_t kCompLight = MyeNameHash("Light");
const uint64_t kFieldIntensity = MyeNameHash("intensity");
const uint64_t kFieldSafeRadius = MyeNameHash("safeRadius");

// 登録フィールドは32本まで。動かない調整値は
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
    // 開始位置の有効性は startCaptured で保持する。原点も有効な開始位置。
    MyeVec3 startPos = {};
    int32_t caughtGrace = 0;
    MyeEntityId agent0 = {};
    MyeEntityId agent1 = {};
    // 1 が最新。設置のたびに残存光だけ順位をずらすので tick/整数の桁溢れがない。
    int32_t order0 = 0, order1 = 0, order2 = 0;
    MyeVec3 respawn0 = {}, respawn1 = {}, respawn2 = {};
    bool startCaptured = false;
    float safeRadius = 2.5f;

    int32_t& Order(int32_t i)
    {
        int32_t* values[3] = { &order0, &order1, &order2 };
        return *values[i];
    }
    MyeVec3& RespawnPosition(int32_t i)
    {
        MyeVec3* values[3] = { &respawn0, &respawn1, &respawn2 };
        return *values[i];
    }

    bool ClearPath(MyeUpdateContext& ctx, const MyeVec3& from, const MyeVec3& to,
                   MyeEntityId target)
    {
        const MyeVec3 delta = { to.x - from.x, to.y - from.y, to.z - from.z };
        const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (distance < 0.001f) {
            return true;
        }
        MyeRaycastHit hit = {};
        return !ctx.api->Raycast(ctx.api->engine, from,
            { delta.x / distance, delta.y / distance, delta.z / distance }, distance, &hit)
            || (hit.entity.index == target.index && hit.entity.generation == target.generation);
    }

    bool IsProtected(MyeUpdateContext& ctx, const MyeVec3& pos)
    {
        if (safeRadius <= 0.0f) {
            return false;
        }
        for (int32_t i = 0; i < 3; ++i) {
            if (*State(i) != kLampPlaced || !ctx.api->IsAlive(ctx.api->engine, *Lamp(i))) {
                continue;
            }
            MyeVec3 lp = {};
            ctx.api->GetLocalPosition(ctx.api->engine, *Lamp(i), &lp);
            if (std::abs(pos.y - lp.y) > 3.0f || Dist2XZ(pos, lp) > safeRadius * safeRadius) {
                continue;
            }
            lp.y = pos.y;
            if (ClearPath(ctx, pos, lp, *Lamp(i))) {
                return true;
            }
        }
        return false;
    }

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
        if (!startCaptured) {
            startPos = pos; // 開始地点 = 企画 4-2 の「決して消えない光」がある場所
            startCaptured = true;
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

        // ---- 捕まる → 最新の残存光を消費して復活 (無ければ開始位置) ----
        if (caughtGrace > 0) {
            --caughtGrace;
        } else if (!IsProtected(ctx, pos)) {
            const MyeEntityId agents[2] = { agent0, agent1 };
            for (const MyeEntityId& a : agents) {
                if (MyeEntityIdIsNull(a) || !api->IsAlive(api->engine, a)) {
                    continue;
                }
                MyeVec3 ap = {};
                api->GetLocalPosition(api->engine, a, &ap);
                if (Dist2XZ(pos, ap) > kCatchRadiusM * kCatchRadiusM
                    || std::abs(pos.y - ap.y) > kCatchRadiusM || !ClearPath(ctx, pos, ap, a)) {
                    continue;
                }
                Abort(ctx);
                const MyeVec3 destination = ConsumeRespawnLight(ctx);
                self.SetLocalPosition(destination);
                api->CharacterMove(api->engine, ctx.self, { 0.0f, 0.0f, 0.0f });
                caughtGrace = kGraceTicks;
                MyeLogf(ctx, "watcher: caught - carried=%d", CarriedCount());
                return;
            }
        }

        // ---- 設置 / 回収 ----
        const bool held = MyeActionHeld(ctx, "WatcherLight");
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
                const int32_t oldOrder[3] = { order0, order1, order2 };
                for (int32_t i = 0; i < 3; ++i) {
                    if (*State(i) == kLampPlaced && Order(i) > 0) {
                        Order(i) = 2;
                        for (int32_t j = 0; j < 3; ++j) {
                            if (*State(j) == kLampPlaced && oldOrder[j] > 0
                                && oldOrder[j] < oldOrder[i]) {
                                ++Order(i);
                            }
                        }
                    }
                }
                Order(busyIdx) = 1;
                RespawnPosition(busyIdx) = pos;
                *State(busyIdx) = kLampPlaced;
                MyeSetField(ctx, *lamp, kCompLight, kFieldSafeRadius, safeRadius);
                mode = 0;
                progress = 0;
                busyIdx = -1;
                MyeLogf(ctx, "watcher: light placed - carried=%d", CarriedCount());
            }
            return;
        }
        // 回収完了まで照明と光センサーへの入力を維持する。
        MyeSetField(ctx, *lamp, kCompLight, kFieldIntensity, lightIntensity);
        if (progress >= retrieveTicks) {
            Stow(ctx, busyIdx);
            *State(busyIdx) = kLampCarried;
            Order(busyIdx) = 0;
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
            if (*State(i) != kLampPlaced || MyeEntityIdIsNull(*Lamp(i))
                || !ctx.api->IsAlive(ctx.api->engine, *Lamp(i))) {
                continue;
            }
            MyeVec3 lp = {};
            ctx.api->GetLocalPosition(ctx.api->engine, *Lamp(i), &lp);
            if (std::abs(pos.y - lp.y) > 3.0f) {
                continue;
            }
            MyeVec3 sight = lp;
            sight.y = pos.y;
            if (!ClearPath(ctx, pos, sight, *Lamp(i))) {
                continue;
            }
            const float d2 = Dist2XZ(pos, lp);
            if (d2 < bestD2) {
                bestD2 = d2;
                best = i;
            }
        }
        return best;
    }

    // 完成した残存光だけを消費する。携行品や設置途中の光には触らない。
    MyeVec3 ConsumeRespawnLight(MyeUpdateContext& ctx)
    {
        int32_t idx = -1;
        for (int32_t i = 0; i < 3; ++i) {
            if (*State(i) == kLampPlaced && Order(i) > 0
                && ctx.api->IsAlive(ctx.api->engine, *Lamp(i))
                && (idx < 0 || Order(i) < Order(idx))) {
                idx = i;
            }
        }
        if (idx < 0) {
            return startPos;
        }
        const MyeVec3 destination = RespawnPosition(idx);
        Stow(ctx, idx);
        *State(idx) = kLampLost;
        Order(idx) = 0;
        return destination;
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
        MyeSetField(ctx, *lamp, kCompLight, kFieldSafeRadius, 0.0f);
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
                FIELDS(MYE_F_RANGE(placeTicks, "設置にかかる tick", 1.0f, 600.0f),
                       MYE_F_RANGE(retrieveTicks, "回収にかかる tick", 1.0f, 600.0f),
                       MYE_F_JP(mode, "動作 (0=待機 1=設置 2=回収)"), MYE_F_JP(progress, "進捗 (tick)"),
                       MYE_F_JP(busyIdx, "作業中のランプ番号"), MYE_F_JP(lamp0, "ランプ 1"),
                       MYE_F_JP(lamp1, "ランプ 2"), MYE_F_JP(lamp2, "ランプ 3"),
                       MYE_F_JP(state0, "ランプ 1 の状態"), MYE_F_JP(state1, "ランプ 2 の状態"),
                       MYE_F_JP(state2, "ランプ 3 の状態"),
                       MYE_F_RANGE(lightIntensity, "光の強さ", 0.0f, 20.0f),
                       MYE_F_JP(startPos, "開始位置"), MYE_F_JP(caughtGrace, "捕捉の猶予 (tick)"),
                       MYE_F_JP(agent0, "敵 1"), MYE_F_JP(agent1, "敵 2"),
                       MYE_F_JP(order0, "光1の設置順位"), MYE_F_JP(order1, "光2の設置順位"),
                       MYE_F_JP(order2, "光3の設置順位"), MYE_F_JP(respawn0, "光1の復活位置"),
                       MYE_F_JP(respawn1, "光2の復活位置"), MYE_F_JP(respawn2, "光3の復活位置"),
                       MYE_F_JP(startCaptured, "開始位置取得済み"),
                       MYE_F_RANGE(safeRadius, "安全半径", 0.0f, 10.0f)));
