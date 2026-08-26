#pragma once
#include <string>

#include <DirectXMath.h>

#include "imgui.h"
#include "ImGuizmo/ImGuizmo.h"

#include "Editor/EditorWidgets.h" // M47b追補: ToolbarFlow (ツールバー折り返し)
#include "Engine/Engine/Asset/TerrainEdit.h" // M58f: 地形ブラシ
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Renderer/EditorLinePass.h"
#include "Engine/Renderer/PickingPass.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

struct Selection;
class UndoStack;
struct EditorSettings;
struct CameraComponent;

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

    // M56e: エディタカメラのワールド位置。反射プローブを「今見ている場所」で焼くのに使う
    const DirectX::XMFLOAT3& CameraPosition() const { return camPos_; }

    // ギズモ状態への参照 (M27c: グローバルツールバーと共有)
    ImGuizmo::OPERATION& GizmoOp() { return gizmoOp_; }
    ImGuizmo::MODE& GizmoMode() { return gizmoMode_; }

private:
    void BuildOverlays(EngineContext& ctx, Selection& selection);
    void DrawToolbar(EditorSettings& settings);
    void DrawGizmo(EngineContext& ctx, Selection& selection, UndoStack& undo,
                   const EditorSettings& settings, float rectX, float rectY, float rectW,
                   float rectH);
    void HandleCamera(EngineContext& ctx, Selection& selection, UndoStack& undo,
                      EditorSettings& settings);
    // ---- カメラの視錐台ワイヤ / 操縦モード / プレビュー窓 ----
    // 操縦対象のエンティティ。操縦していない / 対象が消えた / Camera を失ったら kNullEntity
    // (消えていたら操縦自体も畳む — バナーだけ残り続けるのを防ぐ)
    EntityID PilotTarget(EngineContext& ctx);
    // ワイヤ/プレビューの対象 = 操縦中ならその対象、でなければ選択中のカメラ。0 = 無し
    uint64_t ResolveCameraFid(EngineContext& ctx, const Selection& selection);
    void AddFrustumWire(const DirectX::XMFLOAT4X4& world, const CameraComponent& cam);
    void RenderCameraPreview(EngineContext& ctx);
    // 操縦中のカメラ操作 (エディタカメラと同じ入力を、書き込み先だけ振り替える)
    void HandlePilotCamera(EngineContext& ctx, Selection& selection, UndoStack& undo,
                           EditorSettings& settings, EntityID cam);
    // 開きっぱなしの Undo 記録を閉じる (ボタンを離した / 操縦をやめた / 窓の外で離した)
    void ClosePilotRecord(EngineContext& ctx, Selection& selection, UndoStack& undo);
    void DrawPilotBanner();
    void DrawCameraPreview(const ImVec2& imgPos, const ImVec2& size);
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

    // ---- カメラの視錐台ワイヤ / 操縦モード / プレビュー窓 ----
    // ワイヤもプレビューも「選択中のカメラ 1 台」だけに出す。全カメラに視錐台を描くと
    // カメラが数台あるだけで画面が線だらけになるため (Unity/Unreal も選択中のみ)。
    // ★ワイヤの画角とプレビューの絵は**同じ 1 つのアスペクト**から引く (食い違うと
    //   「線の通りに写らない」= 嘘の絵になる)。CameraComponent は aspect を持たない —
    //   描画時にレンダーターゲットの実寸で決まる値なので、ここで 16:9 に固定している
    uint64_t camTargetFid_ = 0;   // ワイヤ/プレビューの対象 (0 = 無し)
    float frustumFar_ = 25.0f;    // 視錐台の**表示上の**打ち切り距離 (実 farZ が小さければそちら)
    ToolbarFlow toolbarFlow_;     // M47b追補: パネルが狭いときのツールバー折り返し
    RenderTexture previewRt_;
    bool previewValid_ = false;   // 直近の OnRenderViews でプレビューを描けたか
    std::string previewLabel_;    // プレビュー窓とバナーに出す対象カメラの名前
    // 操縦ドラッグ中の Undo 記録 (ドラッグ全体で 1 エントリ = ギズモと同じ流儀)。
    // 対象 fileId を控えるのは、記録の途中で対象が消えても同じ相手で閉じるため
    bool pilotRecording_ = false;
    uint64_t pilotRecordFid_ = 0;

    PickingPass picking_;    // クリック選択 (遅延 Init)
    EditorLinePass lines_;   // グリッド/ワイヤ/アウトライン (遅延 Init)
};

} // namespace mye
