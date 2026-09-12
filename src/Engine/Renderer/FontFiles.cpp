//====================================================================================
//                          FontFiles.cpp
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          プロジェクトフォントの選択規則の実装
//====================================================================================
#include "Engine/Renderer/FontFiles.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace mye {
namespace fontfiles {

std::vector<std::wstring> ListProjectFontFiles(const std::wstring& assetsRoot)
{
    std::vector<std::wstring> found;
    if (assetsRoot.empty()) {
        return found;
    }
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(assetsRoot) / L"fonts";
    if (!std::filesystem::is_directory(dir, ec)) {
        return found;
    }
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        std::wstring ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".ttf" || ext == L".ttc") {
            found.push_back(e.path().wstring());
        }
    }
    // directory_iterator の列挙順はファイルシステム任せなので、明示キー (フルパス) で整列する
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace fontfiles
} // namespace mye
