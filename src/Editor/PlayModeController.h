#pragma once
#include "nlohmann/json.hpp"

namespace mye {

class Scene;

enum class PlayState { Editing, Playing, Paused };

// Play / Pause / Step 制御 (engine_spec.md 9 章)。
// Play 開始時にシーンを JSON スナップショットし、Stop で復元する
// (Play 中の編集は Unity と同様に破棄される — ユーザー決定事項)
class PlayModeController {
public:
    PlayState State() const { return state_; }
    bool InPlayMode() const { return state_ != PlayState::Editing; }

    void Play(Scene& scene);
    void Stop(Scene& scene);
    void TogglePause();
    void Step(); // Paused 中に 1 tick だけ進める

    // 毎 tick 呼ぶ。true ならこの tick でゲームロジックを進める (Step は 1 回で消費)
    bool ConsumeSimulateTick();

private:
    PlayState state_ = PlayState::Editing;
    nlohmann::json snapshot_;
    bool stepPending_ = false;
};

} // namespace mye
