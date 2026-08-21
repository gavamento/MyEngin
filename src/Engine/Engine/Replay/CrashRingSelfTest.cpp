#include "Engine/Engine/Replay/CrashRingSelfTest.h"

#include <cstring>
#include <filesystem>
#include <string>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/CrashRing.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/CrashHandler.h"

namespace mye {
namespace {

// 被験シーン。SimSnapshot の被覆と揃える必要は無いので最小限でよい —
// ここで見たいのは「.rep のバイト列としての組み立て」であって sim の網羅ではない
void BuildScene(Scene& scene)
{
    World& w = scene.GetWorld();
    GameObject a = scene.CreateGameObjectTracked("Alpha");
    GameObject b = scene.CreateGameObjectTracked("Beta");
    a.AddComponent<MeshRendererComponent>();
    b.AddComponent<ColliderComponent>();
    w.ApplyStructuralChanges();
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

// リングの現イメージをファイルへ落として ReplayPlayer で読み直す。
// ★ここを in-memory の自前パーサでやらない: 検証したいのは「ハンドラが書いたものを
//   本物のローダが読めるか」で、自前パーサだと両方同時に間違っても気づけない
bool RoundTrip(const CrashRing& ring, const std::wstring& path, ReplayPlayer& out)
{
    return ring.WriteRepFile(path.c_str()) && out.Load(path);
}

} // namespace

bool RunCrashRingSelfTest()
{
    MYE_LOG_INFO("==== CrashRing (crash bundle .rep) self test ====");
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

    // ---- 1. 固定バッファ書式化 (ハンドラ内で使う唯一の書式化手段) ----
    {
        char buf[64];
        crashfmt::Sink s(buf, sizeof(buf));
        s.Ascii("t=");
        s.U64(0);
        s.Ascii(" p=");
        s.U64(7, 3);
        s.Ascii(" h=0x");
        s.Hex(0xDEADBEEFull, 8);
        check(std::strcmp(buf, "t=0 p=007 h=0xDEADBEEF") == 0, "Sink: 桁埋め / 16 進 / 連結");
        check(!s.Truncated(), "Sink: 収まっていれば truncated は立たない");
    }
    {
        // 溢れは「捨てて印を立てる」。★書けたところまでは必ず終端付きで有効であること —
        // ハンドラ内で壊れた文字列を掴んで二重フォルトする経路を作らないため
        char buf[8];
        crashfmt::Sink s(buf, sizeof(buf));
        s.Ascii("0123456789ABCDEF");
        check(std::strcmp(buf, "0123456") == 0 && s.Length() == 7, "Sink: 溢れは切り詰める");
        check(s.Truncated(), "Sink: 溢れたら truncated");
    }
    {
        wchar_t wbuf[32];
        crashfmt::WSink s(wbuf, 32);
        s.Str(L"C:\\dir");
        s.Ascii("\\crash.rep"); // ASCII リテラルを wchar_t 側へ流す経路
        check(std::wcscmp(wbuf, L"C:\\dir\\crash.rep") == 0, "WSink: Str + Ascii の混在");
        s.Clear();
        check(s.Length() == 0 && wbuf[0] == L'\0', "WSink: Clear");
    }
    {
        check(ParseCrashTestKind(L"av") == CrashTestKind::AccessViolation
                  && ParseCrashTestKind(L"purecall") == CrashTestKind::PureCall
                  && ParseCrashTestKind(L"terminate") == CrashTestKind::Terminate
                  && ParseCrashTestKind(L"invalidparam") == CrashTestKind::InvalidParam
                  && ParseCrashTestKind(L"stackoverflow") == CrashTestKind::StackOverflow
                  && ParseCrashTestKind(L"nonsense") == CrashTestKind::None
                  && ParseCrashTestKind(nullptr) == CrashTestKind::None,
              "ParseCrashTestKind");
    }

    // ---- 2. リング本体 ----
    std::error_code ec;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
    const std::wstring repPath = (tempDir / L"mye_crashring_selftest.rep").wstring();

    Scene scene;
    BuildScene(scene);
    InputSnapshot prevTickInput[kMaxPlayers] = {};
    uint64_t audioHandleSeq = 0;
    uint64_t tickIndex = 100;
    SimRefs refs;
    refs.scene = &scene;
    refs.prevTickInput = prevTickInput;
    refs.audioHandleSeq = &audioHandleSeq;
    refs.tickIndex = &tickIndex;

    CrashRing ring;
    CrashRingConfig cfg;
    cfg.snapshotInterval = 10; // 撮り直しの検査を短い tick 数で回すため
    cfg.maxTicks = 6;
    ring.Configure(cfg);
    ring.SetEnabled(true);
    check(ring.Begin(refs, 100), "Begin: 1 枚目を撮る");
    check(ring.SnapshotTick() == 100 && ring.RecordCount() == 0 && ring.SnapshotBytes() > 0,
          "Begin 直後: レコード 0 本 / スナップショットは非空");

    // 3 tick 完走させる
    uint64_t hashes[3] = { 0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull };
    for (uint64_t i = 0; i < 3; ++i) {
        const InputSnapshot in = MakeInput(static_cast<uint32_t>(i));
        ring.OnTickBegin(100 + i, &in, 1);
        tickIndex = 101 + i;
        ring.OnTickEnd(refs, 100 + i, hashes[i]);
    }
    check(ring.RecordCount() == 3 && !ring.InFlight(), "完走 3 tick が 3 レコード");

    {
        ReplayPlayer p;
        if (check(RoundTrip(ring, repPath, p), "本物の ReplayPlayer で読み直せる")) {
            check(p.TickCount() == 3 && p.PlayerCount() == 1, "tickCount / playerCount");
            bool inputsOk = true;
            bool hashesOk = true;
            for (uint64_t i = 0; i < 3; ++i) {
                const InputSnapshot want = MakeInput(static_cast<uint32_t>(i));
                // .rep の tick 番号はスナップショット tick からの絶対値ではなく 0 始まり
                inputsOk = inputsOk
                    && std::memcmp(&p.InputForTick(i), &want, sizeof(InputSnapshot)) == 0;
                hashesOk = hashesOk && p.ExpectedHash(i) == hashes[i];
            }
            check(inputsOk, "入力列がそのまま往復する");
            check(hashesOk, "ハッシュ列がそのまま往復する");
            check(!p.Snapshot().empty(), "開始スナップショットが埋め込まれている");
            // 埋め込みを実際に復元できること (= シーン非依存に再生できる .rep である証拠)
            Scene other;
            BuildScene(other);
            InputSnapshot otherPrev[kMaxPlayers] = {};
            uint64_t otherSeq = 0;
            uint64_t otherTick = 0;
            SimRefs otherRefs;
            otherRefs.scene = &other;
            otherRefs.prevTickInput = otherPrev;
            otherRefs.audioHandleSeq = &otherSeq;
            otherRefs.tickIndex = &otherTick;
            const bool restored =
                RestoreSimSnapshot(otherRefs, p.Snapshot().data(), p.Snapshot().size());
            check(restored && otherTick == 100, "埋め込みスナップショットを別シーンへ復元できる");
        }
    }

    // ---- 3. in-flight tick (このサブの肝) ----
    // tick に入ったが走り切っていない = 期待ハッシュが存在しない。
    // ★ここに嘘の値を書くと「再現しなかった」が「MISMATCH」に化ける (Replay.h の予約)
    {
        const InputSnapshot in = MakeInput(9);
        ring.OnTickBegin(103, &in, 1);
    }
    check(ring.InFlight() && ring.RecordCount() == 4, "OnTickBegin だけで 1 レコード増える");
    {
        ReplayPlayer p;
        if (check(RoundTrip(ring, repPath, p), "in-flight 込みで読み直せる")) {
            check(p.TickCount() == 4, "in-flight tick も tickCount に入る");
            check(p.ExpectedHash(3) == 0 && !p.HasExpectedHash(3),
                  "in-flight tick の期待ハッシュは 0 = 期待値なし");
            check(p.HasExpectedHash(2), "完走した tick は期待値を持つ");
            const InputSnapshot want = MakeInput(9);
            check(std::memcmp(&p.InputForTick(3), &want, sizeof(InputSnapshot)) == 0,
                  "in-flight tick の入力は載っている (= 落ちた tick へ再突入できる)");
        }
    }
    tickIndex = 104;
    ring.OnTickEnd(refs, 103, 0x4444444444444444ull);
    check(!ring.InFlight() && ring.RecordCount() == 4, "走り切ったら同じレコードを上書きする");
    {
        ReplayPlayer p;
        if (check(RoundTrip(ring, repPath, p), "確定後も読み直せる")) {
            check(p.ExpectedHash(3) == 0x4444444444444444ull, "確定ハッシュで上書きされている");
        }
    }

    // ---- 4. 撮り直し (interval) ----
    const uint64_t snapsBefore = ring.SnapshotCount();
    for (uint64_t i = 4; i < 10; ++i) {
        const InputSnapshot in = MakeInput(static_cast<uint32_t>(i));
        ring.OnTickBegin(100 + i, &in, 1);
        tickIndex = 101 + i;
        ring.OnTickEnd(refs, 100 + i, 0x5000000000000000ull + i);
    }
    // maxTicks = 6 にも interval = 10 にも当たるので、どこかで必ず撮り直している
    check(ring.SnapshotCount() > snapsBefore, "上限/間隔で撮り直す");
    check(ring.RecordCount() <= cfg.maxTicks, "レコードは上限を超えない");
    check(ring.SnapshotTick() > 100, "撮り直しでスナップショット tick が前へ進む");

    // ---- 5. tick が飛んだら記録を捨てる ----
    // シーク (M52e) の再シムで tick 番号は戻る。連続していない入力列を .rep として
    // 出してしまうと「再現できない再現用ファイル」になるので、取り直しへ落とす
    {
        const uint64_t before = ring.SnapshotCount();
        const InputSnapshot in = MakeInput(1);
        ring.OnTickBegin(9999, &in, 1); // 明らかに不連続
        size_t size = 0;
        check(ring.RepImage(size) == nullptr && size == 0,
              "tick が飛んだ直後はイメージを出さない (壊れた .rep を渡さない)");
        tickIndex = 10000;
        ring.OnTickEnd(refs, 9999, 0x6666666666666666ull);
        check(ring.SnapshotCount() == before + 1 && ring.SnapshotTick() == 10000,
              "次の tick 末で撮り直して復帰する");
        size = 0;
        check(ring.RepImage(size) != nullptr && size > 0, "復帰後はまたイメージを出す");
    }

    // ---- 5.5 マルチ入力レーン (M52g) ----
    // レコード長は playerCount で決まる。ここが 1 レーン固定のままだと、2P で落ちた
    // crash.rep は「入力列とハッシュ列が 1 レーンぶんずつずれた」ファイルになり、
    // 読めるのに再現しないという最悪の壊れ方をする
    {
        CrashRing ring2;
        CrashRingConfig cfg2;
        cfg2.snapshotInterval = 100;
        cfg2.maxTicks = 8;
        cfg2.playerCount = 2;
        ring2.Configure(cfg2);
        ring2.SetEnabled(true);
        uint64_t tick2 = 500;
        InputSnapshot prev2[kMaxPlayers] = {};
        uint64_t seq2 = 0;
        SimRefs refs2;
        refs2.scene = &scene;
        refs2.prevTickInput = prev2;
        refs2.audioHandleSeq = &seq2;
        refs2.tickIndex = &tick2;
        check(ring2.Begin(refs2, 500), "2P: Begin");
        check(ring2.RecordBytes() == sizeof(InputSnapshot) * 2 + sizeof(uint64_t),
              "2P: レコード長は 入力 2 本 + ハッシュ");
        for (uint64_t i = 0; i < 3; ++i) {
            const InputSnapshot lanes[2] = { MakeInput(static_cast<uint32_t>(i)),
                                             MakeInput(static_cast<uint32_t>(i) + 50) };
            ring2.OnTickBegin(500 + i, lanes, 2);
            tick2 = 501 + i;
            ring2.OnTickEnd(refs2, 500 + i, 0x7000000000000000ull + i);
        }
        ReplayPlayer p2;
        if (check(RoundTrip(ring2, repPath, p2), "2P: 本物の ReplayPlayer で読み直せる")) {
            check(p2.PlayerCount() == 2 && p2.TickCount() == 3, "2P: playerCount / tickCount");
            bool lanesOk = true;
            for (uint64_t i = 0; i < 3; ++i) {
                const InputSnapshot w0 = MakeInput(static_cast<uint32_t>(i));
                const InputSnapshot w1 = MakeInput(static_cast<uint32_t>(i) + 50);
                lanesOk = lanesOk
                    && std::memcmp(&p2.InputForTick(i, 0), &w0, sizeof(InputSnapshot)) == 0
                    && std::memcmp(&p2.InputForTick(i, 1), &w1, sizeof(InputSnapshot)) == 0
                    && p2.ExpectedHash(i) == 0x7000000000000000ull + i;
            }
            check(lanesOk, "2P: レーンごとの入力とハッシュがそのまま往復する");
        }
    }

    // ---- 6. 無効化 ----
    ring.SetEnabled(false);
    const uint64_t recBefore = ring.RecordCount();
    {
        const InputSnapshot in = MakeInput(2);
        ring.OnTickBegin(10000, &in, 1);
    }
    ring.OnTickEnd(refs, 10000, 0x7777777777777777ull);
    check(ring.RecordCount() == recBefore, "無効化したら何も記録しない");

    std::filesystem::remove(repPath, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== CrashRing self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== CrashRing self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
