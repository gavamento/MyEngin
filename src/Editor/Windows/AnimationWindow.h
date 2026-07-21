#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include <DirectXMath.h>

#include "Editor/Selection.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

class UndoStack;
struct AnimationClipAsset;

// Animation ウィンドウ (engine_spec.md 9 章、M14)。
// 選択エンティティの AnimatorComponent + AnimationClip を編集する:
//   タイムライン (スクラブ)、トラック/ドープシート、キー追加・削除、
//   プレビュー (編集時は AnimationSystem が動かないので窓が明示サンプリング → 終了時に復元)、
//   レコードモード (Transform 変更を現在 tick に自動キー化)。
class AnimationWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo);

private:
    void StartPreview(EngineContext& ctx, EntityID animator);
    void StopPreview(EngineContext& ctx);
    void HandleRecord(EngineContext& ctx, AnimationClipAsset& clip, EntityID animator);

    int32_t previewTick_ = 0;
    bool preview_ = false;   // スクラブでポーズ表示 (終了時に元ポーズへ復元)
    bool playing_ = false;   // プレビュー自動再生
    bool recording_ = false; // Transform 変更を自動キー化
    uint64_t activeFid_ = 0; // プレビュー/レコード中の animator fileId

    std::vector<std::pair<EntityID, LocalTransform>> snapshot_; // プレビュー前のポーズ

    // レコード用: 前フレームの LocalTransform (変化検出)
    DirectX::XMFLOAT3 recPos_ = { 0, 0, 0 };
    DirectX::XMFLOAT4 recRot_ = { 0, 0, 0, 1 };
    DirectX::XMFLOAT3 recScl_ = { 1, 1, 1 };
    bool recValid_ = false;

    AnimationSystem sampler_;
};

} // namespace mye
