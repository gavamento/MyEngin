#include "Editor/Windows/SceneViewWindow.h"

#include <algorithm>
#include <cmath>

#include "Editor/AssetOps.h"
#include "Editor/CameraPilot.h"
#include "Editor/CreateMenu.h"
#include "Editor/EditorSettings.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Asset/TerrainAsset.h" // M58f: 地形ブラシ
#include "Engine/Engine/Asset/TerrainEdit.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/Audio/AudioSourceSystem.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Parts.h"
#include "Engine/Engine/Physics/ConvexHull.h" // M60f: shape=5 のワイヤ表示
#include "Engine/Engine/Physics/PhysicsDebugDraw.h"
#include "Engine/Engine/Physics/Shapes.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/RenderPath.h"

#include "fontawesome/IconsFontAwesome6.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
constexpr float kEditorFovDeg = 60.0f;
constexpr float kNearZ = 0.1f;
constexpr float kFarZ = 1000.0f;

// カメラプレビュー窓の RT。★視錐台ワイヤもこのアスペクトで描く — 「線の内側に写る」が
// 成り立たないとワイヤが嘘になるので、両者は必ず同じ 1 つの値から引く。
// CameraComponent は aspect を持たない (描画時にターゲット実寸で決まる) ため固定値
constexpr int kCamPreviewW = 320;
constexpr int kCamPreviewH = 180;
constexpr float kCamPreviewAspect = static_cast<float>(kCamPreviewW) / kCamPreviewH;

void CamBasis(float pitchDeg, float yawDeg, XMVECTOR& fwd, XMVECTOR& right, XMVECTOR& up)
{
    const XMVECTOR q = XMQuaternionRotationRollPitchYaw(pitchDeg * kDeg2Rad, yawDeg * kDeg2Rad, 0.0f);
    fwd = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
    right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);
    up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), q);
}

// レイがワールド AABB を**抜ける** t (スラブ法の tmax)。ミス (かすらない) は false。
// 部位ボリューム選択 (M49) の遮蔽近似に使う — GPU ピックは深度を返さないので、
// 「ピックしたメッシュの AABB を抜ける前にボリュームへ入るか」で手前/奥を判定する
bool RayAabbExit(const XMFLOAT3& o, const XMFLOAT3& d, const XMFLOAT3& lo, const XMFLOAT3& hi,
                 float& outExitT)
{
    float tmin = 0.0f, tmax = kFarZ;
    const float* op = &o.x;
    const float* dp = &d.x;
    const float* lp = &lo.x;
    const float* hp = &hi.x;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dp[i]) < 1e-8f) {
            if (op[i] < lp[i] || op[i] > hp[i]) {
                return false;
            }
            continue;
        }
        const float inv = 1.0f / dp[i];
        float t0 = (lp[i] - op[i]) * inv;
        float t1 = (hp[i] - op[i]) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmin > tmax) {
            return false;
        }
    }
    outExitT = tmax;
    return true;
}

// ---- 地形ブラシ (M58f) ----

// ブラシの対象になる地形 1 枚。
// ★選択中のエンティティを優先し、無ければ**最初に見つかった 1 枚**に落とす。
//   「選択しないと塗れない」は地形を選ぶ手段 (地形はアイコンを持たない = クリック選択も
//   ブラシモード中は止めている) が無いので詰む。逆に「常に最初の 1 枚」だと地形が
//   2 枚あるシーンで切り替えられない
struct TerrainTarget {
    EntityID entity = kNullEntity;
    XMFLOAT4X4 world = {};
    const char* source = nullptr;
};

bool FindTerrainTarget(EngineContext& ctx, const Selection& sel, TerrainTarget& out)
{
    if (ctx.scene == nullptr) {
        return false;
    }
    World& world = ctx.scene->GetWorld();
    EntityID preferred = kNullEntity;
    if (sel.primary != 0) {
        GameObject g = ctx.scene->FindByFileId(sel.primary);
        if (g) {
            preferred = g.Id();
        }
    }
    bool found = false;
    const ComponentTypeId req[] = { TerrainComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(TerrainComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* tc = static_cast<const TerrainComponent*>(arch.GetPtr(ti, row));
            if (tc->source[0] == '\0') {
                continue;
            }
            const EntityID e = arch.EntityAt(row);
            if (found && !(preferred == e)) {
                continue; // 既に候補がある — 選択中のものだけが上書きできる
            }
            out.entity = e;
            out.world = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            out.source = tc->source;
            found = true;
        }
    });
    return found;
}

// ワールド行列を「親を考慮したローカル TRS」へ落として書き戻す。
// ギズモのドラッグとカメラ操縦が共有する — 親付きのカメラを操縦したときに
// 親のワールド変換を掛け戻し忘れると、動かした瞬間に飛ぶ。
// 分解できない (退化した) 行列では何も書かない (NaN を撒かない)
void WriteWorldToLocal(World& world, EntityID e, LocalTransform& lt, const XMFLOAT4X4& worldM)
{
    XMMATRIX localMat = XMLoadFloat4x4(&worldM);
    const EntityID parent = world.GetParent(e);
    if (!parent.IsNull()) {
        if (auto* pwm = world.GetComponent<WorldMatrixComponent>(parent)) {
            const XMMATRIX pw = XMLoadFloat4x4(&pwm->value);
            localMat = XMMatrixMultiply(localMat, XMMatrixInverse(nullptr, pw));
        }
    }
    XMVECTOR s, r, t;
    if (XMMatrixDecompose(&s, &r, &t, localMat)) {
        XMStoreFloat3(&lt.position, t);
        XMStoreFloat4(&lt.rotation, r);
        XMStoreFloat3(&lt.scale, s);
    }
}

// ワールド行列から各軸のスケール量を取り出す
XMFLOAT3 MatrixScale(const XMFLOAT4X4& m)
{
    return { XMVectorGetX(XMVector3Length(XMVectorSet(m._11, m._12, m._13, 0))),
             XMVectorGetX(XMVector3Length(XMVectorSet(m._21, m._22, m._23, 0))),
             XMVectorGetX(XMVector3Length(XMVectorSet(m._31, m._32, m._33, 0))) };
}

} // namespace

void SceneViewWindow::OnRenderViews(EngineContext& ctx, Selection& selection)
{
    // 視錐台ワイヤ / プレビュー窓の対象を先に決める (BuildOverlays が読む)。
    // 死んだ操縦対象をここで畳むので、ワイヤ・プレビュー・バナー・入力の 4 者が
    // 常に同じ 1 台を指す
    camTargetFid_ = ResolveCameraFid(ctx, selection);
    previewValid_ = false; // 下の RenderCameraPreview が描けたときだけ立てる

    if (desiredW_ <= 0 || desiredH_ <= 0) {
        return;
    }
    rt_.Resize(*ctx.device, desiredW_, desiredH_);
    if (!rt_.IsValid()) {
        return;
    }

    const XMMATRIX camWorld =
        XMMatrixRotationRollPitchYaw(camPitch_ * kDeg2Rad, camYaw_ * kDeg2Rad, 0.0f)
        * XMMatrixTranslation(camPos_.x, camPos_.y, camPos_.z);

    CameraOverride cam;
    XMStoreFloat4x4(&cam.view, XMMatrixInverse(nullptr, camWorld));
    cam.position = camPos_;
    cam.fovYDeg = kEditorFovDeg;
    cam.nearZ = kNearZ; // 下の lastProj_ と同じ定数から引く (既定値への暗黙依存を切る)
    cam.farZ = kFarZ;
    cam.debugViewMode = viewMode_; // M40b: Lit/Unlit/Wireframe (SceneView のみ)

    // ギズモがレンダ画像とピクセル一致するよう、描画と同じ view/proj を保存する
    lastView_ = cam.view;
    const float aspect =
        (rt_.Height() > 0) ? static_cast<float>(rt_.Width()) / static_cast<float>(rt_.Height()) : 1.0f;
    const XMMATRIX proj = orthographic_
        ? XMMatrixOrthographicLH(static_cast<float>(rt_.Width()) * 0.02f,
                                 static_cast<float>(rt_.Height()) * 0.02f, kNearZ, kFarZ)
        : XMMatrixPerspectiveFovLH(XMConvertToRadians(kEditorFovDeg), aspect, kNearZ, kFarZ);
    XMStoreFloat4x4(&lastProj_, proj);
    // M55b: 射影の組み立てはここ 1 箇所に集約し、描画側 (RenderSystem) には組み直させない。
    // これまで RenderSystem は fovYDeg から透視を組み直していたので、Ortho トグルが
    // オーバーレイ/ギズモ/ピッキングにしか効かず絵は常に透視のまま = 3 者が食い違っていた。
    // ジッタ (M55b) はこの行列を **元** に RenderSystem 側で載せる — lastProj_ は
    // 非ジッタのまま = ギズモとピッキングは揺れない
    cam.hasProj = true;
    cam.proj = lastProj_;

    FrameTarget target;
    target.rtv = rt_.RTV();
    target.dsv = rt_.DSV();
    target.width = rt_.Width();
    target.height = rt_.Height();
    target.depthSRV = rt_.DepthSRV();       // M42a
    target.dsvReadOnly = rt_.DSVReadOnly();
    target.viewKey = 2; // SceneView
    ctx.renderSystem->Render(ctx.scene->GetWorld(), *ctx.device, *ctx.renderPath, *ctx.shaders,
                             *ctx.resources, target, &cam, ctx.particles, ctx.vfx);

    // エディタ補助線 (グリッド/ワイヤ/アウトライン) を SceneView RT の上に重ねる。
    // backbuffer/リプレイ経路には出さない (sim 非影響)
    if (!lines_.IsReady()) {
        lines_.Init(*ctx.device, *ctx.shaders);
    }
    lines_.Begin();
    BuildOverlays(ctx, selection);
    lines_.Render(*ctx.device, *ctx.shaders, rt_.RTV(), rt_.DSV(), rt_.Width(), rt_.Height(),
                  lastView_, lastProj_);

    // カメラプレビュー窓 (SceneView の絵とは独立した 2 枚目の RT)。
    // ★SceneView の描画と補助線を**全部済ませてから**描く — RTV を握り替えるので
    //   途中に挟むと補助線がプレビュー面へ流れ込む (M56e のプローブ焼きと同じ罠)
    RenderCameraPreview(ctx);
}

// 操縦対象のエンティティ。対象が消えていたら操縦自体を畳んで kNullEntity を返す
EntityID SceneViewWindow::PilotTarget(EngineContext& ctx)
{
    CameraPilotState& pilot = GetCameraPilot();
    if (!pilot.Active() || ctx.scene == nullptr) {
        return kNullEntity;
    }
    World& world = ctx.scene->GetWorld();
    GameObject obj = ctx.scene->FindByFileId(pilot.fileId);
    // ★fileId はシーンを跨いで使い回されるので「生きている」だけでは足りない。
    //   Camera を持っているかまで見て初めて、シーンを読み直したあとに無関係な
    //   オブジェクトを操縦し始める事故が消える
    if (obj && world.GetComponent<CameraComponent>(obj.Id()) != nullptr
        && world.GetComponent<WorldMatrixComponent>(obj.Id()) != nullptr
        && world.GetComponent<LocalTransform>(obj.Id()) != nullptr) {
        return obj.Id();
    }
    pilot.Stop();
    return kNullEntity;
}

uint64_t SceneViewWindow::ResolveCameraFid(EngineContext& ctx, const Selection& selection)
{
    previewLabel_.clear();
    if (ctx.scene == nullptr) {
        return 0;
    }
    World& world = ctx.scene->GetWorld();
    uint64_t fid = 0;
    if (!PilotTarget(ctx).IsNull()) {
        fid = GetCameraPilot().fileId;
    } else if (selection.primary != 0) {
        GameObject obj = ctx.scene->FindByFileId(selection.primary);
        if (obj && world.GetComponent<CameraComponent>(obj.Id()) != nullptr
            && world.GetComponent<WorldMatrixComponent>(obj.Id()) != nullptr) {
            fid = selection.primary;
        }
    }
    if (fid != 0) {
        if (GameObject obj = ctx.scene->FindByFileId(fid)) {
            previewLabel_ = world.GetName(obj.Id());
        }
    }
    return fid;
}

void SceneViewWindow::AddFrustumWire(const XMFLOAT4X4& world, const CameraComponent& cam)
{
    constexpr uint32_t kFrustum = 0x60E0FFFFu;
    constexpr uint32_t kFrustumCut = 0x2E5F78FFu; // 打ち切った far 面 (暗くする)

    // 表示上の far。既定 farZ=1000 を素直に描くと視錐台が画面を埋めて何も見えないので
    // ツールバーのスライダで打ち切る。★実 farZ の方が小さければそちらが勝つ
    // (打ち切りは「見やすさのための嘘」なので、本物より遠くを主張してはいけない)
    const float shownFar = std::max(cam.nearZ + 0.01f, std::min(cam.farZ, frustumFar_));
    const bool cut = shownFar < cam.farZ;

    XMFLOAT3 c[8];
    ComputeFrustumCorners(world, cam.fovYDeg, kCamPreviewAspect, cam.nearZ, shownFar, c);
    const uint32_t farColor = cut ? kFrustumCut : kFrustum;
    for (int i = 0; i < 4; ++i) {
        const int n = (i + 1) & 3; // 隣の隅 (面内の順が 左下→右下→右上→左上 なので辺になる)
        lines_.AddLine(c[i], c[n], kFrustum);         // near 面
        lines_.AddLine(c[4 + i], c[4 + n], farColor); // far 面
        lines_.AddLine(c[i], c[4 + i], farColor);     // 側面の稜線
    }
    // 頂点 (カメラ位置) から near 四隅へ。「どこから見ているか」が一目で分かる
    const XMFLOAT3 apex = { world._41, world._42, world._43 };
    for (int i = 0; i < 4; ++i) {
        lines_.AddLine(apex, c[i], kFrustum);
    }
    // 上方向マーカー (near 面の上辺に載せる三角)。ロールが付いていると一緒に傾くので、
    // 「このカメラは傾いている」が絵で分かる = 操縦モードがロールを保つ理由が見える
    const XMVECTOR topMid =
        XMVectorScale(XMVectorAdd(XMLoadFloat3(&c[2]), XMLoadFloat3(&c[3])), 0.5f);
    const XMVECTOR bottomMid =
        XMVectorScale(XMVectorAdd(XMLoadFloat3(&c[0]), XMLoadFloat3(&c[1])), 0.5f);
    XMFLOAT3 tip;
    XMStoreFloat3(&tip, XMVectorAdd(topMid, XMVectorScale(XMVectorSubtract(topMid, bottomMid),
                                                          0.35f)));
    lines_.AddLine(c[2], tip, kFrustum);
    lines_.AddLine(c[3], tip, kFrustum);
}

void SceneViewWindow::RenderCameraPreview(EngineContext& ctx)
{
    if (camTargetFid_ == 0 || ctx.scene == nullptr) {
        return;
    }
    World& world = ctx.scene->GetWorld();
    GameObject obj = ctx.scene->FindByFileId(camTargetFid_);
    if (!obj) {
        return;
    }
    const auto* cam = world.GetComponent<CameraComponent>(obj.Id());
    const auto* wmc = world.GetComponent<WorldMatrixComponent>(obj.Id());
    if (cam == nullptr || wmc == nullptr) {
        return;
    }
    previewRt_.Resize(*ctx.device, kCamPreviewW, kCamPreviewH);
    if (!previewRt_.IsValid()) {
        return;
    }

    // view = inverse(world) — RenderSystem がシーンカメラに使う式そのまま。
    // 射影は渡さない (hasProj=false) ので RenderSystem が fov/near/far と
    // **この RT の実寸**から組む = プレビューのアスペクトは常に kCamPreviewAspect
    CameraOverride pv;
    XMStoreFloat4x4(&pv.view, XMMatrixInverse(nullptr, XMLoadFloat4x4(&wmc->value)));
    pv.position = { wmc->value._41, wmc->value._42, wmc->value._43 };
    pv.fovYDeg = cam->fovYDeg;
    pv.nearZ = cam->nearZ;
    pv.farZ = cam->farZ;

    FrameTarget target;
    target.rtv = previewRt_.RTV();
    target.dsv = previewRt_.DSV();
    target.width = previewRt_.Width();
    target.height = previewRt_.Height();
    target.depthSRV = previewRt_.DepthSRV();
    target.dsvReadOnly = previewRt_.DSVReadOnly();
    // ★viewKey = 0 (= 履歴を持たないビュー)。1..3 を借りると SceneView / GameView の
    //   TAA 履歴・前フレーム world 行列・前フレーム viewProj を上書きして、本編の絵が
    //   毎フレーム壊れる。ProbeBaker が 0 を選んでいるのと同じ理由
    target.viewKey = 0;
    ctx.renderSystem->Render(world, *ctx.device, *ctx.renderPath, *ctx.shaders, *ctx.resources,
                             target, &pv, ctx.particles, ctx.vfx);
    previewValid_ = true;
}

void SceneViewWindow::BuildOverlays(EngineContext& ctx, Selection& selection)
{
    World& world = ctx.scene->GetWorld();

    if (showGrid_) {
        lines_.AddGrid(20, 1.0f, 0x5A5A64FFu, 0xC04848FFu, 0x4868C0FFu);
    }

    if (showGizmos_) {
        constexpr uint32_t kCollider = 0x40D040FFu;
        constexpr uint32_t kLight = 0xF0E040FFu;
        constexpr uint32_t kCamera = 0x40C0F0FFu;
        constexpr uint32_t kEmitter = 0xF08020FFu;

        // コライダー (球 / OBB / カプセル、M28a)。寸法・基底は物理と同じ
        // shapes::MakePoseFromMatrix から取る = ギズモと判定のズレを構造的に防ぐ
        const ComponentTypeId colReq[] = { ColliderComponent::sTypeId,
                                           WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(colReq, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(ColliderComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* col = static_cast<const ColliderComponent*>(arch.GetPtr(ci, row));
                const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const ShapePose pose = shapes::MakePoseFromMatrix(*col, wm);
                const XMFLOAT3 pos = { pose.px, pose.py, pose.pz };
                if (col->shape == 0) {
                    lines_.AddWireSphere(pos, pose.radius, kCollider);
                } else if (col->shape == 2) {
                    lines_.AddWireCapsule(pos, { pose.bx[0], pose.bx[1], pose.bx[2] },
                                          { pose.by[0], pose.by[1], pose.by[2] },
                                          { pose.bz[0], pose.bz[1], pose.bz[2] }, pose.radius,
                                          pose.halfSeg, kCollider);
                } else if (col->shape == 5) {
                    // M60f: 凸包は**実際の稜線**を描く。箱で代用すると「どこまでが当たり
                    // 判定なのか」が分からず、凸包コライダーのデバッグが成立しない。
                    // 実体は MakePoseFromMatrix が convexcol 経由で解決済み (未生成は null)
                    const auto* hull = static_cast<const ConvexHullData*>(pose.meshData);
                    if (hull != nullptr) {
                        auto toWorld = [&](const XMFLOAT3& v) {
                            const float lx = v.x * pose.sx, ly = v.y * pose.sy, lz = v.z * pose.sz;
                            return XMFLOAT3{
                                pose.px + pose.bx[0] * lx + pose.by[0] * ly + pose.bz[0] * lz,
                                pose.py + pose.bx[1] * lx + pose.by[1] * ly + pose.bz[1] * lz,
                                pose.pz + pose.bx[2] * lx + pose.by[2] * ly + pose.bz[2] * lz
                            };
                        };
                        for (const ConvexEdge& e : hull->edges) {
                            lines_.AddLine(toWorld(hull->verts[static_cast<size_t>(e.v0)]),
                                           toWorld(hull->verts[static_cast<size_t>(e.v1)]),
                                           kCollider);
                        }
                    }
                } else {
                    // OBB: 基底 × スケール適用済み half extents を行列に組んで描画
                    XMFLOAT4X4 boxWorld = {
                        pose.bx[0], pose.bx[1], pose.bx[2], 0,
                        pose.by[0], pose.by[1], pose.by[2], 0,
                        pose.bz[0], pose.bz[1], pose.bz[2], 0,
                        pose.px,    pose.py,    pose.pz,    1,
                    };
                    lines_.AddWireBox(boxWorld, { pose.hx, pose.hy, pose.hz }, kCollider);
                }
            }
        });

        // ライト (位置マーカー + 前方向)
        const ComponentTypeId liReq[] = { LightComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(liReq, [&](Archetype& arch) {
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT3 pos = { wm._41, wm._42, wm._43 };
                lines_.AddWireSphere(pos, 0.3f, kLight);
                const XMVECTOR fwd = XMVector3Normalize(XMVectorSet(wm._31, wm._32, wm._33, 0));
                XMFLOAT3 tip;
                XMStoreFloat3(&tip, XMVectorAdd(XMLoadFloat3(&pos), XMVectorScale(fwd, 2.0f)));
                lines_.AddLine(pos, tip, kLight);
            }
        });

        // カメラ (ボックス glyph)
        const ComponentTypeId caReq[] = { CameraComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(caReq, [&](Archetype& arch) {
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                lines_.AddWireBox(wm, { 0.3f, 0.3f, 0.45f }, kCamera);
            }
        });

        // 視錐台ワイヤは**選択中 (= 操縦中) の 1 台だけ**。全カメラに出すと、カメラが
        // 数台あるだけで画面が線だらけになって何も読めなくなる
        if (camTargetFid_ != 0) {
            if (GameObject obj = ctx.scene->FindByFileId(camTargetFid_)) {
                const auto* cam = world.GetComponent<CameraComponent>(obj.Id());
                const auto* wmc = world.GetComponent<WorldMatrixComponent>(obj.Id());
                if (cam != nullptr && wmc != nullptr) {
                    AddFrustumWire(wmc->value, *cam);
                }
            }
        }

        // パーティクルエミッタ (クロス glyph)
        const ComponentTypeId emReq[] = { ParticleEmitterComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(emReq, [&](Archetype& arch) {
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT3 p = { wm._41, wm._42, wm._43 };
                const float r = 0.4f;
                lines_.AddLine({ p.x - r, p.y, p.z }, { p.x + r, p.y, p.z }, kEmitter);
                lines_.AddLine({ p.x, p.y - r, p.z }, { p.x, p.y + r, p.z }, kEmitter);
                lines_.AddLine({ p.x, p.y, p.z - r }, { p.x, p.y, p.z + r }, kEmitter);
            }
        });

        // ばねジョイント (owner↔connected を結ぶ線 + 両端マーカー、M29a)
        constexpr uint32_t kSpring = 0xE060E0FFu;
        const ComponentTypeId sjReq[] = { SpringJointComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(sjReq, [&](Archetype& arch) {
            const int si = arch.FindTypeIndex(SpringJointComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* sj = static_cast<const SpringJointComponent*>(arch.GetPtr(si, row));
                if (sj->connectedEntity.IsNull() || !world.IsAlive(sj->connectedEntity)) {
                    continue;
                }
                const auto* owm = world.GetComponent<WorldMatrixComponent>(sj->connectedEntity);
                if (!owm) {
                    continue;
                }
                const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT3 a = { wm._41, wm._42, wm._43 };
                const XMFLOAT3 b = { owm->value._41, owm->value._42, owm->value._43 };
                lines_.AddLine(a, b, kSpring);
                lines_.AddWireSphere(a, 0.08f, kSpring);
                lines_.AddWireSphere(b, 0.08f, kSpring);
            }
        });

        // 関節 (M60a)。アンカー 2 点 (owner=緑 / 相手=黄) と、それを結ぶ「ずれ」の線。
        // ★**編集中に見えること**がこのギズモの存在理由 — 走らせる前にアンカーの位置が
        //   意図どおりかを確かめられないと関節の authoring は成立しない。Play 中の
        //   ライブ表示は PhysicsDebugFlags.joints (両ビューに出る) が別に持っている
        constexpr uint32_t kJointA = 0x40FF90FFu;
        constexpr uint32_t kJointB = 0xFFD040FFu;
        constexpr uint32_t kJointErr = 0xFF3030FFu;
        const ComponentTypeId jtReq[] = { JointComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(jtReq, [&](Archetype& arch) {
            const int ji = arch.FindTypeIndex(JointComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* jc = static_cast<const JointComponent*>(arch.GetPtr(ji, row));
                const XMFLOAT4X4& wm =
                    static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                auto xform = [](const XMFLOAT4X4& m, const XMFLOAT3& v) {
                    return XMFLOAT3{ v.x * m._11 + v.y * m._21 + v.z * m._31 + m._41,
                                     v.x * m._12 + v.y * m._22 + v.z * m._32 + m._42,
                                     v.x * m._13 + v.y * m._23 + v.z * m._33 + m._43 };
                };
                const XMFLOAT3 a = xform(wm, jc->anchor);
                XMFLOAT3 b;
                if (jc->connectedEntity.IsNull()) {
                    b = jc->connectedAnchor; // 相手が居ないときだけワールド座標 (ソルバと同規約)
                } else {
                    const auto* owm =
                        world.GetComponent<WorldMatrixComponent>(jc->connectedEntity);
                    if (!owm) {
                        continue;
                    }
                    b = xform(owm->value, jc->connectedAnchor);
                }
                lines_.AddWireSphere(a, 0.10f, kJointA);
                lines_.AddWireSphere(b, 0.10f, kJointB);
                lines_.AddLine(a, b, kJointErr);
            }
        });

        // 定常力 (力方向の矢印、M29a)。長さは正規化 + 固定 (大きさは Inspector で読む)
        constexpr uint32_t kForce = 0xF0A040FFu;
        const ComponentTypeId cfReq[] = { ConstantForceComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(cfReq, [&](Archetype& arch) {
            const int fi = arch.FindTypeIndex(ConstantForceComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* cf = static_cast<const ConstantForceComponent*>(arch.GetPtr(fi, row));
                const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                XMVECTOR dir = XMVectorSet(cf->force.x, cf->force.y, cf->force.z, 0);
                if (cf->relative != 0) {
                    // ローカル指定はワールド行列の回転成分で向きを変換 (表示のみ)
                    XMFLOAT4X4 rot = wm;
                    rot._41 = rot._42 = rot._43 = 0;
                    dir = XMVector3TransformNormal(dir, XMLoadFloat4x4(&rot));
                }
                if (XMVectorGetX(XMVector3LengthSq(dir)) < 1e-8f) {
                    continue;
                }
                dir = XMVector3Normalize(dir);
                const XMFLOAT3 p = { wm._41, wm._42, wm._43 };
                XMFLOAT3 tip;
                XMStoreFloat3(&tip, XMVectorAdd(XMLoadFloat3(&p), XMVectorScale(dir, 1.2f)));
                lines_.AddLine(p, tip, kForce);
                // 矢先 (tip から根本方向へ小さな八の字)
                XMVECTOR back = XMVectorScale(dir, -0.25f);
                XMVECTOR up = XMVectorSet(0, 1, 0, 0);
                XMVECTOR side = XMVector3Cross(dir, up);
                if (XMVectorGetX(XMVector3LengthSq(side)) < 1e-6f) {
                    side = XMVectorSet(1, 0, 0, 0);
                } else {
                    side = XMVector3Normalize(side);
                }
                XMFLOAT3 w1, w2;
                XMStoreFloat3(&w1, XMVectorAdd(XMLoadFloat3(&tip),
                                               XMVectorAdd(back, XMVectorScale(side, 0.12f))));
                XMStoreFloat3(&w2, XMVectorAdd(XMLoadFloat3(&tip),
                                               XMVectorSubtract(back, XMVectorScale(side, 0.12f))));
                lines_.AddLine(tip, w1, kForce);
                lines_.AddLine(tip, w2, kForce);
            }
        });

        // キャラクターコントローラ (カプセルワイヤ、M29b)。寸法規約は物理とミラー
        // (radius×max(sx,sz)、halfSeg = height/2×sy − radius、常にワールド Y 軸)
        constexpr uint32_t kCharCtrl = 0x30E0B0FFu;
        const ComponentTypeId chReq[] = { CharacterControllerComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(chReq, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(CharacterControllerComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* cc =
                    static_cast<const CharacterControllerComponent*>(arch.GetPtr(ci, row));
                const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT3 sc = MatrixScale(wm);
                const float wr = cc->radius * std::max(std::fabs(sc.x), std::fabs(sc.z));
                const float wh = cc->height * 0.5f * std::fabs(sc.y);
                const float halfSeg = (wh > wr) ? (wh - wr) : 0.0f;
                lines_.AddWireCapsule({ wm._41, wm._42, wm._43 }, { 1, 0, 0 }, { 0, 1, 0 },
                                      { 0, 0, 1 }, wr, halfSeg, kCharCtrl);
            }
        });

        // スプライト (サイズ枠、M29c)。ビルボードは常時回るのでワイヤは XY 平面固定のヒント表示
        constexpr uint32_t kVfx = 0xC080F0FFu;
        const ComponentTypeId spReq[] = { SpriteRendererComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(spReq, [&](Archetype& arch) {
            const int si = arch.FindTypeIndex(SpriteRendererComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* sp = static_cast<const SpriteRendererComponent*>(arch.GetPtr(si, row));
                const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT3 p = { wm._41, wm._42, wm._43 };
                const float hx = sp->size.x * 0.5f;
                const float hy = sp->size.y * 0.5f;
                lines_.AddLine({ p.x - hx, p.y - hy, p.z }, { p.x + hx, p.y - hy, p.z }, kVfx);
                lines_.AddLine({ p.x + hx, p.y - hy, p.z }, { p.x + hx, p.y + hy, p.z }, kVfx);
                lines_.AddLine({ p.x + hx, p.y + hy, p.z }, { p.x - hx, p.y + hy, p.z }, kVfx);
                lines_.AddLine({ p.x - hx, p.y + hy, p.z }, { p.x - hx, p.y - hy, p.z }, kVfx);
            }
        });

        // オーディオ (M45e)。音源はマーカー球、リスナーは向きが要るので前方向線も引く。
        // **減衰球 (min/max) は選択中の 1 個だけ** — 全音源に描くとシーンが球だらけになる
        constexpr uint32_t kAudio = 0x40E0C0FFu;    // 音源マーカー / リスナー
        constexpr uint32_t kAudioMin = 0x40FFA0FFu; // minDistance (ここまでは減衰しない)
        constexpr uint32_t kAudioMax = 0x2080A0FFu; // maxDistance (ここから先は無音)
        const GameObject selForAudio = ctx.scene->FindByFileId(selection.primary);
        const EntityID selAudioEntity = selForAudio ? selForAudio.Id() : kNullEntity;

        const ComponentTypeId auReq[] = { AudioSourceComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(auReq, [&](Archetype& arch) {
            const int ai = arch.FindTypeIndex(AudioSourceComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* src = static_cast<const AudioSourceComponent*>(arch.GetPtr(ai, row));
                const XMFLOAT4X4& wm =
                    static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT3 p = { wm._41, wm._42, wm._43 };
                lines_.AddWireSphere(p, 0.18f, kAudio);
                if (!(arch.EntityAt(row) == selAudioEntity)) {
                    continue;
                }
                // ★実効値は再生時とまったく同じ 1 本の規則 (MakeSourcePlay) から取る。
                //   ここで overrideAttenuation の分岐を書き直すと、ギズモと実際の鳴り方が
                //   静かにズレる (コライダーのギズモを shapes:: 経由にしてあるのと同じ理由)
                const SoundAsset* asset =
                    ctx.sounds != nullptr ? ctx.sounds->Get(src->sound.value) : nullptr;
                if (asset == nullptr || ctx.audio == nullptr) {
                    continue;
                }
                PlayDesc desc;
                AudioSpatial sp;
                MakeSourcePlay(*asset, *src, *ctx.audio, -1, 0.0f, 0.0f, desc, sp);
                if (sp.spatialBlend <= 0.0f) {
                    continue; // 2D 音源に距離球を描くと嘘になる
                }
                lines_.AddWireSphere(p, sp.minDistance, kAudioMin);
                lines_.AddWireSphere(p, sp.maxDistance, kAudioMax);
            }
        });

        const ComponentTypeId alReq[] = { AudioListenerComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(alReq, [&](Archetype& arch) {
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const XMFLOAT4X4& wm =
                    static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT3 pos = { wm._41, wm._42, wm._43 };
                lines_.AddWireSphere(pos, 0.3f, kAudio);
                const XMVECTOR fwd = XMVector3Normalize(XMVectorSet(wm._31, wm._32, wm._33, 0));
                XMFLOAT3 tip;
                XMStoreFloat3(&tip, XMVectorAdd(XMLoadFloat3(&pos), XMVectorScale(fwd, 1.2f)));
                lines_.AddLine(pos, tip, kAudio);
            }
        });

        // 3D テキスト (T 字 glyph、M29c)
        const ComponentTypeId txReq[] = { TextMeshComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(txReq, [&](Archetype& arch) {
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const XMFLOAT4X4& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT3 p = { wm._41, wm._42, wm._43 };
                const float r = 0.25f;
                lines_.AddLine({ p.x - r, p.y + r, p.z }, { p.x + r, p.y + r, p.z }, kVfx);
                lines_.AddLine({ p.x, p.y + r, p.z }, { p.x, p.y - r, p.z }, kVfx);
            }
        });

        // 部位ソケット (八面体 glyph + 前方ティック、M48i)。
        // ボーン追従 (joint 指定あり) と静的ソケットで色を分ける — 実行時に動くかどうかが
        // 一目で分かることが、この glyph の主目的
        constexpr uint32_t kPartBone = 0xF060C0FFu;   // 追従あり (マゼンタ)
        constexpr uint32_t kPartStatic = 0x8080A0FFu; // 静的ソケット (くすんだ青灰)
        const ComponentTypeId ptReq[] = { PartComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(ptReq, [&](Archetype& arch) {
            const int pi = arch.FindTypeIndex(PartComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* pc = static_cast<const PartComponent*>(arch.GetPtr(pi, row));
                const XMFLOAT4X4& wm =
                    static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT3 p = { wm._41, wm._42, wm._43 };
                const uint32_t c = (pc->joint[0] != '\0') ? kPartBone : kPartStatic;
                // 八面体のワイヤ (= 3 平面の菱形)。ワールド軸に揃えるので向きに関係なく読める
                const float r = 0.09f;
                const XMFLOAT3 px = { p.x + r, p.y, p.z }, nx = { p.x - r, p.y, p.z };
                const XMFLOAT3 py = { p.x, p.y + r, p.z }, ny = { p.x, p.y - r, p.z };
                const XMFLOAT3 pz = { p.x, p.y, p.z + r }, nz = { p.x, p.y, p.z - r };
                const XMFLOAT3* ring[3][4] = { { &px, &py, &nx, &ny },
                                               { &py, &pz, &ny, &nz },
                                               { &pz, &px, &nz, &nx } };
                for (const XMFLOAT3** rg : ring) {
                    for (int i = 0; i < 4; ++i) {
                        lines_.AddLine(*rg[i], *rg[(i + 1) & 3], c);
                    }
                }
                // 取り付け向き (+Z) を短いティックで示す — ソケットは向きが本体なので
                const XMVECTOR fwd = XMVector3Normalize(XMVectorSet(wm._31, wm._32, wm._33, 0));
                XMFLOAT3 tip;
                XMStoreFloat3(&tip, XMVectorAdd(XMLoadFloat3(&p), XMVectorScale(fwd, 0.22f)));
                lines_.AddLine(p, tip, c);
            }
        });

        // 部位の範囲 (M49): 箱/球ボリュームのワイヤ。ポーズはクリック選択・RaycastParts と
        // 同じ Parts::MakePartBoundsPose から取る = 表示と判定のズレを構造的に防ぐ
        const ComponentTypeId pbReq[] = { PartComponent::sTypeId, PartBoundsComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(pbReq, [&](Archetype& arch) {
            const int pi = arch.FindTypeIndex(PartComponent::sTypeId);
            const int bi = arch.FindTypeIndex(PartBoundsComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* pc = static_cast<const PartComponent*>(arch.GetPtr(pi, row));
                const auto* pb = static_cast<const PartBoundsComponent*>(arch.GetPtr(bi, row));
                const XMFLOAT4X4& wm =
                    static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const uint32_t c = (pc->joint[0] != '\0') ? kPartBone : kPartStatic;
                const ShapePose pose = Parts::MakePartBoundsPose(*pb, wm);
                const XMFLOAT3 pos = { pose.px, pose.py, pose.pz };
                if (pose.shape == 0) {
                    lines_.AddWireSphere(pos, pose.radius, c);
                } else {
                    XMFLOAT4X4 boxWorld = {
                        pose.bx[0], pose.bx[1], pose.bx[2], 0,
                        pose.by[0], pose.by[1], pose.by[2], 0,
                        pose.bz[0], pose.bz[1], pose.bz[2], 0,
                        pose.px,    pose.py,    pose.pz,    1,
                    };
                    lines_.AddWireBox(boxWorld, { pose.hx, pose.hy, pose.hz }, c);
                }
            }
        });

        // 反射プローブの影響ボックス (M56f)。**箱は軸平行**なのでワールド行列は平行移動だけ
        // (エンティティの回転もスケールも見ない = ここで拾わないのが仕様どおり)。
        // ★これが無いとプローブは画面に一切現れない — メッシュを持たず、焼くまでは絵にも
        //   寄与しないので、置いた箱の大きさを確かめる手段が他に無い
        constexpr uint32_t kProbeBox = 0x60C0FFFFu; // 水色
        const ComponentTypeId rpReq[] = { ReflectionProbeComponent::sTypeId,
                                          WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(rpReq, [&](Archetype& arch) {
            const int ri = arch.FindTypeIndex(ReflectionProbeComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* rp = static_cast<const ReflectionProbeComponent*>(arch.GetPtr(ri, row));
                const XMFLOAT4X4& wm =
                    static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                const XMFLOAT4X4 boxWorld = {
                    1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, wm._41, wm._42, wm._43, 1,
                };
                lines_.AddWireBox(boxWorld,
                                  { std::fabs(rp->extents.x), std::fabs(rp->extents.y),
                                    std::fabs(rp->extents.z) },
                                  kProbeBox);
                lines_.AddWireSphere({ wm._41, wm._42, wm._43 }, 0.2f, kProbeBox); // 撮影点
            }
        });
    }

    // 選択アウトライン (常時最前面)
    GameObject sel = ctx.scene->FindByFileId(selection.primary);
    if (sel) {
        auto* wm = world.GetComponent<WorldMatrixComponent>(sel.Id());
        if (wm) {
            XMFLOAT3 lo = { -0.5f, -0.5f, -0.5f };
            XMFLOAT3 hi = { 0.5f, 0.5f, 0.5f };
            if (auto* mr = world.GetComponent<MeshRendererComponent>(sel.Id())) {
                if (Mesh* mesh = ctx.resources->meshes.Get(mr->mesh)) {
                    lo = mesh->aabbMin;
                    hi = mesh->aabbMax;
                }
            }
            const XMFLOAT3 center = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                                     (lo.z + hi.z) * 0.5f };
            const XMFLOAT3 half = { (hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f,
                                    (hi.z - lo.z) * 0.5f };
            const XMMATRIX boxWorld =
                XMMatrixTranslation(center.x, center.y, center.z) * XMLoadFloat4x4(&wm->value);
            XMFLOAT4X4 bw;
            XMStoreFloat4x4(&bw, boxWorld);
            lines_.AddWireBox(bw, half, 0xFFA030FFu, /*onTop*/ true);
        }
    }
}

void SceneViewWindow::DrawToolbar(EditorSettings& settings)
{
    // ビューポート左上のオーバーレイツールバー (ギズモ操作 / 座標系 / 投影 / カメラ速度)
    const ImVec2 p = ImGui::GetItemRectMin();
    ImGui::SetCursorScreenPos(ImVec2(p.x + 8.0f, p.y + 8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.13f, 0.85f));
    // M47b: 幅は中身から自動決定する。訳文が長いと 830px 固定ではボタンが見切れるため
    ImGui::BeginChild("##sv_toolbar", ImVec2(0.0f, 30.0f), ImGuiChildFlags_AutoResizeX,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    auto opBtn = [&](const char* label, ImGuizmo::OPERATION op) {
        const bool on = (gizmoOp_ == op);
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.45f, 0.78f, 1.0f));
        }
        if (ImGui::Button(label)) {
            gizmoOp_ = op;
        }
        if (on) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
    };
    opBtn(Tr(StrId::SceneView_Move), ImGuizmo::TRANSLATE);
    opBtn(Tr(StrId::SceneView_Rotate), ImGuizmo::ROTATE);
    opBtn(Tr(StrId::SceneView_Scale), ImGuizmo::SCALE);
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    if (ImGui::Button(Tr(gizmoMode_ == ImGuizmo::LOCAL ? StrId::Tool_SpaceLocal : StrId::Tool_SpaceWorld))) {
        gizmoMode_ = (gizmoMode_ == ImGuizmo::LOCAL) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
    }
    ImGui::SameLine();
    ImGui::Checkbox(Tr(StrId::SceneView_Ortho), &orthographic_);
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    ImGui::Checkbox(Tr(StrId::SceneView_Grid), &showGrid_);
    ImGui::SameLine();
    ImGui::Checkbox(Tr(StrId::SceneView_Gizmos), &showGizmos_);
    ImGui::SameLine();
    // 物理デバッグ可視化 (M59e)。**Play 中しか線は出ない** — 積むのは tick 側なので
    // (編集中は物理が走らず接触も速度も無い)。有効中はボタンを着色して気付けるようにする
    {
        PhysicsDebugFlags& pd = GetPhysicsDebugFlags();
        const bool anyOn = pd.contacts || pd.velocities || pd.joints;
        if (anyOn) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.45f, 0.78f, 1.0f));
        }
        if (ImGui::Button(Tr(StrId::SceneView_PhysDebug))) {
            ImGui::OpenPopup("##sv_physdbg_popup");
        }
        if (anyOn) {
            ImGui::PopStyleColor();
        }
        if (ImGui::BeginPopup("##sv_physdbg_popup")) {
            ImGui::Checkbox(Tr(StrId::SceneView_PhysContact), &pd.contacts);
            ImGui::Checkbox(Tr(StrId::SceneView_PhysImpulse), &pd.impulses);
            ImGui::Checkbox(Tr(StrId::SceneView_PhysVel), &pd.velocities);
            ImGui::Checkbox(Tr(StrId::SceneView_PhysJoint), &pd.joints); // M60a
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    // 表示モード (M40b): Lit / Unlit / Wireframe。GameView は常に Lit
    auto modeBtn = [&](const char* label, int mode) {
        const bool on = (viewMode_ == mode);
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.45f, 0.78f, 1.0f));
        }
        if (ImGui::Button(label)) {
            viewMode_ = mode;
        }
        if (on) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
    };
    modeBtn(Tr(StrId::SceneView_Lit), 0);
    modeBtn(Tr(StrId::SceneView_Unlit), 1);
    modeBtn(Tr(StrId::SceneView_Wire), 2);
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    // カメラ速度 (M27d)。RMB ホールド中のホイールでも変わる (HandleCamera)
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::SliderFloat("##camspeed", &settings.camMoveSpeed, 0.5f, 60.0f, Tr(StrId::SceneView_CamSpeed),
                           ImGuiSliderFlags_Logarithmic)) {
        camSpeedDirty_ = true; // 永続化は操作終了時 (HandleCamera 側の Save に相乗り)
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        camSpeedDirty_ = false;
        settings.Save();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    // 視錐台ワイヤの表示上の打ち切り距離。既定 farZ=1000 のカメラを素直に描くと
    // 視錐台が画面を埋めるので、ここで「どこまで描くか」を手元で決められるようにする。
    // ★シーンにも設定ファイルにも保存しない — 地形ブラシと同じで「いまの見え方」であって
    //   カメラの属性ではない (保存すると別プロジェクトへ持ち出したときに意味が変わる)
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("##frustumfar", &frustumFar_, 2.0f, 500.0f,
                       Tr(StrId::SceneView_FrustumFar), ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    // 地形ブラシ (M58f)。on の間はピッキングもギズモも止まる = 「別のツール」であることを
    // 押しっぱなしの色で見せる
    if (terrainBrush_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.45f, 0.20f, 1.0f));
    }
    if (ImGui::Button(Tr(StrId::Terrain_Brush))) {
        terrainBrush_ = !terrainBrush_;
        terrainStroking_ = false;
    }
    if (terrainBrush_) {
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void SceneViewWindow::DrawPilotBanner()
{
    CameraPilotState& pilot = GetCameraPilot();
    if (!pilot.Active()) {
        return;
    }
    // ツールバー (直前に描いた子ウィンドウ) の真下に貼る。★「今どのカメラを動かして
    // いるのか」と「どうやって抜けるのか」が画面に出ていないと、操縦中なのに気づかず
    // ギズモのつもりでカメラを飛ばす事故になる
    const ImVec2 tl = ImGui::GetItemRectMin();
    const float top = ImGui::GetItemRectMax().y;
    ImGui::SetCursorScreenPos(ImVec2(tl.x, top + 6.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.18f, 0.07f, 0.88f));
    ImGui::BeginChild("##sv_pilot", ImVec2(0.0f, 52.0f), ImGuiChildFlags_AutoResizeX,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Text(Tr(StrId::SceneView_PilotOn),
                previewLabel_.empty() ? "?" : previewLabel_.c_str());
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::SceneView_PilotStop))) {
        pilot.Stop();
    }
    ImGui::TextUnformatted(Tr(StrId::SceneView_PilotKeys));
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void SceneViewWindow::DrawCameraPreview(const ImVec2& imgPos, const ImVec2& size)
{
    if (!previewValid_ || !previewRt_.IsValid()) {
        return;
    }
    // ★ImGui のアイテムを作らず drawlist だけで描く — ツールバー/地形パネルが
    //   「直前のアイテム矩形」で位置を決めているので、ここで矩形を動かすと全部ずれる
    const float w = 256.0f;
    const float h = w * static_cast<float>(kCamPreviewH) / static_cast<float>(kCamPreviewW);
    const float pad = 12.0f;
    const float titleH = ImGui::GetTextLineHeight() + 4.0f;
    if (size.x < w + pad * 2.0f || size.y < h + titleH + pad * 2.0f) {
        return; // ビューが小さすぎる — 出すと本編を覆ってしまう
    }
    const ImVec2 tl(imgPos.x + size.x - w - pad, imgPos.y + size.y - h - pad);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(tl.x - 4.0f, tl.y - titleH - 2.0f),
                      ImVec2(tl.x + w + 4.0f, tl.y + h + 4.0f), IM_COL32(26, 28, 33, 220), 4.0f);
    dl->AddImage(reinterpret_cast<ImTextureID>(previewRt_.SRV()), tl, ImVec2(tl.x + w, tl.y + h));
    dl->AddRect(tl, ImVec2(tl.x + w, tl.y + h), IM_COL32(0x40, 0xC0, 0xF0, 0xFF));
    const std::string title = previewLabel_.empty()
        ? std::string(Tr(StrId::SceneView_CamPreview))
        : std::string(Tr(StrId::SceneView_CamPreview)) + " - " + previewLabel_;
    dl->AddText(ImVec2(tl.x, tl.y - titleH), IM_COL32(0xD8, 0xE0, 0xE8, 0xFF), title.c_str());
}

void SceneViewWindow::DrawGizmo(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                const EditorSettings& settings, float rectX, float rectY,
                                float rectW, float rectH)
{
    World& world = ctx.scene->GetWorld();
    GameObject sel = ctx.scene->FindByFileId(selection.primary);
    if (!sel) {
        return;
    }
    const EntityID e = sel.Id();
    auto* wm = world.GetComponent<WorldMatrixComponent>(e);
    auto* lt = world.GetComponent<LocalTransform>(e);
    if (!wm || !lt) {
        return;
    }

    ImGuizmo::SetOrthographic(orthographic_);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(rectX, rectY, rectW, rectH);

    XMFLOAT4X4 worldM = wm->value; // 現在のワールド行列 (前フレームの transform 更新結果)

    // スナップ (Ctrl 押下時。量は editor_settings)
    const bool snap = ImGui::GetIO().KeyCtrl;
    float snapVals[3] = { settings.snapTranslate, settings.snapTranslate, settings.snapTranslate };
    if (gizmoOp_ == ImGuizmo::ROTATE) {
        snapVals[0] = snapVals[1] = snapVals[2] = settings.snapRotateDeg;
    } else if (gizmoOp_ == ImGuizmo::SCALE) {
        snapVals[0] = snapVals[1] = snapVals[2] = settings.snapScale;
    }

    const bool used = ImGuizmo::Manipulate(&lastView_.m[0][0], &lastProj_.m[0][0], gizmoOp_,
                                           gizmoMode_, &worldM.m[0][0], nullptr,
                                           snap ? snapVals : nullptr);
    const bool using_ = ImGuizmo::IsUsing();

    // ドラッグ開始 (rising edge): この時点で LocalTransform はまだ変更前 → before を撮る
    if (using_ && !gizmoActive_) {
        gizmoActive_ = true;
        undo.BeginRecord("Gizmo", selection);
        undo.CaptureBefore(*ctx.scene, selection.primary);
    }

    if (used) {
        // ワールド行列 → ローカル行列 (親があれば親ワールドの逆行列を掛ける)
        WriteWorldToLocal(world, e, *lt, worldM);
    }

    // ドラッグ終了 (falling edge): after を撮って 1 エントリ確定
    if (!using_ && gizmoActive_) {
        gizmoActive_ = false;
        undo.CaptureAfter(*ctx.scene, selection.primary);
        undo.EndRecord(selection);
    }
}

void SceneViewWindow::FocusOnSelection(EngineContext& ctx, Selection& selection)
{
    World& world = ctx.scene->GetWorld();
    GameObject sel = ctx.scene->FindByFileId(selection.primary);
    if (!sel) {
        return;
    }
    auto* wm = world.GetComponent<WorldMatrixComponent>(sel.Id());
    if (!wm) {
        return;
    }
    const XMFLOAT3 target = { wm->value._41, wm->value._42, wm->value._43 };

    // メッシュ AABB (M8) があれば半径を推定してフレーミング距離を決める
    float radius = 1.5f;
    if (auto* mr = world.GetComponent<MeshRendererComponent>(sel.Id())) {
        if (Mesh* mesh = ctx.resources->meshes.Get(mr->mesh)) {
            const XMVECTOR ext = XMVectorSubtract(XMLoadFloat3(&mesh->aabbMax),
                                                  XMLoadFloat3(&mesh->aabbMin));
            float sx = 1, sy = 1, sz = 1;
            if (auto* lt = world.GetComponent<LocalTransform>(sel.Id())) {
                sx = lt->scale.x; sy = lt->scale.y; sz = lt->scale.z;
            }
            const XMVECTOR scaled =
                XMVectorMultiply(ext, XMVectorSet(std::fabs(sx), std::fabs(sy), std::fabs(sz), 0));
            radius = std::max(0.5f, XMVectorGetX(XMVector3Length(scaled)) * 0.5f);
        }
    }
    const float dist = radius / std::sin(XMConvertToRadians(kEditorFovDeg) * 0.5f) + radius;

    XMVECTOR fwd, right, up;
    CamBasis(camPitch_, camYaw_, fwd, right, up);
    const XMVECTOR pos = XMVectorSubtract(XMLoadFloat3(&target), XMVectorScale(fwd, dist));
    XMStoreFloat3(&camPos_, pos);
}

void SceneViewWindow::HandleCamera(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                   EditorSettings& settings)
{
    const ImGuiIO& io = ImGui::GetIO();

    // F: 選択をフレーミング
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        FocusOnSelection(ctx, selection);
    }
    // W/E/R: ギズモ操作切替 (Unity/Unreal 風)
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) { gizmoOp_ = ImGuizmo::TRANSLATE; }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) { gizmoOp_ = ImGuizmo::ROTATE; }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) { gizmoOp_ = ImGuizmo::SCALE; }

    XMVECTOR fwd, right, up;
    CamBasis(camPitch_, camYaw_, fwd, right, up);

    // 操縦モード: RMB / MMB / ホイールの**書き込み先**をエディタカメラからカメラ本体へ
    // 振り替える。操作そのものは同じ (右ドラッグでルック + WASDQE / 中ドラッグでパン /
    // ホイールで前後) — 「いつものカメラ操作でカメラを動かす」がこの機能の要件そのもの
    const EntityID pilotCam = PilotTarget(ctx);
    bool consumed = false;
    if (!pilotCam.IsNull()) {
        HandlePilotCamera(ctx, selection, undo, settings, pilotCam);
        consumed = ImGui::IsMouseDown(ImGuiMouseButton_Right)
            || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    } else {
        // ホイール: RMB ホールド中は移動速度調整 (M27d、Unity/UE 風)、それ以外は前後ズーム
        if (io.MouseWheel != 0.0f) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                settings.camMoveSpeed = std::clamp(
                    settings.camMoveSpeed * std::pow(1.15f, io.MouseWheel), 0.5f, 60.0f);
                camSpeedDirty_ = true;
            } else {
                const XMVECTOR pos =
                    XMVectorAdd(XMLoadFloat3(&camPos_), XMVectorScale(fwd, io.MouseWheel * 1.5f));
                XMStoreFloat3(&camPos_, pos);
            }
        }
        // 速度変更はドラッグ終了時にまとめて永続化 (ホイール毎のファイル IO を避ける)
        if (camSpeedDirty_ && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camSpeedDirty_ = false;
            settings.Save();
        }

        // RMB ドラッグ: FPS ルック + WASDQE 移動 (RMB 中のみ W/E/R は移動として扱う)
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camYaw_ += io.MouseDelta.x * 0.25f;
            camPitch_ = std::clamp(camPitch_ + io.MouseDelta.y * 0.25f, -89.0f, 89.0f);
            CamBasis(camPitch_, camYaw_, fwd, right, up);
            XMVECTOR move = XMVectorZero();
            if (ImGui::IsKeyDown(ImGuiKey_W)) { move = XMVectorAdd(move, fwd); }
            if (ImGui::IsKeyDown(ImGuiKey_S)) { move = XMVectorSubtract(move, fwd); }
            if (ImGui::IsKeyDown(ImGuiKey_D)) { move = XMVectorAdd(move, right); }
            if (ImGui::IsKeyDown(ImGuiKey_A)) { move = XMVectorSubtract(move, right); }
            if (ImGui::IsKeyDown(ImGuiKey_E)) { move = XMVectorAdd(move, XMVectorSet(0, 1, 0, 0)); }
            if (ImGui::IsKeyDown(ImGuiKey_Q)) { move = XMVectorSubtract(move, XMVectorSet(0, 1, 0, 0)); }
            if (XMVectorGetX(XMVector3LengthSq(move)) > 0.0001f) {
                const float speed =
                    settings.camMoveSpeed * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 3.0f : 1.0f);
                move = XMVectorScale(XMVector3Normalize(move), speed * io.DeltaTime);
                XMStoreFloat3(&camPos_, XMVectorAdd(XMLoadFloat3(&camPos_), move));
            }
            consumed = true;
        }
        // MMB ドラッグ: パン (画面平面移動)
        else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            const float k = 0.02f;
            XMVECTOR pos = XMLoadFloat3(&camPos_);
            pos = XMVectorSubtract(pos, XMVectorScale(right, io.MouseDelta.x * k));
            pos = XMVectorAdd(pos, XMVectorScale(up, io.MouseDelta.y * k));
            XMStoreFloat3(&camPos_, pos);
            consumed = true;
        }
    }

    // Alt+LMB ドラッグ: 選択を中心にオービット。
    // ★操縦中も**エディタ視点**に効かせる (F のフレーミングも同じ) — 操縦中に視点を
    //   動かす手段が無いと、カメラを遠くへ飛ばした瞬間に何も見えなくなって詰む。
    //   オービットとフレーミングは「見る位置」しか変えないので対象を取り違えようが無い
    if (!consumed && io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float orbitDist = 12.0f;
        const XMVECTOR pivot = XMVectorAdd(XMLoadFloat3(&camPos_), XMVectorScale(fwd, orbitDist));
        camYaw_ += io.MouseDelta.x * 0.3f;
        camPitch_ = std::clamp(camPitch_ + io.MouseDelta.y * 0.3f, -89.0f, 89.0f);
        CamBasis(camPitch_, camYaw_, fwd, right, up);
        const XMVECTOR pos = XMVectorSubtract(pivot, XMVectorScale(fwd, orbitDist));
        XMStoreFloat3(&camPos_, pos);
    }
}

void SceneViewWindow::HandlePilotCamera(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                        EditorSettings& settings, EntityID cam)
{
    const ImGuiIO& io = ImGui::GetIO();
    World& world = ctx.scene->GetWorld();
    auto* wmc = world.GetComponent<WorldMatrixComponent>(cam);
    auto* lt = world.GetComponent<LocalTransform>(cam);
    if (wmc == nullptr || lt == nullptr) {
        return;
    }

    const bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const bool mmb = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    float wheel = io.MouseWheel;

    // RMB ホールド中のホイールは移動速度の調整 (エディタカメラと同じ約束)。
    // カメラ本体は動かないので Undo エントリも作らない
    if (rmb && wheel != 0.0f) {
        settings.camMoveSpeed =
            std::clamp(settings.camMoveSpeed * std::pow(1.15f, wheel), 0.5f, 60.0f);
        camSpeedDirty_ = true;
        wheel = 0.0f;
    }
    if (camSpeedDirty_ && !rmb) {
        camSpeedDirty_ = false;
        settings.Save();
    }

    // ★姿勢は四元数のまま差分回転で回す。yaw/pitch へ分解して組み直すとロール
    //   (視線軸まわりの傾き) の受け皿が無く、操縦した瞬間に水平へ戻ってしまう —
    //   ギズモで付けた傾きが黙って消える = 静かなデータ損失になる。
    //   スケールも分解した値をそのまま戻すので、親のスケールごと保たれる
    XMVECTOR scale, rot, pos;
    if (!XMMatrixDecompose(&scale, &rot, &pos, XMLoadFloat4x4(&wmc->value))) {
        return; // 退化した行列 (スケール 0 等) — 触ると NaN を撒く
    }
    bool mutated = false;

    if (rmb && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        // 姿勢の更新は純関数へ切り出してある (CameraPilotSelfTest が唯一の機械検査 —
        // 操縦は ImGui のマウス入力で駆動されるのでリプレイにもスクショにも載らない)
        const XMVECTOR next = PilotApplyLook(rot, io.MouseDelta.x, io.MouseDelta.y);
        if (!XMVector4Equal(next, rot)) {
            rot = next;
            mutated = true;
        }
    }

    // 基底は**カメラの現在の姿勢**から取る (ロールが乗っていれば右も上も一緒に傾く =
    // 傾けたカメラの平行移動が画面と一致する)
    const XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), rot);
    const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), rot);
    const XMVECTOR up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), rot);

    if (rmb) {
        XMVECTOR move = XMVectorZero();
        if (ImGui::IsKeyDown(ImGuiKey_W)) { move = XMVectorAdd(move, fwd); }
        if (ImGui::IsKeyDown(ImGuiKey_S)) { move = XMVectorSubtract(move, fwd); }
        if (ImGui::IsKeyDown(ImGuiKey_D)) { move = XMVectorAdd(move, right); }
        if (ImGui::IsKeyDown(ImGuiKey_A)) { move = XMVectorSubtract(move, right); }
        if (ImGui::IsKeyDown(ImGuiKey_E)) { move = XMVectorAdd(move, XMVectorSet(0, 1, 0, 0)); }
        if (ImGui::IsKeyDown(ImGuiKey_Q)) { move = XMVectorSubtract(move, XMVectorSet(0, 1, 0, 0)); }
        if (XMVectorGetX(XMVector3LengthSq(move)) > 0.0001f) {
            const float speed =
                settings.camMoveSpeed * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 3.0f : 1.0f);
            pos = XMVectorAdd(pos, XMVectorScale(XMVector3Normalize(move), speed * io.DeltaTime));
            mutated = true;
        }
    } else if (mmb) {
        // MMB ドラッグ: パン (画面平面移動)
        const float k = 0.02f;
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
            pos = XMVectorSubtract(pos, XMVectorScale(right, io.MouseDelta.x * k));
            pos = XMVectorAdd(pos, XMVectorScale(up, io.MouseDelta.y * k));
            mutated = true;
        }
    }
    if (wheel != 0.0f) {
        pos = XMVectorAdd(pos, XMVectorScale(fwd, wheel * 1.5f));
        mutated = true;
    }

    if (mutated) {
        // Undo はギズモと同じ流儀 — ドラッグの立ち上がりで before を撮り、ボタンを
        // 離した時点 (ClosePilotRecord) で 1 エントリに閉じる。まだ書き戻していない
        // ここで撮るので before は「動かす前」のまま
        if (!pilotRecording_) {
            pilotRecording_ = true;
            pilotRecordFid_ = ctx.scene->EnsureFileId(cam);
            undo.BeginRecord("Pilot Camera", selection);
            undo.CaptureBefore(*ctx.scene, pilotRecordFid_);
        }
        XMFLOAT4X4 worldM;
        XMStoreFloat4x4(&worldM, XMMatrixScalingFromVector(scale)
                                     * XMMatrixRotationQuaternion(rot)
                                     * XMMatrixTranslationFromVector(pos));
        WriteWorldToLocal(world, cam, *lt, worldM);
    }
}

void SceneViewWindow::ClosePilotRecord(EngineContext& ctx, Selection& selection, UndoStack& undo)
{
    if (!pilotRecording_) {
        return;
    }
    // ★ボタンを離す場所は SceneView の外かもしれない (ドラッグしたままウィンドウを出る)。
    //   HandleCamera はホバー中しか呼ばれないので、閉じ判定は OnImGui から**無条件で**
    //   回す。開きっぱなしにすると次の編集が全部このエントリに巻き込まれる
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        return;
    }
    pilotRecording_ = false;
    undo.CaptureAfter(*ctx.scene, pilotRecordFid_);
    undo.EndRecord(selection);
    pilotRecordFid_ = 0;
}

bool SceneViewWindow::HandleTerrainBrush(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                         const ImVec2& imgPos, const ImVec2& size)
{
    terrainHasTarget_ = false;
    TerrainTarget target;
    if (!terrainBrush_ || ctx.assetsRoot.empty() || !FindTerrainTarget(ctx, selection, target)) {
        terrainStroking_ = false;
        return false;
    }
    std::wstring abs = ctx.assetsRoot;
    if (!abs.empty() && abs.back() != L'\\' && abs.back() != L'/') {
        abs += L'\\';
    }
    abs += Utf8ToWide(target.source);
    terrainHasTarget_ = true;

    // 作業コピー。ストロークの外でも必要 (ブラシリングを地表に貼るのに高さが要る)。
    // ★ストロークの開始時には**必ず読み直す** — Undo/Redo はサイドカーを直接書き換えるので、
    //   ここのコピーを使い回すと「取り消した結果」を握り潰して塗り戻してしまう
    if (terrainStrokeSrc_ != abs && !terrainStroking_) {
        if (!TerrainAsset::Load(abs, terrainStrokeWork_)) {
            terrainStrokeSrc_.clear();
            return false;
        }
        terrainStrokeSrc_ = abs;
    }
    if (!terrainStrokeWork_.Valid()) {
        return false;
    }

    // カーソル下の地表 (地形ローカル空間)
    XMFLOAT3 hitLocal = { 0.0f, 0.0f, 0.0f };
    bool haveHit = false;
    XMFLOAT3 ro, rd;
    if (MouseRay(imgPos, size, ro, rd)) {
        XMVECTOR det;
        const XMMATRIX inv = XMMatrixInverse(&det, XMLoadFloat4x4(&target.world));
        if (XMVectorGetX(det) != 0.0f) {
            XMFLOAT3 lo, ld;
            XMStoreFloat3(&lo, XMVector3TransformCoord(XMLoadFloat3(&ro), inv));
            XMStoreFloat3(&ld, XMVector3TransformNormal(XMLoadFloat3(&rd), inv));
            haveHit = TerrainEdit::RaycastLocal(terrainStrokeWork_, lo, ld, kFarZ, hitLocal);
        }
    }

    // ブラシリング (ImGui drawlist に world→screen 投影。GPU パスを増やさない)
    if (haveHit) {
        const XMMATRIX wvp = XMMatrixMultiply(
            XMLoadFloat4x4(&target.world),
            XMMatrixMultiply(XMLoadFloat4x4(&lastView_), XMLoadFloat4x4(&lastProj_)));
        constexpr int kRingSegments = 48;
        ImVec2 pts[kRingSegments];
        int n = 0;
        for (int i = 0; i < kRingSegments; ++i) {
            const float a = 6.28318530718f * static_cast<float>(i) / kRingSegments;
            const float lx = hitLocal.x + terrainRadius_ * std::cos(a);
            const float lz = hitLocal.z + terrainRadius_ * std::sin(a);
            // 地表から少し浮かせる (Z ファイトではなく、リングが尾根に隠れて途切れないよう)
            const float ly = TerrainEdit::SampleHeightLocal(terrainStrokeWork_, lx, lz) + 0.05f;
            const XMVECTOR clip = XMVector4Transform(XMVectorSet(lx, ly, lz, 1.0f), wvp);
            const float w = XMVectorGetW(clip);
            if (w <= 0.01f) {
                continue; // カメラ後方の点は落とす (残りだけで輪を描く)
            }
            pts[n++] = ImVec2(imgPos.x + (XMVectorGetX(clip) / w * 0.5f + 0.5f) * size.x,
                              imgPos.y + (0.5f - XMVectorGetY(clip) / w * 0.5f) * size.y);
        }
        if (n >= 3) {
            // ★imgui 1.92.8 で thickness と flags の順序が入れ替わっている
            //   (旧順序のオーバーロードは = delete。M51f の AddRect と同じ罠)
            ImGui::GetWindowDrawList()->AddPolyline(pts, n, IM_COL32(0xFF, 0xC0, 0x40, 0xE0), 2.0f,
                                                    ImDrawFlags_Closed);
        }
    }

    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered();
    bool consumed = false;

    if (hovered && haveHit && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (TerrainAsset::Load(abs, terrainStrokeWork_)) {
            terrainStrokeSrc_ = abs;
            terrainStrokeBase_ = terrainStrokeWork_; // 差分の基準 (ストローク全体で 1 エントリ)
            terrainStroking_ = true;
            terrainHasDab_ = false;
        }
    }

    if (terrainStroking_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        consumed = true;
        // ダブの間隔。★カーソルが止まっている間も塗り続けると、押しっぱなしで穴が
        //   底まで抜ける (しかも 1 ダブ = 1 回のサイドカー書き出しなのでディスクも回り続ける)。
        //   実際のブラシと同じ「一定距離ごとに 1 ダブ」にしてある
        const float spacing = std::max(0.05f, terrainRadius_ * 0.25f);
        const float dx = hitLocal.x - terrainLastDab_.x;
        const float dz = hitLocal.z - terrainLastDab_.z;
        if (haveHit && (!terrainHasDab_ || dx * dx + dz * dz >= spacing * spacing)) {
            TerrainEdit::Brush b;
            b.mode = static_cast<TerrainEdit::BrushMode>(terrainBrushMode_);
            if (io.KeyShift) {
                b.mode = TerrainEdit::BrushMode::Smooth; // Shift = 一時的に平滑化
            }
            b.centerX = hitLocal.x;
            b.centerZ = hitLocal.z;
            b.radius = terrainRadius_;
            b.strength = terrainStrength_;
            if (b.mode == TerrainEdit::BrushMode::Raise && io.KeyCtrl) {
                b.strength = -terrainStrength_; // Ctrl = 掘る
            }
            b.layer = static_cast<uint32_t>(std::clamp(terrainLayer_, 0, 3));
            if (TerrainEdit::ApplyBrush(terrainStrokeWork_, b)) {
                if (TerrainEdit::SaveEdits(abs, terrainStrokeWork_)) {
                    if (ctx.renderSystem != nullptr) {
                        ctx.renderSystem->InvalidateTerrain();
                    }
                } else {
                    MYE_LOG_ERROR(Tr(StrId::Terrain_SaveFail), WideToUtf8(abs).c_str());
                }
            }
            terrainLastDab_ = hitLocal;
            terrainHasDab_ = true;
        }
    }

    if (terrainStroking_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        terrainStroking_ = false;
        consumed = true;
        // ★Undo エントリは**ストローク全体で 1 個**。ダブごとに積むと 1 撫でで数十回の
        //   Ctrl+Z が必要になる (ギズモのドラッグを 1 エントリにまとめているのと同じ流儀)
        TerrainEdit::TerrainPatch patch;
        if (TerrainEdit::MakeDiffPatch(terrainStrokeBase_, terrainStrokeWork_, patch)
            && !patch.Empty()) {
            UndoFileOp op;
            op.kind = UndoFileOp::Kind::TerrainPaint;
            op.pathA = abs;
            TerrainEdit::SerializePatch(patch, op.bytes);
            undo.PushFileOp("Terrain Brush", std::move(op));
        }
        terrainStrokeBase_ = TerrainAsset::TerrainData{}; // 基準コピーを手放す
    }
    return consumed;
}

void SceneViewWindow::DrawTerrainBrushPanel(EngineContext& ctx)
{
    if (!terrainBrush_) {
        return;
    }
    (void)ctx;
    // ツールバー (直前に描いた子ウィンドウ) の真下に貼る
    const ImVec2 tl = ImGui::GetItemRectMin();
    const float top = ImGui::GetItemRectMax().y;
    ImGui::SetCursorScreenPos(ImVec2(tl.x, top + 6.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.13f, 0.85f));
    ImGui::BeginChild("##sv_terrain_brush", ImVec2(0.0f, 84.0f), ImGuiChildFlags_AutoResizeX,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!terrainHasTarget_) {
        ImGui::TextUnformatted(Tr(StrId::Terrain_NoTarget));
    } else {
        auto modeBtn = [&](StrId id, int mode) {
            const bool on = (terrainBrushMode_ == mode);
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.45f, 0.78f, 1.0f));
            }
            if (ImGui::Button(Tr(id))) {
                terrainBrushMode_ = mode;
            }
            if (on) {
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();
        };
        modeBtn(StrId::Terrain_ModeRaise, 0);
        modeBtn(StrId::Terrain_ModeSmooth, 1);
        modeBtn(StrId::Terrain_ModePaint, 2);
        if (terrainBrushMode_ == 2) {
            ImGui::SetNextItemWidth(90.0f);
            ImGui::Combo(Tr(StrId::Terrain_Layer), &terrainLayer_, "0\0" "1\0" "2\0" "3\0");
        } else {
            ImGui::NewLine();
        }
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat(Tr(StrId::Terrain_Radius), &terrainRadius_, 1.0f, 64.0f, "%.1f m");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        // Raise だけ単位が m/ダブ、Smooth/Paint は 0..1 の寄せ率 (上限を共有しても
        // ApplyBrush 側が clamp するので害は無い)
        ImGui::SliderFloat(Tr(StrId::Terrain_Strength), &terrainStrength_, 0.05f, 4.0f, "%.2f");
        ImGui::TextUnformatted(Tr(StrId::Terrain_Hint));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void SceneViewWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo,
                              EditorSettings& settings)
{
    // 開きっぱなしの操縦 Undo をまず閉じる。★ここは `open` チェックより**前**でなければ
    // ならない — ドラッグしたままビューの外でボタンを離す / タブを閉じる経路があり、
    // 記録が開いたままだと以降の編集が全部そのエントリに巻き込まれる
    ClosePilotRecord(ctx, selection, undo);

    if (!open) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(Tr(StrId::Win_Scene), &open);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    desiredW_ = static_cast<int>(std::max(avail.x, 16.0f));
    desiredH_ = static_cast<int>(std::max(avail.y, 16.0f));

    const ImVec2 imgPos = ImGui::GetCursorScreenPos();
    if (rt_.IsValid()) {
        ImGui::Image(reinterpret_cast<ImTextureID>(rt_.SRV()), avail);
        // AssetBrowser からのドロップ: .cs はカーソル下の 3D オブジェクトにアタッチ、
        // .mat.json はカーソル下のオブジェクトの材質に割当 (どちらもピッキング)、
        // その他 (プレハブ/モデル/画像) はカーソル下の地面 (y=0) に配置
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pa = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
                const std::wstring path = Utf8ToWide(static_cast<const char*>(pa->Data));
                const AssetType dropType = AssetDatabase::ClassifyPath(path);
                if (dropType == AssetType::Script || dropType == AssetType::Material) {
                    const ImGuiIO& dio = ImGui::GetIO();
                    const int px = static_cast<int>(dio.MousePos.x - imgPos.x);
                    const int py = static_cast<int>(dio.MousePos.y - imgPos.y);
                    if (px >= 0 && py >= 0 && px < rt_.Width() && py < rt_.Height()) {
                        if (!picking_.IsReady()) {
                            picking_.Init(*ctx.device, *ctx.shaders);
                        }
                        const EntityID hit =
                            picking_.Pick(*ctx.device, ctx.scene->GetWorld(), *ctx.shaders,
                                          *ctx.resources, lastView_, lastProj_, rt_.Width(),
                                          rt_.Height(), px, py);
                        if (!hit.IsNull()) {
                            if (dropType == AssetType::Script) {
                                AttachScriptToEntity(ctx, selection, undo, path, hit);
                            } else {
                                AssignMaterialToEntity(ctx, selection, undo, path, hit);
                            }
                        } else if (dropType == AssetType::Script) {
                            MYE_LOG_WARN("no entity under cursor — drop the script onto an object "
                                         "(or an entity row in Hierarchy)");
                        } else {
                            MYE_LOG_WARN("no entity under cursor — drop the material onto a mesh "
                                         "object");
                        }
                    }
                } else {
                    XMFLOAT3 gp = { 0, 0, 0 };
                    const XMFLOAT3* pp = GroundPointUnderCursor(imgPos, avail, gp) ? &gp : nullptr;
                    InstantiateAssetAtPath(ctx, selection, undo, path, pp, 0);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    // 地形ブラシ (M58f)。★ギズモ**より前**に置く — 塗っている最中にギズモが左ボタンを
    // 掴むと、地形を撫でたつもりで選択オブジェクトが飛んでいく
    const bool brushConsumed = HandleTerrainBrush(ctx, selection, undo, imgPos, avail);

    // ギズモ (ImGui 描画レイヤ — シーン RT/backbuffer には焼き込まれない)
    if (selection.primary != 0 && !terrainBrush_) {
        DrawGizmo(ctx, selection, undo, settings, imgPos.x, imgPos.y, avail.x, avail.y);
    }

    // ---- ビルボードアイコン (M40b): カメラ/ライト/エミッタ位置に FA アイコンを重ねる。
    //      GPU パス不要 (ImGui drawlist に world→screen 投影) + クリックで選択 ----
    const ImGuiIO& io = ImGui::GetIO();
    bool iconClicked = brushConsumed; // ブラシが左ボタンを掴んでいる間は選択させない
    if (showGizmos_ && rt_.IsValid()) {
        World& world = ctx.scene->GetWorld();
        const XMMATRIX vp =
            XMMatrixMultiply(XMLoadFloat4x4(&lastView_), XMLoadFloat4x4(&lastProj_));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        struct IconHit {
            float dist2;
            EntityID entity;
        };
        IconHit best = { 14.0f * 14.0f, kNullEntity }; // クリック判定半径 14px

        auto drawIcons = [&](ComponentTypeId type, const char* icon, ImU32 color) {
            const ComponentTypeId req[] = { type, WorldMatrixComponent::sTypeId };
            world.ForEachArchetype(req, [&](Archetype& arch) {
                const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const XMFLOAT4X4& wm =
                        static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                    const XMVECTOR clip =
                        XMVector4Transform(XMVectorSet(wm._41, wm._42, wm._43, 1.0f), vp);
                    const float w = XMVectorGetW(clip);
                    if (w <= 0.01f) {
                        continue; // カメラ後方
                    }
                    const float ndcX = XMVectorGetX(clip) / w;
                    const float ndcY = XMVectorGetY(clip) / w;
                    if (ndcX < -1.1f || ndcX > 1.1f || ndcY < -1.1f || ndcY > 1.1f) {
                        continue;
                    }
                    const ImVec2 sp(imgPos.x + (ndcX * 0.5f + 0.5f) * avail.x,
                                    imgPos.y + (0.5f - ndcY * 0.5f) * avail.y);
                    const ImVec2 ts = ImGui::CalcTextSize(icon);
                    dl->AddText(ImVec2(sp.x - ts.x * 0.5f + 1.0f, sp.y - ts.y * 0.5f + 1.0f),
                                IM_COL32(0, 0, 0, 160), icon); // 視認性のための影
                    dl->AddText(ImVec2(sp.x - ts.x * 0.5f, sp.y - ts.y * 0.5f), color, icon);
                    const float dx = io.MousePos.x - sp.x;
                    const float dy = io.MousePos.y - sp.y;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < best.dist2) {
                        best = { d2, arch.EntityAt(row) };
                    }
                }
            });
        };
        drawIcons(CameraComponent::sTypeId, ICON_FA_VIDEO, IM_COL32(0x40, 0xC0, 0xF0, 0xFF));
        drawIcons(LightComponent::sTypeId, ICON_FA_LIGHTBULB, IM_COL32(0xF0, 0xE0, 0x40, 0xFF));
        drawIcons(ParticleEmitterComponent::sTypeId, ICON_FA_FIRE,
                  IM_COL32(0xF0, 0x80, 0x20, 0xFF));
        // 部位ソケット (M48i): メッシュを持たないので通常のピッキングでは掴めない。
        // アイコン経路に載せて初めてクリック選択できるようになる
        drawIcons(PartComponent::sTypeId, ICON_FA_ANCHOR, IM_COL32(0xF0, 0x60, 0xC0, 0xFF));

        // アイコンクリックで選択 (ピッキングより優先。Ctrl はトグル = ピッキングと同じ流儀)
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() && !io.KeyAlt && !terrainBrush_
            && !best.entity.IsNull()) {
            const uint64_t iconFid = ctx.scene->EnsureFileId(best.entity);
            if (io.KeyCtrl) {
                selection.Toggle(iconFid);
            } else {
                selection.SelectOnly(iconFid);
            }
            iconClicked = true;
        }
    }

    // クリックでピッキング選択 (ギズモ上・オービット操作中・アイコンヒット時は除外)
    const bool overGizmo = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overGizmo
        && !io.KeyAlt && !iconClicked && rt_.IsValid()) {
        const int px = static_cast<int>(io.MousePos.x - imgPos.x);
        const int py = static_cast<int>(io.MousePos.y - imgPos.y);
        if (px >= 0 && py >= 0 && px < rt_.Width() && py < rt_.Height()) {
            if (!picking_.IsReady()) {
                picking_.Init(*ctx.device, *ctx.shaders);
            }
            const EntityID hit = picking_.Pick(*ctx.device, ctx.scene->GetWorld(), *ctx.shaders,
                                               *ctx.resources, lastView_, lastProj_, rt_.Width(),
                                               rt_.Height(), px, py);

            // 部位ボリューム (M49): CPU レイで部位の範囲を判定し、GPU ピックと付き合わせる。
            // 判定・ポーズはワイヤ表示/sim と同じ Parts::RaycastParts の 1 本。
            // 優先規則: ボリュームは「メッシュ表面を包むクリック領域」なので、ピックした
            // メッシュの AABB を**抜ける前**にボリュームへ入るなら部位が勝つ (ボリュームは
            // メッシュ AABB の内側にあるのが普通で、入口 t 同士の比較だと常にメッシュが
            // 勝ってしまう)。手前の別メッシュに遮られている部位は AABB の出口より遠いので
            // 選ばれない。深度バッファは読まない近似
            EntityID chosen = hit;
            XMFLOAT3 ro, rd;
            Parts::PartRayHit partHit;
            if (MouseRay(imgPos, avail, ro, rd)
                && Parts::RaycastParts(ctx.scene->GetWorld(), kNullEntity, 0, ro, rd, kFarZ,
                                       partHit)) {
                float exitT = kFarZ; // ピック無し / AABB 不明 (スキンメッシュ等) は部位が勝つ
                if (!hit.IsNull()) {
                    World& w = ctx.scene->GetWorld();
                    const auto* mr = w.GetComponent<MeshRendererComponent>(hit);
                    const auto* wmc = w.GetComponent<WorldMatrixComponent>(hit);
                    const Mesh* mesh = mr ? ctx.resources->meshes.Get(mr->mesh) : nullptr;
                    if (mesh && wmc) {
                        XMFLOAT3 lo, hi;
                        WorldAabb(wmc->value, mesh->aabbMin, mesh->aabbMax, lo, hi);
                        float t;
                        if (RayAabbExit(ro, rd, lo, hi, t)) {
                            exitT = t;
                        }
                    }
                }
                if (partHit.distance <= exitT) {
                    chosen = partHit.entity;
                }
            }

            if (!chosen.IsNull()) {
                const uint64_t fid = ctx.scene->EnsureFileId(chosen);
                if (io.KeyCtrl) {
                    selection.Toggle(fid);
                } else {
                    selection.SelectOnly(fid);
                }
            } else if (!io.KeyCtrl) {
                selection.Clear();
            }
        }
    }

    // 右クリック (ドラッグなし・ギズモ外) → 生成メニュー (Hierarchy 右クリックと同じ項目)。
    // RMB ドラッグは FPS ルック (HandleCamera) なので、移動量 4px 未満のリリースのみクリック扱い。
    // 地面点はメニュー操作中にカーソルが動くため、開いた瞬間に固定する
    if (ImGui::IsWindowHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)
        && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()
        && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 4.0f * 4.0f) {
        ctxSpawnValid_ = GroundPointUnderCursor(imgPos, avail, ctxSpawnPos_);
        ImGui::OpenPopup("##scene_create");
    }
    if (ImGui::BeginPopup("##scene_create")) {
        DrawCreateMenuItems(ctx, selection, undo, kNullEntity,
                            ctxSpawnValid_ ? &ctxSpawnPos_ : nullptr);
        ImGui::EndPopup();
    }

    // カメラ操作: ウィンドウ上 & ギズモ操作中でない時のみ
    if (ImGui::IsWindowHovered() && !ImGuizmo::IsUsing()) {
        HandleCamera(ctx, selection, undo, settings);
    }
    // Esc で操縦を抜ける (ホバーでも フォーカスでも効かせる — 操縦を止めたいときに
    // カーソルがビューの上にあるとは限らない)
    if (GetCameraPilot().Active() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)
        && (ImGui::IsWindowHovered() || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))) {
        GetCameraPilot().Stop();
    }
    DrawCameraPreview(imgPos, avail); // drawlist のみ = 下の GetItemRect* を汚さない
    DrawToolbar(settings);
    DrawPilotBanner();          // ツールバー直下
    DrawTerrainBrushPanel(ctx); // ツールバー直下 (GetItemRect* が上の子ウィンドウを指す)
    ImGui::End();
}

bool SceneViewWindow::MouseRay(const ImVec2& imgPos, const ImVec2& size, XMFLOAT3& origin,
                               XMFLOAT3& dir) const
{
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float ndcX = ((io.MousePos.x - imgPos.x) / size.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((io.MousePos.y - imgPos.y) / size.y) * 2.0f;
    const XMMATRIX viewProj =
        XMMatrixMultiply(XMLoadFloat4x4(&lastView_), XMLoadFloat4x4(&lastProj_));
    XMVECTOR det;
    const XMMATRIX inv = XMMatrixInverse(&det, viewProj);
    if (XMVectorGetX(det) == 0.0f) {
        return false;
    }
    // クリップ空間の near/far をワールドへ逆射影しレイを作る (DX: NDC z は [0,1])
    const XMVECTOR nearP = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inv);
    const XMVECTOR farP = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inv);
    XMStoreFloat3(&origin, nearP);
    XMStoreFloat3(&dir, XMVector3Normalize(XMVectorSubtract(farP, nearP)));
    return true;
}

bool SceneViewWindow::GroundPointUnderCursor(const ImVec2& imgPos, const ImVec2& size,
                                             XMFLOAT3& out) const
{
    XMFLOAT3 ro, rd;
    if (!MouseRay(imgPos, size, ro, rd)) {
        return false;
    }
    if (std::fabs(rd.y) < 1e-6f) {
        return false; // 視線が地面と平行
    }
    const float t = -ro.y / rd.y;
    const XMVECTOR hit = XMVectorAdd(XMLoadFloat3(&ro),
                                     XMVectorScale(XMLoadFloat3(&rd), (t < 0.0f) ? 0.0f : t));
    XMStoreFloat3(&out, hit);
    return true;
}

bool SceneViewWindow::PickAtCenter(EngineContext& ctx, Selection& selection)
{
    if (!rt_.IsValid()) {
        return false;
    }
    if (!picking_.IsReady()) {
        picking_.Init(*ctx.device, *ctx.shaders);
    }
    const EntityID hit = picking_.Pick(*ctx.device, ctx.scene->GetWorld(), *ctx.shaders,
                                       *ctx.resources, lastView_, lastProj_, rt_.Width(),
                                       rt_.Height(), rt_.Width() / 2, rt_.Height() / 2);
    if (hit.IsNull()) {
        return false;
    }
    selection.SelectOnly(ctx.scene->EnsureFileId(hit));
    return true;
}

} // namespace mye
