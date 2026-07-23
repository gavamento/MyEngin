#pragma once
#include <DirectXMath.h>

#include "imgui.h"
#include "ImGuizmo/ImGuizmo.h"

#include "Engine/Engine/EngineLoop.h"
#include "Engine/Renderer/EditorLinePass.h"
#include "Engine/Renderer/PickingPass.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

struct Selection;
class UndoStack;
struct EditorSettings;

// エディタカメラでシーンを描画するビュー (engine_spec.md 9 章)。
// カメラはエンティティではなくエディタ所有 (Play 状態と無関係に操作できる)。
// M9: ImGuizmo による移動/回転/拡大ギズモ、選択オブジェクトのフレーミング、
//     オービット/パン/ズームのカメラ操作。
class SceneViewWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnRenderViews(EngineContext& ctx, Selection& selection); // フェーズ 6: RT へ描画 + 補助線
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo, EditorSettings& settings);

    // ビュー中心をピッキングして選択する (自動テスト用 — --pick-test)。ヒットで true
    bool PickAtCenter(EngineContext& ctx, Selection& selection);

    // ギズモ状態への参照 (M27c: グローバルツールバーと共有)
    ImGuizmo::OPERATION& GizmoOp() { return gizmoOp_; }
    ImGuizmo::MODE& GizmoMode() { return gizmoMode_; }

private:
    void BuildOverlays(EngineContext& ctx, Selection& selection);
    void DrawToolbar(EditorSettings& settings);
    void DrawGizmo(EngineContext& ctx, Selection& selection, UndoStack& undo,
                   const EditorSettings& settings, float rectX, float rectY, float rectW,
                   float rectH);
    void HandleCamera(EngineContext& ctx, Selection& selection, EditorSettings& settings);
    void FocusOnSelection(EngineContext& ctx, Selection& selection);
    // カーソル下のワールド地面 (y=0) 座標を求める (ドラッグ配置用)。ヒットで true
    bool GroundPointUnderCursor(const ImVec2& imgPos, const ImVec2& size,
                                DirectX::XMFLOAT3& out) const;

    RenderTexture rt_;
    DirectX::XMFLOAT3 camPos_ = { 0.0f, 7.0f, -16.0f };
    float camYaw_ = 0.0f;
    float camPitch_ = 18.0f;
    int desiredW_ = 0;
    int desiredH_ = 0;

    // 描画に使った view/proj (ギズモがピクセル一致するよう OnRenderViews で保存)
    DirectX::XMFLOAT4X4 lastView_ = {};
    DirectX::XMFLOAT4X4 lastProj_ = {};

    // ギズモ状態
    ImGuizmo::OPERATION gizmoOp_ = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE gizmoMode_ = ImGuizmo::LOCAL;
    bool orthographic_ = false;
    bool gizmoActive_ = false; // Undo transient 記録中 (ドラッグ全体で 1 エントリ)
    bool camSpeedDirty_ = false; // RMB+ホイールで速度変更中 (RMB リリース時に settings.Save)
    bool showGrid_ = true;
    bool showGizmos_ = true; // コライダー/ライト/カメラ等の補助表示 (ビルボードアイコン含む)
    int viewMode_ = 0;       // SceneView 表示モード (M40b): 0=Lit 1=Unlit 2=Wireframe

    PickingPass picking_;    // クリック選択 (遅延 Init)
    EditorLinePass lines_;   // グリッド/ワイヤ/アウトライン (遅延 Init)
};

} // namespace mye
