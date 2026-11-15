//====================================================================================
//                          AiChangesWindow.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Changes 窓 (状態の帯・セッション・変更の一覧・詳細と差分)
//====================================================================================
#include "Editor/Windows/Project/AiChangesWindow.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "imgui.h"
#include "fontawesome/IconsFontAwesome6.h"

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

namespace mye {

namespace {

ImVec4 ToImVec4(const OverlayColor& c)
{
    return ImVec4(c.r, c.g, c.b, c.a);
}

// 一覧の仮想行の印
constexpr int kItemMainHeader = -1;
constexpr int kItemUncertainHeader = -2;
constexpr int kItemEditorHeader = -3;

const char* const kKindFilters[] = { "", "added", "modified", "deleted", "renamed" };
const char* const kAttributionFilters[] = { "", "verified", "claimed", "inferred", "unknown" };

ImVec4 KindColor(const std::string& changeType)
{
    if (changeType == "added") {
        return themeColor::Success;
    }
    if (changeType == "deleted") {
        return themeColor::Error;
    }
    if (changeType == "renamed") {
        return themeColor::Prefab;
    }
    return themeColor::Warning;
}

ImVec4 AttributionColor(const std::string& attribution)
{
    if (attribution == "verified") {
        return themeColor::Success;
    }
    if (attribution == "inferred") {
        return themeColor::Warning;
    }
    if (attribution == "unknown") {
        return themeColor::TextDim;
    }
    return themeColor::Text;
}

// 状態の帯の色。動作中は穏やか、使えない状態は警告色
ImVec4 BannerColor(TrackState state)
{
    switch (state) {
    case TrackState::Ready: return themeColor::Success;
    case TrackState::Disabled:
    case TrackState::NoProject:
    case TrackState::Connecting: return themeColor::TextDim;
    case TrackState::NoService:
    case TrackState::ProtoMismatch:
    case TrackState::WrongProject:
    case TrackState::DbDegraded:
    case TrackState::Disconnected: break;
    }
    return themeColor::Warning;
}

const char* KindFilterLabel(const std::string& value)
{
    if (value.empty()) {
        return Tr(StrId::AiChg_FilterAny);
    }
    // 一覧の種類の文面と同じ表を引く (値が未知でも落ちない)
    static thread_local std::string cache;
    cache = KindLabel(value);
    return cache.c_str();
}

const char* AttributionFilterLabel(const std::string& value)
{
    if (value.empty()) {
        return Tr(StrId::AiChg_FilterAny);
    }
    static thread_local std::string cache;
    cache = AttributionLabel(value);
    return cache.c_str();
}

} // namespace

std::vector<DiffHunkInput> ToDiffHunks(const std::vector<TrackHunk>& hunks)
{
    std::vector<DiffHunkInput> out;
    out.reserve(hunks.size());
    for (const TrackHunk& h : hunks) {
        DiffHunkInput in;
        in.oldStart = h.oldStart;
        in.oldCount = h.oldCount;
        in.newStart = h.newStart;
        in.newCount = h.newCount;
        in.before = h.beforeText;
        in.after = h.afterText;
        in.contextPre = h.contextPre;
        in.contextPost = h.contextPost;
        in.eolOnly = h.eolOnly;
        in.oversize = h.oversize;
        in.beforeBytes = h.beforeBytes;
        in.afterBytes = h.afterBytes;
        out.push_back(std::move(in));
    }
    return out;
}

void AiChangesWindow::SyncInterest(ChangeModel& model, TrackState state) const
{
    // 閉じている間は何も取りに行かない。開いていても、サービスと話せない状態では見る部分が無い
    const bool visible = open && (state == TrackState::Ready || state == TrackState::DbDegraded || state == TrackState::Disconnected);
    model.SetInterest(visible ? kTrackAllParts : 0u);
}

void AiChangesWindow::ApplyFilter(ChangeModel& model)
{
    TrackListFilter filter;
    if (scope_ == Scope::Session) {
        filter.sessionId = sessionId_;
    }
    filter.path = pathFilter_;
    filter.changeType = kindFilter_;
    if (scope_ == Scope::Uncertain) {
        // 「セッションなし (不明 / 推定)」は、読み込み済みの行から拾わず、サービスの絞り込みで取る
        filter.attributions = { "unknown", "inferred" };
    } else if (!attributionFilter_.empty()) {
        filter.attributions = { attributionFilter_ };
    }
    model.SetFilter(filter);
}

void AiChangesWindow::SelectScope(ChangeModel& model, Scope scope, const std::string& sessionId)
{
    if (scope_ != scope || sessionId_ != sessionId) {
        scope_ = scope;
        sessionId_ = sessionId;
        ApplyFilter(model);
    }
}

void AiChangesWindow::OnImGui(ChangeTrackingSession& session, const AiChangesHost& host)
{
    ChangeModel& model = session.Model();
    SyncInterest(model, session.State());
    session.Overlay().SetWindowOpen(open); // 強調が切でも、窓が開いていれば帯の件数のために取る
    if (!open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(980.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Win_AiChanges), &open)) {
        ImGui::End();
        return;
    }
    // 状態が変わった後の最初のフレームで、見る部分を合わせる (Begin の結果で窓が畳まれていれば見ていない)
    DrawHeader(session, host);

    const TrackState state = session.State();
    const bool hasData = model.Status().valid && (state == TrackState::Ready || state == TrackState::DbDegraded || state == TrackState::Disconnected);
    if (hasData) {
        DrawSceneBand(session);
        ImGui::Separator();
        const float height = (std::max)(ImGui::GetContentRegionAvail().y, 120.0f);
        if (ImGui::BeginTable("##aichg", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0.0f, height))) {
            ImGui::TableSetupColumn("##sessions", ImGuiTableColumnFlags_WidthStretch, 0.22f);
            ImGui::TableSetupColumn("##changes", ImGuiTableColumnFlags_WidthStretch, 0.34f);
            ImGui::TableSetupColumn("##detail", ImGuiTableColumnFlags_WidthStretch, 0.44f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawSessions(model);
            ImGui::TableNextColumn();
            DrawChanges(model);
            ImGui::TableNextColumn();
            DrawDetail(session, host);
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void AiChangesWindow::DrawHeader(ChangeTrackingSession& session, const AiChangesHost& host)
{
    const TrackState state = session.State();
    const std::string dbReason = session.Model().Status().dbReason;
    const std::string banner = StateBannerText(state, host.allowTracking, dbReason);
    ImGui::PushStyleColor(ImGuiCol_Text, BannerColor(state));
    ImGui::TextWrapped("%s", banner.c_str());
    ImGui::PopStyleColor();

    // 動作中の要約 (進捗・追跡中のファイル・Blob の総量・欠落区間・監視の縮退)
    if (state == TrackState::Ready || state == TrackState::DbDegraded) {
        for (const std::string& line : StatusLines(session.Model().Status())) {
            ImGui::TextDisabled("%s", line.c_str());
        }
    }

    // 有効・無効の切り替え。自動実行の起動では効かないので、出さずに理由だけ
    if (!host.allowTracking) {
        ImGui::BeginDisabled();
        bool shown = host.enabled;
        ImGui::Checkbox(Tr(StrId::AiChg_Enable), &shown);
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_NonInteractiveNote));
    } else {
        bool value = host.enabled;
        if (ImGui::Checkbox(Tr(StrId::AiChg_Enable), &value) && value != host.enabled) {
            if (value) {
                confirmRequested_ = true; // 有効にするときは 1 回確認する (最初のベースラインで内容を複製するため)
            } else if (host.setEnabled) {
                host.setEnabled(false);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::AiChg_EnableNote));
        }
    }
    if (confirmRequested_) {
        ImGui::OpenPopup(Tr(StrId::AiChg_ConfirmTitle));
        confirmRequested_ = false;
    }
    if (ImGui::BeginPopupModal(Tr(StrId::AiChg_ConfirmTitle), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(Tr(StrId::AiChg_ConfirmBody));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_EnableNote));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(Tr(StrId::AiChg_ConfirmOk))) {
            if (host.setEnabled) {
                host.setEnabled(true);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Common_Cancel))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void AiChangesWindow::DrawSessions(ChangeModel& model)
{
    ImGui::TextUnformatted(Tr(StrId::AiChg_Sessions));
    if (!ImGui::BeginChild("##sessionlist", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
        ImGui::EndChild();
        return;
    }
    auto pick = [&](Scope scope, const std::string& id) { SelectScope(model, scope, id); };
    if (ImGui::Selectable(Tr(StrId::AiChg_AllChanges), scope_ == Scope::All)) {
        pick(Scope::All, std::string());
    }
    if (ImGui::Selectable(Tr(StrId::AiChg_UncertainScope), scope_ == Scope::Uncertain)) {
        pick(Scope::Uncertain, std::string());
    }
    ImGui::Separator();
    const std::vector<TrackSessionRow>& sessions = model.Sessions();
    if (sessions.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_NoSessions));
    }
    const float lineH = ImGui::GetTextLineHeight();
    for (const TrackSessionRow& s : sessions) {
        const SessionRowText text = FormatSessionRow(s);
        ImGui::PushID(s.sessionId.c_str());
        if (ImGui::Selectable("##session", scope_ == Scope::Session && sessionId_ == s.sessionId, 0, ImVec2(0.0f, lineH * 2.0f + 4.0f))) {
            pick(Scope::Session, s.sessionId);
        }
        // 文字は項目の上へ描く (カーソルを動かさない)
        const ImVec2 min = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::string head = text.title + "  [" + text.status + "]";
        const std::string tail = text.time + "  " + text.count;
        dl->AddText(ImVec2(min.x + 4.0f, min.y + 1.0f), ImGui::GetColorU32(ImGuiCol_Text), head.c_str());
        dl->AddText(ImVec2(min.x + 4.0f, min.y + 3.0f + lineH), ImGui::GetColorU32(ImGuiCol_TextDisabled), tail.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void AiChangesWindow::DrawChanges(ChangeModel& model)
{
    ImGui::TextUnformatted(Tr(StrId::AiChg_Changes));

    // フィルタ: パスは入力を終えたとき、種類と帰属は選んだとき
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##pathfilter", Tr(StrId::AiChg_FilterPath), pathFilter_, sizeof(pathFilter_));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ApplyFilter(model);
    }
    const float comboW = ImGui::GetContentRegionAvail().x * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f;
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::BeginCombo("##kindfilter", KindFilterLabel(kindFilter_))) {
        for (const char* value : kKindFilters) {
            if (ImGui::Selectable(KindFilterLabel(value), kindFilter_ == value)) {
                kindFilter_ = value;
                ApplyFilter(model);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    // 「セッションなし」の項目は帰属を不明 / 推定に決めて取るので、帰属の絞り込みは効かない
    ImGui::BeginDisabled(scope_ == Scope::Uncertain);
    if (ImGui::BeginCombo("##attrfilter", AttributionFilterLabel(attributionFilter_))) {
        for (const char* value : kAttributionFilters) {
            if (ImGui::Selectable(AttributionFilterLabel(value), attributionFilter_ == value)) {
                attributionFilter_ = value;
                ApplyFilter(model);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::Checkbox(Tr(StrId::AiChg_MixSections), &mixUncertain_);
    ImGui::Checkbox(Tr(StrId::AiChg_ShowEditor), &showEditor_);
    if (model.ListIncomplete()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::AiChg_ListIncomplete));
        ImGui::PopStyleColor();
    }

    const std::vector<TrackChangeRow>& rows = model.Changes();
    // 仮想行 (見出しと変更) を組む。節への振り分けは添字だけで、本文を触らない
    items_.clear();
    size_t hiddenEditor = 0;
    if (scope_ == Scope::Uncertain) {
        // サービスが不明 / 推定に絞って返した行をそのまま並べる (節に分けない)
        for (size_t i = 0; i < rows.size(); ++i) {
            items_.push_back(static_cast<int>(i));
        }
    } else {
        const ChangePartition parts = PartitionChanges(rows, mixUncertain_);
        const bool showEditor = showEditor_ && !parts.editor.empty();
        hiddenEditor = showEditor_ ? 0 : parts.editor.size();
        if (!parts.main.empty() && (!parts.uncertain.empty() || showEditor)) {
            items_.push_back(kItemMainHeader);
        }
        for (const size_t i : parts.main) {
            items_.push_back(static_cast<int>(i));
        }
        if (!parts.uncertain.empty()) {
            items_.push_back(kItemUncertainHeader);
            for (const size_t i : parts.uncertain) {
                items_.push_back(static_cast<int>(i));
            }
        }
        if (showEditor) {
            items_.push_back(kItemEditorHeader);
            for (const size_t i : parts.editor) {
                items_.push_back(static_cast<int>(i));
            }
        }
    }
    if (hiddenEditor > 0) {
        ImGui::TextDisabled("%s", (std::to_string(hiddenEditor) + " " + Tr(StrId::AiChg_EditorHidden)).c_str());
    }

    if (!ImGui::BeginChild("##changelist", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
        ImGui::EndChild();
        return;
    }
    if (items_.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_NoChanges));
    }
    // 見出しも行も同じ高さ (クリッパーは一定の高さを前提にする)
    const float lineH = ImGui::GetTextLineHeight();
    const float itemH = lineH * 2.0f + 4.0f;
    const float charGap = ImGui::CalcTextSize(" ").x * 1.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(items_.size()), itemH + ImGui::GetStyle().ItemSpacing.y);
    int lastShown = -1;
    while (clipper.Step()) {
        for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
            lastShown = n;
            const int item = items_[static_cast<size_t>(n)];
            if (item < 0) {
                ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, itemH));
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                const char* title = Tr(item == kItemMainHeader        ? StrId::AiChg_SectionMain
                                       : item == kItemUncertainHeader ? StrId::AiChg_SectionUncertain
                                                                      : StrId::AiChg_SectionEditor);
                dl->AddText(ImVec2(min.x + 2.0f, min.y + itemH - lineH - 4.0f), colDim, title);
                dl->AddLine(ImVec2(min.x, max.y - 2.0f), ImVec2(max.x, max.y - 2.0f), ImGui::GetColorU32(ImGuiCol_Separator));
                continue;
            }
            const TrackChangeRow& row = rows[static_cast<size_t>(item)];
            const ChangeRowText text = FormatChangeRow(row);
            ImGui::PushID(static_cast<int>(row.revisionId));
            if (ImGui::Selectable("##row", model.SelectedRevision() == row.revisionId, 0, ImVec2(0.0f, itemH))) {
                model.SelectRevision(row.revisionId);
            }
            // 文字は項目の上へ描く (カーソルを動かさない)。1 行目 = パス、2 行目 = 種類・帰属・印・時刻
            const ImVec2 min = ImGui::GetItemRectMin();
            dl->AddText(ImVec2(min.x + 4.0f, min.y + 1.0f), colText, text.path.c_str());
            float x = min.x + 4.0f;
            const float y = min.y + 3.0f + lineH;
            auto put = [&](ImU32 col, const std::string& piece) {
                if (piece.empty()) {
                    return;
                }
                dl->AddText(ImVec2(x, y), col, piece.c_str());
                x += ImGui::CalcTextSize(piece.c_str()).x + charGap;
            };
            put(ImGui::GetColorU32(KindColor(row.changeType)), text.kind);
            put(ImGui::GetColorU32(AttributionColor(row.actor == "editor_user" || row.actor == "restore" ? std::string("editor") : row.attribution)),
                text.attribution);
            put(ImGui::GetColorU32(themeColor::Warning), text.completeness.empty() ? std::string() : "[" + text.completeness + "]");
            put(colDim, text.superseded.empty() ? std::string() : "[" + text.superseded + "]");
            put(colDim, text.time);
            ImGui::PopID();
        }
    }
    clipper.End();
    // 末尾まで来たら続きを読む (読み込み中は LoadMore が何もしない)。自動で足りなければボタン
    if (model.HasMoreChanges()) {
        if (lastShown >= static_cast<int>(items_.size()) - 2) {
            model.LoadMore();
        }
        if (ImGui::Button(Tr(StrId::AiChg_LoadMore))) {
            model.LoadMore();
        }
    }
    ImGui::EndChild();
}

void AiChangesWindow::DrawDetail(ChangeTrackingSession& session, const AiChangesHost& host)
{
    ChangeModel& model = session.Model();
    ImGui::TextUnformatted(Tr(StrId::AiChg_Detail));
    if (!ImGui::BeginChild("##detail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
        ImGui::EndChild();
        return;
    }
    const nlohmann::json& d = model.Detail();
    if (model.SelectedRevision() == 0 || !d.is_object()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_SelectRevision));
        ImGui::EndChild();
        return;
    }
    auto str = [&](const char* key) { return d.contains(key) && d[key].is_string() ? d[key].get<std::string>() : std::string(); };
    const std::string path = str("path");
    const std::string oldPath = str("oldPath");
    const std::string changeType = str("changeType");
    const std::string attribution = str("attribution");
    const std::string completeness = str("completeness");
    const std::string diffMode = str("diffMode");

    ImGui::TextUnformatted((changeType == "renamed" && !oldPath.empty() ? oldPath + " -> " + path : path).c_str());
    ImGui::TextColored(KindColor(changeType), "%s", KindLabel(changeType).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", FormatTimeMs(d.value("observedAt", int64_t(0))).c_str());

    // 帰属: 区分だけでなく、根拠の一覧と候補のセッション
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr(StrId::AiChg_Attribution));
    ImGui::SameLine();
    ImGui::TextColored(AttributionColor(attribution), "%s", AttributionLabel(attribution).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", ActorLabel(str("actor")).c_str());
    const std::string note = AttributionNote(str("actor"), attribution);
    if (!note.empty()) {
        // 説明は 1 文が長いので折り返す (狭い詳細の欄で右が切れない)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", note.c_str());
        ImGui::PopStyleColor();
    }
    std::string evidenceSource;
    ImGui::TextUnformatted(Tr(StrId::AiChg_Evidence));
    const nlohmann::json& evidence = d.contains("evidence") ? d["evidence"] : nlohmann::json();
    if (!evidence.is_array() || evidence.empty()) {
        ImGui::TextDisabled("  %s", Tr(StrId::AiChg_NoEvidence));
    } else {
        for (const nlohmann::json& e : evidence) {
            if (e.is_object() && e.value("kind", std::string()) == "observed" && e.contains("detail")) {
                evidenceSource = EvidenceSource(e["detail"]);
            }
            ImGui::Bullet();
            ImGui::TextWrapped("%s", EvidenceText(e).c_str());
        }
    }
    const nlohmann::json& candidates = d.contains("candidateSessions") ? d["candidateSessions"] : nlohmann::json();
    if (candidates.is_array() && !candidates.empty()) {
        ImGui::TextUnformatted(Tr(StrId::AiChg_Candidates));
        for (const nlohmann::json& c : candidates) {
            const std::string id = c.is_string() ? c.get<std::string>() : c.dump();
            std::string label = id.substr(0, 8);
            for (const TrackSessionRow& s : model.Sessions()) {
                if (s.sessionId == id && !s.provider.empty()) {
                    label = s.provider + " " + id.substr(0, 8);
                }
            }
            ImGui::BulletText("%s", label.c_str());
        }
    }

    // completeness が complete でないときは、その意味を必ず 1 行で出す
    const std::string completenessNote = CompletenessNote(completeness, evidenceSource);
    if (!completenessNote.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("[%s] %s", CompletenessLabel(completeness).c_str(), completenessNote.c_str());
        ImGui::PopStyleColor();
    }
    if (str("changeState") == "superseded") {
        ImGui::TextDisabled("[%s] %s", Tr(StrId::AiChg_Superseded), Tr(StrId::AiChg_SupersededNote));
    }

    // diff_mode ごとの理由。Blob が欠けているときは内容を表示できない
    ImGui::Spacing();
    const std::string modeNote = DiffModeNote(diffMode);
    if (!modeNote.empty()) {
        ImGui::TextWrapped("[%s] %s", DiffModeLabel(diffMode).c_str(), modeNote.c_str());
    }
    bool blobMissing = false;
    for (const char* side : { "before", "after" }) {
        if (d.contains(side) && d[side].is_object() && d[side].value("blobState", std::string()) == "missing") {
            blobMissing = true;
        }
    }
    if (blobMissing) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::AiChg_BlobMissing));
        ImGui::PopStyleColor();
    }

    // シーン / プレハブ / アクターの構造の変更 (describe に sceneSummary があるとき)
    if (d.contains("sceneSummary") && d["sceneSummary"].is_object()) {
        DrawSceneChanges(session, host);
    }

    // 表示の切り替えと、対象ファイルへの移動
    const bool hasLines = diffMode == "lines" || diffMode == "whole";
    if (hasLines) {
        if (ImGui::RadioButton(Tr(StrId::AiChg_LayoutInline), diff_.Layout() == DiffLayout::Inline)) {
            diff_.SetLayout(DiffLayout::Inline);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(Tr(StrId::AiChg_LayoutSplit), diff_.Layout() == DiffLayout::Split)) {
            diff_.SetLayout(DiffLayout::Split);
        }
        ImGui::SameLine();
    }
    const std::wstring absPath = ProjectFilePath(host.projectRoot, path);
    const bool canOpen = !absPath.empty() && changeType != "deleted";
    ImGui::BeginDisabled(!canOpen);
    if (ImGui::SmallButton(Tr(StrId::AiChg_OpenFile)) && host.openFile) {
        // Hunk の新しい側の先頭行で開く (選んだ Hunk、無ければ最初の Hunk)
        const std::vector<TrackHunk>& hunks = model.Hunks();
        int line = 1;
        if (!hunks.empty()) {
            const int selected = diff_.SelectedHunk();
            line = static_cast<int>(hunks[selected >= 0 && static_cast<size_t>(selected) < hunks.size() ? static_cast<size_t>(selected) : 0].newStart);
        }
        host.openFile(absPath, (std::max)(1, line));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(StrId::AiChg_ShowInExplorer)) && host.showInExplorer) {
        host.showInExplorer(absPath);
    }
    ImGui::EndDisabled();

    if (!hasLines) {
        ImGui::EndChild();
        return;
    }
    // Hunk の入れ替わりのときだけ DiffView の入力を作り直す (毎フレームは作らない)
    if (shownHunksSerial_ != model.HunksSerial()) {
        diff_.SetHunks(ToDiffHunks(model.Hunks()));
        shownHunksSerial_ = model.HunksSerial();
    }
    const char* empty = Tr(model.HunksFailed() ? StrId::AiChg_HunksFailed
                                               : (model.HunksComplete() ? StrId::AiChg_NoHunks : StrId::AiChg_HunksLoading));
    diff_.Draw("##diff", empty, 0.0f, 0.0f);
    ImGui::EndChild();
}

SceneChangeActivation AiChangesWindow::ActivateSceneChange(const TrackSceneChange& change, const SceneChangeOverlay& overlay,
                                                           const AiChangesHost& host)
{
    SceneChangeActivation result = SceneChangeActivation::Selected;
    const std::string& doc = change.resolvedDocumentId.empty() ? change.documentId : change.resolvedDocumentId;
    if (!change.hasFileId) {
        result = SceneChangeActivation::NoEntity;
    } else if (overlay.DocumentId().empty() || doc != overlay.DocumentId()) {
        result = SceneChangeActivation::NotOpen;
    } else if (!host.selectEntity || !host.selectEntity(static_cast<uint64_t>(change.fileId))) {
        result = SceneChangeActivation::EntityMissing;
    }
    lastActivation_ = result;
    hasActivation_ = true;
    return result;
}

// 開いているシーンについての帯: 強調の切り替え・件数・「ここまで確認済みにする」・不完全の注記
void AiChangesWindow::DrawSceneBand(ChangeTrackingSession& session)
{
    SceneChangeOverlay& overlay = session.Overlay();
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::AiChg_SceneBand));
    ImGui::SameLine();
    if (overlay.DocumentPath().empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_OverlayNoScene));
        return;
    }
    ImGui::TextDisabled("%s", WideToUtf8(std::filesystem::path(overlay.DocumentPath()).filename().wstring()).c_str());
    bool show = overlay.Show();
    if (ImGui::Checkbox(Tr(StrId::AiChg_OverlayShow), &show)) {
        overlay.SetShow(show);
    }
    ImGui::SameLine();
    DrawColorSettings(overlay);
    ImGui::SameLine();
    if (!overlay.Loaded()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_OverlayLoading));
    } else {
        const OverlayCounts counts = overlay.Counts();
        const OverlayPalette& palette = overlay.Palette();
        ImGui::TextColored(ToImVec4(palette.Get(OverlayColorId::Added)), "%s %d", Tr(StrId::AiChg_OverlayAdded), counts.added);
        ImGui::SameLine();
        ImGui::TextColored(ToImVec4(palette.Get(OverlayColorId::Modified)), "%s %d", Tr(StrId::AiChg_OverlayModified), counts.modified);
        ImGui::SameLine();
        ImGui::TextColored(ToImVec4(palette.Get(OverlayColorId::Removed)), "%s %d", Tr(StrId::AiChg_OverlayRemoved), counts.removed);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(overlay.DocumentId().empty() || overlay.LatestRevisionId() <= 0);
    if (ImGui::SmallButton(Tr(StrId::AiChg_OverlayAck))) {
        overlay.AcknowledgeUpTo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", Tr(StrId::AiChg_OverlayAckNote));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!overlay.HasAck());
    if (ImGui::SmallButton(Tr(StrId::AiChg_OverlayShowAll))) {
        overlay.ClearAck();
    }
    ImGui::EndDisabled();
    if (overlay.Incomplete()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::AiChg_OverlayIncomplete));
        ImGui::PopStyleColor();
    }
    if (overlay.Truncated()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::AiChg_OverlayTruncated));
        ImGui::PopStyleColor();
    }
    if (overlay.BoxStats().omitted > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s %d", Tr(StrId::AiChg_BoxesOmitted), overlay.BoxStats().omitted);
        ImGui::PopStyleColor();
    }
}

// 強調の 4 色の設定 (歯車のポップアップ)。ドラッグ中は表示だけ変え、マウスを離したときに保存する
void AiChangesWindow::DrawColorSettings(SceneChangeOverlay& overlay)
{
    constexpr const char* kPopupId = "##aichg_colors";
    if (ImGui::SmallButton(ICON_FA_GEAR "###aichg_colors_btn")) {
        ImGui::OpenPopup(kPopupId);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::AiChg_ColorSettings));
    }
    if (ImGui::BeginPopup(kPopupId)) {
        ImGui::TextUnformatted(Tr(StrId::AiChg_ColorSettings));
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_ColorNote));
        const StrId labels[kOverlayColorCount] = { StrId::AiChg_OverlayAdded, StrId::AiChg_OverlayModified, StrId::AiChg_OverlayRemoved,
                                                   StrId::AiChg_OverlayConflict };
        OverlayPalette working = overlay.Palette();
        bool changed = false;
        for (int i = 0; i < kOverlayColorCount; ++i) {
            ImGui::PushID(i);
            changed = ImGui::ColorEdit4(Tr(labels[i]), &working.colors[i].r) || changed;
            ImGui::PopID();
        }
        if (changed) {
            overlay.PreviewPalette(working);
        }
        if (ImGui::Button(Tr(StrId::AiChg_ColorReset))) {
            overlay.SetPalette(OverlayPalette());
        }
        ImGui::EndPopup();
    }
    if (overlay.PaletteUnsaved() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        overlay.CommitPalette();
    }
}

// 選んだ Revision の構造の変更の一覧。見えている行だけを組み立てる (数万件でも重くしない)
void AiChangesWindow::DrawSceneChanges(ChangeTrackingSession& session, const AiChangesHost& host)
{
    const ChangeModel& model = session.Model();
    const SceneChangeOverlay& overlay = session.Overlay();
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr(StrId::AiChg_SceneChanges));
    const std::vector<TrackSceneChange>& changes = model.SceneChanges();
    if (!model.SceneChangesComplete() && !model.SceneChangesFailed()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_SceneChangesLoading));
    }
    if (model.SceneChangesFailed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::AiChg_SceneChangesFailed));
        ImGui::PopStyleColor();
    }
    if (model.SceneChangesComplete() && changes.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_NoSceneChanges));
    }
    if (!changes.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_SceneClickHint));
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float height = (std::min)(static_cast<float>(changes.size()), 8.0f) * lineH + 6.0f;
        if (ImGui::BeginChild("##scenechanges", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(changes.size()), lineH);
            while (clipper.Step()) {
                for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                    const TrackSceneChange& c = changes[static_cast<size_t>(n)];
                    const OverlayEntity* known = c.hasFileId ? overlay.Find(static_cast<uint64_t>(c.fileId)) : nullptr;
                    const SceneChangeText text = FormatSceneChange(c, known != nullptr ? known->name : std::string());
                    std::string line = "[" + text.kind + "] " + text.target;
                    if (!text.component.empty() || !text.field.empty()) {
                        line += " " + text.component + (text.component.empty() || text.field.empty() ? "" : ".") + text.field;
                    }
                    if (!text.note.empty()) {
                        line += ": " + text.note;
                    } else if (!text.before.empty() && !text.after.empty()) {
                        line += ": " + text.before + " -> " + text.after;
                    } else if (!text.before.empty()) {
                        line += ": - " + text.before; // 削除された値
                    } else if (!text.after.empty()) {
                        line += ": + " + text.after;  // 追加された値
                    }
                    ImGui::PushID(n);
                    if (ImGui::Selectable(line.c_str(), false)) {
                        ActivateSceneChange(c, overlay, host);
                    }
                    ImGui::PopID();
                }
            }
            clipper.End();
        }
        ImGui::EndChild();
    }
    if (hasActivation_ && lastActivation_ != SceneChangeActivation::Selected && lastActivation_ != SceneChangeActivation::NoEntity) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(lastActivation_ == SceneChangeActivation::NotOpen ? StrId::AiChg_SceneNotOpen : StrId::AiChg_EntityGone));
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

} // namespace mye
