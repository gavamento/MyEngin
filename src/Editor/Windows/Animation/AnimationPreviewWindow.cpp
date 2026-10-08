// ============================================================================
//                          AnimationPreviewWindow.cpp
// ============================================================================
// 骨アニメのプレビュー窓 (M89n)。
// ============================================================================
#include "Editor/Windows/Animation/AnimationPreviewWindow.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

#include "Editor/Scene/Selection.h"
#include "Editor/Windows/Animation/ControllerStateEdit.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Animation/AnimatorController.h"
#include "Engine/Engine/Animation/SkinningSystem.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

#include "imgui.h"

namespace mye {

using namespace DirectX;

namespace {

constexpr float kFovYDeg = 40.0f;
constexpr float kPi = 3.14159265f;
constexpr int32_t kQ = SkinnedMeshComponent::kPoseTimeQPerTick;
constexpr int32_t kWeightOne = SkinnedMeshComponent::kPoseWeightOne;
// 再生は 60 tick/秒 (sim と同じ速さで見せる)
constexpr float kTicksPerSecond = 60.0f;
// 画像の最小の大きさ (これより狭い窓では描かない)
constexpr float kMinImageSize = 64.0f;
// 骨の線の色 (ABGR、ImGui の IM_COL32 の並び)
constexpr ImU32 kBoneColor = IM_COL32(255, 210, 60, 255);
constexpr ImU32 kEventColor = IM_COL32(110, 200, 255, 255);

XMMATRIX WorldOf(World& world, EntityID e)
{
    const WorldMatrixComponent* wm = world.GetComponent<WorldMatrixComponent>(e);
    return wm != nullptr ? XMLoadFloat4x4(&wm->value) : XMMatrixIdentity();
}

// 写した SkinnedMesh を、窓が書くポーズプログラムだけで描かれる状態にする (旧経路の時計・フェード・IK・ルートモーションを外す)
void ResetPoseInputs(SkinnedMeshComponent& sm)
{
    sm.playing = false;
    sm.fadeTotal = 0;
    sm.fadeElapsed = 0;
    sm.poseClaim = 0;
    sm.poseLayerCount = 0;
    sm.poseRootJoint = -1;
    sm.poseRootYaw = 0;
    sm.poseIkCount = 0;
    sm.poseIkPelvisJoint = -1;
}

// ステートモードで選べるステート = 骨を駆動するもの
bool Selectable(const ControllerState& state)
{
    return StateDrivesSkeleton(state);
}

} // namespace

void AnimationPreviewWindow::ReleaseGpu()
{
    rt_.Release();
    render_.ReleaseGpu();
}

void AnimationPreviewWindow::SetTick(int32_t tick)
{
    tick_ = std::max(tick, 0);
    playing_ = false;
    playAccum_ = 0.0f;
}

XMMATRIX AnimationPreviewWindow::ViewMatrix() const
{
    const XMVECTOR center = XMLoadFloat3(&center_);
    const XMVECTOR dir = XMVectorSet(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), -std::cos(pitch_) * std::cos(yaw_), 0.0f);
    const XMVECTOR eye = XMVectorAdd(center, XMVectorScale(dir, distance_));
    return XMMatrixLookAtLH(eye, center, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
}

void AnimationPreviewWindow::Rebuild(EngineContext& ctx, EntityID sourceRoot, const std::vector<EntityID>& driven)
{
    scene_.Clear();
    meshes_.clear();
    mainMesh_ = -1;
    World& src = ctx.scene->GetWorld();
    World& dst = scene_.GetWorld();
    // 元のキャラの根の空間へ写す (シーンのどこに居ても原点に立つ)
    const XMMATRIX rootInv = XMMatrixInverse(nullptr, WorldOf(src, sourceRoot));
    uint32_t mainIndex = UINT32_MAX;
    for (EntityID m : driven) {
        const SkinnedMeshComponent* sm = src.GetComponent<SkinnedMeshComponent>(m);
        if (sm == nullptr) {
            continue;
        }
        GameObject copy = scene_.CreateGameObject(src.GetName(m));
        XMVECTOR scale, rotation, translation;
        if (!XMMatrixDecompose(&scale, &rotation, &translation, XMMatrixMultiply(WorldOf(src, m), rootInv))) {
            scale = XMVectorSplatOne();
            rotation = XMQuaternionIdentity();
            translation = XMVectorZero();
        }
        LocalTransform* lt = copy.GetComponent<LocalTransform>();
        XMStoreFloat3(&lt->scale, scale);
        XMStoreFloat4(&lt->rotation, rotation);
        XMStoreFloat3(&lt->position, translation);
        SkinnedMeshComponent* csm = copy.AddComponent<SkinnedMeshComponent>();
        *csm = *sm;
        ResetPoseInputs(*csm);
        if (const MeshRendererComponent* mr = src.GetComponent<MeshRendererComponent>(m)) {
            *copy.AddComponent<MeshRendererComponent>() = *mr;
        }
        if (m.index < mainIndex) {
            mainIndex = m.index;
            mainMesh_ = static_cast<int32_t>(meshes_.size());
        }
        meshes_.push_back(copy.Id());
    }
    GameObject sun = scene_.CreateGameObject("preview_sun");
    sun.AddComponent<LightComponent>();
    XMStoreFloat4(&sun.GetComponent<LocalTransform>()->rotation, XMQuaternionRotationRollPitchYaw(0.7f, 0.5f, 0.0f));
    dst.ApplyStructuralChanges();
    transforms_.Update(dst);
    built_ = !meshes_.empty();
}

void AnimationPreviewWindow::WritePose(const SkinnedModelLibrary& models)
{
    World& world = scene_.GetWorld();
    const int32_t count = std::min(static_cast<int32_t>(sources_.size()), SkinnedMeshComponent::kMaxPoseLayers);
    // ブレンドツリーの位相 (1 周 = 2^32)。非ループの末尾は UINT32_MAX = 末尾に張り付いた位置 (エンジンと同じ)
    uint32_t phase = 0;
    if (lengthTicks_ > 0) {
        phase = tick_ >= lengthTicks_ ? UINT32_MAX
                                      : static_cast<uint32_t>((static_cast<uint64_t>(tick_) << 32) / static_cast<uint64_t>(lengthTicks_));
    }
    for (EntityID e : meshes_) {
        SkinnedMeshComponent* sm = world.GetComponent<SkinnedMeshComponent>(e);
        if (sm == nullptr) {
            continue;
        }
        const SkinnedModel* model = models.Get(sm->model);
        ResetPoseInputs(*sm);
        sm->poseLayerCount = count;
        for (int32_t i = 0; i < SkinnedMeshComponent::kMaxPoseLayers; ++i) {
            sm->poseLayers[i] = {};
        }
        for (int32_t i = 0; i < count; ++i) {
            const Source& s = sources_[static_cast<size_t>(i)];
            SkinnedMeshComponent::PoseLayer& layer = sm->poseLayers[i];
            layer.clip = model != nullptr ? model->FindClipByHash(s.clipHash) : -1;
            if (s.usesPhase) {
                const int32_t ticks =
                    layer.clip >= 0 ? SkeletalClipTicks(model->clips[static_cast<size_t>(layer.clip)]) : 0;
                layer.timeQ = BlendPhaseToTimeQ(phase, ticks, loop_);
            } else {
                layer.timeQ = tick_ * kQ;
            }
            layer.prevTimeQ = layer.timeQ;
            layer.stepQ = 0;
            layer.weightQ = s.weightQ;
        }
    }
}

void AnimationPreviewWindow::FitCamera(const SkinnedModelLibrary& models)
{
    // 今のポーズの関節の位置の外接箱に合わせる (メッシュの頂点は見ない。骨が体の中に収まっている前提)
    World& world = scene_.GetWorld();
    XMVECTOR lo = XMVectorReplicate(1e30f);
    XMVECTOR hi = XMVectorReplicate(-1e30f);
    bool any = false;
    for (EntityID e : meshes_) {
        const SkinnedMeshComponent* sm = world.GetComponent<SkinnedMeshComponent>(e);
        const SkinnedModel* model = sm != nullptr ? models.Get(sm->model) : nullptr;
        if (model == nullptr) {
            continue;
        }
        SampleSkinnedLocals(*model, *sm, locals_);
        const XMMATRIX w = WorldOf(world, e);
        for (size_t j = 0; j < model->joints.size(); ++j) {
            const XMVECTOR p = XMMatrixMultiply(JointGlobalFromLocals(*model, locals_, static_cast<int32_t>(j)), w).r[3];
            lo = XMVectorMin(lo, p);
            hi = XMVectorMax(hi, p);
            any = true;
        }
    }
    if (!any) {
        return;
    }
    XMStoreFloat3(&center_, XMVectorScale(XMVectorAdd(lo, hi), 0.5f));
    const float radius = std::max(XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo))) * 0.5f, 0.1f);
    distance_ = radius / std::sin(kFovYDeg * 0.5f * kPi / 180.0f) * 1.2f;
}

void AnimationPreviewWindow::OnImGui(EngineContext& ctx, Selection& selection)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_AnimPreview), &open)) {
        ImGui::End();
        return;
    }
    if (ctx.scene == nullptr || ctx.resources == nullptr) {
        ImGui::End();
        return;
    }
    const SkinnedModelLibrary& models = ctx.resources->skinnedModels;
    World& world = ctx.scene->GetWorld();
    GameObject go = ctx.scene->FindByFileId(selection.primary);
    std::vector<EntityID> driven;
    if (go) {
        CollectDrivenSkinnedMeshes(world, go.Id(), driven);
    }
    if (driven.empty()) {
        built_ = false;
        builtFor_ = 0;
        ImGui::TextWrapped("%s", Tr(StrId::AnimPrev_NoTarget));
        ImGui::End();
        return;
    }
    // 選択が変わった・駆動する SkinnedMesh の数が変わった・押された、で写し直す (元のシーンの編集は自動では追わない)
    const bool reload = ImGui::Button(Tr(StrId::AnimPrev_Reload));
    ImGui::SameLine();
    if (reload || !built_ || builtFor_ != selection.primary || builtCount_ != driven.size()) {
        Rebuild(ctx, go.Id(), driven);
        builtFor_ = selection.primary;
        builtCount_ = driven.size();
        WritePose(models);
        FitCamera(models);
    }
    if (!built_) {
        ImGui::End();
        return;
    }
    World& previewWorld = scene_.GetWorld();
    const SkinnedMeshComponent* mainSm = previewWorld.GetComponent<SkinnedMeshComponent>(meshes_[static_cast<size_t>(mainMesh_)]);
    const SkinnedModel* mainModel = mainSm != nullptr ? models.Get(mainSm->model) : nullptr;

    // ---- 何を見せるか (クリップ / ステート) ----
    const AnimatorControllerComponent* comp = go.GetComponent<AnimatorControllerComponent>();
    const ControllerAsset* ctrl =
        (comp != nullptr && ctx.controllers != nullptr) ? ctx.controllers->Get(comp->controller.value) : nullptr;
    int32_t mode = static_cast<int32_t>(mode_);
    ImGui::RadioButton(Tr(StrId::AnimPrev_ModeClip), &mode, static_cast<int32_t>(Mode::Clip));
    if (ctrl != nullptr) {
        ImGui::SameLine();
        ImGui::RadioButton(Tr(StrId::AnimPrev_ModeState), &mode, static_cast<int32_t>(Mode::State));
    } else {
        mode = static_cast<int32_t>(Mode::Clip);
    }
    mode_ = static_cast<Mode>(mode);
    ImGui::SameLine();
    ImGui::Checkbox(Tr(StrId::AnimPrev_Bones), &showBones_);

    sources_.clear();
    lengthTicks_ = 0;
    loop_ = 1;
    if (mode_ == Mode::Clip) {
        const std::vector<std::string> clips = NamedSkeletalClips(mainModel);
        if (clips.empty()) {
            ImGui::TextUnformatted(Tr(StrId::AnimPrev_NoClips));
        } else {
            if (std::find(clips.begin(), clips.end(), clipName_) == clips.end()) {
                clipName_ = clips.front();
            }
            if (ImGui::BeginCombo(Tr(StrId::AnimPrev_Clip), clipName_.c_str())) {
                for (const std::string& c : clips) {
                    if (ImGui::Selectable(c.c_str(), c == clipName_)) {
                        clipName_ = c;
                        tick_ = 0;
                    }
                }
                ImGui::EndCombo();
            }
            const uint64_t hash = HashStr(clipName_);
            const int32_t clip = mainModel->FindClipByHash(hash);
            lengthTicks_ = clip >= 0 ? SkeletalClipTicks(mainModel->clips[static_cast<size_t>(clip)]) : 0;
            sources_.push_back({ hash, false, kWeightOne });
        }
    } else {
        if (stateIndex_ < 0 || stateIndex_ >= static_cast<int32_t>(ctrl->states.size())
            || !Selectable(ctrl->states[static_cast<size_t>(stateIndex_)])) {
            stateIndex_ = -1;
            for (size_t i = 0; i < ctrl->states.size(); ++i) {
                if (Selectable(ctrl->states[i])) {
                    stateIndex_ = static_cast<int32_t>(i);
                    break;
                }
            }
        }
        if (stateIndex_ < 0) {
            ImGui::TextUnformatted(Tr(StrId::AnimPrev_NoStates));
        } else {
            if (ImGui::BeginCombo(Tr(StrId::AnimPrev_State), ctrl->states[static_cast<size_t>(stateIndex_)].name.c_str())) {
                for (size_t i = 0; i < ctrl->states.size(); ++i) {
                    if (Selectable(ctrl->states[i])
                        && ImGui::Selectable(ctrl->states[i].name.c_str(), static_cast<int32_t>(i) == stateIndex_)) {
                        stateIndex_ = static_cast<int32_t>(i);
                        tick_ = 0;
                    }
                }
                ImGui::EndCombo();
            }
            const ControllerState& state = ctrl->states[static_cast<size_t>(stateIndex_)];
            loop_ = state.loop;
            // パラメータは窓の値で上書きした写し (シーンのコンポーネントは書き換えない)
            int32_t params[AnimatorControllerComponent::kMaxParams] = {};
            std::copy(std::begin(comp->params), std::end(comp->params), std::begin(params));
            if (state.blendType == ControllerBlendType::None) {
                sources_.push_back({ state.skelClipHash, false, kWeightOne });
            } else {
                // スライダの範囲は子の閾値 / 位置の範囲
                float loX = 0.0f, hiX = 0.0f, loY = 0.0f, hiY = 0.0f;
                for (size_t i = 0; i < state.blendChildren.size(); ++i) {
                    const ControllerBlendChild& c = state.blendChildren[i];
                    const float x = state.blendType == ControllerBlendType::Blend1D ? c.threshold : c.posX;
                    loX = i == 0 ? x : std::min(loX, x);
                    hiX = i == 0 ? x : std::max(hiX, x);
                    loY = i == 0 ? c.posY : std::min(loY, c.posY);
                    hiY = i == 0 ? c.posY : std::max(hiY, c.posY);
                }
                ImGui::SliderFloat(Tr(StrId::AnimPrev_ParamX), &paramX_, loX, hiX > loX ? hiX : loX + 1.0f);
                if (state.blendType == ControllerBlendType::Blend2D) {
                    ImGui::SliderFloat(Tr(StrId::AnimPrev_ParamY), &paramY_, loY, hiY > loY ? hiY : loY + 1.0f);
                }
                if (state.blendParam >= 0 && state.blendParam < AnimatorControllerComponent::kMaxParams) {
                    params[state.blendParam] = std::bit_cast<int32_t>(paramX_);
                }
                if (state.blendParamY >= 0 && state.blendParamY < AnimatorControllerComponent::kMaxParams
                    && state.blendType == ControllerBlendType::Blend2D) {
                    params[state.blendParamY] = std::bit_cast<int32_t>(paramY_);
                }
                BlendChildWeight w[kMaxBlendLayers];
                const int32_t n = ComputeBlendWeights(state, paramX_, paramY_, w);
                for (int32_t i = 0; i < n; ++i) {
                    sources_.push_back({ state.blendChildren[static_cast<size_t>(w[i].child)].clipHash, true, w[i].weightQ });
                }
            }
            lengthTicks_ = ControllerStateLengthTicks(*ctrl, state, params, ctx.anims, mainModel);
        }
    }

    // ---- 時刻 (再生・スクラブ・±1 tick) ----
    const int32_t lastTick = loop_ != 0 ? std::max(lengthTicks_ - 1, 0) : lengthTicks_;
    if (playing_ && lengthTicks_ > 0) {
        playAccum_ += ImGui::GetIO().DeltaTime * kTicksPerSecond;
        const int32_t steps = static_cast<int32_t>(playAccum_);
        playAccum_ -= static_cast<float>(steps);
        tick_ += steps;
        if (loop_ != 0) {
            tick_ %= lengthTicks_;
        }
    }
    tick_ = std::clamp(tick_, 0, lastTick);
    if (ImGui::Button(playing_ ? Tr(StrId::AnimPrev_Pause) : Tr(StrId::AnimPrev_Play))) {
        playing_ = !playing_;
        playAccum_ = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("|<")) {
        tick_ = 0;
        playing_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("-1")) {
        tick_ = loop_ != 0 && tick_ == 0 ? lastTick : std::max(tick_ - 1, 0);
        playing_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("+1")) {
        tick_ = loop_ != 0 && tick_ >= lastTick ? 0 : std::min(tick_ + 1, lastTick);
        playing_ = false;
    }
    ImGui::SameLine();
    ImGui::Text("%d / %d tick", tick_, lengthTicks_);
    DrawTimeline(ctrl, mainModel);

    WritePose(models);

    // ---- 画像 (ドラッグで回す、ホイールで寄る) ----
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x >= kMinImageSize && avail.y >= kMinImageSize) {
        width_ = static_cast<int32_t>(avail.x);
        height_ = static_cast<int32_t>(avail.y);
        const ImVec2 rectMin = ImGui::GetCursorScreenPos();
        if (rt_.IsValid() && rt_.SRV() != nullptr) {
            ImGui::Image(reinterpret_cast<ImTextureID>(rt_.SRV()), avail);
        } else {
            ImGui::Dummy(avail);
        }
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                yaw_ += d.x * 0.01f;
                pitch_ = std::clamp(pitch_ + d.y * 0.01f, -1.4f, 1.4f);
            }
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                distance_ = std::max(distance_ * (wheel > 0.0f ? 0.9f : 1.1f), 0.05f);
            }
            ImGui::SetTooltip("%s", Tr(StrId::AnimPrev_Hint));
        }
        if (showBones_) {
            DrawBones(models, { rectMin.x, rectMin.y }, { avail.x, avail.y });
        }
    } else {
        width_ = 0;
        height_ = 0;
    }
    ImGui::End();
}

void AnimationPreviewWindow::DrawTimeline(const ControllerAsset* ctrl, const SkinnedModel* mainModel)
{
    const int32_t lastTick = loop_ != 0 ? std::max(lengthTicks_ - 1, 0) : lengthTicks_;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##anim_preview_tick", &tick_, 0, std::max(lastTick, 0))) {
        playing_ = false;
    }
    // イベントの印: 最も重い層のクリップのイベントを、そのクリップの長さの割合でタイムラインへ置く
    // (ブレンドツリーの子は位相を共有するので、割合で揃えれば全員の位置と一致する)
    if (ctrl == nullptr || mainModel == nullptr || sources_.empty() || lengthTicks_ <= 0) {
        return;
    }
    const auto heaviest = std::max_element(sources_.begin(), sources_.end(),
                                           [](const Source& a, const Source& b) { return a.weightQ < b.weightQ; });
    const ControllerClipEvents* events = nullptr;
    for (const ControllerClipEvents& ce : ctrl->clipEvents) {
        if (ce.clipHash == heaviest->clipHash) {
            events = &ce;
            break;
        }
    }
    const int32_t clip = mainModel->FindClipByHash(heaviest->clipHash);
    const int32_t clipTicks = clip >= 0 ? SkeletalClipTicks(mainModel->clips[static_cast<size_t>(clip)]) : 0;
    if (events == nullptr || clipTicks <= 0) {
        return;
    }
    // スライダの枠 (直前の項目) の上に、つまみの可動範囲に合わせて印を打つ
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    const float grab = ImGui::GetStyle().GrabMinSize * 0.5f + ImGui::GetStyle().FramePadding.x;
    const float x0 = lo.x + grab;
    const float x1 = hi.x - grab;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float span = static_cast<float>(std::max(lastTick, 1));
    for (const ControllerClipEvent& ev : events->events) {
        const float atTick = static_cast<float>(ev.tick) / static_cast<float>(clipTicks) * static_cast<float>(lengthTicks_);
        const float x = x0 + (x1 - x0) * std::clamp(atTick / span, 0.0f, 1.0f);
        dl->AddTriangleFilled({ x - 4.0f, hi.y + 6.0f }, { x + 4.0f, hi.y + 6.0f }, { x, hi.y }, kEventColor);
        if (ImGui::IsMouseHoveringRect({ x - 5.0f, lo.y }, { x + 5.0f, hi.y + 7.0f })) {
            ImGui::SetTooltip("%s (%d tick)", ev.name.c_str(), ev.tick);
        }
    }
    ImGui::Dummy({ 1.0f, 6.0f });
}

void AnimationPreviewWindow::DrawBones(const SkinnedModelLibrary& models, const XMFLOAT2& rectMin, const XMFLOAT2& rectSize)
{
    // 描画と同じカメラ (透視の x, y は深度の向きに依らない) で関節を画面へ写し、メッシュの上に線で重ねる
    World& world = scene_.GetWorld();
    transforms_.Update(world);
    const float aspect = rectSize.x / rectSize.y;
    const XMMATRIX viewProj = XMMatrixMultiply(
        ViewMatrix(), XMMatrixPerspectiveFovLH(kFovYDeg * kPi / 180.0f, aspect, 0.01f, 1000.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect({ rectMin.x, rectMin.y }, { rectMin.x + rectSize.x, rectMin.y + rectSize.y }, true);
    const auto toScreen = [&](FXMVECTOR world3, ImVec2& out) {
        const XMVECTOR clip = XMVector4Transform(XMVectorSetW(world3, 1.0f), viewProj);
        const float w = XMVectorGetW(clip);
        if (w <= 1e-4f) {
            return false; // カメラの後ろ
        }
        out.x = rectMin.x + (XMVectorGetX(clip) / w * 0.5f + 0.5f) * rectSize.x;
        out.y = rectMin.y + (0.5f - XMVectorGetY(clip) / w * 0.5f) * rectSize.y;
        return true;
    };
    for (EntityID e : meshes_) {
        const SkinnedMeshComponent* sm = world.GetComponent<SkinnedMeshComponent>(e);
        const SkinnedModel* model = sm != nullptr ? models.Get(sm->model) : nullptr;
        if (model == nullptr) {
            continue;
        }
        SampleSkinnedLocals(*model, *sm, locals_);
        const XMMATRIX entityWorld = WorldOf(world, e);
        for (size_t j = 0; j < model->joints.size(); ++j) {
            const int32_t parent = model->joints[j].parent;
            ImVec2 a, b;
            const XMVECTOR pj = XMMatrixMultiply(JointGlobalFromLocals(*model, locals_, static_cast<int32_t>(j)), entityWorld).r[3];
            if (!toScreen(pj, a)) {
                continue;
            }
            dl->AddCircleFilled(a, 2.5f, kBoneColor);
            if (parent < 0) {
                continue;
            }
            const XMVECTOR pp = XMMatrixMultiply(JointGlobalFromLocals(*model, locals_, parent), entityWorld).r[3];
            if (toScreen(pp, b)) {
                dl->AddLine(a, b, kBoneColor, 1.5f);
            }
        }
    }
    dl->PopClipRect();
}

void AnimationPreviewWindow::OnRenderViews(EngineContext& ctx)
{
    if (!open || !built_ || width_ <= 0 || height_ <= 0 || ctx.device == nullptr) {
        return;
    }
    // ポストプロセスを通さないので、sRGB への変換は RT の形式に任せる (AssetPreviewCache と同じ)
    rt_.Resize(*ctx.device, width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    if (!rt_.IsValid()) {
        return;
    }
    World& world = scene_.GetWorld();
    transforms_.Update(world);
    CameraOverride cam;
    const XMMATRIX view = ViewMatrix();
    XMStoreFloat4x4(&cam.view, view);
    XMStoreFloat3(&cam.position, XMMatrixInverse(nullptr, view).r[3]);
    cam.fovYDeg = kFovYDeg;
    cam.nearZ = 0.01f;
    cam.farZ = 1000.0f;
    render_.enablePostFx = false;
    render_.enableShadows = false;
    FrameTarget target;
    target.rtv = rt_.RTV();
    target.dsv = rt_.DSV();
    target.width = rt_.Width();
    target.height = rt_.Height();
    target.clearColor[0] = 0.0152f;
    target.clearColor[1] = 0.0152f;
    target.clearColor[2] = 0.0194f;
    target.clearColor[3] = 1.0f;
    render_.Render(world, *ctx.device, *ctx.renderPathForward, *ctx.shaders, *ctx.resources, target, &cam, nullptr);
}

} // namespace mye
