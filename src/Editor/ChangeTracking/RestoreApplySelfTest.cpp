//====================================================================================
//                          RestoreApplySelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の実行 (ジャーナル・適用・巻き戻し・起動時の回復) の回帰テスト
//====================================================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <Windows.h>
#include <TlHelp32.h>
#include <winioctl.h>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/ChangeTrackingFakeIo.h"
#include "Editor/ChangeTracking/ChangeTrackingSelfTest.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/RestoreFormat.h"
#include "Editor/ChangeTracking/RestoreJournal.h"
#include "Editor/ChangeTracking/RestorePlan.h"
#include "Editor/ChangeTracking/RestorePreview.h"
#include "Editor/ChangeTracking/RestoreRunFormat.h"
#include "Editor/ChangeTracking/RestoreRunner.h"
#include "Editor/ChangeTracking/RestoreWriteIo.h"
#include "Editor/Windows/Project/AiChangesWindow.h"
#include "Editor/Windows/Project/RestoreRunWindow.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mye {
namespace {

using namespace tracktest;
using Check = std::function<void(bool, const char*)>;

template <typename F>
void InBothLanguagesForApply(F&& body)
{
    const Lang previous = CurrentLanguage();
    for (const Lang lang : { Lang::En, Lang::Ja }) {
        SetLanguage(lang);
        body(lang);
    }
    SetLanguage(previous);
}

std::string Hash(const std::string& bytes)
{
    return Sha256Hex(bytes);
}

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

// ---- 一時プロジェクト (本物のディレクトリ。このリポジトリの作業ツリーでは試さない) ----

struct Proj {
    fs::path root;

    explicit Proj(const std::string& name)
    {
        static int counter = 0;
        root = fs::temp_directory_path() / (L"mye_restore_apply_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++counter) + L"_" + std::wstring(name.begin(), name.end()));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / L"assets", ec);
    }
    ~Proj()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Proj(const Proj&) = delete;
    Proj& operator=(const Proj&) = delete;

    fs::path P(const std::string& rel) const { return root / fs::path(Utf8ToWide(rel)); }
    void Put(const std::string& rel, const std::string& content) const
    {
        std::error_code ec;
        fs::create_directories(P(rel).parent_path(), ec);
        std::ofstream(P(rel), std::ios::binary | std::ios::trunc) << content;
    }
    std::string Get(const std::string& rel) const
    {
        std::ifstream in(P(rel), std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
    bool Exists(const std::string& rel) const
    {
        std::error_code ec;
        return fs::exists(P(rel), ec);
    }
    std::wstring Root() const { return root.wstring(); }
};

// 一時プロジェクトの全ファイル (相対パス → ハッシュ)。skipMye = .mye (ジャーナル) を除く
std::map<std::string, std::string> Snapshot(const fs::path& root, bool skipMye)
{
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path rel = fs::relative(it->path(), root, ec);
        const std::string key = WideToUtf8(rel.generic_wstring());
        if (skipMye && (key == ".mye" || key.rfind(".mye/", 0) == 0)) {
            continue;
        }
        if (it->is_regular_file(ec)) {
            std::ifstream in(it->path(), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            out[key] = Hash(text.str());
        } else {
            out[key] = "<dir>";
        }
    }
    return out;
}

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

// ---- 失敗・割り込みを差し込める書き込み口 (本物の書き込み口を包む) ----

class FaultyWriteIo final : public RestoreWriteIo {
public:
    // 利用者のファイル (.mye の外) への操作の通し番号 (1 始まり)。この番号の操作を失敗させる / 例外を投げる。-1 = しない
    int failOnUserOp = -1;
    int throwOnUserOp = -1;
    // 利用者のファイルへの操作の直前に呼ぶ (他者の書き込みを差し込む)。引数は (通し番号, 操作の対象)
    std::function<void(int, const std::wstring&)> beforeUserOp;
    // ジャーナル (.mye の中) への書き込みの直前に呼ぶ。引数は書く内容 (Write のとき)
    std::function<void(const std::wstring&, std::string_view)> beforeJournalWrite;
    int failJournalWrites = 0; // 真の間、ジャーナルへの書き込みを失敗させる (数字は残り回数。-1 = ずっと)
    int userOps = 0;
    int journalWrites = 0;
    int removeTrees = 0;
    std::vector<std::wstring> userLog;

    bool Write(const std::wstring& abs, std::string_view bytes) override
    {
        if (Gate(abs, bytes)) {
            return false;
        }
        return inner_->Write(abs, bytes);
    }
    bool Append(const std::wstring& abs, uint64_t offset, std::string_view bytes) override
    {
        if (Gate(abs, bytes)) {
            return false;
        }
        return inner_->Append(abs, offset, bytes);
    }
    bool Remove(const std::wstring& abs) override
    {
        if (Gate(abs, {})) {
            return false;
        }
        return inner_->Remove(abs);
    }
    bool Rename(const std::wstring& from, const std::wstring& to, bool replace) override
    {
        if (Gate(to, {})) {
            return false;
        }
        return inner_->Rename(from, to, replace);
    }
    bool Copy(const std::wstring& from, const std::wstring& to) override
    {
        if (Gate(to, {})) {
            return false;
        }
        return inner_->Copy(from, to);
    }
    bool MakeDirs(const std::wstring& abs) override { return inner_->MakeDirs(abs); }
    bool RemoveTree(const std::wstring& abs) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++removeTrees;
        return inner_->RemoveTree(abs);
    }

private:
    static bool InJournal(const std::wstring& abs) { return abs.find(L"\\.mye\\") != std::wstring::npos; }

    // 真 = この操作を失敗させる
    bool Gate(const std::wstring& abs, std::string_view bytes)
    {
        int index = 0;
        bool inJournal = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inJournal = InJournal(abs);
            if (inJournal) {
                ++journalWrites;
            } else {
                index = ++userOps;
                userLog.push_back(abs);
            }
        }
        if (inJournal) {
            if (beforeJournalWrite) {
                beforeJournalWrite(abs, bytes);
            }
            if (failJournalWrites != 0) {
                if (failJournalWrites > 0) {
                    --failJournalWrites;
                }
                return true;
            }
            return false;
        }
        if (beforeUserOp) {
            beforeUserOp(index, abs);
        }
        if (index == throwOnUserOp) {
            throw std::runtime_error("injected exception");
        }
        return index == failOnUserOp;
    }

    std::unique_ptr<RestoreWriteIo> inner_ = MakeSystemRestoreWriteIo();
    std::mutex mutex_;
};

// ---- 偽のサービスと窓の道具 ----

struct ApplyRig {
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();
    ChangeTrackingSession session;
    Proj proj;
    FaultyWriteIo* faulty = nullptr;
    std::map<std::string, std::string> blobs;
    std::vector<json> recordArgs; // restore_record の引数 (届いた順)
    std::vector<std::string> order; // restore_expect / restore_record:<state> の届いた順
    size_t answered = 0;
    int begins = 0;
    int ends = 0;
    std::vector<std::vector<BatchChange>> endLists;
    bool failRecords = false;
    bool corruptBlobs = false;
    int64_t clockMs = 1'000'000;
    std::function<void()> onAnswer; // 応答を返す前に呼ぶ (切断などを差し込む)
    std::function<bool(const std::string&, const json&, json&)> modelAnswer; // list / describe など、復元以外の op の応答

    explicit ApplyRig(const std::string& name) : proj(name)
    {
        session.StartWithIo(std::make_unique<FakeIo>(s), proj.Root(), true);
        session.Poll();
        FeedJson(*s, OkReply(LastHelloId(*s), { { "proto", 1 }, { "projectId", ProjectId(proj.Root()) }, { "role", "editor" } }));
        session.Poll();
        auto write = std::make_unique<FaultyWriteIo>();
        faulty = write.get();
        RestoreRunner& runner = session.Restore();
        runner.SetIo(MakeSystemRestoreFileIo(), std::move(write));
        runner.SetClock([this]() { return clockMs; });
        RestoreRunHooks hooks;
        hooks.beginBatch = [this]() { ++begins; };
        hooks.endBatch = [this](const std::vector<BatchChange>& changes) {
            ++ends;
            endLists.push_back(changes);
        };
        runner.SetHooks(std::move(hooks));
    }

    RestoreRunner& Runner() { return session.Restore(); }

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
            if (onAnswer) {
                onAnswer();
            }
            json result = json::object();
            if (modelAnswer && modelAnswer(op, req, result)) {
                batch += OkReply(id, result).dump() + "\n";
                continue;
            }
            if (op == trackop::kRestoreExpect) {
                order.push_back("restore_expect");
            } else if (op == trackop::kRestoreRecord) {
                recordArgs.push_back(req["args"]);
                order.push_back("restore_record:" + req["args"].value("state", std::string()));
                if (failRecords) {
                    batch += ErrReply(id, "internal").dump() + "\n";
                    continue;
                }
            } else if (op == trackop::kRestoreList) {
                result = { { "items", json::array({ { { "restoreId", "r1" }, { "state", "committed" }, { "startedAt", 1700000000000ll }, { "targets", json::array({ { { "paths", json::array({ "assets/a.txt" }) } } }) } } }) } };
            } else if (op == trackop::kBlobGet) {
                const json& args = req["args"];
                const auto it = blobs.find(args.value("hash", std::string()));
                if (it == blobs.end()) {
                    batch += ErrReply(id, "blob_missing").dump() + "\n";
                    continue;
                }
                std::string content = it->second;
                if (corruptBlobs && !content.empty()) {
                    content[content.size() / 2] ^= 0x55;
                }
                const size_t offset = (std::min<size_t>)(args.value("offset", static_cast<size_t>(0)), content.size());
                const size_t length = (std::min<size_t>)(args.value("length", static_cast<size_t>(524288)), content.size() - offset);
                result = { { "data", Base64Encode(content.substr(offset, length)) }, { "size", content.size() }, { "offset", offset }, { "length", length } };
            } else if (op == trackop::kSubscribe) {
                result = json::object();
            }
            batch += OkReply(id, result).dump() + "\n";
        }
        if (!batch.empty()) {
            s->chunks.push_back(batch);
        }
    }

    void Round()
    {
        if (Runner().Busy()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 大きいファイルのハッシュ・複写 (別のスレッド) の完了を待つ
        }
        s->nowMs += 100;
        clockMs += 100;
        session.Poll();
        AnswerAll();
        session.Poll();
    }

    // 実行が終わる (結果が出る) まで。上限の回数を超えたら false
    bool RunToEnd(int maxRounds = 600)
    {
        for (int i = 0; i < maxRounds; ++i) {
            Round();
            if (!Runner().Busy()) {
                Round(); // 記録の送信の残りを流す
                return true;
            }
        }
        return false;
    }

    size_t Count(const char* op) const { return Requests(*s, op).size(); }
};

// ---- 変更の記述 ----

json Side(const std::string& content)
{
    return { { "hash", Hash(content) }, { "size", content.size() }, { "blobState", "present" } };
}

json Detail(const char* type, const std::string& before, const std::string& after, const std::string& path, int64_t revisionId = 5, const char* oldPath = nullptr)
{
    json d = { { "revisionId", revisionId }, { "path", path }, { "changeType", type }, { "actor", "ai" }, { "attribution", "claimed" }, { "completeness", "complete" },
               { "diffMode", "lines" }, { "changeState", "active" } };
    d["before"] = std::string(type) == "added" ? json() : Side(before);
    d["after"] = std::string(type) == "deleted" ? json() : Side(after);
    if (oldPath != nullptr) {
        d["oldPath"] = oldPath;
    }
    return d;
}

std::string Lines(int count, const std::map<int, std::string>& replaced = {})
{
    std::string text;
    for (int i = 1; i <= count; ++i) {
        const auto it = replaced.find(i);
        text += (it != replaced.end() ? it->second : "a" + std::to_string(i)) + "\n";
    }
    return text;
}

TrackHunk MakeHunk(int64_t id, int64_t ordinal, int64_t line, const std::string& before, const std::string& after, const std::string& pre, const std::string& post)
{
    TrackHunk h;
    h.hunkId = id;
    h.ordinal = ordinal;
    h.oldStart = line;
    h.oldCount = 1;
    h.newStart = line;
    h.newCount = 1;
    h.beforeText = before;
    h.afterText = after;
    h.contextPre = pre;
    h.contextPost = post;
    h.state = "active";
    return h;
}

struct Prepared {
    RestoreScope scope = RestoreScope::File;
    json detail;
    std::vector<TrackHunk> hunks;
    std::vector<int64_t> selected;
    RestorePlan plan;
    std::vector<std::string> targets;
};

// プレビューと同じ手順 (本物の読み取り口) で計画を作る
Prepared Prepare(ApplyRig& r, RestoreScope scope, const json& detail, std::vector<TrackHunk> hunks = {}, std::vector<int64_t> selected = {})
{
    Prepared p;
    p.scope = scope;
    p.detail = detail;
    p.hunks = std::move(hunks);
    p.selected = std::move(selected);
    auto io = MakeSystemRestoreFileIo();
    RestorePreview preview;
    preview.Open(scope, detail, p.hunks, p.selected, r.proj.Root(), r.session.Client(), *io);
    for (int i = 0; i < 4000 && preview.GetPhase() == RestorePreview::Phase::Preparing; ++i) {
        r.Round();
        preview.Tick(*io);
        if (preview.GetPhase() == RestorePreview::Phase::Preparing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 大きいファイルのハッシュ (別のスレッド)
        }
    }
    p.plan = preview.Plan();
    p.targets = preview.TargetPaths();
    return p;
}

RestoreRunner::StartRequest MakeRequest(const Prepared& p, std::function<std::vector<RestoreBlock>(bool)> blocks = {})
{
    RestoreRunner::StartRequest request;
    request.scope = p.scope;
    request.detail = p.detail;
    request.allHunks = p.hunks;
    request.selectedHunkIds = p.selected;
    request.previewOps = p.plan.ops;
    request.targetPaths = p.targets;
    request.blocks = blocks ? std::move(blocks) : [](bool) { return std::vector<RestoreBlock>(); };
    return request;
}

RestoreBlock PlayingBlock()
{
    RestoreBlock block;
    block.kind = RestoreBlockKind::Gate;
    block.gate = GateBlocker::Playing;
    return block;
}

std::vector<fs::path> JournalDirs(const Proj& proj)
{
    std::vector<fs::path> dirs;
    std::error_code ec;
    const fs::path base = proj.root / L".mye" / L"ai_changes" / L"recovery";
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        dirs.push_back(it->path());
    }
    return dirs;
}

std::string JournalStateOf(const fs::path& dir)
{
    std::ifstream in(dir / L"manifest.json", std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const json j = json::parse(text.str(), nullptr, false);
    return j.is_object() ? j.value("state", std::string()) : std::string();
}

// この Editor プロセスの子プロセスの名前 (git を起動していないことの確認に使う)
std::multiset<std::wstring> ChildProcessNames()
{
    std::multiset<std::wstring> names;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return names;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snap, &entry); more; more = Process32NextW(snap, &entry)) {
        if (entry.th32ParentProcessID == GetCurrentProcessId()) {
            names.insert(entry.szExeFile);
        }
    }
    CloseHandle(snap);
    return names;
}

bool Started(ApplyRig& r, const Prepared& p, std::function<std::vector<RestoreBlock>(bool)> blocks = {})
{
    return r.Runner().Start(MakeRequest(p, std::move(blocks)));
}

// ---- (1) 適用: ファイル全体・Hunk・追加の取り消し・削除の取り消し・名前の変更の取り消し ----

void TestApplyKinds(const Check& check)
{
    const std::string before = Lines(24);
    const std::string after = Lines(24, { { 2, "B2" }, { 22, "B22" } });
    const auto hunk1 = MakeHunk(11, 0, 2, "a2\n", "B2\n", "a1\n", "a3\na4\na5\n");
    const auto hunk2 = MakeHunk(12, 1, 22, "a22\n", "B22\n", "a19\na20\na21\n", "a23\na24\n");

    // (a) ファイル全体
    {
        ApplyRig r("whole");
        r.proj.Put("assets/a.txt", after);
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"), { hunk1, hunk2 });
        check(p.plan.verdict == RestoreVerdict::Ready, "apply: the plan of a whole-file restore is ready");
        check(Started(r, p) && r.Runner().Busy(), "apply: a ready plan starts");
        check(r.proj.Get("assets/a.txt") == after && r.begins == 0, "apply: nothing is written before the runner is driven (the preview wrote nothing)");
        check(r.RunToEnd() && r.Runner().Result().outcome == RestoreOutcome::Committed, "apply: the whole-file restore commits");
        check(r.proj.Get("assets/a.txt") == before, "apply: the file is byte-identical to the Before content");
        check(r.begins == 1 && r.ends == 1 && r.endLists.back().size() == 1 && r.endLists.back()[0].kind == BatchChange::Kind::Modified, "apply: one BeginBatch and one EndBatch, with the modified file");
        const std::vector<fs::path> dirs = JournalDirs(r.proj);
        check(dirs.size() == 1 && JournalStateOf(dirs[0]) == "committed", "journal: the journal of a finished restore says committed");
        check(fs::exists(dirs[0] / L"pre" / L"0001.bin") && r.proj.Exists(".mye/ai_changes/recovery/" + WideToUtf8(dirs[0].filename().wstring()) + "/pre/0001.bin"), "journal: the content from before the write is kept in pre\\0001.bin");
        const std::vector<std::string> expected = { "restore_expect", "restore_record:applying", "restore_record:committed" };
        std::vector<std::string> got = r.order;
        got.erase(std::remove(got.begin(), got.end(), std::string("restore_record:prepared")), got.end());
        check(got == expected, "service: restore_expect is sent before the first write, then the applying and committed records, in that order");
        check(r.Count(trackop::kEditorSaved) == 0, "service: the restore's own writes are never reported as editor saves");
        check(!r.recordArgs.empty() && r.recordArgs.back()["targets"][0]["revisionId"] == 5 && r.recordArgs.back()["postHashes"]["assets/a.txt"] == Hash(before),
              "service: the committed record carries the target and the hash of what was written");
        check(!r.Runner().Pending(), "apply: nothing is pending after a committed restore");
    }
    // (b) Hunk を 1 つだけ: After からその Hunk だけ Before に戻した内容とバイト一致
    {
        ApplyRig r("hunk");
        r.proj.Put("assets/a.txt", after);
        const Prepared p = Prepare(r, RestoreScope::Hunks, Detail("modified", before, after, "assets/a.txt"), { hunk1, hunk2 }, { 11 });
        check(p.plan.verdict == RestoreVerdict::Ready && p.plan.ops.size() == 1, "apply: the plan of one hunk is ready");
        check(Started(r, p) && r.RunToEnd() && r.Runner().Result().outcome == RestoreOutcome::Committed, "apply: restoring one hunk commits");
        const std::string expected = Lines(24, { { 22, "B22" } });
        check(r.proj.Get("assets/a.txt") == expected, "apply: only that hunk went back to Before; the file is byte-identical to After with that hunk reverted");
        check(r.Count(trackop::kBlobGet) >= 0 && r.begins == 1 && r.ends == 1, "apply: a hunk restore also runs inside one batch");
    }
    // (c) 追加の取り消し (削除)
    {
        ApplyRig r("added");
        r.proj.Put("assets/new/n.txt", "new file\n");
        const Prepared p = Prepare(r, RestoreScope::File, Detail("added", "", "new file\n", "assets/new/n.txt"));
        check(p.plan.verdict == RestoreVerdict::Ready && p.plan.ops[0].kind == RestoreOp::Kind::Delete, "apply: undoing an addition is a delete");
        check(Started(r, p) && r.RunToEnd() && r.Runner().Result().outcome == RestoreOutcome::Committed && !r.proj.Exists("assets/new/n.txt"), "apply: the added file is deleted");
        check(r.endLists.back().size() == 1 && r.endLists.back()[0].kind == BatchChange::Kind::Deleted, "apply: the batch reports the deleted file");
        check(r.proj.Exists("assets/new"), "apply: the folder that held the file is left alone");
    }
    // (d) 削除の取り消し (作成。親ディレクトリも作る)
    {
        ApplyRig r("deleted");
        r.blobs[Hash("gone\n")] = "gone\n";
        const Prepared p = Prepare(r, RestoreScope::File, Detail("deleted", "gone\n", "", "assets/deep/er/g.txt"));
        check(p.plan.verdict == RestoreVerdict::Ready && p.plan.ops[0].kind == RestoreOp::Kind::Create, "apply: undoing a deletion is a create");
        check(Started(r, p) && r.RunToEnd() && r.Runner().Result().outcome == RestoreOutcome::Committed && r.proj.Get("assets/deep/er/g.txt") == "gone\n", "apply: the deleted file is created again with the stored content");
        check(r.endLists.back()[0].kind == BatchChange::Kind::Added, "apply: the batch reports the created file");
    }
    // (e) 名前の変更の取り消し (内容も違う: 2 操作)
    {
        ApplyRig r("rename");
        r.proj.Put("assets/new_name.txt", "after body\n");
        r.blobs[Hash("before body\n")] = "before body\n";
        const Prepared p = Prepare(r, RestoreScope::File, Detail("renamed", "before body\n", "after body\n", "assets/new_name.txt", 5, "assets/old_name.txt"));
        check(p.plan.verdict == RestoreVerdict::Ready && p.plan.ops.size() == 2, "apply: undoing a rename with changed content is two operations");
        check(Started(r, p) && r.RunToEnd() && r.Runner().Result().outcome == RestoreOutcome::Committed, "apply: the rename is undone");
        check(!r.proj.Exists("assets/new_name.txt") && r.proj.Get("assets/old_name.txt") == "before body\n", "apply: the file is back under the old name with the Before content");
        check(r.endLists.back().size() == 2 && r.endLists.back()[0].kind == BatchChange::Kind::Renamed, "apply: the batch reports the rename and the content change");
    }
    // (f) 名前の変更の取り消し (内容は同じ: 1 操作。Blob を取らない)
    {
        ApplyRig r("rename_same");
        r.proj.Put("assets/new_name.txt", "same body\n");
        r.blobs[Hash("same body\n")] = "same body\n";
        const Prepared p = Prepare(r, RestoreScope::File, Detail("renamed", "same body\n", "same body\n", "assets/new_name.txt", 5, "assets/old_name.txt"));
        const size_t blobCallsBefore = r.Count(trackop::kBlobGet);
        check(p.plan.ops.size() == 1 && Started(r, p) && r.RunToEnd() && r.Runner().Result().outcome == RestoreOutcome::Committed, "apply: a pure rename is undone");
        check(r.proj.Get("assets/old_name.txt") == "same body\n" && !r.proj.Exists("assets/new_name.txt"), "apply: the file moved back");
        check(r.Count(trackop::kBlobGet) <= blobCallsBefore + 1, "apply: a pure rename fetches no content (at most the one-byte existence check of the recheck)");
    }
}

// ---- (2)(3) 書く前に止まる: 競合・確認のあとの変化 ----

void TestStopsBeforeWriting(const Check& check)
{
    const std::string before = Lines(12);
    const std::string after = Lines(12, { { 6, "A6" } });
    // 競合: After のあとで再編集された対象は、プレビューの計画に書く操作が無く、始められない。1 バイトも書かれず、ジャーナルも作られない
    {
        ApplyRig r("conflict");
        const std::string edited = Lines(12, { { 6, "Z6" } });
        r.proj.Put("assets/a.txt", edited);
        r.blobs[Hash(before)] = before;
        const std::map<std::string, std::string> tree = Snapshot(r.proj.root, false);
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
        check(p.plan.verdict == RestoreVerdict::Conflict && p.plan.ops.empty(), "stop: a file edited after the change is a conflict with no operation");
        check(!Started(r, p) && !r.Runner().Busy(), "stop: a conflicting plan cannot be started");
        r.Round();
        check(Snapshot(r.proj.root, false) == tree && r.faulty->userOps == 0 && r.faulty->journalWrites == 0 && JournalDirs(r.proj).empty() && r.begins == 0 && r.ends == 0,
              "stop: not one byte is written, no journal is created and the batch is never opened");
    }
    // 確認の画面を出している間に対象が書き換えられた: 実行の直前の判定し直しで止まる (計画が違う)
    {
        ApplyRig r("recheck_changed");
        r.proj.Put("assets/a.txt", after);
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
        r.proj.Put("assets/a.txt", Lines(12, { { 6, "someone else" } })); // 確認の画面を出している間
        const std::string touched = r.proj.Get("assets/a.txt");
        check(Started(r, p) && r.RunToEnd(), "stop: the run starts and ends");
        const RestoreRunResult& result = r.Runner().Result();
        check(result.outcome == RestoreOutcome::Blocked && result.reason == RestoreStopReason::PlanChanged, "stop: a target changed after the preview is stopped by the re-check (plan changed)");
        check(r.proj.Get("assets/a.txt") == touched && r.faulty->userOps == 0 && JournalDirs(r.proj).empty() && r.begins == 0 && r.ends == 0,
              "stop: nothing was written and no journal exists");
    }
    // Play を始めた (ゲート): 実行の直前の判定し直しで止まる
    {
        ApplyRig r("gate_recheck");
        r.proj.Put("assets/a.txt", after);
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
        bool playing = true; // プレビューのあと、Play が始まった
        check(Started(r, p, [&](bool) { return playing ? std::vector<RestoreBlock>{ PlayingBlock() } : std::vector<RestoreBlock>(); }) && r.RunToEnd(), "stop: the run ends");
        const RestoreRunResult& result = r.Runner().Result();
        check(result.outcome == RestoreOutcome::Blocked && result.reason == RestoreStopReason::Gate && result.blocks.size() == 1 && result.blocks[0].gate == GateBlocker::Playing,
              "stop: starting Play after the preview stops the run with the blocking reason");
        check(r.proj.Get("assets/a.txt") == after && r.faulty->userOps == 0 && JournalDirs(r.proj).empty() && r.begins == 0 && r.ends == 0, "stop: nothing written, no journal");
    }
    // 準備の途中 (内容を取ったあと、書く直前) でゲートが閉じた: 書かない。ジャーナルは捨てる
    {
        ApplyRig r("gate_late");
        r.proj.Put("assets/a.txt", after);
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
        int calls = 0;
        check(Started(r, p, [&](bool) {
                  return ++calls >= 2 ? std::vector<RestoreBlock>{ PlayingBlock() } : std::vector<RestoreBlock>();
              }) && r.RunToEnd(), "stop: the run ends");
        check(r.Runner().Result().outcome == RestoreOutcome::Blocked && r.Runner().Result().reason == RestoreStopReason::Gate && calls == 2, "stop: the gate is judged again right before the first write");
        check(r.proj.Get("assets/a.txt") == after && r.faulty->userOps == 0 && JournalDirs(r.proj).empty() && r.begins == 0 && r.ends == 0, "stop: the prepared journal is thrown away and nothing was written");
    }
    // 内容を保存したあと、書く直前に対象が書き換えられた: 書かない
    {
        ApplyRig r("content_late");
        r.proj.Put("assets/a.txt", after);
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
        int calls = 0;
        const std::string other = Lines(12, { { 6, "late edit" } });
        check(Started(r, p, [&](bool) {
                  if (++calls == 2) {
                      r.proj.Put("assets/a.txt", other); // 2 回目の判定の直前に他者が書く
                  }
                  return std::vector<RestoreBlock>();
              }) && r.RunToEnd(), "stop: the run ends");
        check(r.Runner().Result().outcome == RestoreOutcome::Blocked && r.Runner().Result().reason == RestoreStopReason::ContentChanged && r.proj.Get("assets/a.txt") == other && r.faulty->userOps == 0,
              "stop: a target rewritten between the saving of the journal and the first write is not overwritten");
    }
    // プレビューのあとで親がジャンクションに差し替えられた: 書かずに止まる
    {
        ApplyRig r("junction");
        r.proj.Put("assets/sub/a.txt", after);
        const std::string outsideBefore = "outside\n";
        fs::create_directories(r.proj.root.parent_path() / (r.proj.root.filename().wstring() + L"_outside"));
        const fs::path outside = r.proj.root.parent_path() / (r.proj.root.filename().wstring() + L"_outside");
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/sub/a.txt"));
        check(p.plan.verdict == RestoreVerdict::Ready, "path: the plan is ready before the folder is swapped");
        std::error_code ec;
        fs::rename(r.proj.P("assets/sub"), r.proj.P("assets/sub_real"), ec);
        const bool junction = MakeJunction(r.proj.P("assets/sub"), outside);
        std::ofstream(outside / L"a.txt", std::ios::binary) << outsideBefore << after;
        check(junction, "path: a junction could be created for the test");
        const std::string outsideContent = outsideBefore + after;
        check(Started(r, p) && r.RunToEnd(), "path: the run ends");
        check(r.Runner().Result().outcome == RestoreOutcome::Blocked && r.Runner().Result().reason == RestoreStopReason::PlanChanged, "path: a parent swapped for a junction after the preview stops the run before any write");
        std::ifstream in(outside / L"a.txt", std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        check(text.str() == outsideContent && r.faulty->userOps == 0 && r.begins == 0, "path: the folder behind the junction was not written");
        // 書き込みを始めたあとの差し替え (操作ごとの検査): 最初の journal の applying 保存の直後に差し替える
        fs::remove(r.proj.P("assets/sub"), ec); // 空のジャンクションを外す (リンク先は消えない)
        fs::rename(r.proj.P("assets/sub_real"), r.proj.P("assets/sub"), ec);
        r.Runner().AcknowledgeResult();
        bool swapped = false;
        r.faulty->beforeJournalWrite = [&](const std::wstring&, std::string_view bytes) {
            if (!swapped && bytes.find("\"state\": \"applying\"") != std::string_view::npos) {
                swapped = true;
                std::error_code e2;
                fs::rename(r.proj.P("assets/sub"), r.proj.P("assets/sub_real"), e2);
                MakeJunction(r.proj.P("assets/sub"), outside);
            }
        };
        const Prepared p2 = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/sub/a.txt"));
        check(p2.plan.verdict == RestoreVerdict::Ready && Started(r, p2) && r.RunToEnd(), "path: a second run started and ended");
        check(swapped && r.Runner().Result().reason == RestoreStopReason::PathChanged && r.Runner().Result().completedOps == 0,
              "path: a folder swapped for a junction after the applying record is caught by the per-operation check before any write");
        check(r.Runner().Result().outcome == RestoreOutcome::RecoveryRequired, "path: the path now leads to content that is not what we wrote, so the rollback leaves it alone and asks for recovery");
        std::ifstream in2(outside / L"a.txt", std::ios::binary);
        std::stringstream text2;
        text2 << in2.rdbuf();
        check(text2.str() == outsideContent && r.begins == 1 && r.ends == 1, "path: nothing was written through the junction and the batch was closed");
        fs::remove(r.proj.P("assets/sub"), ec);
        fs::remove_all(outside, ec);
    }
}

// ---- (4) 失敗 → 巻き戻し ----

void TestRollback(const Check& check)
{
    // 名前を戻して内容を書く 2 操作。2 つ目の書き込みを失敗させる: 1 つ目 (名前を戻す) が pre\ の内容へ戻り、rolled_back
    {
        ApplyRig r("rb_second");
        r.proj.Put("assets/new_name.txt", "after body\n");
        r.blobs[Hash("before body\n")] = "before body\n";
        const Prepared p = Prepare(r, RestoreScope::File, Detail("renamed", "before body\n", "after body\n", "assets/new_name.txt", 5, "assets/old_name.txt"));
        r.faulty->failOnUserOp = 2; // 1 つ目 = 名前の変更、2 つ目 = 内容の書き込み
        check(Started(r, p) && r.RunToEnd(), "rollback: the run ends");
        const RestoreRunResult& result = r.Runner().Result();
        check(result.outcome == RestoreOutcome::RolledBack && result.reason == RestoreStopReason::WriteFailed, "rollback: a failed second write ends as rolled back");
        check(r.proj.Get("assets/new_name.txt") == "after body\n" && !r.proj.Exists("assets/old_name.txt"), "rollback: the first operation (the rename) was undone; the file is exactly as before");
        const std::vector<fs::path> dirs = JournalDirs(r.proj);
        check(dirs.size() == 1 && JournalStateOf(dirs[0]) == "rolled_back", "rollback: the journal says rolled_back");
        check(r.begins == 1 && r.ends == 1 && !r.Runner().RecoveryPending(), "rollback: the batch is closed once and nothing needs recovery");
        std::vector<std::string> states;
        for (const std::string& o : r.order) {
            states.push_back(o);
        }
        check(std::find(states.begin(), states.end(), "restore_record:rolling_back") != states.end() && states.back() == "restore_record:rolled_back", "rollback: the service is told rolling_back and then rolled_back");
    }
    // 巻き戻しの途中で、1 つ目が他者に書き換えられた: そのファイルは触らず recovery_required
    {
        ApplyRig r("rb_foreign");
        r.proj.Put("assets/new_name.txt", "after body\n");
        r.blobs[Hash("before body\n")] = "before body\n";
        const Prepared p = Prepare(r, RestoreScope::File, Detail("renamed", "before body\n", "after body\n", "assets/new_name.txt", 5, "assets/old_name.txt"));
        r.faulty->failOnUserOp = 2;
        r.faulty->beforeUserOp = [&](int index, const std::wstring&) {
            if (index == 2) {
                r.proj.Put("assets/old_name.txt", "written by someone else while we were applying\n"); // 名前を戻した先を他者が書き換える
            }
        };
        check(Started(r, p) && r.RunToEnd(), "rollback: the run ends");
        const RestoreRunResult& result = r.Runner().Result();
        check(result.outcome == RestoreOutcome::RecoveryRequired && result.unresolved.size() == 1 && result.unresolved[0] == "assets/old_name.txt", "rollback: a file someone else wrote is left untouched and reported (recovery required)");
        check(r.proj.Get("assets/old_name.txt") == "written by someone else while we were applying\n", "rollback: the foreign content is exactly as the other writer left it");
        check(r.proj.Get("assets/new_name.txt") == "after body\n", "rollback: the other file was still put back (only the foreign one is untouched)");
        const std::vector<fs::path> dirs = JournalDirs(r.proj);
        check(dirs.size() == 1 && JournalStateOf(dirs[0]) == "recovery_required" && r.Runner().RecoveryPending() && r.Runner().Pending(), "rollback: the journal says recovery_required and a new restore is refused");
        check(r.begins == 1 && r.ends == 1, "rollback: the batch is closed even when the rollback cannot finish");
    }
    // 例外: 書き込みの途中で例外が出ても EndBatch を通り、巻き戻す
    {
        ApplyRig r("rb_exception");
        r.proj.Put("assets/new_name.txt", "after body\n");
        r.blobs[Hash("before body\n")] = "before body\n";
        const Prepared p = Prepare(r, RestoreScope::File, Detail("renamed", "before body\n", "after body\n", "assets/new_name.txt", 5, "assets/old_name.txt"));
        r.faulty->throwOnUserOp = 2;
        check(Started(r, p) && r.RunToEnd(), "exception: the run ends");
        check(r.Runner().Result().reason == RestoreStopReason::Exception && r.Runner().Result().outcome == RestoreOutcome::RolledBack, "exception: an exception while writing ends as a rollback");
        check(r.begins == 1 && r.ends == 1 && r.proj.Get("assets/new_name.txt") == "after body\n" && !r.proj.Exists("assets/old_name.txt"), "exception: EndBatch was called once and the file is back to its original state");
    }
    // 書いたあとの読み直しが合わない (他者が書き換えた): 巻き戻す
    {
        ApplyRig r("rb_verify");
        const std::string before = Lines(12);
        const std::string after = Lines(12, { { 6, "A6" } });
        r.proj.Put("assets/a.txt", after);
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
        // 書いた直後 (操作のあとの manifest 保存) に、他者が書き換える
        r.faulty->beforeJournalWrite = [&](const std::wstring&, std::string_view bytes) {
            if (bytes.find("\"completedOps\": 1") != std::string_view::npos && bytes.find("\"state\": \"applying\"") != std::string_view::npos) {
                r.proj.Put("assets/a.txt", "tampered\n");
            }
        };
        check(Started(r, p) && r.RunToEnd(), "verify: the run ends");
        // 他者が書いた内容は軌跡に無いので触らない = recovery_required (読み直しの失敗 + 巻き戻せない)
        check(r.Runner().Result().outcome == RestoreOutcome::RecoveryRequired && r.Runner().Result().reason == RestoreStopReason::VerifyFailed && r.proj.Get("assets/a.txt") == "tampered\n",
              "verify: a file that does not read back as written is not blindly overwritten (recovery required, foreign content kept)");
        check(r.begins == 1 && r.ends == 1, "verify: the batch is closed");
    }
    // 3 ファイルの巻き戻し (ジャーナルの手組み。計画の作り手は 2 ファイルまでなので、巻き戻しの仕組みだけを 3 ファイルで確かめる)
    {
        ApplyRig r("rb_three");
        const std::string a0 = "A0\n", b0 = "B0\n", c0 = "C0\n";
        const std::string a1 = "A1\n", b1 = "B1\n";
        r.proj.Put("assets/a.txt", a1);       // 書いた (1 つ目)
        r.proj.Put("assets/b.txt", b1);       // 書いた (2 つ目)
        r.proj.Put("assets/c.txt", "c-foreign\n"); // 3 つ目は他者が書いた (軌跡に無い)
        JournalManifest m;
        m.restoreId = "11111111-2222-4333-8444-555555555555";
        m.state = JournalState::Applying;
        m.scope = "file";
        const std::vector<RestoreOp> ops = [&] {
            std::vector<RestoreOp> v;
            for (const auto& [path, hash] : std::vector<std::pair<std::string, std::string>>{ { "assets/a.txt", Hash(a1) }, { "assets/b.txt", Hash(b1) }, { "assets/c.txt", Hash("C1\n") } }) {
                RestoreOp op;
                op.kind = RestoreOp::Kind::Overwrite;
                op.path = path;
                op.hash = hash;
                v.push_back(op);
            }
            return v;
        }();
        m.ops = ToJournalOps(ops);
        m.files = BuildJournalFiles(ops, { { "assets/a.txt", Hash(a0) }, { "assets/b.txt", Hash(b0) }, { "assets/c.txt", Hash(c0) } });
        const std::string pre[3] = { a0, b0, c0 };
        for (size_t i = 0; i < 3; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "pre/%04zu.bin", i + 1);
            m.files[i].preFile = name;
            m.files[i].preSize = static_cast<int64_t>(pre[i].size());
        }
        m.completedOps = 2;
        RestoreJournalStore store(r.proj.Root(), *r.faulty);
        check(store.Begin(m), "three files: the hand-made journal is written");
        for (size_t i = 0; i < 3; ++i) {
            r.faulty->Write(store.FileIn(m.restoreId, m.files[i].preFile), pre[i]);
        }
        r.Runner().ScanRecovery();
        check(r.Runner().Recoveries().size() == 1 && r.Runner().RecoveryPending(), "three files: an applying journal is detected at startup");
        const int userOpsBefore = r.faulty->userOps;
        check(r.faulty->userOps == userOpsBefore, "three files: detecting it writes nothing");
        check(r.Runner().StartRecoveryRollback(m.restoreId, [](bool) { return std::vector<RestoreBlock>(); }) && r.RunToEnd(), "three files: the user-chosen rollback runs");
        check(r.Runner().Result().outcome == RestoreOutcome::RecoveryRequired && r.Runner().Result().unresolved == std::vector<std::string>{ "assets/c.txt" },
              "three files: the two files that hold what was written go back; the one someone else wrote is left");
        check(r.proj.Get("assets/a.txt") == a0 && r.proj.Get("assets/b.txt") == b0 && r.proj.Get("assets/c.txt") == "c-foreign\n", "three files: contents are the originals for the two and untouched for the third");
        check(r.begins == 1 && r.ends == 1, "three files: the rollback is inside one batch");
    }
}

// ---- (5) 起動時の回復 ----

void TestRecovery(const Check& check)
{
    // ジャーナルの各状態のディレクトリを直接置く
    ApplyRig r("recovery");
    const std::string a0 = "A0\n", a1 = "A1\n", b0 = "B0\n", b1 = "B1\n";
    r.proj.Put("assets/a.txt", a1);
    r.proj.Put("assets/b.txt", b0);
    auto make = [&](const std::string& id, JournalState state) {
        JournalManifest m;
        m.restoreId = id;
        m.state = state;
        m.scope = "file";
        m.revisionId = 5;
        m.startedAt = 1000;
        m.updatedAt = 1000;
        std::vector<RestoreOp> ops;
        for (const auto& [path, hash] : std::vector<std::pair<std::string, std::string>>{ { "assets/a.txt", Hash(a1) }, { "assets/b.txt", Hash(b1) } }) {
            RestoreOp op;
            op.kind = RestoreOp::Kind::Overwrite;
            op.path = path;
            op.hash = hash;
            ops.push_back(op);
        }
        m.ops = ToJournalOps(ops);
        m.files = BuildJournalFiles(ops, { { "assets/a.txt", Hash(a0) }, { "assets/b.txt", Hash(b0) } });
        m.files[0].preFile = "pre/0001.bin";
        m.files[0].preSize = static_cast<int64_t>(a0.size());
        m.files[1].preFile = "pre/0002.bin";
        m.files[1].preSize = static_cast<int64_t>(b0.size());
        m.completedOps = 1; // 1 つ目の後
        RestoreJournalStore store(r.proj.Root(), *r.faulty);
        store.Begin(m);
        r.faulty->Write(store.FileIn(id, "pre/0001.bin"), a0);
        r.faulty->Write(store.FileIn(id, "pre/0002.bin"), b0);
    };
    make("aaaaaaaa-0000-4000-8000-000000000001", JournalState::Prepared);
    make("aaaaaaaa-0000-4000-8000-000000000002", JournalState::Applying);
    make("aaaaaaaa-0000-4000-8000-000000000003", JournalState::RollingBack);
    make("aaaaaaaa-0000-4000-8000-000000000004", JournalState::RecoveryRequired);
    make("aaaaaaaa-0000-4000-8000-000000000005", JournalState::Committed);
    // manifest.json が壊れたジャーナル / 空のジャーナル
    {
        const std::string dir = ".mye/ai_changes/recovery/bbbbbbbb-0000-4000-8000-000000000006";
        r.proj.Put(dir + "/manifest.json", "{ this is not json");
        r.proj.Put(dir + "/pre/0001.bin", "kept copy\n");
        r.proj.Put(".mye/ai_changes/recovery/bbbbbbbb-0000-4000-8000-000000000007/pre/0001.bin", "no manifest at all\n");
    }
    std::map<std::string, std::string> userTree = Snapshot(r.proj.root, true);
    const int userOps = r.faulty->userOps;
    r.Runner().ScanRecovery();
    const std::vector<RestoreJournalStore::Entry>& found = r.Runner().Recoveries();
    std::set<std::string> ids;
    int broken = 0;
    for (const auto& e : found) {
        ids.insert(e.id.substr(e.id.size() - 2));
        broken += e.broken ? 1 : 0;
    }
    check(ids == std::set<std::string>{ "02", "03", "04", "06", "07" } && broken == 2, "recovery: applying / rolling_back / recovery_required and the two broken journals are found; committed and prepared are not");
    check(!fs::exists(r.proj.root / L".mye" / L"ai_changes" / L"recovery" / L"aaaaaaaa-0000-4000-8000-000000000001"), "recovery: a prepared journal is thrown away (nothing had been written)");
    check(fs::exists(r.proj.root / L".mye" / L"ai_changes" / L"recovery" / L"aaaaaaaa-0000-4000-8000-000000000005"), "recovery: a committed journal is kept for the retention period");
    check(Snapshot(r.proj.root, true) == userTree && r.faulty->userOps == userOps, "recovery: detecting the journals writes nothing to the user's files");
    check(r.Runner().RecoveryPending() && r.Runner().Pending(), "recovery: a restore is pending until the user chooses");
    // 新しい復元は始められない
    {
        const std::string before = Lines(12);
        const std::string after = Lines(12, { { 6, "A6" } });
        r.proj.Put("assets/c.txt", after);
        userTree = Snapshot(r.proj.root, true);
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/c.txt"));
        check(p.plan.verdict == RestoreVerdict::Ready && !Started(r, p), "recovery: a new restore is refused while a recovery is pending");
        r.Round();
        check(r.proj.Get("assets/c.txt") == after && r.begins == 0, "recovery: the refused restore wrote nothing");
    }
    // 画面 (ヘッドレスの ImGui): 3 つの内容が出る。英日
    {
        AiChangesHost host;
        host.projectRoot = r.proj.Root();
        host.restoreJournalPending = [&]() { return r.Runner().RecoveryPending(); };
        RestoreRunWindow window;
        window.OpenRecovery();
        InBothLanguagesForApply([&](Lang) {
            WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); });
            const std::vector<RecoveryEntryView>& views = window.LastRecoveryViews();
            bool three = false;
            bool brokenShown = false;
            for (const RecoveryEntryView& v : views) {
                if (v.broken) {
                    brokenShown = brokenShown || !v.brokenText.empty();
                } else if (v.files.size() == 2 && !v.files[0].before.empty() && !v.files[0].intended.empty() && !v.files[0].now.empty() && !v.files[0].verdict.empty()) {
                    three = true;
                }
            }
            check(views.size() == 5 && three && brokenShown, "recovery screen: before / intended / now are shown per file, and the broken journals are shown as such");
        });
        check(Snapshot(r.proj.root, true) == userTree, "recovery screen: drawing it changed nothing");
    }
    // 破れたもの: 無視 (中身は消さない)
    {
        const size_t pendingBefore = r.Runner().Recoveries().size();
        check(r.Runner().Dismiss("bbbbbbbb-0000-4000-8000-000000000006") && r.Runner().Dismiss("bbbbbbbb-0000-4000-8000-000000000007") && r.Runner().Recoveries().size() == pendingBefore - 2,
              "recovery: a broken journal can be dismissed without crashing");
        check(r.proj.Exists(".mye/ai_changes/recovery/bbbbbbbb-0000-4000-8000-000000000006/pre/0001.bin"), "recovery: dismissing keeps the saved copies");
    }
    // 「今のままにする」: 何も書かず、回復待ちから外れる
    {
        check(r.Runner().KeepCurrent("aaaaaaaa-0000-4000-8000-000000000003") && Snapshot(r.proj.root, true) == userTree, "recovery: keep current writes nothing");
        check(JournalStateOf(r.proj.root / L".mye" / L"ai_changes" / L"recovery" / L"aaaaaaaa-0000-4000-8000-000000000003") == "recovered", "recovery: the journal is marked recovered");
        r.Round();
        check(r.Count(trackop::kRestoreRecord) >= 1, "recovery: the choice is recorded in the history");
    }
    // 「適用前へ戻す」: a は書いた内容なので戻る。b は適用前のまま (軌跡の 0 番目) なので触らない
    {
        bool blockedFirst = false;
        const std::string id = "aaaaaaaa-0000-4000-8000-000000000002";
        check(!r.Runner().StartRecoveryRollback(id, [&](bool) { blockedFirst = true; return std::vector<RestoreBlock>{ PlayingBlock() }; }) && blockedFirst && r.Runner().Result().outcome == RestoreOutcome::Blocked,
              "recovery: the choice is also gated (Play blocks it) and writes nothing");
        r.Runner().AcknowledgeResult();
        check(r.Runner().StartRecoveryRollback(id, [](bool) { return std::vector<RestoreBlock>(); }) && r.RunToEnd(), "recovery: put back the original content runs");
        check(r.Runner().Result().outcome == RestoreOutcome::RolledBack && r.proj.Get("assets/a.txt") == a0 && r.proj.Get("assets/b.txt") == b0, "recovery: the file holding the written content went back; the other is as it was");
        check(JournalStateOf(r.proj.root / L".mye" / L"ai_changes" / L"recovery" / Utf8ToWide(id)) == "recovered" && r.begins == 1 && r.ends == 1, "recovery: the journal is recovered and the batch closed once");
    }
    r.Runner().AcknowledgeResult();
    check(r.Runner().Recoveries().size() == 1 && r.Runner().Recoveries()[0].id.substr(r.Runner().Recoveries()[0].id.size() - 2) == "04", "recovery: only the recovery_required journal is still pending");
}


// ---- (6) 内容の取得の失敗 ----

void TestFetch(const Check& check)
{
    const std::string before = Lines(12);
    const std::string after = Lines(12, { { 6, "A6" } });
    // 取った内容のハッシュが記録と違う (途中で化けた): 何も書かない
    {
        ApplyRig r("blob_mismatch");
        r.proj.Put("assets/a.txt", after);
        r.blobs[Hash(before)] = before;
        r.corruptBlobs = true;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
        check(Started(r, p) && r.RunToEnd(), "fetch: the run ends");
        check(r.Runner().Result().outcome == RestoreOutcome::Blocked && r.Runner().Result().reason == RestoreStopReason::BlobMismatch, "fetch: content whose hash differs from the recorded Before is never written");
        check(r.proj.Get("assets/a.txt") == after && r.faulty->userOps == 0 && JournalDirs(r.proj).empty() && r.begins == 0 && r.ends == 0, "fetch: nothing written, the prepared journal is gone, no batch");
    }
    // 取っている間に切断: 何も書かずにやめる
    {
        ApplyRig r("blob_disconnect");
        r.proj.Put("assets/a.txt", after);
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
        const size_t blobCalls = r.Count(trackop::kBlobGet);
        r.onAnswer = [&]() {
            if (r.Count(trackop::kBlobGet) > blobCalls) { // 内容を取りに行った要求のあと
                r.s->peerClosed = true;
                r.s->chunks.clear();
            }
        };
        check(Started(r, p) && r.RunToEnd(), "fetch: the run ends when the connection drops");
        check(r.Runner().Result().outcome == RestoreOutcome::Blocked && r.Runner().Result().reason == RestoreStopReason::FetchFailed, "fetch: a disconnect while fetching stops the run");
        check(r.proj.Get("assets/a.txt") == after && r.faulty->userOps == 0 && JournalDirs(r.proj).empty() && r.begins == 0 && r.ends == 0, "fetch: nothing written after a disconnect");
    }
    // ジャーナルを書けない: 何も書かない
    {
        ApplyRig r("journal_fail");
        r.proj.Put("assets/a.txt", after);
        r.blobs[Hash(before)] = before;
        const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
        r.faulty->failJournalWrites = -1;
        check(Started(r, p) && r.RunToEnd(), "journal: the run ends when the journal cannot be written");
        check(r.Runner().Result().reason == RestoreStopReason::JournalFailed && r.proj.Get("assets/a.txt") == after && r.faulty->userOps == 0 && r.begins == 0, "journal: without a journal nothing is written");
    }
}

// ---- (7) git を起動しない・Index に触らない ----

void TestGitUntouched(const Check& check)
{
    ApplyRig r("git");
    const std::string before = Lines(12);
    const std::string after = Lines(12, { { 6, "A6" } });
    r.proj.Put("assets/a.txt", after);
    // 一時の git リポジトリの形 (.git\index と HEAD)。復元は .git の下を読みも書きもしない
    std::string index(4096, '\0');
    for (size_t i = 0; i < index.size(); ++i) {
        index[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    r.proj.Put(".git/index", index);
    r.proj.Put(".git/HEAD", "ref: refs/heads/main\n");
    const std::string indexBefore = r.proj.Get(".git/index");
    r.blobs[Hash(before)] = before;
    const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
    const std::multiset<std::wstring> childrenBefore = ChildProcessNames();
    bool sawGit = false;
    check(Started(r, p), "git: the run starts");
    for (int i = 0; i < 600 && r.Runner().Busy(); ++i) {
        r.Round();
        for (const std::wstring& name : ChildProcessNames()) {
            if (_wcsicmp(name.c_str(), L"git.exe") == 0) {
                sawGit = true;
            }
        }
    }
    check(r.Runner().Result().outcome == RestoreOutcome::Committed, "git: the restore commits");
    check(r.proj.Get(".git/index") == indexBefore && r.proj.Get(".git/HEAD") == "ref: refs/heads/main\n", "git: the bytes of .git\\index are the same before and after the restore");
    check(!sawGit && ChildProcessNames() == childrenBefore, "git: no git process (and no child process at all) was started during the restore");
    for (const std::wstring& path : r.faulty->userLog) {
        check(path.find(L"\\.git\\") == std::wstring::npos, "git: no write went into .git");
    }
}

// ---- (8) 大きいファイル (8 MiB 超): ファイル全体の復元だけ。フレームを止めない ----

void TestLargeFile(const Check& check)
{
    ApplyRig r("large");
    std::string before(kRestoreReadMaxBytes + 70000u, 'x'); // 8 MiB を少し超える
    for (size_t i = 0; i < before.size(); i += 4096) {
        before[i] = static_cast<char>('a' + (i / 4096) % 26);
    }
    std::string after = before;
    after[100] = '#';
    after[after.size() - 7] = '#';
    r.proj.Put("assets/big.bin", after);
    r.blobs[Hash(before)] = before;
    const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/big.bin"));
    check(p.plan.verdict == RestoreVerdict::Ready && p.plan.ops.size() == 1, "large: a file over 8 MiB is judged by the streamed hash and the plan is ready");
    // ハッシュ・複写・取得が 1 回の Poll に収まらない (数フレームに分かれる)
    check(Started(r, p), "large: the run starts");
    int rounds = 0;
    for (; rounds < 3000 && r.Runner().Busy(); ++rounds) {
        r.Round();
    }
    check(rounds > 10 && r.Runner().Result().outcome == RestoreOutcome::Committed, "large: the restore of a file just over 8 MiB commits over many frames");
    check(r.proj.Get("assets/big.bin") == before, "large: the file is byte-identical to the Before content");
    const std::vector<fs::path> dirs = JournalDirs(r.proj);
    check(dirs.size() == 1 && fs::exists(dirs[0] / L"pre" / L"0001.bin") && fs::file_size(dirs[0] / L"pre" / L"0001.bin") == after.size(), "large: the big original was copied to the journal (not held in memory)");
    check(r.Count(trackop::kBlobGet) >= 17, "large: the content is fetched in 512 KiB pieces");
    // 失敗時: 大きいファイルも巻き戻る
    {
        ApplyRig r2("large_rb");
        r2.proj.Put("assets/big.bin", after);
        r2.proj.Put("assets/small.txt", "after\n");
        r2.blobs[Hash(before)] = before;
        r2.blobs[Hash("before\n")] = "before\n";
        const Prepared q = Prepare(r2, RestoreScope::File, Detail("renamed", before, after, "assets/big.bin", 5, "assets/old.bin"));
        check(q.plan.verdict == RestoreVerdict::Ready && q.plan.ops.size() == 2, "large rollback: undoing a rename of a big file is two operations");
        r2.faulty->failOnUserOp = 2;
        check(Started(r2, q), "large rollback: the run starts");
        for (int i = 0; i < 3000 && r2.Runner().Busy(); ++i) {
            r2.Round();
        }
        check(r2.Runner().Result().outcome == RestoreOutcome::RolledBack && r2.proj.Get("assets/big.bin") == after && !r2.proj.Exists("assets/old.bin"), "large rollback: the big file is back exactly as it was");
    }
    // 取り消し: 書き始める前だけ効く
    {
        ApplyRig r3("large_cancel");
        r3.proj.Put("assets/big.bin", after);
        r3.blobs[Hash(before)] = before;
        const Prepared q = Prepare(r3, RestoreScope::File, Detail("modified", before, after, "assets/big.bin"));
        check(Started(r3, q), "cancel: the run starts");
        for (int i = 0; i < 5; ++i) {
            r3.Round();
        }
        r3.Runner().Cancel();
        r3.Round();
        check(r3.Runner().Result().outcome == RestoreOutcome::Cancelled && r3.proj.Get("assets/big.bin") == after && r3.begins == 0 && JournalDirs(r3.proj).empty(), "cancel: cancelling before the first write writes nothing and removes the journal");
        r3.Runner().Cancel(); // 結果が出たあとは何も起きない
        check(!r3.Runner().Busy(), "cancel: a late cancel does nothing");
    }
}

// ---- (9) サービスへの記録: 送れなかった記録は送り直す ----

void TestRecords(const Check& check)
{
    const std::string before = Lines(12);
    const std::string after = Lines(12, { { 6, "A6" } });
    ApplyRig r("records");
    r.proj.Put("assets/a.txt", after);
    r.blobs[Hash(before)] = before;
    r.failRecords = true; // サービスが記録を受けられない
    const Prepared p = Prepare(r, RestoreScope::File, Detail("modified", before, after, "assets/a.txt"));
    check(Started(r, p) && r.RunToEnd(), "records: the run ends even when the service rejects the records");
    check(r.Runner().Result().outcome == RestoreOutcome::Committed && r.proj.Get("assets/a.txt") == before, "records: the file restore does not depend on the service's answer to the records");
    check(r.Runner().PendingRecordCount() > 0 && r.Runner().Result().recordPending, "records: the records that could not be sent are kept and shown as pending");
    InBothLanguagesForApply([&](Lang) {
        const RestoreRunView view = BuildRestoreRunView(r.Runner());
        check(view.mode == RestoreRunView::Mode::Result && !view.recordNote.empty() && !view.headline.empty(), "records: the result tells the history was not sent");
    });
    r.failRecords = false;
    for (int i = 0; i < 60 && r.Runner().PendingRecordCount() > 0; ++i) {
        r.Round();
    }
    check(r.Runner().PendingRecordCount() == 0 && r.recordArgs.back().value("state", std::string()) == "committed", "records: once the service answers, the pending records are sent again in order");
    r.Runner().AcknowledgeResult();
    // 履歴
    r.Runner().RequestHistory();
    for (int i = 0; i < 5; ++i) {
        r.Round();
    }
    const std::vector<RestoreHistoryRow> rows = BuildHistoryRows(r.Runner().History());
    check(rows.size() == 1 && rows[0].target == "assets/a.txt" && !rows[0].time.empty() && rows[0].state == RestoreStateText("committed"), "history: the restore history is fetched and shown");
}

// ---- (10) 窓: 実行ボタン・進行・結果 (ヘッドレスの ImGui) ----

void TestWindow(const Check& check)
{
    const std::string before = Lines(12);
    const std::string after = Lines(12, { { 6, "A6" } });
    ApplyRig r("window");
    r.proj.Put("assets/a.txt", after);
    r.blobs[Hash(before)] = before;
    // モデルに 1 件の変更を仕込む (list / describe / hunks の応答)
    json describe = Detail("modified", before, after, "assets/a.txt");
    describe["reviewable"] = true;
    describe["review"] = "pending";
    describe["reviewCounts"] = { { "pending", 1 }, { "approved", 0 }, { "rejected", 0 }, { "needsReview", 0 } };
    describe["reviewLeaves"] = json::array();
    describe["reviewHistory"] = json::array();
    describe["hunks"] = json::array({ { { "ordinal", 0 } } });
    r.session.Model().SetInterest(kTrackAllParts);
    // 応答: list / describe / hunks / sessions / status
    r.modelAnswer = [&](const std::string& op, const json&, json& result) {
        {
            if (op == trackop::kList) {
                result = { { "items", json::array({ { { "revisionId", 5 }, { "path", "assets/a.txt" }, { "changeType", "modified" }, { "actor", "ai" }, { "attribution", "claimed" }, { "review", "pending" } } }) }, { "nextCursor", nullptr } };
            } else if (op == trackop::kDescribe) {
                result = describe;
            } else if (op == trackop::kHunks) {
                result = { { "items", json::array({ { { "hunkId", 21 }, { "ordinal", 0 }, { "oldStart", 6 }, { "oldCount", 1 }, { "newStart", 6 }, { "newCount", 1 }, { "beforeText", "a6\n" }, { "afterText", "A6\n" }, { "contextPre", "a3\na4\na5\n" }, { "contextPost", "a7\na8\na9\n" }, { "eolOnly", false }, { "state", "active" }, { "review", "pending" } } }) }, { "nextCursor", nullptr } };
            } else if (op == trackop::kSceneDiff) {
                result = { { "items", json::array() }, { "nextCursor", nullptr } };
            } else if (op == trackop::kSessions) {
                result = { { "items", json::array() } };
            } else if (op == trackop::kStatus) {
                result = { { "phase", "watching" }, { "db", { { "state", "recording" } } } };
            } else {
                return false; // 復元の op と blob_get は ApplyRig::AnswerAll の既定が答える
            }
            return true;
        }
    };
    // list → describe → hunks の順に取る
    r.session.Model().SelectRevision(5);
    for (int i = 0; i < 8; ++i) {
        r.Round();
    }
    AiChangesWindow window;
    window.open = true;
    AiChangesHost host;
    host.allowTracking = true;
    host.enabled = true;
    host.projectRoot = r.proj.Root();
    host.restoreJournalPending = [&]() { return r.Runner().RecoveryPending(); };
    RestoreRunWindow runWindow;
    auto frame = [&]() {
        r.s->nowMs += 100;
        r.clockMs += 100;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        WithHeadlessImGuiFrame([&] {
            window.OnImGui(r.session, host);
            runWindow.OnImGui(r.session, host);
        });
    };
    for (int i = 0; i < 3; ++i) {
        frame();
    }
    check(window.OpenRestore(r.session, RestoreScope::File, host), "window: the preview opens for the selected change");
    for (int i = 0; i < 12; ++i) {
        frame();
    }
    const RestoreScreen& screen = window.LastRestoreScreen();
    check(screen.hasPlan && screen.verdict == RestoreVerdict::Ready && screen.blocks.empty() && screen.canApply && !screen.applyNote.empty(),
          "window: with a ready plan and nothing blocking, the Restore button can be pressed");
    // 止める理由が立つと押せない
    {
        AiChangesHost blocked = host;
        blocked.gateInputs = []() {
            GateInputs in;
            in.playing = true;
            return in;
        };
        const std::vector<RestoreBlock> blocks = window.RestoreBlocks(r.session, blocked);
        check(!blocks.empty() && !window.ApplyRestore(r.session, blocked) && !r.Runner().Busy(), "window: with Play running, applying is refused and nothing starts");
    }
    // 押す
    check(window.ApplyRestore(r.session, host) && r.Runner().Busy() && !window.Restore().IsOpen(), "window: applying starts the run and closes the preview");
    check(r.Runner().WriteSectionActive() == false, "window: the write section has not started before the files were saved");
    bool sawProgress = false;
    bool sawWriteSection = false;
    for (int i = 0; i < 80 && (r.Runner().Busy() || !r.Runner().HasResult()); ++i) {
        frame();
        sawProgress = sawProgress || runWindow.LastRunView().mode == RestoreRunView::Mode::Progress;
        sawWriteSection = sawWriteSection || r.Runner().WriteSectionActive();
    }
    check(sawProgress && sawWriteSection, "window: a progress screen is shown while running, and the write section (the gate's in-flight state) was active while writing");
    frame();
    InBothLanguagesForApply([&](Lang) {
        frame();
        const RestoreRunView& view = runWindow.LastRunView();
        check(view.mode == RestoreRunView::Mode::Result && view.outcome == RestoreOutcome::Committed && !view.headline.empty(), "window: the result screen says the restore succeeded (both languages)");
    });
    check(r.proj.Get("assets/a.txt") == before, "window: the file was restored through the window's button");
    r.Runner().AcknowledgeResult();
    frame();
    check(runWindow.LastRunView().mode == RestoreRunView::Mode::None, "window: acknowledging the result clears the screen");
    // 失敗の結果も画面に出る (英日)
    {
        ApplyRig r2("window_fail");
        r2.proj.Put("assets/new_name.txt", "after body\n");
        r2.blobs[Hash("before body\n")] = "before body\n";
        const Prepared p = Prepare(r2, RestoreScope::File, Detail("renamed", "before body\n", "after body\n", "assets/new_name.txt", 5, "assets/old_name.txt"));
        r2.faulty->failOnUserOp = 2;
        Started(r2, p);
        r2.RunToEnd();
        InBothLanguagesForApply([&](Lang) {
            const RestoreRunView view = BuildRestoreRunView(r2.Runner());
            check(view.outcome == RestoreOutcome::RolledBack && !view.headline.empty() && !view.reasons.empty() && !view.journal.empty(), "window: a rolled-back result names the cause and the journal (both languages)");
        });
    }
}

// ---- (10b) 書き込みの途中で Editor が終了する / 落ちる: 次の起動で回復の対象になる ----

void TestShutdownMidWrite(const Check& check)
{
    const std::string before = Lines(12);
    const std::string after = Lines(12, { { 6, "A6" } });
    ApplyRig r("shutdown");
    r.proj.Put("assets/new_name.txt", "after body\n");
    r.blobs[Hash("before body\n")] = "before body\n";
    const Prepared p = Prepare(r, RestoreScope::File, Detail("renamed", "before body\n", "after body\n", "assets/new_name.txt", 5, "assets/old_name.txt"));
    check(Started(r, p), "shutdown: the run starts");
    for (int i = 0; i < 400 && !r.Runner().WriteSectionActive(); ++i) {
        r.Round();
    }
    check(r.Runner().WriteSectionActive() && r.begins == 1 && r.ends == 0, "shutdown: the run reached the write section (BeginBatch called, not yet closed)");
    r.Runner().Shutdown(); // Editor の終了
    check(r.begins == 1 && r.ends == 1 && !r.Runner().WriteSectionActive(), "shutdown: leaving in the middle of a write still closes the batch exactly once");
    const std::vector<fs::path> dirs = JournalDirs(r.proj);
    check(dirs.size() == 1 && JournalStateOf(dirs[0]) == "applying", "shutdown: the journal stays applying (the next start offers recovery)");
    // 次の起動 (別の Runner): 検出される。選ぶまで何も書かない
    ApplyRig next("shutdown_next");
    const std::string journalName = WideToUtf8(dirs[0].filename().wstring());
    std::error_code ec;
    fs::create_directories(next.proj.root / L".mye" / L"ai_changes" / L"recovery", ec);
    fs::copy(dirs[0], next.proj.root / L".mye" / L"ai_changes" / L"recovery" / dirs[0].filename(), fs::copy_options::recursive, ec);
    next.Runner().ScanRecovery();
    check(next.Runner().RecoveryPending() && next.Runner().Recoveries().size() == 1 && next.Runner().Recoveries()[0].id == journalName, "shutdown: the next start detects the unfinished journal");
}

// ---- (11) 書き込みの口が 1 か所であること ----

std::string ReadSource(const char* relative)
{
    const fs::path starts[] = { fs::path(GetExecutableDir()), fs::current_path() };
    for (fs::path dir : starts) {
        for (int up = 0; up < 8 && !dir.empty(); ++up) {
            std::error_code ec;
            if (fs::is_regular_file(dir / relative, ec)) {
                std::ifstream in(dir / relative, std::ios::binary);
                std::stringstream text;
                text << in.rdbuf();
                return text.str();
            }
            dir = dir.parent_path();
        }
    }
    return std::string();
}

void TestWriteSurface(const Check& check)
{
    // 利用者のファイルを書き換える API は RestoreWriteIo.cpp にだけある。ほかのファイルは持たない (RestoreFileIo は読み取り専用)
    const char* const kOthers[] = {
        "src/Editor/ChangeTracking/RestoreRunner.cpp", "src/Editor/ChangeTracking/RestoreJournal.cpp", "src/Editor/ChangeTracking/RestoreFileIo.cpp",
        "src/Editor/ChangeTracking/RestorePreview.cpp", "src/Editor/ChangeTracking/RestorePlan.cpp", "src/Editor/Windows/Project/RestoreRunWindow.cpp",
    };
    const char* const kForbidden[] = { "WriteFileReplacing", "DeleteFileW", "MoveFileEx", "CopyFileW", "CreateFileW(", "std::ofstream", "fs::remove", "fs::rename", "fs::copy", "CreateProcess", "ShellExecute", "_wsystem", "popen" };
    for (const char* file : kOthers) {
        const std::string text = ReadSource(file);
        bool clean = !text.empty();
        for (const char* api : kForbidden) {
            // RestoreFileIo.cpp の読み取り (CreateFileW(GENERIC_READ ...)) だけは例外
            if (std::strcmp(api, "CreateFileW(") == 0 && std::strstr(file, "RestoreFileIo.cpp") != nullptr) {
                continue;
            }
            if (text.find(api) != std::string::npos) {
                clean = false;
            }
        }
        std::string message = std::string("write surface: ") + file + " has no direct write / process API (all writes go through RestoreWriteIo)";
        static std::vector<std::string> keep;
        keep.push_back(message);
        check(clean, keep.back().c_str());
    }
    check(ReadSource("src/Editor/ChangeTracking/RestoreWriteIo.cpp").find("WriteFileReplacing") != std::string::npos, "write surface: RestoreWriteIo writes through WriteFileReplacing");
}

} // namespace

void RunRestoreApplySelfTests(const std::function<void(bool, const char*)>& check)
{
    TestApplyKinds(check);
    TestStopsBeforeWriting(check);
    TestRollback(check);
    TestRecovery(check);
    TestFetch(check);
    TestGitUntouched(check);
    TestLargeFile(check);
    TestRecords(check);
    TestWindow(check);
    TestShutdownMidWrite(check);
    TestWriteSurface(check);
}

} // namespace mye
