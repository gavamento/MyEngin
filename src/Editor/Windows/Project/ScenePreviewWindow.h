//====================================================================================
//                          ScenePreviewWindow.h
//  MyEngine/ 秋田蓮音                                                      11/16/2026
//                                          Before / After の 3D 比較の窓 (専用 RT に描く。宣言)
//====================================================================================
#pragma once
#include <memory>

#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/RestoreFileIo.h"
#include "Editor/ChangeTracking/ScenePreviewSession.h"
#include "Engine/Engine/Rendering/RenderSystem.h"
#include "Engine/Renderer/Device/RenderTexture.h"

namespace mye {

struct EngineContext;

// ある Revision の Before と After を、プレビュー専用の Scene へ読んで専用の RT に描く (前例: AnimationPreviewWindow)。
// 表示は Before / After / 重ね / 左右 (カメラは共有)。**本番の Scene・Undo・Selection・ReloadHub・プロジェクトのファイルに触れない**
// (状態は ScenePreviewSession が持ち、内容は blob_get か読み取りだけで取る)。GameView・撮影・Runtime には出ない (この窓の RT だけ)。
// GPU 資源 (RenderTexture / RenderSystem) はデバイス消失で ReleaseGpu し、次の描画で作り直す (M88)。Scene は CPU 側にあるので残る
class ScenePreviewWindow {
public:
    bool open = false;

    // 開く。シーンの文書でなければ false (何も開かない)。再び開くと前のプレビューを破棄して取り直す
    bool Open(ChangeTrackingSession& session, ScenePreviewOpenArgs args);
    // 閉じて Scene と RT を破棄する
    void Close();

    void OnImGui(ChangeTrackingSession& session);
    // D3D の描画はここだけ (EditorApp::OnRenderViews から)。閉じている・読めた側が無ければ何もしない
    void OnRenderViews(EngineContext& ctx);
    void ReleaseGpu();

    ScenePreviewSession& Session() { return session_; }
    const ScenePreviewSession& Session() const { return session_; }
    // 試験用: 今のファイルの読み取り口を差し替える (Open の前に)。既定は本物の読み取り専用の実装
    void SetFileIo(std::unique_ptr<RestoreFileIo> io);
    // 試験用: 作られている RT の数
    int GpuTargetCount() const { return (rt_[0].IsValid() ? 1 : 0) + (rt_[1].IsValid() ? 1 : 0); }
    // 試験用: 直近の ImGui フレームが決めた、描く画像の数と 1 枚の大きさ
    int ImageCount() const { return imageCount_; }

private:
    static constexpr int kSideBefore = 0;
    static constexpr int kSideAfter = 1;

    void DrawControls(ChangeTrackingSession& session);
    void DrawImages(ChangeTrackingSession& session);
    // 重ねの印 (変わったエンティティ)。画像の左上と大きさは画面の px
    void DrawMarkers(ChangeTrackingSession& session, float rectX, float rectY, float width, float height);
    void ReleaseTargets();

    // ★宣言順に意味がある: session_ が持つ生ポインタ (io_) より先に壊れないように、io_ を先に宣言する
    std::unique_ptr<RestoreFileIo> io_;
    ScenePreviewSession session_;
    // 側 (Before / After) ごとに 1 つ。RenderSystem は描いた Scene の履歴を持つので、側を混ぜない。
    // 大きいオブジェクトなので EditorApp の中に直に持たず、描くときに作る (EditorApp はスタックに置かれ、Debug ではセルフテストの余裕を食う)
    std::unique_ptr<RenderSystem> render_[2];
    // 画像の枠ごとの RT (左右のときだけ 2 枚)
    RenderTexture rt_[2];
    int imageCount_ = 0;
    int slotSide_[2] = { kSideAfter, kSideAfter }; // 枠が描く側
    int imageWidth_ = 0;
    int imageHeight_ = 0;
    bool focusRequested_ = false; // 開いた直後に前面へ (中央のタブ束に入るので、出さないと別のタブの裏に隠れる)
};

} // namespace mye
