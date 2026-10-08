//====================================================================================
//                          ClipEventEdit.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                  アニメイベントの編集 (純関数、M89o)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

#include "Engine/Engine/Animation/AnimatorController.h"

namespace mye {

// クリップのイベント一覧。同名が複数あればエンジンと同じく先頭を返す。無ければ nullptr
ControllerClipEvents* FindClipEventsMutable(ControllerAsset& ctrl, uint64_t clipHash);

// clip のイベントを tick に 1 件足し、その index を返す (一覧が無ければ作る)。種類は Script、名前は "Event"
int32_t AddClipEvent(ControllerAsset& ctrl, const std::string& clip, int32_t tick);

// 1 件消す。一覧が空になったら一覧ごと消す (保存に空の配列を残さない)。範囲外は何もしない
void RemoveClipEvent(ControllerAsset& ctrl, uint64_t clipHash, int32_t index);

// 名前・アセットを書き、ハッシュを揃える (ハッシュだけ古いと BT や音の参照が食い違う)
void SetClipEventName(ControllerClipEvent& ev, const std::string& name);
void SetClipEventAsset(ControllerClipEvent& ev, const std::string& asset);

// 種類を変える。asset は Sound では音のキー、Effect ではプレハブのキーと意味が変わるので空にする
void SetClipEventKind(ControllerClipEvent& ev, ClipEventKind kind);

// タイムライン (ステートの長さ) 上の tick を、クリップの長さの割合で写したクリップの tick (四捨五入、0..clipTicks)。
// ブレンドツリーの子は位相を共有するので、割合で揃えれば全員の位置と一致する
int32_t TimelineTickToClipTick(int32_t timelineTick, int32_t timelineTicks, int32_t clipTicks);

} // namespace mye
