#include "Editor/Windows/SceneViewWindow.h"

#include <algorithm>
#include <cmath>

#include "Editor/EditorSettings.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/RenderPath.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
constexpr float kEditorFovDeg = 60.0f;
constexpr float kNearZ = 0.1f;
constexpr float kFarZ = 1000.0f;

void CamBasis(float pitchDeg, float yawDeg, XMVECTOR& fwd, XMVECTOR& right, XMVECTOR& up)
{
    const XMVECTOR q = XMQuaternionRotationRollPitchYaw(pitchDeg * kDeg2Rad, yawDeg * kDeg2Rad, 0.0f);
    fwd = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
    right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);
    up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), q);
}

} // namespace

void SceneViewWindow::OnRenderViews(EngineContext& ctx)
{
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

    // ギズモがレンダ画像とピクセル一致するよう、描画と同じ view/proj を保存する
    lastView_ = cam.view;
    const float aspect =
        (rt_.Height() > 0) ? static_cast<float>(rt_.Width()) / static_cast<float>(rt_.Height()) : 1.0f;
    const XMMATRIX proj = orthographic_
        ? XMMatrixOrthographicLH(static_cast<float>(rt_.Width()) * 0.02f,
                                 static_cast<float>(rt_.Height()) * 0.02f, kNearZ, kFarZ)
        : XMMatrixPerspectiveFovLH(XMConvertToRadians(kEditorFovDeg), aspect, kNearZ, kFarZ);
    XMStoreFloat4x4(&lastProj_, proj);

    FrameTarget target;
    target.rtv = rt_.RTV();
    target.dsv = rt_.DSV();
    target.width = rt_.Width();
    target.height = rt_.Height();
    ctx.renderSystem->Render(ctx.scene->GetWorld(), *ctx.device, *ctx.renderPath, *ctx.shaders,
                             *ctx.resources, target, &cam, ctx.particles);
}

void SceneViewWindow::DrawToolbar()
{
    // ビューポート左上のオーバーレイツールバー (ギズモ操作 / 座標系 / 投影)
    const ImVec2 p = ImGui::GetItemRectMin();
    ImGui::SetCursorScreenPos(ImVec2(p.x + 8.0f, p.y + 8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.13f, 0.85f));
    ImGui::BeginChild("##sv_toolbar", ImVec2(360.0f, 30.0f), ImGuiChildFlags_None,
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
    opBtn("Move", ImGuizmo::TRANSLATE);
    opBtn("Rotate", ImGuizmo::ROTATE);
    opBtn("Scale", ImGuizmo::SCALE);
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    if (ImGui::Button(gizmoMode_ == ImGuizmo::LOCAL ? "Local" : "World")) {
        gizmoMode_ = (gizmoMode_ == ImGuizmo::LOCAL) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Ortho", &orthographic_);
    ImGui::EndChild();
    ImGui::PopStyleColor();
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
            XMStoreFloat3(&lt->position, t);
            XMStoreFloat4(&lt->rotation, r);
            XMStoreFloat3(&lt->scale, s);
        }
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

void SceneViewWindow::HandleCamera(EngineContext& ctx, Selection& selection)
{
    (void)ctx;
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

    // ホイール: 前後ズーム
    if (io.MouseWheel != 0.0f) {
        const XMVECTOR pos = XMVectorAdd(XMLoadFloat3(&camPos_), XMVectorScale(fwd, io.MouseWheel * 1.5f));
        XMStoreFloat3(&camPos_, pos);
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
            const float speed = ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 20.0f : 6.0f;
            move = XMVectorScale(XMVector3Normalize(move), speed * io.DeltaTime);
            XMStoreFloat3(&camPos_, XMVectorAdd(XMLoadFloat3(&camPos_), move));
        }
    }
    // MMB ドラッグ: パン (画面平面移動)
    else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        const float k = 0.02f;
        XMVECTOR pos = XMLoadFloat3(&camPos_);
        pos = XMVectorSubtract(pos, XMVectorScale(right, io.MouseDelta.x * k));
        pos = XMVectorAdd(pos, XMVectorScale(up, io.MouseDelta.y * k));
        XMStoreFloat3(&camPos_, pos);
    }
    // Alt+LMB ドラッグ: 選択を中心にオービット
    else if (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float orbitDist = 12.0f;
        const XMVECTOR pivot = XMVectorAdd(XMLoadFloat3(&camPos_), XMVectorScale(fwd, orbitDist));
        camYaw_ += io.MouseDelta.x * 0.3f;
        camPitch_ = std::clamp(camPitch_ + io.MouseDelta.y * 0.3f, -89.0f, 89.0f);
        CamBasis(camPitch_, camYaw_, fwd, right, up);
        const XMVECTOR pos = XMVectorSubtract(pivot, XMVectorScale(fwd, orbitDist));
        XMStoreFloat3(&camPos_, pos);
    }
}

void SceneViewWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo,
                              EditorSettings& settings)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin("Scene");
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
    }

    // ギズモ (ImGui 描画レイヤ — シーン RT/backbuffer には焼き込まれない)
    if (selection.primary != 0) {
        DrawGizmo(ctx, selection, undo, settings, imgPos.x, imgPos.y, avail.x, avail.y);
    }

    // クリックでピッキング選択 (ギズモ上・オービット操作中は除外)
    const ImGuiIO& io = ImGui::GetIO();
    const bool overGizmo = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overGizmo
        && !io.KeyAlt && rt_.IsValid()) {
        const int px = static_cast<int>(io.MousePos.x - imgPos.x);
        const int py = static_cast<int>(io.MousePos.y - imgPos.y);
        if (px >= 0 && py >= 0 && px < rt_.Width() && py < rt_.Height()) {
            if (!picking_.IsReady()) {
                picking_.Init(*ctx.device, *ctx.shaders);
            }
            const EntityID hit = picking_.Pick(*ctx.device, ctx.scene->GetWorld(), *ctx.shaders,
                                               *ctx.resources, lastView_, lastProj_, rt_.Width(),
                                               rt_.Height(), px, py);
            if (!hit.IsNull()) {
                const uint64_t fid = ctx.scene->EnsureFileId(hit);
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

    // カメラ操作: ウィンドウ上 & ギズモ操作中でない時のみ
    if (ImGui::IsWindowHovered() && !ImGuizmo::IsUsing()) {
        HandleCamera(ctx, selection);
    }

    DrawToolbar();
    ImGui::End();
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
