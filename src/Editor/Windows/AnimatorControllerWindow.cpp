#include "Editor/Windows/AnimatorControllerWindow.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "Editor/Selection.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AnimatorController.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"

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

} // namespace

void AnimatorControllerWindow::OnImGui(EngineContext& ctx, Selection& selection)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin("Animator", &open)) {
        ImGui::End();
        return;
    }
    if (!ctx.controllers || !ctx.scene) {
        ImGui::TextDisabled("(no controller library)");
        ImGui::End();
        return;
    }

    // 選択エンティティの AnimatorControllerComponent → 参照している .controller.json
    AnimatorControllerComponent* comp = nullptr;
    if (selection.primary != 0) {
        GameObject go = ctx.scene->FindByFileId(selection.primary);
        if (go) {
            comp = go.GetComponent<AnimatorControllerComponent>();
        }
    }
    if (!comp) {
        ImGui::TextWrapped("Select an entity that has an AnimatorController component.");
        ImGui::End();
        return;
    }
    ControllerAsset* ctrl = ctx.controllers->GetMutable(comp->controller.value);
    if (!ctrl) {
        ImGui::TextWrapped("AnimatorController references an unknown .controller.json.");
        ImGui::End();
        return;
    }
    const int nStates = static_cast<int>(ctrl->states.size());

    // ---- ツールバー ----
    ImGui::Text("Controller: %s", ctrl->name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Add State")) {
        ControllerState s;
        char nm[32];
        snprintf(nm, sizeof(nm), "State%d", nStates);
        s.name = nm;
        ctrl->states.push_back(s);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add Transition") && nStates >= 2) {
        ControllerTransition t;
        t.from = 0;
        t.to = 1;
        ctrl->transitions.push_back(t);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Save")) {
        ctx.controllers->SaveToFile(ctrl->hash);
    }
    ImGui::Separator();

    // ---- 左: パラメータ + インスペクタ ----
    ImGui::BeginChild("ctrl_left", ImVec2(260, 0), true);
    ImGui::TextUnformatted("Parameters (live)");
    ImGui::Separator();
    for (int i = 0; i < static_cast<int>(ctrl->parameters.size()) && i < 4; ++i) {
        ImGui::PushID(i);
        ImGui::InputInt(ctrl->parameters[i].name.c_str(), &comp->params[i]);
        ImGui::PopID();
    }
    if (ctrl->parameters.size() < 4 && ImGui::SmallButton("+ param")) {
        char nm[16];
        snprintf(nm, sizeof(nm), "param%zu", ctrl->parameters.size());
        ctrl->parameters.push_back({ nm });
    }
    ImGui::Separator();
    ImGui::Text("Current: %s", (comp->currentState >= 0 && comp->currentState < nStates)
                                   ? ctrl->states[comp->currentState].name.c_str()
                                   : "-");
    if (comp->transitionTo >= 0 && comp->transitionTo < nStates) {
        ImGui::Text("-> %s (%d/%d)", ctrl->states[comp->transitionTo].name.c_str(),
                    comp->transitionTick, comp->transitionDuration);
    }
    ImGui::Separator();

    // 選択ステートの編集
    if (selectedState_ >= 0 && selectedState_ < nStates) {
        ControllerState& s = ctrl->states[selectedState_];
        ImGui::Text("State: %s", s.name.c_str());
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", s.name.c_str());
        if (ImGui::InputText("name", buf, sizeof(buf))) {
            s.name = buf;
        }
        // クリップ選択 (AnimationLibrary から)
        if (ctx.anims) {
            const std::vector<AnimClipEntry> clips = ctx.anims->Enumerate();
            const char* cur = "(none)";
            for (const auto& c : clips) {
                if (c.hash == s.clipHash) {
                    cur = c.name.c_str();
                }
            }
            if (ImGui::BeginCombo("clip", cur)) {
                for (const auto& c : clips) {
                    if (ImGui::Selectable(c.name.c_str(), c.hash == s.clipHash)) {
                        s.clipHash = c.hash;
                        s.clipPath = c.name + ".anim.json";
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::InputInt("speed", &s.speed);
        bool loop = s.loop != 0;
        if (ImGui::Checkbox("loop", &loop)) {
            s.loop = loop ? 1 : 0;
        }
    } else {
        ImGui::TextDisabled("(click a node to edit)");
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- 右: ノードグラフキャンバス ----
    ImGui::BeginChild("ctrl_canvas", ImVec2(0, 0), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    std::vector<NodePos>& pos = NodePositions(ctrl->hash, ctrl->states.size());
    const ImVec2 nodeSize(140.0f, 44.0f);

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
                                 : (selectedTransition_ == ti ? IM_COL32(230, 200, 90, 255)
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
        dl->AddRect(p, pmax, selectedState_ == i ? IM_COL32(230, 200, 90, 255)
                                                 : IM_COL32(20, 20, 24, 255),
                    5.0f, selectedState_ == i ? 2.5f : 1.0f);
        dl->AddText(ImVec2(p.x + 8, p.y + 6), IM_COL32(235, 235, 240, 255), ctrl->states[i].name.c_str());
        if (i == ctrl->defaultState) {
            dl->AddText(ImVec2(p.x + 8, p.y + 24), IM_COL32(150, 200, 255, 255), "default");
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
    if (ImGui::CollapsingHeader("Transitions", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto stateName = [&](int idx) {
            return (idx == -1) ? "Any" : (idx >= 0 && idx < nStates ? ctrl->states[idx].name.c_str() : "?");
        };
        for (int ti = 0; ti < static_cast<int>(ctrl->transitions.size()); ++ti) {
            ControllerTransition& t = ctrl->transitions[ti];
            ImGui::PushID(1000 + ti);
            if (ImGui::TreeNode("t", "%s -> %s  (dur %d, cond %zu)", stateName(t.from),
                                stateName(t.to), t.duration, t.conditions.size())) {
                selectedTransition_ = ti;
                ImGui::InputInt("from", &t.from);
                ImGui::InputInt("to", &t.to);
                ImGui::InputInt("duration", &t.duration);
                bool exit = t.hasExitTime != 0;
                if (ImGui::Checkbox("hasExitTime", &exit)) {
                    t.hasExitTime = exit ? 1 : 0;
                }
                ImGui::TextUnformatted("Conditions:");
                for (int ci = 0; ci < static_cast<int>(t.conditions.size()); ++ci) {
                    ControllerCondition& c = t.conditions[ci];
                    ImGui::PushID(ci);
                    ImGui::SetNextItemWidth(60);
                    ImGui::InputInt("param", &c.param);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(50);
                    int op = static_cast<int>(c.op);
                    if (ImGui::Combo("##op", &op, kOps, IM_ARRAYSIZE(kOps))) {
                        c.op = static_cast<CondOp>(op);
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60);
                    ImGui::InputInt("val", &c.value);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) {
                        t.conditions.erase(t.conditions.begin() + ci);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("+ condition")) {
                    t.conditions.push_back({ 0, CondOp::Gt, 0 });
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    ImGui::End();
}

} // namespace mye
