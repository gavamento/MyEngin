#include "Editor/LayoutManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"

namespace fs = std::filesystem;

namespace mye {

void LayoutManager::Init(const std::wstring& layoutsDir, std::vector<PanelBinding> panels)
{
    dir_ = layoutsDir;
    panels_ = std::move(panels);
}

std::wstring LayoutManager::IniPath(const std::string& name) const
{
    return dir_ + L"\\" + Utf8ToWide(name) + L".ini";
}

std::wstring LayoutManager::PanelsPath(const std::string& name) const
{
    return dir_ + L"\\" + Utf8ToWide(name) + L".panels.json";
}

std::vector<std::string> LayoutManager::ListLayouts() const
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension().wstring() == L".ini") {
            names.push_back(WideToUtf8(entry.path().stem().wstring()));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

void LayoutManager::SaveCurrent(const std::string& name)
{
    std::error_code ec;
    fs::create_directories(dir_, ec);
    // ImGui のドック/ウィンドウ設定 (UTF-8 パスは ImFileOpen が wide へ変換する)
    ImGui::SaveIniSettingsToDisk(WideToUtf8(IniPath(name)).c_str());
    // パネル開閉フラグの sidecar
    nlohmann::json panels;
    for (const PanelBinding& b : panels_) {
        panels[b.key] = *b.flag;
    }
    nlohmann::json j;
    j["formatVersion"] = 1;
    j["panels"] = std::move(panels);
    std::ofstream f{ fs::path(PanelsPath(name)), std::ios::binary };
    if (f) {
        const std::string text = j.dump(2);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    MYE_LOG_INFO("layout saved: %s", name.c_str());
}

void LayoutManager::ApplyPendingLoad()
{
    if (pendingLoad_.empty()) {
        return;
    }
    const std::string name = pendingLoad_;
    pendingLoad_.clear();
    std::error_code ec;
    const std::wstring ini = IniPath(name);
    if (!fs::exists(ini, ec)) {
        MYE_LOG_WARN("layout not found: %s", name.c_str());
        return;
    }
    ImGui::LoadIniSettingsFromDisk(WideToUtf8(ini).c_str());
    // パネル開閉の復元 (sidecar 不在なら現状維持。ini のロード自体は成立している)
    std::ifstream f{ fs::path(PanelsPath(name)), std::ios::binary };
    if (f) {
        try {
            nlohmann::json j;
            f >> j;
            const nlohmann::json panels = j.value("panels", nlohmann::json::object());
            for (const PanelBinding& b : panels_) {
                *b.flag = panels.value(b.key, *b.flag);
            }
        } catch (const nlohmann::json::exception& ex) {
            MYE_LOG_WARN("layout panels.json parse error: %s", ex.what());
        }
    }
    MYE_LOG_INFO("layout loaded: %s", name.c_str());
}

bool LayoutManager::DrawToolbarUi()
{
    bool resetRequested = false;
    if (ImGui::Button(ICON_FA_TABLE_COLUMNS)) {
        ImGui::OpenPopup("##layouts");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("レイアウト (保存/切替)");
    }
    if (ImGui::BeginPopup("##layouts")) {
        // ファイル列挙はポップアップ表示中のみ (毎フレームのディレクトリ走査を避ける)
        const std::vector<std::string> names = ListLayouts();
        for (const std::string& n : names) {
            ImGui::PushID(n.c_str());
            if (ImGui::MenuItem(n.c_str())) {
                pendingLoad_ = n; // 適用は次フレーム冒頭 (ApplyPendingLoad)
            }
            ImGui::PopID();
        }
        if (!names.empty()) {
            ImGui::Separator();
        }
        if (ImGui::MenuItem("レイアウトを保存...")) {
            openSaveModal_ = true;
        }
        if (!names.empty() && ImGui::BeginMenu("削除")) {
            for (const std::string& n : names) {
                ImGui::PushID(n.c_str());
                if (ImGui::MenuItem(n.c_str())) {
                    std::error_code ec;
                    fs::remove(IniPath(n), ec);
                    fs::remove(PanelsPath(n), ec);
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("リセット (既定)")) {
            resetRequested = true;
        }
        ImGui::EndPopup();
    }

    // ---- 保存モーダル (命名) ----
    if (openSaveModal_) {
        openSaveModal_ = false;
        ImGui::OpenPopup(Tr(StrId::Popup_SaveLayout));
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_SaveLayout), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("同名レイアウトは上書きされます");
        ImGui::SetNextItemWidth(240.0f);
        const bool enter = ImGui::InputText("名前", nameBuf_, sizeof(nameBuf_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        std::string name = nameBuf_;
        // 末尾の空白/ドットは Windows のファイル名として不正なので落とす
        while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
            name.pop_back();
        }
        // 拒否文字は RenameAsset と同じ集合
        const bool invalid =
            name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos;
        const bool doSave = ImGui::Button("保存", ImVec2(90, 0)) || enter;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("キャンセル", ImVec2(90, 0));
        if (doSave && !invalid) {
            SaveCurrent(name);
            ImGui::CloseCurrentPopup();
        } else if (doSave) {
            MYE_LOG_WARN("layout: invalid name: %s", nameBuf_);
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return resetRequested;
}

} // namespace mye
