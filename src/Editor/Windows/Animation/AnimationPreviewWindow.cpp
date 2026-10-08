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

#include "Editor/App/DiskCompare.h"
#include "Editor/Scene/Selection.h"
#include "Editor/SourceControl/ScmHint.h"
#include "Editor/Windows/Animation/ClipEventEdit.h"
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
constexpr ImU32 kEventSelectedColor = IM_COL32(255, 140, 70, 255);
constexpr ImU32 kEventLaneColor = IM_COL32(255, 255, 255, 18);
constexpr ImU32 kTrailColor = IM_COL32(120, 255, 140, 255);
// イベントの帯の高さと、印をつかめる距離 (px)
constexpr float kEventLaneHeight = 14.0f;
constexpr float kEventPickRadius = 6.0f;
// イベントの種類 (.controller.json の "kind" と同じ綴り。ClipEventKind の並び)
const char* kEventKinds[] = { "script", "sound", "effect", "noise" };

XMMATRIX WorldOf(World& world, EntityID e)
{
    const WorldMatrixComponent* wm = world.GetComponent<WorldMatrixComponent>(e);
    return wm != nullptr ? XMLoadFloat4x4(&wm->value) : XMMatrixIdentity();
}

// 画像の矩形へ透視投影する。カメラの後ろなら false
bool ToScreen(const XMMATRIX& viewProj, FXMVECTOR p, const XMFLOAT2& rectMin, const XMFLOAT2& rectSize, ImVec2& out)
{
    const XMVECTOR clip = XMVector4Transform(XMVectorSetW(p, 1.0f), viewProj);
    const float w = XMVectorGetW(clip);
    if (w <= 1e-4f) {
        return false;
    }
    out.x = rectMin.x + (XMVectorGetX(clip) / w * 0.5f + 0.5f) * rectSize.x;
    out.y = rectMin.y + (0.5f - XMVectorGetY(clip) / w * 0.5f) * rectSize.y;
    return true;
}

// std::string を ImGui::InputText で編集する (固定長の作業領域を経由。長さ制限はアセットのキーに十分な 128)
bool InputString(const char* label, std::string& value)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    if (!ImGui::InputText(label, buf, sizeof(buf))) {
        return false;
    }
    value = buf;
    return true;
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

bool AnimationPreviewWindow::HasUnsavedChanges() const
{
    if (controllers_ == nullptr) {
        return false;
    }
    for (const uint64_t hash : touched_) {
        const ControllerAsset* c = controllers_->Get(hash);
        if (c != nullptr && TextDiffersFromDisk(c->path, ControllerLibrary::ToJson(*c).dump(2))) {
            return true;
        }
    }
    return false;
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

void AnimationPreviewWindow::WritePose(const SkinnedModelLibrary& models, int32_t tick)
{
    World& world = scene_.GetWorld();
    const int32_t count = std::min(static_cast<int32_t>(sources_.size()), SkinnedMeshComponent::kMaxPoseLayers);
    // ブレンドツリーの位相 (1 周 = 2^32)。非ループの末尾は UINT32_MAX = 末尾に張り付いた位置 (エンジンと同じ)
    uint32_t phase = 0;
    if (lengthTicks_ > 0) {
        phase = tick >= lengthTicks_ ? UINT32_MAX
                                     : static_cast<uint32_t>((static_cast<uint64_t>(tick) << 32) / static_cast<uint64_t>(lengthTicks_));
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
                layer.timeQ = tick * kQ;
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
        WritePose(models, tick_);
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
    // イベントを書き換えるので可変で引く (AnimatorControllerWindow と同じく、保存までは共有のアセットを直接編集する)
    ControllerAsset* ctrl =
        (comp != nullptr && ctx.controllers != nullptr) ? ctx.controllers->GetMutable(comp->controller.value) : nullptr;
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
    ImGui::SameLine();
    ImGui::Checkbox(Tr(StrId::AnimPrev_Trail), &showTrail_);

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
            sources_.push_back({ clipName_, hash, false, kWeightOne });
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
                sources_.push_back({ state.skelClip, state.skelClipHash, false, kWeightOne });
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
                    const ControllerBlendChild& child = state.blendChildren[static_cast<size_t>(w[i].child)];
                    sources_.push_back({ child.clip, child.clipHash, true, w[i].weightQ });
                }
            }
            lengthTicks_ = ControllerStateLengthTicks(*ctrl, state, params, ctx.anims, mainModel);
        }
    }

    // ---- 時刻 (再生・スクラブ・±1 tick) ----
    const int32_t lastTick = LastTick();
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
    DrawTimeline(ctx.controllers, ctrl, mainModel);

    ComputeRootTrail(models, mainModel);
    WritePose(models, tick_);

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
        DrawOverlay(models, { rectMin.x, rectMin.y }, { avail.x, avail.y });
    } else {
        width_ = 0;
        height_ = 0;
    }
    ImGui::End();
}

int32_t AnimationPreviewWindow::LastTick() const
{
    // ループは長さちょうどが 0 と同じ位置なので 1 つ手前まで、非ループは末尾の tick まで
    return loop_ != 0 ? std::max(lengthTicks_ - 1, 0) : lengthTicks_;
}

void AnimationPreviewWindow::DrawTimeline(ControllerLibrary* controllers, ControllerAsset* ctrl, const SkinnedModel* mainModel)
{
    const int32_t lastTick = LastTick();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##anim_preview_tick", &tick_, 0, std::max(lastTick, 0))) {
        playing_ = false;
    }
    const ImVec2 sliderLo = ImGui::GetItemRectMin();
    const ImVec2 sliderHi = ImGui::GetItemRectMax();
    eventTimelineTicks_.clear();
    // イベントは最も重い層のクリップのものを、そのクリップの長さの割合でタイムラインへ置く
    // (ブレンドツリーの子は位相を共有するので、割合で揃えれば全員の位置と一致する)
    const Source* heaviest = nullptr;
    int32_t clipTicks = 0;
    if (ctrl != nullptr && mainModel != nullptr && !sources_.empty() && lengthTicks_ > 0) {
        heaviest = &*std::max_element(sources_.begin(), sources_.end(),
                                      [](const Source& a, const Source& b) { return a.weightQ < b.weightQ; });
        const int32_t clip = mainModel->FindClipByHash(heaviest->clipHash);
        clipTicks = clip >= 0 ? SkeletalClipTicks(mainModel->clips[static_cast<size_t>(clip)]) : 0;
    }
    if (heaviest == nullptr || heaviest->clip.empty() || clipTicks <= 0) {
        selEvent_ = -1;
        dragging_ = false;
        return;
    }
    if (selClipHash_ != heaviest->clipHash) {
        selClipHash_ = heaviest->clipHash;
        selEvent_ = -1;
        dragging_ = false;
    }
    controllers_ = controllers;
    const auto touch = [&]() {
        if (std::find(touched_.begin(), touched_.end(), ctrl->hash) == touched_.end()) {
            touched_.push_back(ctrl->hash);
        }
    };

    // ---- イベントの帯 (スライダのつまみの可動範囲と同じ横位置で印を打つ) ----
    const float grab = ImGui::GetStyle().GrabMinSize * 0.5f + ImGui::GetStyle().FramePadding.x;
    const float x0 = sliderLo.x + grab;
    const float x1 = sliderHi.x - grab;
    const float span = static_cast<float>(std::max(lastTick, 1));
    const auto eventX = [&](int32_t eventTick) {
        const float atTick = static_cast<float>(eventTick) / static_cast<float>(clipTicks) * static_cast<float>(lengthTicks_);
        return x0 + (x1 - x0) * std::clamp(atTick / span, 0.0f, 1.0f);
    };
    const auto mouseClipTick = [&]() {
        const float f = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(x1 - x0, 1.0f), 0.0f, 1.0f);
        return TimelineTickToClipTick(static_cast<int32_t>(std::lround(f * span)), lengthTicks_, clipTicks);
    };
    ImGui::InvisibleButton("##anim_preview_events", { std::max(sliderHi.x - sliderLo.x, 1.0f), kEventLaneHeight });
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    const bool laneHovered = ImGui::IsItemHovered();
    ControllerClipEvents* events = FindClipEventsMutable(*ctrl, heaviest->clipHash);
    const int32_t eventCount = events != nullptr ? static_cast<int32_t>(events->events.size()) : 0;
    if (selEvent_ >= eventCount) {
        selEvent_ = -1;
        dragging_ = false;
    }
    int32_t hovered = -1;
    float hoveredDist = kEventPickRadius;
    for (int32_t i = 0; i < eventCount; ++i) {
        const float x = eventX(events->events[static_cast<size_t>(i)].tick);
        const float dist = std::fabs(ImGui::GetIO().MousePos.x - x);
        if (laneHovered && dist <= hoveredDist) {
            hovered = i;
            hoveredDist = dist;
        }
    }
    // 押した: 印の上ならそれを選んでつかむ、空いた所なら選択を外す。ダブルクリックで空いた所に足す
    if (ImGui::IsItemActivated()) {
        selEvent_ = hovered;
        dragging_ = hovered >= 0;
        if (hovered < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            selEvent_ = AddClipEvent(*ctrl, heaviest->clip, mouseClipTick());
            touch();
            events = FindClipEventsMutable(*ctrl, heaviest->clipHash);
        }
    }
    if (dragging_ && ImGui::IsItemActive() && events != nullptr && selEvent_ >= 0) {
        ControllerClipEvent& ev = events->events[static_cast<size_t>(selEvent_)];
        const int32_t t = mouseClipTick();
        if (t != ev.tick) {
            ev.tick = t;
            touch();
        }
    }
    if (!ImGui::IsItemActive()) {
        dragging_ = false;
    }
    if (const int32_t tipEvent = dragging_ ? selEvent_ : hovered; tipEvent >= 0 && events != nullptr) {
        const ControllerClipEvent& ev = events->events[static_cast<size_t>(tipEvent)];
        ImGui::SetTooltip("%s (%d tick)", ev.name.c_str(), ev.tick);
    } else if (laneHovered) {
        ImGui::SetTooltip("%s", Tr(StrId::AnimPrev_EventLaneHint));
    }

    // ---- 対象のクリップと操作 ----
    ImGui::Text("%s %s", Tr(StrId::AnimPrev_EventsOf), heaviest->clip.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(StrId::AnimPrev_AddEvent))) {
        selEvent_ = AddClipEvent(*ctrl, heaviest->clip, TimelineTickToClipTick(tick_, lengthTicks_, clipTicks));
        touch();
        events = FindClipEventsMutable(*ctrl, heaviest->clipHash);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selEvent_ < 0);
    if (ImGui::SmallButton(Tr(StrId::AnimPrev_DeleteEvent)) && selEvent_ >= 0) {
        RemoveClipEvent(*ctrl, heaviest->clipHash, selEvent_);
        selEvent_ = -1;
        touch();
        events = FindClipEventsMutable(*ctrl, heaviest->clipHash);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(StrId::Common_Save)) && controllers != nullptr) {
        if (controllers->SaveToFile(ctrl->hash)) {
            scmhint::Changed(ctrl->path); // バッジと Changes の即時反映 (AnimatorControllerWindow と同じ)
        }
    }
    if (events != nullptr && selEvent_ >= 0 && selEvent_ < static_cast<int32_t>(events->events.size())) {
        if (DrawEventFields(events->events[static_cast<size_t>(selEvent_)], clipTicks, mainModel)) {
            touch();
        }
    }

    // ---- 印 (編集を反映した後の位置で打つ) と、軌跡に重ねる位置 ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, kEventLaneColor);
    const int32_t finalCount = events != nullptr ? static_cast<int32_t>(events->events.size()) : 0;
    for (int32_t i = 0; i < finalCount; ++i) {
        const int32_t eventTick = events->events[static_cast<size_t>(i)].tick;
        const float x = eventX(eventTick);
        const bool selected = i == selEvent_;
        const float half = selected ? 5.0f : 4.0f;
        dl->AddTriangleFilled({ x - half, hi.y }, { x + half, hi.y }, { x, lo.y + 1.0f },
                              selected ? kEventSelectedColor : kEventColor);
        eventTimelineTicks_.push_back(
            static_cast<int32_t>(std::lround(static_cast<double>(eventTick) * lengthTicks_ / clipTicks)));
    }
}

bool AnimationPreviewWindow::DrawEventFields(ControllerClipEvent& ev, int32_t clipTicks, const SkinnedModel* mainModel)
{
    bool changed = false;
    ImGui::PushID("anim_preview_event");
    std::string name = ev.name;
    if (InputString(Tr(StrId::AnimPrev_EvName), name)) {
        SetClipEventName(ev, name);
        changed = true;
    }
    int32_t tick = ev.tick;
    if (ImGui::SliderInt(Tr(StrId::AnimPrev_EvTick), &tick, 0, clipTicks)) {
        ev.tick = std::clamp(tick, 0, clipTicks);
        changed = true;
    }
    int kind = static_cast<int>(ev.kind);
    if (ImGui::Combo(Tr(StrId::AnimPrev_EvKind), &kind, kEventKinds, IM_ARRAYSIZE(kEventKinds))) {
        SetClipEventKind(ev, static_cast<ClipEventKind>(kind));
        changed = true;
    }
    changed |= ImGui::SliderFloat(Tr(StrId::AnimPrev_EvMinWeight), &ev.minWeight, 0.0f, 1.0f);
    // 位置を取るジョイント (空 = エンティティの位置)。候補は主 SkinnedMesh のモデル (エンジンが引くのと同じ)
    if (ImGui::BeginCombo(Tr(StrId::AnimPrev_EvJoint), ev.joint.empty() ? Tr(StrId::AnimPrev_EvJointNone) : ev.joint.c_str())) {
        if (ImGui::Selectable(Tr(StrId::AnimPrev_EvJointNone), ev.joint.empty())) {
            ev.joint.clear();
            changed = true;
        }
        if (mainModel != nullptr) {
            for (size_t j = 0; j < mainModel->joints.size(); ++j) {
                const std::string& jointName = mainModel->joints[j].name;
                ImGui::PushID(static_cast<int>(j));
                if (!jointName.empty() && ImGui::Selectable(jointName.c_str(), jointName == ev.joint)) {
                    ev.joint = jointName;
                    changed = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    switch (ev.kind) {
    case ClipEventKind::Script:
        changed |= ImGui::DragFloat(Tr(StrId::AnimPrev_EvValue), &ev.value, 0.01f);
        changed |= ImGui::InputInt(Tr(StrId::AnimPrev_EvInt), &ev.intValue);
        break;
    case ClipEventKind::Sound: {
        std::string asset = ev.asset;
        if (InputString(Tr(StrId::AnimPrev_EvSound), asset)) {
            SetClipEventAsset(ev, asset);
            changed = true;
        }
        changed |= ImGui::DragFloat(Tr(StrId::AnimPrev_EvVolume), &ev.volume, 0.01f, 0.0f, 4.0f);
        changed |= ImGui::DragFloat(Tr(StrId::AnimPrev_EvPitch), &ev.pitch, 0.01f, 0.1f, 4.0f);
        break;
    }
    case ClipEventKind::Effect: {
        std::string asset = ev.asset;
        if (InputString(Tr(StrId::AnimPrev_EvPrefab), asset)) {
            SetClipEventAsset(ev, asset);
            changed = true;
        }
        break;
    }
    case ClipEventKind::Noise:
        changed |= ImGui::DragFloat(Tr(StrId::AnimPrev_EvLoudness), &ev.loudness, 0.01f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat(Tr(StrId::AnimPrev_EvRange), &ev.range, 0.1f, 0.0f, 1000.0f);
        break;
    }
    ImGui::PopID();
    return changed;
}

void AnimationPreviewWindow::ComputeRootTrail(const SkinnedModelLibrary& models, const SkinnedModel* mainModel)
{
    trail_.clear();
    if (!showTrail_ || mainModel == nullptr || lengthTicks_ <= 0 || mainMesh_ < 0) {
        return;
    }
    // エンジンのルートモーションが測るのと同じジョイント (親の無い最初のジョイント)
    const int32_t root = FindRootJoint(*mainModel);
    if (root < 0) {
        return;
    }
    World& world = scene_.GetWorld();
    const EntityID mainId = meshes_[static_cast<size_t>(mainMesh_)];
    const XMMATRIX entityWorld = WorldOf(world, mainId);
    // 1 周の各 tick のポーズを書いてサンプルする (呼び出し側が直後に今の tick で書き直す)
    const int32_t lastTick = LastTick();
    trail_.reserve(static_cast<size_t>(lastTick) + 1);
    for (int32_t t = 0; t <= lastTick; ++t) {
        WritePose(models, t);
        const SkinnedMeshComponent* sm = world.GetComponent<SkinnedMeshComponent>(mainId);
        if (sm == nullptr) {
            trail_.clear();
            return;
        }
        SampleSkinnedLocals(*mainModel, *sm, locals_);
        XMFLOAT3 p;
        XMStoreFloat3(&p, XMMatrixMultiply(JointGlobalFromLocals(*mainModel, locals_, root), entityWorld).r[3]);
        trail_.push_back(p);
    }
}

void AnimationPreviewWindow::DrawOverlay(const SkinnedModelLibrary& models, const XMFLOAT2& rectMin, const XMFLOAT2& rectSize)
{
    // 描画と同じカメラ (透視の x, y は深度の向きに依らない) で画面へ写し、メッシュの上に線で重ねる
    World& world = scene_.GetWorld();
    transforms_.Update(world);
    const float aspect = rectSize.x / rectSize.y;
    const XMMATRIX viewProj = XMMatrixMultiply(
        ViewMatrix(), XMMatrixPerspectiveFovLH(kFovYDeg * kPi / 180.0f, aspect, 0.01f, 1000.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect({ rectMin.x, rectMin.y }, { rectMin.x + rectSize.x, rectMin.y + rectSize.y }, true);
    const auto toScreen = [&](FXMVECTOR p, ImVec2& out) { return ToScreen(viewProj, p, rectMin, rectSize, out); };

    // ---- ルートの軌跡: 線、イベントの位置、今の位置 ----
    if (showTrail_ && trail_.size() >= 2) {
        const auto point = [&](size_t i, ImVec2& out) { return toScreen(XMLoadFloat3(&trail_[i]), out); };
        for (size_t i = 1; i < trail_.size(); ++i) {
            ImVec2 a, b;
            if (point(i - 1, a) && point(i, b)) {
                dl->AddLine(a, b, kTrailColor, 1.5f);
            }
        }
        const size_t last = trail_.size() - 1;
        for (size_t i = 0; i < eventTimelineTicks_.size(); ++i) {
            ImVec2 a;
            const size_t at = std::min(static_cast<size_t>(std::max(eventTimelineTicks_[i], 0)), last);
            if (point(at, a)) {
                const bool selected = static_cast<int32_t>(i) == selEvent_;
                dl->AddCircleFilled(a, selected ? 5.0f : 4.0f, selected ? kEventSelectedColor : kEventColor);
            }
        }
        ImVec2 now;
        if (point(std::min(static_cast<size_t>(tick_), last), now)) {
            dl->AddCircle(now, 6.0f, kTrailColor, 0, 2.0f);
        }
    }

    if (!showBones_) {
        dl->PopClipRect();
        return;
    }
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
