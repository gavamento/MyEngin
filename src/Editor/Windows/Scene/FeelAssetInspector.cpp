//====================================================================================
//                          FeelAssetInspector.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りプリセット (.feel.json、M94d、ADR-035) のアセット Inspector の実装
//====================================================================================
#include "Editor/Windows/Scene/FeelAssetInspector.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "imgui.h"

#include "Editor/Widgets/CurveEditorWidget.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Feel/FeelLibrary.h"
#include "Engine/Engine/Feel/FeelState.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Loop/GameFlow.h"
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
    hash_ = 0;
    valid_ = false;
    saved_ = MyeFeelDesc{};
    FeelLibrary* lib = feellib::Library();
    if (lib == nullptr) {
        return;
    }
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
    if (!FeelLibrary::FromJson(root, saved_)) { // 読み値は SanitizeFeelDesc 済み
        return;
    }
    hash_ = FeelLibrary::HashForPath(path);
    if (!lib->Contains(hash_)) {
        lib->Register(path, saved_); // 起動後に外から置かれたファイル
    }
    valid_ = true;
}

void FeelAssetInspector::DrawTime(MyeFeelDesc& d)
{
    ImGui::PushID("time");
    DragTicks(Tr(StrId::Feel_HitStop), d.hitStopTicks);
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::SliderInt(Tr(StrId::Feel_SlowPercent), &d.slowPercent, 0, 100);
    DragTicks(Tr(StrId::Feel_SlowHold), d.slowHoldTicks);
    DragTicks(Tr(StrId::Feel_SlowRecover), d.slowRecoverTicks);
    EaseCombo(Tr(StrId::Feel_SlowEase), d.slowRecoverEase);
    ImGui::PopID();
}

void FeelAssetInspector::DrawShake(MyeFeelDesc& d)
{
    ImGui::PushID("shake");
    DragTicks(Tr(StrId::Feel_Duration), d.shakeTicks);
    DragF(Tr(StrId::Feel_ShakePos), d.shakePosAmp, 0.002f, 0.0f, 10.0f);
    DragF(Tr(StrId::Feel_ShakeRot), d.shakeRotAmpDeg, 0.05f, 0.0f, 45.0f, "%.2f");
    DragF(Tr(StrId::Feel_ShakeFreq), d.shakeFrequencyHz, 0.1f, 0.0f, 120.0f, "%.1f");
    ImGui::TextUnformatted(Tr(StrId::Feel_Envelope));
    DrawCurveEditor("envelope", d.shakeEnvelope);
    ImGui::PopID();
}

void FeelAssetInspector::DrawKick(MyeFeelDesc& d)
{
    ImGui::PushID("kick");
    DragTicks(Tr(StrId::Feel_Duration), d.kickTicks);
    float off[3] = { d.kickOffset.x, d.kickOffset.y, d.kickOffset.z };
    ImGui::SetNextItemWidth(kFieldWidth * 1.5f);
    if (ImGui::DragFloat3(Tr(StrId::Feel_KickOffset), off, 0.002f, -10.0f, 10.0f, "%.3f")) {
        d.kickOffset = { off[0], off[1], off[2] };
    }
    DragF(Tr(StrId::Feel_KickPitch), d.kickPitchDeg, 0.05f, -45.0f, 45.0f, "%.2f");
    DragF(Tr(StrId::Feel_KickFov), d.kickFovDeg, 0.05f, -45.0f, 45.0f, "%.2f");
    ImGui::TextUnformatted(Tr(StrId::Feel_Curve));
    DrawCurveEditor("curve", d.kickCurve);
    ImGui::PopID();
}

void FeelAssetInspector::DrawRumble(MyeFeelDesc& d)
{
    ImGui::PushID("rumble");
    DragTicks(Tr(StrId::Feel_Duration), d.rumbleTicks);
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::SliderFloat(Tr(StrId::Feel_RumbleLow), &d.rumbleLow, 0.0f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::SliderFloat(Tr(StrId::Feel_RumbleHigh), &d.rumbleHigh, 0.0f, 1.0f, "%.2f");
    ImGui::TextUnformatted(Tr(StrId::Feel_Curve));
    DrawCurveEditor("curve", d.rumbleCurve);
    ImGui::PopID();
}

void FeelAssetInspector::DrawFlash(MyeFeelDesc& d)
{
    ImGui::PushID("flash");
    DragTicks(Tr(StrId::Feel_Duration), d.flashTicks);
    DragF(Tr(StrId::Feel_FlashVignette), d.flashVignette, 0.005f, -4.0f, 4.0f);
    DragF(Tr(StrId::Feel_FlashChroma), d.flashChromAberration, 0.0005f, -4.0f, 4.0f, "%.4f");
    DragF(Tr(StrId::Feel_FlashExposure), d.flashExposure, 0.01f, -1.0f, 4.0f);
    DragF(Tr(StrId::Feel_FlashSaturation), d.flashSaturation, 0.01f, -1.0f, 4.0f);
    float col[4] = { d.flashColor.r, d.flashColor.g, d.flashColor.b, d.flashColor.a };
    if (ImGui::ColorEdit4(Tr(StrId::Feel_FlashColor), col, ImGuiColorEditFlags_Float)) {
        d.flashColor = { col[0], col[1], col[2], col[3] };
    }
    ImGui::TextUnformatted(Tr(StrId::Feel_Curve));
    DrawCurveEditor("curve", d.flashCurve);
    ImGui::PopID();
}

void FeelAssetInspector::Draw(EngineContext& ctx, UndoStack& undo, const std::wstring& path)
{
    namespace fs = std::filesystem;
    FeelLibrary* lib = feellib::Library();
    FeelPreset* preset = (valid_ && lib != nullptr) ? lib->GetMutable(hash_) : nullptr;
    if (preset == nullptr) {
        ImGui::TextDisabled("%s", Tr(StrId::Feel_Failed));
        return;
    }
    MyeFeelDesc& d = preset->desc;
    // Undo: このフレームの編集前の値 (ドラッグは離したフレームで 1 件に畳む = DocUndoRecorder)
    const nlohmann::json before = FeelLibrary::ToJson(d);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr(StrId::Feel_Note));
    ImGui::PopTextWrapPos();

    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecTime), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawTime(d);
    }
    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecShake), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawShake(d);
    }
    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecKick))) {
        DrawKick(d);
    }
    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecRumble))) {
        DrawRumble(d);
    }
    if (ImGui::CollapsingHeader(Tr(StrId::Feel_SecFlash))) {
        DrawFlash(d);
    }

    ImGui::Separator();
    // プレビュー: 今の値の揺れ・キック・振動・画面効果を、EngineLoop の**描画専用のプレビュー表**へ鳴らす。
    // sim の表 (Scene::Feel) と TimeControl には触れない — 描画フレームから hash 対象の状態を書くと、録画・検証・
    // タイムトラベルの再シミュレーションで再現できない (AGENTS 4.2)。時間効果 (ヒットストップ / スロー) は sim を
    // 変えるので、プレビューでは鳴らさない (スクリプトか FeelTrigger から鳴らして確かめる)。
    // Play 中だけ: プレビュー表の経過は sim の tick で進むので、編集中は止まったままになる
    const bool canPreview = ctx.feelPreview != nullptr && ctx.simulateScripts;
    ImGui::BeginDisabled(!canPreview);
    if (ImGui::Button(Tr(StrId::Feel_Preview), ImVec2(110, 0)) && canPreview) {
        TimeControl unused; // プレビュー表は時間効果を受けない (PresentationOnly で外してある)
        ctx.feelPreview->Play(PresentationOnly(d), 1.0f, preset->nameHash, unused);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", Tr(StrId::Feel_PreviewTip));
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        SanitizeFeelDesc(d); // 手入力の NaN / 範囲外をファイルへ焼かない
        std::ofstream out(fs::path(path), std::ios::binary);
        if (out) {
            out << FeelLibrary::ToJson(d).dump(2);
            out.close();
            saved_ = d;
            MYE_LOG_INFO("feel saved: %s", WideToUtf8(path).c_str());
        } else {
            MYE_LOG_ERROR("%s: %s", Tr(StrId::Log_WriteFeelFail), WideToUtf8(path).c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        d = saved_; // ディスクの値へ戻す (この戻しも Undo できる)
    }
    if (std::memcmp(&d, &saved_, sizeof(MyeFeelDesc)) != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::Feel_Unsaved));
    }
    undoRecorder_.Observe(undo, "Feel", UndoDocOp::Kind::Feel, hash_, before, FeelLibrary::ToJson(d),
                          ImGui::IsAnyItemActive());
}

} // namespace mye
