#include "Editor/Windows/TimelineWindow.h"

#include <algorithm>

#include "Editor/PlayModeController.h"
#include "Engine/Core/Localization.h"
#include "Engine/Engine/Replay/TimeTravel.h"
#include "Engine/Platform/InputActions.h"

#include "fontawesome/IconsFontAwesome6.h"
#include "Engine/Renderer/ImGuiTheme.h" // themeColor (意味色)

#include "imgui.h"

namespace mye {
namespace {

ImU32 ToImU32(uint32_t rgba)
{
    // 0xRRGGBBAA → ImGui の ABGR パック
    const float r = static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f;
    const float b = static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f;
    const float a = static_cast<float>(rgba & 0xFFu) / 255.0f;
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

ImVec4 ToImVec4(uint32_t rgba)
{
    return ImGui::ColorConvertU32ToFloat4(ToImU32(rgba));
}

} // namespace

void TimelineWindow::OnImGui(EngineContext& ctx, PlayModeController& playMode)
{
    if (!open) {
        return;
    }
    // 初回はレーン帯 + 分岐表が収まる高さで開く (M72c。ini に残っていればそちらが勝つ)
    ImGui::SetNextWindowSize(ImVec2(620.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Win_Timeline), &open)) {
        ImGui::End();
        return;
    }
    TimeTravel* const tt = ctx.timeTravel;
    if (tt == nullptr || !playMode.InPlayMode()) {
        ImGui::TextUnformatted(Tr(StrId::TT_NotPlaying));
        ImGui::End();
        return;
    }
    if (!tt->Enabled()) {
        // Play を押した直後の 1 フレーム (リングの開始は次の tick 境界)
        ImGui::TextUnformatted(Tr(StrId::TT_Warming));
        ImGui::End();
        return;
    }

    const unsigned long long first = tt->FirstTick();
    const unsigned long long end = tt->EndTick();
    const unsigned long long now = ctx.tickIndex;
    ImGui::Text(Tr(StrId::TT_Range), first, end, now);
    ImGui::Text(Tr(StrId::TT_Snapshots), static_cast<int>(tt->SnapshotCount()),
                static_cast<double>(tt->SnapshotBytes()) / (1024.0 * 1024.0),
                static_cast<int>(tt->Config().snapshotInterval));

    // ---- スクラブ ----
    // 目盛りは first からの相対にする (tick は 64bit だが ImGui のスライダーは int)
    const int span = static_cast<int>(end - first);
    int pos = (pendingPos_ >= 0) ? static_cast<int>(pendingPos_)
                                 : static_cast<int>(std::min(now, end) - first);
    pos = std::clamp(pos, 0, span);
    ImGui::SetNextItemWidth(-1.0f);
    const bool moved = ImGui::SliderInt("##ttpos", &pos, 0, span);
    const auto Seek = [&](int target) {
        target = std::clamp(target, 0, span);
        pendingPos_ = target;
        // ★スクラブは必ずポーズを伴う。動いている世界の過去を覗いても次の tick で
        //   上書きされるだけで、観察という目的を果たさない
        playMode.Pause();
        tt->RequestSeek(first + static_cast<unsigned long long>(target));
    };
    if (moved) {
        Seek(pos);
    }
    if (!ImGui::IsItemActive() && !tt->Scrubbing()) {
        pendingPos_ = -1; // 掴んでいない & 再生中は現在 tick に追従させる
    }

    // ---- 相対移動 (スライダーでは 1 tick を掴みにくいため) ----
    if (ImGui::Button(ICON_FA_BACKWARD_FAST)) {
        Seek(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("-30")) {
        Seek(pos - 30);
    }
    ImGui::SameLine();
    if (ImGui::Button("-1")) {
        Seek(pos - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("+1")) {
        Seek(pos + 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("+30")) {
        Seek(pos + 30);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FORWARD_FAST)) {
        Seek(span);
    }

    // ---- 分岐の明示 ----
    if (tt->Scrubbing()) {
        ImGui::Separator();
        ImGui::TextColored(themeColor::PlayAccent, "%s", Tr(StrId::TT_Scrubbing));
        if (ImGui::Button(Tr(StrId::TT_Resume))) {
            tt->EndScrub();
            playMode.Resume();
            pendingPos_ = -1;
        }
    }

    // ---- 直前のシークの自己検証結果 ----
    // ★「戻して同じ入力で回したら記録と同じハッシュになったか」を毎回出す。
    //   ここが赤いときのタイムラインは**嘘のタイムライン**なので、黙って見せない
    const SeekReport& s = tt->LastSeek();
    if (s.outcome != SeekOutcome::None) {
        ImGui::Separator();
        switch (s.outcome) {
        case SeekOutcome::Ok:
            ImGui::Text(Tr(StrId::TT_SeekOk), static_cast<unsigned long long>(s.target),
                        static_cast<unsigned long long>(s.resimTicks), s.ms);
            break;
        case SeekOutcome::HashMismatch:
            ImGui::TextColored(themeColor::Error, Tr(StrId::TT_SeekMismatch),
                               static_cast<unsigned long long>(s.target));
            break;
        default:
            ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::TT_SeekFailed));
            break;
        }
    }

    // ---- 分岐レーン (M72c) ----
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::TT_Lanes));
    DrawLaneStrip(ctx, *tt);
    DrawBranchTable(ctx, *tt, playMode);
    DrawOverrides(ctx, *tt);

    ImGui::Separator();
    ImGui::TextDisabled("%s", Tr(StrId::TT_CsharpNote));
    ImGui::End();
}

void TimelineWindow::DrawOverrides(EngineContext& ctx, TimeTravel& tt)
{
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::TT_OvrTitle));
    InputOverrideSet& set = tt.Overrides();
    const InputActions* const ia = ctx.inputActions;
    const size_t nActions = ia != nullptr ? ia->Actions().size() : 0;
    const size_t nAxes = ia != nullptr ? ia->Axes().size() : 0;
    if (nActions + nAxes == 0) {
        ImGui::TextDisabled("%s", Tr(StrId::TT_OvrNoActions));
    } else {
        ovrSel_ = std::clamp(ovrSel_, 0, static_cast<int>(nActions + nAxes) - 1);
        const bool selIsAxis = static_cast<size_t>(ovrSel_) >= nActions;
        const char* current = selIsAxis ? ia->Axes()[static_cast<size_t>(ovrSel_) - nActions].name.c_str()
                                        : ia->Actions()[static_cast<size_t>(ovrSel_)].name.c_str();
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("##ovrsel", current)) {
            for (size_t i = 0; i < nActions; ++i) {
                if (ImGui::Selectable(ia->Actions()[i].name.c_str(), static_cast<size_t>(ovrSel_) == i)) {
                    ovrSel_ = static_cast<int>(i);
                }
            }
            for (size_t i = 0; i < nAxes; ++i) {
                if (ImGui::Selectable(ia->Axes()[i].name.c_str(),
                                      static_cast<size_t>(ovrSel_) == nActions + i)) {
                    ovrSel_ = static_cast<int>(nActions + i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(Tr(StrId::TT_OvrLane));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        ImGui::InputInt("##ovrlane", &ovrLane_, 0, 0);
        ovrLane_ = std::clamp(ovrLane_, 0, static_cast<int>(kMaxPlayers) - 1);
        ImGui::SameLine();
        ImGui::TextUnformatted(Tr(StrId::TT_OvrFrom));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("##ovrfrom", &ovrFrom_, 0, 0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::TT_OvrFromHint));
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(Tr(StrId::TT_OvrTicks));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("##ovrlen", &ovrLen_, 0, 0);
        ovrLen_ = std::max(1, ovrLen_);
        if (selIsAxis) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::SliderFloat("##ovrval", &ovrValue_, -1.0f, 1.0f, "%.2f");
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::TT_OvrAdd))) {
            const uint64_t from = ovrFrom_ < 0 ? ctx.tickIndex : static_cast<uint64_t>(ovrFrom_);
            const uint64_t to = from + static_cast<uint64_t>(ovrLen_);
            const uint32_t lane = static_cast<uint32_t>(ovrLane_);
            set.items.push_back(
                selIsAxis ? InputOverride::HoldAxis(ia->Axes()[static_cast<size_t>(ovrSel_) - nActions],
                                                    ovrValue_, lane, from, to)
                          : InputOverride::HoldAction(ia->Actions()[static_cast<size_t>(ovrSel_)], lane,
                                                      from, to));
        }
    }
    if (set.items.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::TT_OvrNone));
    } else {
        size_t toErase = set.items.size();
        for (size_t i = 0; i < set.items.size(); ++i) {
            const InputOverride& o = set.items[i];
            ImGui::PushID(static_cast<int>(i));
            const bool done = o.toTick <= ctx.tickIndex;
            if (done) {
                ImGui::TextDisabled(Tr(StrId::TT_OvrRow), o.label, o.lane,
                                    static_cast<unsigned long long>(o.fromTick),
                                    static_cast<unsigned long long>(o.toTick));
            } else {
                ImGui::Text(Tr(StrId::TT_OvrRow), o.label, o.lane,
                            static_cast<unsigned long long>(o.fromTick),
                            static_cast<unsigned long long>(o.toTick));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(StrId::TT_Delete))) {
                toErase = i;
            }
            ImGui::PopID();
        }
        if (toErase < set.items.size()) {
            set.items.erase(set.items.begin() + static_cast<ptrdiff_t>(toErase));
        }
        if (ImGui::SmallButton(Tr(StrId::TT_OvrClear))) {
            set.Clear();
        }
    }
    ImGui::TextDisabled("%s", Tr(StrId::TT_OvrHint));
}

void TimelineWindow::DrawLaneStrip(const EngineContext& ctx, const TimeTravel& tt)
{
    // 帯の tick 軸はライブの先頭から「いちばん遠い終端」まで。分岐の終端はライブより
    // 先にあることが普通 (捨てた未来のほう) なので、ライブの EndTick では足りない
    const uint64_t first = tt.FirstTick();
    uint64_t maxEnd = tt.EndTick();
    for (const TimeTravelBranch& b : tt.Branches()) {
        maxEnd = std::max(maxEnd, tt.EndTickOn(b.id));
    }
    const double span = static_cast<double>(std::max<uint64_t>(1, maxEnd - first));
    const float width = std::max(64.0f, ImGui::GetContentRegionAvail().x);
    constexpr float kRowH = 7.0f;
    constexpr float kGap = 2.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    const auto ToX = [&](uint64_t tick) {
        const double f = static_cast<double>(tick - first) / span;
        return origin.x + static_cast<float>(f) * width;
    };
    const auto Row = [&](int i, uint64_t fork, uint64_t endTick, ImU32 color) {
        const float y = origin.y + static_cast<float>(i) * (kRowH + kGap);
        // 長さ 0 のレーン (編集だけして走らせていない) も 2px の目印は出す
        const float x0 = ToX(fork);
        const float x1 = std::max(x0 + 2.0f, ToX(endTick));
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + kRowH), color, 2.0f);
    };
    int row = 0;
    Row(row++, first, tt.EndTick(), ImGui::ColorConvertFloat4ToU32(themeColor::PlayAccent));
    for (const TimeTravelBranch& b : tt.Branches()) {
        Row(row++, b.forkTick, tt.EndTickOn(b.id), ToImU32(BranchLaneColor(b.id)));
    }
    const float totalH = static_cast<float>(row) * (kRowH + kGap);
    // 現在 tick の縦線 (ライブの上に乗っている場所)
    const float xNow = ToX(std::clamp<uint64_t>(ctx.tickIndex, first, maxEnd));
    dl->AddLine(ImVec2(xNow, origin.y - 1.0f), ImVec2(xNow, origin.y + totalH),
                ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
    ImGui::Dummy(ImVec2(width, totalH));
}

void TimelineWindow::DrawBranchTable(EngineContext& ctx, TimeTravel& tt, PlayModeController& playMode)
{
    const auto& branches = tt.Branches();
    if (branches.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::TT_NoBranches));
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
        | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##ttlanes", 5, flags)) {
        return;
    }
    ImGui::TableSetupColumn(Tr(StrId::TT_LaneColLane), ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn(Tr(StrId::TT_LaneColRange));
    ImGui::TableSetupColumn(Tr(StrId::TT_LaneColDivergence));
    ImGui::TableSetupColumn(Tr(StrId::TT_LaneColGhost));
    ImGui::TableSetupColumn(Tr(StrId::TT_LaneColActions), ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableHeadersRow();

    // ライブの行 (切替も削除も無い。比較の基準)
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(themeColor::PlayAccent, "%s", Tr(StrId::TT_LaneLive));
    ImGui::TableNextColumn();
    ImGui::Text(Tr(StrId::TT_LaneRange), static_cast<unsigned long long>(tt.FirstTick()),
                static_cast<unsigned long long>(tt.EndTick()),
                static_cast<unsigned long long>(tt.EndTick() - tt.FirstTick()));
    ImGui::TableNextColumn();
    ImGui::TextDisabled("-");
    ImGui::TableNextColumn();
    ImGui::TextDisabled("-");
    ImGui::TableNextColumn();

    uint32_t toDelete = 0;
    for (const TimeTravelBranch& b : branches) {
        ImGui::PushID(static_cast<int>(b.id));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ToImVec4(BranchLaneColor(b.id)), Tr(StrId::TT_LaneName), b.id,
                           static_cast<unsigned long long>(b.forkTick));
        ImGui::TableNextColumn();
        const uint64_t bEnd = tt.EndTickOn(b.id);
        ImGui::Text(Tr(StrId::TT_LaneRange), static_cast<unsigned long long>(b.forkTick),
                    static_cast<unsigned long long>(bEnd),
                    static_cast<unsigned long long>(bEnd - b.forkTick));
        ImGui::TableNextColumn();
        // ★毎フレーム比べてよい: 共通範囲の u64 比較だけで、スナップショットの探索は 2 分探索
        const DivergenceReport d = tt.FirstDivergence(TimeTravel::kLiveLane, b.id);
        if (!d.comparable) {
            ImGui::TextDisabled("-");
        } else if (d.diverged) {
            ImGui::Text(Tr(StrId::TT_DivAt), static_cast<unsigned long long>(d.firstTick));
        } else {
            ImGui::TextDisabled("%s", Tr(StrId::TT_DivNone));
        }
        ImGui::TableNextColumn();
        // ---- ゴースト (M72e): 表示トグル + 焼き結果 ----
        if (TimeTravelBranch* mb = tt.FindBranchMut(b.id)) {
            ImGui::Checkbox("##ghost", &mb->ghostVisible);
            ImGui::SameLine();
        }
        if (!b.ghostBaked) {
            ImGui::TextDisabled("%s", Tr(StrId::TT_GhostPending));
        } else if (!b.ghost.verified) {
            ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::TT_GhostMismatch));
        } else if (b.ghost.truncated) {
            ImGui::TextColored(themeColor::Warning, Tr(StrId::TT_GhostTruncated),
                               static_cast<unsigned long long>(b.ghost.lastTick),
                               b.ghost.bytes / 1024);
        } else {
            ImGui::Text(Tr(StrId::TT_GhostOk), b.ghost.MovingCount(), b.ghost.bytes / 1024);
        }
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(Tr(StrId::TT_Switch))) {
            // ★スクラブと同じ規約: 切替は必ずポーズを伴い、EngineLoop がフレーム頭で
            //   レーンを差し替えてから強制復元 + 再シムする
            playMode.Pause();
            tt.RequestSwitch(b.id);
            pendingPos_ = -1;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::TT_SwitchHint));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr(StrId::TT_Delete))) {
            toDelete = b.id;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (toDelete != 0) {
        tt.DeleteBranch(toDelete); // ループの外で消す (Branches() を舐めている最中に縮めない)
    }
    ImGui::TextDisabled(Tr(StrId::TT_BranchNote), static_cast<int>(tt.Config().maxBranches));
    (void)ctx;
}

} // namespace mye
