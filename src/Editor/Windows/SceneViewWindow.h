#pragma once
#include <DirectXMath.h>

#include "imgui.h"
#include "ImGuizmo/ImGuizmo.h"

#include "Engine/Engine/Asset/TerrainEdit.h" // M58f: 地形ブラシ
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
    // 地形ブラシ (M58f)。カーソル下の地表を求めてダブを置き、リング表示を重ねる。
    // 戻り値 = 左ボタンを消費した (= ピッキング/ギズモへ流さない)
    bool HandleTerrainBrush(EngineContext& ctx, Selection& selection, UndoStack& undo,
                            const ImVec2& imgPos, const ImVec2& size);
    void DrawTerrainBrushPanel(EngineContext& ctx);
    void FocusOnSelection(EngineContext& ctx, Selection& selection);
    // カーソル位置のワールドレイ (origin + 正規化 dir)。ビュー行列が特異なら false
    bool MouseRay(const ImVec2& imgPos, const ImVec2& size, DirectX::XMFLOAT3& origin,
                  DirectX::XMFLOAT3& dir) const;
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

    // 右クリック生成メニュー: ポップアップを開いた瞬間の地面点 (メニュー操作中にカーソルが
    // 動くため開いた時点で固定する)
    DirectX::XMFLOAT3 ctxSpawnPos_ = {};
    bool ctxSpawnValid_ = false;

    // ---- 地形ブラシ (M58f) ----
    // ★状態はエディタ側にしか無い (シーンにも設定ファイルにも保存しない) — ブラシは
    //   「いまの操作」であって地形アセットの属性ではないため。
    bool terrainBrush_ = false; // ブラシモード (on の間はピッキングとギズモを止める)
    int terrainBrushMode_ = 0;  // 0=Raise 1=Smooth 2=Paint (TerrainEdit::BrushMode と同順)
    float terrainRadius_ = 12.0f;
    float terrainStrength_ = 1.0f; // Raise は m/ダブ、Smooth/Paint は 0..1
    int terrainLayer_ = 0;         // Paint の対象レイヤ
    bool terrainStroking_ = false;  // 左ドラッグ中 (= 1 Undo エントリの単位)
    bool terrainHasTarget_ = false; // 直近のフレームで塗れる地形が見つかったか (パネル表示用)
    std::wstring terrainStrokeSrc_;              // ストローク対象の `.terrain.json` 絶対パス
    TerrainAsset::TerrainData terrainStrokeBase_; // ストローク開始時の画素 (差分の基準)
    TerrainAsset::TerrainData terrainStrokeWork_; // 進行中の画素 (ダブごとに書き出す)
    DirectX::XMFLOAT3 terrainLastDab_ = { 0.0f, 0.0f, 0.0f }; // 直前のダブ位置 (間隔判定)
    bool terrainHasDab_ = false;

    PickingPass picking_;    // クリック選択 (遅延 Init)
    EditorLinePass lines_;   // グリッド/ワイヤ/アウトライン (遅延 Init)
};

} // namespace mye
