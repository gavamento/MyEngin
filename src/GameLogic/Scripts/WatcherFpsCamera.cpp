// 企画 (三校企画.md) の一人称操作 (M65g)。**エンジンには 1 行も足していない** —
// 視点は v15 の GetMouseDelta / SetCursorMode (M64a で開通して以来これが初の利用者)、
// 移動は v5 の CharacterMove、足音は M65c の autoFootstep がそのまま担当する。
//
// ★**既定ではカメラを 1 バイトも触らない**。`--acoustic-demo` の golden 2 枚は俯瞰で
//   撮っているので、「マウスが窓の上を通っただけで一人称に切り替わる」実装にすると
//   スクショ回帰が理由不明に揺れる (撮影中に手がマウスに触れただけで赤くなる)。
//   切り替えは **V キーのトグル 1 本**だけ = 事故では入らない入力に限定してある。
// ★視点角は登録フィールド = ハッシュ対象 = .rep 被覆。`--synth-input` が生デルタを
//   流すので、replay 7 ペア目が「同じデルタ列から同じ角度が出る」ことを毎回検査する
//   (M64a の ABI に実走の被覆が付くのはここが初めて)。
// ★呼吸 (企画 3-3) もここ。止まっているあいだだけ極小の波を出す — 「完全な無音には
//   なれない」が企画の主張で、同時に**暗闇で自分の足元だけは見える**ことの実装でもある。
#include "Shared/ScriptAPI.h"

namespace {
// <Windows.h> を引き込まないため VK コードを直接定義 (PlayerController.cpp と同じ流儀)
constexpr uint8_t kVkShift = 0x10;
constexpr uint8_t kVkControl = 0x11;
constexpr uint8_t kVkV = 0x56; // 一人称 / 俯瞰の切り替え

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg2Rad = kPi / 180.0f;

// ★**sin / cos を CRT から取らない**。`Physics\AeroSampling.cpp` の注記が正本で、
//   「std::cos / std::sin は CRT 実装依存でビットが動きうる」。視点角はハッシュ対象の
//   フィールドから移動速度まで一直線に流れるので、ここに CRT 依存を挟むと
//   「別の Windows で .rep が再生できない」種類の壊れ方になる。乗算と加算だけの
//   多項式なら /fp:precise の下でどのビルドでも厳密に同じビット列になる。
//   前提: |x| <= 3pi/2 (半角と Cos の +pi/2 しか渡さないので満たされる)。sin(x)=sin(pi-x)
//   の対称性で [-pi/2, pi/2] へ折り返し、
//   9 次のテイラー (この区間で誤差 1e-9 未満 = 視点には過剰なほど)
float Sin(float x)
{
    if (x > kPi * 0.5f) {
        x = kPi - x;
    } else if (x < -kPi * 0.5f) {
        x = -kPi - x;
    }
    const float x2 = x * x;
    return x
        * (1.0f
           + x2
               * (-1.0f / 6.0f
                  + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}
float Cos(float x)
{
    return Sin(x + kPi * 0.5f);
}

// AcousticEmitterComponent (Engine/Core/Components.h) の名前ハッシュ。
// **毎 tick 取り直さない** (WavePinger と同じ流儀)
const uint64_t kCompEmitter = MyeNameHash("AcousticEmitter");
const uint64_t kFieldStride = MyeNameHash("stepDistanceM");
const uint64_t kFieldLoudness = MyeNameHash("pendingLoudness");
const uint64_t kFieldRadius = MyeNameHash("pendingRadiusM");
const uint64_t kFieldTone = MyeNameHash("pendingTone");
const uint64_t kFieldTicksPerRing = MyeNameHash("ticksPerRing");

// ★**登録フィールドは 16 本まで** (ScriptAPI.h の MYE_SF_FOREACH)。上限は窮屈に見えるが、
//   「動かないものを ECS に置かない」線引きを強制してくれる — 調整値をフィールドで持つと
//   snapshot に載りハッシュに入り、DLL リロードで**古い値が生き残る**。定数はここへ置く
constexpr float kPitchLimitDeg = 80.0f;
constexpr float kWalkStrideM = 0.9f;   // 企画 3-2: 速度は歩幅 (= 波の間隔) で表す
constexpr float kRunStrideM = 0.55f;
constexpr float kCrouchStrideM = 1.7f;
constexpr float kJumpSpeed = 4.2f;
constexpr int32_t kBreathTicks = 96;   // 1.6 秒に 1 回 (企画 3-3)
constexpr float kBreathRadiusM = 3.0f; // 10m 先の耳 (閾値 0.0015) には届かない大きさ
// ★視点は箱 (半高 0.8) の**上へ十分に**出す。0.95 だと箱の天面が画面下端に帯として
//   映り込んだ (probe のスクショで発見) — 縦画角の半分 30 度に対し、0.275m 横の天面が
//   0.15m 下では 29 度にしかならないため。1.10 なら 42 度で画角の外へ落ちる
constexpr float kEyeHeight = 1.10f;
} // namespace

struct WatcherFpsCamera : Script<WatcherFpsCamera> {
    // ---- 視点 ----
    // ★感度は**必ずフィールドで持つ**。GetMouseDelta が返すのは生のマウスカウントで
    //   DPI は機種依存なので、定数を直書きするとマウスを替えただけで別のゲームになる
    //   (EngineAPI.h の v15 の注記)
    float lookSensDeg = 0.06f; // 1 カウントあたりの回転量 [度]
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;

    // ---- 移動 (企画 3-2: 速度がそのまま危険度になる) ----
    // ★**歩幅**で速度差を表す。音の大きさそのものは床材が決める (企画 3-4 = M65c) ので、
    //   走ると「同じ大きさの波がより短い間隔で出る」形になる。振幅まで速度で変えるには
    //   エミッタ側に係数フィールドが要る = エンジンの変更なので v1 の境界の外
    float walkSpeed = 2.2f;
    float runSpeed = 4.4f;
    float crouchSpeed = 1.0f;

    // ---- 呼吸 (企画 3-3) ----
    int32_t breathPhase = 0;
    float breathLoudness = 0.07f;

    // ---- カメラ (絵の側。sim には出ない) ----
    int32_t firstPerson = 0;
    int32_t prevViewKey = 0;
    int32_t cursorMode = 0;
    MyeEntityId camera = {};

    void Update(MyeUpdateContext& ctx)
    {
        const MyeEngineApi* api = ctx.api;

        // ---- 視点 ----
        int32_t dx = 0, dy = 0;
        api->GetMouseDelta(api->engine, &dx, &dy);
        yawDeg += static_cast<float>(dx) * lookSensDeg;
        pitchDeg += static_cast<float>(dy) * lookSensDeg; // 下向きが正 (画面座標と同じ)
        // 折り返しは 1 回で足りる (1 tick の回転量が 360 度を超えることはない)
        if (yawDeg > 180.0f) {
            yawDeg -= 360.0f;
        } else if (yawDeg < -180.0f) {
            yawDeg += 360.0f;
        }
        if (pitchDeg > kPitchLimitDeg) {
            pitchDeg = kPitchLimitDeg;
        } else if (pitchDeg < -kPitchLimitDeg) {
            pitchDeg = -kPitchLimitDeg;
        }

        // 角度 → クォータニオン。GameObject::SetLocalRotationEuler (=
        // XMQuaternionRotationRollPitchYaw) と**同じ並び**になるよう手で組んである
        // (roll=0 のとき x=sp*cy / y=cp*sy / z=-sp*sy / w=cp*cy)
        const float hp = pitchDeg * kDeg2Rad * 0.5f;
        const float hy = yawDeg * kDeg2Rad * 0.5f;
        const float sp = Sin(hp), cp = Cos(hp);
        const float sy = Sin(hy), cy = Cos(hy);
        const MyeQuat rot = { sp * cy, cp * sy, -sp * sy, cp * cy };
        MyeGameObject self = MyeSelf(ctx);
        self.SetLocalRotation(rot);
        // ★体ごと向く。俯瞰の絵でも「どちらを見ているか」が読めるし、投擲・設置の
        //   スクリプトは**この回転から前方を導く** (角度を持つのはここ 1 箇所)

        // ---- 移動 ----
        const bool run = api->KeyDown(api->engine, kVkShift) != 0;
        const bool crouch = api->KeyDown(api->engine, kVkControl) != 0;
        const float speed = crouch ? crouchSpeed : (run ? runSpeed : walkSpeed);
        const float stride = crouch ? kCrouchStrideM : (run ? kRunStrideM : kWalkStrideM);
        const float ax = MyeAxis(ctx, "MoveX");
        const float ay = MyeAxis(ctx, "MoveY");
        // yaw だけで水平面へ落とす (見上げても前進速度が落ちないようにする)
        const float fwdX = Sin(yawDeg * kDeg2Rad), fwdZ = Cos(yawDeg * kDeg2Rad);
        const float vx = (fwdZ * ax + fwdX * ay) * speed;
        const float vz = (-fwdX * ax + fwdZ * ay) * speed;
        api->CharacterMove(api->engine, ctx.self, { vx, 0.0f, vz });
        if (MyeActionPressed(ctx, "Jump")) {
            api->CharacterJump(api->engine, ctx.self, kJumpSpeed);
        }
        MyeSetField(ctx, ctx.self, kCompEmitter, kFieldStride, stride);

        // ---- 呼吸 (企画 3-3) ----
        // ★止まっているあいだ**だけ**数える。歩いていれば足音が出るので、両方鳴らすと
        //   明示要求 (呼吸) が自動足音を踏み潰して床材が絵から消える (M65c の「明示 > 自動」)
        const bool still = (ax > -0.05f && ax < 0.05f) && (ay > -0.05f && ay < 0.05f);
        if (!still) {
            breathPhase = 0;
        } else if (++breathPhase >= kBreathTicks) {
            breathPhase = 0;
            MyeSetField(ctx, ctx.self, kCompEmitter, kFieldTicksPerRing, int32_t{ 2 });
            MyeSetField(ctx, ctx.self, kCompEmitter, kFieldTone, int32_t{ 0 });
            MyeSetField(ctx, ctx.self, kCompEmitter, kFieldRadius, kBreathRadiusM);
            MyeSetField(ctx, ctx.self, kCompEmitter, kFieldLoudness, breathLoudness);
        }

        // ---- カメラ (ここから下は絵の側。sim のハッシュには 1 ビットも出ない) ----
        const int32_t viewKey = api->KeyDown(api->engine, kVkV);
        if (viewKey && !prevViewKey) {
            firstPerson = firstPerson ? 0 : 1;
            // SetCursorMode の作法は「Escape (= Pause) を見たら 0、再開の意思表示で 1」
            // (EngineAPI.h v15)。毎 tick 1 を書き続けると Escape の逃げ道を握り潰す
            cursorMode = firstPerson;
            api->SetCursorMode(api->engine, cursorMode);
        }
        prevViewKey = viewKey;
        if (MyeActionPressed(ctx, "Pause") && cursorMode != 0) {
            cursorMode = 0;
            api->SetCursorMode(api->engine, 0);
        }
        if (!firstPerson) {
            return; // ★俯瞰のまま = golden を撮る経路。カメラには触らない
        }
        if (MyeEntityIdIsNull(camera) || !api->IsAlive(api->engine, camera)) {
            camera = api->FindByName(api->engine, "Main Camera");
            if (MyeEntityIdIsNull(camera)) {
                return;
            }
        }
        MyeVec3 p = self.GetLocalPosition();
        p.y += kEyeHeight;
        api->SetLocalPosition(api->engine, camera, p);
        api->SetLocalRotation(api->engine, camera, rot);
    }
};
REGISTER_SCRIPT(WatcherFpsCamera,
                FIELDS(lookSensDeg, yawDeg, pitchDeg, walkSpeed, runSpeed, crouchSpeed,
                       breathPhase, breathLoudness, firstPerson, prevViewKey, cursorMode, camera));
