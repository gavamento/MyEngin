//====================================================================================
//                          ComputeInspectorSection.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          スクリプトのインスペクターの Compute 欄の描画
//====================================================================================
#include "Editor/Windows/Scene/ComputeInspectorSection.h"

#include <cfloat>
#include <string>

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Compute/ComputeService.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

#include "imgui.h"

namespace mye {

const char* ComputeRowStateText(compute::RowState state)
{
    switch (state) {
    case compute::RowState::FailedNotDeclared: return Tr(StrId::Insp_ComputeStateNotDeclared);
    case compute::RowState::DeclaredNoKernel: return Tr(StrId::Insp_ComputeStateNoKernel);
    case compute::RowState::LintViolation: return Tr(StrId::Insp_ComputeStateLint);
    case compute::RowState::Ran: return Tr(StrId::Insp_ComputeStateRan);
    case compute::RowState::NotRun: return Tr(StrId::Insp_ComputeStateNotRun);
    }
    return "?";
}

const char* ComputeRowTargetText(compute::RowTarget target)
{
    switch (target) {
    case compute::RowTarget::None: return Tr(StrId::Insp_ComputeTargetNone);
    case compute::RowTarget::Cpu: return Tr(StrId::Insp_ComputeTargetCpu);
    case compute::RowTarget::Gpu: return Tr(StrId::Insp_ComputeTargetGpu);
    case compute::RowTarget::SubstitutedCpu: return Tr(StrId::Insp_ComputeTargetSubst);
    case compute::RowTarget::Failed: return Tr(StrId::Insp_ComputeTargetFailed);
    case compute::RowTarget::Skipped: return Tr(StrId::Insp_ComputeTargetSkipped);
    }
    return "?";
}

const char* ComputeRowVersionText(compute::RowVersion version, compute::CallerKind lane)
{
    switch (version) {
    case compute::RowVersion::Unknown: return Tr(StrId::Insp_ComputeVersionUnknown);
    case compute::RowVersion::Match: return Tr(StrId::Insp_ComputeVersionMatch);
    case compute::RowVersion::Mismatch:
        return lane == compute::CallerKind::Cpp ? Tr(StrId::Insp_ComputeVersionStaleCpp)
                                                : Tr(StrId::Insp_ComputeVersionStaleCs);
    case compute::RowVersion::NoCpuVersion: return Tr(StrId::Insp_ComputeVersionNone);
    }
    return "?";
}

std::string ComputeRowDetail(const compute::ComputeInspectorRow& row, compute::CallerKind lane)
{
    std::string detail;
    switch (row.state) {
    case compute::RowState::FailedNotDeclared:
        detail = lane == compute::CallerKind::Cpp ? Tr(StrId::Insp_ComputeDeclareCpp) : Tr(StrId::Insp_ComputeDeclareCs);
        break;
    case compute::RowState::DeclaredNoKernel:
        detail = Tr(StrId::Insp_ComputeFileMissing);
        break;
    case compute::RowState::LintViolation:
        detail = row.violation;
        break;
    default:
        break;
    }
    if (row.lastFail != compute::FailReason::None && row.target == compute::RowTarget::Failed) {
        if (!detail.empty()) {
            detail += "\n";
        }
        detail += std::string(Tr(StrId::Insp_ComputeLastFail)) + ": " + compute::FailReasonName(row.lastFail);
    }
    return detail;
}

namespace {

ImVec4 StateColor(compute::RowState state)
{
    switch (state) {
    case compute::RowState::FailedNotDeclared:
    case compute::RowState::DeclaredNoKernel:
    case compute::RowState::LintViolation: return themeColor::Error;
    case compute::RowState::Ran: return themeColor::Success;
    case compute::RowState::NotRun: break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

ImVec4 VersionColor(compute::RowVersion version)
{
    switch (version) {
    case compute::RowVersion::Mismatch: return themeColor::Warning;
    case compute::RowVersion::Match: return themeColor::Success;
    default: break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

} // namespace

void DrawComputeInspectorSection(EngineContext& ctx, const char* scriptType, compute::CallerKind lane)
{
    if (ctx.computeService == nullptr || scriptType == nullptr) {
        return;
    }
    compute::ComputeService& service = *ctx.computeService;
    const std::vector<compute::ComputeInspectorRow> rows = compute::BuildComputeInspectorRows(service, lane, scriptType);
    if (rows.empty()) {
        return; // 使わないスクリプトに空の欄を出さない
    }
    ImGui::PushID("compute");
    if (ImGui::TreeNodeEx(Tr(StrId::Insp_Compute), ImGuiTreeNodeFlags_DefaultOpen)) {
        // 狭いインスペクターでも全項目が読めるよう、カーネルごとに「見出し + 項目の一覧」で出す
        for (const compute::ComputeInspectorRow& row : rows) {
            ImGui::PushID(row.kernel.c_str());
            ImGui::TextUnformatted(row.kernel.c_str());
            ImGui::SameLine();
            ImGui::TextColored(StateColor(row.state), "%s", ComputeRowStateText(row.state));
            const std::string detail = ComputeRowDetail(row, lane);
            if (!detail.empty()) {
                // 直し方の文は長いので、TextDisabled のまま折り返す
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", detail.c_str());
                ImGui::PopStyleColor();
            }
            if (ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                const auto label = [](const char* text) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", text);
                    ImGui::TableNextColumn();
                };
                label(Tr(StrId::Insp_ComputeColTarget));
                ImGui::TextUnformatted(ComputeRowTargetText(row.target));
                label(Tr(StrId::Insp_ComputeColElements));
                ImGui::Text("%u", row.elements);
                label(Tr(StrId::Insp_ComputeColTransfer));
                ImGui::Text("%llu / %llu", static_cast<unsigned long long>(row.uploadBytes),
                            static_cast<unsigned long long>(row.readbackBytes));
                label(Tr(StrId::Insp_ComputeColTime));
                if (row.target == compute::RowTarget::Gpu) {
                    ImGui::Text("%llu", static_cast<unsigned long long>(row.gpuDispatches));
                } else {
                    ImGui::Text("%.3f", row.cpuMs);
                }
                label(Tr(StrId::Insp_ComputeColVersion));
                ImGui::TextColored(VersionColor(row.version), "%s", ComputeRowVersionText(row.version, lane));
                // 上書きはカーネル単位。どのスクリプトで変えても、そのカーネルを使うすべてのスクリプトに効く
                label(Tr(StrId::Insp_ComputeColOverride));
                const char* labels[2] = { Tr(StrId::Insp_ComputeOverrideAuto), Tr(StrId::Insp_ComputeOverrideCpu) };
                int selected = row.overrideCpu ? 1 : 0;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##override", &selected, labels, 2)) {
                    service.SetKernelOverrideCpu(row.kernel, selected == 1);
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::TextWrapped("%s", Tr(StrId::Insp_ComputeOverrideNote));
        ImGui::TreePop();
    }
    ImGui::PopID();
}

} // namespace mye
