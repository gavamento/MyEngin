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
#include <iterator>

#include "imgui.h"
#include "fontawesome/IconsFontAwesome6.h"

#include "Editor/ChangeTracking/RestoreFormat.h"
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

// レビューの状態の色 (テーマの意味色。強調の 4 色 (追加・変更・削除・競合) とは別)
ImVec4 ReviewColor(const std::string& state)
{
    if (state == "approved" || state == "partially_approved") {
        return themeColor::Success;
    }
    if (state == "rejected" || state == "partially_rejected") {
        return themeColor::Error;
    }
    if (state == "needs_review") {
        return themeColor::Warning;
    }
    return themeColor::TextDim;
}

// 詳細の reviewLeaves の最初の葉の種類 (hunk / scene / revision)。無ければ空
std::string LeafKindOf(const nlohmann::json& detail)
{
    if (detail.is_object() && detail.contains("reviewLeaves") && detail["reviewLeaves"].is_array() && !detail["reviewLeaves"].empty()
        && detail["reviewLeaves"][0].is_object() && detail["reviewLeaves"][0].contains("kind") && detail["reviewLeaves"][0]["kind"].is_string()) {
        return detail["reviewLeaves"][0]["kind"].get<std::string>();
    }
    return std::string();
}

const char* const kReviewFilters[] = { "", "pending", "approved", "rejected", "needs_review", "partially_approved", "partially_rejected" };

const char* ReviewFilterLabel(const std::string& value)
{
    if (value.empty()) {
        return Tr(StrId::AiChg_FilterAny);
    }
    static thread_local std::string cache;
    cache = ReviewStateLabel(value);
    return cache.c_str();
}

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

void ApplyReviewBadges(std::vector<DiffHunkInput>& input, const std::vector<TrackHunk>& hunks)
{
    const size_t n = (std::min)(input.size(), hunks.size());
    for (size_t i = 0; i < n; ++i) {
        if (hunks[i].review.empty()) {
            continue;
        }
        input[i].badge = "[" + ReviewStateLabel(hunks[i].review) + "]";
        input[i].badgeColor = ImGui::GetColorU32(ReviewColor(hunks[i].review));
    }
}

void AiChangesWindow::SyncInterest(ChangeModel& model, TrackState state) const
{
    // 閉じている間は何も取りに行かない。開いていても、サービスと話せない状態では見る部分が無い
    const bool visible = open && (state == TrackState::Ready || state == TrackState::DbDegraded || state == TrackState::Disconnected);
    model.SetInterest(visible ? kTrackAllParts : 0u);
}

void AiChangesWindow::ApplyFilter(ChangeModel& model)
{
    multi_.clear(); // 一覧の中身が変わる
    anchor_ = 0;
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
    filter.review = reviewFilter_;
    model.SetFilter(filter);
}

void AiChangesWindow::SetReviewFilter(ChangeModel& model, const std::string& state)
{
    if (reviewFilter_ != state) {
        reviewFilter_ = state;
        ApplyFilter(model);
    }
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
    int64_t inspectFileId = 0;
    if (session.Overlay().TakeInspectRequest(inspectFileId) && ShowRemovedBeforeFromOverlay(session.Overlay(), inspectFileId)) {
        open = true; // Scene View のゴーストを選んだ: Before を見せるために開く
    }
    if (!open) {
        return;
    }
    restore_.Tick(RestoreIo()); // 今のファイルの読み取りを 1 塊だけ進める (フレームを止めない)
    TickRestoreRequest(session, host);
    ImGui::SetNextWindowSize(ImVec2(980.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Win_AiChanges), &open)) {
        ImGui::End();
        return;
    }
    // 状態が変わった後の最初のフレームで、見る部分を合わせる (Begin の結果で窓が畳まれていれば見ていない)
    DrawHeader(session, host);
    DrawRestorePopup(session, host);

    const TrackState state = session.State();
    const bool hasData = model.Status().valid && (state == TrackState::Ready || state == TrackState::DbDegraded || state == TrackState::Disconnected);
    if (hasData) {
        DrawBulkPopup(session);
        DrawSceneBand(session, host);
        DrawRemovedBefore(session, host);
        DrawReviewStatus(session);
        ImGui::Separator();
        const float height = (std::max)(ImGui::GetContentRegionAvail().y, 120.0f);
        if (ImGui::BeginTable("##aichg", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0.0f, height))) {
            ImGui::TableSetupColumn("##sessions", ImGuiTableColumnFlags_WidthStretch, 0.22f);
            ImGui::TableSetupColumn("##changes", ImGuiTableColumnFlags_WidthStretch, 0.34f);
            ImGui::TableSetupColumn("##detail", ImGuiTableColumnFlags_WidthStretch, 0.44f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawSessions(session);
            ImGui::TableNextColumn();
            DrawChanges(session);
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
    // 復元: 回復待ちがあれば知らせ、履歴の画面へ行ける (画面は RestoreRunWindow。この窓を閉じても回復の画面は起動時に出ている)
    if (host.restoreJournalPending && host.restoreJournalPending()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextUnformatted(Tr(StrId::AiChg_RcNotice));
        ImGui::PopStyleColor();
        if (host.openRestoreRecovery) {
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(StrId::AiChg_RcOpen))) {
                host.openRestoreRecovery();
            }
        }
    }
    if (host.openRestoreHistory && state != TrackState::Disabled && state != TrackState::NoProject) {
        if (ImGui::SmallButton(Tr(StrId::AiChg_RhButton))) {
            host.openRestoreHistory();
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

void AiChangesWindow::DrawSessions(ChangeTrackingSession& session)
{
    ChangeModel& model = session.Model();
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
    // 選んでいるセッション全体へのレビュー (件数を出して 1 回確認する)
    if (scope_ == Scope::Session && !sessionId_.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvBulkSession));
        if (const std::optional<ReviewAction> picked = DrawReviewButtons("sessionbulk", session.Review().Busy())) {
            RequestBulkSession(session, *picked);
        }
        ImGui::Separator();
    }
    const std::vector<TrackSessionRow>& sessions = model.Sessions();
    if (sessions.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_NoSessions));
    }
    const float lineH = ImGui::GetTextLineHeight();
    for (const TrackSessionRow& s : sessions) {
        const SessionRowText text = FormatSessionRow(s);
        ImGui::PushID(s.sessionId.c_str());
        const float sessionH = lineH * (text.review.empty() ? 2.0f : 3.0f) + 4.0f;
        if (ImGui::Selectable("##session", scope_ == Scope::Session && sessionId_ == s.sessionId, 0, ImVec2(0.0f, sessionH))) {
            pick(Scope::Session, s.sessionId);
        }
        // 文字は項目の上へ描く (カーソルを動かさない)
        const ImVec2 min = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::string head = text.title + "  [" + text.status + "]";
        const std::string tail = text.time + "  " + text.count;
        dl->AddText(ImVec2(min.x + 4.0f, min.y + 1.0f), ImGui::GetColorU32(ImGuiCol_Text), head.c_str());
        dl->AddText(ImVec2(min.x + 4.0f, min.y + 3.0f + lineH), ImGui::GetColorU32(ImGuiCol_TextDisabled), tail.c_str());
        if (!text.review.empty()) {
            dl->AddText(ImVec2(min.x + 4.0f, min.y + 5.0f + lineH * 2.0f), ImGui::GetColorU32(ReviewColor(s.review)), text.review.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void AiChangesWindow::DrawChanges(ChangeTrackingSession& session)
{
    ChangeModel& model = session.Model();
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
    ImGui::TextUnformatted(Tr(StrId::AiChg_RvFilter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##reviewfilter", ReviewFilterLabel(reviewFilter_))) {
        for (const char* value : kReviewFilters) {
            if (ImGui::Selectable(ReviewFilterLabel(value), reviewFilter_ == value)) {
                SetReviewFilter(model, value);
            }
        }
        ImGui::EndCombo();
    }
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
    // 複数の行を選んでいるとき: まとめてのレビュー
    if (multi_.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvMultiHint));
    } else {
        ImGui::Text("%d %s", static_cast<int>(multi_.size()), Tr(StrId::AiChg_RvSelectedCount));
        ImGui::SameLine();
        if (const std::optional<ReviewAction> picked = DrawReviewButtons("selbulk", session.Review().Busy())) {
            RequestBulkSelected(session, *picked);
        }
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
            if (ImGui::Selectable("##row", model.SelectedRevision() == row.revisionId || multi_.count(row.revisionId) > 0, 0, ImVec2(0.0f, itemH))) {
                const ImGuiIO& io = ImGui::GetIO();
                ClickChange(model, row.revisionId, io.KeyCtrl, io.KeyShift);
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
            put(ImGui::GetColorU32(ReviewColor(row.review)), text.review.empty() ? std::string() : "<" + text.review + ">");
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
    if (shownRevision_ != model.SelectedRevision()) {
        shownRevision_ = model.SelectedRevision();
        selectedHunkId_ = 0;
        selectedSceneId_ = 0;
        sceneScope_ = 0;
    }
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

    DrawReviewPanel(session);
    DrawRestoreButtons(session, host, str("actor"));

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
        std::vector<DiffHunkInput> input = ToDiffHunks(model.Hunks());
        ApplyReviewBadges(input, model.Hunks());
        diff_.SetHunks(input);
        shownHunksSerial_ = model.HunksSerial();
        // レビューのたびに詳細を取り直すので、選んでいた Hunk を ID で探して保つ
        const std::vector<TrackHunk>& hunks = model.Hunks();
        for (size_t i = 0; i < hunks.size() && selectedHunkId_ != 0; ++i) {
            if (hunks[i].hunkId == selectedHunkId_) {
                diff_.SelectHunk(static_cast<int>(i));
            }
        }
    }
    if (diffMode == "whole" && std::any_of(model.Hunks().begin(), model.Hunks().end(), [](const TrackHunk& h) { return h.oversize; })) {
        DrawBlobHeads(session);
    }
    DrawHunkReview(session);
    const char* empty = Tr(model.HunksFailed() ? StrId::AiChg_HunksFailed
                                               : (model.HunksComplete() ? StrId::AiChg_NoHunks : StrId::AiChg_HunksLoading));
    if (diff_.Draw("##diff", empty, 0.0f, 0.0f)) {
        SelectHunkIndex(model, diff_.SelectedHunk());
    }
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
void AiChangesWindow::DrawSceneBand(ChangeTrackingSession& session, const AiChangesHost& host)
{
    SceneChangeOverlay& overlay = session.Overlay();
    ImGui::Separator();
    if (host.externalChangePending) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::AiChg_PendingExternal));
        ImGui::PopStyleColor();
    }
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
    // 1 行に収まらないので、ゴーストの切り替えは次の行から (件数と「確認済み」のボタンが続く)
    bool ghosts = overlay.ShowGhosts();
    ImGui::BeginDisabled(!overlay.Show());
    if (ImGui::Checkbox(Tr(StrId::AiChg_GhostShow), &ghosts)) {
        overlay.SetShowGhosts(ghosts);
    }
    ImGui::EndDisabled();
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
    if (overlay.GhostStats().omitted > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s %d", Tr(StrId::AiChg_GhostOmitted), overlay.GhostStats().omitted);
        ImGui::PopStyleColor();
    }
    if (overlay.GhostStats().undrawable > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s %d", Tr(StrId::AiChg_GhostUndrawable), overlay.GhostStats().undrawable);
        ImGui::PopStyleColor();
    }
}

// ---- 削除されたエンティティの Before (M104r) ----

void AiChangesWindow::ShowRemovedBefore(const TrackSceneChange& removedRow, int64_t revisionId, const SceneChangeOverlay& overlay)
{
    removedView_ = BuildRemovedBeforeView(removedRow.fileId, revisionId, removedRow.hasBefore, removedRow.oversize, removedRow.beforeValue);
    const std::string& doc = removedRow.resolvedDocumentId.empty() ? removedRow.documentId : removedRow.resolvedDocumentId;
    removedNotOpen_ = overlay.DocumentId().empty() || doc != overlay.DocumentId();
    restorePending_ = false;
    restoreRowMissing_ = false;
}

bool AiChangesWindow::ShowRemovedBeforeFromOverlay(const SceneChangeOverlay& overlay, int64_t fileId)
{
    const OverlayEntity* entity = overlay.Find(static_cast<uint64_t>(fileId));
    if (entity == nullptr || entity->status != OverlayStatus::Removed) {
        return false;
    }
    // 削除した変更 = その削除を含む最後の Revision (消えたあとに別の変更は載らない)
    const int64_t revisionId = entity->revisionIds.empty() ? 0 : entity->revisionIds.back();
    removedView_ = BuildRemovedBeforeView(fileId, revisionId, entity->hasBefore, entity->beforeOversize, entity->before);
    removedNotOpen_ = overlay.DocumentId().empty();
    restorePending_ = false;
    restoreRowMissing_ = false;
    return true;
}

void AiChangesWindow::CloseRemovedBefore()
{
    removedView_ = RemovedBeforeView();
    restorePending_ = false;
    restoreRowMissing_ = false;
}

bool AiChangesWindow::RequestRestoreRemoved(ChangeTrackingSession& session)
{
    if (!removedView_.shown || removedView_.revisionId == 0 || removedNotOpen_) {
        return false;
    }
    restorePending_ = true;
    restoreRowMissing_ = false;
    restoreRevisionId_ = removedView_.revisionId;
    restoreFileId_ = removedView_.fileId;
    session.Model().SelectRevision(restoreRevisionId_);
    return true;
}

// 「このエンティティを復元...」の続き: 削除した変更の詳細とシーンの変更が揃ったら、その EntityRemoved の行でシーンの復元の画面を開く
void AiChangesWindow::TickRestoreRequest(ChangeTrackingSession& session, const AiChangesHost& host)
{
    if (!restorePending_) {
        return;
    }
    const ChangeModel& model = session.Model();
    if (model.SelectedRevision() != restoreRevisionId_ || model.SceneChangesFailed()) {
        restorePending_ = false; // 別の変更へ移った / 取れなかった
        restoreRowMissing_ = model.SceneChangesFailed();
        return;
    }
    const nlohmann::json& detail = model.Detail();
    const bool detailReady = detail.is_object() && detail.contains("revisionId") && detail["revisionId"].is_number_integer()
                             && detail["revisionId"].get<int64_t>() == restoreRevisionId_;
    if (!detailReady || !model.SceneChangesComplete()) {
        return; // 読み込み中
    }
    restorePending_ = false;
    const TrackSceneChange* row = nullptr;
    for (const TrackSceneChange& change : model.SceneChanges()) {
        if (change.changeType == "EntityRemoved" && change.hasFileId && change.fileId == restoreFileId_) {
            row = &change;
        }
    }
    if (row == nullptr) {
        restoreRowMissing_ = true;
        return;
    }
    selectedSceneId_ = row->sceneChangeId;
    if (host.openSceneRestore) {
        host.openSceneRestore(SceneRestoreScope::Entity, *row);
    }
}

// Before の読み取り専用の表示。文字だけで、入力のウィジェットは 1 つも置かない (記録を見るだけ)
void AiChangesWindow::DrawRemovedBefore(ChangeTrackingSession& session, const AiChangesHost& host)
{
    beforeLines_.clear();
    if (!removedView_.shown) {
        return;
    }
    const SceneChangeOverlay& overlay = session.Overlay();
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::AiChg_BeforeTitle));
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(StrId::AiChg_BeforeClose))) {
        CloseRemovedBefore();
        return;
    }
    auto line = [&](const std::string& text, bool dim = false) {
        beforeLines_.push_back(text);
        if (dim) {
            ImGui::TextDisabled("%s", text.c_str());
        } else {
            ImGui::TextUnformatted(text.c_str());
        }
    };
    const float height = ImGui::GetTextLineHeightWithSpacing() * 9.0f;
    if (ImGui::BeginChild("##removedbefore", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
        const RemovedBeforeView& v = removedView_;
        if (v.problem != BeforeProblem::None) {
            const StrId reason = v.problem == BeforeProblem::Oversize     ? StrId::AiChg_BeforeOversize
                                 : v.problem == BeforeProblem::NoBefore ? StrId::AiChg_BeforeNoBefore
                                                                        : StrId::AiChg_BeforeUnreadable;
            beforeLines_.push_back(Tr(reason));
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
            ImGui::TextWrapped("%s", Tr(reason));
            ImGui::PopStyleColor();
        } else {
            line(std::string(Tr(StrId::AiChg_BeforeName)) + " " + v.name + " (#" + std::to_string(v.fileId) + ")");
            line(std::string(Tr(StrId::AiChg_BeforeParent)) + " " + (v.hasParent ? std::to_string(v.parentFileId) : std::string(Tr(StrId::AiChg_BeforeNoParent))));
            for (const BeforeField& f : v.entityFields) {
                line(f.name + ": " + f.value, true);
            }
            for (const BeforeComponent& c : v.components) {
                beforeLines_.push_back(c.name);
                if (ImGui::TreeNodeEx(c.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (const BeforeField& f : c.fields) {
                        line(f.name.empty() ? f.value : f.name + ": " + f.value);
                    }
                    ImGui::TreePop();
                }
            }
            const uint32_t notes = overlay.GhostNotesFor(v.fileId);
            const std::pair<uint32_t, StrId> kNotes[] = { { kGhostNoteParentUnknown, StrId::AiChg_GhostNoteParent },
                                                          { kGhostNoteMeshCurrent, StrId::AiChg_GhostNoteMesh },
                                                          { kGhostNoteNoTransform, StrId::AiChg_GhostNoteNoTransform } };
            for (const auto& [bit, text] : kNotes) {
                if ((notes & bit) != 0) {
                    beforeLines_.push_back(Tr(text));
                    ImGui::TextWrapped("%s", Tr(text));
                }
            }
        }
    }
    ImGui::EndChild();
    // 復元への導線 (sub-16 のシーンの復元の、Entity の削除の取り消し)。ここから World は書かない: 押すと復元のプレビューが開くだけ
    ImGui::BeginDisabled(!host.openSceneRestore || removedView_.revisionId == 0 || removedNotOpen_ || restorePending_);
    if (ImGui::SmallButton(Tr(StrId::AiChg_BeforeRestore))) {
        RequestRestoreRemoved(session);
    }
    ImGui::EndDisabled();
    if (removedView_.revisionId == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_BeforeRestoreNeedRev));
    } else if (removedNotOpen_) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_SrNotOpenShort));
    } else if (restorePending_) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_BeforeRestoreLoading));
    } else if (restoreRowMissing_) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextUnformatted(Tr(StrId::AiChg_BeforeRestoreMissing));
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
                    if (!c.review.empty()) {
                        line = "<" + ReviewStateLabel(c.review) + "> " + line;
                    }
                    ImGui::PushID(n);
                    const bool tinted = c.review == "approved" || c.review == "rejected" || c.review == "needs_review";
                    if (tinted) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ReviewColor(c.review));
                    }
                    if (ImGui::Selectable(line.c_str(), selectedSceneId_ == c.sceneChangeId)) {
                        selectedSceneId_ = c.sceneChangeId;
                        ActivateSceneChange(c, overlay, host);
                        if (c.changeType == "EntityRemoved") {
                            ShowRemovedBefore(c, model.SelectedRevision(), overlay); // 実体が無いので選択はせず、Before を読み取り専用で見せる
                        }
                    }
                    if (tinted) {
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopID();
                }
            }
            clipper.End();
        }
        ImGui::EndChild();
    }
    DrawSceneReview(session);
    DrawSceneRestoreButton(session, host);
    if (hasActivation_ && lastActivation_ != SceneChangeActivation::Selected && lastActivation_ != SceneChangeActivation::NoEntity) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(lastActivation_ == SceneChangeActivation::NotOpen ? StrId::AiChg_SceneNotOpen : StrId::AiChg_EntityGone));
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

// ---- レビュー (M104o) ----

namespace {

// 構造の変更 1 行に付けられる範囲 (狭い順: フィールド → コンポーネント → エンティティ → Revision 全体)
std::vector<ReviewTarget> SceneReviewOptions(int64_t revisionId, const TrackSceneChange& change)
{
    std::vector<ReviewTarget> options;
    if (change.hasFileId) {
        if (!change.field.empty()) {
            options.push_back(ReviewManager::ForField(revisionId, change.fileId, change.component, change.field));
        }
        if (!change.component.empty()) {
            options.push_back(ReviewManager::ForComponent(revisionId, change.fileId, change.component));
        }
        options.push_back(ReviewManager::ForEntity(revisionId, change.fileId));
    }
    options.push_back(ReviewManager::ForRevision(revisionId));
    return options;
}

bool IsReviewableDetail(const nlohmann::json& detail, int64_t revisionId)
{
    return detail.is_object() && detail.contains("reviewable") && detail["reviewable"].is_boolean() && detail["reviewable"].get<bool>()
           && detail.contains("revisionId") && detail["revisionId"].is_number_integer() && detail["revisionId"].get<int64_t>() == revisionId;
}

} // namespace

void AiChangesWindow::SetReviewNote(const std::string& note)
{
    std::snprintf(reviewNote_, sizeof(reviewNote_), "%s", note.c_str());
}

void AiChangesWindow::ClickChange(ChangeModel& model, int64_t revisionId, bool ctrl, bool shift)
{
    if (ctrl) {
        if (multi_.empty() && model.SelectedRevision() != 0) {
            multi_.insert(model.SelectedRevision()); // いま詳細に出している行も選択に入れる
        }
        if (multi_.erase(revisionId) == 0) {
            multi_.insert(revisionId);
        }
        anchor_ = revisionId;
        return;
    }
    if (shift && anchor_ != 0) {
        // 表示順で、起点から押した行までの変更の行
        const std::vector<TrackChangeRow>& rows = model.Changes();
        std::vector<int64_t> order;
        if (items_.empty()) {
            for (const TrackChangeRow& row : rows) {
                order.push_back(row.revisionId);
            }
        } else {
            for (const int item : items_) {
                if (item >= 0 && static_cast<size_t>(item) < rows.size()) {
                    order.push_back(rows[static_cast<size_t>(item)].revisionId);
                }
            }
        }
        const auto from = std::find(order.begin(), order.end(), anchor_);
        const auto to = std::find(order.begin(), order.end(), revisionId);
        if (from != order.end() && to != order.end()) {
            multi_.clear();
            const auto lo = from < to ? from : to;
            const auto hi = from < to ? to : from;
            for (auto it = lo; it <= hi; ++it) {
                multi_.insert(*it);
            }
            return;
        }
    }
    multi_.clear();
    anchor_ = revisionId;
    model.SelectRevision(revisionId);
}

bool AiChangesWindow::SubmitReview(ChangeTrackingSession& session, std::vector<ReviewTarget> targets, ReviewAction action)
{
    const bool sent = session.Review().Submit(std::move(targets), action, reviewNote_);
    noteSent_ = sent && reviewNote_[0] != '\0';
    return sent;
}

bool AiChangesWindow::ReviewRevision(ChangeTrackingSession& session, ReviewAction action)
{
    const ChangeModel& model = session.Model();
    if (model.SelectedRevision() == 0 || !IsReviewableDetail(model.Detail(), model.SelectedRevision())) {
        return false;
    }
    return SubmitReview(session, { ReviewManager::ForRevision(model.SelectedRevision()) }, action);
}

void AiChangesWindow::SelectHunkIndex(const ChangeModel& model, int index)
{
    const std::vector<TrackHunk>& hunks = model.Hunks();
    diff_.SelectHunk(index);
    const int selected = diff_.SelectedHunk();
    selectedHunkId_ = selected >= 0 && static_cast<size_t>(selected) < hunks.size() ? hunks[static_cast<size_t>(selected)].hunkId : 0;
}

bool AiChangesWindow::ReviewSelectedHunk(ChangeTrackingSession& session, ReviewAction action)
{
    const ChangeModel& model = session.Model();
    if (model.SelectedRevision() == 0 || !IsReviewableDetail(model.Detail(), model.SelectedRevision()) || LeafKindOf(model.Detail()) != "hunk"
        || shownHunksSerial_ != model.HunksSerial()) {
        return false;
    }
    const int selected = diff_.SelectedHunk();
    const std::vector<TrackHunk>& hunks = model.Hunks();
    if (selected < 0 || static_cast<size_t>(selected) >= hunks.size()) {
        return false;
    }
    return SubmitReview(session, { ReviewManager::ForHunk(model.SelectedRevision(), hunks[static_cast<size_t>(selected)].hunkId) }, action);
}

void AiChangesWindow::SelectSceneChange(int64_t sceneChangeId)
{
    if (selectedSceneId_ != sceneChangeId) {
        sceneScope_ = 0;
    }
    selectedSceneId_ = sceneChangeId;
}

bool AiChangesWindow::ReviewSelectedScene(ChangeTrackingSession& session, ReviewAction action)
{
    const ChangeModel& model = session.Model();
    if (model.SelectedRevision() == 0 || !IsReviewableDetail(model.Detail(), model.SelectedRevision()) || LeafKindOf(model.Detail()) != "scene") {
        return false;
    }
    for (const TrackSceneChange& change : model.SceneChanges()) {
        if (change.sceneChangeId != selectedSceneId_) {
            continue;
        }
        const std::vector<ReviewTarget> options = SceneReviewOptions(model.SelectedRevision(), change);
        const size_t index = (std::min)(static_cast<size_t>((std::max)(sceneScope_, 0)), options.size() - 1);
        return SubmitReview(session, { options[index] }, action);
    }
    return false;
}

bool AiChangesWindow::RequestBulkSelected(ChangeTrackingSession& session, ReviewAction action)
{
    ChangeModel& model = session.Model();
    std::vector<int64_t> selected(multi_.begin(), multi_.end());
    if (selected.empty() && model.SelectedRevision() != 0) {
        selected.push_back(model.SelectedRevision());
    }
    int skipped = 0;
    std::vector<int64_t> ids = ReviewManager::RevisionsOf(model.Changes(), selected, &skipped);
    if (ids.empty() || session.Review().Busy()) {
        return false;
    }
    session.Review().PrepareRevisions(std::move(ids), skipped);
    bulkAction_ = action;
    bulkPending_ = true;
    bulkOpenRequested_ = true;
    return true;
}

bool AiChangesWindow::RequestBulkSession(ChangeTrackingSession& session, ReviewAction action)
{
    if (sessionId_.empty() || session.Review().Busy()) {
        return false;
    }
    session.Review().PrepareSession(sessionId_);
    bulkAction_ = action;
    bulkPending_ = true;
    bulkOpenRequested_ = true;
    return true;
}

bool AiChangesWindow::ConfirmBulk(ChangeTrackingSession& session)
{
    const bool sent = session.Review().CommitPrepared(bulkAction_, reviewNote_);
    noteSent_ = sent && reviewNote_[0] != '\0';
    bulkPending_ = false;
    return sent;
}

void AiChangesWindow::CancelBulk(ChangeTrackingSession& session)
{
    session.Review().CancelPrepared();
    bulkPending_ = false;
}

std::optional<ReviewAction> AiChangesWindow::DrawReviewButtons(const char* id, bool busy)
{
    std::optional<ReviewAction> picked;
    constexpr ReviewAction kActions[] = { ReviewAction::Approve, ReviewAction::Reject, ReviewAction::NeedsReview, ReviewAction::Reopen };
    ImGui::PushID(id);
    ImGui::BeginDisabled(busy);
    for (size_t i = 0; i < std::size(kActions); ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton(ReviewActionLabel(kActions[i]).c_str())) {
            picked = kActions[i];
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return picked;
}

// 送信中・失敗の 1 行 (サービスの応答で確定してから表示を変えるので、結果はここに出す)
void AiChangesWindow::DrawReviewStatus(ChangeTrackingSession& session)
{
    ReviewManager& review = session.Review();
    if (review.GetStatus() == ReviewManager::Status::Sending) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvSending));
    } else if (review.GetStatus() == ReviewManager::Status::Done) {
        if (noteSent_) {
            reviewNote_[0] = '\0'; // 記録できたメモは消す (失敗のときは残して、やり直せるようにする)
            noteSent_ = false;
        }
    } else if (review.GetStatus() == ReviewManager::Status::Failed) {
        noteSent_ = false;
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", ReviewErrorText(review.ErrorCode(), review.ErrorDetail()).c_str());
        const std::string partial = ReviewPartialText(review.RecordedCount(), review.TotalCount());
        if (!partial.empty()) {
            ImGui::TextWrapped("%s", partial.c_str());
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr(StrId::Common_Close))) {
            review.ClearResult();
        }
    }
}

// 選んだ Revision のレビュー: 集約の状態・内訳・操作・メモ・履歴。承認・却下が何をする / しないかを文面で明示する
void AiChangesWindow::DrawReviewPanel(ChangeTrackingSession& session)
{
    const ChangeModel& model = session.Model();
    const nlohmann::json& d = model.Detail();
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr(StrId::AiChg_RvPanel));
    if (!IsReviewableDetail(d, model.SelectedRevision())) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvNotReviewable));
        return;
    }
    const std::string state = d.contains("review") && d["review"].is_string() ? d["review"].get<std::string>() : std::string();
    ImGui::SameLine();
    ImGui::TextColored(ReviewColor(state), "%s", ReviewStateLabel(state).c_str());
    const std::string counts = ReviewCountsText(ChangeModel::ParseReviewCounts(d.contains("reviewCounts") ? d["reviewCounts"] : nlohmann::json()));
    if (!counts.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", counts.c_str());
    }
    if (const std::optional<ReviewAction> picked = DrawReviewButtons("revision", session.Review().Busy())) {
        ReviewRevision(session, *picked);
    }
    DrawAggregates(session);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##reviewnote", Tr(StrId::AiChg_RvNoteHint), reviewNote_, sizeof(reviewNote_));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", Tr(StrId::AiChg_RvExplainApprove));
    ImGui::TextWrapped("%s", Tr(StrId::AiChg_RvExplainReject));
    ImGui::PopStyleColor();

    // 履歴 (古い順)。誰が、は無い (1 人の利用者の記録)
    ImGui::TextUnformatted(Tr(StrId::AiChg_RvHistory));
    const nlohmann::json& history = d.contains("reviewHistory") ? d["reviewHistory"] : nlohmann::json();
    if (!history.is_array() || history.empty()) {
        ImGui::TextDisabled("  %s", Tr(StrId::AiChg_RvNoHistory));
        return;
    }
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float height = (std::min)(static_cast<float>(history.size()), 6.0f) * lineH + 6.0f;
    if (ImGui::BeginChild("##reviewhistory", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(history.size()), lineH);
        while (clipper.Step()) {
            for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                const nlohmann::json& row = history[static_cast<size_t>(n)];
                const std::string rowState = row.is_object() && row.contains("state") && row["state"].is_string() ? row["state"].get<std::string>() : std::string();
                ImGui::PushStyleColor(ImGuiCol_Text, ReviewColor(rowState));
                ImGui::TextUnformatted(FormatReviewHistoryRow(row).c_str());
                ImGui::PopStyleColor();
            }
        }
        clipper.End();
    }
    ImGui::EndChild();
}

// 差分の Hunk を選んだときの、その Hunk へのレビュー (葉が Hunk の Revision だけ)
void AiChangesWindow::DrawHunkReview(ChangeTrackingSession& session)
{
    const ChangeModel& model = session.Model();
    if (!IsReviewableDetail(model.Detail(), model.SelectedRevision()) || LeafKindOf(model.Detail()) != "hunk") {
        return;
    }
    const std::vector<TrackHunk>& hunks = model.Hunks();
    const int selected = diff_.SelectedHunk();
    if (selected < 0 || static_cast<size_t>(selected) >= hunks.size() || shownHunksSerial_ != model.HunksSerial()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvHunkNone));
        return;
    }
    const TrackHunk& hunk = hunks[static_cast<size_t>(selected)];
    ImGui::TextUnformatted(Tr(StrId::AiChg_RvHunkSelected));
    ImGui::SameLine();
    ImGui::Text("#%lld", static_cast<long long>(hunk.hunkId));
    ImGui::SameLine();
    ImGui::TextColored(ReviewColor(hunk.review), "%s", ReviewStateLabel(hunk.review).c_str());
    if (const std::optional<ReviewAction> picked = DrawReviewButtons("hunk", session.Review().Busy())) {
        ReviewSelectedHunk(session, *picked);
    }
}

// シーンの変更の行を選んだときの、その行へのレビュー。範囲は狭い順に選べる (フィールド / コンポーネント / エンティティ / 変更全体)
void AiChangesWindow::DrawSceneReview(ChangeTrackingSession& session)
{
    const ChangeModel& model = session.Model();
    if (!IsReviewableDetail(model.Detail(), model.SelectedRevision()) || LeafKindOf(model.Detail()) != "scene") {
        return;
    }
    const TrackSceneChange* selected = nullptr;
    for (const TrackSceneChange& change : model.SceneChanges()) {
        if (change.sceneChangeId == selectedSceneId_) {
            selected = &change;
        }
    }
    if (selected == nullptr) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvSceneNone));
        return;
    }
    ImGui::TextUnformatted(Tr(StrId::AiChg_RvSceneSelected));
    ImGui::SameLine();
    ImGui::TextColored(ReviewColor(selected->review), "%s", ReviewStateLabel(selected->review).c_str());
    const std::vector<ReviewTarget> options = SceneReviewOptions(model.SelectedRevision(), *selected);
    sceneScope_ = (std::min)((std::max)(sceneScope_, 0), static_cast<int>(options.size()) - 1);
    const ReviewTarget& current = options[static_cast<size_t>(sceneScope_)];
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (ImGui::BeginCombo("##scenescope", ReviewScopeLabel(current.kind, current.ref).c_str())) {
        for (size_t i = 0; i < options.size(); ++i) {
            if (ImGui::Selectable(ReviewScopeLabel(options[i].kind, options[i].ref).c_str(), static_cast<int>(i) == sceneScope_)) {
                sceneScope_ = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvSceneScope));
    if (const std::optional<ReviewAction> picked = DrawReviewButtons("scene", session.Review().Busy())) {
        ReviewSelectedScene(session, *picked);
    }
}

// 選んだ構造の変更の行の「シーンの変更を復元...」。範囲はレビューと同じ選択 (フィールド / コンポーネント / エンティティ / 変更全体)。
// 今開いていないシーンの変更は、項目ごとには戻せない (開くか、ファイル・Hunk の復元を使う)
void AiChangesWindow::DrawSceneRestoreButton(ChangeTrackingSession& session, const AiChangesHost& host)
{
    const ChangeModel& model = session.Model();
    if (!host.openSceneRestore || !IsReviewableDetail(model.Detail(), model.SelectedRevision()) || LeafKindOf(model.Detail()) != "scene") {
        return;
    }
    const TrackSceneChange* selected = nullptr;
    for (const TrackSceneChange& change : model.SceneChanges()) {
        if (change.sceneChangeId == selectedSceneId_) {
            selected = &change;
        }
    }
    if (selected == nullptr) {
        return;
    }
    const std::vector<ReviewTarget> options = SceneReviewOptions(model.SelectedRevision(), *selected);
    const ReviewTarget& current = options[(std::min)(static_cast<size_t>((std::max)(sceneScope_, 0)), options.size() - 1)];
    const SceneRestoreScope scope = current.kind == "field"       ? SceneRestoreScope::Field
                                    : current.kind == "component" ? SceneRestoreScope::Component
                                    : current.kind == "entity"    ? SceneRestoreScope::Entity
                                                                  : SceneRestoreScope::Revision;
    const std::string& doc = selected->resolvedDocumentId.empty() ? selected->documentId : selected->resolvedDocumentId;
    const bool notOpen = session.Overlay().DocumentId().empty() || doc != session.Overlay().DocumentId();
    ImGui::BeginDisabled(notOpen);
    if (ImGui::SmallButton(Tr(StrId::AiChg_SrButton))) {
        host.openSceneRestore(scope, *selected);
    }
    ImGui::EndDisabled();
    if (notOpen) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_SrNotOpenShort));
    }
}

// まとめてのレビューの確認 (件数を出して 1 回だけ)
void AiChangesWindow::DrawBulkPopup(ChangeTrackingSession& session)
{
    if (!bulkPending_) {
        return;
    }
    ReviewManager& review = session.Review();
    if (bulkOpenRequested_) {
        ImGui::OpenPopup(Tr(StrId::AiChg_RvConfirmTitle));
        bulkOpenRequested_ = false;
    }
    if (!ImGui::BeginPopupModal(Tr(StrId::AiChg_RvConfirmTitle), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    if (!review.PreparedOperationId().empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvBulkOperation));
    }
    bool canRecord = false;
    switch (review.GetCollect()) {
    case ReviewManager::Collect::Collecting:
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvCollecting));
        break;
    case ReviewManager::Collect::Failed:
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::AiChg_RvCollectFailed));
        ImGui::PopStyleColor();
        break;
    case ReviewManager::Collect::Ready:
        if (review.PreparedRevisions().empty()) {
            ImGui::TextUnformatted(Tr(StrId::AiChg_RvNothingToReview));
        } else {
            ImGui::TextUnformatted(ReviewConfirmText(review.PreparedRevisions().size(), review.PreparedSkipped(), bulkAction_).c_str());
            canRecord = true;
        }
        break;
    case ReviewManager::Collect::None:
        break;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvExplainApprove));
    ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvExplainReject));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::BeginDisabled(!canRecord || review.Busy());
    if (ImGui::Button(Tr(StrId::AiChg_RvConfirmOk))) {
        ConfirmBulk(session);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Common_Cancel))) {
        CancelBulk(session);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- 操作全体・ファイル単位の集約 (M104p。sub-14 の残り) ----

bool AiChangesWindow::RequestBulkOperation(ChangeTrackingSession& session, ReviewAction action)
{
    const ChangeModel& model = session.Model();
    const nlohmann::json& d = model.Detail();
    if (model.SelectedRevision() == 0 || !IsReviewableDetail(d, model.SelectedRevision()) || session.Review().Busy()) {
        return false;
    }
    const std::string operationId = d.contains("operationId") && d["operationId"].is_string() ? d["operationId"].get<std::string>() : std::string();
    if (operationId.empty()) {
        return false;
    }
    session.Review().PrepareOperation(operationId);
    bulkAction_ = action;
    bulkPending_ = true;
    bulkOpenRequested_ = true;
    return true;
}

// 変更 (ファイル) 単位と操作単位の集約。describe の changeReview / operationReview (サービスが数える)
void AiChangesWindow::DrawAggregates(ChangeTrackingSession& session)
{
    const nlohmann::json& d = session.Model().Detail();
    auto group = [&](const char* key, StrId label) {
        if (!d.contains(key) || !d[key].is_object()) {
            return false;
        }
        const nlohmann::json& g = d[key];
        const std::string state = g.contains("state") && g["state"].is_string() ? g["state"].get<std::string>() : std::string();
        ImGui::TextDisabled("%s", Tr(label));
        ImGui::SameLine();
        ImGui::TextColored(ReviewColor(state), "%s", ReviewGroupText(g).c_str());
        return true;
    };
    group("changeReview", StrId::AiChg_RvFileAggregate);
    if (group("operationReview", StrId::AiChg_RvOperationAggregate)) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvBulkOperation));
        if (const std::optional<ReviewAction> picked = DrawReviewButtons("operation", session.Review().Busy())) {
            RequestBulkOperation(session, *picked);
        }
    } else {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RvNoOperation));
    }
}

// ---- 大きいファイルの先頭 (M104p。sub-10 の残り) ----

void AiChangesWindow::DrawBlobHeads(ChangeTrackingSession& session)
{
    head_.Request(session.Client(), ParseRestoreRevision(session.Model().Detail()));
    ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
    ImGui::TextWrapped("%s", Tr(StrId::AiChg_HeadWhole));
    ImGui::PopStyleColor();
    const float height = ImGui::GetTextLineHeightWithSpacing() * 6.0f;
    for (const bool isBefore : { true, false }) {
        const BlobHeadView::Side& side = isBefore ? head_.Before() : head_.After();
        ImGui::TextUnformatted(Tr(isBefore ? StrId::AiChg_HeadBefore : StrId::AiChg_HeadAfter));
        ImGui::PushID(isBefore ? "headbefore" : "headafter");
        if (ImGui::BeginChild("##head", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
            switch (side.state) {
            case BlobHeadView::State::None: break;
            case BlobHeadView::State::Loading: ImGui::TextDisabled("%s", Tr(StrId::AiChg_HeadLoading)); break;
            case BlobHeadView::State::Failed: ImGui::TextDisabled("%s", Tr(StrId::AiChg_HeadFailed)); break;
            case BlobHeadView::State::Ready:
                if (side.binary) {
                    ImGui::TextDisabled("%s", BlobHeadBinaryText(side.total).c_str());
                } else {
                    ImGui::TextUnformatted(side.text.c_str());
                    if (side.total > side.shown) {
                        ImGui::TextDisabled("%s", BlobHeadTruncatedText(side.shown, side.total).c_str());
                    }
                }
                break;
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
}

// ---- 復元のプレビュー (M104p)。作業ツリーを書かない ----

RestoreFileIo& AiChangesWindow::RestoreIo()
{
    if (!restoreIo_) {
        restoreIo_ = MakeSystemRestoreFileIo();
    }
    return *restoreIo_;
}

bool AiChangesWindow::OpenRestore(ChangeTrackingSession& session, RestoreScope scope, const AiChangesHost& host)
{
    ChangeModel& model = session.Model();
    const nlohmann::json& d = model.Detail();
    if (model.SelectedRevision() == 0 || !d.is_object() || !d.contains("revisionId") || !d["revisionId"].is_number_integer()
        || d["revisionId"].get<int64_t>() != model.SelectedRevision()) {
        return false; // 詳細が無い・選び直した直後で古い
    }
    std::vector<int64_t> hunkIds;
    if (scope == RestoreScope::Hunks) {
        if (selectedHunkId_ == 0) {
            return false;
        }
        hunkIds.push_back(selectedHunkId_);
    }
    restoreDetail_ = d;
    restoreAllHunks_ = model.Hunks();
    restoreHunkIds_ = hunkIds;
    restore_.Open(scope, d, model.Hunks(), hunkIds, host.projectRoot, session.Client(), RestoreIo());
    restoreOpenRequested_ = true;
    return true;
}

void AiChangesWindow::CloseRestore()
{
    restore_.Close();
    restoreDiff_.Clear();
    restoreConflictDiff_.Clear();
    shownRestoreSerial_ = ~0ull;
    restoreOpenRequested_ = false;
    restoreScreen_ = RestoreScreen();
}

std::vector<RestoreBlock> BuildRestoreBlocks(ChangeTrackingSession& session, const AiChangesHost& host, const std::vector<std::string>& targetPaths, bool blobMissing)
{
    RestoreGateInput in;
    if (host.gateInputs) {
        in.gate = host.gateInputs();
    }
    if (host.editedDocuments) {
        in.documents = host.editedDocuments();
    }
    in.targetPaths = targetPaths;
    in.trackConnected = session.Client().CanRequest();
    in.journalPending = host.restoreJournalPending && host.restoreJournalPending();
    in.blobMissing = blobMissing;
    return EvaluateRestoreGate(in);
}

std::vector<RestoreBlock> AiChangesWindow::RestoreBlocks(ChangeTrackingSession& session, const AiChangesHost& host) const
{
    std::vector<RestoreBlock> blocks = BuildRestoreBlocks(session, host, restore_.TargetPaths(), restore_.BlobMissing());
    // 実行中・回復待ちの復元がある間は、新しい復元を始められない (回復待ちは host の口で、すでに入っている)
    const bool hostSaysPending = host.restoreJournalPending && host.restoreJournalPending();
    if (session.Restore().Pending() && !hostSaysPending) {
        RestoreBlock block;
        block.kind = RestoreBlockKind::JournalPending;
        blocks.push_back(block);
    }
    return blocks;
}

bool AiChangesWindow::ApplyRestore(ChangeTrackingSession& session, const AiChangesHost& host)
{
    if (!restore_.IsOpen() || !restore_.HasPlan() || restore_.GetPhase() != RestorePreview::Phase::Ready) {
        return false;
    }
    const RestorePlan& plan = restore_.Plan();
    if (plan.verdict != RestoreVerdict::Ready || plan.ops.empty() || !RestoreBlocks(session, host).empty()) {
        return false;
    }
    RestoreRunner::StartRequest request;
    request.scope = restore_.Scope();
    request.detail = restoreDetail_;
    request.allHunks = restoreAllHunks_;
    request.selectedHunkIds = restoreHunkIds_;
    request.previewOps = plan.ops;
    request.targetPaths = restore_.TargetPaths();
    // 実行の直前に、同じゲートでもう一度判定する。host は 1 フレームだけ有効な値なので写しを持つ
    request.blocks = [&session, hostCopy = host, paths = restore_.TargetPaths()](bool blobMissing) {
        return BuildRestoreBlocks(session, hostCopy, paths, blobMissing);
    };
    if (!session.Restore().Start(std::move(request))) {
        return false;
    }
    CloseRestore();
    return true;
}

RestoreNotes AiChangesWindow::RestoreNotesFor(const AiChangesHost& host) const
{
    if (!restore_.HasPlan()) {
        return RestoreNotes();
    }
    const std::string activeScene = host.editedDocuments ? host.editedDocuments().openScene : std::string();
    return BuildRestoreNotes(restore_.Plan(), activeScene, host.stagedState);
}

// 「復元...」「選んだ Hunk を復元...」。押すと必要な内容を集めて計画を作り、プレビューを出す (実行はしない)
void AiChangesWindow::DrawRestoreButtons(ChangeTrackingSession& session, const AiChangesHost& host, const std::string& actor)
{
    const bool restorable = IsRestorableActor(actor);
    ImGui::Spacing();
    ImGui::BeginDisabled(!restorable);
    if (ImGui::SmallButton(Tr(StrId::AiChg_RsButtonFile))) {
        OpenRestore(session, RestoreScope::File, host);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedHunkId_ == 0);
    if (ImGui::SmallButton(Tr(StrId::AiChg_RsButtonHunk))) {
        OpenRestore(session, RestoreScope::Hunks, host);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (!restorable) {
        ImGui::TextWrapped("%s", Tr(StrId::AiChg_RsNotRestorableActor));
    } else if (selectedHunkId_ == 0 && !session.Model().Hunks().empty()) {
        ImGui::TextWrapped("%s", Tr(StrId::AiChg_RsNeedHunk));
    }
    ImGui::PopStyleColor();
}

namespace {

ImVec4 VerdictColor(RestoreVerdict verdict)
{
    switch (verdict) {
    case RestoreVerdict::Ready: return themeColor::Success;
    case RestoreVerdict::AlreadyRestored: return themeColor::TextDim;
    case RestoreVerdict::Conflict: return themeColor::Error;
    case RestoreVerdict::NotRestorable: return themeColor::Warning;
    }
    return themeColor::Text;
}

void BulletLines(const std::vector<std::string>& lines)
{
    for (const std::string& line : lines) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", line.c_str());
    }
}

} // namespace

// プレビューの画面。止める理由・照合の結果・書くことになる内容・注記と、実行のボタン
void AiChangesWindow::DrawRestorePopup(ChangeTrackingSession& session, const AiChangesHost& host)
{
    if (!restore_.IsOpen()) {
        return;
    }
    // 閉じるのは利用者が「閉じる」か x を押したときだけ。ポップアップが開いていなければ (開いた最初のフレーム・文脈が作り直された)、開き直す
    const char* title = Tr(StrId::AiChg_RsTitle);
    if (restoreOpenRequested_ || !ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
        restoreOpenRequested_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(780.0f, 620.0f), ImGuiCond_Appearing);
    bool keepOpen = true;
    if (!ImGui::BeginPopupModal(title, &keepOpen)) {
        return;
    }
    // ゲートは毎フレーム判定し直す (確認の画面を出している間に、保存・再生・ビルドの状態が変わる)
    restoreScreen_ = BuildRestoreScreen(restore_, RestoreBlocks(session, host), RestoreNotesFor(host));
    const RestoreScreen& screen = restoreScreen_;

    ImGui::TextUnformatted(Tr(StrId::AiChg_RsTarget));
    ImGui::SameLine();
    ImGui::TextUnformatted(screen.target.c_str());
    ImGui::TextDisabled("%s", screen.scope.c_str());

    if (screen.phase == RestorePreview::Phase::Preparing) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RsPreparing));
    } else if (screen.phase == RestorePreview::Phase::Failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s %s", Tr(StrId::AiChg_RsFailed), screen.failure.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    if (screen.blocks.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RsNoBlocks));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextUnformatted(Tr(StrId::AiChg_RsBlocks));
        ImGui::PopStyleColor();
        BulletLines(screen.blocks);
    }

    if (screen.hasPlan) {
        ImGui::Separator();
        ImGui::TextUnformatted(Tr(StrId::AiChg_RsResult));
        ImGui::PushStyleColor(ImGuiCol_Text, VerdictColor(screen.verdict));
        ImGui::TextWrapped("%s", screen.verdictText.c_str());
        ImGui::PopStyleColor();
        BulletLines(screen.issues);
        if (!screen.superseded.empty()) {
            ImGui::TextDisabled("%s", screen.superseded.c_str());
        }
        BulletLines(screen.hunkLines);
        if (!screen.hashLine.empty()) {
            ImGui::TextDisabled("%s", screen.hashLine.c_str());
        }
        if (!screen.ops.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(Tr(StrId::AiChg_RsWillWrite));
            BulletLines(screen.ops);
        }
        if (!screen.notes.empty()) {
            ImGui::TextUnformatted(Tr(StrId::AiChg_RsNotes));
            BulletLines(screen.notes);
        }
        // 差分の入力は、準備ができたときに 1 回だけ作り直す
        if (shownRestoreSerial_ != restore_.ViewSerial()) {
            restoreDiff_.SetHunks(restore_.ChangeView());
            restoreConflictDiff_.SetHunks(restore_.ConflictView());
            shownRestoreSerial_ = restore_.ViewSerial();
        }
        if (!restore_.ConflictView().empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(Tr(StrId::AiChg_RsDiffConflict));
            restoreConflictDiff_.Draw("##restoreconflict", Tr(StrId::AiChg_NoHunks), 0.0f, ImGui::GetTextLineHeightWithSpacing() * 8.0f);
        }
        if (screen.verdict == RestoreVerdict::Ready && !restore_.ChangeView().empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(Tr(StrId::AiChg_RsDiffChange));
            restoreDiff_.Draw("##restorediff", Tr(StrId::AiChg_NoHunks), 0.0f, ImGui::GetTextLineHeightWithSpacing() * 10.0f);
        }
    }

    ImGui::Separator();
    // 照合に通って、止める理由が無いときだけ押せる。押すと実行の直前にもう一度判定する
    ImGui::BeginDisabled(!screen.canApply);
    const bool apply = ImGui::Button(Tr(StrId::AiChg_RsApply));
    ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", screen.applyNote.c_str()); // 長い一言は折り返す (ボタンの右に出すと窓の外へ切れる)
    ImGui::PopStyleColor();
    if (apply && ApplyRestore(session, host)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (ImGui::Button(Tr(StrId::Common_Close)) || !keepOpen) {
        ImGui::CloseCurrentPopup();
        CloseRestore();
    }
    ImGui::EndPopup();
}

} // namespace mye
