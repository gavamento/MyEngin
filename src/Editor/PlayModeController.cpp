#include "Editor/PlayModeController.h"

#include "Engine/Core/Log.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Replay/TimeTravel.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Renderer/ComputeAbiRunner.h"

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
        Pause();
    } else if (state_ == PlayState::Paused) {
        Resume();
    }
}

void PlayModeController::Pause()
{
    if (state_ == PlayState::Playing) {
        state_ = PlayState::Paused;
    }
    // M73a: ポーズ = ホールド (tick 番号も止める)。既に Paused でも呼ぶ — スクラブ / 切替 /
    // 差分の要求は Pause() を先に通るので、ここで立てておけば要求側と同じフラグに重なるだけ。
    // ★stepPending_ が残っている間 (RequestStep 後にまだ tick が走っていないフレーム) は
    //   呼ばない — Timeline の Seek ラムダから来た Pause でステップの予算を潰さないため
    if (state_ == PlayState::Paused && !stepPending_ && tt_ != nullptr) {
        tt_->Hold();
    }
}

void PlayModeController::Resume()
{
    if (state_ == PlayState::Paused) {
        state_ = PlayState::Playing;
    }
    // 再生再開は保留中のステップを吸収する。残すと Playing の間は消費されず、次の Pause が
    // 「ステップ待ち」と誤認して Hold を見送る (= 1 tick 余計に走ってから止まる)
    stepPending_ = false;
    if (tt_ != nullptr) {
        tt_->EndScrub(); // 再生再開 = ここから分岐する (M52e / M72a の規約はそのまま)
    }
}

void PlayModeController::Step()
{
    if (state_ != PlayState::Paused) {
        return;
    }
    stepPending_ = true;
    // M73a: ホールド中なら予算 1 で tick ループを 1 本だけ通す。使い切った tick の末で
    // EngineLoop が Hold し直すので、ステップは正確に 1 tick で止まる。
    // リングが無い / ホールドしていないとき (記録中の Pause 等) は
    // ConsumeSimulateTick だけで進む
    if (tt_ != nullptr && tt_->Scrubbing()) {
        tt_->RequestStep(1);
    }
}

void ReleasePlaySessionEngineState(AudioSystem* audio, ComputeAbiRunner* computeAbi)
{
    // M45: 鳴っている voice はエンジン側の状態なので戻らない。ループ音や BGM が Stop 後も
    // 鳴り続けるのを防ぐ。BGM は別レーンなので StopMusic も要る (M45f)
    if (audio != nullptr) {
        audio->StopAll();
        audio->StopMusic(kMusicStopFadeSeconds);
    }
    // スクリプトが作って解放しなかったバッファとシェーダ別のバインドを回収する。
    // 残すと Play を繰り返すうちに上限 (kMaxAbiBuffers) に達し、CreateComputeBuffer が 0 を返す
    if (computeAbi != nullptr) {
        computeAbi->Shutdown();
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
