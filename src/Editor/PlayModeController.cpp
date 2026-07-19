#include "Editor/PlayModeController.h"

#include "Engine/Core/Log.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"

namespace mye {

void PlayModeController::Play(Scene& scene)
{
    if (state_ != PlayState::Editing) {
        return;
    }
    snapshot_ = SceneSerializer::SaveToJson(scene);
    state_ = PlayState::Playing;
    MYE_LOG_INFO("[play] started (snapshot: %zu entities)", snapshot_["entities"].size());
}

void PlayModeController::Stop(Scene& scene)
{
    if (state_ == PlayState::Editing) {
        return;
    }
    SceneSerializer::LoadFromJson(scene, snapshot_);
    snapshot_.clear();
    state_ = PlayState::Editing;
    stepPending_ = false;
    MYE_LOG_INFO("[play] stopped (scene restored)");
}

void PlayModeController::TogglePause()
{
    if (state_ == PlayState::Playing) {
        state_ = PlayState::Paused;
    } else if (state_ == PlayState::Paused) {
        state_ = PlayState::Playing;
    }
}

void PlayModeController::Step()
{
    if (state_ == PlayState::Paused) {
        stepPending_ = true;
    }
}

bool PlayModeController::ConsumeSimulateTick()
{
    if (state_ == PlayState::Playing) {
        return true;
    }
    if (state_ == PlayState::Paused && stepPending_) {
        stepPending_ = false;
        return true;
    }
    return false;
}

} // namespace mye
