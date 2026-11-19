//====================================================================================
//                          ChangeViewSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Changes 窓の文面・節・DiffView の行の組み立ての回帰テスト
//====================================================================================
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"
#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/ChangeFormat.h"
#include "Editor/ChangeTracking/ChangeTrackingSelfTest.h"
#include "Editor/Widgets/DiffView.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"

using nlohmann::json;

namespace mye {
namespace {

using Check = std::function<void(bool, const char*)>;

// 言語を切り替えて呼び、戻す
template <typename F>
void InBothLanguages(F&& body)
{
    const Lang previous = CurrentLanguage();
    for (const Lang lang : { Lang::En, Lang::Ja }) {
        SetLanguage(lang);
        body(lang);
    }
    SetLanguage(previous);
}

bool AllNonEmptyAndDistinct(const std::vector<std::string>& texts)
{
    std::set<std::string> seen;
    for (const std::string& t : texts) {
        if (t.empty() || !seen.insert(t).second) {
            return false;
        }
    }
    return true;
}

TrackChangeRow Row(int64_t id, const char* path, const char* attribution, const char* actor = "ai")
{
    TrackChangeRow r;
    r.revisionId = id;
    r.path = path;
    r.changeType = "modified";
    r.actor = actor;
    r.attribution = attribution;
    r.completeness = "complete";
    return r;
}

void TestFormat(const Check& check)
{
    InBothLanguages([&](Lang lang) {
        const bool en = lang == Lang::En;
        const char* tag = en ? "en" : "ja";
        auto name = [&](const char* what) { return std::string(what) + " (" + tag + ")"; };

        std::vector<std::string> kinds;
        for (const char* k : { "added", "modified", "deleted", "renamed" }) {
            kinds.push_back(KindLabel(k));
        }
        check(AllNonEmptyAndDistinct(kinds) && KindLabel("added") != "added" && KindLabel("renamed") != "renamed", name("format: 4 change kinds have their own labels").c_str());
        check(KindLabel("future_kind") == "future_kind", name("format: an unknown kind falls back to its raw value").c_str());

        std::vector<std::string> attrs;
        std::vector<std::string> attrNotes;
        for (const char* a : { "verified", "claimed", "inferred", "unknown" }) {
            attrs.push_back(AttributionLabel(a));
            attrNotes.push_back(AttributionNote("ai", a));
        }
        check(AllNonEmptyAndDistinct(attrs) && AllNonEmptyAndDistinct(attrNotes), name("format: 4 attribution classes have a label and an explanation").c_str());
        // Editor / 復元で確定した変更 (帰属は verified) は、AI の「検証済み」と同じ説明にしない
        const std::string aiVerified = AttributionNote("ai", "verified");
        const std::string editorNote = AttributionNote("editor_user", "verified");
        const std::string restoreNote = AttributionNote("restore", "verified");
        check(!editorNote.empty() && !restoreNote.empty() && editorNote != aiVerified && restoreNote != aiVerified && editorNote != restoreNote,
              name("format: an editor save and a restore are explained as such, not as an AI's verified edit").c_str());
        std::vector<std::string> actors;
        for (const char* a : { "ai", "editor_user", "restore" }) {
            actors.push_back(ActorLabel(a));
        }
        check(AllNonEmptyAndDistinct(actors), name("format: actors have labels").c_str());

        std::vector<std::string> evidence;
        for (const std::string& kind : EvidenceKinds()) {
            evidence.push_back(EvidenceText(json{ { "kind", kind } }));
        }
        check(EvidenceKinds().size() == 12 && AllNonEmptyAndDistinct(evidence), name("format: 12 evidence kinds each have their own sentence").c_str());
        const std::string unknownKind = EvidenceText(json{ { "kind", "brand_new_kind" } });
        check(unknownKind.find("brand_new_kind") != std::string::npos, name("format: an unknown evidence kind still shows its name").c_str());
        const std::string plain = EvidenceText(json{ { "kind", "observed" }, { "detail", { { "source", "watch" } } } });
        const std::string verify = EvidenceText(json{ { "kind", "observed" }, { "detail", { { "source", "verify_gap" } } } });
        const std::string startup = EvidenceText(json{ { "kind", "observed" }, { "detail", { { "source", "startup_gap" } } } });
        check(verify.size() > plain.size() && startup.size() > plain.size() && verify != startup,
              name("format: observed evidence says how it was found (verify_gap / startup_gap)").c_str());

        std::vector<std::string> completeness;
        for (const char* c : { "complete", "coalesced", "gap", "no_before" }) {
            completeness.push_back(CompletenessLabel(c));
        }
        check(AllNonEmptyAndDistinct(completeness), name("format: 4 completeness values have labels").c_str());
        std::vector<std::string> notes;
        for (const char* c : { "coalesced", "gap", "no_before" }) {
            notes.push_back(CompletenessNote(c, "watch"));
        }
        check(CompletenessNote("complete", "").empty() && AllNonEmptyAndDistinct(notes), name("format: only a non-complete value gets a one-line note").c_str());
        check(CompletenessNote("gap", "verify_gap") != CompletenessNote("gap", "startup_gap") && !CompletenessNote("gap", "verify_gap").empty(),
              name("format: a gap picked up later by the verification scan says so").c_str());

        std::vector<std::string> modeLabels;
        std::vector<std::string> modeNotes;
        for (const char* m : { "lines", "whole", "binary", "excluded", "unavailable" }) {
            modeLabels.push_back(DiffModeLabel(m));
            if (std::strcmp(m, "lines") != 0) {
                modeNotes.push_back(DiffModeNote(m));
            }
        }
        check(AllNonEmptyAndDistinct(modeLabels) && AllNonEmptyAndDistinct(modeNotes) && DiffModeNote("lines").empty(),
              name("format: 5 diff modes have labels, and the 4 without plain lines have a reason").c_str());

        std::vector<std::string> banners;
        for (const TrackState s : { TrackState::Disabled, TrackState::NoProject, TrackState::Connecting, TrackState::Ready, TrackState::NoService,
                                    TrackState::ProtoMismatch, TrackState::WrongProject, TrackState::DbDegraded, TrackState::Disconnected }) {
            banners.push_back(StateBannerText(s, true, ""));
        }
        check(AllNonEmptyAndDistinct(banners), name("format: each of the 9 connection states has its own banner").c_str());
        check(StateBannerText(TrackState::Disabled, false, "") != StateBannerText(TrackState::Disabled, true, ""),
              name("format: a non-interactive launch says tracking does not run").c_str());
        check(StateBannerText(TrackState::DbDegraded, true, "disk is full").find("disk is full") != std::string::npos,
              name("format: the db_degraded banner carries the reason").c_str());
    });

    InBothLanguages([&](Lang) {
        // 行
        TrackChangeRow renamed = Row(5, "new/name.txt", "claimed");
        renamed.changeType = "renamed";
        renamed.oldPath = "old/name.txt";
        renamed.completeness = "gap";
        renamed.changeState = "superseded";
        renamed.observedAt = 1700000000000;
        renamed.afterSize = 2048;
        const ChangeRowText text = FormatChangeRow(renamed);
        check(text.path == "old/name.txt -> new/name.txt" && !text.kind.empty() && !text.attribution.empty() && !text.completeness.empty()
                  && !text.superseded.empty() && text.time.size() == 19 && text.size == "2.0 KiB",
              "format: a list row shows the rename, the marks for non-complete and superseded, the time and the size");
        const ChangeRowText plainRow = FormatChangeRow(Row(6, "a.txt", "verified"));
        check(plainRow.completeness.empty() && plainRow.superseded.empty() && plainRow.size.empty() && plainRow.time.empty(),
              "format: a complete, active row has no marks");

        TrackSessionRow session;
        session.sessionId = "0123456789abcdef";
        session.provider = "claude";
        session.status = "interrupted";
        session.startedAt = 1700000000000;
        session.changeCount = 12;
        const SessionRowText st = FormatSessionRow(session);
        check(st.title == "claude" && st.status == (CurrentLanguage() == Lang::En ? "interrupted" : "中断") && st.time.size() == 19 && st.count.find("12") == 0,
              "format: a session row shows provider, state, start time and the change count");
        session.provider.clear();
        check(FormatSessionRow(session).title == "01234567", "format: a session without a provider falls back to its id");

        // 統合ターミナルで始まったセッションだけ出所を出す (M106f)
        check(FormatSessionRow(session).origin.empty() && FormatSessionRow(session, true).origin.empty(),
              "format: a session outside the integrated terminal shows no origin");
        session.hostTerminal = "42-1763500000-2";
        const SessionRowText otherTerminal = FormatSessionRow(session, false);
        const SessionRowText thisTerminal = FormatSessionRow(session, true);
        const bool en = CurrentLanguage() == Lang::En;
        check(otherTerminal.origin == (en ? "Integrated terminal" : "統合ターミナル")
                  && thisTerminal.origin == (en ? "Integrated terminal (this editor)" : "統合ターミナル (このエディタ)"),
              "format: an integrated-terminal session shows its origin, and marks this editor's own terminal");
    });

    // 時刻: UTC ミリ秒 → 現地時刻。現地の日時として読み戻すと元の秒になる
    {
        const std::string text = FormatTimeMs(1700000000123);
        std::tm tm{};
        int n = std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = -1;
        check(n == 6 && text.size() == 19 && static_cast<int64_t>(std::mktime(&tm)) == 1700000000, "format: UTC milliseconds become a local 'YYYY-MM-DD HH:MM:SS'");
        check(FormatTimeMs(0).empty() && FormatTimeMs(-5).empty(), "format: a time of zero or less is empty");
        check(FormatBytes(512) == "512 B" && FormatBytes(1536) == "1.5 KiB" && FormatBytes(1024 * 1024 * 3 / 2) == "1.5 MiB"
                  && FormatBytes(2LL * 1024 * 1024 * 1024) == "2.00 GiB" && FormatBytes(-1).empty(),
              "format: sizes use 1024 steps");
    }

    // 状態の要約
    InBothLanguages([&](Lang) {
        TrackStatusInfo s;
        check(StatusLines(s).empty(), "status lines: nothing before the first status");
        s.valid = true;
        s.phase = "baselining";
        s.scanDone = 3;
        s.scanTotal = 9;
        s.trackedFiles = 12;
        s.blobBytes = 1536;
        s.blobCount = 5;
        s.gapCount = 2;
        std::vector<std::string> lines = StatusLines(s);
        check(lines.size() == 4 && lines[0].find("3 / 9") != std::string::npos && lines[1].find("12") != std::string::npos
                  && lines[2].find("1.5 KiB") != std::string::npos && lines[2].find('5') != std::string::npos && lines[3].find('2') != std::string::npos,
              "status lines: baseline progress, tracked files, blob total, gaps");
        s.phase = "watching";
        s.scanDone = s.scanTotal = 9;
        s.watchFallback = true;
        lines = StatusLines(s);
        check(lines.size() == 5 && lines[0].find("3 / 9") == std::string::npos && lines[0].find("9 / 9") == std::string::npos,
              "status lines: after the baseline it says done, and a fallback watch adds a line");
    });

    // プロジェクトの外を指すパスは受けない
    {
        check(ProjectFilePath(L"C:\\proj", "assets/a.txt") == L"C:\\proj\\assets\\a.txt", "path: a relative path joins the project root");
        check(ProjectFilePath(L"C:\\proj\\", "a.txt") == L"C:\\proj\\a.txt", "path: a trailing separator on the root is not doubled");
        check(ProjectFilePath(L"C:\\proj", "ゲーム/シーン.json") == L"C:\\proj\\ゲーム\\シーン.json", "path: Japanese names survive");
        check(ProjectFilePath(L"C:\\proj", "../x").empty() && ProjectFilePath(L"C:\\proj", "a/../../x").empty() && ProjectFilePath(L"C:\\proj", "..").empty(),
              "path: '..' is refused");
        check(ProjectFilePath(L"C:\\proj", "C:/x").empty() && ProjectFilePath(L"C:\\proj", "/x").empty() && ProjectFilePath(L"C:\\proj", "\\\\srv\\share").empty()
                  && ProjectFilePath(L"", "a").empty() && ProjectFilePath(L"C:\\proj", "").empty(),
              "path: absolute paths, an empty root and an empty path are refused");
        check(ProjectFilePath(L"C:\\proj", "a..b/c.txt") == L"C:\\proj\\a..b\\c.txt", "path: a name that merely contains two dots is allowed");
    }
}

void TestSections(const Check& check)
{
    std::vector<TrackChangeRow> rows = { Row(1, "a", "claimed"), Row(2, "b", "unknown"), Row(3, "c", "verified"), Row(4, "d", "inferred"), Row(5, "e", "unknown") };
    const ChangePartition split = PartitionChanges(rows, false);
    check(split.main == std::vector<size_t>({ 0, 2 }) && split.uncertain == std::vector<size_t>({ 1, 3, 4 }) && split.editor.empty(),
          "sections: unknown and inferred go to their own section by default, in their original order");
    const ChangePartition mixed = PartitionChanges(rows, true);
    check(mixed.main == std::vector<size_t>({ 0, 1, 2, 3, 4 }) && mixed.uncertain.empty(), "sections: the toggle mixes them into one list");
    check(ClassifyChange("ai", "verified", false) == ChangeSection::Main && ClassifyChange("ai", "claimed", false) == ChangeSection::Main
              && ClassifyChange("ai", "inferred", false) == ChangeSection::Uncertain && ClassifyChange("unknown", "unknown", false) == ChangeSection::Uncertain
              && ClassifyChange("unknown", "unknown", true) == ChangeSection::Main && ClassifyChange("ai", "never_seen", false) == ChangeSection::Main,
          "sections: classification by attribution (an unrecognised value is not hidden in the uncertain section)");
    const ChangePartition none = PartitionChanges({}, false);
    check(none.main.empty() && none.uncertain.empty() && none.editor.empty(), "sections: an empty list partitions into nothing");

    // Editor の保存・復元で確定した変更 (帰属は verified) は、AI の変更の節にも不明 / 推定の節にも入らない
    std::vector<TrackChangeRow> withEditor = { Row(1, "a", "claimed"),   Row(2, "scene", "verified", "editor_user"), Row(3, "b", "unknown", "unknown"),
                                               Row(4, "c", "verified", "restore"), Row(5, "d", "inferred") };
    const ChangePartition three = PartitionChanges(withEditor, false);
    check(three.main == std::vector<size_t>({ 0 }) && three.uncertain == std::vector<size_t>({ 2, 4 }) && three.editor == std::vector<size_t>({ 1, 3 }),
          "sections: changes confirmed as an editor save or a restore get their own section, away from the AI and the unknown ones");
    const ChangePartition threeMixed = PartitionChanges(withEditor, true);
    check(threeMixed.main == std::vector<size_t>({ 0, 2, 4 }) && threeMixed.editor == std::vector<size_t>({ 1, 3 }),
          "sections: mixing unknown / inferred does not pull the editor's changes into the AI list");
    check(ClassifyChange("editor_user", "verified", false) == ChangeSection::Editor && ClassifyChange("restore", "verified", true) == ChangeSection::Editor,
          "sections: actor editor_user / restore is the editor section whatever the mixing toggle says");
    InBothLanguages([&](Lang) {
        const ChangeRowText editorRow = FormatChangeRow(Row(9, "scene.json", "verified", "editor_user"));
        check(editorRow.attribution == ActorLabel("editor_user") && editorRow.attribution != AttributionLabel("verified"),
              "sections: an editor row shows who made it, not the AI-style 'verified'");
        check(FormatChangeRow(Row(9, "a", "verified")).attribution == AttributionLabel("verified"), "sections: an AI row still shows its attribution");
    });
}

std::string Joined(const DiffRows& rows, const std::vector<DiffInlineRow>& list)
{
    std::string out;
    for (const DiffInlineRow& r : list) {
        out += r.kind == DiffRowKind::Header ? 'H' : (r.kind == DiffRowKind::Notice ? 'N' : "C-+~."[static_cast<int>(r.line)]);
        (void)rows;
    }
    return out;
}

void TestRows(const Check& check)
{
    // 行の分け方
    {
        const std::vector<DiffLineSlice> lines = DiffRows::SplitLines("a\r\nb\nc");
        check(lines.size() == 3 && lines[0].body == "a" && lines[0].eol == DiffEol::CrLf && lines[1].body == "b" && lines[1].eol == DiffEol::Lf
                  && lines[2].body == "c" && lines[2].eol == DiffEol::None,
              "diff rows: CRLF, LF and no-EOL lines are told apart");
        check(DiffRows::SplitLines("").empty() && DiffRows::SplitLines("\n").size() == 1 && DiffRows::SplitLines("\n")[0].body.empty(),
              "diff rows: empty text has no lines; a lone LF is one empty line");
        check(DiffRows::SplitLines("a\r")[0].body == "a\r" && DiffRows::SplitLines("a\r")[0].eol == DiffEol::None, "diff rows: a lone CR stays in the text");
    }

    // 複数 Hunk (文脈つき): インラインと左右分割の行の列
    {
        DiffHunkInput h1;
        h1.oldStart = 3;
        h1.oldCount = 1;
        h1.newStart = 3;
        h1.newCount = 2;
        h1.before = "old\n";
        h1.after = "new1\nnew2\n";
        h1.contextPre = "p1\np2\n";
        h1.contextPost = "q\n";
        DiffHunkInput h2;
        h2.oldStart = 20;
        h2.oldCount = 2;
        h2.newStart = 21;
        h2.newCount = 0;
        h2.before = "gone1\ngone2\n";
        h2.contextPre = "r\n";
        DiffRows rows;
        rows.Build({ h1, h2 });
        check(rows.HunkCount() == 2 && rows.HunkHeader(0) == "@@ -3,1 +3,2 @@" && rows.HunkHeader(1) == "@@ -20,2 +21,0 @@", "diff rows: hunk headers read '@@ -a,b +c,d @@'");
        const auto& in = rows.InlineRows();
        check(Joined(rows, in) == "HCC-++C" "HC--", "diff rows: inline = header, context, deletions then additions, context (two hunks)");
        check(in[1].oldNo == 1 && in[1].newNo == 1 && in[2].oldNo == 2 && in[2].newNo == 2 && in[3].oldNo == 3 && in[3].newNo == 0 && in[4].oldNo == 0 && in[4].newNo == 3
                  && in[5].newNo == 4 && in[6].oldNo == 4 && in[6].newNo == 5,
              "diff rows: line numbers (old / new) follow the hunk, with context numbered on both sides");
        check(rows.Text(in[3].text) == "old" && rows.Text(in[5].text) == "new2" && rows.Text(in[1].text) == "p1", "diff rows: line text has no line ending");
        const auto& split = rows.SplitRows();
        // h1: header, 2 文脈, max(1, 2) = 2 行, 1 文脈 = 6 行。h2: header, 1 文脈, 2 行 = 4 行
        check(split.size() == 10, "diff rows: side-by-side has max(deletions, additions) rows per hunk, so the sides stay aligned");
        check(split[3].left.kind == DiffLineKind::Delete && split[3].right.kind == DiffLineKind::Add && split[3].left.no == 3 && split[3].right.no == 3,
              "diff rows: a deletion and an addition at the same position share a row");
        check(split[4].left.kind == DiffLineKind::Empty && split[4].left.no == 0 && split[4].right.kind == DiffLineKind::Add && split[4].right.no == 4,
              "diff rows: an extra addition has an empty left side");
        check(split[8].left.kind == DiffLineKind::Delete && split[8].right.kind == DiffLineKind::Empty && split[9].right.kind == DiffLineKind::Empty,
              "diff rows: deletions only leave the right side empty");
        check(split[1].left.kind == DiffLineKind::Context && split[1].right.kind == DiffLineKind::Context && rows.Text(split[1].left.text) == rows.Text(split[1].right.text),
              "diff rows: context appears on both sides");
        bool aligned = true;
        for (const DiffSplitRow& r : split) {
            aligned = aligned && (r.kind == DiffRowKind::Header || r.left.kind != DiffLineKind::Empty || r.right.kind != DiffLineKind::Empty);
        }
        check(aligned, "diff rows: no side-by-side row is empty on both sides");
    }

    // 空の Before (追加)・空の After (削除)・Hunk 0 個
    {
        DiffHunkInput add;
        add.oldStart = 1;
        add.oldCount = 0;
        add.newStart = 1;
        add.newCount = 2;
        add.after = "x\ny\n";
        DiffRows rows;
        rows.Build({ add });
        check(Joined(rows, rows.InlineRows()) == "H++" && rows.SplitRows().size() == 3 && rows.SplitRows()[1].left.kind == DiffLineKind::Empty
                  && rows.SplitRows()[2].left.kind == DiffLineKind::Empty && rows.SplitRows()[2].right.no == 2,
              "diff rows: an empty Before (a new file) is all additions with an empty left side");
        DiffHunkInput del;
        del.oldStart = 1;
        del.oldCount = 2;
        del.newStart = 1;
        del.newCount = 0;
        del.before = "x\ny\n";
        rows.Build({ del });
        check(Joined(rows, rows.InlineRows()) == "H--" && rows.SplitRows()[1].right.kind == DiffLineKind::Empty && rows.SplitRows()[2].left.no == 2,
              "diff rows: an empty After (a deleted file) is all deletions with an empty right side");
        rows.Build({});
        check(rows.HunkCount() == 0 && rows.InlineRows().empty() && rows.SplitRows().empty() && rows.MaxColumns() == 0, "diff rows: zero hunks produce no rows");
    }

    // 改行だけの Hunk: 色は付けず、行末の変化を印にする
    {
        DiffHunkInput eol;
        eol.oldStart = 1;
        eol.oldCount = 2;
        eol.newStart = 1;
        eol.newCount = 2;
        eol.before = "a\r\nb\r\n";
        eol.after = "a\nb\n";
        eol.eolOnly = true;
        DiffRows rows;
        rows.Build({ eol });
        const auto& in = rows.InlineRows();
        check(rows.HunkEolOnly(0) && Joined(rows, in) == "H~~" && in[1].eol == DiffEol::CrLf && in[1].eolAfter == DiffEol::Lf && in[1].eolMark
                  && rows.Text(in[1].text) == "a" && in[1].oldNo == 1 && in[1].newNo == 1,
              "diff rows: an EOL-only hunk becomes one row per line (no add / delete), marked CRLF -> LF");
        const auto& split = rows.SplitRows();
        check(split.size() == 3 && split[1].left.kind == DiffLineKind::EolChange && split[1].right.kind == DiffLineKind::EolChange
                  && split[1].left.eol == DiffEol::CrLf && split[1].right.eol == DiffEol::Lf && rows.Text(split[1].left.text) == rows.Text(split[1].right.text),
              "diff rows: side by side shows both line endings of an EOL-only hunk");
    }

    // CRLF と LF の混在 (本文が変わった行): 行末の種類が変わった行にだけ印
    {
        DiffHunkInput mixed;
        mixed.oldStart = 1;
        mixed.oldCount = 3;
        mixed.newStart = 1;
        mixed.newCount = 3;
        mixed.before = "a\r\nb\nc\r\n";
        mixed.after = "A\nB\nc2\r\n";
        DiffRows rows;
        rows.Build({ mixed });
        const auto& in = rows.InlineRows();
        // 削除 3 行 (a CRLF / b LF / c CRLF) と追加 3 行 (A LF / B LF / c2 CRLF): 1 行目だけ行末が違う
        check(in[1].eolMark && !in[2].eolMark && !in[3].eolMark && in[4].eolMark && !in[5].eolMark && !in[6].eolMark,
              "diff rows: only the lines whose ending changed (CRLF <-> LF) are marked");
        check(in[1].eol == DiffEol::CrLf && in[4].eol == DiffEol::Lf, "diff rows: each side keeps its own ending");
        DiffHunkInput noEol;
        noEol.oldStart = 1;
        noEol.oldCount = 1;
        noEol.newStart = 1;
        noEol.newCount = 1;
        noEol.before = "tail";
        noEol.after = "tail2\n";
        rows.Build({ noEol });
        check(rows.InlineRows()[1].eol == DiffEol::None && rows.InlineRows()[2].eol == DiffEol::Lf && rows.InlineRows()[1].eolMark && rows.InlineRows()[2].eolMark,
              "diff rows: a last line without a line ending is told apart from one with LF");
    }

    // タブ・日本語・長い行
    {
        DiffHunkInput h;
        h.oldStart = 1;
        h.oldCount = 3;
        h.newStart = 1;
        h.newCount = 0;
        h.before = "\tx\nab\tc\nあ\tい\n";
        DiffRows rows;
        rows.Build({ h });
        const auto& in = rows.InlineRows();
        check(rows.Text(in[1].text) == "    x" && rows.Text(in[2].text) == "ab  c" && rows.Text(in[3].text) == "あ  い",
              "diff rows: tabs expand to the next 4-column stop (a full-width character counts as two columns)");
        check(rows.MaxColumns() == 6, "diff rows: the widest line sets the horizontal extent (full-width counts double)");

        DiffHunkInput longLine;
        longLine.oldStart = 1;
        longLine.oldCount = 1;
        longLine.before = std::string(10000, 'x') + "\n";
        rows.Build({ longLine });
        const std::string_view text = rows.Text(rows.InlineRows()[1].text);
        check(text.size() == DiffRows::kMaxLineBytes + 3 && text.substr(text.size() - 3) == "...", "diff rows: an over-long line is cut with an ellipsis");
        // 切れ目が UTF-8 の途中にならない
        DiffHunkInput jp;
        jp.oldStart = 1;
        jp.oldCount = 1;
        for (size_t i = 0; i < 3000; ++i) {
            jp.before += "あ";
        }
        jp.before += "\n";
        rows.Build({ jp });
        const std::string_view jpText = rows.Text(rows.InlineRows()[1].text);
        bool valid = jpText.size() > 3 && (jpText.size() - 3) % 3 == 0;
        check(valid, "diff rows: cutting a long Japanese line never splits a character");

        DiffHunkInput over;
        over.oldStart = 5;
        over.oldCount = 4;
        over.newStart = 5;
        over.newCount = 4;
        over.oversize = true;
        over.beforeBytes = 900000;
        over.afterBytes = 800000;
        rows.Build({ over });
        check(Joined(rows, rows.InlineRows()) == "HN" && rows.SplitRows().size() == 2 && rows.HunkOversize(0) && rows.HunkBeforeBytes(0) == 900000
                  && rows.HunkAfterBytes(0) == 800000,
              "diff rows: an oversize hunk is a header and a notice with its sizes (no body)");
    }

    // 選択
    {
        DiffViewer view;
        DiffHunkInput a;
        a.oldCount = 1;
        a.before = "x\n";
        view.SetHunks({ a, a, a });
        check(view.SelectedHunk() == -1 && view.Layout() == DiffLayout::Inline, "diff view: nothing is selected and the layout is inline at first");
        view.SelectHunk(2);
        check(view.SelectedHunk() == 2, "diff view: a hunk can be selected");
        view.SelectHunk(3);
        check(view.SelectedHunk() == -1, "diff view: an out-of-range selection clears it");
        view.SelectHunk(1);
        view.SetHunks({ a });
        check(view.SelectedHunk() == -1, "diff view: new input clears the selection");
        view.SetLayout(DiffLayout::Split);
        check(view.Layout() == DiffLayout::Split, "diff view: the layout is a state of the widget");
    }
}

// 1 万行の差分が、見えている範囲だけを描くこと。ヘッドレスの ImGui フレームを 1 つ回す
void TestVisibleRows(const Check& check)
{
    DiffHunkInput big;
    big.oldStart = 1;
    big.oldCount = 10000;
    big.newStart = 1;
    big.newCount = 10000;
    for (int i = 0; i < 10000; ++i) {
        big.before += "before line " + std::to_string(i) + "\n";
        big.after += "after line " + std::to_string(i) + "\n";
    }
    DiffViewer view;
    view.SetHunks({ big });
    check(view.Rows().InlineRows().size() == 20001 && view.Rows().SplitRows().size() == 10001, "visible rows: a 10000-line diff builds 20001 inline rows and 10001 side-by-side rows");

    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    // バックエンドは無いが、フォントの atlas は ImGui が自分で積む (テクスチャの要求を処理する描画側は無いまま、描画の呼び出しだけを数える)
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    int drawnInline = 0;
    int drawnSplit = 0;
    float lineHeight = 0.0f;
    for (const DiffLayout layout : { DiffLayout::Inline, DiffLayout::Split }) {
        view.SetLayout(layout);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(800.0f, 400.0f));
        ImGui::Begin("diff test");
        lineHeight = ImGui::GetTextLineHeight() + 2.0f;
        view.Draw("##diff", "empty", 0.0f, 300.0f);
        (layout == DiffLayout::Inline ? drawnInline : drawnSplit) = view.LastDrawnRows();
        ImGui::End();
        ImGui::EndFrame();
    }
    // 高さ 300 px の領域に収まる行数 + クリッパーが前後に足す数行
    const int limit = static_cast<int>(300.0f / lineHeight) + 6;
    check(drawnInline > 0 && drawnInline <= limit && drawnSplit > 0 && drawnSplit <= limit,
          "visible rows: one frame draws only the rows in view (about 300 px of rows), not the 20001 / 10001");

    // 空のときは、呼び手が渡した文面だけを出して行を描かない
    view.Clear();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f));
    ImGui::Begin("diff test 2");
    const bool changed = view.Draw("##diff", "no hunks here", 0.0f, 0.0f);
    const int drawnEmpty = view.LastDrawnRows();
    ImGui::End();
    ImGui::EndFrame();
    check(!changed && drawnEmpty == 0, "visible rows: with zero hunks nothing is drawn but the caller's message");

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}

} // namespace

void WithHeadlessImGuiFrame(const std::function<void()>& frame)
{
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::NewFrame();
    frame();
    ImGui::EndFrame();
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}

void RunChangeViewSelfTests(const std::function<void(bool, const char*)>& check)
{
    TestFormat(check);
    TestSections(check);
    TestRows(check);
    TestVisibleRows(check);
}

} // namespace mye
