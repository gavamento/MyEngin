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
    timeSnapshot_ = scene.Time();       // M51g: シーン文書外の sim 状態も一緒に撮る
    persistSnapshot_ = scene.Persist();
    // Play 開始でシーンをリロードして EntityID を正規化する (Unity の Domain Reload 相当)。
    // これが無いと、エディタで生成/削除/Undo を繰り返した後の EntityID 割当が
    // フレッシュロード時と食い違い、エディタ内で録った .rep が replay_verify
    // (フレッシュロード) と一致しなくなる (M8 の決定論規約)。
    SceneSerializer::LoadFromJson(scene, snapshot_);
    state_ = PlayState::Playing;
    MYE_LOG_INFO("[play] started (snapshot: %zu entities, reloaded)", snapshot_["entities"].size());
}

void PlayModeController::Stop(Scene& scene)
{
    if (state_ == PlayState::Editing) {
        return;
    }
    SceneSerializer::LoadFromJson(scene, snapshot_);
    scene.Time() = timeSnapshot_;       // M51g: Play 中のポーズ/永続値を編集状態へ漏らさない
    scene.Persist() = persistSnapshot_;
    timeSnapshot_ = TimeControl{};
    persistSnapshot_.Clear();
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
