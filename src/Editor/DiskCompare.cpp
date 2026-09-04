#include "Editor/DiskCompare.h"

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

bool TextDiffersFromDisk(const std::wstring& path, const std::string& inMemory,
                         const std::string& whenMissing)
{
    if (path.empty()) {
        return false; // 書き出し先が決まっていない = まだ「保存すべきもの」が無い
    }
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        // ファイルが無い = 一度も保存していない。呼び手が渡した「読み直した既定状態」と
        // 比べる (2 引数版は空文字列 = 「中身があるなら未保存」の従来どおり)
        return Normalize(inMemory) != Normalize(whenMissing);
    }
    const std::string onDisk((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    return Normalize(onDisk) != Normalize(inMemory);
}

bool TextDiffersFromDisk(const std::wstring& path, const std::string& inMemory)
{
    return TextDiffersFromDisk(path, inMemory, std::string());
}

} // namespace mye
