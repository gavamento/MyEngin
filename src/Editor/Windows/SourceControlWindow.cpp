#include "Editor/Windows/SourceControlWindow.h"

#include <algorithm>

#include "Editor/SourceControl/PairRule.h"
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

// History に読み込む件数 (spec の M66c: log{n=100})
constexpr int kHistoryCount = 100;

// "2026-09-03T00:18:55+09:00" -> "2026-09-03 00:18"。
// ★サービスは git の %aI をそのまま返す。整形は表示側の仕事にしてある
//   (サービス側で削ると「並べ替えに使える形」を失う)
std::string ShortDate(const std::string& iso)
{
    if (iso.size() < 16) {
        return iso;
    }
    std::string s = iso.substr(0, 16);
    if (s[10] == 'T') {
        s[10] = ' ';
    }
    return s;
}

// 40 桁 SHA -> 表示用の 7 桁
std::string ShortSha(const std::string& sha)
{
    return sha.size() > 7 ? sha.substr(0, 7) : sha;
}

// 空白だけの本文を「書いた」と見なさない (git も空本文の commit を拒否する)。
// ★const char* で受ける — std::string で受けると固定バッファから毎フレーム
//   一時オブジェクトを作ることになる
bool HasVisibleText(const char* s)
{
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != 0; ++p) {
        if (*p > ' ') {
            return true;
        }
    }
    return false;
}

} // namespace

bool SourceControlWindow::TakeAdoptCanonicalRoot()
{
    const bool v = adoptRequested_;
    adoptRequested_ = false;
    return v;
}

void SourceControlWindow::OnImGui(SourceControlSession& scm, const SourceControlHost& host)
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
            DrawChanges(scm, host);
            ImGui::EndTabItem();
        }
        // Branches は枠だけ (M66e で中身が入る)。
        // ★タブを先に出しておくのは「どこに何が来るか」を先に固定するため
        if (ImGui::BeginTabItem(Tr(StrId::Scm_TabBranches))) {
            ImGui::TextDisabled("%s", Tr(StrId::Scm_ComingSoon));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(Tr(StrId::Scm_TabHistory))) {
            DrawHistory(scm);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void SourceControlWindow::DrawHeader(SourceControlSession& scm)
{
    const SourceControlModel& model = scm.Model();
    // ★status がまだ 1 度も返っていない間にブランチ欄を描かない。
    //   空の branch を detached と読むと、起動直後の 1 秒弱だけ
    //   **「(detached HEAD)」という嘘**が出る (M66c の初回プローブで実際に撮れた)
    if (!model.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_Loading));
    } else {
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
        // identity は「エディタを開いたまま別端末で git config を直した」経路が
        // あるので、更新のたびに聞き直す (git config 2 回 = 十分に安い)
        scm.RequestIdentity();
        if (scm.HistoryValid()) {
            scm.RequestLog(kHistoryCount);
        }
        // 差分も取り直す (中身が変わっていても選択は変わらないので、
        // 二重要求よけの記録を消してから SyncDiffRequest に拾わせる)
        diffRequestedPath_.clear();
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

void SourceControlWindow::DrawBlockerTooltip(const std::vector<GateBlocker>& blockers)
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(Tr(StrId::Scm_Blocked));
    for (const GateBlocker b : blockers) {
        // 全件出す。1 件だけ出すと「直したのにまだ押せない」を繰り返させることになる
        ImGui::BulletText("%s", Tr(GateBlockerText(b)));
    }
    ImGui::EndTooltip();
}

void SourceControlWindow::CollectRevertTargets(const std::vector<PairedEntry>& rows,
                                               std::vector<std::string>& paths, int& untracked)
{
    // ★対の規則で束ねる。本体だけ戻して `.meta` を残すと、次の status で
    //   「.meta だけ変更されている」行が出る = 直したはずのものが直っていない
    paths = pairrule::ListedPaths(rows);
    untracked = 0;
    for (const PairedEntry& row : rows) {
        // 未追跡 = 消える。件数を数えて確認モーダルに出す (元に戻せない操作なので、
        // 「何件消えるか」を押す前に見せる)
        if (row.state == ChangeState::Untracked) {
            ++untracked;
        }
    }
}

std::vector<PairedEntry> SourceControlWindow::SelectedRows(const SourceControlModel& model) const
{
    std::vector<PairedEntry> rows;
    rows.reserve(selected_.size());
    for (const std::string& path : selected_) {
        // status が更新されて消えた行は黙って落とす (stage した直後に選択が
        // 残っていても、次の status で行そのものが消えることがある)
        if (const PairedEntry* e = model.FindEntry(path)) {
            rows.push_back(*e);
        }
    }
    return rows;
}

void SourceControlWindow::SyncDiffRequest(SourceControlSession& scm)
{
    // 差分は 1 件選択のときだけ。複数選択で「どれの差分か」を推測させない
    if (selected_.size() != 1) {
        diffRequestedPath_.clear();
        return;
    }
    if (selected_[0] == diffRequestedPath_ && diffStaged_ == diffRequestedStaged_) {
        return; // 同じ要求を毎フレーム投げない
    }
    diffRequestedPath_ = selected_[0];
    diffRequestedStaged_ = diffStaged_;
    scm.RequestDiff(diffRequestedPath_, diffStaged_);
}

void SourceControlWindow::DrawChanges(SourceControlSession& scm, const SourceControlHost& host)
{
    const SourceControlModel& model = scm.Model();
    if (!model.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_Loading));
        return;
    }

    // ---- 操作列 ----
    // ★書き込みが飛んでいる間 (WriteInFlight) は塞ぐ。git は index.lock を握るので
    //   連打すると自分同士で locked_index を踏む
    const bool busy = scm.WriteInFlight();
    const std::vector<PairedEntry> rows = SelectedRows(model);
    ImGui::BeginDisabled(rows.empty() || busy);
    if (ImGui::Button(Tr(StrId::Scm_Stage))) {
        scm.StageRows(rows);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Scm_Unstage))) {
        scm.UnstageRows(rows);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (busy) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_Busy));
    } else if (rows.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_SelectToStage));
    } else {
        ImGui::TextDisabled(Tr(StrId::Scm_SelectedCount), static_cast<int>(rows.size()));
    }

    // ---- 破棄 (M66d) ----
    // ★stage / unstage と違い、破棄は working tree を書き換える = **ゲートを通す**。
    //   再生中や未保存のまま実行すると、エディタが掴んでいるファイルが下から
    //   差し替わる (未追跡ファイルに至っては消える)。
    // ★行を分けているのは幅の問題だけではない — 「index を動かすだけの操作」と
    //   「ディスクを書き換えて元に戻せない操作」を同じ行に並べない
    //   (既定のドック幅 285px では 4 個目のボタンのラベルが実際に切れた)
    const bool gateOpen = host.writeBlockers.empty() && host.requestRevert;
    ImGui::BeginDisabled(rows.empty() || busy || !gateOpen);
    if (ImGui::Button(Tr(StrId::Scm_Discard))) {
        std::vector<std::string> paths;
        int untracked = 0;
        CollectRevertTargets(rows, paths, untracked);
        if (!paths.empty()) {
            host.requestRevert(std::move(paths), untracked);
        }
    }
    ImGui::EndDisabled();
    // ★BeginDisabled の**外**でツールチップを出す。中に置くと ImGui が
    //   ホバー判定ごと殺すので「押せない理由」が永久に読めない
    if (!gateOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        DrawBlockerTooltip(host.writeBlockers);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(model.entries.empty() || busy || !gateOpen);
    if (ImGui::Button(Tr(StrId::Scm_DiscardAll))) {
        std::vector<std::string> paths;
        int untracked = 0;
        CollectRevertTargets(model.entries, paths, untracked);
        if (!paths.empty()) {
            host.requestRevert(std::move(paths), untracked);
        }
    }
    ImGui::EndDisabled();
    if (!gateOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        DrawBlockerTooltip(host.writeBlockers);
    }
    ImGui::Separator();

    if (model.entries.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_NoChanges));
    } else {
        ImGui::Text(Tr(StrId::Scm_ChangeCount), model.ChangedCount());
    }

    // ---- 一覧 / 差分 / コミット欄の高さ配分 ----
    // ★コミット欄の高さを**先に**確保してから残りを分ける。逆にすると窓を縮めた
    //   ときにコミットボタンが窓の外へ押し出されて押せなくなる
    const ImGuiStyle& style = ImGui::GetStyle();
    // ★既定のドック帯 (Assets と同束 = 200px 強) では 3 行の入力欄 + ラベルが入らず、
    //   コミットボタンが窓の外へ落ちる (M66c のプローブで実際に撮れた)。
    //   窓が低いときはラベルを省き入力欄を 2 行にする = 「選ぶ→書く→押す」が
    //   スクロールなしで完結する側を優先する
    const bool compact = ImGui::GetContentRegionAvail().y < 280.0f;
    const float msgLines = compact ? 2.0f : 3.0f;
    float commitH = (compact ? 0.0f : ImGui::GetTextLineHeightWithSpacing())     // ラベル
        + ImGui::GetTextLineHeight() * msgLines + style.FramePadding.y * 2.0f    // 入力欄
        + style.ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();              // ボタン行
    if (host.sceneDirty) {
        commitH += ImGui::GetTextLineHeightWithSpacing();
    }
    if (scm.IdentityChecked() && !scm.IdentityOk()) {
        commitH += ImGui::GetTextLineHeightWithSpacing();
    }
    const float avail = ImGui::GetContentRegionAvail().y;
    const float diffHeader = ImGui::GetFrameHeightWithSpacing();
    const float rest = avail - commitH - style.ItemSpacing.y * 2.0f;
    // ★既定のドック (Assets と同じ下段の帯 = 200px 強) では 3 段は入らない。
    //   入らないときに削るのは**差分ペイン**で、コミット欄ではない — 一覧と
    //   コミット欄が窓の外へ押し出されると「選ぶ」も「コミットする」も
    //   できなくなり、窓が何のためにあるのか分からなくなる。
    //   差分を見たい人は窓を広げるか切り離せば戻ってくる
    constexpr float kDiffPaneMinRoom = 220.0f;
    const bool showDiff = rest >= kDiffPaneMinRoom;
    const float listH = showDiff ? (rest - diffHeader) * 0.55f : (std::max)(60.0f, rest);

    if (ImGui::BeginChild("###ScmChangeList", ImVec2(0, listH), ImGuiChildFlags_Borders)) {
        for (const int child : model.nodes[0].children) {
            DrawNode(model, child);
        }
    }
    ImGui::EndChild();

    SyncDiffRequest(scm);
    if (showDiff) {
        DrawDiffPane(scm, rest - diffHeader - listH);
    }
    ImGui::Separator();
    DrawCommitBox(scm, host, compact);
}

void SourceControlWindow::DrawDiffPane(SourceControlSession& scm, float height)
{
    const DiffView& diff = scm.Diff();
    if (ImGui::Checkbox(Tr(StrId::Scm_DiffStaged), &diffStaged_)) {
        diffRequestedPath_.clear(); // 次のフレームで取り直す
    }
    ImGui::SameLine();
    if (selected_.size() == 1) {
        ImGui::TextDisabled("%s", selected_[0].c_str());
    } else {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_DiffPick));
    }
    // ★横スクロールは**差分本文があるときだけ**有効にする。案内文まで横スクロール
    //   領域に入れると、窓が細いときに 1 行が右へ流れて読めなくなる
    //   (M66c のプローブで "No textual diff (new, binary or unchanged on thi" と切れた)
    const bool hasDiffText = selected_.size() == 1 && diff.valid && !diff.loading
        && !diff.text.empty();
    if (ImGui::BeginChild("###ScmDiffView", ImVec2(0, height), ImGuiChildFlags_Borders,
                          hasDiffText ? ImGuiWindowFlags_HorizontalScrollbar : 0)) {
        if (selected_.size() != 1) {
            ImGui::TextWrapped("%s", Tr(StrId::Scm_DiffPick));
        } else if (diff.loading || !diff.valid) {
            ImGui::TextWrapped("%s", Tr(StrId::Scm_Loading));
        } else if (diff.text.empty()) {
            // 未追跡ファイル・バイナリ・そちら側は無変更、のいずれか。
            // git が何も出さないので、こちらで理由を断定はしない
            ImGui::TextWrapped("%s", Tr(StrId::Scm_DiffEmpty));
        } else {
            // ★TextUnformatted に (begin, end) を渡す。std::string を c_str() で
            //   渡すと ImGui が内部で strlen を舐めるので、数百 KB の差分で
            //   毎フレーム走査が入る
            ImGui::TextUnformatted(diff.text.c_str(), diff.text.c_str() + diff.text.size());
            if (diff.truncated) {
                ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
                ImGui::TextWrapped("%s", Tr(StrId::Scm_DiffTruncated));
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndChild();
}

void SourceControlWindow::DrawCommitBox(SourceControlSession& scm, const SourceControlHost& host,
                                       bool compact)
{
    if (!compact) {
        ImGui::TextUnformatted(Tr(StrId::Scm_CommitMessage));
    }
    ImGui::InputTextMultiline("###ScmCommitMessage", commitMessage_, sizeof(commitMessage_),
                              ImVec2(-1.0f, ImGui::GetTextLineHeight() * (compact ? 2.0f : 3.0f)
                                                + ImGui::GetStyle().FramePadding.y * 2.0f));
    if (host.sceneDirty) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextUnformatted(Tr(StrId::Scm_UnsavedNotIncluded));
        ImGui::PopStyleColor();
    }
    // ★identity は「まだ聞いていない」と「未設定」を区別する。起動直後の
    //   1 フレームで案内が出ると、設定済みの人にも一瞬警告が見える
    const bool identityMissing = scm.IdentityChecked() && !scm.IdentityOk();
    if (identityMissing) {
        // ★案内を出すだけでなく**ボタンも塞ぐ** (spec §4.1「commit 周り」、M66d)。
        //   実測: user.name だけ未設定でも git は OS アカウント名 + 機体名
        //   (`akita@DESKTOP-....(none)`) で補完して commit に**成功する**。
        //   つまり「git に任せる」を選ぶと、共有する履歴に誰のものか分からない
        //   author が静かに混ざる。設定 UI は作らない (決定 6) ので案内 + 無効化まで
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextWrapped("%s", Tr(StrId::Scm_IdentitySetup));
        ImGui::PopStyleColor();
    }

    const bool canCommit =
        HasVisibleText(commitMessage_) && !scm.WriteInFlight() && !identityMissing;
    ImGui::BeginDisabled(!canCommit);
    if (ImGui::Button(Tr(StrId::Scm_Commit))) {
        scm.Commit(commitMessage_);
        commitMessage_[0] = '\0';
    }
    if (host.sceneDirty) {
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Scm_SaveAndCommit))) {
            // 保存 -> 保存した文書を対の規則で stage -> commit の 3 手を 1 操作で。
            // ★stage を挟むのが要点。保存しただけでは index は古いままなので、
            //   そのまま commit すると**保存前の中身**が記録される (「保存して
            //   コミット」を押した人の意図と正反対のものが残る)
            const std::wstring saved = host.saveDocument ? host.saveDocument() : std::wstring();
            if (!saved.empty()) {
                scm.StageSavedPath(saved);
            }
            scm.Commit(commitMessage_);
            commitMessage_[0] = '\0';
        }
    }
    ImGui::EndDisabled();
}

void SourceControlWindow::DrawHistory(SourceControlSession& scm)
{
    if (!historyRequested_) {
        // タブを開いた最初のフレームで 1 回だけ取りに行く
        // (窓を開いているだけで log が走り続けるのは無駄)
        historyRequested_ = true;
        scm.RequestLog(kHistoryCount);
    }
    if (!scm.HistoryValid()) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_Loading));
        return;
    }
    const std::vector<CommitInfo>& history = scm.History();
    if (history.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_HistoryEmpty));
        return;
    }
    const float detailH = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
    const float listH = (std::max)(80.0f, ImGui::GetContentRegionAvail().y - detailH);
    if (ImGui::BeginChild("###ScmHistoryList", ImVec2(0, listH), ImGuiChildFlags_Borders)) {
        for (const CommitInfo& c : history) {
            ImGui::PushID(c.sha.c_str());
            if (ImGui::Selectable("###ScmHistoryRow", selectedCommit_ == c.sha)) {
                selectedCommit_ = c.sha;
            }
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("%s  %s  %s", ShortSha(c.sha).c_str(), ShortDate(c.date).c_str(),
                                c.author.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(c.subject.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    // 選択したコミットの件名を**折り返して全文**出す (一覧では窓幅で切れるため)
    const CommitInfo* picked = nullptr;
    for (const CommitInfo& c : history) {
        if (c.sha == selectedCommit_) {
            picked = &c;
        }
    }
    if (picked == nullptr) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_HistoryPick));
        return;
    }
    ImGui::TextWrapped("%s", picked->subject.c_str());
    ImGui::TextDisabled("%s", picked->sha.c_str());
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
