#include "Editor/DocumentDirty.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace mye {

namespace {

// 改行を LF へ揃え、末尾の空白 / 改行を落とす
std::string Normalize(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c != '\r') {
            out.push_back(c);
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

} // namespace

bool TextDiffersFromDisk(const std::wstring& path, const std::string& inMemory)
{
    if (path.empty()) {
        return false; // 書き出し先が決まっていない = まだ「保存すべきもの」が無い
    }
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        // ファイルが無い = 一度も保存していない。中身があるなら未保存の変更そのもの
        return !Normalize(inMemory).empty();
    }
    const std::string onDisk((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    return Normalize(onDisk) != Normalize(inMemory);
}

} // namespace mye
