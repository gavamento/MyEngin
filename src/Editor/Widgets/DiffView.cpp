//====================================================================================
//                          DiffView.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          Hunk の列を描く読み取り専用の差分表示部品 (行の組み立てと描画)
//====================================================================================
#include "Editor/Widgets/DiffView.h"

#include <algorithm>
#include <cstdio>

#include "imgui.h"

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

namespace mye {

namespace {

// 追加・削除・改行だけの変更の面の色。テーマの意味色 (Success / Error / Warning) に合わせた半透明の面で、
// 色の設定 (別のサブ) とは独立
constexpr ImU32 kColDeleteBg = IM_COL32(220, 70, 70, 56);
constexpr ImU32 kColAddBg = IM_COL32(70, 190, 100, 56);
constexpr ImU32 kColEolBg = IM_COL32(230, 180, 60, 40);
constexpr ImU32 kColEmptyBg = IM_COL32(255, 255, 255, 10);
constexpr ImU32 kColHeaderBg = IM_COL32(255, 255, 255, 18);
constexpr ImU32 kColSelectedHeaderBg = IM_COL32(60, 110, 190, 110);
constexpr ImU32 kColSplitLine = IM_COL32(255, 255, 255, 40);

// 桁数 (全角 = 2)。CJK・全角形・ハングルの主な範囲
int CodepointColumns(uint32_t cp)
{
    const bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3)
                      || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60)
                      || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
    return wide ? 2 : 1;
}

// UTF-8 の先頭バイトから 1 文字の長さ (不正なバイトは 1)
size_t Utf8Length(unsigned char lead)
{
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

uint32_t Utf8Decode(std::string_view s, size_t pos, size_t len)
{
    if (len == 1 || pos + len > s.size()) {
        return static_cast<unsigned char>(s[pos]);
    }
    uint32_t cp = static_cast<unsigned char>(s[pos]) & (0xFFu >> (len + 1));
    for (size_t i = 1; i < len; ++i) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[pos + i]) & 0x3Fu);
    }
    return cp;
}

int64_t DigitCount(int64_t n)
{
    int64_t digits = 1;
    while (n >= 10) {
        n /= 10;
        ++digits;
    }
    return digits;
}

const char* EolLabel(DiffEol eol)
{
    switch (eol) {
    case DiffEol::Lf: return "LF";
    case DiffEol::CrLf: return "CRLF";
    case DiffEol::None: break;
    }
    return "NoEOL";
}

} // namespace

std::vector<DiffLineSlice> DiffRows::SplitLines(std::string_view text)
{
    std::vector<DiffLineSlice> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t lf = text.find('\n', start);
        DiffLineSlice slice;
        if (lf == std::string_view::npos) {
            slice.body = text.substr(start);
            slice.eol = DiffEol::None;
            start = text.size();
        } else {
            size_t end = lf;
            slice.eol = DiffEol::Lf;
            if (end > start && text[end - 1] == '\r') {
                --end;
                slice.eol = DiffEol::CrLf;
            }
            slice.body = text.substr(start, end - start);
            start = lf + 1;
        }
        lines.push_back(slice);
    }
    return lines;
}

DiffTextRef DiffRows::Store(std::string_view body)
{
    DiffTextRef ref;
    ref.off = static_cast<uint32_t>(pool_.size());
    int32_t column = 0;
    size_t pos = 0;
    bool truncated = false;
    while (pos < body.size()) {
        if (pool_.size() - ref.off >= kMaxLineBytes) {
            truncated = true;
            break;
        }
        if (body[pos] == '\t') {
            const int32_t pad = kTabWidth - (column % kTabWidth);
            pool_.append(static_cast<size_t>(pad), ' ');
            column += pad;
            ++pos;
            continue;
        }
        const size_t len = (std::min)(Utf8Length(static_cast<unsigned char>(body[pos])), body.size() - pos);
        pool_.append(body.substr(pos, len));
        column += CodepointColumns(Utf8Decode(body, pos, len));
        pos += len;
    }
    if (truncated) {
        pool_ += "...";
        column += 3;
    }
    ref.len = static_cast<uint32_t>(pool_.size()) - ref.off;
    maxColumns_ = (std::max)(maxColumns_, column);
    return ref;
}

void DiffRows::Clear()
{
    inline_.clear();
    split_.clear();
    headers_.clear();
    pool_.clear();
    maxColumns_ = 0;
    maxLineNo_ = 0;
}

void DiffRows::Build(const std::vector<DiffHunkInput>& hunks)
{
    Clear();
    for (size_t h = 0; h < hunks.size(); ++h) {
        const DiffHunkInput& in = hunks[h];
        const int32_t hunk = static_cast<int32_t>(h);
        HunkInfo info;
        info.header = "@@ -" + std::to_string(in.oldStart) + "," + std::to_string(in.oldCount) + " +" + std::to_string(in.newStart) + ","
                      + std::to_string(in.newCount) + " @@";
        info.eolOnly = in.eolOnly;
        info.oversize = in.oversize;
        info.beforeBytes = in.beforeBytes;
        info.afterBytes = in.afterBytes;
        info.badge = in.badge;
        info.badgeColor = in.badgeColor;
        headers_.push_back(std::move(info));
        maxLineNo_ = (std::max)({ maxLineNo_, in.oldStart + in.oldCount + 3, in.newStart + in.newCount + 3 });

        DiffInlineRow header;
        header.kind = DiffRowKind::Header;
        header.hunk = hunk;
        inline_.push_back(header);
        DiffSplitRow splitHeader;
        splitHeader.kind = DiffRowKind::Header;
        splitHeader.hunk = hunk;
        split_.push_back(splitHeader);

        if (in.oversize) {
            DiffInlineRow notice;
            notice.kind = DiffRowKind::Notice;
            notice.hunk = hunk;
            inline_.push_back(notice);
            DiffSplitRow splitNotice;
            splitNotice.kind = DiffRowKind::Notice;
            splitNotice.hunk = hunk;
            split_.push_back(splitNotice);
            continue;
        }

        const std::vector<DiffLineSlice> pre = SplitLines(in.contextPre);
        const std::vector<DiffLineSlice> before = SplitLines(in.before);
        const std::vector<DiffLineSlice> after = SplitLines(in.after);
        const std::vector<DiffLineSlice> post = SplitLines(in.contextPost);

        // 文脈の行 (両側で同じ行)。インラインは旧・新の両方の行番号を持つ
        auto addContext = [&](const std::vector<DiffLineSlice>& lines, int64_t oldFirst, int64_t newFirst) {
            for (size_t i = 0; i < lines.size(); ++i) {
                const DiffTextRef text = Store(lines[i].body);
                DiffInlineRow row;
                row.hunk = hunk;
                row.line = DiffLineKind::Context;
                row.oldNo = oldFirst + static_cast<int64_t>(i);
                row.newNo = newFirst + static_cast<int64_t>(i);
                row.text = text;
                row.eol = lines[i].eol;
                inline_.push_back(row);
                DiffSplitRow split;
                split.hunk = hunk;
                split.left = { DiffLineKind::Context, row.oldNo, text, lines[i].eol, false };
                split.right = { DiffLineKind::Context, row.newNo, text, lines[i].eol, false };
                split_.push_back(split);
            }
        };

        addContext(pre, in.oldStart - static_cast<int64_t>(pre.size()), in.newStart - static_cast<int64_t>(pre.size()));

        if (in.eolOnly && before.size() == after.size()) {
            // 本文は同じで行末だけが違う。色は付けず、行末の印だけで示す
            for (size_t i = 0; i < before.size(); ++i) {
                const DiffTextRef text = Store(before[i].body);
                DiffInlineRow row;
                row.hunk = hunk;
                row.line = DiffLineKind::EolChange;
                row.oldNo = in.oldStart + static_cast<int64_t>(i);
                row.newNo = in.newStart + static_cast<int64_t>(i);
                row.text = text;
                row.eol = before[i].eol;
                row.eolAfter = after[i].eol;
                row.eolMark = true;
                inline_.push_back(row);
                DiffSplitRow split;
                split.hunk = hunk;
                split.left = { DiffLineKind::EolChange, row.oldNo, text, before[i].eol, true };
                split.right = { DiffLineKind::EolChange, row.newNo, text, after[i].eol, true };
                split_.push_back(split);
            }
        } else {
            // インライン: 削除の行 → 追加の行。左右分割: 同じ位置の削除と追加を 1 行に並べ、足りない側は空行
            const size_t paired = (std::min)(before.size(), after.size());
            auto eolDiffers = [&](size_t i) { return i < paired && before[i].eol != after[i].eol; };
            for (size_t i = 0; i < before.size(); ++i) {
                DiffInlineRow row;
                row.hunk = hunk;
                row.line = DiffLineKind::Delete;
                row.oldNo = in.oldStart + static_cast<int64_t>(i);
                row.text = Store(before[i].body);
                row.eol = before[i].eol;
                row.eolMark = eolDiffers(i);
                inline_.push_back(row);
            }
            for (size_t i = 0; i < after.size(); ++i) {
                DiffInlineRow row;
                row.hunk = hunk;
                row.line = DiffLineKind::Add;
                row.newNo = in.newStart + static_cast<int64_t>(i);
                row.text = Store(after[i].body);
                row.eol = after[i].eol;
                row.eolMark = eolDiffers(i);
                inline_.push_back(row);
            }
            // 左右分割は文字列を二重に積まないよう、インラインで積んだ範囲を参照する
            const size_t inlineAfterStart = inline_.size() - after.size();
            const size_t inlineBeforeStart = inlineAfterStart - before.size();
            const size_t rowsNeeded = (std::max)(before.size(), after.size());
            for (size_t i = 0; i < rowsNeeded; ++i) {
                DiffSplitRow split;
                split.hunk = hunk;
                if (i < before.size()) {
                    const DiffInlineRow& src = inline_[inlineBeforeStart + i];
                    split.left = { DiffLineKind::Delete, src.oldNo, src.text, src.eol, src.eolMark };
                }
                if (i < after.size()) {
                    const DiffInlineRow& src = inline_[inlineAfterStart + i];
                    split.right = { DiffLineKind::Add, src.newNo, src.text, src.eol, src.eolMark };
                }
                split_.push_back(split);
            }
        }

        addContext(post, in.oldStart + in.oldCount, in.newStart + in.newCount);
    }
}

void DiffViewer::SetHunks(const std::vector<DiffHunkInput>& hunks)
{
    rows_.Build(hunks);
    selected_ = -1;
}

void DiffViewer::Clear()
{
    rows_.Clear();
    selected_ = -1;
}

void DiffViewer::SelectHunk(int hunk)
{
    selected_ = hunk >= 0 && static_cast<size_t>(hunk) < rows_.HunkCount() ? hunk : -1;
}

namespace {

struct Metrics {
    float charW = 0.0f;
    float lineH = 0.0f;
    float numW = 0.0f;    // 行番号 1 列の幅
    float gutterW = 0.0f; // 1 ペインの、本文より左の幅
    float textW = 0.0f;   // 本文の最大幅の見積り
    float paneW = 0.0f;   // 1 ペインの幅
};

// 1 行ぶんの文字を置く。行番号・印・行末の印・本文の順
void DrawLineText(ImDrawList* dl, const ImVec2& pos, ImU32 col, std::string_view text)
{
    if (!text.empty()) {
        dl->AddText(pos, col, text.data(), text.data() + text.size());
    }
}

void DrawNumber(ImDrawList* dl, const ImVec2& pos, ImU32 col, int64_t no)
{
    if (no > 0) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(no));
        dl->AddText(pos, col, buf);
    }
}

} // namespace

bool DiffViewer::Draw(const char* id, const char* emptyMessage, float width, float height)
{
    lastDrawnRows_ = 0;
    if (rows_.HunkCount() == 0) {
        ImGui::TextWrapped("%s", emptyMessage);
        return false;
    }
    const bool isInline = layout_ == DiffLayout::Inline;
    const size_t rowCount = isInline ? rows_.InlineRows().size() : rows_.SplitRows().size();

    Metrics m;
    m.charW = ImGui::CalcTextSize("0").x;
    m.lineH = ImGui::GetTextLineHeight() + 2.0f;
    m.numW = m.charW * static_cast<float>(std::max<int64_t>(3, DigitCount(rows_.MaxLineNo()))) + m.charW;
    // 行番号 (インラインは旧・新、左右分割は片側に 1 つ) + 印 1 桁 + 行末の印 5 桁
    const float signW = m.charW * 2.0f;
    const float eolW = m.charW * 6.0f;
    m.gutterW = (isInline ? m.numW * 2.0f : m.numW) + signW + eolW;
    m.textW = m.charW * static_cast<float>(rows_.MaxColumns()) + m.charW * 2.0f;
    m.paneW = m.gutterW + m.textW;
    const float contentW = isInline ? m.paneW : m.paneW * 2.0f;

    bool changed = false;
    ImGui::SetNextWindowContentSize(ImVec2(contentW, 0.0f));
    if (!ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 colDim = ImGui::GetColorU32(themeColor::TextDim);
    const ImU32 colAccent = ImGui::GetColorU32(themeColor::Accent);

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rowCount), m.lineH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            ++lastDrawnRows_;
            const int hunk = isInline ? rows_.InlineRows()[static_cast<size_t>(i)].hunk : rows_.SplitRows()[static_cast<size_t>(i)].hunk;
            const DiffRowKind kind = isInline ? rows_.InlineRows()[static_cast<size_t>(i)].kind : rows_.SplitRows()[static_cast<size_t>(i)].kind;
            ImGui::PushID(i);
            ImGui::InvisibleButton("##row", ImVec2(contentW, m.lineH));
            ImGui::PopID();
            if (ImGui::IsItemClicked() && selected_ != hunk) {
                selected_ = hunk;
                changed = true;
            }
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const bool selected = hunk == selected_;
            const float textY = min.y + 1.0f;

            if (kind == DiffRowKind::Header || kind == DiffRowKind::Notice) {
                dl->AddRectFilled(min, max, selected && kind == DiffRowKind::Header ? kColSelectedHeaderBg : kColHeaderBg);
                const size_t h = static_cast<size_t>(hunk);
                if (kind == DiffRowKind::Header) {
                    const std::string& header = rows_.HunkHeader(h);
                    dl->AddText(ImVec2(min.x + m.charW, textY), colDim, header.c_str(), header.c_str() + header.size());
                    float afterX = min.x + m.charW * 3.0f + ImGui::CalcTextSize(header.c_str()).x;
                    if (rows_.HunkEolOnly(h)) {
                        dl->AddText(ImVec2(afterX, textY), ImGui::GetColorU32(themeColor::Warning), Tr(StrId::DiffView_EolOnly));
                        afterX += ImGui::CalcTextSize(Tr(StrId::DiffView_EolOnly)).x + m.charW * 2.0f;
                    }
                    const std::string& badge = rows_.HunkBadge(h);
                    if (!badge.empty()) {
                        const uint32_t color = rows_.HunkBadgeColor(h);
                        dl->AddText(ImVec2(afterX, textY), color != 0 ? color : colDim, badge.c_str(), badge.c_str() + badge.size());
                    }
                } else {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "%s (%lld / %lld B)", Tr(StrId::DiffView_Oversize),
                                  static_cast<long long>(rows_.HunkBeforeBytes(h)), static_cast<long long>(rows_.HunkAfterBytes(h)));
                    dl->AddText(ImVec2(min.x + m.charW, textY), ImGui::GetColorU32(themeColor::Warning), buf);
                }
            } else if (isInline) {
                const DiffInlineRow& row = rows_.InlineRows()[static_cast<size_t>(i)];
                if (row.line == DiffLineKind::Delete) {
                    dl->AddRectFilled(min, max, kColDeleteBg);
                } else if (row.line == DiffLineKind::Add) {
                    dl->AddRectFilled(min, max, kColAddBg);
                } else if (row.line == DiffLineKind::EolChange) {
                    dl->AddRectFilled(min, max, kColEolBg);
                }
                float x = min.x;
                DrawNumber(dl, ImVec2(x + m.charW * 0.5f, textY), colDim, row.oldNo);
                x += m.numW;
                DrawNumber(dl, ImVec2(x + m.charW * 0.5f, textY), colDim, row.newNo);
                x += m.numW;
                const char* sign = row.line == DiffLineKind::Delete ? "-" : (row.line == DiffLineKind::Add ? "+" : " ");
                dl->AddText(ImVec2(x + m.charW * 0.5f, textY), colText, sign);
                x += signW;
                if (row.eolMark) {
                    char buf[24];
                    if (row.line == DiffLineKind::EolChange) {
                        std::snprintf(buf, sizeof(buf), "%s>%s", EolLabel(row.eol), EolLabel(row.eolAfter));
                    } else {
                        std::snprintf(buf, sizeof(buf), "%s", EolLabel(row.eol));
                    }
                    dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(themeColor::Warning), buf);
                }
                x += eolW;
                DrawLineText(dl, ImVec2(x, textY), row.line == DiffLineKind::EolChange ? colDim : colText, rows_.Text(row.text));
            } else {
                const DiffSplitRow& row = rows_.SplitRows()[static_cast<size_t>(i)];
                const DiffSplitSide* sides[2] = { &row.left, &row.right };
                for (int s = 0; s < 2; ++s) {
                    const DiffSplitSide& side = *sides[s];
                    const float x0 = min.x + m.paneW * static_cast<float>(s);
                    const ImVec2 paneMin(x0, min.y);
                    const ImVec2 paneMax(x0 + m.paneW, max.y);
                    ImU32 bg = 0;
                    switch (side.kind) {
                    case DiffLineKind::Delete: bg = kColDeleteBg; break;
                    case DiffLineKind::Add: bg = kColAddBg; break;
                    case DiffLineKind::EolChange: bg = kColEolBg; break;
                    case DiffLineKind::Empty: bg = kColEmptyBg; break;
                    case DiffLineKind::Context: break;
                    }
                    if (bg != 0) {
                        dl->AddRectFilled(paneMin, paneMax, bg);
                    }
                    float x = x0;
                    DrawNumber(dl, ImVec2(x + m.charW * 0.5f, textY), colDim, side.no);
                    x += m.numW;
                    if (side.eolMark) {
                        dl->AddText(ImVec2(x + signW, textY), ImGui::GetColorU32(themeColor::Warning), EolLabel(side.eol));
                    }
                    x += signW + eolW;
                    DrawLineText(dl, ImVec2(x, textY), side.kind == DiffLineKind::EolChange ? colDim : colText, rows_.Text(side.text));
                }
                dl->AddLine(ImVec2(min.x + m.paneW, min.y), ImVec2(min.x + m.paneW, max.y), kColSplitLine);
            }
            if (selected) {
                dl->AddRectFilled(min, ImVec2(min.x + 3.0f, max.y), colAccent);
            }
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    return changed;
}

} // namespace mye
