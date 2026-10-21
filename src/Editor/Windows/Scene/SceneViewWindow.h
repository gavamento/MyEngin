#pragma once
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "imgui.h"
#include "ImGuizmo/ImGuizmo.h"

#include "Editor/Widgets/EditorWidgets.h" // M47b追補: ToolbarFlow (ツールバー折り返し)
#include "Engine/Engine/Asset/TerrainEdit.h" // M58f: 地形ブラシ
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Renderer/Passes/EditorLinePass.h"
#include "Engine/Renderer/Passes/GhostMeshPass.h" // M72i: 分岐のゴーストのメッシュ
#include "Engine/Renderer/Passes/PickingPass.h"
#include "Engine/Renderer/Device/RenderTexture.h"

namespace mye {

struct Selection;
class World;
class UndoStack;
struct EditorSettings;
struct CameraComponent;
struct ColliderComponent;
class TilePaletteWindow;

// エディタカメラでシーンを描画するビュー (engine_spec.md 9 章)。
// カメラはエンティティではなくエディタ所有 (Play 状態と無関係に操作できる)。
// M9: ImGuizmo による移動/回転/拡大ギズモ、選択オブジェクトのフレーミング、
//     オービット/パン/ズームのカメラ操作。
class SceneViewWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    // 2D モード (正射影 + XY 正面)。レイアウト保存の対象で、シーンには保存しない。
    // 切り替えの副作用 (カメラ姿勢の退避と復元) は次の OnRenderViews が SyncMode2d で行う
    bool mode2d = false;
    void OnRenderViews(EngineContext& ctx, Selection& selection); // フェーズ 6: RT へ描画 + 補助線
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo, EditorSettings& settings);
    // デバイス復旧 (M88): RT と遅延 Init のパスを手放す。次の OnRenderViews / ピック要求で作り直される
    void ReleaseGpu();

    // M94l: タイルパレット窓。2D モードで窓が開いている間、左ボタンはセルを塗る入力になる (EditorApp が毎フレーム渡す)
    void SetTilePalette(TilePaletteWindow* window) { tilePalette_ = window; }

    // ビュー中心をピッキングして選択する (自動テスト用 — --pick-test)。ヒットで true
    bool PickAtCenter(EngineContext& ctx, Selection& selection);

    // M56e: エディタカメラのワールド位置。反射プローブを「今見ている場所」で焼くのに使う
    const DirectX::XMFLOAT3& CameraPosition() const { return camPos_; }

    // ギズモ状態への参照 (M27c: グローバルツールバーと共有)
    ImGuizmo::OPERATION& GizmoOp() { return gizmoOp_; }
    ImGuizmo::MODE& GizmoMode() { return gizmoMode_; }

private:
    void BuildOverlays(EngineContext& ctx, Selection& selection);
    // mode2d の変化を検出してカメラ姿勢を切り替える (ON: 3D 姿勢を退避して XY 正面へ / OFF: 退避を復元)
    void SyncMode2d();
    // 描画・ギズモ・ピッキングが使う投影が正射影か (従来の Ortho トグルか 2D モード)
    bool ViewIsOrtho() const { return orthographic_ || mode2d; }
    // BuildOverlays が同じ順に呼ぶギズモ (showGizmos_ のときだけ)。どれも lines_ へ線を積むだけ
    void DrawColliderGizmos(World& world, EntityID only = kNullEntity);
    void DrawLightGizmos(World& world);
    void DrawCameraGizmos(EngineContext& ctx, World& world);
    void DrawEmitterGizmos(World& world);
    void DrawSpringJointGizmos(World& world);
    void DrawJointGizmos(World& world);
    void DrawConstantForceGizmos(World& world);
    void DrawCharacterControllerGizmos(World& world);
    void DrawSpriteGizmos(World& world);
    void DrawSprite2DGizmos(EngineContext& ctx, World& world, const Selection& selection);
    void DrawTilemapGizmos(EngineContext& ctx, World& world, const Selection& selection); // M94j
    void DrawLight2DGizmos(EngineContext& ctx, World& world);                              // M94o
    void DrawShadowCaster2DGizmos(World& world);                                           // M94p
    void DrawLine2DGizmos(World& world);                                                   // M94r
    void DrawEffector2DGizmos(World& world);                                                 // M94t
    void DrawGridAgent2DGizmos(World& world);                                                // M94u
    void DrawCamera2DFollowGizmos(World& world);                                             // M94v
    void DrawSpriteShape2DGizmos(World& world);                                              // M94s
    // M94h: kPolygon2D / kEdge2D の点列 (緑) を前後面の輪郭と支柱で描く
    void AddShape2DWire(const ColliderComponent& col, const DirectX::XMFLOAT4X4& wm);
    void DrawAudioGizmos(EngineContext& ctx, World& world, const Selection& selection);
    void DrawTextMeshGizmos(World& world);
    void DrawPartSocketGizmos(World& world);
    void DrawPartBoundsGizmos(World& world);
    void DrawReflectionProbeGizmos(World& world);
    void DrawNavSurfaceGizmos(World& world); // M82e: Surface のベイク範囲
    void DrawNavObstacleGizmos(World& world); // M82f: Obstacle の切り抜く形
    void DrawNavModifierGizmos(World& world); // M82g: Modifier の塗る箱
    void DrawNavLinkGizmos(World& world);     // M82h: Link の入口・出口・矢印
    // M85g: 選択中の PatrolRoute の点の球・線・向きの矢印 (番号は ImGui 側 = HandlePatrolRoute)
    void DrawPatrolRouteGizmos(EngineContext& ctx, World& world, const Selection& selection);
    // M83: 選択中の AIPerception の視野 (扇形: 見える距離と見失う距離)・聞こえる距離の円・目の位置
    void DrawPerceptionGizmos(EngineContext& ctx, World& world, const Selection& selection);
    void DrawSelectionOutline(EngineContext& ctx, World& world, const Selection& selection);
    // 分岐のゴースト (M72e、SceneViewGhost.cpp): 非ライブの分岐を同じ tick のワイヤ箱 + トレイルで重ねる
    void BuildGhostOverlay(EngineContext& ctx);
    void DrawToolbar(EditorSettings& settings);
    // M85j: drawDebug が立った BehaviorTreeComponent の頭上に、実行中のタスク名を重ねる (描画専用。線は BehaviorTreeSystem::AppendDebugLines)
    void DrawBehaviorTreeLabels(EngineContext& ctx, World& world, float rectX, float rectY, float rectW, float rectH);
    // M85g: 選択中の PatrolRoute の点の番号・クリック選択・ドラッグ移動 (ドラッグ 1 回 = 1 Undo)。
    // 点を選んでいる間は true を返す (呼び出し側はエンティティの変形ギズモを出さない)。点をクリックしたら clickConsumed が true
    bool HandlePatrolRoute(EngineContext& ctx, Selection& selection, UndoStack& undo, const EditorSettings& settings,
                           float rectX, float rectY, float rectW, float rectH, bool& clickConsumed);
    // M94h: 「Edit Collider」が入った kPolygon2D / kEdge2D の点の表示・ドラッグ移動・辺クリックで追加・Delete で削除。
    // 編集はアセット (.shape2d.json) に書く (ドラッグ 1 回 = 保存 1 回、Undo 無し)。
    // 編集中は true を返す (呼び出し側は変形ギズモを出さない)。点・辺をクリックしたら clickConsumed が true
    bool HandleShape2DEdit(EngineContext& ctx, Selection& selection, const EditorSettings& settings, float rectX,
                           float rectY, float rectW, float rectH, bool& clickConsumed);
    // M94r: 選択中の Line2D の点の表示・ドラッグ移動・辺クリックで追加・Delete で削除。点をクリックして選ぶまでは false
    // (変形ギズモを出す)。点を選んでいる間は true を返し、変形ギズモを出さない。ドラッグ 1 回 = 追加 1 回 = 削除 1 回 = 1 Undo。
    // 点・辺をクリックしたら clickConsumed が true
    bool HandleLine2DEdit(EngineContext& ctx, Selection& selection, UndoStack& undo, const EditorSettings& settings,
                          float rectX, float rectY, float rectW, float rectH, bool& clickConsumed);
    // M94s: 選択中の SpriteShape2D の点編集 (Line2D と同じ流儀。点を選ぶまでは false)
    bool HandleSpriteShape2DEdit(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                 const EditorSettings& settings, float rectX, float rectY, float rectW, float rectH,
                                 bool& clickConsumed);

    // ---- 点列ハンドル編集の共通部 (HandleShape2DEdit / HandleLine2DEdit / HandleSpriteShape2DEdit) ----
    // 点列の持ち主 (ワールド行列) と Scene View の描画矩形。点は持ち主のローカル XY
    struct PointHandleFrame {
        const DirectX::XMFLOAT4X4* world = nullptr;
        float rectX = 0.0f;
        float rectY = 0.0f;
        float rectW = 0.0f;
        float rectH = 0.0f;
    };
    // 点ハンドル 1 フレームぶん: 点の画面位置、カメラ前方か、マウスに最も近い点 (ピック半径内、無ければ -1)
    struct PointHandleScan {
        std::vector<ImVec2> screen;
        std::vector<char> visible;
        int nearest = -1;
    };
    // コンポーネントのフィールドへ直接書く点リスト (Line2D / SpriteShape2D)。minCount 以下には削除しない
    struct PointListTarget {
        DirectX::XMFLOAT2* points = nullptr;
        int32_t* pointCount = nullptr;
        int maxCount = 0;
        int minCount = 0;
        bool closedLoop = false; // 最後の点から最初の点への辺も、辺クリックの追加先に数える (3 点以上)
    };
    struct PointListEditState {
        int point = -1;        // 選んでいる点 (-1 = 無し)
        uint64_t fileId = 0;   // 選んだときのエンティティ (ドラッグ中の Undo 記録の対象でもある)
        bool dragging = false; // 点のドラッグ中 (UndoStack の記録を開いている)
    };
    struct PointListEditLabels {
        const char* edit;
        const char* add;
        const char* remove;
        const char* banner; // 編集中に画面下へ出す案内
    };
    bool PointToScreen(const PointHandleFrame& f, const DirectX::XMFLOAT2& p, ImVec2& out) const;
    bool MouseToLocal(const PointHandleFrame& f, DirectX::XMFLOAT2& out) const;
    void ScanPointHandles(const PointHandleFrame& f, const DirectX::XMFLOAT2* pts, int count, int chosen,
                          ImU32 normalColor, PointHandleScan& scan) const;
    int PickSegmentUnderMouse(const PointHandleScan& scan, int count, int segments) const;
    void ClosePointListDrag(EngineContext& ctx, Selection& selection, UndoStack& undo, PointListEditState& st);
    bool EditPointList(EngineContext& ctx, Selection& selection, UndoStack& undo, const EditorSettings& settings,
                       const PointHandleFrame& frame, const PointListTarget& target, PointListEditState& st,
                       const PointListEditLabels& labels, bool& clickConsumed);
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
    // M94l: タイルパレット窓へ Scene View のマウスをセル座標にして渡し、カーソルのセル・ドラッグ範囲・選択範囲の枠を重ねる。
    // 戻り値 = 左ボタンを窓に渡した (= ピッキング / ギズモへ流さない)
    bool HandleTilePaint(EngineContext& ctx, const ImVec2& imgPos, const ImVec2& size);
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
    // 直近の OnImGui でビューが実際に見えていたか (閉じた / タブの裏 = false)。
    // ★OnRenderViews は OnImGui より**前**に走るので「前フレームの可視性」で判定する。
    //   これが無いと、見えていないビューも毎フレーム GBuffer + RT の全パスを描いていた
    bool shownLastFrame_ = false;

    // 描画に使った view/proj (ギズモがピクセル一致するよう OnRenderViews で保存)
    DirectX::XMFLOAT4X4 lastView_ = {};
    DirectX::XMFLOAT4X4 lastProj_ = {};

    // ギズモ状態
    ImGuizmo::OPERATION gizmoOp_ = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE gizmoMode_ = ImGuizmo::LOCAL;
    bool orthographic_ = false;
    // 2D モードの状態。orthoSize2d_ は画面の半高 (ワールド単位)。saved3d* は ON にする直前の 3D 姿勢
    float orthoSize2d_ = 5.0f;
    bool applied2d_ = false; // SyncMode2d が最後に反映した mode2d
    DirectX::XMFLOAT3 saved3dPos_ = { 0.0f, 7.0f, -16.0f };
    float saved3dYaw_ = 0.0f;
    float saved3dPitch_ = 18.0f;
    bool saved3dOrtho_ = false;
    bool gizmoActive_ = false; // Undo transient 記録中 (ドラッグ全体で 1 エントリ)
    uint64_t gizmoFileId_ = 0;
    bool gizmoCollider_ = false;
    bool gizmoPatrol_ = false; // 記録中のドラッグが巡回点のもの
    int patrolPoint_ = -1;     // ドラッグで動かすために選んでいる巡回点 (-1 = 無し)
    uint64_t patrolFileId_ = 0; // patrolPoint_ を選んだときのエンティティ (選択が変わったら捨てる)
    int shape2dPoint_ = -1;      // M94h: 編集で選んでいる 2D 形状の点 (-1 = 無し)
    uint64_t shape2dFileId_ = 0; // 同 選んだときのエンティティ
    bool shape2dDragging_ = false;
    PointListEditState line2dEdit_;       // M94r: 編集で選んでいる Line2D の点
    PointListEditState spriteShapeEdit_;  // M94s: 同 SpriteShape2D の点
    std::vector<DirectX::XMFLOAT2> shape2dDragPoints_; // ドラッグ中の作業用点列 (離したら保存)
    bool gizmoBlockedUntilRelease_ = false;
    bool camSpeedDirty_ = false; // RMB+ホイールで速度変更中 (RMB リリース時に settings.Save)
    bool showGrid_ = true;
    bool showGizmos_ = true; // コライダー/ライト/カメラ等の補助表示 (ビルボードアイコン含む)
    bool showGhosts_ = true; // 分岐のゴースト (M72e)。分岐が無ければ何も描かない = 既定 on で無害
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

    TilePaletteWindow* tilePalette_ = nullptr; // M94l: 所有は EditorApp
    PickingPass picking_;    // クリック選択 (遅延 Init)
    EditorLinePass lines_;   // グリッド/ワイヤ/アウトライン (遅延 Init)
    GhostMeshPass ghostMesh_; // M72i: 分岐のゴーストの半透明メッシュ (遅延 Init)。線より先に描く
};

} // namespace mye
