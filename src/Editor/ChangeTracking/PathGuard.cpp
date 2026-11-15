//====================================================================================
//                          PathGuard.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の書き先の検査 (履歴のパスがプロジェクトの外・保護領域を指さないか)
//====================================================================================
#include "Editor/ChangeTracking/PathGuard.h"

#include <cwctype>
#include <vector>

#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

constexpr wchar_t kSep = 0x5C; // バックスラッシュ

std::string AsciiLower(const std::string& text)
{
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::vector<std::string> SplitSlash(const std::string& rel)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t end = rel.find('/', start);
        parts.push_back(rel.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            return parts;
        }
        start = end + 1;
    }
}

// Win32 が拡張子の前までを見て予約名とみなす名前 (CON、NUL.txt、com1.log、"NUL .txt" も)
bool IsReservedDeviceName(const std::string& component)
{
    std::string base = AsciiLower(component.substr(0, component.find('.')));
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" || base == "conout$") {
        return true;
    }
    return base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) && base[3] >= '1' && base[3] <= '9';
}

// 8.3 の短い名前の形 (".git" の別名は "GIT~1")。実在の長い名前と区別できないので、形が合えば保護領域の疑いとして拒否する
bool LooksLikeShortName(const std::string& component)
{
    const size_t tilde = component.find('~');
    if (tilde == std::string::npos || tilde == 0 || tilde > 6) {
        return false;
    }
    size_t i = tilde + 1;
    size_t digits = 0;
    while (i < component.size() && component[i] >= '0' && component[i] <= '9') {
        ++i;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    if (i == component.size()) {
        return true;
    }
    return component[i] == '.' && component.size() - i - 1 <= 3 && component.find('.', i + 1) == std::string::npos;
}

bool IsProtectedName(const std::string& component)
{
    const std::string lower = AsciiLower(component);
    return lower == ".git" || lower == ".mye" || LooksLikeShortName(lower);
}

std::wstring TrimTrailingSeparators(std::wstring root)
{
    while (!root.empty() && (root.back() == kSep || root.back() == L'/')) {
        root.pop_back();
    }
    return root;
}

} // namespace

PathIssue CheckRelativePath(const std::string& rel)
{
    if (rel.empty()) {
        return PathIssue::Empty;
    }
    if (rel[0] == '/' || rel[0] == '\\') {
        return PathIssue::Absolute; // 絶対パスと UNC
    }
    if (rel.size() >= 2 && rel[1] == ':' && ((rel[0] >= 'A' && rel[0] <= 'Z') || (rel[0] >= 'a' && rel[0] <= 'z'))) {
        return PathIssue::Absolute; // ドライブ名
    }
    for (const char c : rel) {
        if (c == '\\') {
            return PathIssue::Separator;
        }
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            return PathIssue::InvalidChar;
        }
    }
    for (const std::string& component : SplitSlash(rel)) {
        if (component.empty()) {
            return PathIssue::Separator;
        }
        if (component == "." || component == "..") {
            return PathIssue::Traversal;
        }
        if (component.find(':') != std::string::npos) {
            return PathIssue::Stream;
        }
        if (component.find_first_of("<>\"|?*") != std::string::npos) {
            return PathIssue::InvalidChar;
        }
        if (component.back() == ' ' || component.back() == '.') {
            return PathIssue::TrailingDotOrSpace;
        }
        if (IsReservedDeviceName(component)) {
            return PathIssue::ReservedDevice;
        }
        if (IsProtectedName(component)) {
            return PathIssue::ProtectedDirectory;
        }
    }
    return PathIssue::None;
}

std::wstring JoinProjectPath(const std::wstring& projectRoot, const std::string& rel)
{
    std::wstring joined = TrimTrailingSeparators(projectRoot);
    std::wstring tail = Utf8ToWide(rel);
    for (wchar_t& c : tail) {
        if (c == L'/') {
            c = kSep;
        }
    }
    joined.push_back(kSep);
    joined += tail;
    return joined;
}

PathIssue CheckPathOnDisk(const std::wstring& projectRoot, const std::string& rel, RestoreFileIo& io)
{
    const std::wstring root = TrimTrailingSeparators(projectRoot);
    const uint32_t rootAttrs = io.Attributes(root);
    if (root.empty() || rootAttrs == kRestoreAttrAbsent || (rootAttrs & kRestoreAttrDirectory) == 0) {
        return PathIssue::OutsideRoot;
    }
    const std::vector<std::string> parts = SplitSlash(rel);
    std::wstring current = root;
    for (size_t i = 0; i < parts.size(); ++i) {
        current.push_back(kSep);
        current += Utf8ToWide(parts[i]);
        const uint32_t attrs = io.Attributes(current);
        if (attrs == kRestoreAttrAbsent) {
            return PathIssue::None; // ここから先はこれから作る場所
        }
        if ((attrs & kRestoreAttrReparse) != 0) {
            return PathIssue::Link;
        }
        if (i + 1 < parts.size() && (attrs & kRestoreAttrDirectory) == 0) {
            return PathIssue::ParentNotDirectory;
        }
    }
    return PathIssue::None;
}

PathIssue CheckRestorePath(const std::wstring& projectRoot, const std::string& rel, RestoreFileIo& io)
{
    const PathIssue lexical = CheckRelativePath(rel);
    return lexical != PathIssue::None ? lexical : CheckPathOnDisk(projectRoot, rel, io);
}

std::string RelativeToProject(const std::wstring& projectRoot, const std::wstring& absPath)
{
    const std::wstring root = TrimTrailingSeparators(projectRoot);
    if (root.empty() || absPath.size() <= root.size() + 1) {
        return std::string();
    }
    for (size_t i = 0; i < root.size(); ++i) {
        const wchar_t a = root[i] == L'/' ? kSep : root[i];
        const wchar_t b = absPath[i] == L'/' ? kSep : absPath[i];
        if (std::towlower(a) != std::towlower(b)) {
            return std::string();
        }
    }
    if (absPath[root.size()] != kSep && absPath[root.size()] != L'/') {
        return std::string();
    }
    std::wstring rest = absPath.substr(root.size() + 1);
    for (wchar_t& c : rest) {
        if (c == kSep) {
            c = L'/';
        }
    }
    return WideToUtf8(rest);
}

std::string RestorePathKey(const std::string& rel)
{
    std::wstring wide = Utf8ToWide(rel);
    for (wchar_t& c : wide) {
        c = c == kSep ? L'/' : static_cast<wchar_t>(std::towlower(c));
    }
    return WideToUtf8(wide);
}

} // namespace mye
