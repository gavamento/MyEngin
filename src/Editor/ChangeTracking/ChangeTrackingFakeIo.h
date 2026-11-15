//====================================================================================
//                          ChangeTrackingFakeIo.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          追跡クライアントの試験用の偽の I/O (複数のスイートで共有)
//====================================================================================
#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/TrackIo.h"
#include "Editor/ChangeTracking/TrackProjectId.h"
#include "Editor/ChangeTracking/TrackProtocol.h"

namespace mye {
namespace tracktest {

using nlohmann::json;

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

inline const wchar_t* const kRoot = L"C:\\mye_track_selftest\\proj"; // 実在しなくてよい (偽の I/O はディスクに触れない)

inline void FeedJson(FakeState& s, const json& j)
{
    s.chunks.push_back(j.dump() + "\n");
}

// 書き込まれた要求のうち op が合うもの (古い順)
inline std::vector<json> Requests(const FakeState& s, const char* op)
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

inline json OkReply(uint64_t id, const json& result)
{
    return json{ { "id", id }, { "ok", true }, { "result", result } };
}

inline json ErrReply(uint64_t id, const char* code)
{
    return json{ { "id", id }, { "ok", false }, { "error", { { "code", code }, { "detail", "t" } } } };
}

inline uint64_t LastHelloId(const FakeState& s)
{
    const std::vector<json> hellos = Requests(s, trackop::kHello);
    return hellos.empty() ? 0 : hellos.back()["id"].get<uint64_t>();
}

inline void ReplyHelloOk(FakeState& s)
{
    FeedJson(s, OkReply(LastHelloId(s), { { "proto", 1 }, { "projectId", ProjectId(kRoot) }, { "role", "editor" } }));
}

} // namespace tracktest
} // namespace mye
