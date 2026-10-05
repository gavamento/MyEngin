//====================================================================================
//                          PatrolRouteEdit.cpp
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          巡回ルートの点の編集の実装
//====================================================================================
#include "Editor/Tools/PatrolRouteEdit.h"

#include <cmath>

namespace mye {

namespace {

constexpr float kPatrolAddStep = 1.0f;          // 点を足すときの、直前の点からのずらし量 (m)
constexpr float kMinDeterminant = 1.0e-12f;     // これ未満の行列式は特異とみなす

} // namespace

bool PatrolRouteAddPoint(PatrolRouteComponent& route)
{
    if (route.pointCount < 0 || route.pointCount >= kMaxPatrolPoints) {
        return false;
    }
    const int index = route.pointCount;
    if (index == 0) {
        route.points[0] = { 0.0f, 0.0f, 0.0f };
        route.waitTicks[0] = 0;
    } else {
        route.points[index] = { route.points[index - 1].x, route.points[index - 1].y, route.points[index - 1].z + kPatrolAddStep };
        route.waitTicks[index] = route.waitTicks[index - 1];
    }
    route.pointCount = index + 1;
    return true;
}

bool PatrolRouteRemovePoint(PatrolRouteComponent& route, int index)
{
    if (index < 0 || index >= route.pointCount || route.pointCount > kMaxPatrolPoints) {
        return false;
    }
    for (int i = index; i + 1 < route.pointCount; ++i) {
        route.points[i] = route.points[i + 1];
        route.waitTicks[i] = route.waitTicks[i + 1];
    }
    --route.pointCount;
    route.points[route.pointCount] = { 0.0f, 0.0f, 0.0f }; // 使わない欄は 0 に戻す (保存・ハッシュに古い値を残さない)
    route.waitTicks[route.pointCount] = 0;
    return true;
}

bool PatrolRouteMovePoint(PatrolRouteComponent& route, int index, int delta)
{
    const int other = index + delta;
    if ((delta != -1 && delta != 1) || index < 0 || index >= route.pointCount || other < 0 || other >= route.pointCount
        || route.pointCount > kMaxPatrolPoints) {
        return false;
    }
    const DirectX::XMFLOAT3 point = route.points[index];
    const int32_t wait = route.waitTicks[index];
    route.points[index] = route.points[other];
    route.waitTicks[index] = route.waitTicks[other];
    route.points[other] = point;
    route.waitTicks[other] = wait;
    return true;
}

DirectX::XMFLOAT3 PatrolPointLocalToWorld(const DirectX::XMFLOAT4X4& routeWorld, const DirectX::XMFLOAT3& local)
{
    DirectX::XMFLOAT3 out;
    DirectX::XMStoreFloat3(&out, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&local), DirectX::XMLoadFloat4x4(&routeWorld)));
    return out;
}

bool PatrolPointWorldToLocal(const DirectX::XMFLOAT4X4& routeWorld, const DirectX::XMFLOAT3& world, DirectX::XMFLOAT3& out)
{
    DirectX::XMVECTOR determinant{};
    const DirectX::XMMATRIX inverse = DirectX::XMMatrixInverse(&determinant, DirectX::XMLoadFloat4x4(&routeWorld));
    if (std::fabs(DirectX::XMVectorGetX(determinant)) <= kMinDeterminant) {
        return false;
    }
    DirectX::XMStoreFloat3(&out, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&world), inverse));
    return true;
}

} // namespace mye
