#include "Editor/LightSelectionSelfTest.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/LightSelection.h"

using namespace DirectX;

namespace mye {
namespace {

// テスト用のカメラ: 原点から +Z を向く。near 0.1 / far 100 / fovY 60° / アスペクト 1。
// 「背後 (-Z)」「遠方 (z > 100)」がそれぞれ near/far 平面の外になるので境界を作りやすい
Frustum MakeTestFrustum()
{
    const XMMATRIX v = XMMatrixIdentity();
    const XMMATRIX p = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 1.0f, 0.1f, 100.0f);
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, v * p);
    return BuildFrustum(vp);
}

LightCandidate MakeLight(int32_t type, uint32_t sortKey, float x, float y, float z, float range,
                         int32_t castShadow = 0)
{
    LightCandidate c;
    c.sortKey = sortKey;
    c.castShadow = castShadow;
    c.light.type = type;
    c.light.position = { x, y, z };
    c.light.range = range;
    c.light.direction = { 0, 0, 1 };
    return c;
}

} // namespace

bool RunLightSelectionSelfTest()
{
    MYE_LOG_INFO("==== LightSelection (M54b) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    const Frustum frustum = MakeTestFrustum();

    // ---- 球 × 視錐台の素の判定 ----
    {
        check(SphereInFrustum(frustum, XMFLOAT3{ 0, 0, 50 }, 1.0f),
              "a sphere in front of the camera is inside the frustum");
        check(!SphereInFrustum(frustum, XMFLOAT3{ 0, 0, -50 }, 1.0f),
              "a small sphere behind the camera is outside");
        check(SphereInFrustum(frustum, XMFLOAT3{ 0, 0, -50 }, 60.0f),
              "a sphere behind the camera whose radius reaches the view is inside");
        check(!SphereInFrustum(frustum, XMFLOAT3{ 0, 0, 200 }, 1.0f),
              "a sphere past the far plane is outside");
        check(!SphereInFrustum(frustum, XMFLOAT3{ 400, 0, 50 }, 1.0f),
              "a sphere far off to the side is outside");
        // ★平面は正規化されていない (BuildFrustum) ので、半径を長さで割り忘れると
        //   この 2 つが両方通ってしまう。単位が合っていることの確認
        check(!SphereInFrustum(frustum, XMFLOAT3{ 0, 0, -50 }, 49.0f),
              "the radius is compared in world units (49 does not reach the near plane)");
        check(SphereInFrustum(frustum, XMFLOAT3{ 0, 0, -50 }, 51.0f),
              "the radius is compared in world units (51 does reach the near plane)");
    }

    // ---- ライト 0 本 → 既定の平行光を 1 本補う (従来挙動) ----
    {
        const LightSelection sel = SelectLights(nullptr, 0, &frustum);
        check(sel.count == 1 && sel.lights[0].light.type == 0,
              "an empty scene falls back to one default directional light");
        const XMFLOAT3 d = sel.lights[0].light.direction;
        XMFLOAT3 expect;
        XMStoreFloat3(&expect, XMVector3Normalize(XMVectorSet(0.3f, -0.8f, 0.5f, 0)));
        check(std::fabs(d.x - expect.x) < 1e-6f && std::fabs(d.y - expect.y) < 1e-6f
                  && std::fabs(d.z - expect.z) < 1e-6f,
              "the fallback light keeps the historical direction");
        check(sel.shadowCount == 0 && sel.lights[0].shadowSlot == -1,
              "the fallback light casts no atlas shadow");
    }

    // ---- カリング: 平行光は対象外 / 点光源は影響球で判定 ----
    {
        const LightCandidate cands[] = {
            MakeLight(0, 0, 0, 0, -9999, 1.0f), // 平行光 (位置は無意味)
            MakeLight(1, 1, 0, 0, 50, 2.0f),    // 視界内の点光源
            MakeLight(1, 2, 0, 0, -50, 1.0f),   // 背後の点光源 → 落ちる
            MakeLight(2, 3, 0, 0, 300, 5.0f),   // far の外のスポット → 落ちる
        };
        const LightSelection sel = SelectLights(cands, 4, &frustum);
        check(sel.count == 2 && sel.culled == 2, "out-of-frustum local lights are culled");
        check(sel.lights[0].light.type == 0 && sel.lights[0].sortKey == 0,
              "a directional light is never culled by position");
        check(sel.lights[1].sortKey == 1, "the surviving local light is the one in view");

        // frustum == nullptr (カメラ不在) では 1 本も落とさない
        const LightSelection noCull = SelectLights(cands, 4, nullptr);
        check(noCull.count == 4 && noCull.culled == 0,
              "no frustum means no culling (camera-less views keep every light)");
    }

    // ---- ★全部カリングされても既定の平行光を湧かせない ----
    {
        const LightCandidate cands[] = { MakeLight(1, 7, 0, 0, -50, 1.0f) };
        const LightSelection sel = SelectLights(cands, 1, &frustum);
        check(sel.count == 0 && sel.culled == 1,
              "a scene whose only light is culled stays dark (no phantom sun)");
    }

    // ---- ソート: type → sortKey 昇順。入力順に依らない ----
    {
        std::vector<LightCandidate> cands = {
            MakeLight(2, 30, 0, 0, 50, 5.0f), MakeLight(1, 10, 0, 0, 50, 5.0f),
            MakeLight(0, 99, 0, 0, 50, 5.0f), MakeLight(1, 5, 0, 0, 50, 5.0f),
            MakeLight(2, 20, 0, 0, 50, 5.0f),
        };
        const LightSelection a = SelectLights(cands.data(), 5, &frustum);
        check(a.count == 5, "every in-view light survives");
        const uint32_t expect[5] = { 99, 5, 10, 20, 30 };
        bool ordered = true;
        for (int i = 0; i < 5; ++i) {
            ordered = ordered && a.lights[i].sortKey == expect[i];
        }
        check(ordered, "lights are ordered by (type, entity index)");

        // 入力順を逆にしても結果は同じ = アーキタイプの並び順に依存しない
        std::vector<LightCandidate> reversed(cands.rbegin(), cands.rend());
        const LightSelection b = SelectLights(reversed.data(), 5, &frustum);
        bool same = a.count == b.count;
        for (int i = 0; i < a.count && same; ++i) {
            same = a.lights[i].sortKey == b.lights[i].sortKey;
        }
        check(same, "reversing the input order does not change the result");
    }

    // ---- 上限: 溢れても平行光が必ず残る (type が第 1 キーである理由) ----
    {
        std::vector<LightCandidate> cands;
        for (uint32_t i = 0; i < 5; ++i) {
            cands.push_back(MakeLight(1, i, 0, 0, 50, 5.0f)); // 点光源が先に並ぶ
        }
        cands.push_back(MakeLight(0, 900, 0, 0, 50, 5.0f)); // 平行光は登録が最後
        const LightSelection sel = SelectLights(cands.data(), 6, &frustum, 3);
        check(sel.count == 3 && sel.overflow == 3, "the light count is clamped to maxLights");
        check(sel.lights[0].light.type == 0,
              "the directional light survives the clamp even when registered last");
        check(sel.lights[1].sortKey == 0 && sel.lights[2].sortKey == 1,
              "the kept local lights are the lowest entity indices");
    }

    // ---- 影スロット: 局所ライトへ前詰め。平行光は CSM 担当なので割り当てない ----
    {
        const LightCandidate cands[] = {
            MakeLight(0, 0, 0, 0, 50, 5.0f, 1), // 平行光 (castShadow=1 でも枠は取らない)
            MakeLight(1, 1, 0, 0, 50, 5.0f, 1),
            MakeLight(1, 2, 0, 0, 50, 5.0f, 0), // 影を落とさない
            MakeLight(2, 3, 0, 0, 50, 5.0f, 1),
            MakeLight(2, 4, 0, 0, 50, 5.0f, 1),
        };
        const LightSelection sel = SelectLights(cands, 5, &frustum);
        check(sel.lights[0].shadowSlot == -1,
              "a directional light never takes an atlas slot (CSM owns it)");
        check(sel.lights[1].shadowSlot == 0 && sel.lights[2].shadowSlot == -1
                  && sel.lights[3].shadowSlot == 1 && sel.lights[4].shadowSlot == 2,
              "shadow slots are packed in the deterministic order");
        check(sel.shadowCount == 3, "the shadow caster count matches the assigned slots");

        // 上限を超えた分は影を落とさない (ライト自体は残る)
        const LightSelection capped = SelectLights(cands, 5, &frustum, kMaxLights, 2);
        check(capped.count == 5 && capped.shadowCount == 2,
              "maxShadowLights clamps casters without dropping the lights");
        check(capped.lights[3].shadowSlot == 1 && capped.lights[4].shadowSlot == -1,
              "the clamp drops the last casters in deterministic order");
    }

    MYE_LOG_INFO("==== LightSelection self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
