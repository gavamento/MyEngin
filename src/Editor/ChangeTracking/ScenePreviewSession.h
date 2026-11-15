//====================================================================================
//                          ScenePreviewSession.h
//  MyEngine/ 秋田蓮音                                                      11/16/2026
//                                          Before / After の 3D 比較の状態 (プレビュー専用の Scene・モード・カメラ。GPU に触れない、宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/OverlayPalette.h"
#include "Editor/ChangeTracking/RestorePlan.h"
#include "Editor/ChangeTracking/ScenePreviewContent.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"

namespace mye {

class TrackClient;

// プレビュー専用の Scene と Transform の更新器。本番の Scene・World・ComponentRegistry 以外の状態を共有しない。
// 同時に存在する数を数える (閉じたら 0 に戻ることを試験が確かめる)
class PreviewScene {
public:
    PreviewScene();
    ~PreviewScene();
    PreviewScene(const PreviewScene&) = delete;
    PreviewScene& operator=(const PreviewScene&) = delete;

    Scene scene;
    TransformSystem transforms;

    // 生きている PreviewScene の数
    static int LiveCount();
};

enum class PreviewMode : uint8_t { Before, After, Overlay, SideBySide };
// After の側に出すもの。エディタの未保存の状態は選べない (SaveToJson が fileId を採番しアーキタイプを動かすので、比較のためだけに呼ばない)
enum class PreviewAfterSource : uint8_t { Revision, Disk };

// 変わったエンティティの印 (重ねの表示)。位置はワールド空間
struct PreviewMarker {
    uint64_t fileId = 0;
    std::string name;
    OverlayColorId color = OverlayColorId::Modified;
    DirectX::XMFLOAT3 position = {};
    bool fromBefore = false; // 削除されたもの (After には居ない) は Before の位置
};

// 対象をまわるカメラ (左右の表示では 2 つの画像が同じカメラを使う = 同期)
struct PreviewCamera {
    float yaw = 0.6f;
    float pitch = 0.4f;
    float distance = 6.0f;
    DirectX::XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
};

constexpr float kPreviewFovYDeg = 50.0f;
constexpr float kPreviewNearZ = 0.05f;
constexpr float kPreviewFarZ = 2000.0f;
constexpr float kPreviewMinDistance = 0.2f;
constexpr float kPreviewMaxDistance = 5000.0f;
// 印の上限 (変更が数千あっても、重ねの描画を重くしない)
constexpr int kPreviewMaxMarkers = 256;

// プレビューを開くのに要る入力 (AI Changes 窓が詳細とシーンの変更から集めて渡す)
struct ScenePreviewOpenArgs {
    RestoreRevision revision;
    std::vector<TrackSceneChange> sceneChanges;
    std::wstring projectRoot;
    ManagedComponentFilter isManaged;
    AssetPathResolver assetPath;
};

// ある Revision の Before と After (どちらもサービスの Blob)、または After の代わりに今のディスクの内容を、
// プレビュー専用の Scene へ読んで持つ。**本番の Scene・Undo・Selection・ReloadHub・プロジェクトのファイルに触れない**:
// 内容は blob_get かファイルの読み取りだけで取り、Scene は自前。ImGui も GPU も使わない (描画は ScenePreviewWindow)。
// 1 度に持つ Scene は最大 2 つ (Before と After)
class ScenePreviewSession {
public:
    enum class SideState : uint8_t { Idle, Fetching, Loaded, Failed };

    struct Side {
        SideState state = SideState::Idle;
        PreviewContentFetch fetch;
        PreviewFailure failure = PreviewFailure::None; // state == Failed のとき、取得の失敗 (読み込みの失敗は load.status)
        PreviewLoadInfo load;
        std::unique_ptr<PreviewScene> scene;
    };

    // 開く。シーンの文書 (*.scene.json) でなければ false (何も持たない)。内容の取得はここから始まり、Tick が進める。
    // io は今のファイルを読むために、閉じるまで生きていること
    bool Open(TrackClient& client, RestoreFileIo& io, ScenePreviewOpenArgs args);
    void Close();
    bool IsOpen() const { return open_; }

    // 毎フレーム 1 回 (TrackClient::Poll の後)。取得を進め、読み終えた側を読み込み、両方が済んだら印とカメラと、アセットの確認を始める
    void Tick(TrackClient& client);

    // After の側に何を出すか。切り替えるとその側だけ取り直す
    void SetAfterSource(PreviewAfterSource source, TrackClient& client);
    PreviewAfterSource AfterSource() const { return afterSource_; }

    void SetMode(PreviewMode mode) { mode_ = mode; }
    PreviewMode Mode() const { return mode_; }

    // ---- カメラ (1 つだけ。左右でも共有) ----
    const PreviewCamera& Camera() const { return camera_; }
    void OrbitCamera(float dx, float dy);
    void ZoomCamera(float wheel);
    void PanCamera(float dx, float dy);
    // 変わったエンティティの範囲が収まる位置へ戻す
    void FitCamera();
    DirectX::XMMATRIX ViewMatrix() const;
    DirectX::XMMATRIX ProjMatrix(float aspect) const;

    const Side& Before() const { return before_; }
    const Side& After() const { return after_; }
    // 読み込めた側の Scene (描画用。無ければ null)
    PreviewScene* BeforeScene() { return before_.state == SideState::Loaded ? before_.scene.get() : nullptr; }
    PreviewScene* AfterScene() { return after_.state == SideState::Loaded ? after_.scene.get() : nullptr; }
    bool Settled() const { return Settled(before_) && Settled(after_); }

    const std::vector<PreviewMarker>& Markers() const { return markers_; }
    int MarkersOmitted() const { return markersOmitted_; }
    const PreviewAssetProbe& Assets() const { return assets_; }
    const RestoreRevision& Revision() const { return args_.revision; }
    // 両方の Scene の C# コンポーネントを読まなかった数 (合計)
    int ManagedSkipped() const { return before_.load.managedSkipped + after_.load.managedSkipped; }
    // 試験用: Settled になってからの経過 (印・カメラ・アセットの確認を始めた回数)
    int SettleCount() const { return settleCount_; }

private:
    static bool Settled(const Side& side) { return side.state == SideState::Loaded || side.state == SideState::Failed; }
    void StartSide(Side& side, bool isBefore, TrackClient& client);
    void AdvanceSide(Side& side);
    void OnSettled(TrackClient& client);
    void BuildMarkers();
    void StartAssetProbe(TrackClient& client);

    bool open_ = false;
    ScenePreviewOpenArgs args_;
    RestoreFileIo* io_ = nullptr;
    PreviewAfterSource afterSource_ = PreviewAfterSource::Revision;
    PreviewMode mode_ = PreviewMode::Overlay;
    PreviewCamera camera_;
    Side before_;
    Side after_;
    bool settledHandled_ = false;
    int settleCount_ = 0;
    std::vector<PreviewMarker> markers_;
    int markersOmitted_ = 0;
    PreviewAssetProbe assets_;
    bool assetsStarted_ = false;
};

} // namespace mye
