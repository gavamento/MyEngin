//====================================================================================
//                          SessionTypes.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          システム入力の適用とレーン割り当て (純関数)
//====================================================================================
#include "Engine/Engine/Session/SessionTypes.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {
namespace {

// playerId を持つレーンを探す。state が一致するものだけ (kNoLane = 無し)
int FindLaneOfPlayer(const SessionLanes& lanes, uint64_t playerId, LaneState state)
{
    for (uint32_t i = 0; i < kMaxPlayers; ++i) {
        if (lanes.lanes[i].state == static_cast<uint32_t>(state) && lanes.lanes[i].playerId == playerId) {
            return static_cast<int>(i);
        }
    }
    return kNoLane;
}

// playerId がどれかのレーンに居る (Connected / Reserved)
bool IsPlayerPresent(const SessionLanes& lanes, uint64_t playerId)
{
    return FindLaneOfPlayer(lanes, playerId, LaneState::Connected) != kNoLane
        || FindLaneOfPlayer(lanes, playerId, LaneState::Reserved) != kNoLane;
}

// 適用したイベントの写しを積む (pad は 0、lane は割り当て結果)
void RecordApplied(SessionLanes& lanes, const SystemEvent& ev, int lane)
{
    SystemEvent& dst = lanes.applied[lanes.appliedCount++];
    dst = {};
    dst.eventSeq = ev.eventSeq;
    dst.playerId = ev.playerId;
    dst.kind = ev.kind;
    dst.lane = static_cast<uint8_t>(lane);
}

} // namespace

int AllocateLane(const SessionLanes& lanes)
{
    for (uint32_t i = 0; i < kMaxPlayers; ++i) {
        if (lanes.lanes[i].state == static_cast<uint32_t>(LaneState::Empty)) {
            return static_cast<int>(i);
        }
    }
    return kNoLane;
}

void ApplySystemInput(SessionLanes& lanes, const SystemInputTick& input)
{
    lanes.appliedCount = 0;
    std::memset(lanes.applied, 0, sizeof(lanes.applied));

    uint32_t count = input.eventCount;
    if (count > kMaxSystemEventsPerTick) {
        MYE_LOG_ERROR("[session] system input has %u events (max %u) - the excess is ignored", count,
                      kMaxSystemEventsPerTick);
        count = kMaxSystemEventsPerTick;
    }

    // 同一 tick 内は eventSeq 昇順。入力列の並びに依らず同じ結果にするため整列した写しで処理する
    // (同じ eventSeq は入力順を保つ = stable)
    SystemEvent sorted[kMaxSystemEventsPerTick];
    std::copy(input.events, input.events + count, sorted);
    std::stable_sort(sorted, sorted + count,
                     [](const SystemEvent& a, const SystemEvent& b) { return a.eventSeq < b.eventSeq; });

    for (uint32_t i = 0; i < count; ++i) {
        const SystemEvent& ev = sorted[i];
        // 適用済みの eventSeq 以下 = 重複 / 順序逆転。状態を動かさず無視する
        if (ev.eventSeq == 0 || ev.eventSeq <= lanes.lastEventSeq) {
            MYE_LOG_ERROR("[session] system event seq %llu is not newer than %llu - ignored",
                          static_cast<unsigned long long>(ev.eventSeq),
                          static_cast<unsigned long long>(lanes.lastEventSeq));
            continue;
        }
        // 無効なイベントでも seq は進める (サーバが発行した番号が適用側の欠番検出と食い違わないように)
        lanes.lastEventSeq = ev.eventSeq;

        switch (static_cast<SystemEventKind>(ev.kind)) {
        case SystemEventKind::Join: {
            if (ev.playerId == 0 || IsPlayerPresent(lanes, ev.playerId)) {
                MYE_LOG_ERROR("[session] Join seq %llu: playerId %llu is invalid or already present - ignored",
                              static_cast<unsigned long long>(ev.eventSeq),
                              static_cast<unsigned long long>(ev.playerId));
                break;
            }
            const int lane = AllocateLane(lanes);
            if (lane == kNoLane) {
                MYE_LOG_ERROR("[session] Join seq %llu: no free lane - ignored",
                              static_cast<unsigned long long>(ev.eventSeq));
                break;
            }
            lanes.lanes[lane].state = static_cast<uint32_t>(LaneState::Connected);
            lanes.lanes[lane].playerId = ev.playerId;
            RecordApplied(lanes, ev, lane);
            break;
        }
        case SystemEventKind::Leave: {
            const bool valid = ev.lane < kMaxPlayers
                && lanes.lanes[ev.lane].state == static_cast<uint32_t>(LaneState::Connected)
                && lanes.lanes[ev.lane].playerId == ev.playerId;
            if (!valid) {
                MYE_LOG_ERROR("[session] Leave seq %llu: lane %u is not connected for playerId %llu - ignored",
                              static_cast<unsigned long long>(ev.eventSeq), static_cast<unsigned>(ev.lane),
                              static_cast<unsigned long long>(ev.playerId));
                break;
            }
            lanes.lanes[ev.lane].state = static_cast<uint32_t>(LaneState::Reserved);
            RecordApplied(lanes, ev, ev.lane);
            break;
        }
        case SystemEventKind::Rejoin: {
            const int lane = FindLaneOfPlayer(lanes, ev.playerId, LaneState::Reserved);
            if (ev.playerId == 0 || lane == kNoLane) {
                MYE_LOG_ERROR("[session] Rejoin seq %llu: no reserved lane for playerId %llu - ignored",
                              static_cast<unsigned long long>(ev.eventSeq),
                              static_cast<unsigned long long>(ev.playerId));
                break;
            }
            lanes.lanes[lane].state = static_cast<uint32_t>(LaneState::Connected);
            RecordApplied(lanes, ev, lane);
            break;
        }
        case SystemEventKind::Release: {
            const bool valid = ev.lane < kMaxPlayers
                && lanes.lanes[ev.lane].state == static_cast<uint32_t>(LaneState::Reserved)
                && lanes.lanes[ev.lane].playerId == ev.playerId;
            if (!valid) {
                MYE_LOG_ERROR("[session] Release seq %llu: lane %u is not reserved for playerId %llu - ignored",
                              static_cast<unsigned long long>(ev.eventSeq), static_cast<unsigned>(ev.lane),
                              static_cast<unsigned long long>(ev.playerId));
                break;
            }
            lanes.lanes[ev.lane].state = static_cast<uint32_t>(LaneState::Empty);
            lanes.lanes[ev.lane].playerId = 0;
            RecordApplied(lanes, ev, ev.lane);
            break;
        }
        default:
            MYE_LOG_ERROR("[session] system event seq %llu has unknown kind %u - ignored",
                          static_cast<unsigned long long>(ev.eventSeq), static_cast<unsigned>(ev.kind));
            break;
        }
    }
}

InputSnapshot SubstituteLateInput(const InputSnapshot& prev)
{
    InputSnapshot out = prev;
    std::memset(out.chars, 0, sizeof(out.chars));
    out.charCount = 0;
    out.mouseDeltaX = 0;
    out.mouseDeltaY = 0;
    out.wheelDelta = 0;
    return out;
}

SessionLanes DefaultLanesFor(uint32_t playerCount)
{
    SessionLanes lanes = {};
    const uint32_t n = std::min(playerCount, kMaxPlayers);
    for (uint32_t i = 0; i < n; ++i) {
        lanes.lanes[i].state = static_cast<uint32_t>(LaneState::Connected);
    }
    return lanes;
}

SystemInputTick NormalizeSystemInput(const SystemInputTick& in)
{
    SystemInputTick out = {};
    out.eventCount = std::min(in.eventCount, kMaxSystemEventsPerTick);
    for (uint32_t i = 0; i < out.eventCount; ++i) {
        out.events[i] = in.events[i];
        std::memset(out.events[i].pad, 0, sizeof(out.events[i].pad));
    }
    return out;
}

std::string FirstDifferentSystemInputField(const SystemInputTick& a, const SystemInputTick& b)
{
    if (a.eventCount != b.eventCount) {
        return "systemInput.eventCount";
    }
    const uint32_t n = std::min(a.eventCount, kMaxSystemEventsPerTick);
    char buf[48];
    for (uint32_t i = 0; i < n; ++i) {
        const SystemEvent& x = a.events[i];
        const SystemEvent& y = b.events[i];
        const char* field = nullptr;
        if (x.eventSeq != y.eventSeq) {
            field = "eventSeq";
        } else if (x.playerId != y.playerId) {
            field = "playerId";
        } else if (x.kind != y.kind) {
            field = "kind";
        } else if (x.lane != y.lane) {
            field = "lane";
        } else if (std::memcmp(x.pad, y.pad, sizeof(x.pad)) != 0) {
            field = "pad";
        }
        if (field != nullptr) {
            std::snprintf(buf, sizeof(buf), "systemInput.events[%u].%s", i, field);
            return std::string(buf);
        }
    }
    return std::string();
}

} // namespace mye
