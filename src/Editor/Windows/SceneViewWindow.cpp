#include "Editor/Windows/SceneViewWindow.h"

#include <algorithm>
#include <cmath>

#include "Editor/AssetOps.h"
#include "Editor/CreateMenu.h"
#include "Editor/EditorSettings.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/Shapes.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Scene.h"
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

void CamBasis(float pitchDeg, float yawDeg, XMVECTOR& fwd, XMVECTOR& right, XMVECTOR& up)
{
    const XMVECTOR q = XMQuaternionRotationRollPitchYaw(pitchDeg * kDeg2Rad, yawDeg * kDeg2Rad, 0.0f);
    fwd = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
    right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);
    up = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), q);
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
    ImGui::BeginChild("##sv_toolbar", ImVec2(830.0f, 30.0f), ImGuiChildFlags_None,
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
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &showGrid_);
    ImGui::SameLine();
    ImGui::Checkbox("Gizmos", &showGizmos_);
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
    modeBtn("Lit", 0);
    modeBtn("Unlit", 1);
    modeBtn("Wire", 2);
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    // カメラ速度 (M27d)。RMB ホールド中のホイールでも変わる (HandleCamera)
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::SliderFloat("##camspeed", &settings.camMoveSpeed, 0.5f, 60.0f, "cam %.1f",
                           ImGuiSliderFlags_Logarithmic)) {
        camSpeedDirty_ = true; // 永続化は操作終了時 (HandleCamera 側の Save に相乗り)
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        camSpeedDirty_ = false;
        settings.Save();
    }
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

void SceneViewWindow::HandleCamera(EngineContext& ctx, Selection& selection,
                                   EditorSettings& settings)
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
    if (!open) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin("Scene", &open);
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

    // ギズモ (ImGui 描画レイヤ — シーン RT/backbuffer には焼き込まれない)
    if (selection.primary != 0) {
        DrawGizmo(ctx, selection, undo, settings, imgPos.x, imgPos.y, avail.x, avail.y);
    }

    // ---- ビルボードアイコン (M40b): カメラ/ライト/エミッタ位置に FA アイコンを重ねる。
    //      GPU パス不要 (ImGui drawlist に world→screen 投影) + クリックで選択 ----
    const ImGuiIO& io = ImGui::GetIO();
    bool iconClicked = false;
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

        // アイコンクリックで選択 (ピッキングより優先。Ctrl はトグル = ピッキングと同じ流儀)
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() && !io.KeyAlt
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
        HandleCamera(ctx, selection, settings);
    }

    DrawToolbar(settings);
    ImGui::End();
}

bool SceneViewWindow::GroundPointUnderCursor(const ImVec2& imgPos, const ImVec2& size,
                                             XMFLOAT3& out) const
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
    const XMVECTOR dir = XMVectorSubtract(farP, nearP);
    const float dy = XMVectorGetY(dir);
    if (std::fabs(dy) < 1e-6f) {
        return false; // 視線が地面と平行
    }
    const float t = -XMVectorGetY(nearP) / dy;
    const XMVECTOR hit = XMVectorAdd(nearP, XMVectorScale(dir, (t < 0.0f) ? 0.0f : t));
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
