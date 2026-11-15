//====================================================================================
//                          HunkRestore.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          Hunk の逆適用 (今のファイルの中で探して Before の本文へ戻す。純関数)
//====================================================================================
#include "Editor/ChangeTracking/HunkRestore.h"

#include <algorithm>

namespace mye {

namespace {

using Lines = std::vector<std::string_view>;

bool EqualAt(const Lines& file, size_t at, const Lines& pattern)
{
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (file[at + i] != pattern[i]) {
            return false;
        }
    }
    return true;
}

struct Located {
    int count = 0; // 0 / 1 / 2 (= 2 か所以上)
    size_t first = 0;
    bool exact = false; // 記録した位置で一致した
};

// pattern を file の中で探す。記録した位置 (recorded、負 = 無し) で一致すればそれを採り、他は探さない
Located Locate(const Lines& file, const Lines& pattern, int64_t recorded, bool allowShift)
{
    Located out;
    if (pattern.empty() || pattern.size() > file.size()) {
        return out;
    }
    if (recorded >= 0 && static_cast<size_t>(recorded) + pattern.size() <= file.size() && EqualAt(file, static_cast<size_t>(recorded), pattern)) {
        out.count = 1;
        out.first = static_cast<size_t>(recorded);
        out.exact = true;
        return out;
    }
    if (!allowShift) {
        return out;
    }
    const size_t last = file.size() - pattern.size();
    for (size_t p = 0; p <= last; ++p) {
        if (file[p] != pattern[0] || !EqualAt(file, p, pattern)) {
            continue;
        }
        if (out.count == 0) {
            out.first = p;
        }
        if (++out.count >= 2) {
            break;
        }
    }
    return out;
}

Lines Concat(const Lines& a, const Lines& b, const Lines& c)
{
    Lines out;
    out.reserve(a.size() + b.size() + c.size());
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    out.insert(out.end(), c.begin(), c.end());
    return out;
}

struct Match {
    size_t bodyStart = 0;
    size_t bodyCount = 0;
    size_t index = 0;
};

// 1 つの Hunk を探す。matchOut は Match のときだけ埋める
HunkFindResult FindOne(const Lines& file, const HunkSpec& hunk, const HunkApplyOptions& options, Match* matchOut)
{
    const Lines pre = SplitLinesKeepEol(hunk.contextPre);
    const Lines post = SplitLinesKeepEol(hunk.contextPost);
    const Lines after = SplitLinesKeepEol(hunk.afterText);
    const Lines before = SplitLinesKeepEol(hunk.beforeText);
    // 文脈が 0 行の Hunk (ファイル全体の置き換えなど) は、記録した位置でだけ一致とみなす
    const bool allowShift = options.allowShift && !(pre.empty() && post.empty());
    const int64_t recordedBody = hunk.newStart - 1;
    const int64_t recordedPattern = recordedBody - static_cast<int64_t>(pre.size());

    HunkFindResult result;
    const Located found = Locate(file, Concat(pre, after, post), recordedPattern, allowShift);
    if (found.count >= 2) {
        result.find = HunkFind::Ambiguous;
        return result;
    }
    if (found.count == 1) {
        const size_t bodyStart = found.first + pre.size();
        if (options.exactContextLength && found.exact) {
            const size_t afterEnd = bodyStart + after.size();
            const size_t wantPre = static_cast<size_t>((std::min<int64_t>)(kHunkContextLines, static_cast<int64_t>(bodyStart)));
            const size_t wantPost = static_cast<size_t>((std::min<int64_t>)(kHunkContextLines, static_cast<int64_t>(file.size() - afterEnd)));
            if (pre.size() != wantPre || post.size() != wantPost) {
                return result; // NotFound
            }
        }
        result.find = HunkFind::Match;
        result.shiftLines = static_cast<int64_t>(found.first) - recordedPattern;
        result.bodyLine = static_cast<int64_t>(bodyStart);
        if (matchOut != nullptr) {
            matchOut->bodyStart = bodyStart;
            matchOut->bodyCount = after.size();
        }
        return result;
    }
    if (options.detectRestored) {
        const Located restored = Locate(file, Concat(pre, before, post), recordedPattern, allowShift);
        if (restored.count >= 2) {
            result.find = HunkFind::Ambiguous;
        } else if (restored.count == 1) {
            result.find = HunkFind::Restored;
            result.shiftLines = static_cast<int64_t>(restored.first) - recordedPattern;
            result.bodyLine = static_cast<int64_t>(restored.first + pre.size());
        }
    }
    return result;
}

size_t ByteOffset(const Lines& file, size_t line, std::string_view whole)
{
    return line >= file.size() ? whole.size() : static_cast<size_t>(file[line].data() - whole.data());
}

} // namespace

std::vector<std::string_view> SplitLinesKeepEol(std::string_view text)
{
    Lines lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t newline = text.find('\n', start);
        const size_t end = newline == std::string_view::npos ? text.size() : newline + 1;
        lines.push_back(text.substr(start, end - start));
        start = end;
    }
    return lines;
}

HunkApplyResult UnapplyHunks(std::string_view current, const std::vector<HunkSpec>& hunks, const HunkApplyOptions& options)
{
    HunkApplyResult result;
    result.finds.resize(hunks.size());
    const size_t newlines = static_cast<size_t>(std::count(current.begin(), current.end(), '\n'));
    if (newlines + 1 > kHunkRestoreMaxLines) {
        result.failure = HunkApplyFailure::TooManyLines;
        return result;
    }
    const Lines file = SplitLinesKeepEol(current);

    std::vector<Match> matches;
    for (size_t i = 0; i < hunks.size(); ++i) {
        Match match;
        match.index = i;
        result.finds[i] = FindOne(file, hunks[i], options, &match);
        if (result.finds[i].find == HunkFind::Match) {
            matches.push_back(match);
        }
        if (result.finds[i].find == HunkFind::Ambiguous || result.finds[i].find == HunkFind::NotFound) {
            result.failure = HunkApplyFailure::Conflict;
        }
    }
    if (result.failure != HunkApplyFailure::None) {
        return result;
    }

    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        if (a.bodyStart != b.bodyStart) {
            return a.bodyStart < b.bodyStart;
        }
        return a.index < b.index;
    });
    size_t previousEnd = 0;
    for (const Match& match : matches) {
        if (match.bodyStart < previousEnd) {
            result.failure = HunkApplyFailure::Overlap;
            return result;
        }
        previousEnd = match.bodyStart + match.bodyCount;
    }

    result.changed = !matches.empty();
    result.bytes.reserve(current.size());
    size_t cursorLine = 0;
    for (const Match& match : matches) {
        result.bytes.append(current.substr(ByteOffset(file, cursorLine, current), ByteOffset(file, match.bodyStart, current) - ByteOffset(file, cursorLine, current)));
        result.bytes += hunks[match.index].beforeText;
        cursorLine = match.bodyStart + match.bodyCount;
    }
    result.bytes.append(current.substr(ByteOffset(file, cursorLine, current)));
    return result;
}

} // namespace mye
