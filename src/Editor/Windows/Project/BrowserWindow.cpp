//====================================================================================
//                          BrowserWindow.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ブラウザ窓の実装 (パネル描画・見せる条件の収集・設定)
//====================================================================================
#include "Editor/Windows/Project/BrowserWindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "imgui.h"
#include "imgui_internal.h" // 見せる条件の収集 (g.MovingWindow / g.Windows / モーダル) だけ。ここに閉じる

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

int RoundToInt(float v) { return static_cast<int>(std::lround(v)); }

// ImGui の画面座標 (ビューポート 1 枚 = メイン窓のクライアント座標) の矩形を px の BrowserRect へ
BrowserRect ToClientRect(const ImVec2& min, const ImVec2& max)
{
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    return BrowserRect{ RoundToInt(min.x - origin.x), RoundToInt(min.y - origin.y), RoundToInt(max.x - origin.x),
                        RoundToInt(max.y - origin.y) };
}

// ImGui の内部状態から見せる条件 (4.1.5 の 4 / 5 / 6) を集める。判定そのものは EvaluateBrowserVisibility (純関数)。
// パネルより表示順で後ろ (= 手前に描かれる) の窓のうち、子窓 (パネル自身の子を含む) とツールチップ、
// 選ばれていないドックのタブ・ignoredIds (通知) は重なりの対象にしない。ポップアップ・メニュー・コンボは対象
void CollectImGuiAirspace(BrowserVisibilityInput& out, const std::vector<unsigned int>& ignoredIds)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    out.windowDragging = g.MovingWindow != nullptr;
    out.modalOpen = ImGui::GetTopMostAndVisiblePopupModal() != nullptr;

    const ImGuiWindow* panel = ImGui::FindWindowByName(Tr(StrId::Win_Browser));
    if (panel == nullptr) {
        return;
    }
    const ImGuiWindow* panelHost = panel->DockNode != nullptr ? panel->DockNode->HostWindow : nullptr;
    bool afterPanel = false;
    for (const ImGuiWindow* w : g.Windows) {
        if (w == panel) {
            afterPanel = true;
            continue;
        }
        if (!afterPanel || w == panelHost) {
            continue;
        }
        if (!w->Active || w->Hidden) {
            continue;
        }
        const bool isPopup = (w->Flags & ImGuiWindowFlags_Popup) != 0;
        if ((w->Flags & ImGuiWindowFlags_Tooltip) != 0 || ((w->Flags & ImGuiWindowFlags_ChildWindow) != 0 && !isPopup)) {
            continue;
        }
        if (std::find(ignoredIds.begin(), ignoredIds.end(), w->ID) != ignoredIds.end()) {
            continue; // 通知など、入力を取らない一時表示 (重なった部分はブラウザの下になってよい)
        }
        if (w->DockIsActive && !w->DockTabIsVisible) {
            continue; // 選ばれていないタブ
        }
        out.overlays.push_back(ToClientRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y)));
    }
}

} // namespace

void BrowserWindow::ApplySettings(const BrowserWindowSettings& settings)
{
    selectedId_ = settings.id;
    customExe_ = settings.customExe;
    std::snprintf(urlBuf_, sizeof(urlBuf_), "%s", settings.startUrl.c_str());
    settingsDirty_ = false;
}

bool BrowserWindow::TakeSettingsChanged(BrowserWindowSettings& out)
{
    if (!settingsDirty_) {
        return false;
    }
    settingsDirty_ = false;
    out.id = selectedId_;
    out.customExe = customExe_;
    out.startUrl = urlBuf_;
    return true;
}

void BrowserWindow::Redetect()
{
    candidates_ = DetectInstalledBrowsers();
    detected_ = true;
}

size_t BrowserWindow::ResolveSelectedIndex() const
{
    if (selectedId_ == kBrowserCustomId) {
        return candidates_.size();
    }
    const std::wstring wide = Utf8ToWide(selectedId_);
    for (size_t i = 0; i < candidates_.size(); ++i) {
        if (!selectedId_.empty() && candidates_[i].id == wide) {
            return i;
        }
    }
    // 未選択・見つからない (アンインストールした) ときは、既定の印のもの → 先頭 → 「その他」
    for (size_t i = 0; i < candidates_.size(); ++i) {
        if (candidates_[i].isDefault) {
            return i;
        }
    }
    return 0 < candidates_.size() ? 0 : candidates_.size();
}

bool BrowserWindow::ResolveLaunchTarget(std::wstring& exe, BrowserFamily& family, std::string& name) const
{
    const size_t index = ResolveSelectedIndex();
    if (index < candidates_.size()) {
        exe = candidates_[index].exePath;
        family = candidates_[index].family;
        name = WideToUtf8(candidates_[index].displayName);
    } else {
        exe = Utf8ToWide(customExe_);
        family = ClassifyBrowserFamily(exe);
        name = WideToUtf8(std::filesystem::path(exe).filename().wstring());
    }
    return !exe.empty();
}

bool BrowserWindow::StartLaunch()
{
    if (!detected_) {
        Redetect();
    }
    std::wstring exe;
    BrowserFamily family = BrowserFamily::Unknown;
    std::string name;
    if (!ResolveLaunchTarget(exe, family, name)) {
        return false;
    }
    const int64_t now = BrowserNowMs();
    if (!host_.Launch(exe, BuildBrowserLaunchArgs(family, Utf8ToWide(urlBuf_)), now)) {
        return false;
    }
    embeddedName_ = name;
    launchStartMs_ = now;
    return true;
}

bool BrowserWindow::SelectBrowserById(const std::wstring& id)
{
    if (!detected_) {
        Redetect();
    }
    for (const BrowserCandidate& c : candidates_) {
        if (c.id == id) {
            selectedId_ = WideToUtf8(id);
            return true;
        }
    }
    return false;
}

void BrowserWindow::SetStartUrl(const std::wstring& url)
{
    std::snprintf(urlBuf_, sizeof(urlBuf_), "%s", WideToUtf8(url).c_str());
}

void BrowserWindow::Shutdown()
{
    if (host_.State() == BrowserState::Embedded) {
        host_.ReturnFocusToMain();
        host_.Detach();
    }
}

void BrowserWindow::OnImGui(const BrowserWindowHost& host)
{
    if (open && !wasOpen_) {
        Redetect(); // パネルを開いたとき (毎フレームは読まない)
    }
    wasOpen_ = open;
    if (!open) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 80.0f, viewport->WorkPos.y + 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(880.0f, 620.0f), ImGuiCond_FirstUseEver);
    // 内容はスクロールさせない (子窓が埋めるので、スクロールバーがあると矩形が毎フレーム揺れる)
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin(Tr(StrId::Win_Browser), &open, flags)) {
        ImGui::End();
        return;
    }
    panelDrawn_ = true;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    contentRect_ = ToClientRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y));
    switch (host_.State()) {
    case BrowserState::Embedded:
        DrawEmbedded();
        break;
    case BrowserState::Launching:
        DrawLaunching();
        break;
    default:
        DrawIdle(host);
        break;
    }
    ImGui::End();
}

void BrowserWindow::DrawIdle(const BrowserWindowHost& host)
{
    const size_t selected = ResolveSelectedIndex();
    const bool isCustom = selected >= candidates_.size();

    // ブラウザの選択 + 再検出
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr(StrId::Browser_Combo));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    std::string preview;
    if (isCustom) {
        preview = Tr(StrId::Browser_Other);
    } else {
        preview = WideToUtf8(candidates_[selected].displayName);
        if (candidates_[selected].isDefault) {
            preview += std::string(" ") + Tr(StrId::Browser_DefaultMark);
        }
    }
    if (ImGui::BeginCombo("##browserCombo", preview.c_str())) {
        for (size_t i = 0; i < candidates_.size(); ++i) {
            std::string label = WideToUtf8(candidates_[i].displayName);
            if (candidates_[i].isDefault) {
                label += std::string(" ") + Tr(StrId::Browser_DefaultMark);
            }
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), i == selected)) {
                selectedId_ = WideToUtf8(candidates_[i].id);
                MarkSettingsDirty();
            }
            ImGui::PopID();
        }
        if (ImGui::Selectable(Tr(StrId::Browser_Other), isCustom)) {
            selectedId_ = kBrowserCustomId;
            MarkSettingsDirty();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Browser_Redetect))) {
        Redetect();
    }
    if (candidates_.empty()) {
        ImGui::TextWrapped("%s", Tr(StrId::Browser_NoneFound));
    }

    // その他: exe パスの表示 + 参照
    if (isCustom) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Tr(StrId::Browser_CustomExe));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", customExe_.empty() ? Tr(StrId::Browser_NoExe) : customExe_.c_str());
        if (ImGui::Button(Tr(StrId::Browser_Browse)) && host.pickExe) {
            const std::wstring picked = host.pickExe();
            if (!picked.empty()) {
                customExe_ = WideToUtf8(picked);
                MarkSettingsDirty();
            }
        }
    }

    // 開始 URL (編集が終わったときだけ保存対象にする。1 文字ごとに書かない)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr(StrId::Browser_StartUrl));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 28.0f);
    ImGui::InputTextWithHint("##browserUrl", Tr(StrId::Browser_StartUrlHint), urlBuf_, sizeof(urlBuf_));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        MarkSettingsDirty();
    }

    // 起動 (Closed / Lost / TimedOut / Failed からも押せる)
    const bool canLaunch = !isCustom || !customExe_.empty();
    ImGui::BeginDisabled(!canLaunch);
    if (ImGui::Button(Tr(StrId::Browser_Launch))) {
        StartLaunch();
    }
    ImGui::EndDisabled();

    // 直前の結果 (詳しい文言と取り込みの導線は後続)
    switch (host_.State()) {
    case BrowserState::Closed:
        ImGui::TextWrapped("%s", Tr(StrId::Browser_ResultClosed));
        break;
    case BrowserState::Lost:
        ImGui::TextWrapped("%s", Tr(StrId::Browser_ResultLost));
        break;
    case BrowserState::TimedOut:
        ImGui::TextWrapped("%s", Tr(StrId::Browser_ResultTimedOut));
        break;
    case BrowserState::Failed:
        ImGui::TextWrapped("%s %s", Tr(StrId::Browser_ResultFailed), host_.Message().c_str());
        break;
    default:
        break;
    }
}

void BrowserWindow::DrawLaunching()
{
    const int64_t elapsedMs = BrowserNowMs() - launchStartMs_;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s %d %s", Tr(StrId::Browser_Launching), static_cast<int>(elapsedMs / 1000), Tr(StrId::Browser_SecondsSuffix));
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Browser_Cancel))) {
        host_.CancelLaunch();
    }
}

void BrowserWindow::DrawEmbedded()
{
    // 1 行のツールバー: ブラウザ名 + 切り離す / 閉じる
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(embeddedName_.c_str());
    const ImGuiStyle& style = ImGui::GetStyle();
    const float detachW = ImGui::CalcTextSize(Tr(StrId::Browser_Detach)).x + style.FramePadding.x * 2.0f;
    const float closeW = ImGui::CalcTextSize(Tr(StrId::Browser_Close)).x + style.FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - detachW - closeW - style.ItemSpacing.x);
    if (ImGui::Button(Tr(StrId::Browser_Detach))) {
        host_.ReturnFocusToMain();
        host_.Detach();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Browser_Close))) {
        host_.ReturnFocusToMain();
        host_.RequestClose();
    }

    // ツールバーの下が子窓の内容矩形
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    contentRect_ = ToClientRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y));
    DrawHiddenPlaceholder();
    ImGui::Dummy(avail);
}

// 子窓が隠れている間 (前フレームの判定) に、内容矩形へ枠と 1 行を描く。見えているときは子窓が覆うので描いても害はない
void BrowserWindow::DrawHiddenPlaceholder()
{
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRect(min, ImVec2(min.x + size.x, min.y + size.y), ImGui::GetColorU32(ImGuiCol_Border));
    if (lastHide_ == BrowserHideReason::None || lastHide_ == BrowserHideReason::PanelNotDrawn) {
        return;
    }
    const char* text = Tr(StrId::Browser_HiddenByOverlap);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    list->AddText(ImVec2(min.x + (std::max)(8.0f, (size.x - textSize.x) * 0.5f), min.y + (std::max)(8.0f, (size.y - textSize.y) * 0.5f)),
                  ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
}

void BrowserWindow::ApplyToHost(bool mainMinimized)
{
    // × やメニューで閉じられた: ブラウザは閉じず、普通の窓へ戻す
    if (!open && host_.State() == BrowserState::Embedded) {
        host_.ReturnFocusToMain();
        host_.Detach();
    }
    BrowserVisibilityInput in;
    in.panelDrawn = panelDrawn_;
    in.panel = contentRect_;
    in.mainMinimized = mainMinimized;
    if (panelDrawn_) {
        CollectImGuiAirspace(in, ignoredIds_);
    }
    lastHide_ = EvaluateBrowserVisibility(in);

    BrowserFrameInput frame;
    frame.nowMs = BrowserNowMs();
    frame.show = lastHide_ == BrowserHideReason::None;
    frame.contentRect = contentRect_;
    host_.Update(frame);
    panelDrawn_ = false;
}

} // namespace mye
