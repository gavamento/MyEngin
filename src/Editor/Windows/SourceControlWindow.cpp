#include "Editor/Windows/SourceControlWindow.h"

#include <algorithm>

#include "Engine/Core/Localization.h"
#include "Engine/Renderer/ImGuiTheme.h" // themeColor (意味色。バッジ色はここからしか採らない)

#include "imgui.h"

namespace mye {

namespace {

// 状態 → 意味色。ImGuiTheme の 5 箇条どおり themeColor::* だけを使う
// (直値の ImVec4 を書くとテーマ切替で 1 箇所だけ取り残される)
ImVec4 BadgeColor(ChangeState s)
{
    switch (s) {
    case ChangeState::Conflict:
        return themeColor::Error;
    case ChangeState::Deleted:
        return themeColor::Warning;
    case ChangeState::Added:
        return themeColor::Success;
    case ChangeState::Renamed:
        return themeColor::Prefab;
    case ChangeState::Modified:
        return themeColor::Accent;
    case ChangeState::Untracked:
    case ChangeState::None:
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }
}

// 利用不可の理由 → 文言
StrId UnavailableText(Unavailable u)
{
    switch (u) {
    case Unavailable::NoProject:
        return StrId::Scm_NoProject;
    case Unavailable::NoService:
        return StrId::Scm_NoService;
    case Unavailable::ProtoMismatch:
        return StrId::Scm_ProtoMismatch;
    case Unavailable::NoGit:
        return StrId::Scm_NoGit;
    case Unavailable::GitTooOld:
        return StrId::Scm_GitTooOld;
    case Unavailable::NotRepo:
        return StrId::Scm_NotRepo;
    case Unavailable::ToplevelMismatch:
        return StrId::Scm_ToplevelMismatch;
    case Unavailable::ServiceDied:
    default:
        return StrId::Scm_ServiceDied;
    }
}

// error.code → 文言 (spec §4.3)。表に無い code は nullptr = detail を生で出す
const char* ErrorText(const std::string& code)
{
    struct Row {
        const char* code;
        StrId id;
    };
    static const Row kTable[] = {
        { "not_repo", StrId::ScmErr_NotRepo },
        { "toplevel_mismatch", StrId::ScmErr_ToplevelMismatch },
        { "git_missing", StrId::ScmErr_GitMissing },
        { "git_too_old", StrId::ScmErr_GitTooOld },
        { "identity_missing", StrId::ScmErr_IdentityMissing },
        { "local_changes_overwritten", StrId::ScmErr_LocalChanges },
        { "locked_index", StrId::ScmErr_LockedIndex },
        { "locked_file", StrId::ScmErr_LockedFile },
        { "auth_failed", StrId::ScmErr_AuthFailed },
        { "non_fast_forward", StrId::ScmErr_NonFastForward },
        { "conflict", StrId::ScmErr_Conflict },
        { "merge_in_progress", StrId::ScmErr_MergeInProgress },
        { "nothing_to_commit", StrId::ScmErr_NothingToCommit },
        { "network", StrId::ScmErr_Network },
        { "internal_panic", StrId::ScmErr_InternalPanic },
        { "service_dead", StrId::ScmErr_ServiceDead },
        { "bad_request", StrId::ScmErr_BadRequest },
        { "git_failed", StrId::ScmErr_GitFailed },
        { "timeout", StrId::ScmErr_Timeout },
    };
    for (const Row& r : kTable) {
        if (code == r.code) {
            return Tr(r.id);
        }
    }
    return nullptr;
}

} // namespace

bool SourceControlWindow::TakeAdoptCanonicalRoot()
{
    const bool v = adoptRequested_;
    adoptRequested_ = false;
    return v;
}

void SourceControlWindow::OnImGui(SourceControlSession& scm)
{
    if (!open) {
        return;
    }
    // 浮いた状態で最初に開いたときの大きさ。既定 (ImGui の自動サイズ) だと
    // ヘッダ 1 行分しか無い窓になり、変更一覧が 1 件も見えない
    ImGui::SetNextWindowSize(ImVec2(420.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Win_SourceControl), &open)) {
        ImGui::End();
        return;
    }

    const Unavailable state = scm.State();
    if (state != Unavailable::None) {
        // 「使えない」を 1 つの文言に潰さない — 理由ごとにユーザーがすべきことが違う
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(UnavailableText(state)));
        ImGui::PopStyleColor();
        if (state == Unavailable::NotRepo) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", Tr(StrId::Scm_NotRepoHint));
        } else if (state == Unavailable::ToplevelMismatch && !scm.Toplevel().empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped(Tr(StrId::Scm_ToplevelHint), scm.Toplevel().c_str());
        }
        ImGui::End();
        return;
    }

    DrawHeader(scm);
    ImGui::Separator();

    if (ImGui::BeginTabBar("###ScmTabs")) {
        if (ImGui::BeginTabItem(Tr(StrId::Scm_TabChanges))) {
            DrawChanges(scm.Model());
            ImGui::EndTabItem();
        }
        // Branches / History は枠だけ (M66e / M66c で中身が入る)。
        // ★タブを先に出しておくのは「どこに何が来るか」を先に固定するため
        if (ImGui::BeginTabItem(Tr(StrId::Scm_TabBranches))) {
            ImGui::TextDisabled("%s", Tr(StrId::Scm_ComingSoon));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(Tr(StrId::Scm_TabHistory))) {
            ImGui::TextDisabled("%s", Tr(StrId::Scm_ComingSoon));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void SourceControlWindow::DrawHeader(SourceControlSession& scm)
{
    const SourceControlModel& model = scm.Model();
    const char* branch = model.branch.empty()
        ? Tr(StrId::Scm_Detached)
        : (model.branch == "(detached)" ? Tr(StrId::Scm_Detached) : model.branch.c_str());
    ImGui::TextUnformatted(branch);
    ImGui::SameLine();
    if (model.upstream.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_NoUpstream));
    } else {
        ImGui::TextDisabled("%s", model.upstream.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(Tr(StrId::Scm_AheadBehind), model.ahead, model.behind);
    }

    // ボタンは右寄せ。ただし**幅が足りなければ折り返して次の行に置く**。
    // ★GetContentRegionAvail() から引いた座標をそのまま SameLine へ渡すと、窓を細くした
    //   ときに位置が直前のテキストより左になり、**ブランチ名の上にボタンが重なる**
    //   (M66b の初回プローブで実際にそうなった)。直前の行の右端と比べてから決める
    {
        constexpr float kButtonsWidth = 160.0f;
        // GetItemRectMax はスクリーン座標。SameLine が受け取るのは窓ローカル座標
        const float usedX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
        const float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kButtonsWidth;
        if (rightX > usedX + ImGui::GetStyle().ItemSpacing.x) {
            ImGui::SameLine(rightX);
        }
    }
    if (ImGui::Button(Tr(StrId::Scm_Refresh))) {
        scm.RequestStatus();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Scm_Settings))) {
        ImGui::OpenPopup("###ScmSettingsPopup");
    }
    if (ImGui::BeginPopup("###ScmSettingsPopup")) {
        // 枠だけ (背景 fetch の間隔などは M66f)
        ImGui::TextDisabled("%s", Tr(StrId::Scm_SettingsEmpty));
        ImGui::EndPopup();
    }

    if (scm.MergeInProgress()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
        ImGui::TextWrapped("%s", Tr(StrId::Scm_MergeInProgress));
        ImGui::PopStyleColor();
    } else if (scm.RebaseInProgress()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::Scm_RebaseInProgress));
        ImGui::PopStyleColor();
    }

    // canonicalRoot の食い違い (spec §4.2)。**止めない** — 知らせて、直す口を出すだけ
    if (scm.CanonicalRootMismatch()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::Scm_CanonicalMismatch));
        ImGui::PopStyleColor();
        ImGui::TextDisabled(Tr(StrId::Scm_CanonicalRecorded), scm.CanonicalRoot().c_str());
        if (ImGui::Button(Tr(StrId::Scm_AdoptCanonical))) {
            adoptRequested_ = true;
        }
    }

    if (!scm.ErrorCode().empty()) {
        const char* text = ErrorText(scm.ErrorCode());
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
        if (text != nullptr) {
            ImGui::TextWrapped("%s", text);
        } else {
            // 未知の code は生のまま出す (握り潰すと原因が永久に分からない)
            ImGui::TextWrapped("%s: %s", scm.ErrorCode().c_str(), scm.ErrorDetail().c_str());
        }
        ImGui::PopStyleColor();
    }
}

void SourceControlWindow::DrawChanges(const SourceControlModel& model)
{
    if (!model.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_Loading));
        return;
    }
    if (model.entries.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_NoChanges));
        return;
    }
    ImGui::Text(Tr(StrId::Scm_ChangeCount), model.ChangedCount());
    ImGui::Separator();
    if (ImGui::BeginChild("###ScmChangeList", ImVec2(0, 0), ImGuiChildFlags_None)) {
        for (const int child : model.nodes[0].children) {
            DrawNode(model, child);
        }
    }
    ImGui::EndChild();
}

void SourceControlWindow::DrawNode(const SourceControlModel& model, int index)
{
    const ScmNode& node = model.nodes[static_cast<size_t>(index)];
    // 行頭にバッジ、その右にラベル。
    // ★ImGui::SameLine(x) は**窓ローカルの絶対座標**で、TreePush が積んだインデントを
    //   見ない。固定値 (28.0f) を渡すと、深い階層の行ではラベルがバッジより左に来て
    //   **文字が重なる** (M66b のプローブで main.scene.json の頭が潰れた)。
    //   今の行の開始 x (= インデント込み) を基準にすること
    constexpr float kBadgeWidth = 18.0f;
    const float rowX = ImGui::GetCursorPosX();
    const char* badge = ChangeStateBadge(node.state);
    ImGui::PushStyleColor(ImGuiCol_Text, BadgeColor(node.state));
    ImGui::TextUnformatted(badge[0] != '\0' ? badge : " ");
    ImGui::PopStyleColor();
    ImGui::SameLine(rowX + kBadgeWidth);

    if (node.folder) {
        // ★ID にフルパスを使う。名前だけだと同名フォルダ (assets\a\x と assets\b\x) が
        //   同じ ID になり、片方を開くと両方開く
        ImGui::PushID(node.path.c_str());
        const bool opened = ImGui::TreeNodeEx(node.name.c_str(),
                                              ImGuiTreeNodeFlags_DefaultOpen
                                                  | ImGuiTreeNodeFlags_SpanAvailWidth);
        if (opened) {
            for (const int child : node.children) {
                DrawNode(model, child);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
        return;
    }

    const PairedEntry& entry = model.entries[static_cast<size_t>(node.entry)];
    const bool isSelected =
        std::find(selected_.begin(), selected_.end(), entry.path) != selected_.end();
    ImGui::PushID(node.path.c_str());
    if (ImGui::Selectable(node.name.c_str(), isSelected)) {
        if (ImGui::GetIO().KeyCtrl) {
            auto it = std::find(selected_.begin(), selected_.end(), entry.path);
            if (it != selected_.end()) {
                selected_.erase(it);
            } else {
                selected_.push_back(entry.path);
                // 選択順ではなく path 昇順で保つ = 後続サブ (stage/revert) が
                // 「どの順で git に渡したか」で結果が変わらない
                std::sort(selected_.begin(), selected_.end());
            }
        } else {
            selected_.assign(1, entry.path);
        }
    }
    if (!entry.sidecars.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(Tr(StrId::Scm_SidecarCount), static_cast<int>(entry.sidecars.size()));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(entry.path.c_str());
        if (!entry.oldPath.empty()) {
            ImGui::Text(Tr(StrId::Scm_RenamedFrom), entry.oldPath.c_str());
        }
        for (const std::string& side : entry.sidecars) {
            ImGui::TextDisabled("%s", side.c_str());
        }
        if (!entry.sidecars.empty()) {
            ImGui::TextDisabled("%s", Tr(StrId::Scm_SidecarNote));
        }
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

} // namespace mye
