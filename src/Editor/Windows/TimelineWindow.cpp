#include "Editor/Windows/TimelineWindow.h"

#include <algorithm>

#include "Editor/PlayModeController.h"
#include "Engine/Core/Localization.h"
#include "Engine/Engine/Replay/TimeTravel.h"

#include "fontawesome/IconsFontAwesome6.h"
#include "imgui.h"

namespace mye {

void TimelineWindow::OnImGui(EngineContext& ctx, PlayModeController& playMode)
{
    if (!open) {
        return;
    }
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
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "%s", Tr(StrId::TT_Scrubbing));
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
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), Tr(StrId::TT_SeekMismatch),
                               static_cast<unsigned long long>(s.target));
            break;
        default:
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", Tr(StrId::TT_SeekFailed));
            break;
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", Tr(StrId::TT_CsharpNote));
    ImGui::End();
}

} // namespace mye
