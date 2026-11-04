//====================================================================================
//                          FeelMonitorWindow.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りモニタ (M94i、ADR-031) の実装
//====================================================================================
#include "Editor/Windows/Debug/FeelMonitorWindow.h"

#include <cstdio>
#include <string>

#include "imgui.h"

#include "Editor/App/EditorSettings.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Feel/FeelLibrary.h"
#include "Engine/Engine/Feel/FeelState.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Loop/GameFlow.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/InputActions.h"

namespace mye {

namespace {

// スロットの表示名: プリセットなら名前、コードで組んだ desc なら固定の表示
std::string SourceName(uint64_t sourceKey)
{
    if (sourceKey == 0) {
        return Tr(StrId::FeelMon_FromCode);
    }
    if (const FeelPreset* p = feellib::ResolveName(sourceKey)) {
        return p->name;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(sourceKey));
    return buf;
}

std::string Channels(const MyeFeelDesc& d)
{
    std::string s;
    const auto add = [&s](bool on, const char* tag) {
        if (on) {
            s += s.empty() ? tag : std::string(" ") + tag;
        }
    };
    add(d.hitStopTicks > 0, "stop");
    add(d.slowPercent < 100, "slow");
    add(d.shakeTicks > 0, "shake");
    add(d.kickTicks > 0, "kick");
    add(d.rumbleTicks > 0, "rumble");
    add(d.flashTicks > 0, "flash");
    return s;
}

} // namespace

void FeelMonitorWindow::OnImGui(EngineContext& ctx, EditorSettings& settings)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_FeelMonitor), &open)) {
        ImGui::End();
        return;
    }
    if (ImGui::Checkbox(Tr(StrId::FeelMon_MuteCamera), &settings.feelMuteCamera)) {
        settings.Save(); // 描画への反映は EditorApp が毎フレーム ctx.feelMuteCamera へ写す (窓を閉じていても効く)
    }
    if (ctx.scene == nullptr) {
        ImGui::End();
        return;
    }
    const Scene& scene = *ctx.scene;
    const TimeControl& tc = scene.Time();

    ImGui::SeparatorText(Tr(StrId::FeelMon_Time));
    ImGui::Text(Tr(StrId::FeelMon_TimeLine), tc.freezeTicks, tc.CurrentSlowPercent(), tc.slowHoldTicks,
                tc.slowRecoverLeft, tc.scalePercent);

    ImGui::SeparatorText(Tr(StrId::FeelMon_Playing));
    const FeelState& fs = scene.Feel();
    if (fs.ActiveCount() == 0) {
        ImGui::TextDisabled("%s", Tr(StrId::FeelMon_None));
    } else if (ImGui::BeginTable("##feel", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn(Tr(StrId::FeelMon_ColSource));
        ImGui::TableSetupColumn(Tr(StrId::FeelMon_ColIntensity));
        ImGui::TableSetupColumn(Tr(StrId::FeelMon_ColAge));
        ImGui::TableSetupColumn(Tr(StrId::FeelMon_ColChannels));
        ImGui::TableHeadersRow();
        for (uint32_t i = 0; i < kMaxFeelInstances; ++i) {
            const FeelInstance& s = fs.Slot(i);
            if (s.active == 0) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", i);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(SourceName(s.sourceKey).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", static_cast<double>(s.intensity));
            ImGui::TableNextColumn();
            ImGui::Text("%d / %d", s.ageTicks, FeelPresentationTicks(s.desc));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Channels(s.desc).c_str());
        }
        ImGui::EndTable();
    }

    // いまの出力 (最新 tick の値。描画は補間で間を出す)
    ImGui::SeparatorText(Tr(StrId::FeelMon_Output));
    const FeelFrameOutput o = EvaluateFeel(fs, 1.0f, FeelOutputScale{});
    ImGui::Text(Tr(StrId::FeelMon_OutCamera), static_cast<double>(o.camPos[0]), static_cast<double>(o.camPos[1]),
                static_cast<double>(o.camPos[2]), static_cast<double>(o.camRotDeg[0]),
                static_cast<double>(o.camRotDeg[1]), static_cast<double>(o.camRotDeg[2]),
                static_cast<double>(o.fovDeg));
    ImGui::Text(Tr(StrId::FeelMon_OutRumble), static_cast<double>(o.rumbleLow), static_cast<double>(o.rumbleHigh));
    ImGui::Text(Tr(StrId::FeelMon_OutFlash), static_cast<double>(o.vignette), static_cast<double>(o.exposure),
                static_cast<double>(o.flashWeight));

    // 先行入力 (bufferTicks を持つアクションだけ、レーン 0)
    ImGui::SeparatorText(Tr(StrId::FeelMon_Buffer));
    bool anyBuffer = false;
    if (ctx.inputActions != nullptr) {
        const std::vector<InputActionDef>& actions = ctx.inputActions->Actions();
        for (size_t i = 0; i < actions.size() && i < kMaxBufferedActions; ++i) {
            if (actions[i].bufferTicks == 0) {
                continue;
            }
            anyBuffer = true;
            const uint8_t age = scene.InputBuffer().pressAge[0][i];
            if (age == kInputBufferNone) {
                ImGui::Text(Tr(StrId::FeelMon_BufferClosed), actions[i].name.c_str(), actions[i].bufferTicks);
            } else {
                ImGui::Text(Tr(StrId::FeelMon_BufferOpen), actions[i].name.c_str(), static_cast<int>(age),
                            actions[i].bufferTicks);
            }
        }
    }
    if (!anyBuffer) {
        ImGui::TextDisabled("%s", Tr(StrId::FeelMon_NoBuffer));
    }
    ImGui::End();
}

} // namespace mye
