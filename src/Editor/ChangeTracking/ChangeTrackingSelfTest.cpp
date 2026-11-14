//====================================================================================
//                          ChangeTrackingSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の Editor 側の回帰テスト (偽の I/O)
//====================================================================================
#include "Editor/ChangeTracking/ChangeTrackingSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>

#include "nlohmann/json.hpp"

#include "Editor/App/EditorSettings.h"
#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/TrackClient.h"
#include "Editor/ChangeTracking/TrackIo.h"
#include "Editor/ChangeTracking/TrackLaunch.h"
#include "Editor/ChangeTracking/TrackProjectId.h"
#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mye {
namespace {

// projectId の固定表 (tools\track\tests\project_id_cases.tsv と同じ行。表が見つかれば一致も確かめる)。
// 列: 入力パス / NormalizePathKey の結果 / projectId のもとの文字列 / projectId
struct ProjectIdRow {
    const char* input;
    const char* key;
    const char* source;
    const char* id;
};
const ProjectIdRow kProjectIdRows[] = {
    { R"(C:\Proj\MyGame)", R"(c:\proj\mygame)", R"(c:\proj\mygame)", "64489cc186743810" },
    { R"(c:/proj/mygame)", R"(c:\proj\mygame)", R"(c:\proj\mygame)", "64489cc186743810" },
    { R"(C:\Proj\MyGame\)", R"(c:\proj\mygame\)", R"(c:\proj\mygame)", "64489cc186743810" },
    { R"(C:\Proj\\MyGame\.\sub\..)", R"(c:\proj\mygame)", R"(c:\proj\mygame)", "64489cc186743810" },
    { R"(C:\プロジェクト\ゲーム)", R"(c:\プロジェクト\ゲーム)", R"(c:\プロジェクト\ゲーム)", "18aefa980b605010" },
    { R"(C:\Ｐｒｏ\ÀB\А)", R"(c:\Ｐｒｏ\Àb\А)", R"(c:\Ｐｒｏ\Àb\А)", "bba90799bf7b43e1" },
    { R"(\\Server\Share\Proj)", R"(\\server\share\proj)", R"(\\server\share\proj)", "289e4848cf3b4716" },
    { R"(C:\a\..\..\b)", R"(c:\b)", R"(c:\b)", "c18c3dc926c0dd64" },
    { R"(C:\Proj\MyGame/)", R"(c:\proj\mygame\)", R"(c:\proj\mygame)", "64489cc186743810" },
    { R"(C:\)", R"(c:\)", R"(c:\)", "3dc8472eaebe1a9d" },
    { R"(C:\Proj\.)", R"(c:\proj)", R"(c:\proj)", "907886f67558dd67" },
    { R"(C:\Proj\MyGame\..\)", R"(c:\proj\)", R"(c:\proj)", "907886f67558dd67" },
    { R"(C:\Proj\MyGame\\)", R"(c:\proj\mygame\)", R"(c:\proj\mygame)", "64489cc186743810" },
    { R"(C:\Proj\MyGame\/)", R"(c:\proj\mygame\)", R"(c:\proj\mygame)", "64489cc186743810" },
    { R"(C:/Proj/MyGame/)", R"(c:\proj\mygame\)", R"(c:\proj\mygame)", "64489cc186743810" },
    { R"(C:\Proj\MyGame\/\)", R"(c:\proj\mygame\)", R"(c:\proj\mygame)", "64489cc186743810" },
    { R"(C:\\)", R"(c:\)", R"(c:\)", "3dc8472eaebe1a9d" },
    { R"(\\Server\Share\Proj\)", R"(\\server\share\proj\)", R"(\\server\share\proj)", "289e4848cf3b4716" },
};

// ---- 偽の I/O ----

struct IoCounters {
    int readToken = 0;
    int openPipe = 0;
    int closePipe = 0;
    int write = 0;
    int pump = 0;
    int launch = 0;
    int now = 0; // 時計は I/O ではないので Total に含めない
    int Total() const { return readToken + openPipe + closePipe + write + pump + launch; }
};

struct FakeState {
    IoCounters c;
    int64_t nowMs = 1000;
    bool serviceUp = true;                       // false なら OpenPipe は NotFound
    std::deque<TrackIo::OpenResult> openScript;  // 先に積んだ結果を 1 回ずつ返す
    bool tokenOk = true;
    std::string token = "tokA";
    TrackIo::LaunchResult launchResult = TrackIo::LaunchResult::Launched;
    std::deque<std::string> chunks;              // Pump が 1 回に 1 つ返す
    bool peerClosed = false;
    bool pipeOpen = false;
    bool writeOk = true;
    std::vector<std::string> written;
};

class FakeIo final : public TrackIo {
public:
    explicit FakeIo(std::shared_ptr<FakeState> state) : s_(std::move(state)) {}

    bool ReadServiceToken(const std::wstring&, std::string& tokenOut) override
    {
        ++s_->c.readToken;
        tokenOut = s_->token;
        return s_->tokenOk;
    }
    OpenResult OpenPipe(const std::string&) override
    {
        ++s_->c.openPipe;
        OpenResult r = s_->serviceUp ? OpenResult::Opened : OpenResult::NotFound;
        if (!s_->openScript.empty()) {
            r = s_->openScript.front();
            s_->openScript.pop_front();
        }
        s_->pipeOpen = r == OpenResult::Opened;
        return r;
    }
    void ClosePipe() override
    {
        ++s_->c.closePipe;
        s_->pipeOpen = false;
    }
    bool Write(const std::string& bytes) override
    {
        ++s_->c.write;
        if (!s_->writeOk) {
            return false;
        }
        s_->written.push_back(bytes);
        return true;
    }
    PumpResult Pump(std::string& incoming) override
    {
        ++s_->c.pump;
        if (!s_->pipeOpen) {
            return PumpResult::Closed;
        }
        if (!s_->chunks.empty()) {
            incoming += s_->chunks.front();
            s_->chunks.pop_front();
            return PumpResult::Ok;
        }
        return s_->peerClosed ? PumpResult::Closed : PumpResult::Ok;
    }
    LaunchResult LaunchService(const std::wstring&) override
    {
        ++s_->c.launch;
        return s_->launchResult;
    }
    int64_t NowMs() override
    {
        ++s_->c.now;
        return s_->nowMs;
    }

private:
    std::shared_ptr<FakeState> s_;
};

const wchar_t* const kRoot = L"C:\\mye_track_selftest\\proj"; // 実在しなくてよい (偽の I/O はディスクに触れない)

void FeedJson(FakeState& s, const json& j)
{
    s.chunks.push_back(j.dump() + "\n");
}

// 書き込まれた要求のうち op が合うもの (古い順)
std::vector<json> Requests(const FakeState& s, const char* op)
{
    std::vector<json> out;
    for (const std::string& w : s.written) {
        const json j = json::parse(w, nullptr, false);
        if (j.is_object() && j.value("op", std::string()) == op) {
            out.push_back(j);
        }
    }
    return out;
}

json OkReply(uint64_t id, const json& result)
{
    return json{ { "id", id }, { "ok", true }, { "result", result } };
}

json ErrReply(uint64_t id, const char* code)
{
    return json{ { "id", id }, { "ok", false }, { "error", { { "code", code }, { "detail", "t" } } } };
}

uint64_t LastHelloId(const FakeState& s)
{
    const std::vector<json> hellos = Requests(s, trackop::kHello);
    return hellos.empty() ? 0 : hellos.back()["id"].get<uint64_t>();
}

void ReplyHelloOk(FakeState& s)
{
    FeedJson(s, OkReply(LastHelloId(s), { { "proto", 1 }, { "projectId", ProjectId(kRoot) }, { "role", "editor" } }));
}

// 接続して hello を通す
bool BringReady(TrackClient& client, FakeState& s)
{
    client.Poll();
    ReplyHelloOk(s);
    client.Poll();
    return client.Ready();
}

struct Rig {
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();
    TrackClient client;
    Rig() { client.Start(std::make_unique<FakeIo>(s), kRoot, 4242); }
};

// 1 回の Poll の実時間 (ミリ秒) と TrackIo の呼び出し回数
struct PollCost {
    double ms = 0.0;
    int ioCalls = 0;
};
PollCost TimedPoll(const std::function<void()>& poll, const FakeState& s)
{
    const int before = s.c.Total();
    const auto t0 = std::chrono::steady_clock::now();
    poll();
    PollCost cost;
    cost.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    cost.ioCalls = s.c.Total() - before;
    return cost;
}

std::string CodeOf(const json& response)
{
    return response.contains("error") ? response["error"].value("code", std::string()) : std::string();
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

// ---- 実サービスとの結合 (MYE_TRACK_PROBE=1 のときだけ) ----

DWORD ReadServicePid(const fs::path& root)
{
    std::ifstream f(root / L".mye" / L"ai_changes" / L"service.json", std::ios::binary);
    const json j = f ? json::parse(f, nullptr, false) : json();
    return j.is_object() && j.contains("pid") && j["pid"].is_number_unsigned() ? j["pid"].get<DWORD>() : 0;
}

void RunProbe(const char* mode, const std::function<void(bool, const char*)>& check)
{
    const bool leave = std::string(mode) == "leave"; // サービスを止めずに終わる (Editor の終了後も生きることを手で確かめる)
    const fs::path root = fs::temp_directory_path() / (L"mye_track_probe_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    auto pump = [](ChangeTrackingSession& session, const std::function<bool()>& done, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            session.Poll();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        session.Poll();
        return done();
    };

    HANDLE service = nullptr;
    {
        ChangeTrackingSession session;
        std::vector<json> events;
        session.Start(GetExecutableDir(), root.wstring(), true);
        session.Client().SetNotifyHandler([&](const json& e) {
            events.push_back(e);
            session.Model().OnNotification(e);
        });
        session.Model().SetInterest(kTrackAllParts);
        const bool ready = pump(session, [&] { return session.State() == TrackState::Ready; }, 60000);
        check(ready, "probe: the editor started the service and reached Ready");
        if (ready) {
            const DWORD pid = ReadServicePid(root);
            service = pid != 0 ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid) : nullptr;
            check(service != nullptr, "probe: service.json names a live process");

            std::ofstream(root / L"probe.txt", std::ios::binary) << "one\n";
            const bool notified = pump(session, [&] {
                return std::any_of(events.begin(), events.end(), [](const json& e) {
                    return e.value("event", std::string()) == trackevent::kChangeRecorded && e.value("path", std::string()) == "probe.txt";
                });
            }, 30000);
            check(notified, "probe: change_recorded arrived after writing a file");
            const bool listed = pump(session, [&] {
                const auto& rows = session.Model().Changes();
                return std::any_of(rows.begin(), rows.end(), [](const TrackChangeRow& r) { return r.path == "probe.txt"; });
            }, 15000);
            check(listed, "probe: list shows the file (the model refetched after the notification)");
            check(session.Model().Status().valid && session.Model().Status().trackedFiles >= 1, "probe: status reached the model");
        }
        session.Shutdown(); // 接続を閉じるだけ
        if (service != nullptr) {
            check(WaitForSingleObject(service, 300) == WAIT_TIMEOUT, "probe: the service keeps running after the editor closes its connection");
        }
    }

    if (!leave) {
        ChangeTrackingSession stopper;
        stopper.Start(GetExecutableDir(), root.wstring(), true);
        const bool ready = pump(stopper, [&] { return stopper.State() == TrackState::Ready; }, 30000);
        check(ready, "probe: a new session reconnects to the running service");
        bool answered = false;
        bool shutdownOk = false;
        if (ready) {
            stopper.Client().Request(trackop::kShutdown, json::object(), [&](const json& r) {
                answered = true;
                shutdownOk = r.value("ok", false);
            });
            pump(stopper, [&] { return answered; }, 10000);
        }
        check(shutdownOk, "probe: shutdown (role editor) was accepted");
        const bool exited = service == nullptr || WaitForSingleObject(service, 20000) == WAIT_OBJECT_0;
        check(exited, "probe: no mye_track.exe is left after shutdown");
        if (!exited && service != nullptr) {
            TerminateProcess(service, 1); // この試験が起動したサービスだけを後始末する
        }
        stopper.Shutdown();
    } else {
        MYE_LOG_INFO("probe: leaving the service running (root=%s pid=%lu)", WideToUtf8(root.wstring()).c_str(),
                     static_cast<unsigned long>(ReadServicePid(root)));
    }
    if (service != nullptr) {
        CloseHandle(service);
    }
    if (!leave) {
        fs::remove_all(root, ec);
    }
}

} // namespace

bool RunChangeTrackingSelfTest()
{
    MYE_LOG_INFO("==== Change tracking self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (a) projectId: 固定表の全行 (非 ASCII を含む) と、起動の引用 ----
    {
        bool allKey = true;
        bool allSource = true;
        bool allId = true;
        for (const ProjectIdRow& row : kProjectIdRows) {
            const std::wstring input = Utf8ToWide(row.input);
            allKey = allKey && WideToUtf8(NormalizePathKey(input)) == row.key;
            allSource = allSource && ProjectIdSource(input) == row.source;
            allId = allId && ProjectId(input) == row.id;
            if (ProjectId(input) != row.id) {
                MYE_LOG_ERROR("  projectId differs for %s: %s vs %s", row.input, ProjectId(input).c_str(), row.id);
            }
        }
        check(allKey, "projectId table: NormalizePathKey matches column 2 for every row");
        check(allSource, "projectId table: the source (trailing separators dropped) matches column 3");
        check(allId, "projectId table: the id matches column 4 (non-ASCII rows included)");
        check(std::size(kProjectIdRows) >= 18, "projectId table: all rows are embedded");

        // 表そのものと埋め込みが食い違っていないか (見つからなければ、埋め込みだけで確かめた = 上の検査)
        const fs::path tsv = FindRepoFile(fs::path(L"tools") / L"track" / L"tests" / L"project_id_cases.tsv");
        if (tsv.empty()) {
            MYE_LOG_INFO("  note: project_id_cases.tsv not found from here; the embedded rows were checked");
        } else {
            std::ifstream f(tsv, std::ios::binary);
            std::string line;
            size_t rows = 0;
            bool same = true;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                std::vector<std::string> cols;
                size_t start = 0;
                for (size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1) {
                    cols.push_back(line.substr(start, tab - start));
                }
                cols.push_back(line.substr(start));
                if (rows >= std::size(kProjectIdRows)) {
                    same = false;
                } else {
                    const ProjectIdRow& row = kProjectIdRows[rows];
                    same = same && cols.size() == 4 && cols[0] == row.input && cols[1] == row.key && cols[2] == row.source
                           && cols[3] == row.id;
                }
                ++rows;
            }
            check(same && rows == std::size(kProjectIdRows), "projectId table: the embedded rows equal project_id_cases.tsv");
        }

        check(TrackPipeName("0123456789abcdef") == "\\\\.\\pipe\\mye-track-0123456789abcdef", "pipe name format");
        check(QuoteCommandLineArg(L"C:\\Proj\\") == L"\"C:\\Proj\\\\\"", "quoting: a trailing backslash is doubled before the closing quote");
        check(QuoteCommandLineArg(L"C:\\My Proj") == L"\"C:\\My Proj\"", "quoting: spaces stay inside the quotes");
        check(QuoteCommandLineArg(L"a\"b") == L"\"a\\\"b\"", "quoting: an embedded quote is escaped");
    }

    // ---- (b) 応答の解釈: 順不同・分割・1 回に複数行・空行・不正行・上限超え ----
    {
        Rig r;
        check(BringReady(r.client, *r.s), "responses: a fake service reaches Ready");
        json got[3];
        r.client.Request(trackop::kList, json::object(), [&](const json& j) { got[0] = j; });      // id 2
        r.client.Request(trackop::kDescribe, json::object(), [&](const json& j) { got[1] = j; });  // id 3
        r.client.Request(trackop::kStatus, json::object(), [&](const json& j) { got[2] = j; });    // id 4
        check(r.client.PendingCount() == 3, "responses: three requests are pending");
        // 1 回の読み取りに 3 行 + 空行 + JSON でない行。順番も入れ替える
        r.s->chunks.push_back(OkReply(4, { { "n", 4 } }).dump() + "\n\n" + "this is not json\n" + OkReply(2, { { "n", 2 } }).dump()
                              + "\r\n" + OkReply(3, { { "n", 3 } }).dump() + "\n");
        r.client.Poll();
        check(got[0]["result"]["n"] == 2 && got[1]["result"]["n"] == 3 && got[2]["result"]["n"] == 4,
              "responses: out-of-order ids reach their own callbacks");
        check(r.client.PendingCount() == 0 && r.client.GetStats().badLines == 1,
              "responses: a blank line is skipped and a non-JSON line is counted");

        // 1 行が 3 回の読み取りに分かれる (Poll をまたぐ)
        json split;
        r.client.Request(trackop::kList, json::object(), [&](const json& j) { split = j; }); // id 5
        const std::string line = OkReply(5, { { "text", "0123456789" } }).dump() + "\n";
        const size_t a = line.size() / 3;
        const size_t b = 2 * line.size() / 3;
        r.s->chunks.push_back(line.substr(0, a));
        r.client.Poll();
        check(split.is_null(), "responses: a partial line waits for the rest (1/3)");
        r.s->chunks.push_back(line.substr(a, b - a));
        r.client.Poll();
        check(split.is_null(), "responses: a partial line waits for the rest (2/3)");
        r.s->chunks.push_back(line.substr(b));
        r.client.Poll();
        check(split.value("ok", false) && split["result"]["text"] == "0123456789", "responses: a line split over three reads is reassembled");

        // ちょうど上限の 1 行は通り、上限を超えたら切断する
        int notified = 0;
        r.client.SetNotifyHandler([&](const json&) { ++notified; });
        std::string exact = "{\"event\":\"x\",\"pad\":\"";
        const std::string tail = "\"}";
        exact.append(static_cast<size_t>(kTrackMaxLineBytes) - exact.size() - tail.size(), 'a');
        exact += tail;
        check(exact.size() == static_cast<size_t>(kTrackMaxLineBytes), "responses: the boundary line is exactly 1 MiB");
        r.s->chunks.push_back(exact + "\n");
        r.client.Poll();
        check(notified == 1 && r.client.Ready(), "responses: a line of exactly 1 MiB is accepted");

        json pendingResult;
        r.client.Request(trackop::kList, json::object(), [&](const json& j) { pendingResult = j; });
        r.s->chunks.push_back(std::string(static_cast<size_t>(kTrackMaxLineBytes) + 10, 'x'));
        r.client.Poll();
        check(r.client.State() == TrackState::Disconnected && r.client.Reason() == "line_too_long",
              "responses: a line over 1 MiB disconnects with a reason");
        check(CodeOf(pendingResult) == trackerror::kDisconnected, "responses: the pending request fails when the line is too long");

        // 改行を含む上限超えの行も同じ
        Rig r2;
        BringReady(r2.client, *r2.s);
        r2.s->chunks.push_back(std::string(static_cast<size_t>(kTrackMaxLineBytes) + 10, 'x') + "\n");
        r2.client.Poll();
        check(r2.client.State() == TrackState::Disconnected, "responses: an over-long line that ends with LF also disconnects");
    }

    // ---- (c) hello の失敗ごとの状態 ----
    {
        // proto_mismatch: 再接続しない
        Rig r;
        r.client.Poll();
        FeedJson(*r.s, ErrReply(LastHelloId(*r.s), trackerror::kProtoMismatch));
        r.client.Poll();
        check(r.client.State() == TrackState::ProtoMismatch, "hello: proto_mismatch -> ProtoMismatch");
        const int calls = r.s->c.Total();
        r.s->nowMs += 3600 * 1000;
        for (int i = 0; i < 5; ++i) {
            r.client.Poll();
        }
        check(r.s->c.Total() == calls && r.client.State() == TrackState::ProtoMismatch, "hello: ProtoMismatch never reconnects (no I/O later)");
    }
    {
        // wrong_project と projectId の不一致: 再接続しない
        Rig r;
        r.client.Poll();
        FeedJson(*r.s, ErrReply(LastHelloId(*r.s), trackerror::kWrongProject));
        r.client.Poll();
        check(r.client.State() == TrackState::WrongProject, "hello: wrong_project -> WrongProject");

        Rig r2;
        r2.client.Poll();
        FeedJson(*r2.s, OkReply(LastHelloId(*r2.s), { { "proto", 1 }, { "projectId", "0000000000000000" } }));
        r2.client.Poll();
        check(r2.client.State() == TrackState::WrongProject, "hello: a different projectId in the result -> WrongProject");
        const int calls = r2.s->c.Total();
        r2.s->nowMs += 60 * 1000;
        r2.client.Poll();
        check(r2.s->c.Total() == calls, "hello: WrongProject never reconnects");
    }
    {
        // bad_token: service.json を読み直して 1 回だけやり直し、2 回目は間隔をあける
        Rig r;
        r.s->token = "tokA";
        r.client.Poll();
        FeedJson(*r.s, ErrReply(LastHelloId(*r.s), trackerror::kBadToken));
        r.s->token = "tokB"; // サービスが再起動してトークンが変わった
        r.client.Poll();
        r.client.Poll();
        const std::vector<json> hellos = Requests(*r.s, trackop::kHello);
        check(r.s->c.openPipe == 2 && r.s->c.readToken == 2 && hellos.size() == 2 && hellos[1]["args"]["token"] == "tokB",
              "hello: bad_token rereads service.json and retries once with the new token");
        FeedJson(*r.s, ErrReply(LastHelloId(*r.s), trackerror::kBadToken));
        r.client.Poll();
        const int opens = r.s->c.openPipe;
        for (int i = 0; i < 5; ++i) {
            r.client.Poll();
        }
        check(r.s->c.openPipe == opens && !r.client.Ready(), "hello: a second bad_token waits for the reconnect interval");
        r.s->nowMs += kTrackReconnectIntervalMs;
        r.client.Poll();
        check(r.s->c.openPipe == opens + 1, "hello: ... and then retries");
    }
    {
        // db_degraded: 状態に出し、間隔をおいて再接続を試す
        Rig r;
        r.client.Poll();
        FeedJson(*r.s, ErrReply(LastHelloId(*r.s), trackerror::kDbDegraded));
        r.client.Poll();
        check(r.client.State() == TrackState::DbDegraded, "hello: db_degraded -> DbDegraded");
        const int opens = r.s->c.openPipe;
        r.client.Poll();
        check(r.s->c.openPipe == opens, "hello: DbDegraded waits before reconnecting");
        r.s->nowMs += kTrackReconnectIntervalMs;
        r.client.Poll();
        check(r.s->c.openPipe == opens + 1, "hello: DbDegraded retries after the interval");

        // 接続中の status が db_degraded なら DbDegraded、直れば Ready (接続は保つ)
        Rig r2;
        BringReady(r2.client, *r2.s);
        r2.client.SetDbDegraded(true);
        check(r2.client.State() == TrackState::DbDegraded && r2.client.CanRequest(), "status: a degraded db shows DbDegraded but keeps the connection");
        r2.client.SetDbDegraded(false);
        check(r2.client.Ready(), "status: a recovered db returns to Ready");
    }

    // ---- (d) 時間切れ (偽の時計) ----
    {
        Rig r;
        BringReady(r.client, *r.s);
        std::string readCode;
        std::string recordCode;
        const int64_t t0 = r.s->nowMs;
        r.client.Request(trackop::kList, json::object(), [&](const json& j) { readCode = CodeOf(j); });      // 読み取り: 30 秒
        r.client.Request("editor_saved", json::object(), [&](const json& j) { recordCode = CodeOf(j); });    // 記録: 5 秒
        check(TrackOpTimeoutMs(trackop::kList) == 30000 && TrackOpTimeoutMs("editor_saved") == 5000, "timeouts: read 30 s / record 5 s");
        r.s->nowMs = t0 + kTrackRecordTimeoutMs - 1;
        r.client.Poll();
        check(recordCode.empty() && readCode.empty(), "timeouts: nothing fails just before 5 s");
        r.s->nowMs = t0 + kTrackRecordTimeoutMs;
        r.client.Poll();
        check(recordCode == trackerror::kTimeout && readCode.empty(), "timeouts: the record request fails at 5 s, the read request does not");
        r.s->nowMs = t0 + kTrackReadTimeoutMs - 1;
        r.client.Poll();
        check(readCode.empty(), "timeouts: the read request is still waiting just before 30 s");
        r.s->nowMs = t0 + kTrackReadTimeoutMs;
        r.client.Poll();
        check(readCode == trackerror::kTimeout && r.client.PendingCount() == 0, "timeouts: the read request fails at 30 s");
        FeedJson(*r.s, OkReply(2, json::object())); // 時間切れのあとの応答は黙って数える
        r.client.Poll();
        check(r.client.GetStats().lateResponses == 1 && r.client.Ready(), "timeouts: a late response is ignored and counted");

        // hello に応答が無い (サービスが固まっている): 切って再接続を待つ。フレームは止まらない
        Rig hung;
        double worst = 0.0;
        hung.client.Poll();
        for (int i = 0; i < 100; ++i) {
            hung.s->nowMs += 100;
            const PollCost cost = TimedPoll([&] { hung.client.Poll(); }, *hung.s);
            worst = (std::max)(worst, cost.ms);
            if (cost.ioCalls > kTrackMaxIoCallsPerPoll) {
                check(false, "hung service: a Poll exceeded the I/O call bound");
            }
        }
        check(hung.client.State() == TrackState::Connecting && hung.client.GetStats().timeouts >= 1,
              "hung service: the unanswered hello times out and the client keeps retrying (state only changes)");
        check(worst < 50.0, "hung service: Poll never blocks the frame");
    }

    // ---- (e) 切断 -> 保留中が全部失敗 -> 定数の間隔で再試行 -> 成功。exe が無い場合 ----
    {
        Rig r;
        BringReady(r.client, *r.s);
        std::string code1;
        std::string code2;
        r.client.Request(trackop::kList, json::object(), [&](const json& j) { code1 = CodeOf(j); });
        r.client.Request(trackop::kStatus, json::object(), [&](const json& j) { code2 = CodeOf(j); });
        r.s->peerClosed = true;
        r.s->serviceUp = false;
        r.client.Poll();
        check(r.client.State() == TrackState::Disconnected, "reconnect: a closed pipe -> Disconnected");
        check(code1 == trackerror::kDisconnected && code2 == trackerror::kDisconnected && r.client.PendingCount() == 0,
              "reconnect: every pending request fails with 'disconnected'");

        const int64_t cutAt = r.s->nowMs;
        bool boundOk = true;
        bool idleOk = true;
        int firstAttemptAt = -1;
        for (int frame = 1; frame <= 100; ++frame) {
            r.s->nowMs = cutAt + frame * 100;
            const PollCost cost = TimedPoll([&] { r.client.Poll(); }, *r.s);
            boundOk = boundOk && cost.ioCalls <= kTrackMaxIoCallsPerPoll;
            if (cost.ioCalls > 0 && firstAttemptAt < 0) {
                firstAttemptAt = frame * 100;
            }
            if (frame * 100 < kTrackReconnectIntervalMs) {
                idleOk = idleOk && cost.ioCalls == 0; // 待ちの間は I/O を 1 回も呼ばない
            }
        }
        check(idleOk && firstAttemptAt == kTrackReconnectIntervalMs, "reconnect: no I/O until the reconnect interval, then the first attempt");
        check(boundOk, "reconnect: every frame stays within the I/O call bound");
        check(r.s->c.launch == 1, "reconnect: the service is asked to start only once per outage");
        // サービスが戻る
        r.s->serviceUp = true;
        r.s->peerClosed = false;
        r.s->nowMs += kTrackReconnectIntervalMs;
        r.client.Poll();
        ReplyHelloOk(*r.s);
        r.client.Poll();
        check(r.client.Ready() && r.client.GetStats().connects == 2, "reconnect: Ready again after the service returns");

        // shutting_down を受けたあとの切断では、サービスを起動し直さない (止めた意図を覆さない)
        Rig q;
        BringReady(q.client, *q.s);
        FeedJson(*q.s, json{ { "event", "shutting_down" }, { "reason", "shutdown_op" } });
        q.s->peerClosed = true;
        q.s->serviceUp = false;
        q.client.Poll();
        check(q.client.State() == TrackState::Disconnected, "shutting_down: the closing pipe -> Disconnected");
        for (int i = 1; i <= 50; ++i) {
            q.s->nowMs += 1000;
            q.client.Poll();
        }
        check(q.s->c.launch == 0 && q.s->c.openPipe > 1, "shutting_down: no relaunch after a deliberate stop (it only keeps looking for the pipe)");

        // exe が無い: NoService のまま I/O をしない (探し直しは Start し直すまで)
        Rig n;
        n.s->serviceUp = false;
        n.s->launchResult = TrackIo::LaunchResult::NoExe;
        n.client.Poll();
        check(n.client.State() == TrackState::NoService, "no exe: NoService");
        const int calls = n.s->c.Total();
        n.s->nowMs += 3600 * 1000;
        for (int i = 0; i < 5; ++i) {
            n.client.Poll();
        }
        check(n.s->c.Total() == calls, "no exe: no search again until restarted");
        n.s->launchResult = TrackIo::LaunchResult::Launched;
        n.client.Start(std::make_unique<FakeIo>(n.s), kRoot, 1);
        n.client.Poll();
        check(n.client.State() == TrackState::Connecting && n.s->c.launch == 2, "no exe: Start again searches again");

        // 途中で死ぬ (書き込みが失敗する)
        Rig w;
        BringReady(w.client, *w.s);
        w.s->writeOk = false;
        std::string failure;
        const uint64_t id = w.client.Request(trackop::kList, json::object(), [&](const json& j) { failure = CodeOf(j); });
        check(id == 0 && failure == trackerror::kDisconnected && w.client.State() == TrackState::Disconnected,
              "died mid-way: a failed write returns a failure and waits to reconnect");
    }

    // ---- (f) Poll の量の上限: 1 万行を一度に流し込む ----
    {
        Rig r;
        BringReady(r.client, *r.s);
        int received = 0;
        r.client.SetNotifyHandler([&](const json&) { ++received; });
        for (int chunk = 0; chunk < 100; ++chunk) {
            std::string text;
            for (int i = 0; i < 100; ++i) {
                text += "{\"event\":\"change_recorded\",\"revisionId\":" + std::to_string(chunk * 100 + i) + "}\n";
            }
            r.s->chunks.push_back(std::move(text));
        }
        uint32_t maxLines = 0;
        int polls = 0;
        double worst = 0.0;
        while (received < 10000 && polls < 1000) {
            const PollCost cost = TimedPoll([&] { r.client.Poll(); }, *r.s);
            worst = (std::max)(worst, cost.ms);
            maxLines = (std::max)(maxLines, r.client.GetStats().lastPollLines);
            ++polls;
        }
        check(received == 10000, "poll cap: all 10000 lines are eventually processed");
        check(maxLines <= static_cast<uint32_t>(kTrackMaxLinesPerPoll), "poll cap: one Poll never handles more than kTrackMaxLinesPerPoll lines");
        check(polls >= 10000 / kTrackMaxLinesPerPoll && polls <= 500, "poll cap: it spreads over many frames (a bounded number), not one");
        check(worst < 100.0, "poll cap: a Poll stays short even with 10000 lines waiting");
    }

    // ---- (g) モデル: 通知で古い印、間引き、resync で全部 ----
    {
        auto s = std::make_shared<FakeState>();
        ChangeTrackingSession session;
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, true);
        session.Poll();
        ReplyHelloOk(*s);
        session.Poll(); // Ready: 購読を投げる
        check(session.State() == TrackState::Ready, "model: the session reaches Ready");
        check(Requests(*s, trackop::kSubscribe).size() == 1, "model: Ready subscribes once");
        ChangeModel& model = session.Model();
        model.SetInterest(kTrackAllParts);
        model.SelectRevision(7);

        size_t answered = 0;
        auto answerAll = [&] {
            std::string batch;
            for (; answered < s->written.size(); ++answered) {
                const json req = json::parse(s->written[answered], nullptr, false);
                const std::string op = req.value("op", std::string());
                const uint64_t id = req["id"].get<uint64_t>();
                if (op == trackop::kHello) {
                    continue; // hello は接続のときに返してある
                }
                json result = json::object();
                if (op == trackop::kStatus) {
                    result = { { "phase", "baselining" }, { "trackedFiles", 12 }, { "baseline", { { "scanDone", 3 }, { "scanTotal", 9 } } },
                               { "blobs", { { "count", 5 }, { "bytes", 1000 } } }, { "gaps", { { "count", 2 } } },
                               { "watches", { { "count", 4 }, { "fallback", true } } }, { "db", { { "state", "recording" } } } };
                } else if (op == trackop::kSessions) {
                    result = { { "items", json::array({ { { "sessionId", "s1" }, { "provider", "claude" }, { "status", "running" }, { "startedAt", 5 } } }) } };
                } else if (op == trackop::kList) {
                    result = { { "items", json::array({ { { "revisionId", 9 }, { "path", "a.txt" }, { "changeType", "added" }, { "actor", "ai" }, { "attribution", "claimed" }, { "afterSize", 4 } },
                                                          { { "revisionId", 8 }, { "path", "b.txt" }, { "changeType", "modified" } } }) },
                               { "nextCursor", req["args"].contains("cursor") ? json() : json("8") },
                               { "coverage", { { "baselining", true } } } };
                } else if (op == trackop::kDescribe) {
                    result = { { "revisionId", req["args"]["revisionId"] }, { "hunks", json::array() } };
                }
                batch += OkReply(id, result).dump() + "\n";
            }
            if (!batch.empty()) {
                s->chunks.push_back(batch);
            }
        };
        auto count = [&](const char* op) { return Requests(*s, op).size(); };

        session.Poll(); // Tick: 見ている部分を取りに行く
        check(count(trackop::kStatus) == 1 && count(trackop::kSessions) == 1 && count(trackop::kList) == 1 && count(trackop::kDescribe) == 1,
              "model: the first Tick fetches status / sessions / list / describe once");
        answerAll();
        session.Poll();
        check(model.Status().valid && model.Status().trackedFiles == 12 && model.Status().scanTotal == 9 && model.Status().Baselining()
                  && model.Status().blobBytes == 1000 && model.Status().gapCount == 2 && model.Status().watchFallback,
              "model: status is parsed (tracked files, baseline progress, blobs, gaps, watches)");
        check(model.Sessions().size() == 1 && model.Sessions()[0].sessionId == "s1", "model: sessions are parsed");
        check(model.Changes().size() == 2 && model.Changes()[0].path == "a.txt" && model.Changes()[0].afterSize == 4
                  && model.Changes()[1].beforeSize == -1 && model.HasMoreChanges() && model.ListIncomplete(),
              "model: the list page is parsed (with the cursor and the baselining flag)");
        check(model.Detail().is_object() && model.Detail().value("revisionId", 0) == 7, "model: describe is stored");

        // 間引きの間隔より短い連続の通知で、取り直しは 1 回
        const int64_t t0 = s->nowMs;
        for (int i = 0; i < 10; ++i) {
            s->nowMs = t0 + 20 * i;
            FeedJson(*s, json{ { "event", "change_recorded" }, { "revisionId", 100 + i }, { "path", "x" } });
        }
        session.Poll();
        check(model.IsStale(TrackPart::List) && count(trackop::kList) == 1, "model: change_recorded marks the list stale; no refetch inside the throttle");
        s->nowMs = t0 + kTrackModelMinRefetchMs - 1;
        session.Poll();
        check(count(trackop::kList) == 1, "model: still throttled just before the interval");
        s->nowMs = t0 + kTrackModelMinRefetchMs + 10;
        session.Poll();
        check(count(trackop::kList) == 2 && count(trackop::kStatus) == 2, "model: ten notifications cause exactly one refetch after the interval");
        check(count(trackop::kDescribe) == 1, "model: the detail was not refetched for other revisions");
        answerAll();
        session.Poll();

        // 選んでいる Revision の attribution_updated だけが詳細を古くする
        FeedJson(*s, json{ { "event", "attribution_updated" }, { "revisionId", 8 } });
        session.Poll();
        check(count(trackop::kDescribe) == 1, "model: attribution_updated for another revision leaves the detail alone");
        FeedJson(*s, json{ { "event", "attribution_updated" }, { "revisionId", 7 } });
        session.Poll();
        check(count(trackop::kDescribe) == 2, "model: attribution_updated for the selected revision refetches the detail");
        s->nowMs += kTrackModelMinRefetchMs + 10;
        session.Poll();
        answerAll();
        session.Poll();

        // resync: 何が変わったか分からないので、間引きを待たず全部を取り直す
        const size_t statusBefore = count(trackop::kStatus);
        const size_t sessionsBefore = count(trackop::kSessions);
        const size_t listBefore = count(trackop::kList);
        const size_t describeBefore = count(trackop::kDescribe);
        FeedJson(*s, json{ { "event", "resync" }, { "reason", "queue_overflow" } });
        session.Poll();
        check(count(trackop::kStatus) == statusBefore + 1 && count(trackop::kSessions) == sessionsBefore + 1
                  && count(trackop::kList) == listBefore + 1 && count(trackop::kDescribe) == describeBefore + 1,
              "model: resync refetches everything at once");
        answerAll();
        session.Poll();

        // 見ていない部分は古くなっても取りに行かない
        model.SetInterest(TrackPartMask(TrackPart::Status));
        const size_t listNow = count(trackop::kList);
        FeedJson(*s, json{ { "event", "change_recorded" }, { "revisionId", 200 } });
        s->nowMs += 5000;
        session.Poll();
        check(count(trackop::kList) == listNow && model.IsStale(TrackPart::List), "model: a part nobody looks at is not fetched");
        model.SetInterest(kTrackAllParts);

        // フィルタ: 変えると一覧を取り直し、追加ページは古い応答を捨てる
        TrackListFilter filter;
        filter.attribution = "ai";
        model.SetFilter(filter);
        session.Poll();
        const std::vector<json> lists = Requests(*s, trackop::kList);
        check(lists.back()["args"].value("attribution", std::string()) == "ai", "model: the filter is sent with the list request");
        answerAll();
        session.Poll();
        model.LoadMore();
        const std::vector<json> more = Requests(*s, trackop::kList);
        check(more.back()["args"].value("cursor", std::string()) == "8", "model: LoadMore sends the cursor");
        model.SetFilter(TrackListFilter{}); // 追加ページの応答が届く前にフィルタを変える
        answerAll();
        session.Poll();
        check(model.Changes().size() == 2, "model: a stale page for the old filter is dropped");
    }

    // ---- (h) 無効のときは I/O の口を 1 回も呼ばない ----
    {
        auto s1 = std::make_shared<FakeState>();
        ChangeTrackingSession disabled;
        disabled.StartWithIo(std::make_unique<FakeIo>(s1), kRoot, false);
        for (int i = 0; i < 10; ++i) {
            s1->nowMs += 10000;
            disabled.Poll();
        }
        check(disabled.State() == TrackState::Disabled && s1->c.Total() == 0 && s1->c.now == 0,
              "disabled: the setting off (or a non-interactive launch) never touches pipe / service.json / launch / clock");
        auto s2 = std::make_shared<FakeState>();
        ChangeTrackingSession noProject;
        noProject.StartWithIo(std::make_unique<FakeIo>(s2), L"", true);
        noProject.Poll();
        check(noProject.State() == TrackState::NoProject && s2->c.Total() == 0 && s2->c.now == 0, "no project: NoProject and no I/O");
        // 本物の入口 (Start) でも、無効なら I/O の実体を作らない
        ChangeTrackingSession real;
        real.Start(L"C:\\mye_track_selftest\\bin", kRoot, false);
        real.Poll();
        check(real.State() == TrackState::Disabled, "disabled: Start with enabled = false stays Disabled");

        // 設定: 既定は無効。保存・読み直しで他のキーを壊さない
        EditorSettings defaults;
        check(!defaults.changeTrackingEnabled, "settings: tracking is off by default");
        const fs::path dir = fs::temp_directory_path() / (L"mye_track_settings_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        {
            std::ofstream(dir / L"editor_settings.json", std::ios::binary) << "{\"changeTrackingEnabled\": true, \"snapScale\": 0.25}";
        }
        EditorSettings loaded;
        loaded.Load(dir.wstring());
        check(loaded.changeTrackingEnabled && loaded.snapScale == 0.25f, "settings: changeTrackingEnabled loads");
        loaded.changeTrackingEnabled = false;
        loaded.Save();
        EditorSettings again;
        again.Load(dir.wstring());
        check(!again.changeTrackingEnabled && again.snapScale == 0.25f, "settings: changeTrackingEnabled saves and other keys survive");
        fs::remove_all(dir, ec);
    }

    // ---- (i) 実の入口で exe が無い: NoService になるだけ (MYE_TRACK_EXE を存在しないファイルへ) ----
    {
        wchar_t previous[32768] = {};
        const DWORD had = GetEnvironmentVariableW(kTrackExeEnvVar, previous, static_cast<DWORD>(std::size(previous)));
        SetEnvironmentVariableW(kTrackExeEnvVar, L"C:\\mye_track_selftest\\no_such_mye_track.exe");
        check(FindTrackExe(L"C:\\mye_track_selftest").empty(), "launch: MYE_TRACK_EXE pointing at a missing file means no exe (nothing else is searched)");
        const fs::path root = fs::temp_directory_path() / (L"mye_track_noexe_" + std::to_wstring(GetCurrentProcessId()));
        ChangeTrackingSession session;
        session.Start(L"C:\\mye_track_selftest\\bin", root.wstring(), true);
        const auto t0 = std::chrono::steady_clock::now();
        session.Poll();
        const double pollMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(session.State() == TrackState::NoService && pollMs < 200.0, "launch: no service and no exe -> NoService without blocking");
        if (had > 0 && had < std::size(previous)) {
            SetEnvironmentVariableW(kTrackExeEnvVar, previous);
        } else {
            SetEnvironmentVariableW(kTrackExeEnvVar, nullptr);
        }
    }

    // ---- 実サービスとの結合 (MYE_TRACK_PROBE があるときだけ) ----
    if (const char* probe = std::getenv("MYE_TRACK_PROBE"); probe != nullptr && probe[0] != '\0') {
        RunProbe(probe, check);
    }

    if (failCount == 0) {
        MYE_LOG_INFO("Change tracking self test: ALL PASS");
    } else {
        MYE_LOG_ERROR("Change tracking self test: %d FAILURE(S)", failCount);
    }
    return failCount == 0;
}

} // namespace mye
