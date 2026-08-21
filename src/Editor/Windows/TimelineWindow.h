#pragma once
#include "Engine/Engine/EngineLoop.h"

namespace mye {

class PlayModeController;

// タイムライン (M52e): Play 中のワールドを過去 tick へシークして観察する。
//
// ★この窓は**要求を出すだけ** — Restore と再シムは EngineLoop が tick 境界で行う。
//   ImGui の途中で世界を差し替えると、その後のウィンドウが破棄済み EntityID を掴む。
// ★スクラブすると自動でポーズする (見たい瞬間で世界が止まっていないと観察できない)。
//   再生を再開するとそこから**分岐**し、記録済みの未来は捨てられる (Unity に無い挙動)。
class TimelineWindow {
public:
    bool open = false; // 既定は非表示 (Play しない限り中身が無いため)
    void OnImGui(EngineContext& ctx, PlayModeController& playMode);

private:
    // スライダーを掴んでいる間の表示位置。ドラッグ中は要求済みの目標を出しておかないと、
    // シークが 1 フレーム遅れて効くせいでつまみが手元へ戻ってしまう
    long long pendingPos_ = -1;
};

} // namespace mye
