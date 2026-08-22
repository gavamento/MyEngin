#include "Editor/DecalSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/RenderTypes.h"

using namespace DirectX;

namespace mye {
namespace {

// 「よくある値ではない」変換を 1 個作る。回転も非一様スケールも入れてあるのは、
// 転置忘れ / 行と列の取り違えを恒等に紛れさせないため
XMFLOAT4X4 MakeTestDecalWorld()
{
    const XMMATRIX m = XMMatrixScaling(3.0f, 5.0f, 0.5f)
        * XMMatrixRotationRollPitchYaw(XMConvertToRadians(35.0f), XMConvertToRadians(-70.0f),
                                       XMConvertToRadians(18.0f))
        * XMMatrixTranslation(12.0f, -4.0f, 7.5f);
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, m);
    return out;
}

// ローカル座標 → ワールド座標 (行ベクトル規約)
XMFLOAT3 LocalToWorld(const XMFLOAT4X4& world, float x, float y, float z)
{
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Transform(XMVectorSet(x, y, z, 1.0f), XMLoadFloat4x4(&world)));
    return out;
}

// ワールド座標 → ローカル座標。**シェーダ (decal_project.hlsl) がやっている計算の CPU 鏡**
XMFLOAT3 WorldToLocal(const DecalRenderItem& d, const XMFLOAT3& p)
{
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Transform(XMVectorSet(p.x, p.y, p.z, 1.0f),
                                           XMLoadFloat4x4(&d.invWorld)));
    return out;
}

bool Near(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

DecalRenderItem MakeOrderItem(int32_t sortOrder, uint32_t sortKey)
{
    DecalRenderItem d;
    d.sortOrder = sortOrder;
    d.sortKey = sortKey;
    return d;
}

} // namespace

bool RunDecalSelfTest()
{
    MYE_LOG_INFO("==== Decal (projector box) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    RegisterBuiltinComponents(); // sTypeId 解決 (冪等)

    // ---- FillDecalTransform: 逆行列と投影方向 ----
    {
        const XMFLOAT4X4 world = MakeTestDecalWorld();
        DecalRenderItem d;
        check(FillDecalTransform(world, d), "a non-degenerate world matrix yields a decal item");

        // world * invWorld == 恒等。転置を取り違えると、非対称な回転が入っているので必ず崩れる
        XMFLOAT4X4 round;
        XMStoreFloat4x4(&round,
                        XMLoadFloat4x4(&world) * XMLoadFloat4x4(&d.invWorld));
        bool identity = true;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                identity = identity && Near(round.m[r][c], (r == c) ? 1.0f : 0.0f, 1e-3f);
            }
        }
        check(identity, "invWorld is the inverse of world (world * invWorld == I)");

        // 箱の中心 / 角 / 外側が、シェーダの |local| <= 0.5 判定でそれぞれ正しく落ちるか。
        // ★これが decal_project.hlsl の OBB 判定の CPU 鏡 (式を 2 箇所で書いている唯一の場所)
        const XMFLOAT3 center = LocalToWorld(world, 0.0f, 0.0f, 0.0f);
        const XMFLOAT3 lc = WorldToLocal(d, center);
        check(Near(lc.x, 0.0f) && Near(lc.y, 0.0f) && Near(lc.z, 0.0f),
              "the box center maps back to local (0,0,0)");
        const XMFLOAT3 corner = LocalToWorld(world, 0.5f, -0.5f, 0.5f);
        const XMFLOAT3 lk = WorldToLocal(d, corner);
        check(Near(lk.x, 0.5f) && Near(lk.y, -0.5f) && Near(lk.z, 0.5f),
              "a box corner maps back to local (+0.5,-0.5,+0.5)");
        const XMFLOAT3 outside = LocalToWorld(world, 0.51f, 0.0f, 0.0f);
        const XMFLOAT3 lo = WorldToLocal(d, outside);
        check(std::fabs(lo.x) > 0.5f && std::fabs(lo.y) <= 0.5f && std::fabs(lo.z) <= 0.5f,
              "a point just outside the box fails |local| <= 0.5 on exactly one axis");

        // 投影方向 = ワールド行列の第 3 行 (ローカル +Z) を正規化したもの。
        // LightComponent の向きと同じ規約 — ここがずれると「ライトと同じ感覚で向ける」が崩れる
        XMFLOAT3 expect;
        XMStoreFloat3(&expect, XMVector3Normalize(
                                   XMVectorSet(world._31, world._32, world._33, 0.0f)));
        check(Near(d.projDir.x, expect.x) && Near(d.projDir.y, expect.y)
                  && Near(d.projDir.z, expect.z),
              "projDir is the normalized 3rd row of world (local +Z, same rule as lights)");
        const float len = std::sqrt(d.projDir.x * d.projDir.x + d.projDir.y * d.projDir.y
                                    + d.projDir.z * d.projDir.z);
        check(Near(len, 1.0f), "projDir is normalized (non-uniform scale does not leak in)");
    }

    // ---- 退化した行列は弾く ----
    {
        XMFLOAT4X4 flat;
        XMStoreFloat4x4(&flat, XMMatrixScaling(2.0f, 2.0f, 0.0f)); // 厚み 0 = 逆行列なし
        DecalRenderItem d;
        check(!FillDecalTransform(flat, d), "a zero-thickness box is rejected (no inverse)");
        XMFLOAT4X4 zero = {};
        check(!FillDecalTransform(zero, d), "an all-zero matrix is rejected");
    }

    // ---- 角度フェード ----
    {
        check(Near(DecalAngleFadeCos(0.0f), 1.0f), "angle fade 0deg -> cos 1");
        check(Near(DecalAngleFadeCos(90.0f), 0.0f), "angle fade 90deg -> cos 0 (plain cosine fade)");
        check(Near(DecalAngleFadeCos(180.0f), -1.0f), "angle fade 180deg -> cos -1");
        check(Near(DecalAngleFadeCos(60.0f), 0.5f), "angle fade 60deg -> cos 0.5");
        // ★範囲外を丸めないと、cos が単調でない区間へ出て「角度を狭めたのに広がる」が起きる
        check(Near(DecalAngleFadeCos(-30.0f), 1.0f), "angle fade below 0 clamps to 0deg");
        check(Near(DecalAngleFadeCos(400.0f), -1.0f), "angle fade above 180 clamps to 180deg");
        check(DecalAngleFadeCos(30.0f) > DecalAngleFadeCos(120.0f),
              "a narrower angle gives a larger cosine threshold");
    }

    // ---- 描画順 (規則 7: 収集順に依存しない全順序) ----
    {
        check(DecalDrawOrderLess(MakeOrderItem(-1, 99), MakeOrderItem(0, 0)),
              "sortOrder is the primary key");
        check(DecalDrawOrderLess(MakeOrderItem(3, 4), MakeOrderItem(3, 5)),
              "equal sortOrder falls back to the entity index");
        check(!DecalDrawOrderLess(MakeOrderItem(3, 5), MakeOrderItem(3, 5)),
              "the comparator is irreflexive (strict weak ordering)");

        // 同じ集合を別の順で入れても、並べ替えた結果は必ず同一列になる
        std::vector<DecalRenderItem> a = { MakeOrderItem(2, 7), MakeOrderItem(0, 9),
                                           MakeOrderItem(2, 3), MakeOrderItem(-5, 1) };
        std::vector<DecalRenderItem> b = { MakeOrderItem(2, 3), MakeOrderItem(-5, 1),
                                           MakeOrderItem(0, 9), MakeOrderItem(2, 7) };
        std::sort(a.begin(), a.end(), DecalDrawOrderLess);
        std::sort(b.begin(), b.end(), DecalDrawOrderLess);
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i) {
            same = a[i].sortOrder == b[i].sortOrder && a[i].sortKey == b[i].sortKey;
        }
        check(same, "sorting is independent of the collection order");
        check(a[0].sortKey == 1 && a[1].sortKey == 9 && a[2].sortKey == 3 && a[3].sortKey == 7,
              "the sorted order is (-5,1) (0,9) (2,3) (2,7)");
    }

    // ---- kComponentNoHash: デカールを足してもワールドハッシュが動かない ----
    // ★これが「M56 は .rep 互換の作業が 1 つも要らない」の機械証明。
    //   登録時に kComponentNoHash を書き忘れると、この 1 本だけが落ちる
    {
        Scene scene;
        GameObject host = scene.CreateGameObjectTracked("DecalHost");
        World& w = scene.GetWorld();
        w.ApplyStructuralChanges();
        const uint64_t before = HashWorld(w, nullptr);

        auto* d = w.AddComponent<DecalComponent>(host.Id());
        w.ApplyStructuralChanges();
        check(d != nullptr, "DecalComponent can be added to an entity");
        if (d != nullptr) {
            d->color = { 0.25f, 0.5f, 0.75f, 0.6f };
            d->angleFadeDeg = 42.0f;
            d->sortOrder = 3;
        }
        const uint64_t after = HashWorld(w, nullptr);
        check(before == after, "DecalComponent is kComponentNoHash (the world hash never moves)");
    }

    MYE_LOG_INFO("==== Decal self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
