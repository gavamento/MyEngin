//====================================================================================
//                          ProjectSettingsFile.cpp
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          project_settings.json の共有の読み書き
//====================================================================================
#include "Engine/Engine/App/ProjectSettingsFile.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

using json = nlohmann::json;

// 中身の文字列ごと読む。text は改行コードの検出に使う
ProjectSettingsRead ReadWithText(const std::wstring& path, json& out, std::string& text)
{
    out = json::object();
    text.clear();
    const std::filesystem::path file(path);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        // 存在を確かめられなかった場合は「無い」と決めつけない (上書きの入口になる)
        return ec ? ProjectSettingsRead::Corrupt : ProjectSettingsRead::Missing;
    }
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        return ProjectSettingsRead::Corrupt;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return ProjectSettingsRead::Ok; // 失うキーが無いので空オブジェクトとして扱う
    }
    json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return ProjectSettingsRead::Corrupt;
    }
    out = std::move(parsed);
    return ProjectSettingsRead::Ok;
}

} // namespace

std::wstring ProjectSettingsPath(const std::wstring& assetsRoot)
{
    return assetsRoot + L"\\project_settings.json";
}

ProjectSettingsRead ReadProjectSettingsFile(const std::wstring& path, json& out)
{
    std::string text;
    return ReadWithText(path, out, text);
}

bool UpdateProjectSettingsFile(const std::wstring& path, const std::function<void(json&)>& mutate)
{
    json root;
    std::string oldText;
    if (ReadWithText(path, root, oldText) == ProjectSettingsRead::Corrupt) {
        MYE_LOG_ERROR("project_settings.json is unreadable or not a JSON object - not saved, so its other keys "
                      "are kept. Fix or restore the file first: %s",
                      WideToUtf8(path).c_str());
        return false;
    }
    mutate(root);
    std::string text = root.dump(2) + "\n";
    if (oldText.find("\r\n") != std::string::npos) {
        // dump は文字列中の改行をエスケープするので、生の \n は行末だけ
        std::string crlf;
        crlf.reserve(text.size() + text.size() / 16);
        for (const char c : text) {
            if (c == '\n') {
                crlf += '\r';
            }
            crlf += c;
        }
        text = std::move(crlf);
    }
    if (!WriteFileReplacing(path, text)) {
        MYE_LOG_ERROR("project_settings.json could not be written: %s", WideToUtf8(path).c_str());
        return false;
    }
    return true;
}

} // namespace mye
