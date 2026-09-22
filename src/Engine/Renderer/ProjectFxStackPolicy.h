/*----
 ProjectFxStackPolicy.h  fxstack／ユーザーポストの注入可否 (M78 §4.1)
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once

namespace mye {

// CameraOverride (Scene View 等) 中はユーザーポスト／スタック駆動 CS を Resolve へ渡さない。
// Play / GameView (cameraOverrideActive==false) のみ注入する。
inline bool ShouldInjectProjectFxStack(bool cameraOverrideActive)
{
    return !cameraOverrideActive;
}

} // namespace mye
