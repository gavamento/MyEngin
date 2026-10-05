//====================================================================================
//                          BtManagedTaskLane.h
//  MyEngin/ 秋田蓮音                                                     10/06/2026
//                                          BT の C# タスクを動かす別レーンの窓口 (M85)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

// RunTask の phase。Enter = ノードに入った tick (新しいインスタンスで OnStart)、Tick = 以降の毎 tick (OnTick)、
// Abort = 実行中に Abort された (OnAbort。インスタンスを捨てる)
namespace btmanagedphase {
enum : int32_t {
    kEnter = 0,
    kTick = 1,
    kAbort = 2,
};
} // namespace btmanagedphase

// RunTask の戻り値。0 / 1 / 2 は MyeBtStatus (Running / Success / Failure)、これは「その名前のクラスが無い」
constexpr int32_t kBtManagedUnknownClass = -1;

// C# タスクの実体 (managed 側のインスタンス) を持つ別レーン。実装は ManagedHost。
// BehaviorTreeSystem は null の間 (記録・検証・Net・再シムで C# レーンが止まっている) CsTask を Failure にする。
// インスタンスは World の外にあり、巻き戻しでは戻らない (決定論の保証外)
class BtManagedTaskLane {
public:
    virtual ~BtManagedTaskLane() = default;

    // owner の木のノード nodeIndex (展開後の実行木の添字) の C# タスク className を 1 手進める。
    // 戻り値は MyeBtStatus か kBtManagedUnknownClass。Abort の戻り値は使わない
    virtual int32_t RunTask(EntityID owner, int32_t nodeIndex, const std::string& className, int32_t phase, uint64_t tick) = 0;
};

} // namespace mye
