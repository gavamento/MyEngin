//====================================================================================
//                          PatrolRouteEditSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          巡回ルートの点の編集の回帰テスト実装
//====================================================================================
#include "Editor/Tools/PatrolRouteEditSelfTest.h"

#include <cmath>
#include <cstdint>

#include <DirectXMath.h>

#include "Editor/Scene/Selection.h"
#include "Editor/Tools/PatrolRouteEdit.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"

using namespace DirectX;

namespace mye {
namespace {

bool Near(const XMFLOAT3& a, const XMFLOAT3& b, float eps = 1.0e-4f)
{
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

} // namespace

bool RunPatrolRouteEditSelfTest()
{
    MYE_LOG_INFO("==== PatrolRouteEdit (M85g) self test ====");
    RegisterBuiltinComponents();
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 1. 点の追加・削除・入れ替え ----
    {
        PatrolRouteComponent route;
        check(PatrolRouteAddPoint(route) && route.pointCount == 1 && Near(route.points[0], { 0, 0, 0 }),
              "追加: 最初の点は原点");
        route.points[0] = { 2.0f, 1.0f, 3.0f };
        route.waitTicks[0] = 30;
        check(PatrolRouteAddPoint(route) && route.pointCount == 2 && Near(route.points[1], { 2.0f, 1.0f, 4.0f }) && route.waitTicks[1] == 30,
              "追加: 次の点は直前の点から +Z へずれ、待ちを引き継ぐ");
        route.points[1] = { 5.0f, 0.0f, 5.0f };
        route.waitTicks[1] = 7;
        PatrolRouteAddPoint(route);
        route.points[2] = { 9.0f, 0.0f, 9.0f };
        route.waitTicks[2] = 11;
        check(PatrolRouteMovePoint(route, 2, -1) && Near(route.points[1], { 9.0f, 0.0f, 9.0f }) && route.waitTicks[1] == 11
                  && Near(route.points[2], { 5.0f, 0.0f, 5.0f }) && route.waitTicks[2] == 7,
              "入れ替え: 点と待ちが一緒に隣と入れ替わる");
        check(!PatrolRouteMovePoint(route, 0, -1) && !PatrolRouteMovePoint(route, 2, 1) && !PatrolRouteMovePoint(route, 1, 2)
                  && !PatrolRouteMovePoint(route, 3, -1),
              "入れ替え: 端・範囲外・delta が ±1 でないときは何もしない");
        check(PatrolRouteRemovePoint(route, 1) && route.pointCount == 2 && Near(route.points[1], { 5.0f, 0.0f, 5.0f }) && route.waitTicks[1] == 7
                  && Near(route.points[2], { 0, 0, 0 }) && route.waitTicks[2] == 0,
              "削除: 後ろが詰まり、空いた末尾の欄は 0 に戻る");
        check(!PatrolRouteRemovePoint(route, 2) && !PatrolRouteRemovePoint(route, -1), "削除: 範囲外は何もしない");
        PatrolRouteComponent full;
        while (PatrolRouteAddPoint(full)) {
        }
        check(full.pointCount == kMaxPatrolPoints && !PatrolRouteAddPoint(full), "追加: 32 個で満杯になり、それ以上は足せない");
        PatrolRouteComponent empty;
        check(!PatrolRouteRemovePoint(empty, 0) && !PatrolRouteMovePoint(empty, 0, 1), "空のルートへの削除・入れ替えは何もしない");
    }

    // ---- 2. ワールド <-> ローカル ----
    {
        XMFLOAT4X4 world;
        XMStoreFloat4x4(&world, XMMatrixScaling(2.0f, 1.0f, 0.5f) * XMMatrixRotationY(0.7f) * XMMatrixTranslation(10.0f, 2.0f, -3.0f));
        const XMFLOAT3 local = { 1.5f, -0.5f, 4.0f };
        const XMFLOAT3 at = PatrolPointLocalToWorld(world, local);
        XMFLOAT3 back = {};
        check(PatrolPointWorldToLocal(world, at, back) && Near(back, local), "ワールド -> ローカルは ローカル -> ワールド の逆");
        XMFLOAT4X4 singular;
        XMStoreFloat4x4(&singular, XMMatrixScaling(0.0f, 1.0f, 1.0f));
        XMFLOAT3 untouched = { 7.0f, 7.0f, 7.0f };
        check(!PatrolPointWorldToLocal(singular, at, untouched) && Near(untouched, { 7.0f, 7.0f, 7.0f }),
              "特異なワールド行列 (スケール 0) は false で出力に触らない");
    }

    // ---- 3. ギズモのドラッグ = 1 ドラッグ 1 Undo / Inspector のボタン = 1 操作 1 Undo ----
    {
        Scene scene;
        World& world = scene.GetWorld();
        UndoStack undo;
        Selection selection;
        GameObject go = scene.CreateGameObjectTracked("Route");
        go.SetLocalPosition(10.0f, 0.0f, 0.0f);
        auto* route = go.AddComponent<PatrolRouteComponent>();
        PatrolRouteAddPoint(*route);
        PatrolRouteAddPoint(*route);
        route->points[1] = { 1.0f, 0.0f, 2.0f };
        world.ApplyStructuralChanges();
        const EntityID entity = go.Id();
        const uint64_t fid = scene.EnsureFileId(entity);
        selection.SelectOnly(fid);
        const XMFLOAT3 original = world.GetComponent<PatrolRouteComponent>(entity)->points[1];

        // ドラッグ: 開始で before、複数フレームの書き込み、終了で after (SceneViewWindow の巡回点ギズモと同じ順)
        XMFLOAT4X4 routeWorld;
        XMStoreFloat4x4(&routeWorld, XMMatrixTranslation(10.0f, 0.0f, 0.0f));
        undo.BeginRecord("Patrol Point Gizmo", selection);
        undo.CaptureBefore(scene, fid);
        for (int frame = 1; frame <= 5; ++frame) {
            XMFLOAT3 local = {};
            const XMFLOAT3 target = { 10.0f + 0.5f * static_cast<float>(frame), 0.0f, 2.0f };
            if (PatrolPointWorldToLocal(routeWorld, target, local)) {
                world.GetComponent<PatrolRouteComponent>(entity)->points[1] = local;
            }
        }
        undo.CaptureAfter(scene, fid);
        undo.EndRecord(selection);
        check(Near(world.GetComponent<PatrolRouteComponent>(entity)->points[1], { 2.5f, 0.0f, 2.0f }), "ドラッグ: 最終フレームの位置がローカルへ書かれる");
        check(undo.CanUndo(), "ドラッグ: 記録が 1 つ積まれる");
        undo.Undo(scene, selection);
        world.ApplyStructuralChanges();
        const GameObject afterUndo = scene.FindByFileId(fid);
        check(static_cast<bool>(afterUndo) && Near(world.GetComponent<PatrolRouteComponent>(afterUndo.Id())->points[1], original),
              "ドラッグ: 1 回の Undo で元の位置へ戻る (保存と復元に点が入っている)");
        check(!undo.CanUndo(), "ドラッグ: 5 フレームのドラッグが 1 エントリ (2 回目の Undo は無い)");
        undo.Redo(scene, selection);
        world.ApplyStructuralChanges();
        const GameObject afterRedo = scene.FindByFileId(fid);
        check(static_cast<bool>(afterRedo) && Near(world.GetComponent<PatrolRouteComponent>(afterRedo.Id())->points[1], { 2.5f, 0.0f, 2.0f }),
              "ドラッグ: Redo でドラッグ後の位置へ進む");
        undo.ClearAll();

        // Inspector のボタン (UndoStack::Record) で追加
        const EntityID routeEntity = scene.FindByFileId(fid).Id();
        undo.Record("Add Patrol Point", scene, selection, fid, UndoStack::StructuralChanges::None,
                    [&] { PatrolRouteAddPoint(*world.GetComponent<PatrolRouteComponent>(routeEntity)); });
        check(world.GetComponent<PatrolRouteComponent>(routeEntity)->pointCount == 3, "ボタン: 点を追加");
        undo.Undo(scene, selection);
        world.ApplyStructuralChanges();
        check(world.GetComponent<PatrolRouteComponent>(scene.FindByFileId(fid).Id())->pointCount == 2 && !undo.CanUndo(),
              "ボタン: 追加は 1 回の Undo で戻る");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== PatrolRouteEdit self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== PatrolRouteEdit self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
