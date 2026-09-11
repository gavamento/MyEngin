#include "Editor/Windows/TimelineWindow.h"

#include <algorithm>
#include <cstdio>

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

ImU32 WithAlpha(ImU32 c, float alpha)
{
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// tick → "mm:ss:ff" (60Hz、ff = その秒の中の tick 番号)。表示専用で sim には無関係
void FormatTimecode(uint64_t tick, char* out, size_t n)
{
    std::snprintf(out, n, "%02llu:%02llu:%02llu", static_cast<unsigned long long>(tick / 3600),
                  static_cast<unsigned long long>((tick / 60) % 60),
                  static_cast<unsigned long long>(tick % 60));
}

// actions.json が無いプロジェクトの代替: 生のキー / マウス / パッドのどれかが押されているか
bool RawAnyDown(const InputSnapshot& s)
{
    for (uint8_t b : s.keys) {
        if (b != 0) {
            return true;
        }
    }
    return s.mouseButtons != 0 || s.padButtons != 0;
}

constexpr float kLabelW = 64.0f; // 帯の左のラベル列 (live / B1 / input / override)
constexpr float kRowGap = 2.0f;

} // namespace

void TimelineWindow::OnImGui(EngineContext& ctx, PlayModeController& playMode)
{
    if (!open) {
        return;
    }
    // 初回はレーン帯 + 分岐表が収まる高さで開く (M72c。ini に残っていればそちらが勝つ)
    ImGui::SetNextWindowSize(ImVec2(680.0f, 620.0f), ImGuiCond_FirstUseEver);
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

    // ---- トランスポート + 状態 (M73b) ----
    DrawTransport(ctx, *tt, playMode);

    const unsigned long long first = tt->FirstTick();
    const unsigned long long end = tt->EndTick();
    const unsigned long long now = ctx.tickIndex;
    ImGui::TextDisabled(Tr(StrId::TT_RangeLine), first, end, static_cast<int>(tt->SnapshotCount()),
                        static_cast<double>(tt->SnapshotBytes()) / (1024.0 * 1024.0),
                        static_cast<int>(tt->Config().snapshotInterval));

    // ---- スライダー ----
    // 目盛りは first からの相対にする (tick は 64bit だが ImGui のスライダーは int)
    const int span = static_cast<int>(end - first);
    int pos = (pendingPos_ >= 0) ? static_cast<int>(pendingPos_)
                                 : static_cast<int>(std::min(now, end) - first);
    pos = std::clamp(pos, 0, span);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##ttpos", &pos, 0, span)) {
        Seek(ctx, *tt, playMode, first + static_cast<uint64_t>(std::clamp(pos, 0, span)));
    }
    if (!ImGui::IsItemActive() && !tt->Scrubbing()) {
        pendingPos_ = -1; // 掴んでいない & 再生中は現在 tick に追従させる
    }

    // ---- レーン帯 (M72c / M73b) ----
    DrawLaneStrip(ctx, *tt, playMode);

    // ---- 直前のシークの自己検証結果 ----
    // ★「戻して同じ入力で回したら記録と同じハッシュになったか」を毎回出す。
    //   ここが赤いときのタイムラインは**嘘のタイムライン**なので、黙って見せない
    const SeekReport& s = tt->LastSeek();
    switch (s.outcome) {
    case SeekOutcome::Ok:
        ImGui::TextDisabled(Tr(StrId::TT_SeekOk), static_cast<unsigned long long>(s.target),
                            static_cast<unsigned long long>(s.resimTicks), s.ms);
        break;
    case SeekOutcome::HashMismatch:
        ImGui::TextColored(themeColor::Error, Tr(StrId::TT_SeekMismatch),
                           static_cast<unsigned long long>(s.target));
        break;
    case SeekOutcome::Failed:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::TT_SeekFailed));
        break;
    default:
        break;
    }

    // ---- セクション (折り畳み) ----
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(Tr(StrId::TT_SectionLanes), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawBranchTable(ctx, *tt, playMode);
    }
    if (ImGui::CollapsingHeader(Tr(StrId::TT_OvrTitle), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawOverrides(ctx, *tt);
    }
    const DiffReport& r = tt->LastDiff();
    if (r.valid || r.tick != 0) { // まだ 1 度も撃っていなければ見出しごと出さない
        if (r.tick != diffSeenTick_ || r.ms != diffSeenMs_) {
            ImGui::SetNextItemOpen(true); // 新しい結果が来たフレームで開く (畳んであっても)
            diffSeenTick_ = r.tick;
            diffSeenMs_ = r.ms;
        }
        if (ImGui::CollapsingHeader(Tr(StrId::TT_SectionDiff))) {
            DrawDiff(ctx, *tt);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", Tr(StrId::TT_CsharpNote));
    ImGui::End();
}

void TimelineWindow::Seek(EngineContext& ctx, TimeTravel& tt, PlayModeController& playMode, uint64_t tick)
{
    const uint64_t first = tt.FirstTick();
    tick = std::clamp(tick, first, tt.EndTick());
    pendingPos_ = static_cast<long long>(tick - first);
    // ★スクラブは必ずポーズを伴う。動いている世界の過去を覗いても次の tick で
    //   上書きされるだけで、観察という目的を果たさない
    playMode.Pause();
    // ★ホールド中に現在 tick へ「戻る」要求は出さない (M73a)。再シム 0 本のシークは
    //   記録ハッシュと編集後の世界を突き合わせるだけなので、Inspector で触った直後に
    //   帯をクリックすると HASH MISMATCH が赤く出る (嘘ではないが観察の邪魔)
    if (tick == ctx.tickIndex && tt.Scrubbing()) {
        return;
    }
    tt.RequestSeek(tick);
}

void TimelineWindow::DrawTransport(EngineContext& ctx, TimeTravel& tt, PlayModeController& playMode)
{
    const uint64_t first = tt.FirstTick();
    const uint64_t end = tt.EndTick();
    const uint64_t now = ctx.tickIndex;
    const bool paused = playMode.State() == PlayState::Paused;
    const bool held = tt.Scrubbing();
    const auto Tip = [](const char* text) {
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", text);
        }
    };

    if (ImGui::Button(ICON_FA_BACKWARD_FAST)) {
        Seek(ctx, tt, playMode, first);
    }
    Tip(Tr(StrId::TT_TipFirst));
    ImGui::SameLine();
    if (ImGui::Button("-30")) {
        Seek(ctx, tt, playMode, now >= first + 30 ? now - 30 : first);
    }
    ImGui::SameLine();
    if (ImGui::Button("-1")) {
        Seek(ctx, tt, playMode, now > first ? now - 1 : first);
    }
    ImGui::SameLine();
    // ---- 再生 / 一時停止 (M73a: 一時停止 = ホールド。tick 番号も止まる) ----
    // 規則は PlayModeController が持つ (Pause = Hold / Resume = EndScrub / Step = 予算 1)
    if (paused) {
        if (ImGui::Button(ICON_FA_PLAY)) {
            playMode.Resume();
            pendingPos_ = -1;
        }
        Tip(now < end ? Tr(StrId::TT_Resume) : Tr(StrId::Tool_TipPause));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, themeColor::PlayAccent);
        if (ImGui::Button(ICON_FA_PAUSE)) {
            playMode.Pause();
        }
        ImGui::PopStyleColor();
        Tip(Tr(StrId::Tool_TipPause));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button(ICON_FA_FORWARD_STEP)) {
        playMode.Step(); // ホールド中は正確に 1 tick で再ホールド
        pendingPos_ = -1;
    }
    ImGui::EndDisabled();
    Tip(Tr(StrId::Tool_TipStep));
    ImGui::SameLine();
    if (ImGui::Button("+1")) {
        Seek(ctx, tt, playMode, now + 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("+30")) {
        Seek(ctx, tt, playMode, now + 30);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FORWARD_FAST)) {
        Seek(ctx, tt, playMode, end);
    }
    Tip(Tr(StrId::TT_TipLast));

    // ---- 状態語 + tick + タイムコード ----
    ImGui::SameLine();
    ImGui::TextUnformatted("  ");
    ImGui::SameLine();
    char tc[16] = {};
    FormatTimecode(now, tc, sizeof(tc));
    if (!paused) {
        ImGui::TextColored(themeColor::PlayAccent, "%s", Tr(StrId::TT_StatePlaying));
    } else if (held && now < end) {
        ImGui::Text(Tr(StrId::TT_StateScrubbing), static_cast<unsigned long long>(now));
    } else {
        ImGui::Text(Tr(StrId::TT_StateHeld), static_cast<unsigned long long>(now));
    }
    ImGui::SameLine();
    ImGui::TextDisabled(Tr(StrId::TT_Timecode), static_cast<unsigned long long>(now), tc);
    // 過去で止まっているときだけ「再開すると分岐する」を添える
    if (held && now < end) {
        ImGui::TextDisabled("%s", Tr(StrId::TT_Scrubbing));
    }
}

void TimelineWindow::UpdateInputCache(const EngineContext& ctx, const TimeTravel& tt)
{
    const uint64_t first = tt.FirstTick();
    const uint64_t end = tt.EndTick();
    // 末尾 entry の hashAfter が控えと違う = レーン切替 / 分岐でライブの中身が入れ替わった
    const TimeTravelEntry* tail =
        (inputAnyEnd_ > first && inputAnyEnd_ <= end) ? tt.Entry(inputAnyEnd_ - 1) : nullptr;
    const bool tailOk = inputAnyEnd_ == first || (tail != nullptr && tail->hashAfter == inputAnyLastHash_);
    if (first != inputAnyFirst_ || end < inputAnyEnd_ || !tailOk) {
        if (first > inputAnyFirst_ && first < inputAnyEnd_ && end >= inputAnyEnd_ && tailOk) {
            // 追い出しで先頭が進んだだけ: 前を削る
            inputAny_.erase(inputAny_.begin(),
                            inputAny_.begin() + static_cast<ptrdiff_t>(first - inputAnyFirst_));
            inputAnyFirst_ = first;
        } else {
            inputAny_.clear();
            inputAnyFirst_ = first;
            inputAnyEnd_ = first;
            inputAnyLastHash_ = 0;
        }
    }
    const InputActions* const ia = ctx.inputActions;
    const bool useActions = ia != nullptr && !ia->Actions().empty();
    const uint32_t lanes = std::min<uint32_t>(ctx.playerCount, kMaxPlayers);
    for (uint64_t t = inputAnyEnd_; t < end; ++t) {
        const TimeTravelEntry* e = tt.Entry(t);
        uint8_t any = 0;
        if (e != nullptr) {
            for (uint32_t p = 0; p < lanes && any == 0; ++p) {
                any = (useActions ? ia->AnyActionDown(e->inputs[p]) : RawAnyDown(e->inputs[p])) ? 1 : 0;
            }
            inputAnyLastHash_ = e->hashAfter;
        }
        inputAny_.push_back(any);
    }
    inputAnyEnd_ = end;
}

void TimelineWindow::DrawLaneStrip(EngineContext& ctx, TimeTravel& tt, PlayModeController& playMode)
{
    // 帯の tick 軸はライブの先頭から「いちばん遠い終端」まで。分岐の終端はライブより
    // 先にあることが普通 (捨てた未来のほう) なので、ライブの EndTick では足りない
    const uint64_t first = tt.FirstTick();
    const uint64_t liveEnd = tt.EndTick();
    uint64_t axisEnd = liveEnd;
    const auto& branches = tt.Branches();
    for (const TimeTravelBranch& b : branches) {
        axisEnd = std::max(axisEnd, tt.EndTickOn(b.id));
    }
    const double span = static_cast<double>(std::max<uint64_t>(1, axisEnd - first));
    const float rowH = std::max(10.0f, ImGui::GetTextLineHeight());
    const float stride = rowH + kRowGap;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(64.0f, ImGui::GetContentRegionAvail().x - kLabelW);
    const float x0 = origin.x + kLabelW;
    const int rows = 1 + static_cast<int>(branches.size()) + 2; // live / 分岐 / 入力 / 上書き
    const float totalH = static_cast<float>(rows) * stride;
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 warnCol = ImGui::ColorConvertFloat4ToU32(themeColor::Warning);
    const auto ToX = [&](uint64_t tick) {
        const double f = static_cast<double>(tick - first) / span;
        return x0 + static_cast<float>(f) * width;
    };
    const auto RowY = [&](int i) { return origin.y + static_cast<float>(i) * stride; };
    const auto Label = [&](int i, const char* text, ImU32 col) {
        dl->AddText(ImVec2(origin.x, RowY(i) + (rowH - ImGui::GetTextLineHeight()) * 0.5f), col, text);
    };
    const auto Bar = [&](int i, uint64_t from, uint64_t to, ImU32 col) {
        const float y = RowY(i);
        const float xa = ToX(from);
        const float xb = std::max(xa + 2.0f, ToX(to)); // 長さ 0 のレーン (編集だけ) も 2px の目印
        dl->AddRectFilled(ImVec2(xa, y + 1.0f), ImVec2(xb, y + rowH - 1.0f), col, 2.0f);
    };

    // 帯の背景 (沈んだ面。入力欄と同じ色)
    dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x0 + width, origin.y + totalH),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);

    // ---- ライブ + スナップショットの目盛り ----
    int row = 0;
    const ImU32 liveCol = ImGui::ColorConvertFloat4ToU32(themeColor::PlayAccent);
    Label(row, Tr(StrId::TT_LaneLive), liveCol);
    Bar(row, first, liveEnd, liveCol);
    {
        // live 行の下辺に 1px の目盛り。pinned (分岐点で撮り直した 1 枚) は 2px
        const float yb = RowY(row) + rowH - 1.0f;
        for (const TimeTravelSnap& sn : tt.Snapshots()) {
            const float x = ToX(sn.tick);
            dl->AddRectFilled(ImVec2(x, yb - 4.0f), ImVec2(x + (sn.pinned ? 2.0f : 1.0f), yb + 1.0f),
                              textCol);
        }
    }
    const float liveBottom = RowY(row) + rowH;
    ++row;

    // ---- 分岐: 帯 + fork 点からの縦線 + 乖離 tick の ▼ ----
    for (const TimeTravelBranch& b : branches) {
        const ImU32 col = ToImU32(BranchLaneColor(b.id));
        char name[16] = {};
        std::snprintf(name, sizeof(name), "B%u", b.id);
        Label(row, name, col);
        Bar(row, b.forkTick, tt.EndTickOn(b.id), col);
        const float xf = ToX(b.forkTick);
        dl->AddLine(ImVec2(xf, liveBottom), ImVec2(xf, RowY(row) + 1.0f), WithAlpha(col, 0.7f), 1.0f);
        // ★毎フレーム比べてよい: 共通範囲の u64 比較だけで、スナップショットの探索は 2 分探索
        const DivergenceReport d = tt.FirstDivergence(TimeTravel::kLiveLane, b.id);
        if (d.comparable && d.diverged) {
            const float xd = ToX(d.firstTick);
            const float yt = RowY(row);
            dl->AddTriangleFilled(ImVec2(xd - 3.5f, yt), ImVec2(xd + 3.5f, yt), ImVec2(xd, yt + 5.0f),
                                  warnCol);
        }
        ++row;
    }

    // ---- 入力: どれかのアクションが押されていた tick (ライブレーン) ----
    Label(row, Tr(StrId::TT_RowInput), dimCol);
    UpdateInputCache(ctx, tt);
    {
        const float y = RowY(row);
        const ImU32 inCol = WithAlpha(textCol, 0.55f);
        // 連続する tick は 1 つの矩形にまとめる (3600 個の矩形を積まない)
        const size_t n = inputAny_.size();
        size_t i = 0;
        while (i < n) {
            if (inputAny_[i] == 0) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < n && inputAny_[j] != 0) {
                ++j;
            }
            const float xa = ToX(inputAnyFirst_ + i);
            const float xb = std::max(xa + 1.0f, ToX(inputAnyFirst_ + j));
            dl->AddRectFilled(ImVec2(xa, y + 3.0f), ImVec2(xb, y + rowH - 3.0f), inCol);
            i = j;
        }
    }
    ++row;

    // ---- 上書き区間 (適用済みは淡く) ----
    Label(row, Tr(StrId::TT_RowOverride), dimCol);
    for (const InputOverride& o : tt.Overrides().items) {
        const bool done = o.toTick <= ctx.tickIndex;
        const uint64_t a = std::clamp(o.fromTick, first, axisEnd);
        const uint64_t z = std::clamp(o.toTick, first, axisEnd);
        Bar(row, a, z, WithAlpha(warnCol, done ? 0.35f : 0.9f));
    }
    ++row;

    // ---- 現在 tick の縦線 (全行を貫く) ----
    const float xNow = ToX(std::clamp<uint64_t>(ctx.tickIndex, first, axisEnd));
    dl->AddLine(ImVec2(xNow, origin.y - 1.0f), ImVec2(xNow, origin.y + totalH), textCol, 1.0f);

    // ---- 操作: クリック / ドラッグでシーク、ホイールで ±1 (Shift で ±30) ----
    ImGui::SetCursorScreenPos(ImVec2(x0, origin.y));
    ImGui::InvisibleButton("##ttstrip", ImVec2(width, totalH));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered || active) {
        const float mx = std::clamp(ImGui::GetIO().MousePos.x, x0, x0 + width);
        const uint64_t at = first + static_cast<uint64_t>(static_cast<double>(mx - x0) / width * span + 0.5);
        const uint64_t t = std::min(at, liveEnd); // 分岐の未来へは直接飛べない (Switch で切り替える)
        dl->AddLine(ImVec2(mx, origin.y), ImVec2(mx, origin.y + totalH), WithAlpha(textCol, 0.35f), 1.0f);
        char tc[16] = {};
        FormatTimecode(t, tc, sizeof(tc));
        ImGui::SetTooltip(Tr(StrId::TT_StripHint), static_cast<unsigned long long>(t), tc);
        if (active && dragTick_ != static_cast<long long>(t)) {
            dragTick_ = static_cast<long long>(t);
            Seek(ctx, tt, playMode, t);
        }
    }
    if (!active) {
        dragTick_ = -1;
    }
    if (hovered) {
        // ホイールは帯の上ではシークに使う (窓のスクロールへ渡さない)。上 = 過去へ
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const int64_t delta = (ImGui::GetIO().KeyShift ? 30 : 1) * (wheel > 0.0f ? -1 : 1);
            const int64_t target = static_cast<int64_t>(ctx.tickIndex) + delta;
            Seek(ctx, tt, playMode,
                 target < static_cast<int64_t>(first) ? first : static_cast<uint64_t>(target));
        }
    }
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
        // レーン名はリンク: 押すと分岐点 (fork tick) へシーク (M73b)
        {
            char name[32] = {};
            std::snprintf(name, sizeof(name), Tr(StrId::TT_LaneName), b.id,
                          static_cast<unsigned long long>(b.forkTick));
            ImGui::PushStyleColor(ImGuiCol_TextLink, ToImVec4(BranchLaneColor(b.id)));
            if (ImGui::TextLink(name)) {
                Seek(ctx, tt, playMode, b.forkTick);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::TT_TipFork));
            }
        }
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
            // 乖離 tick もリンク: 押すとその tick へシーク (両レーンに存在する範囲なので安全)
            char at[32] = {};
            std::snprintf(at, sizeof(at), Tr(StrId::TT_DivAt), static_cast<unsigned long long>(d.firstTick));
            if (ImGui::TextLink(at)) {
                Seek(ctx, tt, playMode, d.firstTick);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::TT_TipDivergence));
            }
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
        // ---- フィールド差分 (M72g): 乖離した tick で 2 レーンをダンプして比べる ----
        if (d.comparable && d.diverged) {
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(StrId::TT_Diff))) {
                playMode.Pause();
                tt.RequestDiff(TimeTravel::kLiveLane, b.id, d.firstTick);
                pendingPos_ = -1;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(Tr(StrId::TT_DiffHint), static_cast<unsigned long long>(d.firstTick));
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (toDelete != 0) {
        tt.DeleteBranch(toDelete); // ループの外で消す (Branches() を舐めている最中に縮めない)
    }
    ImGui::TextDisabled(Tr(StrId::TT_BranchNote), static_cast<int>(tt.Config().maxBranches));
}

void TimelineWindow::DrawOverrides(EngineContext& ctx, TimeTravel& tt)
{
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

void TimelineWindow::DrawDiff(EngineContext& ctx, TimeTravel& tt)
{
    const DiffReport& r = tt.LastDiff();
    if (!r.valid) {
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::TT_DiffFailed));
        return;
    }
    ImGui::Text(Tr(StrId::TT_DiffHeader), r.laneB, static_cast<unsigned long long>(r.tick),
                static_cast<unsigned long long>(r.diff.valueDiffs),
                static_cast<unsigned long long>(r.diff.rollupDiffs), r.ms);
    if (!r.restoredOk) {
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::TT_DiffNotRestored));
    }
    if (r.diff.structureDiffers) {
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::TT_DiffStructure));
    }
    if (r.lines.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::TT_DiffNone));
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
        | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    const float rows = static_cast<float>(std::min<size_t>(r.lines.size(), 12)) + 1.5f;
    if (ImGui::BeginTable("##ttdiff", 4, flags, ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * rows))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(Tr(StrId::TT_DiffColEntity));
        ImGui::TableSetupColumn(Tr(StrId::TT_DiffColField));
        ImGui::TableSetupColumn(Tr(StrId::TT_DiffColLive));
        ImGui::TableSetupColumn(Tr(StrId::TT_DiffColBranch));
        ImGui::TableHeadersRow();
        for (const std::string& line : r.lines) {
            // 5 列 (entity / 名前 / comp.field / A / B) のタブ区切り。entity と名前は 1 列にまとめる
            std::string cols[5];
            size_t start = 0;
            for (int c = 0; c < 5; ++c) {
                const size_t tab = line.find('\t', start);
                cols[c] = line.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
                if (tab == std::string::npos) {
                    break;
                }
                start = tab + 1;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s %s", cols[1].c_str(), cols[0].c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cols[2].c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cols[3].c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cols[4].c_str());
        }
        ImGui::EndTable();
    }
    if (r.diff.valueDiffs > r.lines.size()) {
        ImGui::TextDisabled(Tr(StrId::TT_DiffMore),
                            static_cast<unsigned long long>(r.diff.valueDiffs - r.lines.size()));
    }
    (void)ctx;
}

} // namespace mye
