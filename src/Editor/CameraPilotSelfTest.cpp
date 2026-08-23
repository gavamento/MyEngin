#include "Editor/CameraPilotSelfTest.h"

#include <cmath>

#include <DirectXMath.h>

#include "Editor/CameraPilot.h"
#include "Engine/Core/Log.h"

using namespace DirectX;

namespace mye {
namespace {

XMVECTOR Axis(FXMVECTOR rot, float x, float y, float z)
{
    return XMVector3Rotate(XMVectorSet(x, y, z, 0.0f), rot);
}

bool VecNear(FXMVECTOR a, FXMVECTOR b, float tol = 1e-4f)
{
    return XMVectorGetX(XMVector3Length(XMVectorSubtract(a, b))) < tol;
}

// 画面の水平線がどれだけ傾いているか (sin(roll)、符号付き)。0 = 水平。
// 「操縦したら水平に戻ってしまった」を検出するための物差し。
// ★これはピッチでは保存されない量である点に注意 — ピッチは**カメラ自身の右軸**まわりの
//   回転 (実物のジンバルと同じ) なので、傾いたカメラをピッチすると水平線の傾きも動く。
//   保存されるのは「右軸が動かないこと」の方で、下のテストはそちらを不変量として使う
float RollSin(FXMVECTOR rot)
{
    const XMVECTOR fwd = Axis(rot, 0.0f, 0.0f, 1.0f);
    const XMVECTOR up = Axis(rot, 0.0f, 1.0f, 0.0f);
    // 視線に直交する「水平な右方向」。自分の上方向がこれに寄っているほど傾いている
    const XMVECTOR flatRight = XMVector3Normalize(
        XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), fwd));
    return XMVectorGetX(XMVector3Dot(up, flatRight));
}

} // namespace

bool RunCameraPilotSelfTest()
{
    MYE_LOG_INFO("==== Camera pilot self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // 傾いた (ロール 20°) カメラ。これが操縦で水平に戻らないことがこの機能の要件
    const XMVECTOR tilted = XMQuaternionRotationRollPitchYaw(
        XMConvertToRadians(10.0f), XMConvertToRadians(45.0f), XMConvertToRadians(20.0f));
    const float tiltedRoll = RollSin(tilted);
    check(std::fabs(std::fabs(tiltedRoll) - std::sin(XMConvertToRadians(20.0f))) < 1e-3f,
          "the test fixture really has a 20 deg roll (the measurement itself is sane)");

    // ---- 入力ゼロは恒等 ----
    {
        const XMVECTOR same = PilotApplyLook(tilted, 0.0f, 0.0f);
        check(XMVector4Equal(same, tilted),
              "zero mouse delta returns the pose bit-identical (idle frames never drift)");
    }

    // ---- ヨーはワールド上方向まわり: ロールもピッチも変わらない ----
    {
        const XMVECTOR turned = PilotApplyLook(tilted, 40.0f, 0.0f);
        check(std::fabs(RollSin(turned) - tiltedRoll) < 1e-4f,
              "yaw keeps the horizon tilt exactly (the whole point of not decomposing)");
        // 姿勢全体が「ワールド Y まわりの回転」を後掛けしたものと一致する =
        // ヨーがローカル軸ではなくワールド軸まわりである証拠 (混同すると傾いた
        // カメラを振ったときに地平が回る)
        const XMVECTOR worldYaw = XMQuaternionRotationNormal(
            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
            XMConvertToRadians(40.0f * kPilotLookDegPerPixel));
        const XMVECTOR expected = XMQuaternionMultiply(tilted, worldYaw);
        check(VecNear(Axis(turned, 0, 0, 1), Axis(expected, 0, 0, 1))
                  && VecNear(Axis(turned, 0, 1, 0), Axis(expected, 0, 1, 0)),
              "yaw rotates about the world up axis, not the camera's own up");
        // 前方の高さ (= ピッチ成分) はヨーでは動かない
        check(std::fabs(XMVectorGetY(Axis(turned, 0, 0, 1))
                        - XMVectorGetY(Axis(tilted, 0, 0, 1))) < 1e-4f,
              "yaw does not change the pitch");
    }

    // ---- ピッチはカメラ自身の右軸まわり: 右軸が動かない ----
    {
        const XMVECTOR pitched = PilotApplyLook(tilted, 0.0f, 30.0f);
        check(VecNear(Axis(pitched, 1, 0, 0), Axis(tilted, 1, 0, 0)),
              "pitch rotates about the camera's own right axis (that axis is invariant)");
        check(XMVectorGetY(Axis(pitched, 0, 0, 1)) < XMVectorGetY(Axis(tilted, 0, 0, 1)),
              "dragging the mouse down looks down (same feel as the editor camera)");
        // ★ピッチでは水平線の傾き**そのもの**は保存されない (右軸まわりに回すので当然) —
        //   保証するのは「水平へ吸い寄せられない」こと。素朴な分解実装は 1 回で 0 になる
        check(std::fabs(RollSin(pitched)) > 0.2f,
              "pitch never snaps the pose level (the naive rebuild would zero it at once)");
    }

    // ---- ★歯の確認: 素朴な「yaw/pitch へ分解して組み直す」実装だとロールが消える ----
    {
        const XMVECTOR fwd = Axis(tilted, 0.0f, 0.0f, 1.0f);
        const float yaw = std::atan2(XMVectorGetX(fwd), XMVectorGetZ(fwd));
        const float pitch = -std::asin(XMVectorGetY(fwd));
        const XMVECTOR naive = XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.0f);
        check(std::fabs(RollSin(naive)) < 1e-4f && std::fabs(tiltedRoll) > 0.3f,
              "the naive decompose-and-rebuild really would flatten the roll (test has teeth)");
    }

    // ---- 頭打ち: 真上/真下を越えて裏返らない ----
    {
        XMVECTOR q = XMQuaternionIdentity();
        for (int i = 0; i < 200; ++i) {
            q = PilotApplyLook(q, 0.0f, -40.0f); // ひたすら上を向く
        }
        const XMVECTOR fwd = Axis(q, 0.0f, 0.0f, 1.0f);
        check(XMVectorGetY(fwd) > 0.9f && XMVectorGetY(fwd) <= 1.0f,
              "looking up saturates just short of straight up");
        check(XMVectorGetZ(fwd) > 0.0f,
              "the view never flips over the top (z stays on the original side)");
        // 頭打ちに張り付いたあとも、ヨーは効き続ける (操作不能にならない)
        const XMVECTOR turned = PilotApplyLook(q, 40.0f, -40.0f);
        check(!XMVector4Equal(turned, q), "yaw still works while the pitch is clamped");

        XMVECTOR d = XMQuaternionIdentity();
        for (int i = 0; i < 200; ++i) {
            d = PilotApplyLook(d, 0.0f, 40.0f); // ひたすら下を向く
        }
        check(XMVectorGetY(Axis(d, 0, 0, 1)) < -0.9f && XMVectorGetZ(Axis(d, 0, 0, 1)) > 0.0f,
              "looking down saturates the same way");
    }

    // ---- 長時間の操縦でも四元数が正規のまま (差分回転の積み重ねで崩れない) ----
    {
        XMVECTOR q = tilted;
        for (int i = 0; i < 2000; ++i) {
            q = PilotApplyLook(q, (i % 7) - 3.0f, (i % 5) - 2.0f);
        }
        const float len = XMVectorGetX(XMVector4Length(q));
        check(std::fabs(len - 1.0f) < 1e-4f,
              "2000 look steps keep the quaternion normalized (no accumulated drift)");
        check(std::fabs(RollSin(q)) > 0.2f,
              "2000 look steps still never flatten the pose");
    }

    MYE_LOG_INFO("==== Camera pilot self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
