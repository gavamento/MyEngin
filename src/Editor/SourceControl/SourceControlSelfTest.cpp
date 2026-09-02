#include "Editor/SourceControl/SourceControlSelfTest.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "Editor/SourceControl/CollabClient.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

// hello の往復に許す時間。git.exe の起動 1 回分 + α (spec §4.4 のタイムアウト表と同値)
constexpr int kHelloTimeoutMs = 5000;

bool CollabRequired()
{
    const char* v = std::getenv("MYE_COLLAB_REQUIRED");
    return v && v[0] == '1';
}

} // namespace

bool RunSourceControlSelfTest()
{
    MYE_LOG_INFO("==== Source control self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (a) 応答/通知の配線 (DLL 不要) ----
    {
        CollabClient client;
        int called = 0;
        std::string gotVersion;
        const uint64_t id = client.AddPendingForTest([&](const nlohmann::json& msg) {
            ++called;
            gotVersion = msg["result"].value("gitVersion", std::string());
        });
        client.DispatchLine("{\"id\":" + std::to_string(id)
                            + ",\"ok\":true,\"result\":{\"gitVersion\":\"2.48.1\"}}");
        check(called == 1 && gotVersion == "2.48.1", "response is routed to the matching id");
        check(client.PendingCount() == 0, "the callback is removed once fired");

        // 同じ id が二度来ても二度は呼ばれない (worker の再送や CLI の連結で起きうる)
        client.DispatchLine("{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":{}}");
        check(called == 1, "a duplicate response does not fire the callback twice");

        int events = 0;
        client.SetEventHandler([&](const nlohmann::json&) { ++events; });
        client.DispatchLine("{\"event\":\"status_changed\"}");
        check(events == 1, "event line reaches the subscriber");

        // service_error は購読者に配るだけでなく、状態を ServiceDied に落とす
        client.DispatchLine("{\"event\":\"service_error\",\"code\":\"internal_panic\",\"detail\":\"x\"}");
        check(events == 2 && client.State() == Unavailable::ServiceDied,
              "service_error switches the client to ServiceDied");

        // 壊れた行で落ちないこと (サービスが暴走しても**エディタは生き残る**)
        client.DispatchLine("{\"id\":");
        client.DispatchLine("not json at all");
        check(true, "malformed lines are survivable");
    }

    // ---- (b) MyeCollab.dll との実往復 ----
    {
        const std::wstring exeDir = GetExecutableDir();
        const fs::path dllPath = fs::path(exeDir) / L"MyeCollab.dll";
        std::error_code ec;
        if (!fs::exists(dllPath, ec)) {
            if (CollabRequired()) {
                MYE_LOG_ERROR("  FAIL: MyeCollab.dll not found but MYE_COLLAB_REQUIRED=1 "
                              "(run tools\\build_collab.bat)");
                ++failCount;
            } else {
                MYE_LOG_INFO("[selftest] SourceControl: MyeCollab.dll not found - SKIP");
            }
        } else {
            CollabClient client;
            const bool loaded = client.Load(exeDir);
            check(loaded, "MyeCollab.dll loads and the proto version matches");
            if (loaded) {
                // リポジトリでなくてよい (hello は git の所在と版しか見ない)
                const fs::path tmp = fs::temp_directory_path() / L"mye_collab_selftest";
                fs::create_directories(tmp, ec);
                check(client.Create(WideToUtf8(tmp.wstring())), "mye_collab_create returns a handle");

                bool done = false;
                bool ok = false;
                std::string gitVersion;
                std::string errCode;
                client.Request(collabop::kHello, { { "fetchIntervalMin", 5 }, { "autoFetch", false } },
                               [&](const nlohmann::json& msg) {
                                   done = true;
                                   ok = msg.value("ok", false);
                                   if (ok) {
                                       gitVersion = msg["result"].value("gitVersion", std::string());
                                   } else if (msg.contains("error")) {
                                       errCode = msg["error"].value("code", std::string());
                                   }
                               });

                const auto start = std::chrono::steady_clock::now();
                while (!done) {
                    client.Poll();
                    if (done) {
                        break;
                    }
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count();
                    if (elapsed > kHelloTimeoutMs) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                check(done, "hello gets a response within 5s");
                if (done && !ok) {
                    MYE_LOG_ERROR("  hello failed: error.code=%s", errCode.c_str());
                }
                check(ok && !gitVersion.empty(), "hello reports a non-empty gitVersion");
                MYE_LOG_INFO("  [collab] git version: %s",
                             gitVersion.empty() ? "(none)" : gitVersion.c_str());
                client.Shutdown();
                check(client.State() == Unavailable::NoService, "Shutdown unloads the dll");
            }
        }
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Source control self test PASSED ====");
        return true;
    }
    MYE_LOG_ERROR("==== Source control self test FAILED (%d) ====", failCount);
    return false;
}

} // namespace mye
