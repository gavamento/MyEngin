/*----
 ProjectFxStackPolicy.h  fxstack／ユーザーポストの注入可否 (M78 §4.1)
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once
#include <cstdint>

#include "Engine/Core/EntityID.h"

namespace mye {

// CameraOverride (Scene View 等) 中はユーザーポスト／スタック駆動 CS を Resolve へ渡さない。
// Play / GameView (cameraOverrideActive==false) のみ注入する。
inline bool ShouldInjectProjectFxStack(bool cameraOverrideActive)
{
    return !cameraOverrideActive;
}

// そのビューで実際に使う fxStack。null はそのビューのユーザーポスト／CS を消す意味。
// 「注入しないビュー / カメラ無し・CameraPostFx 無し / fxStack 未設定 / GUID がパスに解決できない」は
// どれも null に揃える。以前は fxStack を持つカメラから持たないカメラへ切り替えても前のパスが残っていた
inline AssetID EffectiveFxStack(bool cameraOverrideActive, bool hasCameraPostFx, AssetID fxStack,
                                bool pathResolvable)
{
    if (!ShouldInjectProjectFxStack(cameraOverrideActive) || !hasCameraPostFx || !pathResolvable) {
        return {};
    }
    return fxStack;
}

// fxstack.json を読み直すか。loaded* は直前に読んだ ID とファイル更新時刻、current* は今の値
// (更新時刻が取れないときは 0)。ID が変わったかファイルが書き換わったときだけ読み、
// 毎フレームの同期読み込みと、読めないファイルへの毎フレームの WARN を避ける
inline bool NeedsFxStackReload(AssetID loadedId, int64_t loadedStamp, AssetID currentId,
                               int64_t currentStamp)
{
    if (currentId.IsNull()) {
        return false;
    }
    return currentId != loadedId || currentStamp != loadedStamp;
}

} // namespace mye
