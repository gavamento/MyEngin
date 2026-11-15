//====================================================================================
//                          RestoreSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の判定 (ゲート・競合・パス・プレビュー) の回帰テスト
//====================================================================================
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <Windows.h>
#include <winioctl.h>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/BlobHeadView.h"
#include "Editor/ChangeTracking/ChangeFormat.h"
#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/ChangeTrackingFakeIo.h"
#include "Editor/ChangeTracking/ChangeTrackingSelfTest.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/HunkRestore.h"
#include "Editor/ChangeTracking/PathGuard.h"
#include "Editor/ChangeTracking/RestoreFileIo.h"
#include "Editor/ChangeTracking/RestoreFormat.h"
#include "Editor/ChangeTracking/RestoreGate.h"
#include "Editor/ChangeTracking/RestorePlan.h"
#include "Editor/ChangeTracking/RestorePreview.h"
#include "Editor/ChangeTracking/ReviewManager.h"
#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Editor/Windows/Project/AiChangesWindow.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mye {
namespace {

using namespace tracktest;
using Check = std::function<void(bool, const char*)>;

std::string Hash(const std::string& bytes)
{
    return Sha256Hex(bytes);
}

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

// 固定表を探す (Editor の exe から親へ、カレントから親へ)
fs::path FindRepoFile(const fs::path& relative)
{
    const fs::path starts[] = { fs::path(GetExecutableDir()), fs::current_path() };
    for (fs::path dir : starts) {
        for (int up = 0; up < 8 && !dir.empty(); ++up) {
            std::error_code ec;
            if (fs::is_regular_file(dir / relative, ec)) {
                return dir / relative;
            }
            dir = dir.parent_path();
        }
    }
    return {};
}

// ---- 偽のファイル読み取り口 (読み取りだけ。書く関数は持たない) ----

class FakeFileIo final : public RestoreFileIo {
public:
    std::map<std::wstring, std::string> files;
    std::set<std::wstring> dirs;
    std::set<std::wstring> links;
    int attributeCalls = 0;
    int readCalls = 0;
    std::vector<size_t> readSizes;
    bool failReads = false;

    uint32_t Attributes(const std::wstring& abs) override
    {
        ++attributeCalls;
        if (links.count(abs) != 0) {
            return kRestoreAttrReparse;
        }
        if (dirs.count(abs) != 0) {
            return kRestoreAttrDirectory;
        }
        return files.count(abs) != 0 ? 0x80u : kRestoreAttrAbsent;
    }

    ReadStatus Read(const std::wstring& abs, uint64_t offset, size_t maxBytes, std::string& out, uint64_t& totalSize) override
    {
        ++readCalls;
        if (failReads) {
            return ReadStatus::Error;
        }
        const auto it = files.find(abs);
        if (it == files.end()) {
            return ReadStatus::Absent;
        }
        totalSize = it->second.size();
        const size_t from = static_cast<size_t>((std::min<uint64_t>)(offset, totalSize));
        const size_t count = (std::min<size_t>)(maxBytes, it->second.size() - from);
        out.append(it->second, from, count);
        readSizes.push_back(count);
        return ReadStatus::Ok;
    }

    int Calls() const { return attributeCalls + readCalls; }
};

const std::wstring kFakeRoot = L"C:\\mye_restore_fake\\proj";

std::unique_ptr<FakeFileIo> MakeFakeIo()
{
    auto io = std::make_unique<FakeFileIo>();
    io->dirs.insert(kFakeRoot);
    io->dirs.insert(kFakeRoot + L"\\assets");
    return io;
}

void PutFile(FakeFileIo& io, const std::string& rel, const std::string& content)
{
    io.files[JoinProjectPath(kFakeRoot, rel)] = content;
}

// ---- 計画の入力の組み立て ----

RestoreSide SideOf(const std::string& content, RestoreBlob state = RestoreBlob::Present)
{
    RestoreSide side;
    side.state = state;
    side.hash = Hash(content);
    side.size = static_cast<int64_t>(content.size());
    return side;
}

NowState FileNow(const std::string& content)
{
    NowState now;
    now.kind = NowKind::File;
    now.bytes = content;
    now.hash = Hash(content);
    now.size = static_cast<int64_t>(content.size());
    return now;
}

NowState NowOf(NowKind kind)
{
    NowState now;
    now.kind = kind;
    return now;
}

RestoreRevision Revision(const char* type, const std::string& before, const std::string& after, const char* actor = "ai", const char* mode = "lines")
{
    RestoreRevision rev;
    rev.revisionId = 5;
    rev.path = "assets/a.txt";
    rev.changeType = type;
    rev.actor = actor;
    rev.changeState = "active";
    rev.diffMode = mode;
    if (std::string(type) != "added") {
        rev.before = SideOf(before);
    }
    if (std::string(type) != "deleted") {
        rev.after = SideOf(after);
    }
    return rev;
}

RestoreInput FileInput(const RestoreRevision& rev, const NowState& now)
{
    RestoreInput in;
    in.scope = RestoreScope::File;
    in.revision = rev;
    in.now = now;
    return in;
}

bool NoWrites(const RestorePlan& plan)
{
    return plan.ops.empty();
}

// ---- (1) ファイル全体の判定 (純関数) ----
void TestFilePlans(const Check& check)
{
    const std::string before = "b\n";
    const std::string after = "a\n";
    const std::string other = "z\n";

    // 変更 (modified)
    {
        const RestoreRevision rev = Revision("modified", before, after);
        const RestorePlan ready = BuildRestorePlan(FileInput(rev, FileNow(after)));
        check(ready.verdict == RestoreVerdict::Ready && ready.ops.size() == 1 && ready.ops[0].kind == RestoreOp::Kind::Overwrite
                  && ready.ops[0].source == RestoreOp::Source::BeforeBlob && ready.ops[0].hash == Hash(before) && ready.ops[0].path == "assets/a.txt",
              "plan file: the current content equals the recorded After -> ready, one overwrite with the Before");
        const RestorePlan restored = BuildRestorePlan(FileInput(rev, FileNow(before)));
        check(restored.verdict == RestoreVerdict::AlreadyRestored && NoWrites(restored), "plan file: the current content equals the recorded Before -> already restored, nothing to write");
        const RestorePlan conflict = BuildRestorePlan(FileInput(rev, FileNow(other)));
        check(conflict.verdict == RestoreVerdict::Conflict && conflict.issue == RestoreIssue::FileChanged && NoWrites(conflict),
              "plan file: neither After nor Before -> conflict, and no write operation at all");
        const RestorePlan gone = BuildRestorePlan(FileInput(rev, NowOf(NowKind::Missing)));
        check(gone.verdict == RestoreVerdict::Conflict && NoWrites(gone), "plan file: a modified file that no longer exists is a conflict");
        // After の後で再編集された (要求 §16.4 の 3 番目): 末尾に 1 行足された / 改行だけ変わった
        const RestorePlan edited = BuildRestorePlan(FileInput(rev, FileNow(after + "more\n")));
        check(edited.verdict == RestoreVerdict::Conflict && NoWrites(edited), "plan file: a file edited after the change is a conflict and writes nothing");
        const RestorePlan eol = BuildRestorePlan(FileInput(rev, FileNow("a\r\n")));
        check(eol.verdict == RestoreVerdict::Conflict && NoWrites(eol), "plan file: bytes are compared, a CRLF difference is not ignored");
        // 後の変更に置き換わった変更 (superseded) も、内容で決まる
        RestoreRevision superseded = rev;
        superseded.changeState = "superseded";
        check(BuildRestorePlan(FileInput(superseded, FileNow(after))).verdict == RestoreVerdict::Ready, "plan file: a superseded change is judged by content, not by its state");
        // 行為者 unknown も対象
        RestoreRevision unknown = rev;
        unknown.actor = "unknown";
        check(BuildRestorePlan(FileInput(unknown, FileNow(after))).verdict == RestoreVerdict::Ready, "plan file: an unknown actor's change can be restored");
    }
    // 追加 (added) の取り消しは削除
    {
        const RestoreRevision rev = Revision("added", std::string(), after);
        const RestorePlan ready = BuildRestorePlan(FileInput(rev, FileNow(after)));
        check(ready.verdict == RestoreVerdict::Ready && ready.ops.size() == 1 && ready.ops[0].kind == RestoreOp::Kind::Delete && ready.ops[0].source == RestoreOp::Source::None,
              "plan file: undoing an added file is a delete, only when the content equals After");
        check(BuildRestorePlan(FileInput(rev, FileNow(other))).verdict == RestoreVerdict::Conflict && NoWrites(BuildRestorePlan(FileInput(rev, FileNow(other)))),
              "plan file: an added file edited since is a conflict (never deleted)");
        const RestorePlan gone = BuildRestorePlan(FileInput(rev, NowOf(NowKind::Missing)));
        check(gone.verdict == RestoreVerdict::AlreadyRestored && NoWrites(gone), "plan file: an added file that is already gone is already restored");
    }
    // 削除 (deleted) の取り消しは作成
    {
        const RestoreRevision rev = Revision("deleted", before, std::string());
        const RestorePlan ready = BuildRestorePlan(FileInput(rev, NowOf(NowKind::Missing)));
        check(ready.verdict == RestoreVerdict::Ready && ready.ops.size() == 1 && ready.ops[0].kind == RestoreOp::Kind::Create && ready.ops[0].hash == Hash(before),
              "plan file: undoing a deletion is a create, only when nothing is at the path");
        const RestorePlan occupied = BuildRestorePlan(FileInput(rev, FileNow(other)));
        check(occupied.verdict == RestoreVerdict::Conflict && occupied.issue == RestoreIssue::PathOccupied && NoWrites(occupied),
              "plan file: something at the path of a deleted file is a conflict");
        check(BuildRestorePlan(FileInput(rev, FileNow(before))).verdict == RestoreVerdict::AlreadyRestored, "plan file: a deleted file that is back with its Before content is already restored");
    }
    // 名前の変更 (renamed): 新しい名前の内容が After と一致し、かつ古い名前に何も無い
    {
        RestoreRevision rev = Revision("renamed", before, after);
        rev.path = "assets/new.txt";
        rev.oldPath = "assets/old.txt";
        RestoreInput in = FileInput(rev, FileNow(after));
        in.nowOld = NowOf(NowKind::Missing);
        const RestorePlan ready = BuildRestorePlan(in);
        check(ready.verdict == RestoreVerdict::Ready && ready.ops.size() == 2 && ready.ops[0].kind == RestoreOp::Kind::Rename && ready.ops[0].path == "assets/new.txt"
                  && ready.ops[0].toPath == "assets/old.txt" && ready.ops[1].kind == RestoreOp::Kind::Overwrite && ready.ops[1].path == "assets/old.txt",
              "plan file: a rename with edited content is undone by renaming back and restoring the content");
        RestoreRevision pure = rev;
        pure.before = SideOf(after);
        RestoreInput pureIn = FileInput(pure, FileNow(after));
        pureIn.nowOld = NowOf(NowKind::Missing);
        const RestorePlan pureReady = BuildRestorePlan(pureIn);
        check(pureReady.verdict == RestoreVerdict::Ready && pureReady.ops.size() == 1 && pureReady.ops[0].kind == RestoreOp::Kind::Rename, "plan file: a pure rename is only a rename");
        RestoreInput occupied = in;
        occupied.nowOld = FileNow(other);
        const RestorePlan blocked = BuildRestorePlan(occupied);
        check(blocked.verdict == RestoreVerdict::Conflict && blocked.issue == RestoreIssue::OldPathOccupied && NoWrites(blocked), "plan file: something under the old name is a conflict");
        RestoreInput changed = in;
        changed.now = FileNow(other);
        check(BuildRestorePlan(changed).verdict == RestoreVerdict::Conflict && NoWrites(BuildRestorePlan(changed)), "plan file: the renamed file edited since is a conflict");
        RestoreInput back = FileInput(rev, NowOf(NowKind::Missing));
        back.nowOld = FileNow(before);
        check(BuildRestorePlan(back).verdict == RestoreVerdict::AlreadyRestored, "plan file: a rename that was already undone is already restored");
    }
    // 戻せないもの
    {
        RestoreRevision noBefore = Revision("modified", before, after);
        noBefore.before = RestoreSide();
        const RestorePlan a = BuildRestorePlan(FileInput(noBefore, FileNow(after)));
        check(a.verdict == RestoreVerdict::NotRestorable && a.issue == RestoreIssue::NoBefore && NoWrites(a), "plan file: no_before (the content before was never seen) cannot be restored");
        for (const RestoreBlob state : { RestoreBlob::Missing, RestoreBlob::Excluded, RestoreBlob::Evicted, RestoreBlob::Unknown }) {
            RestoreRevision gone = Revision("modified", before, after);
            gone.before.state = state;
            const RestorePlan p = BuildRestorePlan(FileInput(gone, FileNow(after)));
            check(p.verdict == RestoreVerdict::NotRestorable && p.issue == RestoreIssue::BeforeUnavailable && NoWrites(p), "plan file: a Before blob that is missing / excluded / expired cannot be restored");
        }
        RestoreRevision afterUnknown = Revision("modified", before, after);
        afterUnknown.after.state = RestoreBlob::Excluded;
        afterUnknown.after.hash.clear();
        check(BuildRestorePlan(FileInput(afterUnknown, FileNow(after))).issue == RestoreIssue::AfterUnknown, "plan file: an excluded After cannot be compared");
        RestoreRevision afterBlobGone = Revision("modified", before, after);
        afterBlobGone.after.state = RestoreBlob::Missing; // ハッシュは残っている
        check(BuildRestorePlan(FileInput(afterBlobGone, FileNow(after))).verdict == RestoreVerdict::Ready, "plan file: a missing After blob is fine (the hash is enough to compare)");
        for (const char* actor : { "editor_user", "restore" }) {
            const RestorePlan p = BuildRestorePlan(FileInput(Revision("modified", before, after, actor), FileNow(after)));
            check(p.verdict == RestoreVerdict::NotRestorable && p.issue == RestoreIssue::ActorNotRestorable && NoWrites(p), "plan file: the editor user's own edits and restores are not restored here");
        }
        const RestoreRevision rev = Revision("modified", before, after);
        struct NowCase {
            NowKind kind;
            RestoreIssue issue;
        };
        const NowCase nowCases[] = { { NowKind::Directory, RestoreIssue::NowNotAFile }, { NowKind::Link, RestoreIssue::NowNotAFile },
                                     { NowKind::TooLarge, RestoreIssue::NowTooLarge }, { NowKind::Unreadable, RestoreIssue::NowUnreadable } };
        for (const NowCase& nowCase : nowCases) {
            const RestorePlan p = BuildRestorePlan(FileInput(rev, NowOf(nowCase.kind)));
            check(p.verdict == RestoreVerdict::NotRestorable && p.issue == nowCase.issue && NoWrites(p), "plan file: a folder / link / too large / unreadable current path is not judged");
        }
        RestoreInput rejected = FileInput(rev, FileNow(after));
        rejected.pathIssue = PathIssue::Traversal;
        const RestorePlan p = BuildRestorePlan(rejected);
        check(p.verdict == RestoreVerdict::NotRestorable && p.issue == RestoreIssue::PathRejected && NoWrites(p), "plan file: a rejected destination path never reaches a plan");
    }
    // バイナリ: ファイル全体だけ (ハッシュの一致で判定)
    {
        const RestoreRevision rev = Revision("modified", std::string("\0b\0", 3), std::string("\0a\0", 3), "ai", "binary");
        const RestorePlan ready = BuildRestorePlan(FileInput(rev, FileNow(std::string("\0a\0", 3))));
        check(ready.verdict == RestoreVerdict::Ready && ready.ops.size() == 1 && ready.ops[0].kind == RestoreOp::Kind::Overwrite, "plan file: a binary file is restored whole, judged by hash");
        RestoreInput hunks = FileInput(rev, FileNow(std::string("\0a\0", 3)));
        hunks.scope = RestoreScope::Hunks;
        TrackHunk h;
        h.hunkId = 1;
        hunks.hunks = { h };
        check(BuildRestorePlan(hunks).issue == RestoreIssue::HunkNeedsLines, "plan hunk: a binary file has no hunks to restore");
    }
}

// ---- (2) Hunk の判定と、固定表 (Rust と同じ結果) ----

HunkSpec SpecFrom(const json& h)
{
    HunkSpec spec;
    spec.oldStart = h["oldStart"].get<int64_t>();
    spec.oldCount = h["oldCount"].get<int64_t>();
    spec.newStart = h["newStart"].get<int64_t>();
    spec.newCount = h["newCount"].get<int64_t>();
    spec.beforeText = h["beforeText"].get<std::string>();
    spec.afterText = h["afterText"].get<std::string>();
    spec.contextPre = h["contextPre"].get<std::string>();
    spec.contextPost = h["contextPost"].get<std::string>();
    return spec;
}

TrackHunk TrackHunkFrom(const json& h, int64_t id, int64_t ordinal)
{
    TrackHunk hunk;
    hunk.hunkId = id;
    hunk.ordinal = ordinal;
    hunk.oldStart = h["oldStart"].get<int64_t>();
    hunk.oldCount = h["oldCount"].get<int64_t>();
    hunk.newStart = h["newStart"].get<int64_t>();
    hunk.newCount = h["newCount"].get<int64_t>();
    hunk.beforeText = h["beforeText"].get<std::string>();
    hunk.afterText = h["afterText"].get<std::string>();
    hunk.contextPre = h["contextPre"].get<std::string>();
    hunk.contextPost = h["contextPost"].get<std::string>();
    hunk.eolOnly = h.value("eolOnly", false);
    return hunk;
}

const char* FindName(HunkFind find)
{
    switch (find) {
    case HunkFind::Match: return "match";
    case HunkFind::Restored: return "restored";
    case HunkFind::Ambiguous: return "ambiguous";
    case HunkFind::NotFound: return "notfound";
    }
    return "?";
}

// 「After からその Hunk だけ Before に戻した内容」の別実装 (行の入れ替えだけ。探索も文脈の照合もしない)
std::string RevertOneByLines(const std::string& after, const HunkSpec& h)
{
    const std::vector<std::string_view> lines = SplitLinesKeepEol(after);
    std::string out;
    const size_t start = static_cast<size_t>(h.newStart - 1);
    const size_t count = static_cast<size_t>(h.newCount);
    for (size_t i = 0; i < start && i < lines.size(); ++i) {
        out.append(lines[i]);
    }
    out += h.beforeText;
    for (size_t i = start + count; i < lines.size(); ++i) {
        out.append(lines[i]);
    }
    return out;
}

void TestFixtureTable(const Check& check)
{
    const fs::path path = FindRepoFile(fs::path(L"tests") / L"track" / L"fixture" / L"hunk_apply_cases.json");
    check(!path.empty(), "table: tests/track/fixture/hunk_apply_cases.json is found (the same file the Rust test reads)");
    if (path.empty()) {
        return;
    }
    std::ifstream file(path, std::ios::binary);
    const json table = json::parse(file, nullptr, false);
    check(table.is_object() && table.contains("cases") && table["cases"].is_array() && table["cases"].size() >= 20, "table: the table parses and has at least 20 cases");
    if (!table.is_object() || !table.contains("cases")) {
        return;
    }
    std::set<std::string> names;
    size_t strictErrors = 0;
    for (const json& c : table["cases"]) {
        const std::string name = c["name"].get<std::string>();
        names.insert(name);
        std::vector<HunkSpec> all;
        for (const json& h : c["hunks"]) {
            all.push_back(SpecFrom(h));
        }
        const std::string after = c["after"].get<std::string>();
        const std::string before = c["before"].get<std::string>();
        const std::string current = c.contains("current") ? c["current"].get<std::string>() : after;
        std::vector<HunkSpec> selected;
        if (c.contains("select")) {
            for (const json& i : c["select"]) {
                selected.push_back(all[i.get<size_t>()]);
            }
        } else {
            selected = all;
        }

        // strict: サービスの unapply_hunks と同じ規則 (記録した位置だけ・復元済みを認めない・文脈の長さまで同じ)
        HunkApplyOptions strictOptions;
        strictOptions.allowShift = false;
        strictOptions.detectRestored = false;
        strictOptions.exactContextLength = true;
        const HunkApplyResult strict = UnapplyHunks(current, selected, strictOptions);
        bool ok = true;
        if (c["strict"].contains("result")) {
            ok = strict.Ok() && strict.bytes == c["strict"]["result"].get<std::string>();
        } else {
            ok = !strict.Ok();
            ++strictErrors;
        }
        // tolerant: Editor の判定
        const HunkApplyResult tolerant = UnapplyHunks(current, selected, HunkApplyOptions());
        const json& expect = c["tolerant"];
        const std::string verdict = !tolerant.Ok() ? "conflict" : (tolerant.changed ? "match" : "restored");
        ok = ok && verdict == expect["verdict"].get<std::string>();
        if (verdict == "match" && expect.contains("result")) {
            ok = ok && tolerant.bytes == expect["result"].get<std::string>();
        }
        if (expect.contains("hunks") && expect["hunks"].size() == selected.size() && tolerant.finds.size() == selected.size()) {
            for (size_t i = 0; i < selected.size(); ++i) {
                ok = ok && std::string(FindName(tolerant.finds[i].find)) == expect["hunks"][i]["find"].get<std::string>();
                if (tolerant.finds[i].find == HunkFind::Match) {
                    ok = ok && tolerant.finds[i].shiftLines == expect["hunks"][i]["shift"].get<int64_t>();
                }
            }
        } else {
            ok = false;
        }
        // 全部戻すと Before (After のとき) / 1 つだけ戻すと「After からその Hunk だけ Before へ」(別実装と比べる)
        if (current == after && !c.contains("select")) {
            ok = ok && tolerant.Ok() && tolerant.bytes == before;
        }
        if (current == after && all.size() >= 1) {
            for (size_t i = 0; i < all.size(); ++i) {
                const HunkApplyResult one = UnapplyHunks(current, { all[i] });
                ok = ok && one.Ok() && one.bytes == RevertOneByLines(after, all[i]);
            }
        }
        const std::string message = "table[" + name + "]: C++ strict / tolerant results equal the shared table";
        check(ok, message.c_str());
    }
    check(strictErrors >= 8, "table: the failing rows (shifted / conflict / restored) are in the table");
    for (const char* required : { "replace_one_line", "two_hunks_all", "shifted_unique", "ambiguous_two_places", "not_found", "zero_context_exact", "zero_context_no_shift",
                                  "crlf_lf_mixed", "no_trailing_newline", "japanese", "grow_then_replace_all", "eol_only", "already_restored", "one_conflict_cancels_all" }) {
        const std::string message = std::string("table: case ") + required + " is present";
        check(names.count(required) != 0, message.c_str());
    }
}

void TestHunkPlans(const Check& check)
{
    const fs::path path = FindRepoFile(fs::path(L"tests") / L"track" / L"fixture" / L"hunk_apply_cases.json");
    if (path.empty()) {
        check(false, "plan hunk: the table is needed");
        return;
    }
    std::ifstream file(path, std::ios::binary);
    const json table = json::parse(file, nullptr, false);
    auto find = [&](const char* name) -> const json& {
        for (const json& c : table["cases"]) {
            if (c["name"] == name) {
                return c;
            }
        }
        return table["cases"][0];
    };
    auto hunksOf = [](const json& c) {
        std::vector<TrackHunk> hunks;
        int64_t id = 20;
        for (const json& h : c["hunks"]) {
            hunks.push_back(TrackHunkFrom(h, ++id, static_cast<int64_t>(hunks.size())));
        }
        return hunks;
    };
    auto input = [&](const char* name, const std::string& current, const std::vector<size_t>& select = {}) {
        const json& c = find(name);
        RestoreInput in;
        in.scope = RestoreScope::Hunks;
        in.revision = Revision("modified", c["before"].get<std::string>(), c["after"].get<std::string>());
        const std::vector<TrackHunk> all = hunksOf(c);
        if (select.empty()) {
            in.hunks = all;
        } else {
            for (const size_t i : select) {
                in.hunks.push_back(all[i]);
            }
        }
        in.now = FileNow(current);
        return in;
    };

    const json& basic = find("replace_one_line");
    {
        const RestorePlan p = BuildRestorePlan(input("replace_one_line", basic["after"].get<std::string>()));
        check(p.verdict == RestoreVerdict::Ready && p.ops.size() == 1 && p.ops[0].source == RestoreOp::Source::Bytes && p.ops[0].bytes == basic["before"].get<std::string>()
                  && p.ops[0].hash == Hash(basic["before"].get<std::string>()) && p.hunks.size() == 1 && p.hunks[0].find == HunkFind::Match && p.hunks[0].shiftLines == 0,
              "plan hunk: a hunk found at its recorded position gives one overwrite with the rebuilt bytes");
        const std::string shifted = "p1\np2\n" + basic["after"].get<std::string>();
        const RestorePlan s = BuildRestorePlan(input("replace_one_line", shifted));
        check(s.verdict == RestoreVerdict::Ready && s.hunks[0].shiftLines == 2 && s.ops[0].bytes == "p1\np2\n" + basic["before"].get<std::string>(),
              "plan hunk: a unique match at another place is accepted, and the shift (2 lines) is reported");
        const std::string pat = std::string("a3\na4\na5\nA6\na7\na8\na9\n");
        const RestorePlan amb = BuildRestorePlan(input("replace_one_line", pat + "sep\n" + pat));
        check(amb.verdict == RestoreVerdict::Conflict && amb.issue == RestoreIssue::HunkAmbiguous && NoWrites(amb) && amb.hunks[0].find == HunkFind::Ambiguous,
              "plan hunk: two matches is a conflict and writes nothing");
        const RestorePlan none = BuildRestorePlan(input("replace_one_line", "unrelated\n"));
        check(none.verdict == RestoreVerdict::Conflict && none.issue == RestoreIssue::HunkNotFound && NoWrites(none), "plan hunk: no match is a conflict and writes nothing");
        const RestorePlan restored = BuildRestorePlan(input("replace_one_line", basic["before"].get<std::string>()));
        check(restored.verdict == RestoreVerdict::AlreadyRestored && NoWrites(restored) && restored.hunks[0].find == HunkFind::Restored, "plan hunk: a hunk already restored is reported as such");
        const RestorePlan gone = BuildRestorePlan([&] {
            RestoreInput in = input("replace_one_line", std::string());
            in.now = NowOf(NowKind::Missing);
            return in;
        }());
        check(gone.verdict == RestoreVerdict::Conflict && NoWrites(gone), "plan hunk: a file that no longer exists is a conflict");
    }
    {
        // 複数 Hunk: 1 つでも競合なら 1 つも当てない
        const json& two = find("two_hunks_all");
        const std::string after = two["after"].get<std::string>();
        const RestorePlan all = BuildRestorePlan(input("two_hunks_all", after));
        check(all.verdict == RestoreVerdict::Ready && all.ops[0].bytes == two["before"].get<std::string>() && all.hunks.size() == 2, "plan hunk: all hunks of a revision restore back to the Before");
        const RestorePlan first = BuildRestorePlan(input("two_hunks_all", after, { 0 }));
        check(first.verdict == RestoreVerdict::Ready && first.ops[0].bytes == find("two_hunks_first_only")["tolerant"]["result"].get<std::string>(), "plan hunk: one selected hunk is reverted and the other stays");
        std::string broken = after;
        broken.replace(broken.find("A11"), 3, "Q11");
        const RestorePlan oneBad = BuildRestorePlan(input("two_hunks_all", broken));
        check(oneBad.verdict == RestoreVerdict::Conflict && NoWrites(oneBad) && oneBad.hunks.size() == 2 && oneBad.hunks[0].find == HunkFind::Match && oneBad.hunks[1].find == HunkFind::NotFound,
              "plan hunk: when one hunk conflicts, none is applied (the other one still reports a match)");
        // 範囲が重なる (同じ Hunk を 2 回選んだ)
        RestoreInput twice = input("two_hunks_all", after, { 0, 0 });
        const RestorePlan overlap = BuildRestorePlan(twice);
        check(overlap.verdict == RestoreVerdict::Conflict && overlap.issue == RestoreIssue::HunkOverlap && NoWrites(overlap), "plan hunk: overlapping ranges are a conflict");
    }
    {
        // 改行の種類と eolOnly
        const json& mixed = find("crlf_lf_mixed");
        const RestorePlan m = BuildRestorePlan(input("crlf_lf_mixed", mixed["after"].get<std::string>()));
        check(m.verdict == RestoreVerdict::Ready && m.ops[0].bytes == mixed["before"].get<std::string>() && m.ops[0].bytes.find("l3\r\n") != std::string::npos, "plan hunk: the restored line keeps the Before line ending (CRLF)");
        const json& eol = find("eol_only");
        RestoreInput e = input("eol_only", eol["after"].get<std::string>());
        check(e.hunks[0].eolOnly && BuildRestorePlan(e).verdict == RestoreVerdict::Ready && BuildRestorePlan(e).ops[0].bytes == eol["before"].get<std::string>(), "plan hunk: an eol-only hunk follows the same rule");
        const json& zero = find("zero_context_no_shift");
        check(BuildRestorePlan(input("zero_context_no_shift", zero["current"].get<std::string>())).verdict == RestoreVerdict::Conflict, "plan hunk: a zero-context hunk does not accept a shifted match");
    }
    {
        // whole / oversize / 追加などは Hunk 単位では戻せない
        const json& c = find("replace_one_line");
        RestoreInput whole = input("replace_one_line", c["after"].get<std::string>());
        whole.revision.diffMode = "whole";
        check(BuildRestorePlan(whole).issue == RestoreIssue::HunkNeedsLines && NoWrites(BuildRestorePlan(whole)), "plan hunk: a whole-file diff cannot be restored hunk by hunk");
        RestoreInput big = input("replace_one_line", c["after"].get<std::string>());
        big.hunks[0].oversize = true;
        check(BuildRestorePlan(big).issue == RestoreIssue::HunkOversize, "plan hunk: an oversize hunk (body not kept) cannot be restored on its own");
        RestoreInput added = input("replace_one_line", c["after"].get<std::string>());
        added.revision = Revision("added", std::string(), c["after"].get<std::string>());
        check(BuildRestorePlan(added).issue == RestoreIssue::HunkNeedsModified, "plan hunk: only a modified file is restored hunk by hunk");
        RestoreInput none = input("replace_one_line", c["after"].get<std::string>());
        none.hunks.clear();
        check(BuildRestorePlan(none).issue == RestoreIssue::HunkNoneSelected, "plan hunk: no selected hunk is refused");
    }
    {
        // 衝突の見せ方: 失敗した位置の今の内容と、記録した After
        const json& c = find("replace_one_line");
        const std::string current = "a1\na2\na3\na4\na5\nZ6\na7\na8\na9\n";
        const RestorePlan p = BuildRestorePlan(input("replace_one_line", current));
        const std::vector<DiffHunkInput> view = ConflictHunkView(current, hunksOf(c), p.hunks);
        check(view.size() == 1 && view[0].before == "Z6\n" && view[0].after == "A6\n" && view[0].oldStart == 6, "plan hunk: the conflict view shows the current lines and the recorded After at the failing position");
        const std::vector<DiffHunkInput> reverse = ReverseHunkView(hunksOf(c));
        check(reverse.size() == 1 && reverse[0].before == "A6\n" && reverse[0].after == "a6\n", "plan hunk: the preview diff is the hunk reversed (After -> Before)");
    }
}

// ---- (3) ゲート ----

void TestGate(const Check& check)
{
    auto base = [] {
        RestoreGateInput in;
        in.trackConnected = true;
        in.targetPaths = { "assets/a.txt" };
        return in;
    };
    auto only = [&](GateBlocker gate) {
        const std::vector<RestoreBlock> blocks = EvaluateRestoreGate([&] {
            RestoreGateInput in = base();
            switch (gate) {
            case GateBlocker::Playing: in.gate.playing = true; break;
            case GateBlocker::NetActive: in.gate.netActive = true; break;
            case GateBlocker::BuildRunning: in.gate.buildRunning = true; break;
            case GateBlocker::ScriptBuildRunning: in.gate.scriptBuildRunning = true; break;
            case GateBlocker::OpInFlight: in.gate.opInFlight = true; break;
            case GateBlocker::MergeInProgress: in.gate.mergeInProgress = true; break;
            case GateBlocker::ServiceUnavailable: in.gate.serviceUnavailable = true; break;
            default: break;
            }
            return in;
        }());
        return blocks;
    };
    for (const GateBlocker gate : { GateBlocker::Playing, GateBlocker::NetActive, GateBlocker::BuildRunning, GateBlocker::ScriptBuildRunning, GateBlocker::OpInFlight, GateBlocker::MergeInProgress }) {
        const std::vector<RestoreBlock> blocks = only(gate);
        check(blocks.size() == 1 && blocks[0].kind == RestoreBlockKind::Gate && blocks[0].gate == gate, "gate: Playing / NetActive / BuildRunning / ScriptBuildRunning / OpInFlight / MergeInProgress always block");
    }
    check(only(GateBlocker::ServiceUnavailable).empty(), "gate: ServiceUnavailable (collab / git) does not block a restore");
    check(EvaluateRestoreGate(base()).empty(), "gate: nothing raised -> nothing blocks");

    // SceneDirty: 対象に開いているシーンのファイルが含まれるときだけ
    {
        RestoreGateInput in = base();
        in.gate.sceneDirty = true;
        in.documents.openScene = "Assets/A.TXT"; // 大文字小文字を区別しない
        const std::vector<RestoreBlock> blocks = EvaluateRestoreGate(in);
        check(blocks.size() == 1 && blocks[0].gate == GateBlocker::SceneDirty && blocks[0].path == "assets/a.txt", "gate: SceneDirty blocks when the target is the open scene (case-insensitive) and names the path");
        in.documents.openScene = "assets/other.scene.json";
        check(EvaluateRestoreGate(in).empty(), "gate: SceneDirty does not block a target that is not the open scene");
        in.documents.openScene.clear();
        check(EvaluateRestoreGate(in).empty(), "gate: SceneDirty with a not-yet-saved new scene (no path) does not block");
        in.targetPaths = { "assets/old.txt", "assets/other.scene.json" };
        in.documents.openScene = "assets/other.scene.json";
        check(EvaluateRestoreGate(in).size() == 1, "gate: SceneDirty blocks when any of several targets (a rename's old name) is the open scene");
    }
    // 各窓の未保存と ActorEdit: 文書が分かる窓は対象がその文書のときだけ、分からない窓は安全側で止める
    struct WindowCase {
        GateBlocker gate;
        void (*raise)(GateInputs&);
        EditedDocument EditedDocuments::*member;
    };
    const WindowCase windows[] = {
        { GateBlocker::ActorEdit, [](GateInputs& g) { g.actorEdit = true; }, &EditedDocuments::actor },
        { GateBlocker::AnimationDirty, [](GateInputs& g) { g.animationDirty = true; }, &EditedDocuments::animation },
        { GateBlocker::ControllerDirty, [](GateInputs& g) { g.controllerDirty = true; }, &EditedDocuments::controller },
        { GateBlocker::MixerDirty, [](GateInputs& g) { g.mixerDirty = true; }, &EditedDocuments::mixer },
        { GateBlocker::BehaviorTreeDirty, [](GateInputs& g) { g.behaviorTreeDirty = true; }, &EditedDocuments::behaviorTree },
        { GateBlocker::ProjectSettingsDirty, [](GateInputs& g) { g.projectSettingsDirty = true; }, &EditedDocuments::projectSettings },
        { GateBlocker::SpriteEditorDirty, [](GateInputs& g) { g.spriteEditorDirty = true; }, &EditedDocuments::spriteEditor },
        { GateBlocker::TilePaletteDirty, [](GateInputs& g) { g.tilePaletteDirty = true; }, &EditedDocuments::tilePalette },
    };
    bool unknownBlocks = true;
    bool knownSame = true;
    bool knownOther = true;
    for (const WindowCase& w : windows) {
        RestoreGateInput in = base();
        w.raise(in.gate);
        // 文書を教えられない窓 (known = false): 対象に関係なく止める
        std::vector<RestoreBlock> blocks = EvaluateRestoreGate(in);
        unknownBlocks = unknownBlocks && blocks.size() == 1 && blocks[0].gate == w.gate && blocks[0].path.empty();
        // 文書が分かる窓: 対象がその文書のときだけ止める
        (in.documents.*w.member).known = true;
        (in.documents.*w.member).paths = { "assets/other.json" };
        knownOther = knownOther && EvaluateRestoreGate(in).empty();
        (in.documents.*w.member).paths = { "assets/other.json", "assets/A.txt" };
        blocks = EvaluateRestoreGate(in);
        knownSame = knownSame && blocks.size() == 1 && blocks[0].gate == w.gate && blocks[0].path == "assets/a.txt";
    }
    check(unknownBlocks, "gate: a window that cannot say which document it edits blocks whenever its Dirty flag is up (safe side)");
    check(knownOther, "gate: a window that names its document does not block a different target (ActorEdit / Animation / Animator / Mixer / BT / Project Settings / Sprite / Tile Palette)");
    check(knownSame, "gate: ... and blocks the target that is its document");

    // 追跡に固有の条件
    {
        RestoreGateInput in = base();
        in.trackConnected = false;
        const std::vector<RestoreBlock> a = EvaluateRestoreGate(in);
        check(a.size() == 1 && a[0].kind == RestoreBlockKind::TrackUnavailable, "gate: the tracking service not connected blocks (the Before content cannot be fetched)");
        in = base();
        in.journalPending = true;
        const std::vector<RestoreBlock> b = EvaluateRestoreGate(in);
        check(b.size() == 1 && b[0].kind == RestoreBlockKind::JournalPending, "gate: a pending restore journal blocks");
        in = base();
        in.blobMissing = true;
        const std::vector<RestoreBlock> c = EvaluateRestoreGate(in);
        check(c.size() == 1 && c[0].kind == RestoreBlockKind::BlobMissing, "gate: a missing blob blocks");
    }
    // 理由が 2 つ以上のとき全部並ぶ (1 件目で止めない)。ゲートの列挙順 → 追跡に固有の順
    {
        RestoreGateInput in = base();
        in.gate.playing = true;
        in.gate.netActive = true;
        in.gate.buildRunning = true;
        in.gate.scriptBuildRunning = true;
        in.gate.opInFlight = true;
        in.gate.mergeInProgress = true;
        in.gate.serviceUnavailable = true;
        in.gate.sceneDirty = true;
        in.gate.actorEdit = true;
        in.gate.animationDirty = true;
        in.gate.controllerDirty = true;
        in.gate.mixerDirty = true;
        in.gate.behaviorTreeDirty = true;
        in.gate.projectSettingsDirty = true;
        in.gate.spriteEditorDirty = true;
        in.gate.tilePaletteDirty = true;
        in.documents.openScene = "assets/a.txt";
        in.trackConnected = false;
        in.journalPending = true;
        in.blobMissing = true;
        const std::vector<RestoreBlock> all = EvaluateRestoreGate(in);
        // 15 のゲート (ServiceUnavailable は除く: 常に止める 6 + SceneDirty + ActorEdit + 窓 7) + 追跡の 3 つ
        check(all.size() == 18 && all.front().gate == GateBlocker::SceneDirty && all[all.size() - 3].kind == RestoreBlockKind::TrackUnavailable
                  && all[all.size() - 2].kind == RestoreBlockKind::JournalPending && all.back().kind == RestoreBlockKind::BlobMissing,
              "gate: every reason is listed together, in the gate's order and then the tracking ones (none stops the list)");
        std::set<int> kinds;
        for (const RestoreBlock& b : all) {
            kinds.insert(b.kind == RestoreBlockKind::Gate ? static_cast<int>(b.gate) : 100 + static_cast<int>(b.kind));
        }
        check(kinds.size() == all.size(), "gate: no reason is listed twice");
    }
}

// ---- (4) パスの検査 ----

bool MakeJunction(const fs::path& link, const fs::path& target)
{
    std::error_code ec;
    fs::create_directories(link, ec);
    HANDLE dir = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::wstring print = fs::absolute(target).wstring();
    const std::wstring substitute = L"\\??\\" + print;
    const uint16_t substituteBytes = static_cast<uint16_t>(substitute.size() * sizeof(wchar_t));
    const uint16_t printBytes = static_cast<uint16_t>(print.size() * sizeof(wchar_t));
    const uint16_t dataLength = static_cast<uint16_t>(8 + substituteBytes + 2 + printBytes + 2);
    std::vector<char> buffer(8u + dataLength, 0);
    const uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(buffer.data(), &tag, 4);
    std::memcpy(buffer.data() + 4, &dataLength, 2);
    const uint16_t fields[4] = { 0, substituteBytes, static_cast<uint16_t>(substituteBytes + 2), printBytes };
    std::memcpy(buffer.data() + 8, fields, sizeof(fields));
    std::memcpy(buffer.data() + 16, substitute.c_str(), substituteBytes);
    std::memcpy(buffer.data() + 16 + substituteBytes + 2, print.c_str(), printBytes);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    CloseHandle(dir);
    return ok != FALSE;
}

void TestPathGuard(const Check& check)
{
    // 綴りだけの検査 (純粋)
    struct Row {
        const char* path;
        PathIssue issue;
    };
    const Row rows[] = {
        { "", PathIssue::Empty },
        { "a.txt", PathIssue::None },
        { "assets/a.txt", PathIssue::None },
        { "assets/日本語/ファイル.txt", PathIssue::None },
        { "assets/with space/a b.txt", PathIssue::None },
        { "a/b/c/d/e/f/g/h/i/j/k/l/m.txt", PathIssue::None },
        { "assets/.gitignore", PathIssue::None },
        { "assets/.github/workflows/x.yml", PathIssue::None },
        { "assets/file~backup.txt", PathIssue::None },
        { "COM0/x", PathIssue::None },
        { "COM10", PathIssue::None },
        { "../x", PathIssue::Traversal },
        { "a/../b", PathIssue::Traversal },
        { "a/./b", PathIssue::Traversal },
        { "..", PathIssue::Traversal },
        { "C:/x", PathIssue::Absolute },
        { "C:\\x", PathIssue::Absolute },
        { "c:x", PathIssue::Absolute },
        { "/x", PathIssue::Absolute },
        { "\\\\server\\share\\x", PathIssue::Absolute },
        { "//server/share/x", PathIssue::Absolute },
        { "a\\b", PathIssue::Separator },
        { "a//b", PathIssue::Separator },
        { "a/", PathIssue::Separator },
        { "CON", PathIssue::ReservedDevice },
        { "con.txt", PathIssue::ReservedDevice },
        { "NUL.txt", PathIssue::ReservedDevice },
        { "assets/COM1", PathIssue::ReservedDevice },
        { "assets/lpt9.log", PathIssue::ReservedDevice },
        { "assets/AUX", PathIssue::ReservedDevice },
        { "assets/PRN.tar.gz", PathIssue::ReservedDevice },
        { "a.txt.", PathIssue::TrailingDotOrSpace },
        { "a.txt ", PathIssue::TrailingDotOrSpace },
        { "assets/dir./a.txt", PathIssue::TrailingDotOrSpace },
        { "ab:c", PathIssue::Stream },
        { "a.txt:stream", PathIssue::Stream },
        { "a/b.txt::$DATA", PathIssue::Stream },
        { ".git/config", PathIssue::ProtectedDirectory },
        { "assets/.git/x", PathIssue::ProtectedDirectory },
        { ".GIT/x", PathIssue::ProtectedDirectory },
        { ".mye/ai_changes/x", PathIssue::ProtectedDirectory },
        { "assets/.mye", PathIssue::ProtectedDirectory },
        { "GIT~1/config", PathIssue::ProtectedDirectory },
        { "assets/a~1.txt", PathIssue::ProtectedDirectory }, // 8.3 の短い名前の形は、保護領域の疑いとして拒否 (保守的)
        { "a<b", PathIssue::InvalidChar },
        { "a|b", PathIssue::InvalidChar },
        { "a?b", PathIssue::InvalidChar },
        { "a*b", PathIssue::InvalidChar },
        { "a\"b", PathIssue::InvalidChar },
        { "a\tb", PathIssue::InvalidChar },
        { "a\nb", PathIssue::InvalidChar },
    };
    size_t wrong = 0;
    for (const Row& row : rows) {
        const PathIssue actual = CheckRelativePath(row.path);
        if (actual != row.issue) {
            ++wrong;
            MYE_LOG_ERROR("  pathguard: \"%s\" gave %d, wanted %d", row.path, static_cast<int>(actual), static_cast<int>(row.issue));
        }
    }
    check(wrong == 0, "pathguard: .. / absolute / UNC / drive / CON / NUL.txt / COM1 / trailing . and space / colon / .git / .mye / invalid chars are refused; Japanese, deep and spaced names pass");
    check(RestorePathKey("Assets\\A.TXT") == "assets/a.txt" && RestorePathKey("アセット/A") == "アセット/a", "pathguard: the comparison key ignores ASCII case and separators and keeps other characters");
    check(RelativeToProject(L"C:\\Proj\\", L"c:\\proj\\Assets\\a.txt") == "Assets/a.txt" && RelativeToProject(L"C:\\Proj", L"C:\\Other\\a.txt").empty()
              && RelativeToProject(L"C:\\Proj", L"C:\\Project\\a.txt").empty(),
          "pathguard: a path is made relative to the project only when it is inside it");

    // ファイルシステムを見る部分 (一時ディレクトリに実際にジャンクション・シンボリックリンクを作る)
    const fs::path root = fs::temp_directory_path() / (L"mye_restore_path_" + std::to_wstring(GetCurrentProcessId()));
    const fs::path outside = fs::temp_directory_path() / (L"mye_restore_outside_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
    fs::create_directories(root / L"assets", ec);
    fs::create_directories(outside, ec);
    {
        std::ofstream(root / L"assets" / L"file.txt", std::ios::binary) << "x";
        std::ofstream(outside / L"secret.txt", std::ios::binary) << "secret";
    }
    auto io = MakeSystemRestoreFileIo();
    const std::wstring rootW = root.wstring();
    check(CheckRestorePath(rootW, "assets/new.txt", *io) == PathIssue::None && CheckRestorePath(rootW, "assets/file.txt", *io) == PathIssue::None
              && CheckRestorePath(rootW, "newdir/sub/deep.txt", *io) == PathIssue::None,
          "pathguard disk: a new file, an existing file and a not-yet-existing folder chain pass");
    check(CheckRestorePath(rootW, "assets/file.txt/x", *io) == PathIssue::ParentNotDirectory, "pathguard disk: a file used as a parent folder is refused");
    check(CheckRestorePath((root / L"missing").wstring(), "a.txt", *io) == PathIssue::OutsideRoot, "pathguard disk: a project root that does not exist is refused");
    const bool junction = MakeJunction(root / L"assets" / L"junc", outside);
    check(junction, "pathguard disk: a junction can be created in the temp directory (needed for the next checks)");
    if (junction) {
        check(CheckRestorePath(rootW, "assets/junc/secret.txt", *io) == PathIssue::Link, "pathguard disk: a file behind a junction is refused");
        check(CheckRestorePath(rootW, "assets/junc/new/deeper.txt", *io) == PathIssue::Link, "pathguard disk: a not-yet-existing path behind a junction is refused (the parent is the link)");
        check(CheckRestorePath(rootW, "assets/junc", *io) == PathIssue::Link, "pathguard disk: the junction itself is refused");
    }
    const std::wstring symDir = (root / L"assets" / L"sym").wstring();
    const std::wstring symFile = (root / L"assets" / L"symfile.txt").wstring();
    const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    const bool dirLink = CreateSymbolicLinkW(symDir.c_str(), outside.c_str(), flags | SYMBOLIC_LINK_FLAG_DIRECTORY) != 0;
    const bool fileLink = CreateSymbolicLinkW(symFile.c_str(), (outside / L"secret.txt").c_str(), flags) != 0;
    if (dirLink) {
        check(CheckRestorePath(rootW, "assets/sym/secret.txt", *io) == PathIssue::Link, "pathguard disk: a file behind a directory symbolic link is refused");
    }
    if (fileLink) {
        check(CheckRestorePath(rootW, "assets/symfile.txt", *io) == PathIssue::Link, "pathguard disk: a file symbolic link as the destination is refused");
    }
    if (!dirLink || !fileLink) {
        MYE_LOG_INFO("  note: symbolic links could not be created here (needs developer mode / privilege); the junction rows cover the same reparse-point check");
        check(true, "pathguard disk: (symbolic-link rows skipped on this machine - the reparse-point check is the one the junction rows exercised)");
    }
    // 後始末 (自分が作ったものだけ。リンクはリンクとして外す = 指す先は消さない)
    if (junction) {
        RemoveDirectoryW((root / L"assets" / L"junc").c_str());
    }
    if (dirLink) {
        RemoveDirectoryW(symDir.c_str());
    }
    if (fileLink) {
        DeleteFileW(symFile.c_str());
    }
    check(fs::exists(outside / L"secret.txt", ec), "pathguard disk: removing the links left the folder they pointed at intact");
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
}

// ---- (5) 文面 (英日) ----

void TestFormat(const Check& check)
{
    InBothLanguages([&](Lang lang) {
        const bool en = lang == Lang::En;
        bool pathOk = true;
        for (const PathIssue issue : { PathIssue::Empty, PathIssue::Absolute, PathIssue::Traversal, PathIssue::Separator, PathIssue::InvalidChar, PathIssue::Stream, PathIssue::ReservedDevice,
                                       PathIssue::TrailingDotOrSpace, PathIssue::ProtectedDirectory, PathIssue::OutsideRoot, PathIssue::Link, PathIssue::ParentNotDirectory }) {
            pathOk = pathOk && !PathIssueText(issue).empty();
        }
        check(pathOk && PathIssueText(PathIssue::None).empty(), en ? "format (en): every path refusal has a reason" : "format (ja): every path refusal has a reason");
        bool issueOk = true;
        for (int i = 1; i <= static_cast<int>(RestoreIssue::HunkOverlap); ++i) {
            const std::string text = RestoreIssueText(static_cast<RestoreIssue>(i));
            issueOk = issueOk && !text.empty() && text.find('{') == std::string::npos;
        }
        check(issueOk && RestoreIssueText(RestoreIssue::None).empty(), en ? "format (en): every conflict / not-restorable reason has text" : "format (ja): every conflict / not-restorable reason has text");
        bool verdictOk = true;
        for (const RestoreVerdict v : { RestoreVerdict::Ready, RestoreVerdict::AlreadyRestored, RestoreVerdict::Conflict, RestoreVerdict::NotRestorable }) {
            verdictOk = verdictOk && !RestoreVerdictText(v).empty();
        }
        check(verdictOk, en ? "format (en): every verdict has text" : "format (ja): every verdict has text");
        bool gateOk = true;
        for (int g = 0; g < static_cast<int>(GateBlocker::Count); ++g) {
            RestoreBlock block;
            block.gate = static_cast<GateBlocker>(g);
            gateOk = gateOk && !RestoreBlockText(block).empty();
        }
        for (const RestoreBlockKind kind : { RestoreBlockKind::TrackUnavailable, RestoreBlockKind::JournalPending, RestoreBlockKind::BlobMissing }) {
            RestoreBlock block;
            block.kind = kind;
            gateOk = gateOk && !RestoreBlockText(block).empty();
        }
        RestoreBlock named;
        named.gate = GateBlocker::SceneDirty;
        named.path = "assets/a.scene.json";
        check(gateOk && RestoreBlockText(named).find("assets/a.scene.json") != std::string::npos, en ? "format (en): gate reasons have text and name the document" : "format (ja): gate reasons have text and name the document");

        RestoreOp overwrite;
        overwrite.kind = RestoreOp::Kind::Overwrite;
        overwrite.path = "assets/a.txt";
        overwrite.size = 2048;
        RestoreOp del;
        del.kind = RestoreOp::Kind::Delete;
        del.path = "assets/b.txt";
        RestoreOp create;
        create.kind = RestoreOp::Kind::Create;
        create.path = "assets/c.txt";
        create.size = 10;
        RestoreOp rename;
        rename.kind = RestoreOp::Kind::Rename;
        rename.path = "assets/new.txt";
        rename.toPath = "assets/old.txt";
        const std::string o = RestoreOpText(overwrite, false);
        const std::string h = RestoreOpText(overwrite, true);
        const std::string d = RestoreOpText(del, false);
        const std::string c = RestoreOpText(create, false);
        const std::string r = RestoreOpText(rename, false);
        check(o.find("assets/a.txt") != std::string::npos && o.find("2.0 KiB") != std::string::npos && h != o && o.find('{') == std::string::npos, en ? "format (en): an overwrite names the path and size" : "format (ja): an overwrite names the path and size");
        check(d.find("assets/b.txt") != std::string::npos && d.find(en ? "DELETE" : "削除") != std::string::npos, en ? "format (en): a deletion is spelled out" : "format (ja): a deletion is spelled out");
        check(c.find("assets/c.txt") != std::string::npos && r.find("assets/new.txt") != std::string::npos && r.find("assets/old.txt") != std::string::npos && r.find('{') == std::string::npos,
              en ? "format (en): create and rename name their paths" : "format (ja): create and rename name their paths");

        RestoreHunkOutcome out;
        out.hunkId = 12;
        out.find = HunkFind::Match;
        const std::string same = RestoreHunkOutcomeText(out);
        out.shiftLines = 3;
        const std::string down = RestoreHunkOutcomeText(out);
        out.shiftLines = -2;
        const std::string up = RestoreHunkOutcomeText(out);
        out.find = HunkFind::Restored;
        const std::string restored = RestoreHunkOutcomeText(out);
        out.find = HunkFind::Ambiguous;
        const std::string ambiguous = RestoreHunkOutcomeText(out);
        out.find = HunkFind::NotFound;
        const std::string missing = RestoreHunkOutcomeText(out);
        check(same.find("#12") != std::string::npos && down.find('3') != std::string::npos && up.find('2') != std::string::npos && down != up && same != down && restored != ambiguous && ambiguous != missing
                  && down.find('{') == std::string::npos,
              en ? "format (en): hunk outcomes (exact / shifted down / shifted up / restored / ambiguous / not found) are distinct and filled" : "format (ja): hunk outcomes are distinct and filled");

        RestoreNotes notes;
        notes.deletesFile = true;
        notes.referencesUnchecked = true;
        notes.shifted = true;
        notes.stagedPaths = { "assets/a.txt" };
        notes.stagedUnknown = true;
        notes.metaGuidUnchecked = true;
        notes.stage = ApplyStage::B;
        const std::vector<std::string> lines = RestoreNoteLines(notes);
        notes.stage = ApplyStage::C;
        const std::vector<std::string> linesC = RestoreNoteLines(notes);
        bool filled = lines.size() == 7 && linesC.size() == 7 && lines.back() != linesC.back();
        for (const std::string& line : lines) {
            filled = filled && !line.empty() && line.find('{') == std::string::npos;
        }
        check(filled && lines[3].find("assets/a.txt") != std::string::npos, en ? "format (en): notes (delete, references, shift, staged, unknown, .meta, rebuild) are filled" : "format (ja): notes are filled");
        check(RestoreNoteLines(RestoreNotes()).empty(), en ? "format (en): no notes when nothing applies" : "format (ja): no notes when nothing applies");
    });
    // 英日で文面が違う (表の取りこぼし = 片方の言語が英語のまま、の検出)
    const Lang previousLanguage = CurrentLanguage();
    SetLanguage(Lang::En);
    const std::string enIssue = RestoreIssueText(RestoreIssue::FileChanged);
    const std::string enBlock = RestoreBlockText([] {
        RestoreBlock b;
        b.kind = RestoreBlockKind::TrackUnavailable;
        return b;
    }());
    SetLanguage(Lang::Ja);
    const std::string jaIssue = RestoreIssueText(RestoreIssue::FileChanged);
    const std::string jaBlock = RestoreBlockText([] {
        RestoreBlock b;
        b.kind = RestoreBlockKind::TrackUnavailable;
        return b;
    }());
    SetLanguage(previousLanguage);
    check(enIssue != jaIssue && enBlock != jaBlock, "format: the English and Japanese texts differ");

    // 集約の 1 行と、先頭の表示の文面
    InBothLanguages([&](Lang) {
        const json group = { { "state", "partially_approved" }, { "counts", { { "pending", 1 }, { "approved", 2 }, { "rejected", 0 }, { "needsReview", 0 } } }, { "truncated", true } };
        const std::string text = ReviewGroupText(group);
        check(text.find(ReviewStateLabel("partially_approved")) != std::string::npos && text.find('2') != std::string::npos && ReviewGroupText(json()).empty(), "format: a review aggregate shows its state and counts");
        check(BlobHeadBinaryText(100).find("100 B") != std::string::npos && BlobHeadTruncatedText(8192, 1048576).find("8.0 KiB") != std::string::npos
                  && BlobHeadTruncatedText(8192, 1048576).find("1.0 MiB") != std::string::npos,
              "format: the head-of-blob notes show the sizes");
    });
}

// ---- (6) 注記 (E) ----

void TestNotes(const Check& check)
{
    auto plan = [](std::vector<RestoreOp> ops) {
        RestorePlan p;
        p.verdict = RestoreVerdict::Ready;
        p.ops = std::move(ops);
        return p;
    };
    auto op = [](RestoreOp::Kind kind, const char* path, const char* to = "") {
        RestoreOp o;
        o.kind = kind;
        o.path = path;
        o.toPath = to;
        return o;
    };
    const auto notStaged = [](const std::string&) { return StagedState::NotStaged; };
    {
        const RestoreNotes n = BuildRestoreNotes(plan({ op(RestoreOp::Kind::Delete, "assets/a.txt") }), "", notStaged);
        check(n.deletesFile && n.referencesUnchecked && n.stage == ApplyStage::A && n.stagedPaths.empty() && !n.stagedUnknown, "notes: a delete says so and that references were not checked");
    }
    {
        const RestoreNotes n = BuildRestoreNotes(plan({ op(RestoreOp::Kind::Overwrite, "src/GameLogic/Scripts/Foo.cpp") }), "", notStaged);
        check(n.stage == ApplyStage::B && !n.deletesFile, "notes: a C++ script needs a recompile (stage B)");
        check(BuildRestoreNotes(plan({ op(RestoreOp::Kind::Create, "assets/Scripts/Foo.cs") }), "", notStaged).stage == ApplyStage::B, "notes: a C# script needs a recompile (stage B)");
        check(BuildRestoreNotes(plan({ op(RestoreOp::Kind::Overwrite, "assets/schemas/thing.json") }), "", notStaged).stage == ApplyStage::C, "notes: a schema needs a restart (stage C)");
        check(BuildRestoreNotes(plan({ op(RestoreOp::Kind::Overwrite, "assets/a.scene.json") }), "assets/a.scene.json", notStaged).stage == ApplyStage::B, "notes: the open scene is reloaded (stage B)");
        check(BuildRestoreNotes(plan({ op(RestoreOp::Kind::Overwrite, "assets/tex.png") }), "", notStaged).stage == ApplyStage::A, "notes: a plain asset is swapped in place (stage A)");
        const RestoreNotes meta = BuildRestoreNotes(plan({ op(RestoreOp::Kind::Overwrite, "assets/tex.png.meta") }), "", notStaged);
        check(meta.metaGuidUnchecked && meta.stage == ApplyStage::A, "notes: a .meta says its guid change was not checked");
    }
    {
        const auto staged = [](const std::string& path) { return path == "assets/new.txt" ? StagedState::Staged : StagedState::NotStaged; };
        const RestoreNotes n = BuildRestoreNotes(plan({ op(RestoreOp::Kind::Rename, "assets/new.txt", "assets/old.txt") }), "", staged);
        check(n.stagedPaths == std::vector<std::string>{ "assets/new.txt" } && !n.stagedUnknown, "notes: a staged file is named (both names of a rename are looked up)");
        const RestoreNotes unknown = BuildRestoreNotes(plan({ op(RestoreOp::Kind::Overwrite, "assets/a.txt") }), "", {});
        check(unknown.stagedUnknown && unknown.stagedPaths.empty(), "notes: without a status source, 'staged or not' is unknown (never guessed)");
        RestorePlan shifted = plan({ op(RestoreOp::Kind::Overwrite, "assets/a.txt") });
        RestoreHunkOutcome o;
        o.find = HunkFind::Match;
        o.shiftLines = 4;
        shifted.hunks.push_back(o);
        check(BuildRestoreNotes(shifted, "", notStaged).shifted, "notes: a shifted hunk is noted");
        RestorePlan conflict = plan({ op(RestoreOp::Kind::Overwrite, "assets/a.txt") });
        conflict.verdict = RestoreVerdict::Conflict;
        const RestoreNotes none = BuildRestoreNotes(conflict, "", notStaged);
        check(!none.deletesFile && none.stagedPaths.empty() && !none.stagedUnknown, "notes: nothing is noted for a plan that will not run");
    }
}

// ---- (7) プレビュー (偽のサービス + 偽の読み取り口 + ヘッドレスの ImGui) ----

std::string Base64Encode(const std::string& bytes)
{
    static const char* const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8) | static_cast<uint8_t>(bytes[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = static_cast<uint8_t>(bytes[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

struct Rig {
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();
    ChangeTrackingSession session;
    ChangeModel& model = session.Model();
    size_t answered = 0;
    json listItems = json::array();
    json describeExtra = json::object();
    json hunkItems = json::array();
    std::map<std::string, std::string> blobs; // hash -> 中身 (blob_get が返す)
    bool blobsMissing = false;
    std::vector<json> operationPages;
    int reviewCalls = 0;

    Rig()
    {
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, true);
        session.Poll();
        ReplyHelloOk(*s);
        session.Poll();
        model.SetInterest(kTrackAllParts);
    }

    size_t Count(const char* op) const { return Requests(*s, op).size(); }

    void AnswerAll()
    {
        std::string batch;
        for (; answered < s->written.size(); ++answered) {
            const json req = json::parse(s->written[answered], nullptr, false);
            const std::string op = req.value("op", std::string());
            const uint64_t id = req["id"].get<uint64_t>();
            if (op == trackop::kHello) {
                continue;
            }
            if (op == trackop::kSubscribe) {
                batch += OkReply(id, json::object()).dump() + "\n";
                continue;
            }
            json result = json::object();
            if (op == trackop::kReviewSet) {
                ++reviewCalls;
                batch += OkReply(id, { { "recorded", req["args"]["targets"].size() } }).dump() + "\n";
                continue;
            }
            if (op == trackop::kBlobGet) {
                const json& args = req["args"];
                const std::string hash = args.value("hash", std::string());
                const auto it = blobs.find(hash);
                if (blobsMissing || it == blobs.end()) {
                    batch += ErrReply(id, "blob_missing").dump() + "\n";
                    continue;
                }
                const size_t offset = std::min<size_t>(args.value("offset", static_cast<size_t>(0)), it->second.size());
                const size_t length = std::min<size_t>(args.value("length", static_cast<size_t>(524288)), it->second.size() - offset);
                result = { { "data", Base64Encode(it->second.substr(offset, length)) }, { "size", it->second.size() }, { "offset", offset }, { "length", length } };
            } else if (op == trackop::kStatus) {
                result = { { "phase", "watching" }, { "db", { { "state", "recording" } } } };
            } else if (op == trackop::kSessions) {
                result = { { "items", json::array() } };
            } else if (op == trackop::kList) {
                const json& args = req["args"];
                if (args.contains("operationId")) {
                    size_t page = 0;
                    if (args.contains("cursor")) {
                        page = static_cast<size_t>(std::stoul(args["cursor"].get<std::string>()));
                    }
                    const bool more = page + 1 < operationPages.size();
                    result = { { "items", page < operationPages.size() ? operationPages[page] : json::array() }, { "nextCursor", more ? json(std::to_string(page + 1)) : json() } };
                } else {
                    result = { { "items", listItems }, { "nextCursor", nullptr } };
                }
            } else if (op == trackop::kDescribe) {
                result = describeExtra;
                result["revisionId"] = req["args"]["revisionId"];
            } else if (op == trackop::kHunks) {
                result = { { "items", hunkItems }, { "nextCursor", nullptr } };
            } else if (op == trackop::kSceneDiff) {
                result = { { "items", json::array() }, { "nextCursor", nullptr } };
            }
            batch += OkReply(id, result).dump() + "\n";
        }
        if (!batch.empty()) {
            s->chunks.push_back(batch);
        }
    }

    void Settle(int rounds = 6)
    {
        for (int i = 0; i < rounds; ++i) {
            s->nowMs += 1000;
            session.Poll();
            AnswerAll();
            session.Poll();
        }
    }
};

json SideJson(const std::string& content, const char* state = "present")
{
    return { { "hash", Hash(content) }, { "size", content.size() }, { "blobState", state } };
}

json DetailJson(const char* type, const std::string& before, const std::string& after, const char* path = "assets/a.txt", const char* actor = "ai")
{
    json d = { { "path", path }, { "changeType", type }, { "actor", actor }, { "attribution", "claimed" }, { "completeness", "complete" }, { "diffMode", "lines" }, { "changeState", "active" },
               { "reviewable", true }, { "review", "pending" },
               { "reviewCounts", { { "pending", 1 }, { "approved", 0 }, { "rejected", 0 }, { "needsReview", 0 } } },
               { "reviewLeaves", json::array({ { { "kind", "hunk" }, { "state", "pending" } } }) }, { "reviewHistory", json::array() },
               { "hunks", json::array({ { { "ordinal", 0 } } }) }, { "before", std::string(type) == "added" ? json() : SideJson(before) },
               { "after", std::string(type) == "deleted" ? json() : SideJson(after) } };
    return d;
}

json HunkJson(int64_t id, const std::string& beforeText, const std::string& afterText, int64_t start, const std::string& pre, const std::string& post)
{
    return { { "hunkId", id }, { "ordinal", 0 }, { "oldStart", start }, { "oldCount", 1 }, { "newStart", start }, { "newCount", 1 }, { "beforeText", beforeText }, { "afterText", afterText },
             { "contextPre", pre }, { "contextPost", post }, { "eolOnly", false }, { "state", "active" }, { "review", "pending" } };
}

const std::string kBeforeText = "a1\na2\na3\na4\na5\na6\na7\na8\na9\na10\na11\na12\n";
const std::string kAfterText = "a1\na2\na3\na4\na5\nA6\na7\na8\na9\na10\na11\na12\n";

// 基本の 1 変更 (a.txt の 6 行目の置き換え) を Rig へ仕込む
void SetupBasic(Rig& r, const std::string& currentContent, FakeFileIo& io)
{
    r.listItems = json::array({ { { "revisionId", 5 }, { "path", "assets/a.txt" }, { "changeType", "modified" }, { "actor", "ai" }, { "attribution", "claimed" }, { "review", "pending" } } });
    r.describeExtra = DetailJson("modified", kBeforeText, kAfterText);
    r.hunkItems = json::array({ HunkJson(21, "a6\n", "A6\n", 6, "a3\na4\na5\n", "a7\na8\na9\n") });
    r.blobs[Hash(kBeforeText)] = kBeforeText;
    r.blobs[Hash(kAfterText)] = kAfterText;
    PutFile(io, "assets/a.txt", currentContent);
    r.model.SelectRevision(5);
    r.Settle();
}

AiChangesHost MakeHost()
{
    AiChangesHost host;
    host.allowTracking = true;
    host.enabled = true;
    host.projectRoot = kFakeRoot;
    return host;
}

// 準備が終わる (または上限) まで、窓のフレームとサービスの応答を回す
void Drive(Rig& r, AiChangesWindow& window, const AiChangesHost& host, int maxFrames = 12)
{
    for (int i = 0; i < maxFrames && window.Restore().GetPhase() == RestorePreview::Phase::Preparing; ++i) {
        r.Settle(1);
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
    }
    for (int i = 0; i < 3; ++i) { // 画面を描かせる (ポップアップは開いた次のフレームから)
        r.Settle(1);
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
    }
}

void TestPreviewWindow(const Check& check)
{
    // (a) 一致: ファイル全体。止める理由なし・書くことになる内容・差分・実行は押せない
    {
        Rig r;
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        SetupBasic(r, kAfterText, *fake);
        AiChangesWindow window;
        window.open = true;
        window.SetRestoreFileIo(std::move(io));
        AiChangesHost host = MakeHost();
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        const size_t blobCallsBefore = r.Count(trackop::kBlobGet);
        check(window.OpenRestore(r.session, RestoreScope::File, host), "preview: the restore button opens a preview for the selected revision");
        check(window.Restore().GetPhase() == RestorePreview::Phase::Preparing, "preview: it is 'preparing' while the content is gathered (the frame is not blocked)");
        Drive(r, window, host);
        const RestorePreview& p = window.Restore();
        check(p.GetPhase() == RestorePreview::Phase::Ready && p.HasPlan() && p.Plan().verdict == RestoreVerdict::Ready && p.Plan().ops.size() == 1, "preview: a matching file gives a ready plan");
        const std::vector<json> blobCalls = Requests(*r.s, trackop::kBlobGet);
        check(r.Count(trackop::kBlobGet) == blobCallsBefore + 1 && blobCalls.back()["args"]["hash"] == Hash(kBeforeText) && blobCalls.back()["args"]["length"] == 1,
              "preview: the Before blob is checked with a one-byte blob_get (it exists)");
        const RestoreScreen& screen = window.LastRestoreScreen();
        check(screen.open && screen.hasPlan && screen.verdict == RestoreVerdict::Ready && screen.blocks.empty() && screen.ops.size() == 1 && screen.target == "assets/a.txt",
              "preview screen: the target, the verdict and the operation are shown, with no blocking reason");
        check(!screen.canApply && !screen.applyNote.empty(), "preview screen: the apply button cannot be pressed in this version (with a one-line note)");
        check(p.ChangeView().size() == 1 && p.ChangeView()[0].before == "A6\n" && p.ChangeView()[0].after == "a6\n", "preview: the diff shows the hunk reversed");
        check(window.GetDiffViewer().Rows().HunkCount() <= 1, "preview: opening the preview does not disturb the detail's diff");
        // 閉じても何も変わらない
        window.CloseRestore();
        check(window.Restore().GetPhase() == RestorePreview::Phase::Closed && !window.LastRestoreScreen().open && fake->files[JoinProjectPath(kFakeRoot, "assets/a.txt")] == kAfterText,
              "preview: closing it changes nothing (the file is as it was)");
    }
    // (b) 競合 (ファイル全体 / Hunk): 計画に書く操作が無い。今・Before・After のハッシュと、合わなくなった位置
    {
        Rig r;
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        const std::string edited = "a1\na2\na3\na4\na5\nZ6\na7\na8\na9\na10\na11\na12\n";
        SetupBasic(r, edited, *fake);
        AiChangesWindow window;
        window.open = true;
        window.SetRestoreFileIo(std::move(io));
        const AiChangesHost host = MakeHost();
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        window.OpenRestore(r.session, RestoreScope::File, host);
        Drive(r, window, host);
        const RestoreScreen& file = window.LastRestoreScreen();
        check(file.verdict == RestoreVerdict::Conflict && file.ops.empty() && !file.issues.empty() && !file.hashLine.empty() && window.Restore().Plan().ops.empty(),
              "preview screen: a file edited after the change shows a conflict, the three hashes and no operation");
        // Hunk: 選んだ Hunk の照合
        window.CloseRestore();
        window.SelectHunkIndex(r.model, 0);
        check(window.SelectedHunkId() == 21 && window.OpenRestore(r.session, RestoreScope::Hunks, host), "preview: the selected hunk can be restored on its own");
        Drive(r, window, host);
        const RestoreScreen& hunk = window.LastRestoreScreen();
        check(hunk.verdict == RestoreVerdict::Conflict && hunk.ops.empty() && hunk.hunkLines.size() == 1 && !window.Restore().ConflictView().empty() && window.Restore().ConflictView()[0].before == "Z6\n",
              "preview screen: a conflicting hunk shows its result and where it stopped matching (current -> recorded After)");
    }
    // (c) 位置ずれ: 一致 + 何行ずれたか
    {
        Rig r;
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        SetupBasic(r, "p1\np2\n" + kAfterText, *fake);
        AiChangesWindow window;
        window.open = true;
        window.SetRestoreFileIo(std::move(io));
        const AiChangesHost host = MakeHost();
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        window.SelectHunkIndex(r.model, 0);
        window.OpenRestore(r.session, RestoreScope::Hunks, host);
        Drive(r, window, host);
        const RestoreScreen& screen = window.LastRestoreScreen();
        check(screen.verdict == RestoreVerdict::Ready && screen.hunkLines.size() == 1 && window.Restore().Plan().hunks[0].shiftLines == 2 && screen.notes.size() >= 1,
              "preview screen: a hunk found 2 lines lower is ready, says how far it moved, and notes it");
    }
    // (d) ゲートで止まっている (理由が複数)
    {
        Rig r;
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        SetupBasic(r, kAfterText, *fake);
        AiChangesWindow window;
        window.open = true;
        window.SetRestoreFileIo(std::move(io));
        AiChangesHost host = MakeHost();
        GateInputs gate;
        host.gateInputs = [&gate] { return gate; };
        host.editedDocuments = [] {
            EditedDocuments docs;
            docs.openScene = "assets/a.txt";
            docs.actor.known = true;
            return docs;
        };
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        window.OpenRestore(r.session, RestoreScope::File, host);
        Drive(r, window, host);
        check(window.LastRestoreScreen().blocks.empty(), "preview gate: nothing raised -> no reason listed");
        // 確認の画面を出している間に状態が変わる: 毎フレーム判定し直す
        gate.playing = true;
        gate.sceneDirty = true;
        gate.buildRunning = true;
        gate.serviceUnavailable = true;
        for (int i = 0; i < 2; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        const RestoreScreen& screen = window.LastRestoreScreen();
        check(screen.blocks.size() == 3, "preview gate: Playing + SceneDirty (the target is the open scene) + BuildRunning are all listed; ServiceUnavailable is not");
        gate = GateInputs();
        for (int i = 0; i < 2; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        check(window.LastRestoreScreen().blocks.empty(), "preview gate: the list is judged again every frame (it clears when the state clears)");
        check(window.RestoreBlocks(r.session, host).empty(), "preview gate: RestoreBlocks gives the same answer to the code that will re-check right before writing");
    }
    // (e) Blob の欠け: 戻せない + ゲートに出る
    {
        Rig r;
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        SetupBasic(r, kAfterText, *fake);
        r.blobsMissing = true;
        AiChangesWindow window;
        window.open = true;
        window.SetRestoreFileIo(std::move(io));
        const AiChangesHost host = MakeHost();
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        window.OpenRestore(r.session, RestoreScope::File, host);
        Drive(r, window, host);
        const RestoreScreen& screen = window.LastRestoreScreen();
        check(window.Restore().BlobMissing() && screen.verdict == RestoreVerdict::NotRestorable && screen.ops.empty() && screen.blocks.size() == 1,
              "preview: a Before blob the service no longer has makes it not restorable and is listed as a reason");
    }
    // (f) 追跡が使えない: I/O の口を 1 回も呼ばない。理由は出る
    {
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        PutFile(*fake, "assets/a.txt", kAfterText);
        ChangeTrackingSession offline; // 起動していない (Disabled)
        RestorePreview preview;
        preview.Open(RestoreScope::File, DetailJson("modified", kBeforeText, kAfterText), {}, {}, kFakeRoot, offline.Client(), *fake);
        preview.Tick(*fake);
        check(preview.GetPhase() == RestorePreview::Phase::Ready && !preview.HasPlan() && !preview.Connected() && fake->Calls() == 0,
              "preview: with tracking off / not connected, no file I/O is called and no plan is made");
        AiChangesWindow window;
        window.open = true;
        const AiChangesHost host = MakeHost();
        check(!window.OpenRestore(offline, RestoreScope::File, host), "preview: with no revision detail (nothing to restore) the button does nothing");
        // 接続を失った窓: ゲートが理由を出す
        Rig r;
        auto io2 = MakeFakeIo();
        FakeFileIo* fake2 = io2.get();
        SetupBasic(r, kAfterText, *fake2);
        window.SetRestoreFileIo(std::move(io2));
        window.OpenRestore(r.session, RestoreScope::File, host);
        Drive(r, window, host);
        r.session.Client().Stop();
        const std::vector<RestoreBlock> blocks = window.RestoreBlocks(r.session, host);
        check(blocks.size() == 1 && blocks[0].kind == RestoreBlockKind::TrackUnavailable, "preview gate: losing the connection while the preview is open is listed as a reason");
    }
    // (g) 書き先の拒否: ファイルを読まない
    {
        Rig r;
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        SetupBasic(r, kAfterText, *fake);
        r.describeExtra["path"] = "../evil.txt";
        r.model.SelectRevision(0);
        r.model.SelectRevision(5);
        r.Settle();
        AiChangesWindow window;
        window.open = true;
        window.SetRestoreFileIo(std::move(io));
        const AiChangesHost host = MakeHost();
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        fake->readCalls = 0;
        window.OpenRestore(r.session, RestoreScope::File, host);
        Drive(r, window, host);
        const RestoreScreen& screen = window.LastRestoreScreen();
        check(screen.verdict == RestoreVerdict::NotRestorable && screen.issues.size() == 1 && fake->readCalls == 0 && screen.ops.empty(),
              "preview: a destination that escapes the project is refused before any file is read");
        // リンクの親 (偽の読み取り口に再解析ポイントを置く)
        Rig r2;
        auto io2 = MakeFakeIo();
        FakeFileIo* fake2 = io2.get();
        SetupBasic(r2, kAfterText, *fake2);
        fake2->links.insert(kFakeRoot + L"\\assets");
        AiChangesWindow window2;
        window2.open = true;
        window2.SetRestoreFileIo(std::move(io2));
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window2.OnImGui(r2.session, host); });
        }
        window2.OpenRestore(r2.session, RestoreScope::File, host);
        Drive(r2, window2, host);
        check(window2.Restore().Plan().issue == RestoreIssue::PathRejected && window2.Restore().Plan().pathIssue == PathIssue::Link, "preview: a parent folder that is a junction / symbolic link is refused");
    }
    // (h) 大きいファイルは数フレームに分けて読む。上限を超えるものは判定しない
    {
        Rig r;
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        SetupBasic(r, kAfterText, *fake);
        const std::string big(kRestoreReadChunkBytes * 5 / 2, 'x');
        r.describeExtra["after"] = SideJson(big);
        r.describeExtra["before"] = SideJson("small\n");
        r.blobs[Hash("small\n")] = "small\n";
        PutFile(*fake, "assets/a.txt", big);
        r.model.SelectRevision(0);
        r.model.SelectRevision(5);
        r.Settle();
        RestorePreview preview;
        preview.Open(RestoreScope::File, r.model.Detail(), r.model.Hunks(), {}, kFakeRoot, r.session.Client(), *fake);
        r.Settle(2);
        int ticks = 0;
        while (preview.GetPhase() == RestorePreview::Phase::Preparing && ticks < 10) {
            preview.Tick(*fake);
            ++ticks;
        }
        check(ticks == 3 && preview.GetPhase() == RestorePreview::Phase::Ready && fake->readSizes.size() == 3 && fake->readSizes[0] == kRestoreReadChunkBytes
                  && fake->readSizes[2] == big.size() - 2 * kRestoreReadChunkBytes,
              "preview: a 2.5 MiB file is read over 3 frames, 1 MiB at a time (the frame is not blocked)");
        check(preview.HasPlan() && preview.Plan().verdict == RestoreVerdict::Ready && preview.Now().hash == Hash(big), "preview: the plan uses the hash of the whole file read in pieces");

        PutFile(*fake, "assets/a.txt", std::string(kRestoreReadMaxBytes + 1, 'y'));
        fake->readCalls = 0;
        RestorePreview large;
        large.Open(RestoreScope::File, r.model.Detail(), r.model.Hunks(), {}, kFakeRoot, r.session.Client(), *fake);
        r.Settle(2);
        for (int i = 0; i < 5 && large.GetPhase() == RestorePreview::Phase::Preparing; ++i) {
            large.Tick(*fake);
        }
        check(large.GetPhase() == RestorePreview::Phase::Ready && large.Plan().issue == RestoreIssue::NowTooLarge && fake->readCalls == 1,
              "preview: a file over the 8 MiB limit is not judged, and only its size was looked at (one read)");
        fake->failReads = true;
        RestorePreview unreadable;
        unreadable.Open(RestoreScope::File, r.model.Detail(), r.model.Hunks(), {}, kFakeRoot, r.session.Client(), *fake);
        r.Settle(2);
        for (int i = 0; i < 3 && unreadable.GetPhase() == RestorePreview::Phase::Preparing; ++i) {
            unreadable.Tick(*fake);
        }
        check(unreadable.Plan().issue == RestoreIssue::NowUnreadable, "preview: a file that cannot be read is reported, not guessed");
    }
    // (i) 準備中に閉じる: 後から届いた応答は捨てる
    {
        Rig r;
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        SetupBasic(r, kAfterText, *fake);
        RestorePreview preview;
        preview.Open(RestoreScope::File, r.model.Detail(), r.model.Hunks(), {}, kFakeRoot, r.session.Client(), *fake);
        check(preview.GetPhase() == RestorePreview::Phase::Preparing, "preview: waiting for the blob check");
        preview.Close();
        r.Settle(3); // blob_get の応答が、閉じたあとに届く
        preview.Tick(*fake);
        check(preview.GetPhase() == RestorePreview::Phase::Closed && !preview.HasPlan(), "preview: a response that arrives after closing is ignored");
        // 開き直した別のプレビューに、古い応答が混ざらない
        preview.Open(RestoreScope::File, r.model.Detail(), r.model.Hunks(), {}, kFakeRoot, r.session.Client(), *fake);
        preview.Close();
        preview.Open(RestoreScope::File, r.model.Detail(), r.model.Hunks(), {}, kFakeRoot, r.session.Client(), *fake);
        r.Settle(3);
        for (int i = 0; i < 3 && preview.GetPhase() == RestorePreview::Phase::Preparing; ++i) {
            preview.Tick(*fake);
        }
        check(preview.GetPhase() == RestorePreview::Phase::Ready && preview.HasPlan(), "preview: re-opening after a close prepares normally");
    }
    // (j) サービスの失敗 (blob_get の時間切れなど): 準備に失敗した、と出す
    {
        Rig r;
        auto io = MakeFakeIo();
        FakeFileIo* fake = io.get();
        SetupBasic(r, kAfterText, *fake);
        RestorePreview preview;
        preview.Open(RestoreScope::File, r.model.Detail(), r.model.Hunks(), {}, kFakeRoot, r.session.Client(), *fake);
        r.session.Client().Stop(); // 保留中の要求は失敗で返る
        check(preview.GetPhase() == RestorePreview::Phase::Failed && !preview.HasPlan() && !preview.ErrorCode().empty(), "preview: a failed blob check (the connection dropped) is reported as a failure, not as a conflict");
        const RestoreScreen screen = BuildRestoreScreen(preview, {}, RestoreNotes());
        check(screen.phase == RestorePreview::Phase::Failed && !screen.failure.empty() && !screen.hasPlan, "preview screen: the failure reason is shown");
    }
}

// ---- (8) 作業ツリーを書かない: 本物の読み取り口で、一時プロジェクトの全ファイルが変わらない ----

std::map<std::string, std::string> SnapshotTree(const fs::path& root)
{
    std::map<std::string, std::string> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            std::ifstream f(it->path(), std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            files[WideToUtf8(fs::relative(it->path(), root, ec).wstring())] = Sha256Hex(bytes);
        } else if (it->is_directory(ec)) {
            files[WideToUtf8(fs::relative(it->path(), root, ec).wstring()) + "/"] = "dir";
        }
    }
    return files;
}

void TestNoWrites(const Check& check)
{
    const fs::path root = fs::temp_directory_path() / (L"mye_restore_nowrite_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"assets" / L"sub", ec);
    fs::create_directories(root / L"src", ec);
    const std::string edited = "a1\na2\na3\na4\na5\nZ6\na7\na8\na9\na10\na11\na12\n";
    {
        std::ofstream(root / L"assets" / L"a.txt", std::ios::binary) << kAfterText;
        std::ofstream(root / L"assets" / L"edited.txt", std::ios::binary) << edited;
        std::ofstream(root / L"assets" / L"sub" / L"日本語.txt", std::ios::binary) << "あ\n";
        std::ofstream(root / L"src" / L"main.cpp", std::ios::binary) << "int main() {}\n";
    }
    const std::map<std::string, std::string> before = SnapshotTree(root);
    check(before.size() >= 6, "no-write: the temp project has files to compare");

    Rig r;
    r.listItems = json::array({ { { "revisionId", 5 }, { "path", "assets/a.txt" }, { "changeType", "modified" }, { "actor", "ai" }, { "attribution", "claimed" }, { "review", "pending" } } });
    r.describeExtra = DetailJson("modified", kBeforeText, kAfterText);
    r.hunkItems = json::array({ HunkJson(21, "a6\n", "A6\n", 6, "a3\na4\na5\n", "a7\na8\na9\n") });
    r.blobs[Hash(kBeforeText)] = kBeforeText;
    r.model.SelectRevision(5);
    r.Settle();
    AiChangesWindow window;
    window.open = true; // 既定の (本物の) 読み取り口を使う
    AiChangesHost host;
    host.allowTracking = true;
    host.enabled = true;
    host.projectRoot = root.wstring();
    for (int i = 0; i < 3; ++i) {
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
    }
    bool allReady = true;
    for (const RestoreScope scope : { RestoreScope::File, RestoreScope::Hunks }) {
        window.CloseRestore();
        window.SelectHunkIndex(r.model, 0);
        allReady = allReady && window.OpenRestore(r.session, scope, host);
        Drive(r, window, host);
        allReady = allReady && window.Restore().GetPhase() == RestorePreview::Phase::Ready && window.Restore().Plan().verdict == RestoreVerdict::Ready;
    }
    check(allReady, "no-write: previews of the whole file and of a hunk, read from a real temp project, reach a ready plan");
    // 競合する対象 (edited.txt) も開いて閉じる
    r.describeExtra["path"] = "assets/edited.txt";
    r.model.SelectRevision(0);
    r.model.SelectRevision(5);
    r.Settle();
    window.CloseRestore();
    window.OpenRestore(r.session, RestoreScope::File, host);
    Drive(r, window, host);
    check(window.Restore().Plan().verdict == RestoreVerdict::Conflict, "no-write: a real file edited after the change is a conflict");
    window.CloseRestore();
    check(SnapshotTree(root) == before, "no-write: after creating, driving and closing previews, every file and folder of the temp project has the same path and hash");
    check(!fs::exists(root / L".mye", ec), "no-write: no .mye folder or other file was created in the project");

    // 本物の読み取り口が、塊ごとに読めること
    auto io = MakeSystemRestoreFileIo();
    std::string out;
    uint64_t total = 0;
    const std::wstring abs = JoinProjectPath(root.wstring(), "assets/a.txt");
    check(io->Read(abs, 0, 5, out, total) == RestoreFileIo::ReadStatus::Ok && out == kAfterText.substr(0, 5) && total == kAfterText.size(), "real io: reads the first chunk and reports the total size");
    check(io->Read(abs, 5, 7, out, total) == RestoreFileIo::ReadStatus::Ok && out == kAfterText.substr(0, 12), "real io: the next chunk continues from the offset");
    out.clear();
    check(io->Read(JoinProjectPath(root.wstring(), "assets/none.txt"), 0, 5, out, total) == RestoreFileIo::ReadStatus::Absent, "real io: a missing file is reported as absent");
    check((io->Attributes(JoinProjectPath(root.wstring(), "assets")) & kRestoreAttrDirectory) != 0 && io->Attributes(JoinProjectPath(root.wstring(), "assets/none.txt")) == kRestoreAttrAbsent,
          "real io: attributes tell a folder and a missing path apart");
    fs::remove_all(root, ec);
}

// ---- (9) 操作全体のレビュー・ファイル単位の集約・大きいファイルの先頭 (G) ----

void TestReviewAdditions(const Check& check)
{
    check(BlobHeadView::Base64Decode("TWFu", nullptr) == "Man" && BlobHeadView::Base64Decode("TWE=", nullptr) == "Ma" && BlobHeadView::Base64Decode("TQ==", nullptr) == "M" && BlobHeadView::Base64Decode("", nullptr).empty(),
          "head: base64 (with padding) decodes");
    bool ok = true;
    BlobHeadView::Base64Decode("T*Fu", &ok);
    check(!ok, "head: an invalid base64 character is reported");
    const std::string bytes = std::string("\0\1\2 plain \xE3\x81\x82", 14);
    check(BlobHeadView::Base64Decode(Base64Encode(bytes), nullptr) == bytes, "head: encode -> decode round-trips (binary and UTF-8)");

    AiChangesHost host = MakeHost();
    // 操作全体のまとめてレビュー: 操作の Revision を集め、件数つきの確認を 1 回
    {
        Rig r;
        r.listItems = json::array({ { { "revisionId", 5 }, { "path", "assets/a.txt" }, { "changeType", "modified" }, { "actor", "ai" }, { "attribution", "claimed" }, { "review", "pending" } } });
        r.describeExtra = DetailJson("modified", "b\n", "a\n");
        r.describeExtra["operationId"] = "op-1";
        r.describeExtra["changeReview"] = { { "state", "pending" }, { "counts", { { "pending", 1 }, { "approved", 0 }, { "rejected", 0 }, { "needsReview", 0 } } }, { "truncated", false } };
        r.describeExtra["operationReview"] = { { "state", "partially_approved" }, { "counts", { { "pending", 1 }, { "approved", 2 }, { "rejected", 0 }, { "needsReview", 0 } } }, { "truncated", false } };
        auto row = [](int64_t id, const char* actor) {
            const bool reviewable = std::string(actor) != "editor_user";
            return json{ { "revisionId", id }, { "path", "assets/x.txt" }, { "changeType", "modified" }, { "actor", actor }, { "attribution", "claimed" }, { "review", reviewable ? json("pending") : json() } };
        };
        r.operationPages = { json::array({ row(5, "ai"), row(6, "ai") }), json::array({ row(7, "unknown"), row(8, "editor_user") }) };
        r.model.SelectRevision(5);
        r.Settle();
        AiChangesWindow window;
        window.open = true;
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        check(r.model.Detail().contains("changeReview") && ReviewGroupText(r.model.Detail()["changeReview"]).find(ReviewStateLabel("pending")) != std::string::npos
                  && ReviewGroupText(r.model.Detail()["operationReview"]).find(ReviewStateLabel("partially_approved")) != std::string::npos,
              "aggregate: the detail carries the file's and the operation's aggregate, shown with state and counts");
        check(window.RequestBulkOperation(r.session, ReviewAction::Approve) && window.BulkConfirmPending(), "operation review: asks for a confirmation first");
        r.Settle(4);
        const std::vector<json> lists = Requests(*r.s, trackop::kList);
        const json& collect = lists.back()["args"];
        check(collect.contains("operationId") && collect["operationId"] == "op-1" && !collect.contains("sessionId"), "operation review: the operation's revisions are listed with the operationId filter");
        const ReviewManager& review = r.session.Review();
        check(review.GetCollect() == ReviewManager::Collect::Ready && review.PreparedRevisions() == std::vector<int64_t>({ 5, 6, 7 }) && review.PreparedSkipped() == 1 && review.PreparedOperationId() == "op-1",
              "operation review: every page is collected; only reviewable rows are counted (the editor user's row is skipped)");
        check(r.reviewCalls == 0, "operation review: nothing is recorded before the confirmation");
        for (int i = 0; i < 2; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        check(window.ConfirmBulk(r.session), "operation review: confirming sends the review");
        r.Settle(3);
        const json call = Requests(*r.s, trackop::kReviewSet).back()["args"];
        check(call["targets"].size() == 3 && call["state"] == "approved" && r.reviewCalls == 1, "operation review: one call records the whole operation (3 revisions)");
        // 操作に属さない・対象外の行為者
        r.describeExtra.erase("operationId");
        r.model.SelectRevision(0);
        r.model.SelectRevision(5);
        r.Settle();
        check(!window.RequestBulkOperation(r.session, ReviewAction::Approve), "operation review: a change that belongs to no operation offers nothing");
        r.describeExtra["operationId"] = "op-1";
        r.describeExtra["reviewable"] = false;
        r.model.SelectRevision(0);
        r.model.SelectRevision(5);
        r.Settle();
        check(!window.RequestBulkOperation(r.session, ReviewAction::Approve), "operation review: the editor user's own change offers nothing");
    }
    // 大きいファイル (whole + oversize) の先頭
    {
        Rig r;
        auto io = MakeFakeIo();
        const std::string beforeBig = std::string("before head\n") + std::string(20000, 'b');
        const std::string afterBin = std::string("\0\0binary", 8);
        r.listItems = json::array({ { { "revisionId", 5 }, { "path", "assets/big.txt" }, { "changeType", "modified" }, { "actor", "ai" }, { "attribution", "claimed" }, { "review", "pending" } } });
        r.describeExtra = DetailJson("modified", beforeBig, afterBin, "assets/big.txt");
        r.describeExtra["diffMode"] = "whole";
        r.hunkItems = json::array({ { { "hunkId", 9 }, { "ordinal", 0 }, { "oldStart", 1 }, { "oldCount", 5 }, { "newStart", 1 }, { "newCount", 5 }, { "oversize", true }, { "beforeBytes", 20012 }, { "afterBytes", 8 }, { "state", "active" } } });
        r.blobs[Hash(beforeBig)] = beforeBig;
        r.blobs[Hash(afterBin)] = afterBin;
        r.model.SelectRevision(5);
        r.Settle();
        AiChangesWindow window;
        window.open = true;
        window.SetRestoreFileIo(std::move(io));
        const size_t blobCalls = r.Count(trackop::kBlobGet);
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        const std::vector<json> calls = Requests(*r.s, trackop::kBlobGet);
        check(r.Count(trackop::kBlobGet) == blobCalls + 2 && calls.back()["args"]["length"] == kBlobHeadBytes && calls.back()["args"]["offset"] == 0,
              "head: a whole-file revision with oversize hunks asks for the first part of Before and After (blob_get, bounded)");
        r.Settle(3);
        for (int i = 0; i < 2; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        const BlobHeadView& heads = window.Heads();
        check(heads.Before().state == BlobHeadView::State::Ready && !heads.Before().binary && heads.Before().text == beforeBig.substr(0, static_cast<size_t>(kBlobHeadBytes))
                  && heads.Before().total == static_cast<int64_t>(beforeBig.size()) && heads.Before().total > heads.Before().shown,
              "head: the text start of Before arrives, with the total size so the cut can be noted");
        check(heads.After().state == BlobHeadView::State::Ready && heads.After().binary && heads.After().text.empty(), "head: binary content is not shown as text");
        const size_t after = r.Count(trackop::kBlobGet);
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        check(r.Count(trackop::kBlobGet) == after, "head: the same revision is not fetched again every frame");
        // 英日で描ける
        InBothLanguages([&](Lang) {
            for (int i = 0; i < 2; ++i) {
                WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
            }
        });
        check(true, "head: the detail draws in both languages");
    }
    // 通常の (行 diff の) 変更では先頭を取りに行かない
    {
        Rig r;
        auto io = MakeFakeIo();
        SetupBasic(r, kAfterText, *io);
        AiChangesWindow window;
        window.open = true;
        window.SetRestoreFileIo(std::move(io));
        const size_t blobCalls = r.Count(trackop::kBlobGet);
        for (int i = 0; i < 3; ++i) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
        }
        check(r.Count(trackop::kBlobGet) == blobCalls, "head: an ordinary line diff does not fetch the blob heads");
    }
}

// 復元の画面を英日で描く (落ちない) と、行為者が対象外のときボタンが効かない
void TestButtons(const Check& check)
{
    Rig r;
    auto io = MakeFakeIo();
    FakeFileIo* fake = io.get();
    SetupBasic(r, kAfterText, *fake);
    r.describeExtra["actor"] = "editor_user";
    r.describeExtra["reviewable"] = false;
    r.model.SelectRevision(0);
    r.model.SelectRevision(5);
    r.Settle();
    AiChangesWindow window;
    window.open = true;
    window.SetRestoreFileIo(std::move(io));
    const AiChangesHost host = MakeHost();
    for (int i = 0; i < 3; ++i) {
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
    }
    // 窓のボタンは無効 (行為者が editor_user)。口を直接呼んだ場合も、計画が「戻せない」にする
    window.OpenRestore(r.session, RestoreScope::File, host);
    Drive(r, window, host);
    check(window.Restore().Plan().issue == RestoreIssue::ActorNotRestorable && window.LastRestoreScreen().ops.empty(), "preview: an editor user's change is not restorable even if the entry point is called directly");
    window.CloseRestore();
    InBothLanguages([&](Lang) {
        window.OpenRestore(r.session, RestoreScope::File, host);
        Drive(r, window, host, 4);
        window.CloseRestore();
    });
    check(true, "preview: the preview draws in both languages");
}

} // namespace

void RunRestoreSelfTests(const std::function<void(bool, const char*)>& check)
{
    TestFilePlans(check);
    TestFixtureTable(check);
    TestHunkPlans(check);
    TestGate(check);
    TestPathGuard(check);
    TestFormat(check);
    TestNotes(check);
    TestPreviewWindow(check);
    TestNoWrites(check);
    TestReviewAdditions(check);
    TestButtons(check);
}

} // namespace mye
