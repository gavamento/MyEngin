// ============================================================================
//                          AnimationPreviewWindow.h
// ============================================================================
// 骨アニメのプレビュー窓 (M89n)。選択中のキャラの SkinnedMesh を専用の一時シーンへ写し、
// 骨クリップ / コントローラのステートを任意の tick で描く。シーンの sim には触れない。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Rendering/RenderSystem.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Renderer/Device/RenderTexture.h"

namespace mye {

struct Selection;
struct SkinnedModel;
class SkinnedModelLibrary;
struct ControllerAsset;
struct ControllerState;
struct ControllerClipEvent;
class ControllerLibrary;

// 選択エンティティ (AnimatorController を持つキャラ、または SkinnedMesh そのもの) が駆動する SkinnedMesh を
// 一時シーンへ写して描く。モードは 2 つ:
//   - クリップ: 主 SkinnedMesh のモデルの名前付き骨クリップ 1 本
//   - ステート: 選択エンティティのコントローラの、骨を駆動するステート (クリップ 1 本 / 1D・2D ブレンドツリー)。
//     ブレンドのパラメータは窓の中のスライダで決める (シーンのコンポーネントは書き換えない)
// 再生・スクラブ・±1 tick、骨の線 (メッシュの上に重ねる)。
// タイムラインの下の帯でコントローラの clipEvents を編集する (M89o: 追加・ドラッグ・削除・種類別の欄。保存は窓のボタン)。
// ルートジョイントの 1 周の軌跡を画像に重ねる (M89o。窓はポーズから水平分を抜かないので、メッシュも軌跡に沿って動く)。
// ポーズはエンジンと同じポーズプログラム (SkinnedMesh.poseLayers) を書いて SampleSkinnedLocals で描く。
// GPU 資源 (RenderTexture / RenderSystem) はデバイス消失で ReleaseGpu し、次の描画で作り直す (M88)
class AnimationPreviewWindow {
public:
    bool open = false;
    void OnImGui(EngineContext& ctx, Selection& selection);
    // D3D の描画はここだけ (EditorApp::OnRenderViews から)。窓が閉じている・対象が無ければ何もしない
    void OnRenderViews(EngineContext& ctx);
    void ReleaseGpu();
    // 撮影用 (--anim-preview-tick N): 再生を止めてこの tick を表示する
    void SetTick(int32_t tick);
    // イベントを編集したコントローラのうち 1 本でもディスクと食い違うか (AnimatorControllerWindow と同じ判定)
    bool HasUnsavedChanges() const;

private:
    enum class Mode : int32_t { Clip = 0, State = 1 };
    // ポーズプログラムの元 1 層 (エンジンの SkeletalSource の窓版)
    struct Source {
        std::string clip;       // イベントの一覧を引く名前 (clipEvents のキー)
        uint64_t clipHash = 0;
        bool usesPhase = false; // true = ブレンドツリーの子 (位相をメッシュごとの長さで時刻にする)
        int32_t weightQ = 0;
    };

    void Rebuild(EngineContext& ctx, EntityID sourceRoot, const std::vector<EntityID>& driven);
    void WritePose(const SkinnedModelLibrary& models, int32_t tick);
    void FitCamera(const SkinnedModelLibrary& models);
    void DrawTimeline(ControllerLibrary* controllers, ControllerAsset* ctrl, const SkinnedModel* mainModel);
    // 選択中のイベントの欄 (種類で出す欄が変わる)。何か変えたら true
    bool DrawEventFields(ControllerClipEvent& ev, int32_t clipTicks, const SkinnedModel* mainModel);
    void ComputeRootTrail(const SkinnedModelLibrary& models, const SkinnedModel* mainModel);
    void DrawOverlay(const SkinnedModelLibrary& models, const DirectX::XMFLOAT2& rectMin, const DirectX::XMFLOAT2& rectSize);
    int32_t LastTick() const; // タイムラインの右端 (lengthTicks_ と loop_ から)
    DirectX::XMMATRIX ViewMatrix() const;

    // ---- 一時シーンと描画 ----
    Scene scene_;
    TransformSystem transforms_;
    RenderSystem render_;
    RenderTexture rt_;
    std::vector<EntityID> meshes_; // 写した SkinnedMesh (元の前順のまま。先頭が主 = entity index 最小とは限らない)
    int32_t mainMesh_ = -1;        // meshes_ のうち主 SkinnedMesh (元の entity index が最小)
    bool built_ = false;
    uint64_t builtFor_ = 0;        // 写した元の選択 (fileId)
    size_t builtCount_ = 0;
    int32_t width_ = 0;
    int32_t height_ = 0;

    // ---- 何をどこで見せるか ----
    Mode mode_ = Mode::Clip;
    std::string clipName_;      // クリップモードの骨クリップ名
    int32_t stateIndex_ = -1;   // ステートモードのステート (ControllerAsset::states の index)
    float paramX_ = 0.0f;       // ブレンドツリーのパラメータ (窓の中だけの値)
    float paramY_ = 0.0f;
    std::vector<Source> sources_;
    int32_t loop_ = 1;
    int32_t lengthTicks_ = 0;   // 今のクリップ / ステートの 1 周 (tick)
    int32_t tick_ = 0;
    bool playing_ = true;
    float playAccum_ = 0.0f;    // 再生の端数 (tick)
    bool showBones_ = true;
    bool showTrail_ = true;

    // ---- イベントの編集 (M89o) ----
    uint64_t selClipHash_ = 0;  // 選択中のイベントが属するクリップ。タイムラインの対象 (最も重い層) が変わったら選択を外す
    int32_t selEvent_ = -1;     // そのクリップの events の index
    bool dragging_ = false;     // 帯の上で選択中のイベントをつかんでいる
    std::vector<int32_t> eventTimelineTicks_; // 今のクリップのイベントのタイムライン上の tick (軌跡の印に使う)
    ControllerLibrary* controllers_ = nullptr; // 寿命は EngineContext と同じ
    std::vector<uint64_t> touched_; // 未保存の判定の対象 (この窓でイベントを触ったコントローラ)

    // ---- ルートの軌跡 (M89o) ----
    std::vector<DirectX::XMFLOAT3> trail_; // tick 0..最後の tick の、主 SkinnedMesh のルートジョイントの位置 (一時シーンの空間)

    // ---- カメラ (対象の周りを回る) ----
    float yaw_ = 0.6f;
    float pitch_ = 0.25f;
    float distance_ = 3.0f;
    DirectX::XMFLOAT3 center_ = { 0.0f, 1.0f, 0.0f };
    std::vector<DirectX::XMMATRIX> locals_; // 骨の線の作業領域
};

} // namespace mye
