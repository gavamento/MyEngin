#include "Editor/CameraPilot.h"

#include <cmath>

using namespace DirectX;

namespace mye {

CameraPilotState& GetCameraPilot()
{
    static CameraPilotState state;
    return state;
}

XMVECTOR PilotApplyLook(FXMVECTOR rot, float dxPixels, float dyPixels)
{
    XMVECTOR q = rot;
    // DirectXMath の XMQuaternionMultiply(A, B) は「A の次に B」。したがって
    // ワールド軸まわりの回転は後掛け、ローカル軸まわりの回転は前掛けになる
    if (dxPixels != 0.0f) {
        const XMVECTOR yawQ = XMQuaternionRotationNormal(
            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
            XMConvertToRadians(dxPixels * kPilotLookDegPerPixel));
        q = XMQuaternionNormalize(XMQuaternionMultiply(q, yawQ));
    }
    if (dyPixels != 0.0f) {
        const XMVECTOR pitchQ = XMQuaternionRotationNormal(
            XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
            XMConvertToRadians(dyPixels * kPilotLookDegPerPixel));
        const XMVECTOR cand = XMQuaternionNormalize(XMQuaternionMultiply(pitchQ, q));
        // 頭打ちは**前方ベクトルの y** で見る。角度へ分解して clamp すると、そこで
        // ロールが落ちる (この関数の存在理由そのものが消える)
        const float fy = XMVectorGetY(XMVector3Rotate(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), cand));
        if (std::fabs(fy) < kPilotPitchLimit) {
            q = cand;
        }
    }
    return q;
}

} // namespace mye
