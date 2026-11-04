//====================================================================================
//                          FeelAssetInspector.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りプリセット (.feel.json、M94d、ADR-031) のアセット Inspector の実装
//====================================================================================
#include "Editor/Windows/Scene/FeelAssetInspector.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "imgui.h"

#include "Editor/Widgets/CurveEditorWidget.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Feel/FeelLibrary.h"
#include "Engine/Engine/Feel/FeelState.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

constexpr float kFieldWidth = 160.0f;

bool DragTicks(const char* label, int32_t& v)
{
    ImGui::SetNextItemWidth(kFieldWidth);
    return ImGui::DragInt(label, &v, 0.25f, 0, kMaxFeelChannelTicks);
}

bool DragF(const char* label, float& v, float speed, float lo, float hi, const char* fmt = "%.3f")
{
    ImGui::SetNextItemWidth(kFieldWidth);
    return ImGui::DragFloat(label, &v, speed, lo, hi, fmt);
}

bool EaseCombo(const char* label, int32_t& ease)
{
    bool changed = false;
    ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::BeginCombo(label, MyeEaseName(ease))) {
        for (int32_t e = 0; e < MYE_EASE_COUNT; ++e) {
            if (ImGui::Selectable(MyeEaseName(e), e == ease)) {
                ease = e;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

void FeelAssetInspector::Load(const std::wstring& path)
{
    edit_ = MyeFeelDesc{};
    valid_ = false;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return;
    }
    nlohmann::json root;
    try {
        f >> root;
    } catch (const nlohmann::json::exception&) {
        return;
    }
    valid_ = FeelLibrary::FromJson(root, edit_); // 読み値は SanitizeFeelDesc 済み
    saved_ = edit_;
}

void FeelAssetInspector::DrawTime()
{
    ImGui::PushID("time");
    DragTicks(Tr(StrId::Feel_HitStop), edit_.hitStopTicks);
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::SliderInt(Tr(StrId::Feel_SlowPercent), &edit_.slowPercent, 0, 100);
    DragTicks(Tr(StrId::Feel_SlowHold), edit_.slowHoldTicks);
    DragTicks(Tr(StrId::Feel_SlowRecover), edit_.slowRecoverTicks);
    EaseCombo(Tr(StrId::Feel_SlowEase), edit_.slowRecoverEase);
    ImGui::PopID();
}

void FeelAssetInspector::DrawShake()
{
    ImGui::PushID("shake");
    DragTicks(Tr(StrId::Feel_Duration), edit_.shakeTicks);
    DragF(Tr(StrId::Feel_ShakePos), edit_.shakePosAmp, 0.002f, 0.0f, 10.0f);
    DragF(Tr(StrId::Feel_ShakeRot), edit_.shakeRotAmpDeg, 0.05f, 0.0f, 45.0f, "%.2f");
    DragF(Tr(StrId::Feel_ShakeFreq), edit_.shakeFrequencyHz, 0.1f, 0.0f, 120.0f, "%.1f");
    ImGui::TextUnformatted(Tr(StrId::Feel_Envelope));
    DrawCurveEditor("envelope", edit_.shakeEnvelope);
    ImGui::PopID();
}

void FeelAssetInspector::DrawKick()
{
    ImGui::PushID("kick");
    DragTicks(Tr(StrId::Feel_Duration), edit_.kickTicks);
    float off[3] = { edit_.kickOffset.x, edit_.kickOffset.y, edit_.kickOffset.z };
    ImGui::SetNextItemWidth(kFieldWidth * 1.5f);
    if (ImGui::DragFloat3(Tr(StrId::Feel_KickOffset), off, 0.002f, -10.0f, 10.0f, "%.3f")) {
        edit_.kickOffset = { off[0], off[1], off[2] };
    }
    DragF(Tr(StrId::Feel_KickPitch), edit_.kickPitchDeg, 0.05f, -45.0f, 45.0f, "%.2f");
    DragF(Tr(StrId::Feel_KickFov), edit_.kickFovDeg, 0.05f, -45.0f, 45.0f, "%.2f");
    ImGui::TextUnformatted(Tr(StrId::Feel_Curve));
    DrawCurveEditor("curve", edit_.kickCurve);
    ImGui::PopID();
}

void FeelAssetInspector::DrawRumble()
{
    ImGui::PushID("rumble");
    DragTicks(Tr(StrId::Feel_Duration), edit_.rumbleTicks);
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::SliderFloat(Tr(StrId::Feel_RumbleLow), &edit_.rumbleLow, 0.0f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::SliderFloat(Tr(StrId::Feel_RumbleHigh), &edit_.rumbleHigh, 0.0f, 1.0f, "%.2f");
    ImGui::TextUnformatted(Tr(StrId::Feel_Curve));
    DrawCurveEditor("curve", edit_.rumbleCurve);
    ImGui::PopID();
}

void FeelAssetInspector::DrawFlash()
{
    ImGui::PushID("flash");
    DragTicks(Tr(StrId::Feel_Duration), edit_.flashTicks);
    DragF(Tr(StrId::Feel_FlashVignette), edit_.flashVignette, 0.005f, -4.0f, 4.0f);
    DragF(Tr(StrId::Feel_FlashChroma), edit_.flashChromAberration, 0.0005f, -4.0f, 4.0f, "%.4f");
    DragF(Tr(StrId::Feel_FlashExposure), edit_.flashExposure, 0.01f, -1.0f, 4.0f);
    DragF(Tr(StrId::Feel_FlashSaturation), edit_.flashSaturation, 0.01f, -1.0f, 4.0f);
    float col[4] = { edit_.flashColor.r, edit_.flashColor.g, edit_.flashColor.b, edit_.flashColor.a };
    if (ImGui::ColorEdit4(Tr(StrId::Feel_FlashColor), col, ImGuiColorEditFlags_Float)) {
        edit_.flashColor = { col[0], col[1], col[2], col[3] };
    }
    ImGui::TextUnformatted(Tr(StrId::Feel_Curve));
    DrawCurveEditor("curve", edit_.flashCurve);
    ImGui::PopID();
}

void FeelAssetInspector::Draw(EngineContext& ctx, const std::wstring& path)
{
    namespace fs = std::filesystem;
    if (!valid_) {
        ImGui::TextDisabled("%s", Tr(StrId::Feel_Failed));
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr(StrId::Feel_Note));
    ImGui::PopTextWrapPos();

    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecTime), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawTime();
    }
    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecShake), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawShake();
    }
    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecKick))) {
        DrawKick();
    }
    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecRumble))) {
        DrawRumble();
    }
    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecFlash))) {
        DrawFlash();
    }

    ImGui::Separator();
    // プレビュー: 編集中の写しを正規化してから鳴らす (Play 中だけ。編集中は tick が進まず何も見えない)
    const bool canPreview = ctx.scene != nullptr && ctx.simulateScripts;
    ImGui::BeginDisabled(!canPreview);
    if (ImGui::Button(Tr(StrId::Feel_Preview), ImVec2(110, 0)) && canPreview) {
        MyeFeelDesc d = edit_;
        SanitizeFeelDesc(d);
        ctx.scene->Feel().Play(d, 1.0f, HashStr(FeelLibrary::NameFromPath(path)), ctx.scene->Time());
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", Tr(StrId::Feel_PreviewTip));
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        SanitizeFeelDesc(edit_); // 手入力の NaN / 範囲外をファイルへ焼かない
        std::ofstream out(fs::path(path), std::ios::binary);
        if (out) {
            out << FeelLibrary::ToJson(edit_).dump(2);
            out.close();
            saved_ = edit_;
            // 即時反映: 同じ GUID のまま読み直す (参照側は GUID / 名前なので再解決は要らない)
            if (FeelLibrary* fl = feellib::Library()) {
                fl->LoadFromFile(path);
            }
            MYE_LOG_INFO("feel saved: %s", WideToUtf8(path).c_str());
        } else {
            MYE_LOG_ERROR(Tr(StrId::Log_WriteFeelFail), WideToUtf8(path).c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        Load(path);
    }
    if (std::memcmp(&edit_, &saved_, sizeof(MyeFeelDesc)) != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::Feel_Unsaved));
    }
}

} // namespace mye
