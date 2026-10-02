//====================================================================================
//                          SessionSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          セッション型 / .rep v9 / SessionLanes の自己テスト
//====================================================================================
#include "Engine/Engine/Session/SessionSelfTest.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Replay/CrashRing.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Session/Provenance.h"
#include "Engine/Engine/Session/SessionTypes.h"

namespace mye {
namespace {

SystemEvent MakeEvent(uint64_t seq, uint64_t playerId, SystemEventKind kind, uint32_t lane = 0)
{
    SystemEvent e = {};
    e.eventSeq = seq;
    e.playerId = playerId;
    e.kind = static_cast<uint8_t>(kind);
    e.lane = static_cast<uint8_t>(lane);
    return e;
}

SystemInputTick MakeTick(std::initializer_list<SystemEvent> events)
{
    SystemInputTick t = {};
    for (const SystemEvent& e : events) {
        t.events[t.eventCount++] = e;
    }
    return t;
}

bool LaneIs(const SessionLanes& s, uint32_t lane, LaneState state, uint64_t playerId)
{
    return s.lanes[lane].state == static_cast<uint32_t>(state) && s.lanes[lane].playerId == playerId;
}

InputSnapshot MakeInput(uint32_t seed)
{
    InputSnapshot in = {};
    in.mouseX = static_cast<int32_t>(seed * 3 + 1);
    in.mouseY = static_cast<int32_t>(seed * 7 + 2);
    in.keys[seed % 32] = static_cast<uint8_t>(seed & 0xFF);
    in.padConnected = 1;
    return in;
}

std::vector<char> ReadAll(const std::wstring& path)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void WriteAll(const std::wstring& path, const std::vector<char>& bytes)
{
    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// 3 tick の .rep を録る。events が非 null なら tick 毎のシステム入力として使う
bool RecordSample(const std::wstring& path, uint32_t role, uint32_t lanes,
                  const SystemInputTick* events, uint64_t seed)
{
    ReplayRecorder rec;
    SessionConfig cfg = {};
    cfg.role = role;
    cfg.playerCount = lanes;
    cfg.tickRate = 60;
    cfg.seed = seed;
    cfg.configBits = 0x5;
    cfg.referenceW = 1920;
    cfg.referenceH = 1080;
    SimProvenance prov = {};
    prov.contentHash = 0xC0FFEE;
    SnapshotMeta meta = {};
    meta.tick = 0;
    rec.Start(path, 11, 22, 3, lanes, nullptr, 0, cfg, prov, meta);
    InputSnapshot in[kMaxPlayers] = {};
    for (uint64_t t = 0; t < 3; ++t) {
        for (uint32_t p = 0; p < lanes; ++p) {
            in[p] = MakeInput(static_cast<uint32_t>(t * 10 + p));
        }
        rec.RecordTick(in, lanes, 0xA000 + t, events != nullptr ? &events[t] : nullptr);
    }
    return rec.Finish();
}

// startTick から count tick の .rep を録る (開始スナップショット無し。startMeta.tick に開始 tick を入れる)。
// tick t の入力は MakeInput(t*10+p)、ハッシュは 0xA000+t。badTick の tick だけハッシュを変える (~0 = 変えない)
bool RecordFrom(const std::wstring& path, uint64_t startTick, uint32_t count, uint64_t badTick)
{
    ReplayRecorder rec;
    SessionConfig cfg = {};
    cfg.role = static_cast<uint32_t>(SessionRole::Client);
    cfg.playerCount = 2;
    cfg.tickRate = 60;
    cfg.referenceW = 1920;
    cfg.referenceH = 1080;
    SimProvenance prov = {};
    prov.contentHash = 0xC0FFEE;
    SnapshotMeta meta = {};
    meta.tick = startTick;
    rec.Start(path, 11 + startTick, 22, 3 + static_cast<uint32_t>(startTick), 2, nullptr, 0, cfg, prov, meta);
    InputSnapshot in[kMaxPlayers] = {};
    for (uint64_t t = startTick; t < startTick + count; ++t) {
        for (uint32_t p = 0; p < 2; ++p) {
            in[p] = MakeInput(static_cast<uint32_t>(t * 10 + p));
        }
        rec.RecordTick(in, 2, t == badTick ? 0xBAD : 0xA000 + t, nullptr);
    }
    return rec.Finish();
}

// ---- R4: .rep の逐次書き出し ----

// Server 相当 (4 レーン + システム入力) の .rep を録る。streamFlushTicks = 0 は一括モード。
// tick t の入力は MakeInput(t*10+p)、5 tick ごとにイベント 1 件
bool RecordLongSample(const std::wstring& path, uint32_t streamFlushTicks, uint32_t ticks,
                      const std::vector<std::byte>& snapshot, bool finish = true)
{
    ReplayRecorder rec;
    SessionConfig cfg = {};
    cfg.role = static_cast<uint32_t>(SessionRole::Server);
    cfg.playerCount = 4;
    cfg.tickRate = 60;
    cfg.referenceW = 1920;
    cfg.referenceH = 1080;
    SimProvenance prov = {};
    prov.contentHash = 0xC0FFEE;
    SnapshotMeta meta = {};
    rec.Start(path, 11, 22, 3, 4, snapshot.empty() ? nullptr : snapshot.data(), snapshot.size(), cfg, prov,
              meta, streamFlushTicks);
    if (!rec.IsActive()) {
        return false;
    }
    InputSnapshot in[kMaxPlayers] = {};
    for (uint32_t t = 0; t < ticks; ++t) {
        for (uint32_t p = 0; p < 4; ++p) {
            in[p] = MakeInput(t * 10 + p);
        }
        SystemInputTick sys = {};
        if (t % 5 == 0) {
            sys = MakeTick({ MakeEvent(t / 5 + 1, 1, SystemEventKind::Join, t % 4) });
        }
        rec.RecordTick(in, 4, 0xA000 + t, &sys);
    }
    return finish ? rec.Finish() : true;
}

void CheckReplayStreaming(const std::filesystem::path& tempDir, const std::function<bool(bool, const char*)>& check)
{
    std::error_code ec;
    const std::wstring batchPath = (tempDir / L"mye_rs_batch.rep").wstring();
    const std::wstring streamPath = (tempDir / L"mye_rs_stream.rep").wstring();
    const std::wstring cutPath = (tempDir / L"mye_rs_cut.rep").wstring();
    std::vector<std::byte> blob(96);
    for (size_t i = 0; i < blob.size(); ++i) {
        blob[i] = static_cast<std::byte>(i * 7 + 1);
    }
    constexpr uint64_t kRec = 4 * sizeof(InputSnapshot) + sizeof(SystemInputTick) + sizeof(uint64_t);
    const size_t base = sizeof(MyeReplayHeader) + blob.size();

    // 逐次と一括でバイト一致 (flush 間隔・tick 数・スナップショットの有無を変えて)
    struct Case {
        uint32_t flush;
        uint32_t ticks;
        bool snapshot;
    };
    bool allSame = true;
    for (const Case& c : { Case{ 60, 200, true }, Case{ 1, 7, true }, Case{ 60, 5, false }, Case{ 60, 0, true },
                           Case{ 7, 120, false } }) {
        const std::vector<std::byte> snap = c.snapshot ? blob : std::vector<std::byte>{};
        const bool a = RecordLongSample(batchPath, 0, c.ticks, snap);
        const bool b = RecordLongSample(streamPath, c.flush, c.ticks, snap);
        if (!(a && b && ReadAll(batchPath) == ReadAll(streamPath))) {
            MYE_LOG_ERROR("  R4 case flush=%u ticks=%u snapshot=%d differs", c.flush, c.ticks, c.snapshot ? 1 : 0);
            allSame = false;
        }
    }
    check(allSame, "R4: streaming and batch recorders write byte-identical .rep files");

    // 逐次モードのメモリ: tick を溜め込まない (ファイルにだけある)
    RecordLongSample(streamPath, 60, 200, blob);
    const std::vector<char> full = ReadAll(streamPath);
    check(full.size() == base + 200 * kRec, "R4: a finished streaming .rep has header + snapshot + 200 records");
    {
        ReplayPlayer p;
        check(p.Load(streamPath) && p.TickCount() == 200 && !p.RecoveredUnfinished(),
              "R4: a finished streaming .rep loads normally (not flagged as recovered)");
    }

    // 異常終了を模す: tickCount を 0 に戻し、57 tick + 100 バイトで切る
    {
        std::vector<char> cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(base + 57 * kRec + 100));
        const uint64_t zero = 0;
        std::memcpy(cut.data() + offsetof(MyeReplayHeader, tickCount), &zero, sizeof(zero));
        WriteAll(cutPath, cut);
        ReplayPlayer p;
        // 埋め込み blob は偽物なので RNG の突き合わせは行われない (Peek できない)。tick 列だけを検査する
        if (check(p.Load(cutPath), "R4: a .rep left with tickCount 0 loads")) {
            check(p.TickCount() == 57 && p.RecoveredUnfinished(),
                  "R4: the tick count comes from the file length (57), the cut record is dropped");
            const InputSnapshot want = MakeInput(56 * 10 + 3);
            check(std::memcmp(&p.InputForTick(56, 3), &want, sizeof(InputSnapshot)) == 0
                      && p.ExpectedHash(56) == 0xA000 + 56 && p.SystemInputForTick(55).eventCount == 1
                      && p.SystemInputForTick(56).eventCount == 0,
                  "R4: recovered ticks carry the same inputs, system input and hashes");
        }
        // 切れ目がちょうどレコード境界でも同じ
        cut.resize(base + 3 * kRec);
        WriteAll(cutPath, cut);
        ReplayPlayer p2;
        check(p2.Load(cutPath) && p2.TickCount() == 3, "R4: a cut on a record boundary keeps every whole record");
        // スナップショットの途中で切れたものは読めない (開始点が無い)
        cut.resize(sizeof(MyeReplayHeader) + 10);
        WriteAll(cutPath, cut);
        ReplayPlayer p3;
        check(!p3.Load(cutPath), "R4: a file cut inside the embedded snapshot is rejected");
        // ヘッダ + スナップショットだけ (tick 0 本) は 0 tick の記録として読める
        cut.assign(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(base));
        std::memcpy(cut.data() + offsetof(MyeReplayHeader, tickCount), &zero, sizeof(zero));
        WriteAll(cutPath, cut);
        ReplayPlayer p4;
        check(p4.Load(cutPath) && p4.TickCount() == 0 && !p4.RecoveredUnfinished(),
              "R4: header + snapshot only loads as a 0-tick recording");
    }

    // 実際に閉じないまま途中を覗く: flush 済みの 120 tick は必ず読める
    {
        ReplayRecorder rec;
        SessionConfig cfg = {};
        cfg.role = static_cast<uint32_t>(SessionRole::Server);
        cfg.playerCount = 4;
        rec.Start(streamPath, 11, 22, 3, 4, blob.data(), blob.size(), cfg, SimProvenance{}, SnapshotMeta{}, 60);
        InputSnapshot in[kMaxPlayers] = {};
        for (uint32_t t = 0; t < 130; ++t) {
            in[0] = MakeInput(t);
            rec.RecordTick(in, 4, 0xB000 + t, nullptr);
        }
        const std::vector<char> live = ReadAll(streamPath); // 閉じる前 = 落ちた直後と同じ状態
        WriteAll(cutPath, live);
        ReplayPlayer p;
        const uint64_t onDisk = (live.size() - base) / kRec;
        check(p.Load(cutPath) && p.RecoveredUnfinished() && p.TickCount() == onDisk && onDisk >= 120 && onDisk <= 130
                  && p.ExpectedHash(119) == 0xB000 + 119,
              "R4: a recording that was never closed still yields every flushed tick (>= 120 of 130)");
        check(rec.Finish(), "R4: the same recorder can still be finished afterwards");
        ReplayPlayer q;
        check(q.Load(streamPath) && q.TickCount() == 130 && !q.RecoveredUnfinished(),
              "R4: after Finish all 130 ticks are present");
    }

    // 開けないパスでは Start が非アクティブのまま (黙って記録しない状態を作らない)
    {
        ReplayRecorder rec;
        rec.Start((tempDir / L"mye_rs_no_such_dir_\x1" / L"?.rep").wstring(), 1, 2, 3, 4, nullptr, 0,
                  SessionConfig{}, SimProvenance{}, SnapshotMeta{}, 60);
        check(!rec.IsActive() && !rec.Finish(), "R4: an unwritable path leaves the streaming recorder inactive");
    }
    std::filesystem::remove(batchPath, ec);
    std::filesystem::remove(streamPath, ec);
    std::filesystem::remove(cutPath, ec);
}

} // namespace

bool RunSessionSelfTest()
{
    MYE_LOG_INFO("==== Session (system input / SessionLanes / .rep v9) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
        return cond;
    };

    // ---- 型のサイズ (レイアウトは .rep の一部) ----
    check(sizeof(SystemEvent) == 24 && sizeof(SystemInputTick) == 200 && kMaxSystemEventsPerTick == 8,
          "SystemEvent 24 bytes / SystemInputTick 200 bytes / 8 events per tick");

    // ---- S1: レーン状態の純関数 ----
    {
        SessionLanes s = {};
        ApplySystemInput(s, MakeTick({ MakeEvent(1, 1, SystemEventKind::Join), MakeEvent(2, 2, SystemEventKind::Join),
                                       MakeEvent(3, 3, SystemEventKind::Join), MakeEvent(4, 4, SystemEventKind::Join) }));
        check(LaneIs(s, 0, LaneState::Connected, 1) && LaneIs(s, 1, LaneState::Connected, 2)
                  && LaneIs(s, 2, LaneState::Connected, 3) && LaneIs(s, 3, LaneState::Connected, 4),
              "S1: Join x4 fills lanes 0..3 in eventSeq order");
        check(s.appliedCount == 4 && s.applied[0].lane == 0 && s.applied[3].lane == 3
                  && s.lastEventSeq == 4,
              "S1: applied copy carries the assigned lanes, lastEventSeq advances");
        check(AllocateLane(s) == kNoLane, "S1: AllocateLane reports a full session");

        ApplySystemInput(s, MakeTick({ MakeEvent(5, 5, SystemEventKind::Join) }));
        check(s.appliedCount == 0 && LaneIs(s, 3, LaneState::Connected, 4),
              "S1: the 5th Join gets no lane and is not applied");

        ApplySystemInput(s, MakeTick({ MakeEvent(6, 2, SystemEventKind::Leave, 1) }));
        check(LaneIs(s, 1, LaneState::Reserved, 2) && s.appliedCount == 1,
              "S1: Leave reserves the lane and keeps the playerId");
        check(AllocateLane(s) == kNoLane, "S1: a reserved lane is not allocatable");

        ApplySystemInput(s, MakeTick({ MakeEvent(7, 2, SystemEventKind::Rejoin) }));
        check(LaneIs(s, 1, LaneState::Connected, 2) && s.applied[0].lane == 1,
              "S1: Rejoin with the same playerId returns to the same lane");

        // Release 後の Join は最小の空きレーン (lane 2 と lane 0 を空けて、0 から埋まる)
        ApplySystemInput(s, MakeTick({ MakeEvent(8, 3, SystemEventKind::Leave, 2),
                                       MakeEvent(9, 3, SystemEventKind::Release, 2),
                                       MakeEvent(10, 1, SystemEventKind::Leave, 0),
                                       MakeEvent(11, 1, SystemEventKind::Release, 0) }));
        check(LaneIs(s, 0, LaneState::Empty, 0) && LaneIs(s, 2, LaneState::Empty, 0),
              "S1: Release returns lanes to Empty and clears the playerId");
        check(AllocateLane(s) == 0, "S1: AllocateLane picks the lowest Empty lane");
        ApplySystemInput(s, MakeTick({ MakeEvent(12, 12, SystemEventKind::Join) }));
        check(LaneIs(s, 0, LaneState::Connected, 12), "S1: Join after Release takes the lowest free lane");
    }
    {
        // 同 tick の複数イベントは入力の並びでなく eventSeq 順に適用される
        SessionLanes s = {};
        ApplySystemInput(s, MakeTick({ MakeEvent(21, 21, SystemEventKind::Join),
                                       MakeEvent(20, 20, SystemEventKind::Join) }));
        check(LaneIs(s, 0, LaneState::Connected, 20) && LaneIs(s, 1, LaneState::Connected, 21)
                  && s.applied[0].eventSeq == 20 && s.applied[1].eventSeq == 21,
              "S1: events in one tick are applied in eventSeq order");
    }
    {
        // 不正イベントは無視される (状態を動かさない)
        SessionLanes s = {};
        ApplySystemInput(s, MakeTick({ MakeEvent(1, 1, SystemEventKind::Join), MakeEvent(2, 2, SystemEventKind::Join) }));
        ApplySystemInput(s, MakeTick({ MakeEvent(3, 2, SystemEventKind::Leave, 1) })); // lane 1 を Reserved に
        const SessionLanes before = s;
        const SystemEvent bad[] = {
            MakeEvent(10, 9, SystemEventKind::Leave, 7),      // 存在しないレーン
            MakeEvent(11, 9, SystemEventKind::Leave, 3),      // Empty のレーン
            MakeEvent(12, 1, SystemEventKind::Leave, 1),      // playerId が合わない (lane 1 は Reserved)
            MakeEvent(13, 77, SystemEventKind::Rejoin),       // 予約が無い playerId
            MakeEvent(14, 1, SystemEventKind::Release, 0),    // Connected は Release できない
            MakeEvent(15, 1, SystemEventKind::Join),          // 既に居る playerId
            MakeEvent(16, 0, SystemEventKind::Join),          // playerId 0
            MakeEvent(17, 5, static_cast<SystemEventKind>(99)), // 未知の kind
        };
        SystemInputTick t = {};
        for (const SystemEvent& e : bad) {
            t.events[t.eventCount++] = e;
        }
        ApplySystemInput(s, t);
        check(std::memcmp(s.lanes, before.lanes, sizeof(s.lanes)) == 0 && s.appliedCount == 0,
              "S1: invalid events are ignored and leave the lanes untouched");
        // 重複 / 逆行した eventSeq と seq 0
        const uint64_t seqBefore = s.lastEventSeq;
        ApplySystemInput(s, MakeTick({ MakeEvent(17, 5, SystemEventKind::Join), MakeEvent(0, 6, SystemEventKind::Join) }));
        check(s.appliedCount == 0 && s.lastEventSeq == seqBefore
                  && std::memcmp(s.lanes, before.lanes, sizeof(s.lanes)) == 0,
              "S1: a stale or zero eventSeq is ignored");
        // eventCount が 9 以上の壊れた入力は 8 件に丸める (範囲外を読まない)
        SystemInputTick over = {};
        over.eventCount = 12;
        for (uint32_t i = 0; i < kMaxSystemEventsPerTick; ++i) {
            over.events[i] = MakeEvent(100 + i, 100 + i, SystemEventKind::Join);
        }
        SessionLanes s2 = {};
        ApplySystemInput(s2, over);
        check(s2.appliedCount == 4 && s2.lastEventSeq == 107,
              "S1: eventCount > 8 is clamped (4 lanes filled, the rest had no lane)");
        check(NormalizeSystemInput(over).eventCount == kMaxSystemEventsPerTick,
              "S1: NormalizeSystemInput clamps eventCount");
    }
    {
        InputSnapshot prev = MakeInput(5);
        prev.mouseDeltaX = 40;
        prev.mouseDeltaY = -30;
        prev.wheelDelta = 120;
        prev.charCount = 2;
        prev.chars[0] = 'A';
        prev.chars[1] = 0x3042;
        prev.mouseSurfX = 12.5f;
        prev.surfW = 960;
        const InputSnapshot sub = SubstituteLateInput(prev);
        check(sub.mouseDeltaX == 0 && sub.mouseDeltaY == 0 && sub.wheelDelta == 0 && sub.charCount == 0
                  && sub.chars[0] == 0 && sub.chars[1] == 0,
              "S1: SubstituteLateInput zeroes the consumed fields");
        InputSnapshot expect = prev;
        expect.mouseDeltaX = expect.mouseDeltaY = expect.wheelDelta = 0;
        expect.charCount = 0;
        std::memset(expect.chars, 0, sizeof(expect.chars));
        check(std::memcmp(&sub, &expect, sizeof(InputSnapshot)) == 0,
              "S1: SubstituteLateInput keeps every other field (keys / buttons / position)");
    }
    {
        // クライアントの予測 (D12 の分割): 文字と wheelDelta は 0、mouseDelta は繰り返す。それ以外はそのまま
        InputSnapshot latest = MakeInput(7);
        latest.mouseDeltaX = 40;
        latest.mouseDeltaY = -30;
        latest.wheelDelta = 120;
        latest.charCount = 2;
        latest.chars[0] = 'A';
        const InputSnapshot pred = PredictLaneInput(latest);
        check(pred.mouseDeltaX == 40 && pred.mouseDeltaY == -30 && pred.wheelDelta == 0 && pred.charCount == 0
                  && pred.chars[0] == 0,
              "S1: PredictLaneInput repeats the mouse delta but drops chars and the wheel");
        InputSnapshot expect = latest;
        expect.wheelDelta = 0;
        expect.charCount = 0;
        std::memset(expect.chars, 0, sizeof(expect.chars));
        check(std::memcmp(&pred, &expect, sizeof(InputSnapshot)) == 0,
              "S1: PredictLaneInput keeps every other field");
        const InputSnapshot sub = SubstituteLateInput(latest);
        check(sub.mouseDeltaX == 0 && sub.mouseDeltaY == 0,
              "S1: the server's substitute (a confirmed value) still zeroes the mouse delta");
    }
    {
        const SessionLanes d = DefaultLanesFor(2);
        check(LaneIs(d, 0, LaneState::Connected, 0) && LaneIs(d, 1, LaneState::Connected, 0)
                  && LaneIs(d, 2, LaneState::Empty, 0) && d.systemInput == 0 && d.appliedCount == 0,
              "S1: DefaultLanesFor(2) = lanes 0,1 Connected with playerId 0");
        const SessionLanes big = DefaultLanesFor(99);
        check(LaneIs(big, kMaxPlayers - 1, LaneState::Connected, 0), "S1: DefaultLanesFor clamps to kMaxPlayers");
    }

    // ---- S2: .rep v9 ----
    std::error_code ec;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
    const std::wstring plainPath = (tempDir / L"mye_session_plain.rep").wstring();
    const std::wstring sysPath = (tempDir / L"mye_session_sys.rep").wstring();
    const std::wstring sys2Path = (tempDir / L"mye_session_sys2.rep").wstring();
    const std::wstring v8Path = (tempDir / L"mye_session_v8.rep").wstring();

    SystemInputTick sysTicks[3] = {};
    sysTicks[0] = MakeTick({ MakeEvent(1, 1, SystemEventKind::Join) });
    sysTicks[1] = MakeTick({});
    sysTicks[2] = MakeTick({ MakeEvent(2, 2, SystemEventKind::Join), MakeEvent(3, 1, SystemEventKind::Leave, 0) });

    if (check(RecordSample(plainPath, 0, 2, nullptr, 7), "S2: record a v9 .rep without system input")) {
        ReplayPlayer p;
        if (check(p.Load(plainPath), "S2: v9 .rep without system input loads")) {
            check(p.Header().version == 9 && p.Header().flags == 0 && !p.HasSystemInput()
                      && p.PlayerCount() == 2 && p.TickCount() == 3,
                  "S2: header version 9, no system-input flag, 2 lanes, 3 ticks");
            check(p.Header().session.seed == 7 && p.Header().session.configBits == 0x5
                      && p.Header().session.referenceW == 1920 && p.Header().provenance.contentHash == 0xC0FFEE,
                  "S2: SessionConfig / SimProvenance round-trip");
            const InputSnapshot want = MakeInput(21);
            check(std::memcmp(&p.InputForTick(2, 1), &want, sizeof(InputSnapshot)) == 0
                      && p.ExpectedHash(2) == 0xA002,
                  "S2: lane inputs and hashes round-trip");
        }
        const std::vector<char> bytes = ReadAll(plainPath);
        check(bytes.size() == sizeof(MyeReplayHeader) + 3 * (2 * sizeof(InputSnapshot) + sizeof(uint64_t)),
              "S2: no-system-input tick record = lanes + hash (no extra bytes)");
    }
    if (check(RecordSample(sysPath, static_cast<uint32_t>(SessionRole::Server), 4, sysTicks, 9),
              "S2: record a v9 .rep with system input")) {
        ReplayPlayer p;
        if (check(p.Load(sysPath), "S2: v9 .rep with system input loads")) {
            check(p.HasSystemInput() && (p.Header().flags & kReplayFlagSystemInput) != 0
                      && p.PlayerCount() == 4,
                  "S2: flags.bit0 is set for a Server recording");
            check(p.SystemInputForTick(0).eventCount == 1 && p.SystemInputForTick(1).eventCount == 0
                      && p.SystemInputForTick(2).eventCount == 2
                      && p.SystemInputForTick(2).events[1].kind == static_cast<uint8_t>(SystemEventKind::Leave)
                      && p.SystemInputForTick(2).events[0].eventSeq == 2,
                  "S2: SystemInputTick records round-trip");
            check(p.ExpectedHash(1) == 0xA001 && p.ExpectedHash(2) == 0xA002,
                  "S2: hashes round-trip next to the system input");
        }
        const std::vector<char> bytes = ReadAll(sysPath);
        check(bytes.size() == sizeof(MyeReplayHeader) + 3 * (4 * sizeof(InputSnapshot) + sizeof(SystemInputTick) + sizeof(uint64_t)),
              "S2: system-input tick record = lanes + 200-byte SystemInputTick + hash");
        // 未知の flags ビットは拒否する
        std::vector<char> patched = bytes;
        const uint32_t badFlags = 0x2;
        std::memcpy(patched.data() + offsetof(MyeReplayHeader, flags), &badFlags, sizeof(badFlags));
        WriteAll(v8Path, patched);
        ReplayPlayer q;
        check(!q.Load(v8Path), "S2: unknown header flags are rejected");
        // 範囲外の eventCount を持つレコードは拒否する
        patched = bytes;
        const size_t firstRec = sizeof(MyeReplayHeader) + 4 * sizeof(InputSnapshot);
        const uint32_t badCount = 9;
        std::memcpy(patched.data() + firstRec, &badCount, sizeof(badCount));
        WriteAll(v8Path, patched);
        ReplayPlayer q2;
        check(!q2.Load(v8Path), "S2: a record with eventCount > 8 is rejected");
    }

    // v8: 56 バイトのヘッダ + (2 レーン + hash) x 2 tick。新項目は「不明」で読める
    {
        MyeReplayHeaderV8 h8 = {};
        h8.magic = 0x5045524Du;
        h8.version = 8;
        h8.fixedDt = 1.0f / 60.0f;
        h8.inputSize = sizeof(InputSnapshot);
        h8.tickCount = 2;
        h8.rngState = 123;
        h8.rngInc = 457;
        h8.entityCount = 5;
        h8.playerCount = 2;
        std::vector<char> bytes(reinterpret_cast<const char*>(&h8),
                                reinterpret_cast<const char*>(&h8) + sizeof(h8));
        for (uint32_t t = 0; t < 2; ++t) {
            for (uint32_t p = 0; p < 2; ++p) {
                const InputSnapshot in = MakeInput(t * 10 + p);
                bytes.insert(bytes.end(), reinterpret_cast<const char*>(&in),
                             reinterpret_cast<const char*>(&in) + sizeof(in));
            }
            const uint64_t hash = 0xB000 + t;
            bytes.insert(bytes.end(), reinterpret_cast<const char*>(&hash),
                         reinterpret_cast<const char*>(&hash) + sizeof(hash));
        }
        WriteAll(v8Path, bytes);
        ReplayPlayer p;
        if (check(p.Load(v8Path), "S2: a v8 .rep still loads")) {
            const InputSnapshot want = MakeInput(11);
            check(p.Header().version == 8 && p.PlayerCount() == 2 && p.TickCount() == 2
                      && p.RngState() == 123 && p.RngInc() == 457 && p.Header().entityCount == 5,
                  "S2: v8 header fields are read");
            check(std::memcmp(&p.InputForTick(1, 1), &want, sizeof(InputSnapshot)) == 0
                      && p.ExpectedHash(0) == 0xB000 && p.ExpectedHash(1) == 0xB001,
                  "S2: v8 tick records (lanes + hash) are read");
            check(!p.HasSystemInput() && p.Header().session.role == 0 && p.Header().session.playerCount == 0
                      && p.Header().provenance.contentHash == 0,
                  "S2: v8 reads the new fields as unknown (zero)");
        }
        // 版の範囲外 (v7 / v10) は拒否する
        std::vector<char> old = bytes;
        uint32_t ver = 7;
        std::memcpy(old.data() + offsetof(MyeReplayHeaderV8, version), &ver, sizeof(ver));
        WriteAll(v8Path, old);
        ReplayPlayer q;
        check(!q.Load(v8Path), "S2: v7 .rep is rejected");
        ver = 10;
        std::memcpy(old.data() + offsetof(MyeReplayHeaderV8, version), &ver, sizeof(ver));
        WriteAll(v8Path, old);
        ReplayPlayer q2;
        check(!q2.Load(v8Path), "S2: a newer version is rejected");
    }

    // --rep-diff: SystemInputTick の差分を項目名つきで出す。role の違いは差分にしない
    {
        RecordSample(sysPath, static_cast<uint32_t>(SessionRole::Server), 4, sysTicks, 9);
        RecordSample(sys2Path, static_cast<uint32_t>(SessionRole::Client), 4, sysTicks, 9);
        const ReplayDiffResult same = DiffReplayFiles(sysPath, sys2Path);
        check(same.same, "S2: --rep-diff ignores SessionConfig.role (server vs client recording)");
        SystemInputTick changed[3] = { sysTicks[0], sysTicks[1], sysTicks[2] };
        changed[2].events[1].lane = 3;
        RecordSample(sys2Path, static_cast<uint32_t>(SessionRole::Client), 4, changed, 9);
        const ReplayDiffResult d = DiffReplayFiles(sysPath, sys2Path);
        check(!d.same && d.firstDiffTick == 2
                  && d.summary.find("systemInput.events[1].lane") != std::string::npos,
              "S2: --rep-diff names the differing system-input field and tick");
        RecordSample(sys2Path, static_cast<uint32_t>(SessionRole::Client), 4, sysTicks, 10);
        const ReplayDiffResult s = DiffReplayFiles(sysPath, sys2Path);
        check(!s.same && s.summary.find("session.seed") != std::string::npos,
              "S2: --rep-diff names a differing SessionConfig field");
    }

    // --rep-diff-overlap: 開始 tick が違う 2 本 (サーバ .rep と途中参加クライアント .rep) の重なり区間だけを比べる
    {
        const std::wstring a = (tempDir / L"mye_session_overlap_a.rep").wstring();
        const std::wstring b = (tempDir / L"mye_session_overlap_b.rep").wstring();
        check(RecordFrom(a, 0, 6, ~0ull) && RecordFrom(b, 3, 4, ~0ull), "overlap: record a 6-tick run and a run from tick 3");
        check(!DiffReplayFiles(a, b).same, "overlap: the strict comparison rejects runs with different start ticks");
        const ReplayDiffResult ok = DiffReplayFiles(a, b, 3);
        check(ok.same && ok.summary.find("3 ticks [3, 6)") != std::string::npos,
              "overlap: [3, 6) is compared and identical");
        const ReplayDiffResult few = DiffReplayFiles(a, b, 4);
        check(!few.same && few.summary.find("overlap in only 3") != std::string::npos,
              "overlap: a shorter overlap than the minimum fails");
        check(RecordFrom(b, 3, 4, 5), "overlap: record the later run with a corrupted hash at tick 5");
        const ReplayDiffResult bad = DiffReplayFiles(a, b, 3);
        check(!bad.same && bad.firstDiffTick == 5 && bad.summary.find("tick 5: world hash differs") != std::string::npos,
              "overlap: the corrupted tick is named by its session tick, not its index in the file");
        check(RecordFrom(b, 10, 4, ~0ull), "overlap: record a run that does not overlap");
        check(!DiffReplayFiles(a, b, 1).same, "overlap: disjoint runs never compare equal");
        std::filesystem::remove(a, ec);
        std::filesystem::remove(b, ec);
    }

    // ---- S3: スナップショットとハッシュ上の SessionLanes ----
    {
        Scene scene;
        {
            World& w = scene.GetWorld();
            GameObject a = scene.CreateGameObjectTracked("Alpha");
            a.AddComponent<MeshRendererComponent>();
            w.ApplyStructuralChanges();
        }
        InputSnapshot prevTickInput[kMaxPlayers] = {};
        uint64_t audioHandleSeq = 0;
        uint64_t tickIndex = 50;
        SimRefs refs;
        refs.scene = &scene;
        refs.prevTickInput = prevTickInput;
        refs.audioHandleSeq = &audioHandleSeq;
        refs.tickIndex = &tickIndex;

        SessionLanes& lanes = scene.Lanes();
        lanes.systemInput = 1;
        ApplySystemInput(lanes, MakeTick({ MakeEvent(1, 1, SystemEventKind::Join), MakeEvent(2, 2, SystemEventKind::Join),
                                           MakeEvent(3, 1, SystemEventKind::Leave, 0) }));
        const SessionLanes saved = lanes;

        std::vector<std::byte> blob;
        check(CaptureSimSnapshot(refs, blob), "S3: capture with SessionLanes");
        // 壊してから戻す
        lanes = {};
        lanes.lanes[3].state = static_cast<uint32_t>(LaneState::Connected);
        lanes.lanes[3].playerId = 999;
        check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "S3: restore succeeds");
        check(std::memcmp(&scene.Lanes(), &saved, sizeof(SessionLanes)) == 0,
              "S3: SessionLanes come back byte-identical (lanes / lastEventSeq / applied events)");
        std::vector<std::byte> blob2;
        check(CaptureSimSnapshot(refs, blob2) && blob2 == blob, "S3: re-captured blob is byte-identical");

        uint64_t pState = 0;
        uint64_t pInc = 0;
        check(PeekSimSnapshotWorldRng(blob.data(), blob.size(), pState, pInc)
                  && pState == scene.GetWorld().Rng().State() && pInc == scene.GetWorld().Rng().Inc(),
              "S3: PeekSimSnapshotWorldRng matches the world RNG (World section is last)");

        // 壊れた SES 節 (appliedCount > 8) は復元を拒否し、世界に触れない
        {
            std::vector<std::byte> broken = blob;
            const uint32_t magic = 0x31534553u;
            const auto it = std::search(broken.begin(), broken.end(),
                                        reinterpret_cast<const std::byte*>(&magic),
                                        reinterpret_cast<const std::byte*>(&magic) + sizeof(magic));
            if (check(it != broken.end(), "S3: SES section is present in the blob")) {
                const uint32_t badApplied = 9;
                std::memcpy(&*it + 2 * sizeof(uint32_t), &badApplied, sizeof(badApplied));
                const SessionLanes beforeBad = scene.Lanes();
                check(!RestoreSimSnapshot(refs, broken.data(), broken.size())
                          && std::memcmp(&scene.Lanes(), &beforeBad, sizeof(SessionLanes)) == 0,
                      "S3: a corrupt SES section is rejected without touching the scene");
            }
        }

        // ハッシュ: システム入力を持たない記録では SessionLanes を畳まない (既存のハッシュ列を動かさない)
        World& w = scene.GetWorld();
        lanes.systemInput = 0;
        const uint64_t ungated = HashWorld(w, SimSourcesOf(scene, nullptr, nullptr, nullptr));
        const uint64_t reference = HashWorld(w, SimSources{ nullptr, &scene.Time(), &scene.Persist(),
                                                              nullptr, nullptr, &scene.UI(), nullptr });
        lanes.lastEventSeq += 5;
        lanes.lanes[2].state = static_cast<uint32_t>(LaneState::Connected);
        const uint64_t ungated2 = HashWorld(w, SimSourcesOf(scene, nullptr, nullptr, nullptr));
        check(ungated == reference && ungated2 == reference,
              "S3: SessionLanes are not folded into the hash when the record has no system input");
        HashDump dump;
        HashWorldDump(w, SimSourcesOf(scene, nullptr, nullptr, nullptr), 0, dump);
        const auto hasLanesLine = [](const HashDump& d) {
            return std::any_of(d.lines.begin(), d.lines.end(), [](const std::string& l) {
                return l.find("\tSessionLanes\t") != std::string::npos;
            });
        };
        check(!hasLanesLine(dump) && dump.total == reference, "S3: the ungated dump has no SessionLanes rows");

        lanes.systemInput = 1;
        const uint64_t gated = HashWorld(w, SimSourcesOf(scene, nullptr, nullptr, nullptr));
        lanes.lanes[2].playerId = 5;
        const uint64_t gated2 = HashWorld(w, SimSourcesOf(scene, nullptr, nullptr, nullptr));
        check(gated != reference && gated2 != gated,
              "S3: with system input the lane state is part of the world hash");
        HashDump dump2;
        HashWorldDump(w, SimSourcesOf(scene, nullptr, nullptr, nullptr), 0, dump2);
        check(hasLanesLine(dump2) && dump2.total == gated2,
              "S3: --hash-dump shows named SessionLanes rows and agrees with HashWorld");

        // CrashRing は Server 役のセッションで SystemInputTick 付きのレコードを書く
        {
            lanes = saved;
            CrashRing ring;
            CrashRingConfig cfg;
            cfg.hashInterval = 1;
            cfg.maxTicks = 8;
            cfg.playerCount = 2;
            cfg.session.role = static_cast<uint32_t>(SessionRole::Server);
            ring.Configure(cfg);
            ring.SetEnabled(true);
            check(ring.Begin(refs, 50), "S3: CrashRing Begin with a Server session");
            check(ring.RecordBytes() == 2 * sizeof(InputSnapshot) + sizeof(SystemInputTick) + sizeof(uint64_t),
                  "S3: CrashRing record length includes the SystemInputTick");
            const InputSnapshot in[2] = { MakeInput(1), MakeInput(2) };
            const SystemInputTick sys = MakeTick({ MakeEvent(4, 4, SystemEventKind::Join) });
            ring.OnTickBegin(50, in, 2, &sys);
            tickIndex = 51;
            ring.OnTickEnd(refs, 50, 0xABCDEF);
            const std::wstring crashPath = (tempDir / L"mye_session_crash.rep").wstring();
            ReplayPlayer p;
            if (check(ring.WriteRepFile(crashPath.c_str()) && p.Load(crashPath),
                      "S3: the crash ring writes a loadable v9 .rep")) {
                check(p.HasSystemInput() && p.TickCount() == 1 && p.SystemInputForTick(0).eventCount == 1
                          && p.SystemInputForTick(0).events[0].eventSeq == 4 && p.ExpectedHash(0) == 0xABCDEF
                          && p.Header().session.role == static_cast<uint32_t>(SessionRole::Server)
                          && p.Header().startMeta.tick == 50,
                      "S3: crash .rep keeps the system input, the hash and the session");
            }
            CrashRing plainRing;
            CrashRingConfig plainCfg;
            plainCfg.playerCount = 2;
            plainRing.Configure(plainCfg);
            plainRing.SetEnabled(true);
            check(plainRing.Begin(refs, 50)
                      && plainRing.RecordBytes() == 2 * sizeof(InputSnapshot) + sizeof(uint64_t),
                  "S3: a session without system input keeps the old record length");
        }
        lanes = {};
    }

    // ---- D9: 埋め込みスナップショットの RNG はヘッダと一致していること ----
    {
        Scene scene;
        {
            GameObject a = scene.CreateGameObjectTracked("Alpha");
            a.AddComponent<MeshRendererComponent>();
            scene.GetWorld().ApplyStructuralChanges();
            scene.GetWorld().Rng().NextU32();
        }
        InputSnapshot prevTickInput[kMaxPlayers] = {};
        uint64_t audioHandleSeq = 0;
        uint64_t tickIndex = 0;
        SimRefs refs;
        refs.scene = &scene;
        refs.prevTickInput = prevTickInput;
        refs.audioHandleSeq = &audioHandleSeq;
        refs.tickIndex = &tickIndex;
        std::vector<std::byte> blob;
        CaptureSimSnapshot(refs, blob);
        World& w = scene.GetWorld();
        const InputSnapshot in = MakeInput(3);
        const std::wstring snapPath = (tempDir / L"mye_session_snap.rep").wstring();

        ReplayRecorder good;
        good.Start(snapPath, w.Rng().State(), w.Rng().Inc(), w.AliveCount(), 1, blob.data(), blob.size());
        good.RecordTick(&in, 1, 0x77);
        good.Finish();
        ReplayPlayer pg;
        check(pg.Load(snapPath) && !pg.Snapshot().empty(),
              "D9: an embedded snapshot whose header RNG matches the blob loads");

        ReplayRecorder bad;
        bad.Start(snapPath, w.Rng().State() + 1, w.Rng().Inc(), w.AliveCount(), 1, blob.data(), blob.size());
        bad.RecordTick(&in, 1, 0x77);
        bad.Finish();
        ReplayPlayer pb;
        check(!pb.Load(snapPath), "D9: a header RNG that differs from the embedded snapshot is rejected");
    }

    // ---- P1: engineVersion ----
    {
        check(EngineVersionFromGit("unknown") == 0 && EngineVersionFromGit("") == 0,
              "P1: an unknown git descriptor maps to 0 (= unknown)");
        const uint64_t clean = EngineVersionFromGit("0123456789ab");
        const uint64_t dirty = EngineVersionFromGit("0123456789ab-dirty");
        check(clean != 0 && dirty != 0 && clean != dirty, "P1: a dirty build has a different engineVersion");
        check(IsDirtyGit("0123456789ab-dirty") && !IsDirtyGit("0123456789ab") && !IsDirtyGit(""),
              "P1: -dirty is detected");
        check(MakeSimProvenance(1, 2, 6).engineVersion == EngineVersionFromGit(EngineBuildGit()),
              "P1: the provenance carries the engineVersion of this build");
    }

    // ---- P2: gameVersion と CompareProvenance ----
    {
        const std::wstring dllA = (tempDir / L"mye_prov_a.bin").wstring();
        const std::wstring dllB = (tempDir / L"mye_prov_b.bin").wstring();
        const std::wstring dllA2 = (tempDir / L"mye_prov_a2.bin").wstring();
        WriteAll(dllA, { 'G', 'L', '1', 0 });
        WriteAll(dllA2, { 'G', 'L', '1', 0 });
        WriteAll(dllB, { 'G', 'L', '2', 0 });
        const uint64_t ha = HashFileBytes(dllA);
        check(ha != 0 && ha == HashFileBytes(dllA2) && ha != HashFileBytes(dllB)
                  && HashFileBytes((tempDir / L"mye_prov_missing.bin").wstring()) == 0,
              "P2: gameVersion = byte hash (same bytes match, different bytes differ, missing file = 0)");

        const SimProvenance base = MakeSimProvenance(ha, 0xC0DE, 6);
        check(CompareProvenance(base, base) == ProvenanceMismatch::None, "P2: identical provenance matches");
        SimProvenance other = base;
        other.gameVersion = HashFileBytes(dllB);
        check(CompareProvenance(base, other) == ProvenanceMismatch::GameVersion,
              "P2: a different GameLogic.dll is reported as GameVersion");
        check(CompareProvenance(base, other, /*allowGameMismatch=*/true) == ProvenanceMismatch::None,
              "P2: --allow-game-mismatch downgrades only the game mismatch");
        other.contentHash ^= 1;
        check(CompareProvenance(base, other, true) == ProvenanceMismatch::ContentHash,
              "P2: --allow-game-mismatch does not hide a content mismatch");

        struct Case {
            ProvenanceMismatch want;
            void (*mutate)(SimProvenance&);
        };
        const Case cases[] = {
            { ProvenanceMismatch::ProtocolVersion, [](SimProvenance& p) { p.protocolVersion += 1; } },
            { ProvenanceMismatch::ApiVersion, [](SimProvenance& p) { p.apiVersion += 1; } },
            { ProvenanceMismatch::ReplayVersion, [](SimProvenance& p) { p.replayVersion += 1; } },
            { ProvenanceMismatch::SchemaVersion, [](SimProvenance& p) { p.schemaVersion ^= 1; } },
            { ProvenanceMismatch::EngineVersion, [](SimProvenance& p) { p.engineVersion ^= 1; } },
            { ProvenanceMismatch::GameVersion, [](SimProvenance& p) { p.gameVersion ^= 1; } },
            { ProvenanceMismatch::ContentHash, [](SimProvenance& p) { p.contentHash ^= 1; } },
            { ProvenanceMismatch::InitialSnapshot, [](SimProvenance& p) { p.initialSnapshotHash ^= 1; } },
        };
        bool allCases = true;
        for (const Case& c : cases) {
            SimProvenance p = base;
            c.mutate(p);
            allCases = allCases && CompareProvenance(base, p) == c.want;
        }
        check(allCases, "P2: every provenance field is reported under its own name");

        // 0 = 不明の規則: 双方 0 は WARN 付きで一致、片方だけ 0 は不一致 (engineVersion / contentHash)
        SimProvenance z1 = base, z2 = base;
        z1.engineVersion = z2.engineVersion = 0;
        z1.contentHash = z2.contentHash = 0;
        check(CompareProvenance(z1, z2) == ProvenanceMismatch::None,
              "P2: unknown (0) on both sides matches (with a warning)");
        z2.engineVersion = 5;
        check(CompareProvenance(z1, z2) == ProvenanceMismatch::EngineVersion,
              "P2: engineVersion unknown on one side only is a mismatch");
        z2.engineVersion = 0;
        z2.contentHash = 5;
        check(CompareProvenance(z1, z2) == ProvenanceMismatch::ContentHash,
              "P2: contentHash unknown on one side only is a mismatch");
        check(ComputeSchemaVersion() == ComputeSchemaVersion() && ComputeSchemaVersion() != 0,
              "P2: schemaVersion is a stable fold");

        // --allow-game-mismatch は configBits に載るが、--rep-diff の比較からは外れる
        // (ほかのビットの食い違いは今までどおり割れる)
        auto recordWithBits = [&](const std::wstring& path, uint32_t bits) {
            ReplayRecorder rec;
            SessionConfig cfg = {};
            cfg.playerCount = 1;
            cfg.tickRate = 60;
            cfg.configBits = bits;
            SnapshotMeta meta = {};
            rec.Start(path, 11, 22, 3, 1, nullptr, 0, cfg, SimProvenance{}, meta);
            const InputSnapshot in = MakeInput(1);
            rec.RecordTick(&in, 1, 0xA0);
            return rec.Finish();
        };
        const std::wstring repA = (tempDir / L"mye_prov_cfg_a.rep").wstring();
        const std::wstring repB = (tempDir / L"mye_prov_cfg_b.rep").wstring();
        check(recordWithBits(repA, kCfgSynthInput | kCfgJobs)
                  && recordWithBits(repB, kCfgSynthInput | kCfgJobs | kCfgAllowGameMismatch)
                  && DiffReplayFiles(repA, repB).same,
              "P2: --rep-diff ignores the --allow-game-mismatch bit");
        check(recordWithBits(repB, kCfgSynthInput) && !DiffReplayFiles(repA, repB).same,
              "P2: --rep-diff still compares the other configBits");

        for (const std::wstring& p : { dllA, dllB, dllA2, repA, repB }) {
            std::filesystem::remove(p, ec);
        }
    }

    // ---- P3: contentHash / content_manifest.json ----
    {
        check(IsContentExcludedExtension(L".png") && IsContentExcludedExtension(L".PNG")
                  && IsContentExcludedExtension(L".hlsl") && IsContentExcludedExtension(L".wav")
                  && IsContentExcludedExtension(L".ttf") && IsContentExcludedExtension(L".dds"),
              "P3: image / shader / audio / font extensions are excluded (case-insensitive)");
        check(!IsContentExcludedExtension(L".json") && !IsContentExcludedExtension(L".meta")
                  && !IsContentExcludedExtension(L".prefab") && !IsContentExcludedExtension(L".fbx")
                  && !IsContentExcludedExtension(L"") && !IsContentExcludedExtension(L".newkind"),
              "P3: scenes / prefabs / .meta / models / unknown kinds are included (exclusion list, not allow list)");

        namespace fs = std::filesystem;
        const fs::path root = tempDir / L"mye_content_test";
        fs::remove_all(root, ec);
        fs::create_directories(root / L"scenes", ec);
        fs::create_directories(root / L"shaders", ec);
        auto put = [&](const fs::path& rel, const std::string& text) {
            WriteAll((root / rel).wstring(), std::vector<char>(text.begin(), text.end()));
        };
        put(L"scenes\\main.scene.json", "{\"a\":1}");
        put(L"scenes\\main.scene.json.meta", "guid-1");
        put(L"hero.prefab", "prefab");
        put(L"tex.png", "png-1");
        put(L"shaders\\x.hlsl", "hlsl-1");
        put(L"hit.WAV", "wav-1");

        std::vector<ContentEntry> entries;
        const bool collected = CollectContentEntries(root.wstring(), entries);
        const uint64_t base = FoldContentEntries(entries);
        bool sorted = true;
        for (size_t i = 1; i < entries.size(); ++i) {
            sorted = sorted && entries[i - 1].path < entries[i].path;
        }
        check(collected && entries.size() == 3 && sorted && entries[0].path == "hero.prefab"
                  && entries[1].path == "scenes/main.scene.json",
              "P3: entries are the non-excluded files, root-relative, '/'-separated, sorted");

        auto hashNow = [&]() {
            std::vector<ContentEntry> e;
            CollectContentEntries(root.wstring(), e);
            return FoldContentEntries(e);
        };
        put(L"tex.png", "png-2-changed");
        put(L"shaders\\x.hlsl", "hlsl-2-changed");
        put(L"hit.WAV", "wav-2-changed");
        check(hashNow() == base, "P3: changing excluded files (png / hlsl / wav) leaves contentHash unchanged");
        put(L"shaders\\new.hlsl", "added");
        check(hashNow() == base, "P3: adding an excluded file leaves contentHash unchanged");
        put(L"tex.png.meta", "import settings"); // 除外する種類の .meta も対象外
        put(L"tex.dds.meta", "generated at first run");
        check(hashNow() == base, "P3: the .meta of an excluded kind (png / dds) is not hashed");
        fs::create_directories(root / L"scripts" / L"Generated", ec);
        put(L"scripts\\Generated\\Schema.cs", "// regenerated at every start");
        check(hashNow() == base, "P3: the engine-generated scripts\\Generated\\ output is not hashed");

        put(L"hero.prefab", "prefaB");
        const uint64_t afterPrefab = hashNow();
        check(afterPrefab != base, "P3: changing a .prefab by one byte changes contentHash");
        put(L"hero.prefab", "prefab");
        put(L"scenes\\main.scene.json", "{\"a\":2}");
        check(hashNow() != base, "P3: changing a .scene.json by one byte changes contentHash");
        put(L"scenes\\main.scene.json", "{\"a\":1}");
        put(L"scenes\\main.scene.json.meta", "guid-2");
        check(hashNow() != base, "P3: changing a .meta by one byte changes contentHash");
        put(L"scenes\\main.scene.json.meta", "guid-1");
        put(L"extra.newkind", "x");
        check(hashNow() != base, "P3: adding a file of an unknown kind changes contentHash (included by default)");
        fs::remove(root / L"extra.newkind", ec);
        check(hashNow() == base, "P3: restoring the files restores contentHash");

        // manifest の往復。assets 直下の manifest 自身は対象に入らない
        const std::wstring manifestPath = (root / kContentManifestName).wstring();
        uint64_t written = 0;
        uint64_t readBack = 0;
        size_t fileCount = 0;
        check(WriteContentManifest(manifestPath, root.wstring(), &written) && written == base
                  && ReadContentManifest(manifestPath, readBack, &fileCount) && readBack == base
                  && fileCount == 3,
              "P3: manifest round-trip returns the same contentHash");
        put(L"content_manifest.json.meta", "guid-of-the-manifest"); // AssetDatabase が付ける sidecar
        check(hashNow() == base && ResolveContentHash(root.wstring()) == base,
              "P3: the manifest and its .meta are not hashed, and ResolveContentHash reads the manifest");

        // 手で壊された manifest は信用しない (計算値へ落ちる)
        std::vector<char> text = ReadAll(manifestPath);
        const std::string needle = "\"size\": ";
        const std::string all(text.begin(), text.end());
        const size_t at = all.find(needle);
        if (at != std::string::npos) {
            std::string broken = all;
            broken[at + needle.size()] = broken[at + needle.size()] == '9' ? '8' : '9';
            WriteAll(manifestPath, std::vector<char>(broken.begin(), broken.end()));
        }
        check(at != std::string::npos && !ReadContentManifest(manifestPath, readBack),
              "P3: a hand-edited manifest is rejected");
        put(L"hero.prefab", "prefaB");
        check(ResolveContentHash(root.wstring()) == afterPrefab,
              "P3: an unusable manifest falls back to computing from the assets");

        // 正規化: 大文字小文字だけ違うパスは同じ contentHash になる
        const fs::path rootB = tempDir / L"mye_content_test_b";
        fs::remove_all(rootB, ec);
        fs::create_directories(rootB / L"Scenes", ec);
        WriteAll((rootB / L"Hero.PREFAB").wstring(), { 'p', 'r', 'e', 'f', 'a', 'B' });
        WriteAll((rootB / L"Scenes" / L"Main.Scene.JSON").wstring(), { '{', '"', 'a', '"', ':', '1', '}' });
        WriteAll((rootB / L"Scenes" / L"Main.Scene.JSON.meta").wstring(), { 'g', 'u', 'i', 'd', '-', '1' });
        std::vector<ContentEntry> eb;
        CollectContentEntries(rootB.wstring(), eb);
        check(FoldContentEntries(eb) == afterPrefab, "P3: paths are lower-cased, so letter case alone does not change contentHash");

        fs::remove_all(root, ec);
        fs::remove_all(rootB, ec);
    }

    CheckReplayStreaming(tempDir, check);

    std::filesystem::remove(plainPath, ec);
    std::filesystem::remove(sysPath, ec);
    std::filesystem::remove(sys2Path, ec);
    std::filesystem::remove(v8Path, ec);
    std::filesystem::remove(tempDir / L"mye_session_snap.rep", ec);
    std::filesystem::remove(tempDir / L"mye_session_crash.rep", ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== Session self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Session self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
