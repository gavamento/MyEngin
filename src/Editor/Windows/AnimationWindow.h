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

    // 触ったクリップのうち 1 本でもディスクと食い違うか (M66d、spec §4.1 の S6)。
    // ★この窓は dirty フラグを持たない (編集がキー追加/削除/長さ/レコードと散っていて、
    //   1 箇所漏らすと「未保存なのに保存済みに見える」= git の書き込みで黙って消える)。
    //   代わりに「この窓が触ったクリップ」を覚えておいて、そのつど直列化して照合する。
    //   評価が高いので呼ぶのは GitTransaction のゲートだけ (500 ms キャッシュ付き)
    bool HasUnsavedChanges() const;

private:
    // 触ったクリップを記録する (同じものは 1 回だけ)
    void MarkTouched(AnimationLibrary* anims, uint64_t clipHash);

    // ★ライブラリのポインタは EngineContext の寿命と同じ (EngineLoop がメンバで持つ)
    //   ので、窓を閉じた後でも安全に読める。窓を一度も開いていなければ nullptr =
    //   「この窓では何も編集していない」で正しい
    AnimationLibrary* anims_ = nullptr;
    std::vector<uint64_t> touched_;

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
