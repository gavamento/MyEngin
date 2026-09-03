#include "Editor/Windows/SourceControlWindow.h"

#include <algorithm>
#include <cstdio>

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

// 競合の種別 (サービスの綴り) → 文言 (M66g)。
// ★表に無い綴りは「マージできませんでした」に落とす。git が組を増やしても
//   表示が「不明」になるだけで、解決の操作 (ours / theirs) は効き続ける
StrId ConflictKindText(const std::string& kind)
{
    struct Row {
        const char* kind;
        StrId id;
    };
    static const Row kTable[] = {
        { "both_modified", StrId::ScmConf_BothModified },
        { "added_by_both", StrId::ScmConf_AddedByBoth },
        { "deleted_by_them", StrId::ScmConf_DeletedByThem },
        { "deleted_by_us", StrId::ScmConf_DeletedByUs },
        { "both_deleted", StrId::ScmConf_BothDeleted },
        { "added_by_us", StrId::ScmConf_AddedByUs },
        { "added_by_them", StrId::ScmConf_AddedByThem },
    };
    for (const Row& r : kTable) {
        if (kind == r.kind) {
            return r.id;
        }
    }
    return StrId::ScmConf_Unmerged;
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

std::string SourceControlWindow::TakeCreatedBranch()
{
    std::string name;
    name.swap(createdBranch_);
    return name;
}

std::string SourceControlWindow::TakePushed()
{
    std::string target;
    target.swap(pushedTo_);
    return target;
}

void SourceControlWindow::OnImGui(SourceControlSession& scm, const SourceControlHost& host)
{
    // ★差分の窓は Source Control 窓の開閉と**独立**に描く。ここを `open` の内側に
    //   入れると、Source Control を閉じた瞬間に差分の窓が消えて二度と閉じられなくなる
    //   (ドックのタブだけが残る)
    DrawDiffWindow(scm);
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

    DrawHeader(scm, host);
    ImGui::Separator();

    if (ImGui::BeginTabBar("###ScmTabs")) {
        if (ImGui::BeginTabItem(Tr(StrId::Scm_TabChanges))) {
            DrawChanges(scm, host);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(Tr(StrId::Scm_TabBranches))) {
            DrawBranches(scm, host);
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

void SourceControlWindow::DrawHeader(SourceControlSession& scm, const SourceControlHost& host)
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
        if (scm.Branches().valid) {
            // 一度でも Branches タブを開いていれば取り直す (外で `git branch` を
            // 叩かれた分はこれでしか入ってこない)
            scm.RequestBranches();
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
        // ★背景 fetch の設定 (M66f)。**個人設定**なので EditorSettings 側にある。
        //   窓は値を預かって編集するだけで、保存と hello の再送は EditorApp が行う
        //   (窓が EditorSettings を直接触ると、保存の契機が 2 箇所に散る)
        bool autoFetch = host.autoFetch;
        int interval = host.fetchIntervalMin;
        bool changed = false;
        if (ImGui::Checkbox(Tr(StrId::Scm_AutoFetch), &autoFetch)) {
            changed = true;
        }
        ImGui::BeginDisabled(!autoFetch);
        ImGui::SetNextItemWidth(120.0f);
        // ★1 分未満にできないようにする。0 にすると worker のタイマーが毎秒 git を
        //   起動し続ける (Rust 側は 0 を許すがそれはテスト用)
        if (ImGui::InputInt(Tr(StrId::Scm_FetchInterval), &interval)) {
            interval = (std::max)(1, (std::min)(interval, 1440));
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", Tr(StrId::Scm_FetchNote));
        if (changed && host.applyFetchSettings) {
            host.applyFetchSettings(autoFetch, interval);
        }
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
        // ★「どうすればいいか」まで書く (M66f)。code の説明だけだと、押した人は
        //   ターミナルへ逃げるしかない。push はモーダルを通らないので**ここが唯一の出口**
        if (scm.ErrorCode() == collaberr::kNonFastForward) {
            ImGui::TextWrapped("%s", Tr(StrId::Scm_PushHintPullFirst));
        } else if (scm.ErrorCode() == collaberr::kAuthFailed) {
            ImGui::TextWrapped("%s", Tr(StrId::Scm_AuthHint));
        }
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

void SourceControlWindow::DrawRemoteBar(SourceControlSession& scm, const SourceControlHost& host)
{
    if (!remoteRequested_) {
        // Changes タブを最初に描いたフレームで 1 回だけ。**fetch はしない**
        // (ネットワークに出るのはユーザーが押したときと背景のタイマーだけ)
        remoteRequested_ = true;
        scm.RequestRemoteState();
    }
    const RemoteState& remote = scm.Remote();
    const SourceControlModel& model = scm.Model();
    const bool busy = scm.WriteInFlight();

    // ---- fetch / pull / push ----
    // ★既定のドック幅 (左列 ≒ 285px) に収まるのはボタン 3 個まで。ここはちょうど 3 個で、
    //   状態テキストは下の帯が受け持つ
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(Tr(StrId::Scm_Fetch))) {
        scm.RequestFetch();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    // pull は working tree を入れ替える = **ゲートを通す** (revert / checkout と同じ)。
    // ★behind が 0 のときは押せない。押しても「Already up to date」で終わるうえ、
    //   段階の事前判定 (`HEAD..@{u}`) は fetch 済みの追跡ブランチが基準なので、
    //   「まだ fetch していない状態の pull」は予測が必ず空になる (= 予測の意味が消える)
    const bool gateOpen = host.writeBlockers.empty() && host.requestPull;
    const bool canPull = model.behind > 0 && gateOpen;
    ImGui::BeginDisabled(!canPull || busy);
    if (ImGui::Button(Tr(StrId::Scm_Pull))) {
        host.requestPull();
    }
    ImGui::EndDisabled();
    // ★BeginDisabled の外でツールチップを出す (中だとホバー判定ごと殺される)
    if (!gateOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        DrawBlockerTooltip(host.writeBlockers);
    }
    ImGui::SameLine();

    // push はゲート不要 (ディスクを 1 バイトも触らない = 開いている文書に影響しない)
    ImGui::BeginDisabled(busy || (remote.valid && !remote.hasRemote));
    if (ImGui::Button(Tr(StrId::Scm_Push))) {
        // setUpstream=false: upstream が無ければサービス側が自動で -u を張る。
        // ★失敗の表示はヘッダの赤字 (ErrorText) が受け持つ — 押した直後に見る場所を
        //   2 つに分けない。成功だけ EditorApp がトーストにする
        const std::string target =
            scm.Model().upstream.empty() ? std::string("origin") : scm.Model().upstream;
        scm.Push(false, [this, target](bool ok, const std::string&, const std::string&) {
            if (ok) {
                pushedTo_ = target;
            }
        });
    }
    ImGui::EndDisabled();

    // ---- 上流との関係の帯 ----
    if (remote.valid && !remote.hasRemote) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_NoRemote));
        return;
    }
    if (model.upstream.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_NoUpstream));
        return;
    }
    if (model.behind > 0) {
        // ★展開でコミット一覧 (誰が何を入れたか)。畳んだ 1 行だけでも
        //   「取り込むものがある」が分かるようにする
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Accent);
        char label[192];
        std::snprintf(label, sizeof(label), Tr(StrId::Scm_BehindBanner), model.behind,
                      model.upstream.c_str());
        if (ImGui::ArrowButton("###ScmRemoteExpand",
                               remoteExpanded_ ? ImGuiDir_Down : ImGuiDir_Right)) {
            remoteExpanded_ = !remoteExpanded_;
            if (remoteExpanded_ && scm.Remote().commits.empty()) {
                // 帯は status の behind で出るが、一覧は remote_state でしか来ない
                scm.RequestRemoteState();
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        if (remoteExpanded_) {
            if (remote.commits.empty()) {
                ImGui::TextDisabled("%s", Tr(StrId::Scm_Loading));
            } else {
                // ★BeginChild / EndChild は**戻り値に関わらず対で呼ぶ**
                //   (この版の ImGui は畳まれていても EndChild を要求する)
                if (ImGui::BeginChild("###ScmRemoteCommits", ImVec2(0, 84.0f),
                                      ImGuiChildFlags_Borders)) {
                    for (const CommitInfo& c : remote.commits) {
                        ImGui::TextDisabled("%s", c.author.c_str());
                        ImGui::SameLine();
                        ImGui::TextUnformatted(c.subject.c_str());
                    }
                }
                ImGui::EndChild();
            }
        }
    } else if (model.ahead > 0) {
        ImGui::TextDisabled(Tr(StrId::Scm_AheadBanner), model.ahead);
    } else {
        ImGui::TextDisabled(Tr(StrId::Scm_UpToDate), model.upstream.c_str());
    }
}

void SourceControlWindow::DrawChanges(SourceControlSession& scm, const SourceControlHost& host)
{
    const SourceControlModel& model = scm.Model();
    if (!model.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_Loading));
        return;
    }
    // ★競合中は一覧ごと競合モードへ切り替える (spec §4.1 決定 9)。タブもボタンも
    //   増やさない — 競合中にできることは「解決する」「中止する」だけで、
    //   stage / commit / pull はすべてゲートで閉じている
    if (scm.MergeInProgress() || scm.RebaseInProgress() || model.HasConflict()) {
        DrawConflicts(scm, host);
        return;
    }

    DrawRemoteBar(scm, host);
    ImGui::Separator();

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
    // 差分は別窓へ出した (M66e) ので、ここは「一覧 + コミット欄」の 2 段だけ。
    // ★コミット欄の高さを先に確保し、残り全部を一覧に渡す
    const float listH = (std::max)(60.0f, avail - commitH - style.ItemSpacing.y * 2.0f);

    if (ImGui::BeginChild("###ScmChangeList", ImVec2(0, listH), ImGuiChildFlags_Borders)) {
        for (const int child : model.nodes[0].children) {
            DrawNode(model, child);
        }
    }
    ImGui::EndChild();

    SyncDiffRequest(scm);
    ImGui::Separator();
    DrawCommitBox(scm, host, compact);
}

void SourceControlWindow::DrawConflicts(SourceControlSession& scm, const SourceControlHost& host)
{
    // ★見出しは赤。競合中は「保存もコミットも切替もできない」異常な状態で、
    //   そこにいることが 1 行で分かる必要がある
    ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
    ImGui::TextWrapped("%s", Tr(scm.RebaseInProgress() ? StrId::Scm_RebaseInProgress
                                                       : StrId::Scm_ConflictMode));
    ImGui::PopStyleColor();
    const ConflictList& list = scm.Conflicts();
    if (!list.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_Loading));
        return;
    }
    const std::vector<ConflictRow> rows = BuildConflictRows(list.files);
    const bool busy = scm.WriteInFlight();
    // ★MergeInProgress を除いたゲートで判定する。含めると「競合中は競合を
    //   解決できない」という手詰まりになる (未保存 / 再生中 / ビルド中は効かせる)
    const std::vector<GateBlocker> blockers = BlockersForConflictOps(host.writeBlockers);
    const bool gateOpen = blockers.empty();
    const bool allResolved = rows.empty();

    // ---- 操作列 (既定のドック幅 285px に収まるのは 3 個まで) ----
    ImGui::BeginDisabled(busy || !gateOpen || !host.requestMergeAbort);
    if (ImGui::Button(Tr(StrId::Scm_AbortMerge))) {
        host.requestMergeAbort();
    }
    ImGui::EndDisabled();
    // ★BeginDisabled の外でツールチップを出す (中だとホバー判定ごと殺される)
    if (!gateOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        DrawBlockerTooltip(blockers);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !gateOpen || !allResolved || !host.requestMergeContinue);
    if (ImGui::Button(Tr(StrId::Scm_ContinueMerge))) {
        host.requestMergeContinue();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!allResolved) {
            // 「押せない理由」がゲートではなく**残件**であることを言う
            ImGui::SetTooltip(Tr(StrId::Scm_ConflictRemaining), static_cast<int>(rows.size()));
        } else if (!gateOpen) {
            DrawBlockerTooltip(blockers);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !host.openMergeTool);
    if (ImGui::Button(Tr(StrId::Scm_MergeTool))) {
        // 外部ツールは非同期 (別コンソール)。結果はファイル監視が拾って
        // 一覧が減る = ここでは待たない
        host.openMergeTool();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (allResolved) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Success);
        ImGui::TextWrapped("%s", Tr(StrId::Scm_ConflictAllResolved));
        ImGui::PopStyleColor();
    } else {
        ImGui::Text(Tr(StrId::Scm_ConflictCount), static_cast<int>(rows.size()));
    }
    if (!list.merged.empty()) {
        // 競合しなかった分は**もう適用されている** (GitTransaction が EndBatch で流す)。
        // 件数だけ出しておかないと「pull したのに何も来ていない」ように見える
        ImGui::TextDisabled(Tr(StrId::Scm_ConflictMerged), static_cast<int>(list.merged.size()));
    }

    if (ImGui::BeginChild("###ScmConflictList", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (const ConflictRow& row : rows) {
            ImGui::PushID(row.path.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
            ImGui::TextUnformatted("!");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted(row.path.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(row.path.c_str());
                for (const std::string& p : row.paths) {
                    // 対で解決する = 実際に git へ渡すパスを見せる
                    ImGui::TextDisabled("%s", p.c_str());
                }
                ImGui::EndTooltip();
            }
            // ours / theirs は**どちらも常に押せる**。片側の版が無い競合
            // (modify/delete) では「その選択でファイルが消える」という意味になるので、
            // 無効化せずにツールチップで伝える
            if (ImGui::SmallButton(Tr(StrId::Scm_TakeOurs))) {
                scm.Resolve(row.paths, collabop::kSideOurs, {});
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(row.ours ? StrId::Scm_TakeOursHint
                                                    : StrId::Scm_TakeDeletesHint));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(StrId::Scm_TakeTheirs))) {
                scm.Resolve(row.paths, collabop::kSideTheirs, {});
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(row.theirs ? StrId::Scm_TakeTheirsHint
                                                      : StrId::Scm_TakeDeletesHint));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Tr(ConflictKindText(row.kind)));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void SourceControlWindow::DrawDiffWindow(SourceControlSession& scm)
{
    if (!diffOpen) {
        return;
    }
    // 浮いた状態で最初に開いたときの大きさ (差分は横に長い)
    ImGui::SetNextWindowSize(ImVec2(640.0f, 320.0f), ImGuiCond_FirstUseEver);
    if (diffFocusRequest_) {
        diffFocusRequest_ = false;
        ImGui::SetNextWindowFocus(); // ドック束の中で手前のタブにする
    }
    if (!ImGui::Begin(Tr(StrId::Win_Diff), &diffOpen)) {
        ImGui::End();
        return;
    }
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
    ImGui::Separator();
    // ★横スクロールは**差分本文があるときだけ**有効にする。案内文まで横スクロール
    //   領域に入れると、窓が細いときに 1 行が右へ流れて読めなくなる
    //   (M66c のプローブで "No textual diff (new, binary or unchanged on thi" と切れた)
    const bool hasDiffText = selected_.size() == 1 && diff.valid && !diff.loading
        && !diff.text.empty();
    if (ImGui::BeginChild("###ScmDiffView", ImVec2(0, 0), ImGuiChildFlags_None,
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
    ImGui::End();
}

void SourceControlWindow::DrawBranches(SourceControlSession& scm, const SourceControlHost& host)
{
    if (!branchesRequested_) {
        // タブを開いた最初のフレームで 1 回だけ (窓を開いているだけで
        // for-each-ref が走り続けるのは無駄)
        branchesRequested_ = true;
        scm.RequestBranches();
    }
    const BranchList& branches = scm.Branches();
    if (!branches.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_Loading));
        return;
    }

    // ---- 操作列 ----
    // ★既定のドック幅 (左列 ≒ 285px) に収まるのはボタン 3 個まで。
    //   ここは 2 個 + 状態テキストに留める
    const bool busy = scm.WriteInFlight();
    const bool gateOpen = host.writeBlockers.empty() && host.requestCheckout;
    const bool pickedOther = !selectedBranch_.empty() && selectedBranch_ != branches.current;
    ImGui::BeginDisabled(!pickedOther || busy || !gateOpen);
    if (ImGui::Button(Tr(StrId::Scm_Switch))) {
        // ★ここでは**要求するだけ**。事前判定 (diff_names) → 確認 → checkout →
        //   後処理は GitTransaction が持つ (窓が working tree を触らない)
        host.requestCheckout(selectedBranch_);
    }
    ImGui::EndDisabled();
    // ★BeginDisabled の外でツールチップを出す (中だとホバー判定ごと殺される)
    if (!gateOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        DrawBlockerTooltip(host.writeBlockers);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(Tr(StrId::Scm_NewBranch))) {
        newBranchName_[0] = '\0';
        newBranchOpen_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (busy) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_Busy));
    } else if (!pickedOther) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_BranchPick));
    }

    // ---- 作成モーダル ----
    if (newBranchOpen_) {
        ImGui::OpenPopup(Tr(StrId::Scm_NewBranchTitle));
    }
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        // ★Appearing ではなく Always。AlwaysAutoResize の窓は出た最初のフレームに
        //   自分の大きさを知らないので、Appearing だとずれた位置で確定する (M66d の nit)
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                       vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Scm_NewBranchTitle), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled(Tr(StrId::Scm_NewBranchFrom),
                            branches.current.empty() ? Tr(StrId::Scm_Detached)
                                                     : branches.current.c_str());
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText(Tr(StrId::Scm_NewBranchName), newBranchName_, sizeof(newBranchName_));
        ImGui::Spacing();
        const bool nameOk = HasVisibleText(newBranchName_);
        ImGui::BeginDisabled(!nameOk || busy);
        if (ImGui::Button(Tr(StrId::Scm_NewBranchCreate), ImVec2(120, 0))) {
            const std::string name = newBranchName_;
            // from は空 = 現在の HEAD (サービス側の既定)
            scm.CreateBranch(name, {},
                             [this, name](bool ok, const std::string&, const std::string&) {
                                 if (ok) {
                                     createdBranch_ = name;
                                     selectedBranch_ = name;
                                 }
                             });
            newBranchOpen_ = false;
            // ★閉じるのを明示する。開いたままにすると次に開く modal が
            //   その上に積まれ、位置も入力も前の modal に引きずられる
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(110, 0))) {
            newBranchOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    if (branches.locals.empty() && branches.remotes.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Scm_BranchNone));
        return;
    }
    // ---- 一覧 (ローカル → リモート) ----
    auto drawRows = [this, &branches](const std::vector<BranchInfo>& rows) {
        for (const BranchInfo& b : rows) {
            const bool isCurrent = b.name == branches.current;
            ImGui::PushID(b.name.c_str());
            if (isCurrent) {
                // 現在のブランチだけ意味色で強調 (themeColor のみ = テーマ切替に追随)
                ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Accent);
            }
            if (ImGui::Selectable(b.name.c_str(), selectedBranch_ == b.name)) {
                selectedBranch_ = b.name;
            }
            if (isCurrent) {
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", Tr(StrId::Scm_BranchCurrent));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(b.name.c_str());
                if (!b.upstream.empty()) {
                    ImGui::TextDisabled(Tr(StrId::Scm_BranchTracking), b.upstream.c_str());
                }
                // 短縮 SHA は表示専用 (7 桁)。UI では長い方に意味が無い
                ImGui::TextDisabled("%s", ShortSha(b.oid).c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    };
    if (ImGui::BeginChild("###ScmBranchList", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        ImGui::SeparatorText(Tr(StrId::Scm_BranchLocal));
        drawRows(branches.locals);
        if (!branches.remotes.empty()) {
            // ★リモート追跡は checkout の扱いが違う (-t で追跡ブランチを作ってから乗る)
            //   ので節を分ける。混ぜると「同じ名前が 2 つある」ように見える
            ImGui::SeparatorText(Tr(StrId::Scm_BranchRemote));
            drawRows(branches.remotes);
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
        // 行を選んだら差分の窓を開く (spec §4.3「選択時に開く」)。閉じたあとは
        // 次に選び直すまで開かない = 邪魔にならない。
        // ★**手前に出すところまでやる**。開くだけだと、同じドック束の別タブ
        //   (既定では Assets) の後ろに隠れたままで、押しても何も起きないように見える
        //   (M66e のプローブで実際にそうなった)
        diffOpen = true;
        diffFocusRequest_ = true;
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
