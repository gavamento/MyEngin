//====================================================================================
//                          ChangeTrackingSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の Editor 側の回帰テスト (偽の I/O)
//====================================================================================
#include "Editor/ChangeTracking/ChangeTrackingSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwctype>
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
#include "Editor/ChangeTracking/ChangeTrackingFakeIo.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/TrackClient.h"
#include "Editor/ChangeTracking/TrackIo.h"
#include "Editor/ChangeTracking/TrackLaunch.h"
#include "Editor/ChangeTracking/TrackProjectId.h"
#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Editor/SourceControl/ScmHint.h"
#include "Editor/Windows/Project/AiChangesWindow.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"

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

// 偽の I/O は ChangeTrackingFakeIo.h (他のスイートと共有)
using namespace tracktest;

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

// モデルの試験の 1 節ぶんの道具立て: 偽の I/O で Ready まで進め、書かれた要求に偽の応答を返す。節ごとに 1 つ作り、使い回さない
struct ModelRig {
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();
    ChangeTrackingSession session;
    ChangeModel& model = session.Model();
    size_t answered = 0;
    // describe の応答
    std::string describeMode = "lines";
    int describeHunks = 0;
    // hunks の応答 (ページごとの items。最後のページ以外は nextCursor = そのページの最後の ordinal)
    std::vector<json> hunkPages;
    // list の応答の items を差し替える (null なら既定の 2 件)
    json listItems;
    bool failHunks = false;
    size_t hunksServed = 0;

    ModelRig()
    {
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, true);
        session.Poll();
        ReplyHelloOk(*s);
        session.Poll(); // Ready: 購読を投げる
        model.SetInterest(kTrackAllParts);
        model.SelectRevision(7);
    }

    size_t Count(const char* op) const { return Requests(*s, op).size(); }

    // まだ答えていない要求に、まとめて偽の応答を返す
    void AnswerAll()
    {
        std::string batch;
        for (; answered < s->written.size(); ++answered) {
            const json req = json::parse(s->written[answered], nullptr, false);
            const std::string op = req.value("op", std::string());
            const uint64_t id = req["id"].get<uint64_t>();
            if (op == trackop::kHello) {
                continue; // hello は接続のときに返してある
            }
            if (op == trackop::kHunks && failHunks) {
                batch += ErrReply(id, "internal").dump() + "\n";
                continue;
            }
            json result = json::object();
            if (op == trackop::kStatus) {
                result = { { "phase", "baselining" }, { "trackedFiles", 12 }, { "baseline", { { "scanDone", 3 }, { "scanTotal", 9 } } },
                           { "blobs", { { "count", 5 }, { "bytes", 1000 } } }, { "gaps", { { "count", 2 } } },
                           { "watches", { { "count", 4 }, { "fallback", true } } }, { "db", { { "state", "recording" } } } };
            } else if (op == trackop::kSessions) {
                result = { { "items", json::array({ { { "sessionId", "s1" }, { "provider", "claude" }, { "status", "running" }, { "startedAt", 5 }, { "changeCount", 3 } } }) } };
            } else if (op == trackop::kList) {
                result = { { "items", !listItems.is_null() ? listItems
                                                           : json::array({ { { "revisionId", 9 }, { "path", "a.txt" }, { "changeType", "added" }, { "actor", "ai" }, { "attribution", "claimed" },
                                                                             { "afterSize", 4 }, { "changeState", "superseded" } },
                                                                           { { "revisionId", 8 }, { "path", "b.txt" }, { "changeType", "modified" } } }) },
                           { "nextCursor", req["args"].contains("cursor") ? json() : json("8") },
                           { "coverage", { { "baselining", true } } } };
            } else if (op == trackop::kDescribe) {
                json hunks = json::array();
                for (int i = 0; i < describeHunks; ++i) {
                    hunks.push_back({ { "ordinal", i }, { "oldStart", 1 }, { "oldCount", 1 }, { "newStart", 1 }, { "newCount", 1 } });
                }
                result = { { "revisionId", req["args"]["revisionId"] }, { "diffMode", describeMode }, { "hunks", hunks } };
            } else if (op == trackop::kHunks) {
                const size_t page = hunksServed++;
                const json items = page < hunkPages.size() ? hunkPages[page] : json::array();
                const bool more = page + 1 < hunkPages.size();
                result = { { "items", items }, { "nextCursor", more ? json(std::to_string(page)) : json() } };
            }
            batch += OkReply(id, result).dump() + "\n";
        }
        if (!batch.empty()) {
            s->chunks.push_back(batch);
        }
    }
};

// 1 回の Poll の TrackIo の呼び出し回数 (実時間は測らない。時間の上限は偽の時計で確かめる)
struct PollCost {
    int ioCalls = 0;
};
PollCost TimedPoll(const std::function<void()>& poll, const FakeState& s)
{
    const int before = s.c.Total();
    poll();
    PollCost cost;
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
            session.Overlay().OnNotification(e);
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

            // 詳細と Hunk の本文 (実サービスの describe + hunks)。追加の Revision → 追記した変更の Revision
            auto findRow = [&](const char* type) -> int64_t {
                for (const TrackChangeRow& r : session.Model().Changes()) {
                    if (r.path == "probe.txt" && r.changeType == type) {
                        return r.revisionId;
                    }
                }
                return 0;
            };
            auto bodyOf = [&](int64_t revision) {
                session.Model().SelectRevision(revision);
                return pump(session, [&] { return session.Model().HunksComplete() && session.Model().Detail().is_object(); }, 15000);
            };
            const int64_t added = findRow("added");
            const bool addedBody = added != 0 && bodyOf(added);
            const std::vector<TrackHunk>& addedHunks = session.Model().Hunks();
            check(addedBody && addedHunks.size() == 1 && addedHunks[0].beforeText.empty() && addedHunks[0].afterText == "one\n",
                  "probe: describe + hunks bring the new file's body into the model");
            std::ofstream(root / L"probe.txt", std::ios::binary | std::ios::app) << "two\n";
            const bool modifiedListed = pump(session, [&] { return findRow("modified") != 0; }, 30000);
            const int64_t modified = findRow("modified");
            const bool modifiedBody = modifiedListed && bodyOf(modified);
            const std::vector<TrackHunk>& modifiedHunks = session.Model().Hunks();
            check(modifiedBody && modifiedHunks.size() == 1 && modifiedHunks[0].afterText == "two\n" && modifiedHunks[0].beforeText.empty()
                      && modifiedHunks[0].contextPre == "one\n" && modifiedHunks[0].oldCount == 0 && modifiedHunks[0].newCount == 1,
                  "probe: the appended line arrives as a hunk with its context (live service)");
            // 窓を実データで描く (ヘッドレス)
            AiChangesWindow window;
            window.open = true;
            AiChangesHost host;
            host.allowTracking = true;
            host.enabled = true;
            host.projectRoot = root.wstring();
            WithHeadlessImGuiFrame([&] { window.OnImGui(session, host); });
            check(window.GetDiffViewer().Rows().HunkCount() == 1, "probe: the window drew the live model's hunk");

            // Editor の保存の知らせ (実サービス): 保存した内容のハッシュを送ると、その変更の行為者が Editor の利用者になる。
            // 外から書いたファイルは、そのまま「不明」のまま
            const std::wstring savedPath = (root / L"saved_in_editor.txt").wstring();
            std::ofstream(savedPath, std::ios::binary) << "saved in the editor\n";
            std::ofstream(root / L"written_elsewhere.txt", std::ios::binary) << "not the editor\n";
            session.NoteSaved(savedPath);
            auto actorOf = [&](const char* path) {
                for (const TrackChangeRow& r : session.Model().Changes()) {
                    if (r.path == path) {
                        return r.actor;
                    }
                }
                return std::string();
            };
            const bool marked = pump(session, [&] { return actorOf("saved_in_editor.txt") == "editor_user" && !actorOf("written_elsewhere.txt").empty(); }, 30000);
            check(marked && actorOf("saved_in_editor.txt") == "editor_user" && actorOf("written_elsewhere.txt") == "unknown",
                  "probe: a file the editor reported saving is the editor user's change; one written elsewhere stays unknown (live service)");
            window.SetShowEditorChanges(true);
            WithHeadlessImGuiFrame([&] { window.OnImGui(session, host); });
            check(window.DrawnItemCount() >= 2, "probe: the window lists the editor section with the live model");

            // シーンのファイルを外から書き換える → scene_overlay に出る → オーバーレイの表に入る (実サービス)
            const fs::path scenePath = root / L"assets" / L"probe.scene.json";
            fs::create_directories(scenePath.parent_path(), ec);
            auto sceneText = [](const char* text) {
                return std::string("{\"engine\":\"MyEngine\",\"entities\":[{\"childIndex\":0,\"components\":{\"Note\":{\"text\":\"") + text
                       + "\"}},\"fileId\":1,\"name\":\"A\"}],\"nextFileId\":2,\"sceneName\":\"s\",\"version\":4}";
            };
            std::ofstream(scenePath, std::ios::binary) << sceneText("x");
            session.Overlay().SetDocumentPath(scenePath.wstring());
            const bool sceneSeen = pump(session, [&] {
                const auto& rows = session.Model().Changes();
                return std::any_of(rows.begin(), rows.end(), [](const TrackChangeRow& r) { return r.path == "assets/probe.scene.json"; });
            }, 30000);
            check(sceneSeen, "probe: the new scene file is recorded");
            std::ofstream(scenePath, std::ios::binary) << sceneText("y");
            const bool overlaid = pump(session, [&] { return session.Overlay().BadgeFor(1).status == OverlayStatus::Modified; }, 30000);
            check(overlaid && !session.Overlay().DocumentId().empty() && session.Overlay().Counts().modified == 1,
                  "probe: a scene rewritten from outside appears in scene_overlay and in the overlay table (live service)");
            session.Overlay().SetDocumentPath(std::wstring());
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

// ---- Editor の保存の知らせ (editor_saved) ----
// 実ファイルを一時のプロジェクトへ書き、偽の I/O へ送られた行を確かめる。節ごとに道具立てを作り直す。

struct SavedRig {
    fs::path root;
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();
    ChangeTrackingSession session;
    int hashCalls = 0;

    // enabled / connect: 設定で有効か、Ready まで進めるか。root が空なら、プロジェクトを開いていない状態
    explicit SavedRig(const std::string& name, bool enabled = true, bool connect = true, bool noProject = false)
    {
        root = fs::temp_directory_path() / (L"mye_track_saved_" + Utf8ToWide(name) + L"_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / L"assets", ec);
        // 本物のハッシュ関数を、呼び出し回数つきで使う
        session.SetSavedFileHasher([this](const std::wstring& path, std::string& hex) {
            ++hashCalls;
            return HashFileForSavedNotice(path, hex);
        });
        session.StartWithIo(std::make_unique<FakeIo>(s), noProject ? std::wstring() : root.wstring(), enabled);
        if (connect) {
            session.Poll();
            FeedJson(*s, OkReply(LastHelloId(*s), { { "proto", 1 }, { "projectId", ProjectId(root.wstring()) }, { "role", "editor" } }));
            session.Poll();
        }
    }
    ~SavedRig()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // プロジェクトの相対パスにファイルを書く。絶対パスを返す
    std::wstring Write(const std::wstring& rel, const std::string& bytes) const
    {
        const fs::path path = root / rel;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary) << bytes;
        return path.wstring();
    }
    std::vector<json> Sent() const { return Requests(*s, trackop::kEditorSaved); }
};

void RunSavedNoticeTests(const std::function<void(bool, const char*)>& check)
{
    // (k1) 押さえた経路ごとに、保存すると editor_saved が 1 回送られ、ハッシュが書いた内容と一致する。
    //      経路はどれも「保存直後の通知の集まる場所」へ絶対パスを渡す (scmhint の受け口 = EditorApp::NoteFileSaved、
    //      シーンとアクターの保存はそこへ直接)。ここでは、その受け口を ChangeTrackingSession::NoteSaved へ繋いで確かめる
    struct Route {
        const char* name;
        const wchar_t* rel;
        const char* bytes;
        bool viaHub; // 偽なら EditorApp が直接呼ぶ経路 (シーン・アクターの保存)
    };
    const Route routes[] = {
        { "scene save", L"assets\\scenes\\main.scene.json", "{\"version\":4,\"entities\":[]}\n", false },
        { "actor save", L"assets\\prefabs\\enemy.prefab.json", "{\"actor\":true}\n", false },
        { "prefab apply", L"assets\\prefabs\\ship.prefab.json", "{\"applied\":1}\n", true },
        { "asset .meta write", L"assets\\textures\\hero.png.meta", "{\"guid\":\"abc\",\"version\":2}\n", true },
        { "asset move (destination)", L"assets\\moved\\rock.mesh.json", "{\"moved\":true}\n", true },
        { "asset rename (destination)", L"assets\\renamed.mat.json", "{\"mat\":1}\n", true },
        { "settings file", L"assets\\project_settings.json", "{\"tags\":[\"a\"]}\n", true },
        { "inspector asset save (non-ASCII path)", L"assets\\シーン\\メイン.sound.json", "{\"sound\":\"鳴る\"}\n", true },
    };
    for (const Route& route : routes) {
        SavedRig r(std::string("route") + std::to_string(&route - routes));
        const std::wstring abs = r.Write(route.rel, route.bytes);
        if (route.viaHub) {
            scmhint::SetSink([](void* user, const std::wstring& path) { static_cast<ChangeTrackingSession*>(user)->NoteSaved(path); }, &r.session);
            scmhint::Changed(abs);
            scmhint::SetSink(nullptr, nullptr);
        } else {
            r.session.NoteSaved(abs);
        }
        check(r.session.PendingSavedCount() == 1 && r.hashCalls == 0 && r.Sent().empty(),
              (std::string("saved notice: ") + route.name + ": noting a save only queues it (no hashing, no request inside the save)").c_str());
        r.session.Poll();
        const std::vector<json> sent = r.Sent();
        check(sent.size() == 1 && sent[0]["args"].value("path", std::string()) == WideToUtf8(abs)
                  && sent[0]["args"].value("hash", std::string()) == Sha256Hex(route.bytes) && sent[0]["args"].size() == 2 && r.hashCalls == 1,
              (std::string("saved notice: ") + route.name + ": one editor_saved with the path and the SHA-256 of what was written").c_str());
        r.session.Poll();
        check(r.Sent().size() == 1 && r.session.PendingSavedCount() == 0, (std::string("saved notice: ") + route.name + ": it is sent once, not again").c_str());
    }

    // (k2) 追跡が無効 / 非対話起動 / プロジェクト無し / 接続前 / 切断中は、送られず、ハッシュの計算も呼ばれない
    {
        SavedRig off("off", false, false); // 設定で無効 (非対話起動も同じ Disabled)
        off.session.NoteSaved(off.Write(L"assets\\a.scene.json", "a"));
        off.session.Poll();
        check(off.session.State() == TrackState::Disabled && off.hashCalls == 0 && off.Sent().empty() && off.session.PendingSavedCount() == 0 && off.s->c.Total() == 0,
              "saved notice: with tracking off (or a non-interactive launch) nothing is hashed, queued or sent, and no I/O is touched");
        SavedRig none("noproject", true, false, true);
        none.session.NoteSaved(none.Write(L"assets\\a.scene.json", "a"));
        none.session.Poll();
        check(none.session.State() == TrackState::NoProject && none.hashCalls == 0 && none.Sent().empty(), "saved notice: with no project open nothing is hashed or sent");
        SavedRig connecting("connecting", true, false);
        connecting.session.Poll(); // pipe は開いたが hello の応答はまだ
        connecting.session.NoteSaved(connecting.Write(L"assets\\a.scene.json", "a"));
        connecting.session.Poll();
        check(connecting.session.State() == TrackState::Connecting && connecting.hashCalls == 0 && connecting.Sent().empty(),
              "saved notice: before the service has answered hello nothing is hashed or sent");
        SavedRig lost("lost");
        lost.s->peerClosed = true;
        lost.session.Poll();
        check(lost.session.State() == TrackState::Disconnected, "saved notice: (setup) the connection is lost");
        lost.session.NoteSaved(lost.Write(L"assets\\a.scene.json", "a"));
        lost.session.Poll();
        check(lost.hashCalls == 0 && lost.Sent().empty() && lost.session.PendingSavedCount() == 0, "saved notice: while disconnected nothing is hashed, queued or sent");
    }

    // (k3) 切れたのが、知らせを積んだあとでも、ハッシュは計算しない
    {
        SavedRig r("dropped");
        r.session.NoteSaved(r.Write(L"assets\\a.scene.json", "a"));
        r.s->peerClosed = true;
        r.session.Poll();
        check(r.session.State() == TrackState::Disconnected && r.hashCalls == 0 && r.Sent().empty() && r.session.PendingSavedCount() == 0,
              "saved notice: a connection lost after queueing drops the queue without hashing");
    }

    // (k4) プロジェクトの外と .mye の下は、積みもしない
    {
        SavedRig r("outside");
        const fs::path outside = r.root.parent_path() / (r.root.filename().wstring() + L"_sibling.txt");
        {
            std::ofstream(outside, std::ios::binary) << "x";
        }
        r.session.NoteSaved(outside.wstring());
        r.session.NoteSaved(r.Write(L".mye\\ai_changes\\tracking.db", "db"));
        r.session.NoteSaved(r.root.wstring()); // ルート自身
        r.session.NoteSaved(L"");
        r.session.Poll();
        std::error_code ec;
        fs::remove(outside, ec);
        check(r.hashCalls == 0 && r.Sent().empty() && r.session.PendingSavedCount() == 0,
              "saved notice: a path outside the project, under .mye, the root itself or empty is ignored without hashing");
        // 名前が .mye で始まるだけのものは対象
        r.session.NoteSaved(r.Write(L".myeignore", "x"));
        check(r.session.PendingSavedCount() == 1, "saved notice: a file whose name merely starts with .mye is tracked");
    }

    // (k5) 同じファイルの通知は 1 件にまとまる (綴りの大小・区切りの違いも)。保存のたびに何度呼ばれても 1 回
    {
        SavedRig r("dedupe");
        const std::wstring abs = r.Write(L"assets\\scenes\\main.scene.json", "{}\n");
        std::wstring upper = abs;
        for (wchar_t& c : upper) {
            c = static_cast<wchar_t>(towupper(c));
        }
        std::wstring slashes = abs;
        for (wchar_t& c : slashes) {
            c = c == L'\\' ? L'/' : c;
        }
        r.session.NoteSaved(abs);
        r.session.NoteSaved(upper);
        r.session.NoteSaved(slashes);
        check(r.session.PendingSavedCount() == 1, "saved notice: the same file noted three times (case, separators) is one entry");
        r.session.Poll();
        check(r.Sent().size() == 1 && r.hashCalls == 1, "saved notice: so it is hashed and sent once");
    }

    // (k6) 読めない・大きすぎるものは送らない (保存は失敗しない。その変更は不明 / 推定に落ちるだけ)
    {
        SavedRig r("unreadable");
        r.session.NoteSaved((r.root / L"assets" / L"gone.scene.json").wstring()); // 移動元・削除したもの
        r.session.NoteSaved((r.root / L"assets").wstring());                      // フォルダの移動
        r.session.NoteSaved(r.Write(L"assets\\big.bin", std::string(static_cast<size_t>(kTrackSavedHashMaxBytes) + 1, 'x')));
        r.session.NoteSaved(r.Write(L"assets\\edge.bin", std::string(static_cast<size_t>(kTrackSavedHashMaxBytes), 'y')));
        for (int i = 0; i < 3; ++i) {
            r.session.Poll();
        }
        check(r.Sent().size() == 1 && r.Sent()[0]["args"].value("hash", std::string()) == Sha256Hex(std::string(static_cast<size_t>(kTrackSavedHashMaxBytes), 'y')),
              "saved notice: a missing file, a folder and a file over the size cap are not sent; one exactly at the cap is");
        check(r.session.State() == TrackState::Ready, "saved notice: unreadable files never disturb the connection");
    }

    // (k7) 1 回の Poll で扱う件数に上限がある (フレームを止めない)。順序はパスのキー順で決まる
    {
        SavedRig r("budget");
        for (int i = 0; i < 5; ++i) {
            r.session.NoteSaved(r.Write(L"assets\\f" + std::to_wstring(i) + L".json", "n" + std::to_string(i)));
        }
        std::vector<size_t> sentAfter;
        for (int i = 0; i < 3; ++i) {
            r.session.Poll();
            sentAfter.push_back(r.Sent().size());
        }
        check(sentAfter == std::vector<size_t>({ static_cast<size_t>(kTrackSavedPerPoll), static_cast<size_t>(kTrackSavedPerPoll) * 2, 5 }) && r.session.PendingSavedCount() == 0,
              "saved notice: one Poll handles at most kTrackSavedPerPoll files; the rest follow in later frames");
        const std::vector<json> sent = r.Sent();
        check(sent.size() == 5 && sent[0]["args"].value("hash", std::string()) == Sha256Hex("n0") && sent[4]["args"].value("hash", std::string()) == Sha256Hex("n4"),
              "saved notice: the order is by path key, not by a hash container's order");
    }
    {
        SavedRig r("cap");
        for (size_t i = 0; i < kTrackSavedMaxPending + 5; ++i) {
            r.session.NoteSaved((r.root / L"assets" / (L"q" + std::to_wstring(1000 + i) + L".json")).wstring());
        }
        check(r.session.PendingSavedCount() == kTrackSavedMaxPending, "saved notice: the queue has an upper bound");
    }

    // (k8) サービスが断っても (古い版・役割違い)、接続は保たれ、保存の側には何も返らない
    {
        SavedRig r("refused");
        r.session.NoteSaved(r.Write(L"assets\\a.json", "a"));
        r.session.Poll();
        const std::vector<json> sent = r.Sent();
        check(sent.size() == 1, "saved notice: (setup) one request is out");
        FeedJson(*r.s, ErrReply(sent[0]["id"].get<uint64_t>(), "unknown_op"));
        r.session.Poll();
        check(r.session.State() == TrackState::Ready && r.session.Client().PendingCount() <= 1, "saved notice: a refusal is only logged; the connection stays Ready");
    }

    // (k9) 保存を知らされた時点のファイルの大きさと更新時刻を控え、ハッシュの前後で同じでなければ送らない
    //      (その間の外の書き込みを、Editor の利用者の保存として確定しない)
    {
        SavedRig r("rewritten");
        const std::wstring abs = r.Write(L"assets\\a.scene.json", "original");
        r.session.NoteSaved(abs);
        r.Write(L"assets\\a.scene.json", "rewritten by someone else"); // 控えたあとの書き換え (大きさが違う)
        r.session.Poll();
        check(r.Sent().empty() && r.hashCalls == 0 && r.session.PendingSavedCount() == 0,
              "saved notice: a file rewritten after the save was noted (different size) is not sent, and is not even hashed");
    }
    {
        SavedRig r("sametime");
        const std::wstring abs = r.Write(L"assets\\a.scene.json", "AAAA");
        r.session.NoteSaved(abs);
        r.Write(L"assets\\a.scene.json", "BBBB"); // 同じ大きさ。更新時刻だけが違う
        std::error_code ec;
        fs::last_write_time(abs, fs::last_write_time(abs, ec) + std::chrono::seconds(5), ec);
        r.session.Poll();
        check(r.Sent().empty(), "saved notice: a same-size rewrite with a newer modification time is not sent");
    }
    {
        SavedRig r("during");
        const std::wstring abs = r.Write(L"assets\\a.scene.json", "before hashing");
        // ハッシュを取った直後に、別のプロセスが書き換える (直前の確認は通り、直後の確認で気づく)
        r.session.SetSavedFileHasher([&r, abs](const std::wstring& path, std::string& hex) {
            const bool ok = HashFileForSavedNotice(path, hex);
            r.Write(L"assets\\a.scene.json", "changed while the hash was taken");
            return ok;
        });
        r.session.NoteSaved(abs);
        r.session.Poll();
        check(r.Sent().empty(), "saved notice: a file rewritten while it was being hashed is not sent");
    }
    {
        SavedRig r("untouched");
        const std::wstring abs = r.Write(L"assets\\a.scene.json", "stable");
        r.session.NoteSaved(abs);
        r.session.Poll();
        check(r.Sent().size() == 1 && r.Sent()[0]["args"].value("hash", std::string()) == Sha256Hex("stable"),
              "saved notice: an untouched file is still sent with its hash (the stamps match)");
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
        hung.client.Poll();
        for (int i = 0; i < 100; ++i) {
            hung.s->nowMs += 100;
            const PollCost cost = TimedPoll([&] { hung.client.Poll(); }, *hung.s);
            if (cost.ioCalls > kTrackMaxIoCallsPerPoll) {
                check(false, "hung service: a Poll exceeded the I/O call bound");
            }
        }
        check(hung.client.State() == TrackState::Connecting && hung.client.GetStats().timeouts >= 1,
              "hung service: the unanswered hello times out and the client keeps retrying (state only changes)");
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
        while (received < 10000 && polls < 1000) {
            r.client.Poll();
            maxLines = (std::max)(maxLines, r.client.GetStats().lastPollLines);
            ++polls;
        }
        check(received == 10000, "poll cap: all 10000 lines are eventually processed");
        check(maxLines <= static_cast<uint32_t>(kTrackMaxLinesPerPoll), "poll cap: one Poll never handles more than kTrackMaxLinesPerPoll lines");
        // 時計が進まない (時間の上限に当たらない) ので、行数の上限 256 だけで区切られる: ceil(10000 / 256) = 40 フレームちょうど
        check(polls == (10000 + kTrackMaxLinesPerPoll - 1) / kTrackMaxLinesPerPoll && maxLines == static_cast<uint32_t>(kTrackMaxLinesPerPoll),
              "poll cap: with a stopped clock the line cap alone splits 10000 lines into exactly 40 frames");
    }

    // ---- (f2) Poll の時間の上限: 偽の時計で、当たる場合と当たらない場合を固定のフレーム数で確かめる ----
    {
        // 時計が 1 読み取りごとに 1 ms 進む: 開始 (t=0) のあと 1 行ごとに 1 ms, 2 ms, 3 ms。3 ms > 2 ms で 3 行目の後に止まる
        Rig r;
        BringReady(r.client, *r.s);
        int received = 0;
        r.client.SetNotifyHandler([&](const json&) { ++received; });
        std::string text;
        for (int i = 0; i < 100; ++i) {
            text += "{\"event\":\"change_recorded\",\"revisionId\":" + std::to_string(i) + "}\n";
        }
        r.s->chunks.push_back(std::move(text));
        r.s->microsPerRead = 1000;
        uint32_t maxLines = 0;
        int polls = 0;
        while (received < 100 && polls < 1000) {
            r.client.Poll();
            maxLines = (std::max)(maxLines, r.client.GetStats().lastPollLines);
            ++polls;
        }
        check(received == 100 && maxLines == 3 && polls == 34,
              "poll budget: a clock that advances 1 ms per read stops each Poll after 3 lines (100 lines = exactly 34 frames)");

        // 2 ms ちょうどは「超えた」に含めない (> 2000 us): 1 読み取り 500 us なら 1, 2, 3, 4 行目の後が 500, 1000, 1500, 2000 で、5 行目の後 2500 で止まる
        Rig edge;
        BringReady(edge.client, *edge.s);
        int edgeReceived = 0;
        edge.client.SetNotifyHandler([&](const json&) { ++edgeReceived; });
        std::string six;
        for (int i = 0; i < 6; ++i) {
            six += "{\"event\":\"change_recorded\"}\n";
        }
        edge.s->chunks.push_back(std::move(six));
        edge.s->microsPerRead = kTrackPollBudget.count() / 4;
        edge.client.Poll();
        check(edge.client.GetStats().lastPollLines == 5 && edgeReceived == 5, "poll budget: exactly the budget is not over it (stops on the first line past it)");

        // 少ない行数で時計も進まない: 上限のどちらにも当たらず 1 フレームで全部
        Rig few;
        BringReady(few.client, *few.s);
        int fewReceived = 0;
        few.client.SetNotifyHandler([&](const json&) { ++fewReceived; });
        std::string small;
        for (int i = 0; i < 10; ++i) {
            small += "{\"event\":\"change_recorded\"}\n";
        }
        few.s->chunks.push_back(std::move(small));
        few.client.Poll();
        check(fewReceived == 10 && few.client.GetStats().lastPollLines == 10, "poll budget: under both caps, one Poll handles everything");
    }

    // ---- (g) モデル: 節ごとに独立 (それぞれが自分の偽の I/O とセッションを持ち、前の節の状態に依存しない) ----
    {
        // (g1) 最初の Tick で見ている部分を 1 回ずつ取りに行き、応答を解釈して保持する
        ModelRig r;
        check(r.session.State() == TrackState::Ready, "model: the session reaches Ready");
        check(Requests(*r.s, trackop::kSubscribe).size() == 1, "model: Ready subscribes once");
        r.session.Poll(); // Tick: 見ている部分を取りに行く
        check(r.Count(trackop::kStatus) == 1 && r.Count(trackop::kSessions) == 1 && r.Count(trackop::kList) == 1 && r.Count(trackop::kDescribe) == 1,
              "model: the first Tick fetches status / sessions / list / describe once");
        r.AnswerAll();
        r.session.Poll();
        const ChangeModel& model = r.model;
        check(model.Status().valid && model.Status().trackedFiles == 12 && model.Status().scanTotal == 9 && model.Status().Baselining()
                  && model.Status().blobBytes == 1000 && model.Status().gapCount == 2 && model.Status().watchFallback,
              "model: status is parsed (tracked files, baseline progress, blobs, gaps, watches)");
        check(model.Sessions().size() == 1 && model.Sessions()[0].sessionId == "s1" && model.Sessions()[0].changeCount == 3,
              "model: sessions are parsed (with the change count)");
        check(model.Changes().size() == 2 && model.Changes()[0].path == "a.txt" && model.Changes()[0].afterSize == 4
                  && model.Changes()[1].beforeSize == -1 && model.HasMoreChanges() && model.ListIncomplete()
                  && model.Changes()[0].changeState == "superseded",
              "model: the list page is parsed (with the cursor, the baselining flag and the change state)");
        check(model.Detail().is_object() && model.Detail().value("revisionId", 0) == 7, "model: describe is stored");
    }
    {
        // (g2) 間引きの間隔より短い連続の通知で、取り直しは 1 回
        ModelRig r;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        const int64_t t0 = r.s->nowMs;
        for (int i = 0; i < 10; ++i) {
            r.s->nowMs = t0 + 20 * i;
            FeedJson(*r.s, json{ { "event", "change_recorded" }, { "revisionId", 100 + i }, { "path", "x" } });
        }
        r.session.Poll();
        check(r.model.IsStale(TrackPart::List) && r.Count(trackop::kList) == 1, "model: change_recorded marks the list stale; no refetch inside the throttle");
        r.s->nowMs = t0 + kTrackModelMinRefetchMs - 1;
        r.session.Poll();
        check(r.Count(trackop::kList) == 1, "model: still throttled just before the interval");
        r.s->nowMs = t0 + kTrackModelMinRefetchMs + 10;
        r.session.Poll();
        check(r.Count(trackop::kList) == 2 && r.Count(trackop::kStatus) == 2, "model: ten notifications cause exactly one refetch after the interval");
        check(r.Count(trackop::kDescribe) == 1, "model: the detail was not refetched for other revisions");
    }
    {
        // (g3) 選んでいる Revision の attribution_updated だけが詳細を古くする
        ModelRig r;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        FeedJson(*r.s, json{ { "event", "attribution_updated" }, { "revisionId", 8 } });
        r.session.Poll();
        check(r.Count(trackop::kDescribe) == 1, "model: attribution_updated for another revision leaves the detail alone");
        FeedJson(*r.s, json{ { "event", "attribution_updated" }, { "revisionId", 7 } });
        r.s->nowMs += kTrackModelMinRefetchMs + 10; // 間引きの間隔をあける
        r.session.Poll();
        check(r.Count(trackop::kDescribe) == 2, "model: attribution_updated for the selected revision refetches the detail");
    }
    {
        // (g4) resync: 何が変わったか分からないので、間引きを待たず全部を取り直す
        ModelRig r;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        const size_t statusBefore = r.Count(trackop::kStatus);
        const size_t sessionsBefore = r.Count(trackop::kSessions);
        const size_t listBefore = r.Count(trackop::kList);
        const size_t describeBefore = r.Count(trackop::kDescribe);
        FeedJson(*r.s, json{ { "event", "resync" }, { "reason", "queue_overflow" } });
        r.session.Poll();
        check(r.Count(trackop::kStatus) == statusBefore + 1 && r.Count(trackop::kSessions) == sessionsBefore + 1
                  && r.Count(trackop::kList) == listBefore + 1 && r.Count(trackop::kDescribe) == describeBefore + 1,
              "model: resync refetches everything at once");
    }
    {
        // (g5) 見ていない部分は古くなっても取りに行かない
        ModelRig r;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        r.model.SetInterest(TrackPartMask(TrackPart::Status));
        const size_t listNow = r.Count(trackop::kList);
        FeedJson(*r.s, json{ { "event", "change_recorded" }, { "revisionId", 200 } });
        r.s->nowMs += 5000;
        r.session.Poll();
        check(r.Count(trackop::kList) == listNow && r.model.IsStale(TrackPart::List), "model: a part nobody looks at is not fetched");
    }
    {
        // (g6) フィルタ: 変えると一覧を取り直し、追加ページは古い応答を捨てる
        ModelRig r;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        TrackListFilter filter;
        filter.attributions = { "claimed" };
        r.model.SetFilter(filter);
        r.session.Poll();
        check(Requests(*r.s, trackop::kList).back()["args"].value("attribution", std::string()) == "claimed", "model: a single attribution is sent as a string");
        r.AnswerAll();
        r.session.Poll();
        TrackListFilter two;
        two.attributions = { "unknown", "inferred" };
        r.model.SetFilter(two);
        r.session.Poll();
        check(Requests(*r.s, trackop::kList).back()["args"]["attribution"] == json::array({ "unknown", "inferred" }),
              "model: two attributions are sent as an array (one request, not two lists to merge)");
        r.AnswerAll();
        r.session.Poll();
        r.model.LoadMore();
        check(Requests(*r.s, trackop::kList).back()["args"].value("cursor", std::string()) == "8", "model: LoadMore sends the cursor");
        r.model.SetFilter(TrackListFilter{}); // 追加ページの応答が届く前にフィルタを変える
        r.AnswerAll();
        r.session.Poll();
        check(r.model.Changes().size() == 2, "model: a stale page for the old filter is dropped");
    }
    {
        // (g7) Hunk の本文: describe の後に全ページを取る。行の無い diffMode では取りに行かない。失敗・古い応答
        ModelRig r;
        r.describeMode = "lines";
        r.describeHunks = 1;
        r.hunkPages = { json::array({ { { "hunkId", 1 }, { "ordinal", 0 }, { "oldStart", 2 }, { "oldCount", 1 }, { "newStart", 2 }, { "newCount", 1 },
                                        { "beforeText", "a\n" }, { "afterText", "b\n" }, { "contextPre", "x\n" }, { "contextPost", "" }, { "eolOnly", false } } }),
                        json::array({ { { "hunkId", 2 }, { "ordinal", 1 }, { "oldStart", 9 }, { "oldCount", 2 }, { "newStart", 9 }, { "newCount", 2 }, { "oversize", true },
                                        { "beforeBytes", 900000 }, { "afterBytes", 800000 }, { "eolOnly", false } } }) };
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll(); // describe の応答 → hunks 1 ページ目を要求
        check(r.Count(trackop::kHunks) == 1 && !r.model.HunksComplete(), "hunks: describe is followed by a hunks request, and the model is not complete yet");
        r.AnswerAll();
        r.session.Poll(); // 1 ページ目の応答 → 2 ページ目を要求
        const std::vector<json> asks = Requests(*r.s, trackop::kHunks);
        check(asks.size() == 2 && asks[1]["args"].value("cursor", std::string()) == "0", "hunks: the next page is asked with the cursor");
        r.AnswerAll();
        r.session.Poll();
        const std::vector<TrackHunk>& hunks = r.model.Hunks();
        check(r.model.HunksComplete() && !r.model.HunksFailed() && hunks.size() == 2 && hunks[0].beforeText == "a\n" && hunks[0].contextPre == "x\n"
                  && !hunks[0].oversize && hunks[1].oversize && hunks[1].beforeBytes == 900000 && hunks[1].beforeText.empty(),
              "hunks: every page is joined, and an oversize hunk has sizes but no body");
        const uint64_t serial = r.model.HunksSerial();
        const std::vector<DiffHunkInput> inputs = ToDiffHunks(hunks);
        check(inputs.size() == 2 && inputs[0].before == "a\n" && inputs[1].oversize && inputs[1].afterBytes == 800000, "hunks: the model's hunks convert to DiffView input");
        r.model.SelectRevision(8);
        check(r.model.Hunks().empty() && !r.model.HunksComplete() && r.model.HunksSerial() != serial, "hunks: selecting another revision clears the hunks");
    }
    {
        ModelRig r;
        r.describeMode = "binary";
        r.describeHunks = 0;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        check(r.Count(trackop::kHunks) == 0 && r.model.HunksComplete() && r.model.Hunks().empty(), "hunks: a binary change asks for no hunks");
    }
    {
        ModelRig r;
        r.describeMode = "lines";
        r.describeHunks = 1;
        r.failHunks = true;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        check(r.model.HunksFailed() && !r.model.HunksComplete() && r.model.Detail().is_object(), "hunks: a failed page marks the hunks failed; describe stays shown");
    }
    {
        // 選び直したあとに届いた古い Hunk のページは捨てる
        ModelRig r;
        r.describeMode = "lines";
        r.describeHunks = 1;
        r.hunkPages = { json::array({ { { "hunkId", 1 }, { "ordinal", 0 }, { "oldStart", 1 }, { "oldCount", 1 }, { "newStart", 1 }, { "newCount", 1 },
                                        { "beforeText", "old\n" }, { "afterText", "new\n" }, { "contextPre", "" }, { "contextPost", "" }, { "eolOnly", false } } }) };
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll(); // describe の応答 → hunks を要求 (まだ答えない)
        r.model.SelectRevision(99);
        r.AnswerAll();
        r.session.Poll();
        check(r.model.Hunks().empty() && !r.model.HunksComplete(), "hunks: a page for a revision that is no longer selected is dropped");
    }

    // ---- (g8) 窓: 閉じている間は何も取りに行かず、開くと取りに行く。無効の間は窓を開いても I/O の口を呼ばない ----
    {
        ModelRig r;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        AiChangesWindow window;
        window.open = false;
        window.SyncInterest(r.model, r.session.State());
        FeedJson(*r.s, json{ { "event", "change_recorded" }, { "revisionId", 300 } });
        r.s->nowMs += 5000;
        const size_t statusNow = r.Count(trackop::kStatus);
        const size_t listNow = r.Count(trackop::kList);
        const size_t sessionsNow = r.Count(trackop::kSessions);
        r.session.Poll();
        check(r.Count(trackop::kList) == listNow && r.Count(trackop::kStatus) == statusNow && r.Count(trackop::kSessions) == sessionsNow
                  && r.model.IsStale(TrackPart::List),
              "window: while closed, the model fetches nothing (SetInterest(0)) although notifications mark parts stale");
        window.open = true;
        window.SyncInterest(r.model, r.session.State());
        r.session.Poll();
        check(r.Count(trackop::kList) == listNow + 1, "window: opening it makes the model fetch what is stale");
        window.open = false;
        window.SyncInterest(r.model, r.session.State());
        check(!window.open, "window: it is closed by default");
        AiChangesWindow fresh;
        check(!fresh.open, "window: a new window starts closed");
    }
    {
        // (g8b) 「セッションなし (不明 / 推定)」は、読み込み済みの行から拾わず、サービスの attribution フィルタで取る
        ModelRig r;
        r.session.Poll();
        r.AnswerAll();
        r.session.Poll();
        AiChangesWindow window;
        window.SelectScope(r.model, AiChangesWindow::Scope::Uncertain, std::string());
        r.session.Poll();
        const json uncertainAsk = Requests(*r.s, trackop::kList).back()["args"];
        check(uncertainAsk["attribution"] == json::array({ "unknown", "inferred" }) && !uncertainAsk.contains("sessionId"),
              "window: the 'without a session' item asks the service for unknown / inferred only");
        r.AnswerAll();
        r.session.Poll();
        window.SelectScope(r.model, AiChangesWindow::Scope::Session, "s1");
        r.session.Poll();
        const json sessionAsk = Requests(*r.s, trackop::kList).back()["args"];
        check(sessionAsk.value("sessionId", std::string()) == "s1" && !sessionAsk.contains("attribution"),
              "window: a session item asks for that session without the unknown / inferred restriction");
        r.AnswerAll();
        r.session.Poll();
        window.SelectScope(r.model, AiChangesWindow::Scope::All, std::string());
        r.session.Poll();
        check(!Requests(*r.s, trackop::kList).back()["args"].contains("attribution") && !window.ShowEditorChanges(),
              "window: 'all changes' has no attribution restriction, and the editor section is off by default");
    }
    {
        // (g8c) Editor の保存・復元で確定した変更は、既定では一覧に出ず、切り替えると専用の節に出る
        ModelRig r;
        r.listItems = json::array({ { { "revisionId", 12 }, { "path", "a.txt" }, { "changeType", "modified" }, { "actor", "ai" }, { "attribution", "claimed" } },
                                    { { "revisionId", 11 }, { "path", "scene.json" }, { "changeType", "modified" }, { "actor", "editor_user" }, { "attribution", "verified" } },
                                    { { "revisionId", 10 }, { "path", "b.txt" }, { "changeType", "modified" }, { "actor", "unknown" }, { "attribution", "unknown" } },
                                    { { "revisionId", 9 }, { "path", "c.txt" }, { "changeType", "modified" }, { "actor", "restore" }, { "attribution", "verified" } } });
        for (int i = 0; i < 4; ++i) {
            r.session.Poll();
            r.AnswerAll();
        }
        r.session.Poll();
        AiChangesWindow window;
        window.open = true;
        AiChangesHost host;
        host.allowTracking = true;
        host.enabled = true;
        host.projectRoot = kRoot;
        auto draw = [&] { WithHeadlessImGuiFrame([&] { window.OnImGui(r.session, host); }); };
        draw();
        // AI の節 (見出し + 1 件) + 不明の節 (見出し + 1 件) = 4 行 (Editor の 2 件は出ない)
        check(r.model.Changes().size() == 4 && window.DrawnItemCount() == 4, "window: the editor's changes are not in the list by default");
        window.SetShowEditorChanges(true);
        draw();
        // 4 + Editor の見出し + 2 件
        check(window.DrawnItemCount() == 7, "window: switching the editor section on adds a header and the editor's two changes");
    }
    {
        // 窓を実際に描く (ヘッドレスの 1 フレーム): データのある状態で落ちず、Hunk が DiffViewer へ渡る。使えない状態の帯も描く
        ModelRig r;
        r.describeMode = "lines";
        r.describeHunks = 1;
        r.hunkPages = { json::array({ { { "hunkId", 1 }, { "ordinal", 0 }, { "oldStart", 2 }, { "oldCount", 1 }, { "newStart", 2 }, { "newCount", 1 },
                                        { "beforeText", "a\n" }, { "afterText", "b\n" }, { "contextPre", "x\n" }, { "contextPost", "y\n" }, { "eolOnly", false } } }) };
        for (int i = 0; i < 4; ++i) {
            r.session.Poll();
            r.AnswerAll();
        }
        r.session.Poll();
        AiChangesWindow window;
        window.open = true;
        AiChangesHost host;
        host.allowTracking = true;
        host.enabled = true;
        host.projectRoot = L"C:\\mye_track_selftest\\proj";
        int draws = 0;
        for (int frame = 0; frame < 3; ++frame) {
            WithHeadlessImGuiFrame([&] {
                window.OnImGui(r.session, host);
                ++draws;
            });
        }
        check(draws == 3 && r.model.HunksComplete() && window.GetDiffViewer().Rows().HunkCount() == 1 && window.GetDiffViewer().Rows().InlineRows().size() == 5,
              "window: a headless frame draws it with data, and the diff viewer received the model's hunk (header, context, -, +, context)");
        for (const TrackState state : { TrackState::Disabled, TrackState::NoProject, TrackState::Connecting, TrackState::NoService, TrackState::ProtoMismatch,
                                        TrackState::WrongProject, TrackState::DbDegraded, TrackState::Disconnected }) {
            AiChangesWindow other;
            other.open = true;
            ChangeTrackingSession idle;
            idle.Client().SetIdle(state);
            host.allowTracking = state != TrackState::Disabled;
            WithHeadlessImGuiFrame([&] { other.OnImGui(idle, host); });
        }
        check(true, "window: every connection state (off / no project / connecting / no service / mismatch / wrong project / db / lost) draws without a model");
    }
    {
        // 無効の間は、窓を開いても I/O の口を 1 回も呼ばない (時計も)
        auto s = std::make_shared<FakeState>();
        ChangeTrackingSession disabled;
        disabled.StartWithIo(std::make_unique<FakeIo>(s), kRoot, false);
        AiChangesWindow window;
        window.open = true;
        for (int i = 0; i < 10; ++i) {
            s->nowMs += 10000;
            window.SyncInterest(disabled.Model(), disabled.State());
            disabled.Poll();
        }
        check(disabled.State() == TrackState::Disabled && s->c.Total() == 0 && s->c.now == 0,
              "window: with tracking off, an open window never touches the I/O (pipe / service.json / launch / clock)");
    }

    // ---- (g9) 設定の切り替え: 有効にすると Start し直されて接続へ進み、無効にすると接続が閉じる (shutdown は送らない) ----
    {
        auto s = std::make_shared<FakeState>();
        ChangeTrackingSession session;
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, false);
        check(session.State() == TrackState::Disabled && s->c.Total() == 0, "toggle: starts disabled without I/O");
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, true); // 有効にする = Start をやり直す
        session.Poll();
        ReplyHelloOk(*s);
        session.Poll();
        check(session.State() == TrackState::Ready && s->c.openPipe == 1, "toggle: turning it on starts again and reaches Ready");
        const int closesBefore = s->c.closePipe;
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, false); // 無効にする
        check(session.State() == TrackState::Disabled && s->c.closePipe > closesBefore && !s->pipeOpen,
              "toggle: turning it off closes the connection");
        check(Requests(*s, trackop::kShutdown).empty(), "toggle: turning it off does not send shutdown (the service keeps running)");
        const int calls = s->c.Total();
        s->nowMs += 3600 * 1000;
        session.Poll();
        check(s->c.Total() == calls, "toggle: after turning it off no I/O happens");
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, true);
        session.Poll();
        ReplyHelloOk(*s);
        session.Poll();
        check(session.State() == TrackState::Ready && s->c.openPipe == 2 && Requests(*s, trackop::kShutdown).empty(),
              "toggle: turning it on again reconnects, still without shutdown");
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
        session.Poll();
        check(session.State() == TrackState::NoService, "launch: no service and no exe -> NoService (Poll returned, so it did not block)");
        if (had > 0 && had < std::size(previous)) {
            SetEnvironmentVariableW(kTrackExeEnvVar, previous);
        } else {
            SetEnvironmentVariableW(kTrackExeEnvVar, nullptr);
        }
    }

    // ---- (k) Editor の保存の知らせ (editor_saved): 保存直後の通知の口から、接続中だけ、書いた内容のハッシュと一緒に送る ----
    RunSavedNoticeTests(check);

    // ---- (j) AI Changes 窓の文面・節・DiffView (ImGui を回さない部分と、ヘッドレスの 1 フレーム) ----
    RunChangeViewSelfTests(check);

    // ---- (m) シーンの変更の強調・一覧・選択の同期 (M104m) ----
    RunSceneChangeOverlaySelfTests(check);

    // ---- (n) レビュー (M104o) ----
    RunReviewSelfTests(check);

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
