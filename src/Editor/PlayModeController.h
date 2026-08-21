#pragma once
#include "nlohmann/json.hpp"

#include "Engine/Engine/GameFlow.h"

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
    // M52e: 状態を明示して指定する版 (タイムラインのスクラブが使う)。
    // TogglePause だと「今どちらか」を呼び側が知っている必要があり、
    // スクラブのたびに再生/停止が反転する事故になる
    void Pause();
    void Resume();
    void Step(); // Paused 中に 1 tick だけ進める
    // M52e: ステップ要求が立っているか。スクラブ中は tick が止まっているので、
    // 「ステップを押した = 分岐して 1 tick 進めたい」を外から観測する必要がある
    bool StepPending() const { return stepPending_; }

    // 毎 tick 呼ぶ。true ならこの tick でゲームロジックを進める (Step は 1 回で消費)
    bool ConsumeSimulateTick();

private:
    PlayState state_ = PlayState::Editing;
    nlohmann::json snapshot_;
    // M51g: TimeControl / PersistStore はシーン文書 (SaveToJson) の外にある sim 状態なので
    // 別途スナップショットする。忘れると Play 中の永続値/ポーズが Stop 後の編集状態へ漏れる
    TimeControl timeSnapshot_;
    PersistStore persistSnapshot_;
    bool stepPending_ = false;
};

} // namespace mye
