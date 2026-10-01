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
