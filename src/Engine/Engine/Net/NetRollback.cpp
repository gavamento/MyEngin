#include "Engine/Engine/Net/NetRollback.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Replay/CrashRing.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

constexpr uint64_t kNoTick = ~0ull;

} // namespace

bool NetRollback::Begin(const SimRefs& refs, uint64_t startTick, uint32_t maxSpeculation)
{
    Clear();
    maxSpec_ = (maxSpeculation < 1) ? 1u
        : ((maxSpeculation > kNetMaxSpeculationLimit) ? kNetMaxSpeculationLimit : maxSpeculation);
    ringSize_ = maxSpec_ + 4;
    if (!TakeSnapshot(refs, startTick)) {
        MYE_LOG_ERROR("[net] rollback: could not capture the starting snapshot - "
                      "falling back to plain lockstep");
        return false;
    }
    confirmed_ = startTick;
    active_ = true;
    return true;
}

void NetRollback::Clear()
{
    active_ = false;
    confirmed_ = 0;
    rollbacks_ = rollbackTicks_ = maxDepth_ = predictedTicks_ = 0;
    snapBytes_ = 0;
    for (uint32_t i = 0; i < kNetSpecRingMax; ++i) {
        snaps_[i].tick = kNoTick;
        snaps_[i].blob.clear();
        specValid_[i] = false;
        specTick_[i] = 0;
        spec_[i] = NetSpecTick{};
    }
    for (uint32_t i = 0; i < kNetHashRing; ++i) {
        hashTick_[i] = 0;
        hashValue_[i] = 0;
    }
}

bool NetRollback::TakeSnapshot(const SimRefs& refs, uint64_t tick)
{
    Slot& s = snaps_[RingIndex(tick)];
    // ★blob のバッファは使い回す (CaptureSimSnapshot が clear + 追記する)。
    //   毎 tick 148KB を確保し直すと、ロールバックが要らない局面でも確保の
    //   コストだけが 60Hz で積む
    snapBytes_ -= s.blob.size(); // このスロットの寄与をいったん外して撮り直す
    if (!CaptureSimSnapshot(refs, s.blob)) {
        s.tick = kNoTick;
        s.blob.clear();
        return false;
    }
    snapBytes_ += s.blob.size();
    s.tick = tick;
    return true;
}

void NetRollback::OnTickEnd(const SimRefs& refs, uint64_t ranTick, const InputSnapshot* inputs,
                            uint32_t playerCount, uint64_t hashAfter, bool predicted,
                            bool simulated, const SystemInputTick* sys)
{
    if (!active_) {
        return;
    }
    const size_t i = RingIndex(ranTick);
    NetSpecTick& e = spec_[i];
    e = NetSpecTick{};
    if (inputs != nullptr) {
        const uint32_t n = (playerCount < kMaxPlayers) ? playerCount : kMaxPlayers;
        for (uint32_t p = 0; p < n; ++p) {
            e.inputs[p] = inputs[p];
        }
    }
    e.hashAfter = hashAfter;
    e.predicted = predicted;
    e.simulated = simulated;
    if (sys != nullptr) {
        e.sys = NormalizeSystemInput(*sys);
        e.hasSys = true;
    }
    specTick_[i] = ranTick;
    specValid_[i] = true;
    if (predicted) {
        ++predictedTicks_;
    }
    // 「次の tick が走る前」= いまの状態。ここが唯一の撮影点 (構造変更が空)
    if (!TakeSnapshot(refs, ranTick + 1)) {
        MYE_LOG_ERROR("[net] rollback: snapshot failed at tick %llu - rollback disabled",
                      static_cast<unsigned long long>(ranTick + 1));
        active_ = false;
    }
}

const NetSpecTick* NetRollback::Entry(uint64_t tick) const
{
    const size_t i = RingIndex(tick);
    return (specValid_[i] && specTick_[i] == tick) ? &spec_[i] : nullptr;
}

void NetRollback::MarkConfirmed(uint64_t tick)
{
    const size_t i = RingIndex(tick);
    if (specValid_[i] && specTick_[i] == tick) {
        spec_[i].predicted = false;
    }
}

bool NetRollback::InputsMatch(uint64_t tick, const InputSnapshot* lanes,
                              uint32_t playerCount) const
{
    const NetSpecTick* e = Entry(tick);
    if (e == nullptr || lanes == nullptr) {
        return false;
    }
    const uint32_t n = (playerCount < kMaxPlayers) ? playerCount : kMaxPlayers;
    // ★InputSnapshot は明示パディング済みの 112B POD (Input.h の static_assert)。
    //   .rep はこの生バイトをそのまま書いて 2 プロセス間でバイト比較している
    //   (--rep-diff) ので、ここで memcmp を使うのは既存の不変量と同じ土俵
    return std::memcmp(e->inputs, lanes, sizeof(InputSnapshot) * n) == 0;
}

bool NetRollback::SystemMatch(uint64_t tick, const SystemInputTick& sys) const
{
    const NetSpecTick* e = Entry(tick);
    if (e == nullptr) {
        return false;
    }
    const SystemInputTick norm = NormalizeSystemInput(sys);
    return std::memcmp(&e->sys, &norm, sizeof(SystemInputTick)) == 0;
}

const std::vector<std::byte>* NetRollback::SnapshotBefore(uint64_t tick) const
{
    const Slot& s = snaps_[RingIndex(tick)];
    if (s.tick != tick || s.blob.empty()) {
        return nullptr;
    }
    return &s.blob;
}

void NetRollback::NoteCommitted(uint64_t tick, uint64_t hash)
{
    const size_t i = static_cast<size_t>(tick % kNetHashRing);
    hashTick_[i] = tick;
    hashValue_[i] = hash;
}

bool NetRollback::CommittedHash(uint64_t tick, uint64_t& outHash) const
{
    const size_t i = static_cast<size_t>(tick % kNetHashRing);
    if (hashTick_[i] != tick || hashValue_[i] == 0) {
        return false;
    }
    outHash = hashValue_[i];
    return true;
}

void NetRollback::NoteRollback(uint64_t depth)
{
    ++rollbacks_;
    rollbackTicks_ += depth;
    if (depth > maxDepth_) {
        maxDepth_ = depth;
    }
}

bool WriteNetDesyncBundle(const std::wstring& crashRoot, const NetDesyncReport& rep,
                          const CrashRing& ring, const HashDump& dump, std::wstring& outDir)
{
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(crashRoot) / L"crash";
    // ★フォルダ名にレーン番号を入れる。同一マシンで 2 プロセスを回すとき
    //   (net_verify.bat がまさにそれ) に同じ名前だと**両者が同じフォルダを上書きし合い**、
    //   突き合わせに要る 2 本目が残らない。診断は 2 台ぶん揃って初めて成立する
    wchar_t name[64];
    std::swprintf(name, 64, L"desync_%llu_p%u", static_cast<unsigned long long>(rep.tick),
                  rep.localPlayer);
    dir /= name;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        MYE_LOG_ERROR("[net] could not create the desync bundle folder: %s",
                      WideToUtf8(dir.wstring()).c_str());
        return false;
    }
    outDir = dir.wstring();

    // ★.rep を最初に書く。以降で失敗しても「再現に必要な 1 本」だけは必ず残す
    //   (M52f の txt → rep → dump の順序と同じ発想で、失っては困るものから先に出す)
    ring.WriteRepFile((dir / L"local.rep").wstring().c_str());
    WriteHashDump((dir / L"local.dump").wstring(), dump);

    std::ofstream f(dir / L"desync.txt");
    if (!f) {
        return false;
    }
    f << "MyEngine desync report (M52i)\n";
    f << "checkpoint tick  " << rep.tick << "\n";
    f << "detected at tick " << rep.nowTick << "\n";
    f << "role             " << (rep.role == 1 ? "host" : (rep.role == 2 ? "join" : (rep.role == 4 ? "client" : "?"))) << "\n";
    f << "local lane       " << rep.localPlayer << "\n";
    char hex[32];
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(rep.localHash));
    f << "local  hash      " << hex << "\n";
    std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(rep.peerHash));
    f << "peer   hash      " << hex << "\n";
    f << "\n";
    // ★ここが報告の本体。desync は「割れた事実」より「次に何を打てば原因に届くか」が
    //   要る — 手順を思い出せないまま .rep だけ渡されても誰も追えない
    f << "What this means: both peers consumed the same inputs but ended up with different\n"
         "world state. Hashes are exchanged every 8 ticks, so the real divergence is at or\n"
         "just before the checkpoint tick above. The simulation is no longer deterministic\n"
         "across the two builds/machines.\n\n";
    if (rep.role == 4) {
        // 専用サーバ構成: サーバは止まらずバンドルを出さないので、相手側の再現にはサーバの .rep を使う。
        // local.rep は参加 / 再同期の tick から始まるが、再生側が開始 tick を基点に引くので単独で再生できる
        f << "How to find the field that diverged (the server does not write a bundle; use the\n"
             "server's .rep, i.e. the file given to the server's --replay-record):\n";
        f << "  1. Server.exe --replay-verify <server>.rep --hash-dump-tick " << rep.nowTick
          << " --hash-dump server.dump\n";
        f << "  2. Runtime.exe --hash-diff server.dump <this bundle>/local.dump\n";
        f << "     -> names the component/field that went out of sync (local.dump = this client's\n"
             "        state at tick " << rep.nowTick << ").\n";
        f << "  To look at the checkpoint tick " << rep.tick << " itself, replay both .rep files to it:\n";
        f << "     Server.exe --replay-verify <server>.rep --hash-dump-tick " << rep.tick
          << " --hash-dump server.dump\n";
        f << "     Runtime.exe --replay-verify <this bundle>/local.rep --hash-dump-tick " << rep.tick
          << " --hash-dump client.dump\n";
        f << "     Runtime.exe --hash-diff server.dump client.dump\n\n";
    } else {
        f << "How to find the field that diverged (both bundles are needed):\n";
        f << "  1. Runtime.exe --rep-diff <peerA>/local.rep <peerB>/local.rep\n";
        f << "     -> T = the first tick whose recorded hash differs (T <= " << rep.tick << ").\n";
        f << "  2. Runtime.exe --replay-verify <peerA>/local.rep --hash-dump-tick T "
             "--hash-dump a.dump\n";
        f << "     Runtime.exe --replay-verify <peerB>/local.rep --hash-dump-tick T "
             "--hash-dump b.dump\n";
        f << "  3. Runtime.exe --hash-diff a.dump b.dump\n";
        f << "     -> names the component/field that first went out of sync.\n\n";
    }
    f << "local.dump is the field-level dump at tick " << rep.nowTick
      << " (the detection point), kept so the state is\n"
         "not lost even if the .rep can no longer be replayed.\n";
    return true;
}

} // namespace mye
