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

// 「ラベル | 値」の 1 行。訳文 (Tr) はラベルとして文字列のまま出し、printf 系の書式には渡さない (AGENTS 4.3)
void ValueRow(const char* label, const char* value)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value);
}

void IntRow(const char* label, int value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", value);
    ValueRow(label, buf);
}

// 書式は常にリテラル (桁数は %.*f の引数で渡す)
void FloatRow(const char* label, float value, int decimals, bool showSign)
{
    char buf[48];
    if (showSign) {
        std::snprintf(buf, sizeof(buf), "%+.*f", decimals, static_cast<double>(value));
    } else {
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(value));
    }
    ValueRow(label, buf);
}

void Vec3Row(const char* label, const float v[3], int decimals)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.*f, %.*f, %.*f", decimals, static_cast<double>(v[0]), decimals,
                  static_cast<double>(v[1]), decimals, static_cast<double>(v[2]));
    ValueRow(label, buf);
}

constexpr ImGuiTableFlags kValueTableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;

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
    if (ImGui::BeginTable("##time", 2, kValueTableFlags)) {
        IntRow(Tr(StrId::Feel_HitStop), tc.freezeTicks);
        IntRow(Tr(StrId::Feel_SlowPercent), tc.CurrentSlowPercent());
        IntRow(Tr(StrId::Feel_SlowHold), tc.slowHoldTicks);
        IntRow(Tr(StrId::FeelMon_RecoverLeft), tc.slowRecoverLeft);
        IntRow(Tr(StrId::FeelMon_BaseScale), tc.scalePercent);
        ImGui::EndTable();
    }

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
    if (ImGui::BeginTable("##out", 2, kValueTableFlags)) {
        Vec3Row(Tr(StrId::FeelMon_CamOffset), o.camPos, 3);
        Vec3Row(Tr(StrId::FeelMon_CamRot), o.camRotDeg, 2);
        FloatRow(Tr(StrId::Feel_KickFov), o.fovDeg, 2, true);
        FloatRow(Tr(StrId::Feel_RumbleLow), o.rumbleLow, 2, false);
        FloatRow(Tr(StrId::Feel_RumbleHigh), o.rumbleHigh, 2, false);
        FloatRow(Tr(StrId::Feel_FlashVignette), o.vignette, 3, true);
        FloatRow(Tr(StrId::Feel_FlashExposure), o.exposure, 3, true);
        FloatRow(Tr(StrId::FeelMon_FlashWeight), o.flashWeight, 2, false);
        ImGui::EndTable();
    }

    // 先行入力 (bufferTicks を持つアクションだけ、レーン 0)
    ImGui::SeparatorText(Tr(StrId::FeelMon_Buffer));
    bool anyBuffer = false;
    bool anyBufferTable = false;
    if (ctx.inputActions != nullptr) {
        const std::vector<InputActionDef>& actions = ctx.inputActions->Actions();
        for (size_t i = 0; i < actions.size() && i < kMaxBufferedActions; ++i) {
            if (actions[i].bufferTicks == 0) {
                continue;
            }
            anyBuffer = true;
            const uint8_t age = scene.InputBuffer().pressAge[0][i];
            // 訳文は %s の引数として渡す (書式は固定)。アクション名はラベル列
            char value[64];
            if (age == kInputBufferNone) {
                std::snprintf(value, sizeof(value), "%s %d", Tr(StrId::FeelMon_BufClosed),
                              static_cast<int>(actions[i].bufferTicks));
            } else {
                std::snprintf(value, sizeof(value), "%s %d / %d", Tr(StrId::FeelMon_BufOpen), static_cast<int>(age),
                              static_cast<int>(actions[i].bufferTicks));
            }
            if (!anyBufferTable) {
                anyBufferTable = ImGui::BeginTable("##buffer", 2, kValueTableFlags);
            }
            if (anyBufferTable) {
                ValueRow(actions[i].name.c_str(), value);
            }
        }
    }
    if (anyBufferTable) {
        ImGui::EndTable();
    }
    if (!anyBuffer) {
        ImGui::TextDisabled("%s", Tr(StrId::FeelMon_NoBuffer));
    }
    ImGui::End();
}

} // namespace mye
