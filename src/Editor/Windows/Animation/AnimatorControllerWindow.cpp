#include "Editor/Windows/Animation/AnimatorControllerWindow.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Editor/App/DiskCompare.h"
#include "Editor/SourceControl/ScmHint.h" // M66i: 保存直後に status を取り直させる
#include "Editor/Scene/Selection.h"
#include "Editor/Windows/Animation/ControllerStateEdit.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Animation/Animation.h"
#include "Engine/Engine/Animation/AnimatorController.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

#include "Engine/Renderer/ImGui/ImGuiTheme.h" // themeColor (選択 = Accent)

#include "imgui.h"

namespace mye {

std::vector<AnimatorControllerWindow::NodePos>&
AnimatorControllerWindow::NodePositions(uint64_t h, size_t n)
{
    auto& v = layout_[h];
    while (v.size() < n) {
        const size_t i = v.size();
        NodePos p;
        p.x = 30.0f + static_cast<float>(i % 3) * 170.0f;
        p.y = 30.0f + static_cast<float>(i / 3) * 90.0f;
        v.push_back(p);
    }
    return v;
}

namespace {

const char* kOps[] = { "gt", "ge", "lt", "le", "eq", "ne" };
// ControllerParamType の並び (.controller.json の "type" と同じ綴り)
const char* kParamTypes[] = { "int", "float", "bool", "trigger" };

// 実行中の値を型に合わせて編集する (float はビット列で持つ)
void DrawParamValue(ControllerParamType type, int32_t& raw)
{
    switch (type) {
    case ControllerParamType::Int:
        ImGui::InputInt("##v", &raw, 0);
        break;
    case ControllerParamType::Float: {
        float value = std::bit_cast<float>(raw);
        if (ImGui::DragFloat("##v", &value, 0.01f)) {
            raw = std::bit_cast<int32_t>(value);
        }
        break;
    }
    case ControllerParamType::Bool:
    case ControllerParamType::Trigger: {
        bool value = raw != 0;
        if (ImGui::Checkbox("##v", &value)) {
            raw = value ? 1 : 0;
        }
        break;
    }
    }
}

// 条件が参照するパラメータを名前で選ぶ。宣言の無い index (旧アセット) も "#N" で表示して選び直せるようにする
void DrawConditionParamCombo(const ControllerAsset& ctrl, int32_t& param)
{
    const int count = std::min(static_cast<int>(ctrl.parameters.size()), AnimatorControllerComponent::kMaxParams);
    char current[64];
    if (param >= 0 && param < count) {
        snprintf(current, sizeof(current), "%s", ctrl.parameters[param].name.c_str());
    } else {
        snprintf(current, sizeof(current), "#%d", param);
    }
    if (ImGui::BeginCombo("##param", current)) {
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(ctrl.parameters[i].name.c_str(), i == param)) {
                param = i;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

// 遷移矢印 (a→b、b 側に矢じり)
void DrawArrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick)
{
    dl->AddLine(a, b, col, thick);
    ImVec2 d = ImVec2(b.x - a.x, b.y - a.y);
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1e-3f) {
        return;
    }
    d.x /= len;
    d.y /= len;
    const ImVec2 perp(-d.y, d.x);
    const ImVec2 tip(b.x - d.x * 2.0f, b.y - d.y * 2.0f);
    const float s = 8.0f;
    const ImVec2 p1(tip.x - d.x * s + perp.x * s * 0.5f, tip.y - d.y * s + perp.y * s * 0.5f);
    const ImVec2 p2(tip.x - d.x * s - perp.x * s * 0.5f, tip.y - d.y * s - perp.y * s * 0.5f);
    dl->AddTriangleFilled(tip, p1, p2, col);
}

// StateSkelKind の並び
const StrId kSkelKindNames[] = { StrId::Anim_SkelNone, StrId::Anim_SkelClip, StrId::Anim_SkelBlend1D,
                                 StrId::Anim_SkelBlend2D };

const ImU32 kMissingClipColor = IM_COL32(240, 150, 90, 255);

// 骨クリップの名前を選ぶ。候補は主モデルの名前付きクリップ。名前で引くので候補に無い名前も書ける
// (別のモデルを駆動する SkinnedMesh にだけあるクリップ等) — その場合は警告色で「モデルに無い」と出す。
// 返り値 = 名前を変えた
bool DrawSkelClipPicker(const char* label, std::string& clip, const std::vector<std::string>& modelClips)
{
    bool changed = false;
    const bool known = std::find(modelClips.begin(), modelClips.end(), clip) != modelClips.end();
    if (ImGui::BeginCombo(label, clip.empty() ? "-" : clip.c_str())) {
        for (size_t i = 0; i < modelClips.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(modelClips[i].c_str(), modelClips[i] == clip)) {
                clip = modelClips[i];
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", clip.c_str());
        if (ImGui::InputText("##free", buf, sizeof(buf))) {
            clip = buf;
            changed = true;
        }
        ImGui::EndCombo();
    }
    if (!clip.empty() && !known) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMissingClipColor), "%s", Tr(StrId::Anim_NotInModel));
    }
    return changed;
}

// 子ごとの重み (0..1)。ComputeBlendWeights の結果を子の index で引けるように並べ直す
std::vector<float> ChildWeights(const ControllerState& state, float x, float y)
{
    std::vector<float> weights(state.blendChildren.size(), 0.0f);
    BlendChildWeight w[kMaxBlendLayers];
    const int32_t n = ComputeBlendWeights(state, x, y, w);
    for (int32_t i = 0; i < n; ++i) {
        weights[static_cast<size_t>(w[i].child)] =
            static_cast<float>(w[i].weightQ) / static_cast<float>(SkinnedMeshComponent::kPoseWeightOne);
    }
    return weights;
}

// 表示範囲 [lo, hi] を値の集合から作る (幅 0 は ±1、端は 8% の余白)
void FitRange(float& lo, float& hi)
{
    if (!(hi - lo > 1e-6f)) {
        lo -= 1.0f;
        hi += 1.0f;
    }
    const float pad = (hi - lo) * 0.08f;
    lo -= pad;
    hi += pad;
}

void IncludeInRange(float v, float& lo, float& hi)
{
    if (std::isfinite(v)) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
}

const ImU32 kBlendPointColor = IM_COL32(230, 80, 80, 255); // 今のパラメータの値
const ImU32 kBlendIdleColor = IM_COL32(150, 150, 160, 220);

// 1D: 横軸に子の閾値を並べ、重みの大きさで点を太らせ、今の値に縦線を引く
void DrawBlend1DGraph(const ControllerState& state, float x, const std::vector<float>& weights)
{
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(themeColor::Accent);
    const float width = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
    const float height = 40.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float lo = x;
    float hi = x;
    if (!std::isfinite(x)) {
        lo = hi = 0.0f;
    }
    for (const ControllerBlendChild& c : state.blendChildren) {
        IncludeInRange(c.threshold, lo, hi);
    }
    FitRange(lo, hi);
    const auto toScreen = [&](float v) { return p0.x + (v - lo) / (hi - lo) * width; };
    const float axisY = p0.y + height * 0.5f;
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), IM_COL32(30, 32, 38, 255), 3.0f);
    dl->AddLine(ImVec2(p0.x, axisY), ImVec2(p0.x + width, axisY), kBlendIdleColor, 1.0f);
    for (size_t i = 0; i < state.blendChildren.size(); ++i) {
        const float sx = toScreen(state.blendChildren[i].threshold);
        const float w = weights[i];
        dl->AddCircleFilled(ImVec2(sx, axisY), 3.0f + 7.0f * w, w > 0.0f ? accent : kBlendIdleColor);
        char label[8];
        snprintf(label, sizeof(label), "%zu", i);
        dl->AddText(ImVec2(sx - 3.0f, p0.y + 1.0f), IM_COL32(220, 220, 228, 255), label);
    }
    if (std::isfinite(x)) {
        const float sx = toScreen(x);
        dl->AddLine(ImVec2(sx, p0.y + 4.0f), ImVec2(sx, p0.y + height - 4.0f), kBlendPointColor, 2.0f);
    }
    ImGui::Dummy(ImVec2(width, height));
}

// 2D: 子の位置を散布し (縦横同じ縮尺、上が +y)、重みの大きさで点を太らせ、今の (x, y) に十字を描く
void DrawBlend2DGraph(const ControllerState& state, float x, float y, const std::vector<float>& weights)
{
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(themeColor::Accent);
    const float side = std::clamp(ImGui::GetContentRegionAvail().x, 60.0f, 200.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float loX = std::isfinite(x) ? x : 0.0f;
    float hiX = loX;
    float loY = std::isfinite(y) ? y : 0.0f;
    float hiY = loY;
    for (const ControllerBlendChild& c : state.blendChildren) {
        IncludeInRange(c.posX, loX, hiX);
        IncludeInRange(c.posY, loY, hiY);
    }
    FitRange(loX, hiX);
    FitRange(loY, hiY);
    // 縦横の縮尺を広い方へ揃える (gradient band の「近さ」が見た目と合うように)
    const float span = std::max(hiX - loX, hiY - loY);
    const float midX = (loX + hiX) * 0.5f;
    const float midY = (loY + hiY) * 0.5f;
    const auto toScreen = [&](float px, float py) {
        return ImVec2(p0.x + ((px - midX) / span + 0.5f) * side, p0.y + (0.5f - (py - midY) / span) * side);
    };
    dl->AddRectFilled(p0, ImVec2(p0.x + side, p0.y + side), IM_COL32(30, 32, 38, 255), 3.0f);
    const ImVec2 origin = toScreen(0.0f, 0.0f);
    if (origin.x > p0.x && origin.x < p0.x + side) {
        dl->AddLine(ImVec2(origin.x, p0.y), ImVec2(origin.x, p0.y + side), IM_COL32(60, 62, 70, 255), 1.0f);
    }
    if (origin.y > p0.y && origin.y < p0.y + side) {
        dl->AddLine(ImVec2(p0.x, origin.y), ImVec2(p0.x + side, origin.y), IM_COL32(60, 62, 70, 255), 1.0f);
    }
    for (size_t i = 0; i < state.blendChildren.size(); ++i) {
        const ControllerBlendChild& c = state.blendChildren[i];
        const ImVec2 sp = toScreen(c.posX, c.posY);
        const float w = weights[i];
        dl->AddCircleFilled(sp, 3.0f + 8.0f * w, w > 0.0f ? accent : kBlendIdleColor);
        char label[8];
        snprintf(label, sizeof(label), "%zu", i);
        dl->AddText(ImVec2(sp.x + 6.0f, sp.y - 14.0f), IM_COL32(220, 220, 228, 255), label);
    }
    if (std::isfinite(x) && std::isfinite(y)) {
        const ImVec2 sp = toScreen(x, y);
        dl->AddLine(ImVec2(sp.x - 6.0f, sp.y), ImVec2(sp.x + 6.0f, sp.y), kBlendPointColor, 2.0f);
        dl->AddLine(ImVec2(sp.x, sp.y - 6.0f), ImVec2(sp.x, sp.y + 6.0f), kBlendPointColor, 2.0f);
    }
    ImGui::Dummy(ImVec2(side, side));
}

} // namespace

void AnimatorControllerWindow::OnImGui(EngineContext& ctx, Selection& selection)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_Animator), &open)) {
        ImGui::End();
        return;
    }
    if (!ctx.controllers || !ctx.scene) {
        ImGui::TextDisabled("%s", Tr(StrId::Anim_NoCtrlLibrary));
        ImGui::End();
        return;
    }

    // 選択エンティティの AnimatorControllerComponent → 参照している .controller.json
    AnimatorControllerComponent* comp = nullptr;
    GameObject go;
    if (selection.primary != 0) {
        go = ctx.scene->FindByFileId(selection.primary);
        if (go) {
            comp = go.GetComponent<AnimatorControllerComponent>();
        }
    }
    if (!comp) {
        ImGui::TextWrapped("%s", Tr(StrId::Anim_SelectEntity));
        ImGui::End();
        return;
    }
    ControllerAsset* ctrl = ctx.controllers->GetMutable(comp->controller.value);
    if (!ctrl) {
        ImGui::TextWrapped("%s", Tr(StrId::Anim_UnknownCtrl));
        ImGui::End();
        return;
    }
    const int nStates = static_cast<int>(ctrl->states.size());
    World& world = ctx.scene->GetWorld();
    // 骨クリップの候補とステートの長さは主 SkinnedMesh のモデルで決まる (AnimatorControllerSystem と同じ)
    const SkinnedModel* mainModel =
        MainSkinnedModel(world, go.Id(), ctx.resources != nullptr ? &ctx.resources->skinnedModels : nullptr);
    const std::vector<std::string> modelClips = NamedSkeletalClips(mainModel);

    // 未保存判定のために「この窓が触ったコントローラ」を控える (M66d)
    MarkTouched(ctx.controllers, ctrl->hash);

    // ---- ツールバー ----
    ImGui::Text(Tr(StrId::Anim_Controller), ctrl->name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(StrId::Anim_AddState))) {
        ControllerState s;
        char nm[32];
        snprintf(nm, sizeof(nm), "State%d", nStates);
        s.name = nm;
        ctrl->states.push_back(s);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(StrId::Anim_AddTransition)) && nStates >= 2) {
        ControllerTransition t;
        t.from = 0;
        t.to = 1;
        ctrl->transitions.push_back(t);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(StrId::Common_Save))) {
        if (ctx.controllers->SaveToFile(ctrl->hash)) {
            scmhint::Changed(ctrl->path); // M66i: バッジと Changes の即時反映
        }
    }
    ImGui::Separator();

    // ---- 左: パラメータ + インスペクタ ----
    ImGui::BeginChild("ctrl_left", ImVec2(300, 0), true);
    ImGui::TextUnformatted(Tr(StrId::Anim_ParamsLive));
    ImGui::Separator();
    // 1 行 = 名前 (アセット) / 型 (アセット) / 値 (選択中エンティティの実行中の値)
    constexpr int kMaxParams = AnimatorControllerComponent::kMaxParams;
    for (int i = 0; i < static_cast<int>(ctrl->parameters.size()) && i < kMaxParams; ++i) {
        ControllerParam& p = ctrl->parameters[i];
        ImGui::PushID(i);
        char nameBuf[64];
        snprintf(nameBuf, sizeof(nameBuf), "%s", p.name.c_str());
        ImGui::SetNextItemWidth(92);
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
            p.name = nameBuf;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64);
        int type = static_cast<int>(p.type);
        if (ImGui::Combo("##type", &type, kParamTypes, IM_ARRAYSIZE(kParamTypes))) {
            p.type = static_cast<ControllerParamType>(type);
            comp->params[i] = 0; // ビット列の意味が変わるので値は 0 から
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        DrawParamValue(p.type, comp->params[i]);
        ImGui::PopID();
    }
    if (static_cast<int>(ctrl->parameters.size()) < kMaxParams && ImGui::SmallButton(Tr(StrId::Anim_AddParam))) {
        char nm[16];
        snprintf(nm, sizeof(nm), "param%zu", ctrl->parameters.size());
        ctrl->parameters.push_back({ nm });
    }
    ImGui::Separator();
    ImGui::Text(Tr(StrId::Anim_Current), (comp->currentState >= 0 && comp->currentState < nStates)
                                   ? ctrl->states[comp->currentState].name.c_str()
                                   : "-");
    if (comp->transitionTo >= 0 && comp->transitionTo < nStates) {
        ImGui::Text(Tr(StrId::Anim_TransitionRow), ctrl->states[comp->transitionTo].name.c_str(),
                    comp->transitionTick, comp->transitionDuration);
    }
    DrawPoseLayers(world, go.Id(), ctx.resources != nullptr ? &ctx.resources->skinnedModels : nullptr, *ctrl, *comp);
    ImGui::Separator();

    // 選択ステートの編集
    if (selectedState_ >= 0 && selectedState_ < nStates) {
        ControllerState& s = ctrl->states[selectedState_];
        ImGui::Text(Tr(StrId::Anim_State), s.name.c_str());
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", s.name.c_str());
        if (ImGui::InputText(Tr(StrId::Anim_Name), buf, sizeof(buf))) {
            s.name = buf;
        }
        // プロパティクリップ選択 (AnimationLibrary から)。骨の駆動とは独立
        if (ctx.anims) {
            const std::vector<AnimClipEntry> clips = ctx.anims->Enumerate();
            const char* cur = "(none)";
            for (const auto& c : clips) {
                if (c.hash == s.clipHash) {
                    cur = c.name.c_str();
                }
            }
            if (ImGui::BeginCombo(Tr(StrId::Anim_Clip), cur)) {
                for (const auto& c : clips) {
                    if (ImGui::Selectable(c.name.c_str(), c.hash == s.clipHash)) {
                        // M39a: 保存は clipHash (= GUID) の数値参照 — パスの推測書きは不要
                        s.clipHash = c.hash;
                        s.clipPath.clear();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::InputInt(Tr(StrId::Anim_Speed), &s.speed);
        bool loop = s.loop != 0;
        if (ImGui::Checkbox(Tr(StrId::Anim_Loop), &loop)) {
            s.loop = loop ? 1 : 0;
        }
        DrawSkeletonDrive(*ctrl, s, *comp, modelClips);
    } else {
        ImGui::TextDisabled("%s", Tr(StrId::Anim_ClickNode));
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- 右: ノードグラフキャンバス ----
    ImGui::BeginChild("ctrl_canvas", ImVec2(0, 0), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    std::vector<NodePos>& pos = NodePositions(ctrl->hash, ctrl->states.size());
    const ImVec2 nodeSize(140.0f, 60.0f);

    auto center = [&](int i) {
        return ImVec2(origin.x + pos[i].x + nodeSize.x * 0.5f,
                      origin.y + pos[i].y + nodeSize.y * 0.5f);
    };

    // 遷移矢印 (ノードの下に描く)
    for (int ti = 0; ti < static_cast<int>(ctrl->transitions.size()); ++ti) {
        const ControllerTransition& t = ctrl->transitions[ti];
        if (t.from < 0 || t.from >= nStates || t.to < 0 || t.to >= nStates) {
            continue; // Any-state 等は矢印省略 (下のリストに出る)
        }
        const bool active = comp->transitionTo == t.to && comp->currentState == t.from;
        const ImU32 col = active ? IM_COL32(90, 220, 120, 255)
                                 : (selectedTransition_ == ti ? ImGui::ColorConvertFloat4ToU32(themeColor::Accent)
                                                              : IM_COL32(150, 150, 160, 200));
        DrawArrow(dl, center(t.from), center(t.to), col, active ? 3.0f : 1.5f);
    }

    // ステートノード
    for (int i = 0; i < nStates; ++i) {
        const ImVec2 p(origin.x + pos[i].x, origin.y + pos[i].y);
        const ImVec2 pmax(p.x + nodeSize.x, p.y + nodeSize.y);
        ImU32 fill = IM_COL32(60, 66, 82, 240);
        if (i == comp->currentState) {
            fill = IM_COL32(46, 110, 66, 245); // 現在ステート=緑
        }
        if (i == comp->transitionTo) {
            fill = IM_COL32(90, 90, 50, 245); // 遷移先=黄
        }
        dl->AddRectFilled(p, pmax, fill, 5.0f);
        dl->AddRect(p, pmax, selectedState_ == i ? ImGui::ColorConvertFloat4ToU32(themeColor::Accent)
                                                 : IM_COL32(20, 20, 24, 255),
                    5.0f, selectedState_ == i ? 2.5f : 1.0f);
        dl->AddText(ImVec2(p.x + 8, p.y + 6), IM_COL32(235, 235, 240, 255), ctrl->states[i].name.c_str());
        // 2 行目 = 骨の駆動の種類 (クリップ 1 本なら名前)。既定ステートは青字
        const ControllerState& ns = ctrl->states[i];
        const StateSkelKind kind = GetStateSkelKind(ns);
        char sub[64];
        if (kind == StateSkelKind::Clip) {
            snprintf(sub, sizeof(sub), "%s", ns.skelClip.c_str());
        } else if (kind == StateSkelKind::None) {
            snprintf(sub, sizeof(sub), "-");
        } else {
            snprintf(sub, sizeof(sub), "%s (%zu)", Tr(kSkelKindNames[static_cast<int>(kind)]), ns.blendChildren.size());
        }
        dl->AddText(ImVec2(p.x + 8, p.y + 24), IM_COL32(170, 170, 180, 255), sub);
        if (i == ctrl->defaultState) {
            dl->AddText(ImVec2(p.x + 8, p.y + 40), IM_COL32(150, 200, 255, 255), "default");
        }

        // ドラッグ + 選択
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        ImGui::InvisibleButton("node", nodeSize);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 dpos = ImGui::GetIO().MouseDelta;
            pos[i].x += dpos.x;
            pos[i].y += dpos.y;
        }
        if (ImGui::IsItemClicked()) {
            selectedState_ = i;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- 遷移リスト (キャンバス下) ----
    if (ImGui::CollapsingHeader(Tr(StrId::Anim_Transitions), ImGuiTreeNodeFlags_DefaultOpen)) {
        auto stateName = [&](int idx) {
            return (idx == -1) ? "Any" : (idx >= 0 && idx < nStates ? ctrl->states[idx].name.c_str() : "?");
        };
        for (int ti = 0; ti < static_cast<int>(ctrl->transitions.size()); ++ti) {
            ControllerTransition& t = ctrl->transitions[ti];
            ImGui::PushID(1000 + ti);
            if (ImGui::TreeNode("t", "%s -> %s  (dur %d, cond %zu)", stateName(t.from),
                                stateName(t.to), t.duration, t.conditions.size())) {
                selectedTransition_ = ti;
                ImGui::InputInt(Tr(StrId::Anim_From), &t.from);
                ImGui::InputInt(Tr(StrId::Anim_To), &t.to);
                ImGui::InputInt(Tr(StrId::Anim_Duration), &t.duration);
                bool exit = t.hasExitTime != 0;
                if (ImGui::Checkbox(Tr(StrId::Anim_HasExitTime), &exit)) {
                    t.hasExitTime = exit ? 1 : 0;
                }
                ImGui::TextUnformatted(Tr(StrId::Anim_Conditions));
                for (int ci = 0; ci < static_cast<int>(t.conditions.size()); ++ci) {
                    ControllerCondition& c = t.conditions[ci];
                    ImGui::PushID(ci);
                    ImGui::SetNextItemWidth(110);
                    DrawConditionParamCombo(*ctrl, c.param);
                    ImGui::SameLine();
                    const ControllerParamType type = ControllerParamTypeAt(*ctrl, c.param);
                    if (type == ControllerParamType::Trigger) {
                        ImGui::TextDisabled("%s", Tr(StrId::Anim_TriggerCond)); // 演算と値は見ない
                    } else {
                        ImGui::SetNextItemWidth(50);
                        int op = static_cast<int>(c.op);
                        if (ImGui::Combo("##op", &op, kOps, IM_ARRAYSIZE(kOps))) {
                            c.op = static_cast<CondOp>(op);
                        }
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(70);
                        if (type == ControllerParamType::Float) {
                            ImGui::DragFloat(Tr(StrId::Anim_Val), &c.floatValue, 0.01f);
                        } else if (type == ControllerParamType::Bool) {
                            bool value = c.value != 0;
                            if (ImGui::Checkbox(Tr(StrId::Anim_Val), &value)) {
                                c.value = value ? 1 : 0;
                            }
                        } else {
                            ImGui::InputInt(Tr(StrId::Anim_Val), &c.value);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) {
                        t.conditions.erase(t.conditions.begin() + ci);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton(Tr(StrId::Anim_AddCondition))) {
                    t.conditions.push_back({ 0, CondOp::Gt, 0 });
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    ImGui::End();
}

void AnimatorControllerWindow::DrawSkeletonDrive(const ControllerAsset& ctrl, ControllerState& state,
                                                 const AnimatorControllerComponent& comp,
                                                 const std::vector<std::string>& modelClips)
{
    ImGui::Separator();
    const char* kindNames[IM_ARRAYSIZE(kSkelKindNames)];
    for (int i = 0; i < IM_ARRAYSIZE(kSkelKindNames); ++i) {
        kindNames[i] = Tr(kSkelKindNames[i]);
    }
    int kind = static_cast<int>(GetStateSkelKind(state));
    if (ImGui::Combo(Tr(StrId::Anim_SkelKind), &kind, kindNames, IM_ARRAYSIZE(kindNames))) {
        SetStateSkelKind(state, static_cast<StateSkelKind>(kind), modelClips.empty() ? std::string() : modelClips.front());
    }
    if (modelClips.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Anim_NoSkinnedModel));
    }
    switch (GetStateSkelKind(state)) {
    case StateSkelKind::None:
        return;
    case StateSkelKind::Clip: {
        std::string clip = state.skelClip;
        if (DrawSkelClipPicker(Tr(StrId::Anim_SkelClipName), clip, modelClips)) {
            SetStateSkelClip(state, clip);
        }
        return;
    }
    case StateSkelKind::Blend1D:
    case StateSkelKind::Blend2D:
        break;
    }

    // 混ぜ具合を決めるパラメータ (2D は x と y)
    const bool is2D = state.blendType == ControllerBlendType::Blend2D;
    ImGui::TextUnformatted(Tr(is2D ? StrId::Anim_BlendParamX : StrId::Anim_BlendParam));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID("px");
    DrawConditionParamCombo(ctrl, state.blendParam);
    ImGui::PopID();
    if (is2D) {
        ImGui::TextUnformatted(Tr(StrId::Anim_BlendParamY));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushID("py");
        DrawConditionParamCombo(ctrl, state.blendParamY);
        ImGui::PopID();
    }

    // 図と子の重みは選択エンティティの今のパラメータの値で出す (sim と同じ ComputeBlendWeights)
    const float x = ControllerParamAsFloat(ctrl, state.blendParam, comp.params);
    const float y = is2D ? ControllerParamAsFloat(ctrl, state.blendParamY, comp.params) : 0.0f;
    const std::vector<float> weights = ChildWeights(state, x, y);
    if (is2D) {
        DrawBlend2DGraph(state, x, y, weights);
    } else {
        DrawBlend1DGraph(state, x, weights);
    }

    ImGui::TextUnformatted(Tr(StrId::Anim_BlendChildren));
    for (size_t i = 0; i < state.blendChildren.size(); ++i) {
        ControllerBlendChild& child = state.blendChildren[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("%zu", i);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(84);
        std::string clip = child.clip;
        if (DrawSkelClipPicker("##clip", clip, modelClips)) {
            SetBlendChildClip(child, clip);
        }
        ImGui::SameLine();
        if (is2D) {
            ImGui::SetNextItemWidth(40);
            ImGui::DragFloat("##x", &child.posX, 0.01f, 0.0f, 0.0f, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(40);
            ImGui::DragFloat("##y", &child.posY, 0.01f, 0.0f, 0.0f, "%.2f");
        } else {
            ImGui::SetNextItemWidth(50);
            ImGui::DragFloat("##th", &child.threshold, 0.01f, 0.0f, 0.0f, "%.2f");
        }
        ImGui::SameLine();
        ImGui::Text("%3.0f%%", weights[i] * 100.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            state.blendChildren.erase(state.blendChildren.begin() + static_cast<std::ptrdiff_t>(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(Tr(StrId::Anim_AddBlendChild))) {
        AddBlendChild(state, modelClips.empty() ? std::string() : modelClips.front());
    }
}

void AnimatorControllerWindow::DrawPoseLayers(World& world, EntityID entity, const SkinnedModelLibrary* models,
                                              const ControllerAsset& ctrl, const AnimatorControllerComponent& comp)
{
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::Anim_PoseLayers));
    // 位相はブレンドツリーのステートだけが使う (1 周 = 2^32)
    const int nStates = static_cast<int>(ctrl.states.size());
    const auto showPhase = [&](int32_t stateIndex, uint32_t phase) {
        if (stateIndex >= 0 && stateIndex < nStates
            && ctrl.states[static_cast<size_t>(stateIndex)].blendType != ControllerBlendType::None) {
            ImGui::Text("%s %s %.1f%%", ctrl.states[static_cast<size_t>(stateIndex)].name.c_str(), Tr(StrId::Anim_Phase),
                        static_cast<double>(phase) / 4294967296.0 * 100.0);
        }
    };
    showPhase(comp.currentState, comp.statePhase);
    if (comp.transitionTo >= 0) {
        showPhase(comp.transitionTo, comp.transitionToPhase);
    }

    CollectDrivenSkinnedMeshes(world, entity, driven_);
    if (driven_.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Anim_NoSkinnedModel));
        return;
    }
    for (const EntityID e : driven_) {
        const SkinnedMeshComponent* sm = world.GetComponent<SkinnedMeshComponent>(e);
        if (sm == nullptr) {
            continue;
        }
        const SkinnedModel* model = models != nullptr ? models->Get(sm->model) : nullptr;
        ImGui::TextUnformatted(world.GetName(e));
        if (sm->poseLayerCount == 0) {
            ImGui::TextDisabled("  %s", Tr(StrId::Anim_LegacyPose));
            continue;
        }
        for (int32_t l = 0; l < sm->poseLayerCount && l < SkinnedMeshComponent::kMaxPoseLayers; ++l) {
            const SkinnedMeshComponent::PoseLayer& layer = sm->poseLayers[l];
            char clipName[64];
            if (model != nullptr && layer.clip >= 0 && layer.clip < static_cast<int32_t>(model->clips.size())
                && !model->clips[static_cast<size_t>(layer.clip)].name.empty()) {
                snprintf(clipName, sizeof(clipName), "%s", model->clips[static_cast<size_t>(layer.clip)].name.c_str());
            } else {
                snprintf(clipName, sizeof(clipName), "#%d", layer.clip);
            }
            ImGui::Text("  %-8s t %6.1f  w %5.1f%%", clipName,
                        static_cast<double>(layer.timeQ) / SkinnedMeshComponent::kPoseTimeQPerTick,
                        static_cast<double>(layer.weightQ) / SkinnedMeshComponent::kPoseWeightOne * 100.0);
        }
    }
}

void AnimatorControllerWindow::MarkTouched(ControllerLibrary* controllers, uint64_t ctrlHash)
{
    controllers_ = controllers;
    if (ctrlHash == 0
        || std::find(touched_.begin(), touched_.end(), ctrlHash) != touched_.end()) {
        return;
    }
    touched_.push_back(ctrlHash);
}

bool AnimatorControllerWindow::HasUnsavedChanges() const
{
    if (controllers_ == nullptr) {
        return false;
    }
    for (const uint64_t hash : touched_) {
        const ControllerAsset* c = controllers_->Get(hash);
        if (c == nullptr) {
            continue;
        }
        if (TextDiffersFromDisk(c->path, ControllerLibrary::ToJson(*c).dump(2))) {
            return true;
        }
    }
    return false;
}

} // namespace mye
